// Copyright (c) 2026

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUrlQuery>
#include <QUuid>
#include <QtTest/QtTest>
#include <algorithm>

#include "core/database.h"
#include "reminders/reminderscheduler.h"

using namespace omacalendar;

namespace {

class FakeNotificationBackend final : public NotificationBackend {
 public:
  enum class Result { Success, Failure, Hold };

  explicit FakeNotificationBackend(QObject* parent = nullptr)
      : NotificationBackend(parent) {}

  void send(const CalendarNotification& notification) override {
    const uint notificationId = m_nextId++;
    sent.append(notification);
    ids.append(notificationId);
    if (result == Result::Success) {
      emit notificationResult(notification.fingerprint, notification.deliveryToken,
                              notificationId, {});
    } else if (result == Result::Failure) {
      emit notificationResult(notification.fingerprint, notification.deliveryToken, 0,
                              QStringLiteral("injected backend failure"));
    }
  }

  void resolve(const qsizetype index, const Result resolution) {
    const CalendarNotification notification = sent.at(index);
    const uint notificationId = ids.at(index);
    if (resolution == Result::Success) {
      emit notificationResult(notification.fingerprint, notification.deliveryToken,
                              notificationId, {});
    } else if (resolution == Result::Failure) {
      emit notificationResult(notification.fingerprint, notification.deliveryToken, 0,
                              QStringLiteral("injected backend failure"));
    }
  }

  void invoke(const uint notificationId, const QString& action) {
    emit actionInvoked(notificationId, action);
  }

  QList<CalendarNotification> sent;
  QList<uint> ids;
  Result result = Result::Success;

 private:
  uint m_nextId = 1;
};

Event reminderEvent(const QString& summary, const QDateTime& start,
                    const QJsonArray& reminders) {
  Event event;
  event.calendarId = QStringLiteral("local-default");
  event.summary = summary;
  event.description = QStringLiteral("Private notes");
  event.location = QStringLiteral("Conference room 3");
  event.startUtc = start.toUTC();
  event.endUtc = event.startUtc.addSecs(3600);
  event.startDate = event.startUtc.date();
  event.endDate = event.endUtc.date();
  event.startTimeZone = QStringLiteral("UTC");
  event.endTimeZone = QStringLiteral("UTC");
  event.reminders = reminders;
  return event;
}

Event invitationEvent(const QString& summary, const QDateTime& start) {
  Event event = reminderEvent(summary, start, {});
  event.organizer =
      QJsonObject{{QStringLiteral("email"), QStringLiteral("host@example.test")},
                  {QStringLiteral("displayName"), QStringLiteral("Host")}};
  event.attendees = QJsonArray{
      QJsonObject{{QStringLiteral("email"), QStringLiteral("me@example.test")},
                  {QStringLiteral("self"), true},
                  {QStringLiteral("responseStatus"), QStringLiteral("needsAction")}},
      QJsonObject{{QStringLiteral("email"), QStringLiteral("guest@example.test")},
                  {QStringLiteral("responseStatus"), QStringLiteral("accepted")}}};
  return event;
}

ReminderJob reminderForEvent(Database* database, const QString& eventId) {
  const QList<ReminderJob> reminders = database->reminders(500);
  const auto iterator = std::find_if(
      reminders.cbegin(), reminders.cend(),
      [&eventId](const ReminderJob& job) { return job.eventId == eventId; });
  return iterator == reminders.cend() ? ReminderJob{} : *iterator;
}

bool executeSql(const QString& databasePath, const QString& statement,
                QString* errorMessage = nullptr) {
  const QString connectionName =
      QStringLiteral("reminder-test-%1")
          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  bool succeeded = false;
  QString failure;
  {
    QSqlDatabase connection =
        QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    connection.setDatabaseName(databasePath);
    connection.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!connection.open()) {
      failure = connection.lastError().text();
    } else {
      QSqlQuery query(connection);
      succeeded = query.exec(statement);
      if (!succeeded) {
        failure = query.lastError().text();
      }
      connection.close();
    }
  }
  QSqlDatabase::removeDatabase(connectionName);
  if (!succeeded && errorMessage != nullptr) {
    *errorMessage = failure;
  }
  return succeeded;
}

}  // namespace

class ReminderSchedulerTest final : public QObject {
  Q_OBJECT

 private slots:
  void multipleAlarmsPrivacyAndDuplicateSuppression() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    const QString databasePath = directory.filePath(QStringLiteral("store.sqlite"));
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    QDateTime current(QDate(2027, 1, 12), QTime(12, 0), QTimeZone::UTC);
    FakeNotificationBackend backend;
    QList<QUrl> opened;
    ReminderScheduler scheduler(
        &database, &backend, [&current]() { return current; },
        [&opened](const QUrl& url) {
          opened.append(url);
          return true;
        });

    QVERIFY(database.setSetting(QStringLiteral("notificationPrivacy"),
                                QStringLiteral("full_details"), &error));
    Event detailed = reminderEvent(QStringLiteral("Design review"),
                                   current.addSecs(30 * 60), QJsonArray{45, 60});
    QVERIFY2(database.saveLocalEvent(&detailed, OutboxOperation::Create, &error),
             qPrintable(error));
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 2);
    for (const CalendarNotification& notification : std::as_const(backend.sent)) {
      QCOMPARE(notification.summary, QStringLiteral("Design review"));
      QVERIFY(notification.body.contains(QStringLiteral("Conference room 3")));
      QCOMPARE(notification.hints.value(QStringLiteral("desktop-entry")).toString(),
               QStringLiteral("org.omacalendar.OmaCalendar"));
    }
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 2);

    QVERIFY(database.setSetting(QStringLiteral("notificationPrivacy"),
                                QStringLiteral("generic"), &error));
    Event generic = reminderEvent(QStringLiteral("Hidden appointment"),
                                  current.addSecs(10 * 60), QJsonArray{20});
    QVERIFY2(database.saveLocalEvent(&generic, OutboxOperation::Create, &error),
             qPrintable(error));
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 3);
    QCOMPARE(backend.sent.constLast().summary, QStringLiteral("Calendar reminder"));
    QVERIFY(!backend.sent.constLast().body.contains(generic.summary));
    QVERIFY(!backend.sent.constLast().body.contains(generic.location));

    QVERIFY(database.setSetting(QStringLiteral("notificationPrivacy"),
                                QStringLiteral("title_only"), &error));
    Event titleOnly = reminderEvent(QStringLiteral("Visible title"),
                                    current.addSecs(10 * 60), QJsonArray{20});
    QVERIFY2(database.saveLocalEvent(&titleOnly, OutboxOperation::Create, &error),
             qPrintable(error));
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 4);
    QCOMPARE(backend.sent.constLast().summary, titleOnly.summary);
    QVERIFY(!backend.sent.constLast().body.contains(titleOnly.location));

    const QList<ReminderJob> jobs = database.reminders(20, &error);
    QCOMPARE(jobs.size(), 4);
    QVERIFY(std::all_of(jobs.cbegin(), jobs.cend(), [](const ReminderJob& job) {
      return job.state == QStringLiteral("delivered") && job.deliveredAt.isValid();
    }));
    QVERIFY(opened.isEmpty());
  }

  void snoozeIntervalsDismissAndOpenDeepLink() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));
    QDateTime current(QDate(2027, 2, 3), QTime(9, 0), QTimeZone::UTC);
    FakeNotificationBackend backend;
    QList<QUrl> opened;
    ReminderScheduler scheduler(
        &database, &backend, [&current]() { return current; },
        [&opened](const QUrl& url) {
          opened.append(url);
          return true;
        });

    Event event = reminderEvent(QStringLiteral("Snooze me"), current.addSecs(15 * 60),
                                QJsonArray{30});
    QVERIFY2(database.saveLocalEvent(&event, OutboxOperation::Create, &error),
             qPrintable(error));
    scheduler.checkNow();
    QCOMPARE(backend.ids.size(), 1);

    const QList<int> intervals{5, 10, 30, 60};
    for (const int minutes : intervals) {
      const uint notificationId = backend.ids.constLast();
      scheduler.checkNow();
      backend.invoke(notificationId, QStringLiteral("snooze%1").arg(minutes));
      const ReminderJob snoozed = reminderForEvent(&database, event.id);
      QCOMPARE(snoozed.state, QStringLiteral("snoozed"));
      QCOMPARE(snoozed.snoozedUntil, current.addSecs(minutes * 60));
      current = current.addSecs(minutes * 60);
      scheduler.checkNow();
      QCOMPARE(backend.ids.size(), intervals.indexOf(minutes) + 2);
    }

    backend.invoke(backend.ids.constLast(), QStringLiteral("default"));
    QCOMPARE(opened.size(), 1);
    QCOMPARE(opened.first().scheme(), QStringLiteral("omacalendar"));
    QCOMPARE(opened.first().host(), QStringLiteral("event"));
    QCOMPARE(opened.first().path(), QStringLiteral("/") + event.id);
    QVERIFY(!QUrlQuery(opened.first())
                 .queryItemValue(QStringLiteral("recurrenceId"))
                 .isEmpty());

    Event dismissible = reminderEvent(QStringLiteral("Dismiss me"),
                                      current.addSecs(5 * 60), QJsonArray{10});
    QVERIFY2(database.saveLocalEvent(&dismissible, OutboxOperation::Create, &error),
             qPrintable(error));
    scheduler.checkNow();
    const uint dismissId = backend.ids.constLast();
    backend.invoke(dismissId, QStringLiteral("dismiss"));
    QCOMPARE(reminderForEvent(&database, dismissible.id).id, qint64(0));
    const QList<ReminderJob> stillDue = database.dueReminders(current, 20, &error);
    QVERIFY(std::none_of(stillDue.cbegin(), stillDue.cend(),
                         [&dismissible](const ReminderJob& job) {
                           return job.eventId == dismissible.id;
                         }));
  }

  void sleepClockChangesDstAndCancellation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));
    QDateTime current(QDate(2027, 3, 1), QTime(8, 0), QTimeZone::UTC);
    FakeNotificationBackend backend;
    ReminderScheduler scheduler(
        &database, &backend, [&current]() { return current; },
        [](const QUrl&) { return true; });

    Event wakeEvent = reminderEvent(QStringLiteral("Wake delivery"),
                                    current.addSecs(10 * 60), QJsonArray{5});
    QVERIFY2(database.saveLocalEvent(&wakeEvent, OutboxOperation::Create, &error),
             qPrintable(error));
    scheduler.handlePrepareForSleep(true);
    current = current.addSecs(6 * 60);
    scheduler.eventsChanged({QStringLiteral("local-default")});
    QCOMPARE(backend.sent.size(), 0);
    scheduler.handlePrepareForSleep(false);
    QCoreApplication::processEvents();
    QCOMPARE(backend.sent.size(), 1);

    Event forward = reminderEvent(QStringLiteral("Forward clock"),
                                  current.addSecs(2 * 3600), QJsonArray{30});
    QVERIFY2(database.saveLocalEvent(&forward, OutboxOperation::Create, &error),
             qPrintable(error));
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 1);
    current = current.addSecs(2 * 3600);
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 2);
    current = current.addSecs(-3 * 3600);
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 2);

    const QTimeZone newYork("America/New_York");
    QVERIFY(newYork.isValid());
    const QDateTime afterSpringGap(QDate(2027, 3, 14), QTime(3, 30), newYork);
    QVERIFY(afterSpringGap.isValid());
    Event dst =
        reminderEvent(QStringLiteral("DST alarm"), afterSpringGap, QJsonArray{60});
    QVERIFY2(database.saveLocalEvent(&dst, OutboxOperation::Create, &error),
             qPrintable(error));
    const ReminderJob dstJob = reminderForEvent(&database, dst.id);
    QCOMPARE(dstJob.fireAt, afterSpringGap.toUTC().addSecs(-3600));
    current = dstJob.fireAt.addSecs(-1);
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 2);
    current = dstJob.fireAt;
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 3);

    Event cancelled = reminderEvent(QStringLiteral("Cancelled"),
                                    current.addSecs(10 * 60), QJsonArray{30});
    QVERIFY2(database.saveLocalEvent(&cancelled, OutboxOperation::Create, &error),
             qPrintable(error));
    cancelled.status = QStringLiteral("cancelled");
    QVERIFY2(database.saveLocalEvent(&cancelled, OutboxOperation::Update, &error),
             qPrintable(error));
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 3);
    QCOMPARE(reminderForEvent(&database, cancelled.id).id, qint64(0));
  }

  void pendingSeriesDeleteSuspendsExceptionReminderUntilUndo() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));
    QDateTime current(QDate(2027, 3, 2), QTime(12, 0), QTimeZone::UTC);
    FakeNotificationBackend backend;
    ReminderScheduler scheduler(
        &database, &backend, [&current]() { return current; },
        [](const QUrl&) { return true; });

    Event master =
        reminderEvent(QStringLiteral("Recurring series"), current.addSecs(10 * 60), {});
    master.uid = QStringLiteral("series-delete-reminder@example.test");
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=3");
    QVERIFY2(database.saveLocalEvent(&master, OutboxOperation::Create, &error),
             qPrintable(error));

    Event exception = reminderEvent(QStringLiteral("Detached occurrence"),
                                    current.addSecs(10 * 60), QJsonArray{30});
    exception.uid = master.uid;
    exception.recurrenceId = QStringLiteral("2027-03-02T12:10:00.000Z");
    QVERIFY2(database.saveLocalEvent(&exception, OutboxOperation::Create, &error, {},
                                     QStringLiteral("occurrence")),
             qPrintable(error));
    QVERIFY(reminderForEvent(&database, exception.id).id > 0);

    Event removal = master;
    removal.deleted = true;
    QVERIFY2(database.saveLocalEvent(&removal, OutboxOperation::Remove, &error,
                                     QStringLiteral("series-delete-reminder"),
                                     QStringLiteral("series")),
             qPrintable(error));
    QVERIFY(database.hasPendingSeriesRemoval(master.calendarId, master.uid, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 0);
    QCOMPARE(reminderForEvent(&database, exception.id).state,
             QStringLiteral("pending"));

    const QList<OutboxItem> operations = database.outboxItems(20, &error);
    const auto removalOperation = std::find_if(
        operations.cbegin(), operations.cend(), [](const OutboxItem& item) {
          return item.idempotencyKey == QStringLiteral("series-delete-reminder");
        });
    QVERIFY(removalOperation != operations.cend());
    QVERIFY2(database.discardOutbox(removalOperation->id, &error), qPrintable(error));
    QVERIFY(!database.hasPendingSeriesRemoval(master.calendarId, master.uid, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(backend.sent.first().summary, QStringLiteral("Detached occurrence"));
  }

  void deliveryFaultsAndRestartRecovery() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("store.sqlite"));
    Database database;
    QString error;
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    QDateTime current(QDate(2027, 4, 5), QTime(10, 0), QTimeZone::UTC);

    Event claimFailure = reminderEvent(QStringLiteral("Claim failure"),
                                       current.addSecs(5 * 60), QJsonArray{10});
    QVERIFY2(database.saveLocalEvent(&claimFailure, OutboxOperation::Create, &error),
             qPrintable(error));
    QVERIFY2(executeSql(databasePath,
                        QStringLiteral(
                            "CREATE TRIGGER fail_reminder_claim BEFORE UPDATE OF state "
                            "ON reminder_jobs WHEN NEW.state='claimed' BEGIN SELECT "
                            "RAISE(ABORT,'injected claim failure'); END"),
                        &error),
             qPrintable(error));
    FakeNotificationBackend firstBackend;
    {
      ReminderScheduler scheduler(
          &database, &firstBackend, [&current]() { return current; },
          [](const QUrl&) { return true; });
      QSignalSpy errors(&scheduler, &ReminderScheduler::notificationError);
      scheduler.checkNow();
      QCOMPARE(firstBackend.sent.size(), 0);
      QCOMPARE(errors.size(), 1);
    }
    QVERIFY(executeSql(databasePath, QStringLiteral("DROP TRIGGER fail_reminder_claim"),
                       &error));

    Event finishFailure = reminderEvent(QStringLiteral("Finish failure"),
                                        current.addSecs(5 * 60), QJsonArray{10});
    QVERIFY2(database.saveLocalEvent(&finishFailure, OutboxOperation::Create, &error),
             qPrintable(error));
    QVERIFY2(
        executeSql(
            databasePath,
            QStringLiteral("CREATE TRIGGER fail_reminder_finish BEFORE UPDATE OF state "
                           "ON reminder_jobs WHEN NEW.state='delivered' BEGIN SELECT "
                           "RAISE(ABORT,'injected finish failure'); END"),
            &error),
        qPrintable(error));
    {
      ReminderScheduler scheduler(
          &database, &firstBackend, [&current]() { return current; },
          [](const QUrl&) { return true; });
      QSignalSpy errors(&scheduler, &ReminderScheduler::notificationError);
      scheduler.checkNow();
      QCOMPARE(firstBackend.sent.size(), 2);
      QCOMPARE(errors.size(), 2);
    }
    QCOMPARE(reminderForEvent(&database, claimFailure.id).state,
             QStringLiteral("claimed"));
    QCOMPARE(reminderForEvent(&database, finishFailure.id).state,
             QStringLiteral("claimed"));
    QVERIFY(executeSql(databasePath,
                       QStringLiteral("DROP TRIGGER fail_reminder_finish"), &error));

    FakeNotificationBackend restartBackend;
    {
      ReminderScheduler scheduler(
          &database, &restartBackend, [&current]() { return current; },
          [](const QUrl&) { return true; });
      scheduler.start();
      scheduler.stop();
      QCoreApplication::processEvents();
      QCOMPARE(restartBackend.sent.size(), 0);
      QCOMPARE(reminderForEvent(&database, claimFailure.id).state,
               QStringLiteral("claimed"));
      QCOMPARE(reminderForEvent(&database, finishFailure.id).state,
               QStringLiteral("claimed"));

      current = current.addSecs(2 * 60);
      scheduler.checkNow();
      QCOMPARE(restartBackend.sent.size(), 2);
    }
    QCOMPARE(reminderForEvent(&database, claimFailure.id).state,
             QStringLiteral("delivered"));
    QCOMPARE(reminderForEvent(&database, finishFailure.id).state,
             QStringLiteral("delivered"));

    Event retry = reminderEvent(QStringLiteral("Backend retry"),
                                current.addSecs(5 * 60), QJsonArray{10});
    QVERIFY2(database.saveLocalEvent(&retry, OutboxOperation::Create, &error),
             qPrintable(error));
    FakeNotificationBackend retryBackend;
    retryBackend.result = FakeNotificationBackend::Result::Failure;
    ReminderScheduler retryScheduler(
        &database, &retryBackend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    retryScheduler.checkNow();
    QCOMPARE(retryBackend.sent.size(), 1);
    QCOMPARE(reminderForEvent(&database, retry.id).state, QStringLiteral("pending"));
    retryBackend.result = FakeNotificationBackend::Result::Success;
    retryScheduler.checkNow();
    QCOMPARE(retryBackend.sent.size(), 2);
    QCOMPARE(reminderForEvent(&database, retry.id).state, QStringLiteral("delivered"));
  }

  void crashBeforeSendWaitsForLeaseThenRetries() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    const QString databasePath = directory.filePath(QStringLiteral("store.sqlite"));
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    QDateTime current(QDate(2027, 4, 6), QTime(10, 0), QTimeZone::UTC);

    Event event = reminderEvent(QStringLiteral("Claimed before crash"),
                                current.addSecs(5 * 60), QJsonArray{10});
    QVERIFY2(database.saveLocalEvent(&event, OutboxOperation::Create, &error),
             qPrintable(error));
    const ReminderJob original = reminderForEvent(&database, event.id);
    QVERIFY(original.id > 0);

    const QString abandonedToken = QStringLiteral("abandoned-pre-send-attempt");
    const QDateTime leaseExpiresAt = current.addSecs(2 * 60);
    bool claimed = false;
    QVERIFY2(database.claimReminderDelivery(original.id, abandonedToken, current,
                                            leaseExpiresAt, &claimed, &error),
             qPrintable(error));
    QVERIFY(claimed);
    QCOMPARE(reminderForEvent(&database, event.id).state, QStringLiteral("claimed"));
    QCOMPARE(reminderForEvent(&database, event.id).leaseExpiresAt, leaseExpiresAt);

    database.close();
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));

    FakeNotificationBackend backend;
    ReminderScheduler scheduler(
        &database, &backend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    current = leaseExpiresAt.addSecs(-1);
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 0);
    QCOMPARE(reminderForEvent(&database, event.id).state, QStringLiteral("claimed"));

    current = leaseExpiresAt;
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(backend.sent.constFirst().fingerprint,
             QStringLiteral("reminder:") + original.fingerprint);
    QVERIFY(backend.sent.constFirst().deliveryToken != abandonedToken);
    QCOMPARE(reminderForEvent(&database, event.id).state, QStringLiteral("delivered"));

    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 1);
  }

  void crashAfterSendBeforeAckRetriesWithStableFingerprint() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    const QString databasePath = directory.filePath(QStringLiteral("store.sqlite"));
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    QDateTime current(QDate(2027, 4, 7), QTime(10, 0), QTimeZone::UTC);

    Event event = reminderEvent(QStringLiteral("Accepted before crash"),
                                current.addSecs(5 * 60), QJsonArray{10});
    QVERIFY2(database.saveLocalEvent(&event, OutboxOperation::Create, &error),
             qPrintable(error));

    FakeNotificationBackend acceptedBackend;
    acceptedBackend.result = FakeNotificationBackend::Result::Hold;
    {
      ReminderScheduler scheduler(
          &database, &acceptedBackend, [&current]() { return current; },
          [](const QUrl&) { return true; });
      scheduler.checkNow();
      QCOMPARE(acceptedBackend.sent.size(), 1);
      QCOMPARE(reminderForEvent(&database, event.id).state, QStringLiteral("claimed"));
      // Destroying the scheduler here models a daemon crash after Notify was
      // dispatched but before its asynchronous reply was durably acknowledged.
    }

    database.close();
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    current = current.addSecs(2 * 60);
    FakeNotificationBackend retryBackend;
    ReminderScheduler restarted(
        &database, &retryBackend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    restarted.checkNow();
    QCOMPARE(retryBackend.sent.size(), 1);
    QCOMPARE(retryBackend.sent.constFirst().fingerprint,
             acceptedBackend.sent.constFirst().fingerprint);
    QVERIFY(retryBackend.sent.constFirst().deliveryToken !=
            acceptedBackend.sent.constFirst().deliveryToken);
    QCOMPARE(reminderForEvent(&database, event.id).state, QStringLiteral("delivered"));

    // At-least-once delivery permits the narrow duplicate above, but the
    // stable fingerprint and confirmed delivered state suppress further sends.
    restarted.checkNow();
    QCOMPARE(retryBackend.sent.size(), 1);
  }

  void expiredAttemptCannotAcknowledgeReplacementClaim() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));
    const QDateTime claimedAt(QDate(2027, 4, 8), QTime(10, 0), QTimeZone::UTC);
    const QDateTime firstExpiry = claimedAt.addSecs(2 * 60);
    Event event = reminderEvent(QStringLiteral("Fenced retry"),
                                claimedAt.addSecs(5 * 60), QJsonArray{10});
    QVERIFY2(database.saveLocalEvent(&event, OutboxOperation::Create, &error),
             qPrintable(error));
    const ReminderJob job = reminderForEvent(&database, event.id);

    bool claimed = false;
    QVERIFY2(database.claimReminderDelivery(job.id, QStringLiteral("expired-token"),
                                            claimedAt, firstExpiry, &claimed, &error),
             qPrintable(error));
    QVERIFY(claimed);
    QVERIFY2(database.recoverExpiredReminderDeliveries(firstExpiry, &error),
             qPrintable(error));

    claimed = false;
    QVERIFY2(database.claimReminderDelivery(job.id, QStringLiteral("replacement-token"),
                                            firstExpiry, firstExpiry.addSecs(2 * 60),
                                            &claimed, &error),
             qPrintable(error));
    QVERIFY(claimed);
    error.clear();
    QVERIFY(!database.finishReminderDelivery(job.id, QStringLiteral("expired-token"),
                                             firstExpiry, &error));
    QVERIFY(error.contains(QStringLiteral("no longer active")));
    QCOMPARE(reminderForEvent(&database, event.id).state, QStringLiteral("claimed"));

    error.clear();
    QVERIFY2(database.finishReminderDelivery(
                 job.id, QStringLiteral("replacement-token"), firstExpiry, &error),
             qPrintable(error));
    QCOMPARE(reminderForEvent(&database, event.id).state, QStringLiteral("delivered"));
  }

  void invitationNewChangeSeenSleepAndRetrySuppression() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));
    QDateTime current(QDate(2027, 5, 6), QTime(11, 0), QTimeZone::UTC);
    FakeNotificationBackend backend;
    ReminderScheduler scheduler(
        &database, &backend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    scheduler.start();
    scheduler.stop();
    QCoreApplication::processEvents();

    Event invitation =
        invitationEvent(QStringLiteral("Planning session"), current.addDays(1));
    QVERIFY2(database.saveLocalEvent(&invitation, OutboxOperation::Create, &error),
             qPrintable(error));
    scheduler.eventsChanged({invitation.calendarId});
    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(backend.sent.constLast().summary,
             QStringLiteral("New calendar invitation"));
    scheduler.eventsChanged({invitation.calendarId});
    QCOMPARE(backend.sent.size(), 1);

    invitation.summary = QStringLiteral("Updated planning session");
    QVERIFY2(database.saveLocalEvent(&invitation, OutboxOperation::Update, &error),
             qPrintable(error));
    scheduler.eventsChanged({invitation.calendarId});
    QCOMPARE(backend.sent.size(), 2);
    QCOMPARE(backend.sent.constLast().summary, QStringLiteral("Invitation updated"));

    QJsonObject self = invitation.attendees.first().toObject();
    self.insert(QStringLiteral("responseStatus"), QStringLiteral("accepted"));
    invitation.attendees[0] = self;
    QVERIFY2(database.saveLocalEvent(&invitation, OutboxOperation::Update, &error),
             qPrintable(error));
    scheduler.eventsChanged({invitation.calendarId});
    QCOMPARE(backend.sent.size(), 2);

    invitation.status = QStringLiteral("cancelled");
    QVERIFY2(database.saveLocalEvent(&invitation, OutboxOperation::Update, &error),
             qPrintable(error));
    scheduler.eventsChanged({invitation.calendarId});
    QCOMPARE(backend.sent.size(), 3);
    QCOMPARE(backend.sent.constLast().summary, QStringLiteral("Invitation cancelled"));

    Event seen = invitationEvent(QStringLiteral("Already seen"), current.addDays(2));
    QVERIFY2(database.saveLocalEvent(&seen, OutboxOperation::Create, &error),
             qPrintable(error));
    QVERIFY(database.setSetting(QStringLiteral("invitation_seen_%1").arg(seen.id), true,
                                &error));
    scheduler.eventsChanged({seen.calendarId});
    QCOMPARE(backend.sent.size(), 3);
    QVERIFY(database.hasNotificationDeliveryForEvent(seen.id, &error));

    scheduler.handlePrepareForSleep(true);
    seen.location = QStringLiteral("Moved while sleeping");
    QVERIFY2(database.saveLocalEvent(&seen, OutboxOperation::Update, &error),
             qPrintable(error));
    scheduler.eventsChanged({seen.calendarId});
    QCOMPARE(backend.sent.size(), 3);
    scheduler.handlePrepareForSleep(false);
    QCoreApplication::processEvents();
    QCOMPARE(backend.sent.size(), 4);

    Event retry =
        invitationEvent(QStringLiteral("Retry invitation"), current.addDays(3));
    QVERIFY2(database.saveLocalEvent(&retry, OutboxOperation::Create, &error),
             qPrintable(error));
    backend.result = FakeNotificationBackend::Result::Failure;
    scheduler.eventsChanged({retry.calendarId});
    QCOMPARE(backend.sent.size(), 5);
    QVERIFY(!database.hasNotificationDeliveryForEvent(retry.id, &error));
    backend.result = FakeNotificationBackend::Result::Success;
    scheduler.eventsChanged({retry.calendarId});
    QCOMPARE(backend.sent.size(), 6);
    QVERIFY(database.hasNotificationDeliveryForEvent(retry.id, &error));
    scheduler.eventsChanged({retry.calendarId});
    QCOMPARE(backend.sent.size(), 6);
  }

  void existingSchemaRepairAndInvitationCrashWindow() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("store.sqlite"));
    QString error;
    {
      Database database;
      QVERIFY2(database.open(databasePath, &error), qPrintable(error));
      database.close();
    }
    QVERIFY2(executeSql(databasePath,
                        QStringLiteral("DROP TABLE notification_deliveries"), &error),
             qPrintable(error));
    QVERIFY2(executeSql(databasePath,
                        QStringLiteral("DROP INDEX reminder_jobs_claim_lease_index"),
                        &error),
             qPrintable(error));
    QVERIFY2(
        executeSql(databasePath,
                   QStringLiteral("ALTER TABLE reminder_jobs DROP COLUMN claimed_at"),
                   &error),
        qPrintable(error));
    QVERIFY2(
        executeSql(databasePath,
                   QStringLiteral("ALTER TABLE reminder_jobs DROP COLUMN claim_token"),
                   &error),
        qPrintable(error));
    QVERIFY2(executeSql(databasePath,
                        QStringLiteral(
                            "ALTER TABLE reminder_jobs DROP COLUMN lease_expires_at"),
                        &error),
             qPrintable(error));

    Database database;
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    Event invitation =
        invitationEvent(QStringLiteral("Schema repair invitation"),
                        QDateTime(QDate(2027, 6, 1), QTime(12, 0), QTimeZone::UTC));
    QVERIFY2(database.saveLocalEvent(&invitation, OutboxOperation::Create, &error),
             qPrintable(error));
    const QDateTime claimedAt(QDate(2027, 5, 31), QTime(12, 0), QTimeZone::UTC);
    bool claimed = false;
    QVERIFY(database.claimNotificationDelivery(
        QStringLiteral("test-invitation-fingerprint"), QStringLiteral("invitation_new"),
        invitation.id, invitation.localRevision, claimedAt, &claimed, &error));
    QVERIFY(claimed);
    QVERIFY2(
        executeSql(databasePath,
                   QStringLiteral(
                       "CREATE TRIGGER fail_invitation_finish BEFORE UPDATE OF "
                       "state ON notification_deliveries WHEN NEW.state='delivered' "
                       "BEGIN SELECT RAISE(ABORT,'injected invitation finish "
                       "failure'); END"),
                   &error),
        qPrintable(error));
    QVERIFY(!database.finishNotificationDelivery(
        QStringLiteral("test-invitation-fingerprint"), claimedAt, &error));
    QVERIFY(executeSql(databasePath,
                       QStringLiteral("DROP TRIGGER fail_invitation_finish"), &error));
    QVERIFY(
        database.recoverClaimedNotificationDeliveries(claimedAt.addSecs(30), &error));
    QVERIFY(database.hasNotificationDeliveryForEvent(invitation.id, &error));
    claimed = true;
    QVERIFY(database.claimNotificationDelivery(
        QStringLiteral("test-invitation-fingerprint"), QStringLiteral("invitation_new"),
        invitation.id, invitation.localRevision, claimedAt, &claimed, &error));
    QVERIFY(!claimed);

    Event reminder = reminderEvent(QStringLiteral("Schema repair reminder"),
                                   claimedAt.addSecs(5 * 60), QJsonArray{10});
    QVERIFY2(database.saveLocalEvent(&reminder, OutboxOperation::Create, &error),
             qPrintable(error));
    const ReminderJob job = reminderForEvent(&database, reminder.id);
    QVERIFY(job.id > 0);
    QVERIFY(!job.claimedAt.isValid());
  }
};

QTEST_MAIN(ReminderSchedulerTest)
#include "test_reminders.moc"
