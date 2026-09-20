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
#include <ctime>

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

class ScopedTimeZone final {
 public:
  explicit ScopedTimeZone(const QByteArray& timeZone)
      : m_wasSet(qEnvironmentVariableIsSet("TZ")), m_previous(qgetenv("TZ")) {
    qputenv("TZ", timeZone);
    ::tzset();
  }

  ~ScopedTimeZone() {
    if (m_wasSet) {
      qputenv("TZ", m_previous);
    } else {
      qunsetenv("TZ");
    }
    ::tzset();
  }

  ScopedTimeZone(const ScopedTimeZone&) = delete;
  ScopedTimeZone& operator=(const ScopedTimeZone&) = delete;

 private:
  bool m_wasSet = false;
  QByteArray m_previous;
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

QString notificationDeliveryState(const QString& databasePath,
                                  const QString& fingerprint,
                                  QString* errorMessage = nullptr) {
  const QString connectionName =
      QStringLiteral("notification-state-test-%1")
          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  QString state;
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
      query.prepare(QStringLiteral(
          "SELECT state FROM notification_deliveries WHERE fingerprint=?"));
      query.addBindValue(fingerprint);
      if (!query.exec()) {
        failure = query.lastError().text();
      } else if (query.next()) {
        state = query.value(0).toString();
      }
      connection.close();
    }
  }
  QSqlDatabase::removeDatabase(connectionName);
  if (!failure.isEmpty() && errorMessage != nullptr) {
    *errorMessage = failure;
  }
  return state;
}

}  // namespace

class ReminderSchedulerTest final : public QObject {
  Q_OBJECT

 private slots:
  void missingPrivacySettingDefaultsToGeneric() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));
    QDateTime current(QDate(2027, 1, 11), QTime(12, 0), QTimeZone::UTC);
    FakeNotificationBackend backend;
    ReminderScheduler scheduler(
        &database, &backend, [&current]() { return current; },
        [](const QUrl&) { return true; });

    Event event = reminderEvent(QStringLiteral("Private appointment"),
                                current.addSecs(10 * 60), QJsonArray{20});
    QVERIFY2(database.saveLocalEvent(&event, OutboxOperation::Create, &error),
             qPrintable(error));
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(backend.sent.constFirst().summary, QStringLiteral("Calendar reminder"));
    QCOMPARE(backend.sent.constFirst().body,
             QStringLiteral("An event is starting soon"));
    QVERIFY(!backend.sent.constFirst().summary.contains(event.summary));
    QVERIFY(!backend.sent.constFirst().body.contains(event.location));
  }

  void notificationMetadataIsEscapedBeforeDelivery() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));
    QVERIFY(database.setSetting(QStringLiteral("notificationPrivacy"),
                                QStringLiteral("full_details"), &error));
    QDateTime current(QDate(2027, 1, 11), QTime(13, 0), QTimeZone::UTC);
    FakeNotificationBackend backend;
    ReminderScheduler scheduler(
        &database, &backend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    scheduler.start();
    scheduler.stop();
    QCoreApplication::processEvents();

    Event reminder = reminderEvent(QStringLiteral("<b>Board & Budget</b>"),
                                   current.addSecs(10 * 60), QJsonArray{20});
    reminder.location = QStringLiteral("<img src='file:///etc/passwd'> & room");
    QVERIFY2(database.saveLocalEvent(&reminder, OutboxOperation::Create, &error),
             qPrintable(error));
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(backend.sent.constLast().summary,
             QStringLiteral("&lt;b&gt;Board &amp; Budget&lt;/b&gt;"));
    QVERIFY(backend.sent.constLast().body.contains(
        QStringLiteral("&lt;img src='file:///etc/passwd'&gt; &amp; room")));
    QVERIFY(!backend.sent.constLast().body.contains(QStringLiteral("<img")));

    Event invitation = invitationEvent(
        QStringLiteral("<a href='file:///etc/passwd'>Open</a>"), current.addDays(1));
    invitation.location = QStringLiteral("<b>Secret room</b>");
    QVERIFY2(database.saveLocalEvent(&invitation, OutboxOperation::Create, &error),
             qPrintable(error));
    scheduler.eventsChanged({invitation.calendarId});
    QCOMPARE(backend.sent.size(), 2);
    QCOMPARE(backend.sent.constLast().body,
             QStringLiteral("&lt;a href='file:///etc/passwd'&gt;Open&lt;/a&gt; — "
                            "&lt;b&gt;Secret room&lt;/b&gt;"));
    QVERIFY(!backend.sent.constLast().body.contains(QStringLiteral("<a")));
    QVERIFY(!backend.sent.constLast().body.contains(QStringLiteral("<b")));
  }

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
    QVERIFY(database.setSetting(QStringLiteral("notificationPrivacy"),
                                QStringLiteral("full_details"), &error));
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
    QVERIFY(database.hasCompletedNotificationDeliveryForEvent(seen.id, &error));

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
    QVERIFY(database.hasAnyNotificationDeliveryForEvent(retry.id, &error));
    QVERIFY(!database.hasCompletedNotificationDeliveryForEvent(retry.id, &error));
    backend.result = FakeNotificationBackend::Result::Success;
    scheduler.eventsChanged({retry.calendarId});
    QCOMPARE(backend.sent.size(), 6);
    QVERIFY(database.hasCompletedNotificationDeliveryForEvent(retry.id, &error));
    scheduler.eventsChanged({retry.calendarId});
    QCOMPARE(backend.sent.size(), 6);
  }

  void invitationBackendFailureRetriesOnRestartWithoutEventsChanged() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("store.sqlite"));
    Database database;
    QString error;
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    QDateTime current(QDate(2027, 5, 7), QTime(11, 0), QTimeZone::UTC);

    FakeNotificationBackend failingBackend;
    failingBackend.result = FakeNotificationBackend::Result::Failure;
    {
      ReminderScheduler scheduler(
          &database, &failingBackend, [&current]() { return current; },
          [](const QUrl&) { return true; });
      scheduler.start();
      scheduler.stop();
      QCoreApplication::processEvents();

      Event invitation =
          invitationEvent(QStringLiteral("Backend restart"), current.addDays(1));
      invitation.remoteId = QStringLiteral("backend-restart");
      invitation.uid = QStringLiteral("backend-restart@example.test");
      QVERIFY2(database.applyRemoteEvent(invitation, &error), qPrintable(error));
      scheduler.eventsChanged({invitation.calendarId});
      QCOMPARE(failingBackend.sent.size(), 1);
      invitation =
          database.eventByRemoteId(invitation.calendarId, invitation.remoteId, &error);
      QCOMPARE(notificationDeliveryState(databasePath,
                                         invitationFingerprint(invitation), &error),
               QStringLiteral("retry_pending"));
      QVERIFY(database.hasAnyNotificationDeliveryForEvent(invitation.id, &error));
      QVERIFY(
          !database.hasCompletedNotificationDeliveryForEvent(invitation.id, &error));
    }

    database.close();
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    FakeNotificationBackend retryBackend;
    ReminderScheduler restarted(
        &database, &retryBackend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    restarted.start();
    restarted.stop();
    QCoreApplication::processEvents();
    QCOMPARE(retryBackend.sent.size(), 1);
    QCOMPARE(retryBackend.sent.constFirst().summary,
             QStringLiteral("New calendar invitation"));
    QVERIFY(database.isNotificationDeliveryCompleted(
        retryBackend.sent.constFirst().fingerprint, &error));
  }

  void invitationPersistenceFailureRetriesOnRestartWithoutEventsChanged() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("store.sqlite"));
    Database database;
    QString error;
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    QDateTime current(QDate(2027, 5, 8), QTime(11, 0), QTimeZone::UTC);
    FakeNotificationBackend firstBackend;
    QString fingerprint;
    {
      ReminderScheduler scheduler(
          &database, &firstBackend, [&current]() { return current; },
          [](const QUrl&) { return true; });
      scheduler.start();
      scheduler.stop();
      QCoreApplication::processEvents();

      Event invitation =
          invitationEvent(QStringLiteral("Persistence restart"), current.addDays(1));
      invitation.remoteId = QStringLiteral("persistence-restart");
      invitation.uid = QStringLiteral("persistence-restart@example.test");
      QVERIFY2(database.applyRemoteEvent(invitation, &error), qPrintable(error));
      invitation =
          database.eventByRemoteId(invitation.calendarId, invitation.remoteId, &error);
      fingerprint = invitationFingerprint(invitation);
      QVERIFY2(executeSql(
                   databasePath,
                   QStringLiteral(
                       "CREATE TRIGGER fail_invitation_ack BEFORE UPDATE OF state "
                       "ON notification_deliveries WHEN NEW.state='delivered' "
                       "BEGIN SELECT RAISE(ABORT,'injected acknowledgement failure'); "
                       "END"),
                   &error),
               qPrintable(error));
      QSignalSpy errors(&scheduler, &ReminderScheduler::notificationError);
      scheduler.eventsChanged({invitation.calendarId});
      QCOMPARE(firstBackend.sent.size(), 1);
      QCOMPARE(errors.size(), 1);
      QCOMPARE(notificationDeliveryState(databasePath, fingerprint, &error),
               QStringLiteral("claimed"));
      QVERIFY(executeSql(databasePath,
                         QStringLiteral("DROP TRIGGER fail_invitation_ack"), &error));
    }

    database.close();
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    FakeNotificationBackend retryBackend;
    ReminderScheduler restarted(
        &database, &retryBackend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    restarted.start();
    restarted.stop();
    QCoreApplication::processEvents();
    QCOMPARE(retryBackend.sent.size(), 1);
    QCOMPARE(retryBackend.sent.constFirst().fingerprint, fingerprint);
    QVERIFY(retryBackend.sent.constFirst().deliveryToken !=
            firstBackend.sent.constFirst().deliveryToken);
    QVERIFY(database.isNotificationDeliveryCompleted(fingerprint, &error));
  }

  void syncCompletedDoesNotInitializeAfterBaselineFailure() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("store.sqlite"));
    Database database;
    QString error;
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    QDateTime current(QDate(2027, 5, 9), QTime(11, 0), QTimeZone::UTC);
    FakeNotificationBackend backend;
    ReminderScheduler scheduler(
        &database, &backend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    scheduler.start();
    scheduler.stop();
    QCoreApplication::processEvents();

    Account account;
    account.id = QStringLiteral("baseline-failure-account");
    account.provider = ProviderKind::Google;
    account.displayName = QStringLiteral("Baseline failure account");
    account.principal = QStringLiteral("baseline@example.test");
    account.enabled = true;
    account.authStatus = QStringLiteral("connected");
    QVERIFY2(database.upsertAccount(account, &error), qPrintable(error));
    Calendar calendar;
    calendar.id = QStringLiteral("baseline-failure-calendar");
    calendar.accountId = account.id;
    calendar.remoteId = QStringLiteral("baseline-failure-calendar");
    calendar.name = QStringLiteral("Baseline failure calendar");
    calendar.enabled = true;
    QVERIFY2(database.upsertCalendar(calendar, &error), qPrintable(error));
    Event invitation =
        invitationEvent(QStringLiteral("Baseline failure"), current.addDays(1));
    invitation.calendarId = calendar.id;
    invitation.remoteId = QStringLiteral("baseline-failure-invitation");
    invitation.uid = QStringLiteral("baseline-failure-invitation@example.test");
    QVERIFY2(database.applyRemoteEvent(invitation, &error), qPrintable(error));

    QVERIFY2(
        executeSql(databasePath,
                   QStringLiteral(
                       "CREATE TRIGGER fail_invitation_baseline BEFORE INSERT ON "
                       "notification_deliveries WHEN NEW.kind='invitation_baseline' "
                       "BEGIN SELECT RAISE(ABORT,'injected baseline failure'); END"),
                   &error),
        qPrintable(error));
    QSignalSpy errors(&scheduler, &ReminderScheduler::notificationError);
    scheduler.syncCompleted(account.id);
    QCOMPARE(errors.size(), 1);
    QVERIFY(executeSql(
        databasePath, QStringLiteral("DROP TRIGGER fail_invitation_baseline"), &error));
    scheduler.eventsChanged({calendar.id});
    QCOMPARE(backend.sent.size(), 0);

    scheduler.syncCompleted(account.id);
    QCOMPARE(backend.sent.size(), 0);
    invitation = database.eventByRemoteId(calendar.id, invitation.remoteId, &error);
    invitation.summary = QStringLiteral("Baseline failure updated");
    QVERIFY2(database.applyRemoteEvent(invitation, &error), qPrintable(error));
    scheduler.eventsChanged({calendar.id});
    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(backend.sent.constFirst().summary, QStringLiteral("Invitation updated"));
  }

  void existingInvitationsInNewCalendarEstablishBaselineOnFirstSync() {
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

    Account account;
    account.id = QStringLiteral("new-account");
    account.provider = ProviderKind::Google;
    account.displayName = QStringLiteral("New account");
    account.principal = QStringLiteral("user@example.test");
    account.enabled = true;
    account.authStatus = QStringLiteral("connected");
    QVERIFY2(database.upsertAccount(account, &error), qPrintable(error));

    Calendar calendar;
    calendar.id = QStringLiteral("new-calendar");
    calendar.accountId = account.id;
    calendar.remoteId = QStringLiteral("remote-new-calendar");
    calendar.name = QStringLiteral("Imported calendar");
    calendar.enabled = true;
    QVERIFY2(database.upsertCalendar(calendar, &error), qPrintable(error));

    Event imported =
        invitationEvent(QStringLiteral("Existing invitation"), current.addDays(1));
    imported.calendarId = calendar.id;
    imported.remoteId = QStringLiteral("existing-invitation");
    imported.uid = QStringLiteral("existing-invitation@example.test");
    QVERIFY2(database.applyRemoteEvent(imported, &error), qPrintable(error));
    scheduler.eventsChanged({calendar.id});
    QCOMPARE(backend.sent.size(), 0);
    scheduler.syncCompleted(account.id);
    QCOMPARE(backend.sent.size(), 0);

    Event arriving =
        invitationEvent(QStringLiteral("New invitation"), current.addDays(2));
    arriving.calendarId = calendar.id;
    arriving.remoteId = QStringLiteral("new-invitation");
    arriving.uid = QStringLiteral("new-invitation@example.test");
    QVERIFY2(database.applyRemoteEvent(arriving, &error), qPrintable(error));
    scheduler.eventsChanged({calendar.id});
    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(backend.sent.constFirst().summary,
             QStringLiteral("New calendar invitation"));
  }

  void staleReminderBacklogIsDismissedNotDelivered() {
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

    // A past one-time event whose reminder fired long ago must be swept, not
    // delivered: importing history through a new calendar or a restart must
    // not flood the user with stale notifications.
    Event past = reminderEvent(QStringLiteral("Ancient meeting"), current.addDays(-30),
                               QJsonArray{10});
    past.calendarId = QStringLiteral("local-default");
    QVERIFY2(database.saveLocalEvent(&past, OutboxOperation::Create, &error),
             qPrintable(error));
    // Upcoming occurrence but the reminder fired two days ago (daemon was
    // offline): the catch-up delivery is still useful.
    Event upcoming = reminderEvent(QStringLiteral("Upcoming meeting"),
                                   current.addDays(1), QJsonArray{60 * 24 * 3});
    upcoming.calendarId = QStringLiteral("local-default");
    QVERIFY2(database.saveLocalEvent(&upcoming, OutboxOperation::Create, &error),
             qPrintable(error));
    const ReminderJob pastJob = reminderForEvent(&database, past.id);
    QVERIFY(pastJob.id > 0);
    QCOMPARE(pastJob.state, QStringLiteral("pending"));
    const ReminderJob catchUpJob = reminderForEvent(&database, upcoming.id);
    QVERIFY(catchUpJob.id > 0);
    QVERIFY(catchUpJob.fireAt < current);

    scheduler.start();
    scheduler.stop();
    QCoreApplication::processEvents();

    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(backend.sent.constFirst().summary, QStringLiteral("Calendar reminder"));
    // Dismissed jobs leave the working set entirely; reminders() only returns
    // pending/snoozed/claimed/delivered states.
    const ReminderJob swept = reminderForEvent(&database, past.id);
    QCOMPARE(swept.id, qint64(0));
    const ReminderJob delivered = reminderForEvent(&database, upcoming.id);
    QCOMPARE(delivered.state, QStringLiteral("delivered"));
  }

  void allDayReminderStalenessUsesMachineLocalDate() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));

    const QDateTime beforeLocalMidnight(QDate(2027, 5, 6), QTime(6, 30),
                                        QTimeZone::UTC);
    QCOMPARE(beforeLocalMidnight.toLocalTime().date(), QDate(2027, 5, 5));
    Event upcoming = reminderEvent(QStringLiteral("Tomorrow all day"),
                                   beforeLocalMidnight, QJsonArray{3 * 24 * 60});
    upcoming.allDay = true;
    upcoming.timeKind = TimeKind::AllDay;
    upcoming.startUtc = {};
    upcoming.endUtc = {};
    upcoming.startDate = QDate(2027, 5, 6);
    upcoming.endDate = QDate(2027, 5, 7);
    QVERIFY2(database.saveLocalEvent(&upcoming, OutboxOperation::Create, &error),
             qPrintable(error));
    QCOMPARE(database.dueReminders(beforeLocalMidnight, 20, &error).size(), 1);

    FakeNotificationBackend backend;
    ReminderScheduler scheduler(
        &database, &backend, [beforeLocalMidnight]() { return beforeLocalMidnight; },
        [](const QUrl&) { return true; });
    scheduler.checkNow();
    QCOMPARE(backend.sent.size(), 1);

    Event crossing = reminderEvent(QStringLiteral("Midnight crossing"),
                                   beforeLocalMidnight, QJsonArray{3 * 24 * 60});
    crossing.allDay = true;
    crossing.timeKind = TimeKind::AllDay;
    crossing.startUtc = {};
    crossing.endUtc = {};
    crossing.startDate = QDate(2027, 5, 6);
    crossing.endDate = QDate(2027, 5, 7);
    QVERIFY2(database.saveLocalEvent(&crossing, OutboxOperation::Create, &error),
             qPrintable(error));

    const QDateTime afterLocalMidnight = beforeLocalMidnight.addSecs(3600);
    int nowCalls = 0;
    ReminderScheduler crossingScheduler(
        &database, &backend,
        [&]() { return nowCalls++ == 0 ? beforeLocalMidnight : afterLocalMidnight; },
        [](const QUrl&) { return true; });
    crossingScheduler.checkNow();
    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(reminderForEvent(&database, crossing.id).id, qint64(0));
  }

  void mutedCalendarSuppressesReminderDelivery() {
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

    Calendar muted;
    muted.id = QStringLiteral("muted-calendar");
    muted.accountId = QStringLiteral("local-account");
    muted.name = QStringLiteral("Muted");
    muted.enabled = true;
    muted.ignoreAlerts = true;
    QVERIFY2(database.upsertCalendar(muted, &error), qPrintable(error));
    Event event = reminderEvent(QStringLiteral("Muted meeting"),
                                current.addSecs(30 * 60), QJsonArray{10});
    event.calendarId = muted.id;
    QVERIFY2(database.saveLocalEvent(&event, OutboxOperation::Create, &error),
             qPrintable(error));

    scheduler.start();
    scheduler.stop();
    QCoreApplication::processEvents();

    QCOMPARE(backend.sent.size(), 0);
    const ReminderJob job = reminderForEvent(&database, event.id);
    QCOMPARE(job.state, QStringLiteral("pending"));
  }

  void invitationPastHistoryImportsSilently() {
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

    const auto remoteInvitation = [&](const QString& remoteId, const QString& summary,
                                      const QDateTime& start) {
      Event event = invitationEvent(summary, start);
      event.calendarId = QStringLiteral("local-default");
      event.remoteId = remoteId;
      event.uid = remoteId + QStringLiteral("@example.test");
      return event;
    };

    // A newly arriving invitation for a meeting that ended ten days ago is
    // history, not news: it must be imported silently.
    Event ancient = remoteInvitation(QStringLiteral("ancient-invitation"),
                                     QStringLiteral("Ancient"), current.addDays(-10));
    QVERIFY2(database.applyRemoteEvent(ancient, &error), qPrintable(error));
    // applyRemoteEvent takes the event by value; the stored id must be read
    // back from the database.
    ancient = database.eventByRemoteId(ancient.calendarId, ancient.remoteId, &error);
    QVERIFY(!ancient.id.isEmpty());
    scheduler.eventsChanged({ancient.calendarId});
    QCOMPARE(backend.sent.size(), 0);
    QVERIFY(database.hasCompletedNotificationDeliveryForEvent(ancient.id, &error));

    // An update to that same past invitation stays silent as well.
    ancient.summary = QStringLiteral("Ancient (updated)");
    QVERIFY2(database.applyRemoteEvent(ancient, &error), qPrintable(error));
    scheduler.eventsChanged({ancient.calendarId});
    QCOMPARE(backend.sent.size(), 0);

    ancient.status = QStringLiteral("cancelled");
    QVERIFY2(database.applyRemoteEvent(ancient, &error), qPrintable(error));
    scheduler.eventsChanged({ancient.calendarId});
    QCOMPARE(backend.sent.size(), 0);

    Event firstSeenCancellation =
        remoteInvitation(QStringLiteral("first-seen-cancellation"),
                         QStringLiteral("Cancelled once"), current.addDays(1));
    firstSeenCancellation.status = QStringLiteral("cancelled");
    QVERIFY2(database.applyRemoteEvent(firstSeenCancellation, &error),
             qPrintable(error));
    firstSeenCancellation = database.eventByRemoteId(
        firstSeenCancellation.calendarId, firstSeenCancellation.remoteId, &error);
    QVERIFY(!firstSeenCancellation.id.isEmpty());
    scheduler.eventsChanged({firstSeenCancellation.calendarId});
    QCOMPARE(backend.sent.size(), 0);
    QVERIFY(database.hasCompletedNotificationDeliveryForEvent(firstSeenCancellation.id,
                                                              &error));

    // An invitation that ended recently still notifies, and an upcoming one
    // keeps notifying through the same path.
    Event recent = remoteInvitation(QStringLiteral("recent-invitation"),
                                    QStringLiteral("Recent"), current.addSecs(-3600));
    QVERIFY2(database.applyRemoteEvent(recent, &error), qPrintable(error));
    recent = database.eventByRemoteId(recent.calendarId, recent.remoteId, &error);
    QVERIFY(!recent.id.isEmpty());
    scheduler.eventsChanged({recent.calendarId});
    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(backend.sent.constFirst().summary,
             QStringLiteral("New calendar invitation"));
  }

  void invitationDigestCollapsesBursts() {
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

    for (int index = 0; index < 6; ++index) {
      Event event = invitationEvent(QStringLiteral("Invite %1").arg(index),
                                    current.addDays(index + 1));
      event.calendarId = QStringLiteral("local-default");
      event.remoteId = QStringLiteral("digest-invitation-%1").arg(index);
      event.uid = QStringLiteral("digest-invitation-%1@example.test").arg(index);
      QVERIFY2(database.applyRemoteEvent(event, &error), qPrintable(error));
    }
    scheduler.eventsChanged({QStringLiteral("local-default")});
    QCOMPARE(backend.sent.size(), 1);
    QVERIFY(backend.sent.constFirst().summary.contains(QStringLiteral("6")));
    // Every digest member's fingerprint claim was finished after delivery.
    for (int index = 0; index < 6; ++index) {
      const Event stored = database.eventByRemoteId(
          QStringLiteral("local-default"),
          QStringLiteral("digest-invitation-%1").arg(index), &error);
      QVERIFY(!stored.id.isEmpty());
      QVERIFY2(database.hasCompletedNotificationDeliveryForEvent(stored.id, &error),
               qPrintable(error));
    }
    // Re-scanning the same history must not re-notify digest members.
    scheduler.eventsChanged({QStringLiteral("local-default")});
    QCOMPARE(backend.sent.size(), 1);

    QList<Event> digestRetry;
    for (int index = 0; index < 6; ++index) {
      Event event = invitationEvent(QStringLiteral("Digest retry %1").arg(index),
                                    current.addDays(index + 20));
      event.calendarId = QStringLiteral("local-default");
      event.remoteId = QStringLiteral("digest-invitation-retry-%1").arg(index);
      event.uid = QStringLiteral("digest-invitation-retry-%1@example.test").arg(index);
      QVERIFY2(database.applyRemoteEvent(event, &error), qPrintable(error));
      event = database.eventByRemoteId(event.calendarId, event.remoteId, &error);
      QVERIFY2(!event.id.isEmpty(), qPrintable(error));
      digestRetry.append(event);
    }
    backend.result = FakeNotificationBackend::Result::Failure;
    scheduler.eventsChanged({QStringLiteral("local-default")});
    QCOMPARE(backend.sent.size(), 2);
    QVERIFY(backend.sent.constLast().summary.contains(QStringLiteral("6")));
    for (const Event& event : std::as_const(digestRetry)) {
      QVERIFY(database.hasAnyNotificationDeliveryForEvent(event.id, &error));
      QVERIFY(!database.hasCompletedNotificationDeliveryForEvent(event.id, &error));
    }
    backend.result = FakeNotificationBackend::Result::Success;
    scheduler.eventsChanged({QStringLiteral("local-default")});
    QCOMPARE(backend.sent.size(), 3);
    QVERIFY(backend.sent.constLast().summary.contains(QStringLiteral("6")));
    for (const Event& event : std::as_const(digestRetry)) {
      QVERIFY(database.hasCompletedNotificationDeliveryForEvent(event.id, &error));
    }

    // A small follow-up batch notifies individually again.
    Event extra = invitationEvent(QStringLiteral("Invite later"), current.addDays(10));
    extra.calendarId = QStringLiteral("local-default");
    extra.remoteId = QStringLiteral("digest-invitation-later");
    extra.uid = QStringLiteral("digest-invitation-later@example.test");
    QVERIFY2(database.applyRemoteEvent(extra, &error), qPrintable(error));
    scheduler.eventsChanged({extra.calendarId});
    QCOMPARE(backend.sent.size(), 4);
    QCOMPARE(backend.sent.constLast().summary,
             QStringLiteral("New calendar invitation"));
  }

  void allDayInvitationCutoffUsesExclusiveEndDate() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));
    QDateTime current(QDate(2027, 5, 6), QTime(6, 30), QTimeZone::UTC);
    FakeNotificationBackend backend;
    ReminderScheduler scheduler(
        &database, &backend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    scheduler.start();
    scheduler.stop();
    QCoreApplication::processEvents();

    const auto allDayInvitation = [&](const QString& remoteId, const QDate& endDate) {
      Event event =
          invitationEvent(QStringLiteral("All day %1").arg(remoteId),
                          QDateTime(endDate.addDays(-1), QTime(9, 0), QTimeZone::UTC));
      event.calendarId = QStringLiteral("local-default");
      event.remoteId = remoteId;
      event.uid = remoteId + QStringLiteral("@example.test");
      event.allDay = true;
      event.startDate = endDate.addDays(-2);
      event.endDate = endDate;
      event.startUtc = QDateTime(event.startDate, QTime(0, 0), QTimeZone::UTC);
      event.endUtc = QDateTime(endDate, QTime(0, 0), QTimeZone::UTC);
      return event;
    };

    Event recent =
        allDayInvitation(QStringLiteral("all-day-recent"), QDate(2027, 5, 4));
    QVERIFY2(database.applyRemoteEvent(recent, &error), qPrintable(error));
    scheduler.eventsChanged({recent.calendarId});
    QCOMPARE(backend.sent.size(), 1);

    Event ancient =
        allDayInvitation(QStringLiteral("all-day-ancient"), QDate(2027, 5, 3));
    QVERIFY2(database.applyRemoteEvent(ancient, &error), qPrintable(error));
    scheduler.eventsChanged({ancient.calendarId});
    QCOMPARE(backend.sent.size(), 1);
  }

  void digestMemberPersistenceFailureIsReported() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("store.sqlite"));
    Database database;
    QString error;
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    QDateTime current(QDate(2027, 5, 6), QTime(11, 0), QTimeZone::UTC);
    FakeNotificationBackend backend;
    ReminderScheduler scheduler(
        &database, &backend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    scheduler.start();
    scheduler.stop();
    QCoreApplication::processEvents();

    QString failingEventId;
    QString failingFingerprint;
    QList<Event> invitations;
    for (int index = 0; index < 6; ++index) {
      Event event = invitationEvent(QStringLiteral("Persist %1").arg(index),
                                    current.addDays(index + 1));
      event.remoteId = QStringLiteral("digest-persist-%1").arg(index);
      event.uid = QStringLiteral("digest-persist-%1@example.test").arg(index);
      QVERIFY2(database.applyRemoteEvent(event, &error), qPrintable(error));
      event = database.eventByRemoteId(event.calendarId, event.remoteId, &error);
      QVERIFY2(!event.id.isEmpty(), qPrintable(error));
      if (index == 0) {
        failingEventId = event.id;
        failingFingerprint = invitationFingerprint(event);
      }
      invitations.append(event);
    }
    QVERIFY2(executeSql(
                 databasePath,
                 QStringLiteral(
                     "CREATE TRIGGER fail_digest_member_finish BEFORE UPDATE OF state "
                     "ON notification_deliveries WHEN NEW.state='delivered' AND "
                     "NEW.event_id='%1' BEGIN SELECT RAISE(ABORT,'injected digest "
                     "member finish failure'); END")
                     .arg(failingEventId),
                 &error),
             qPrintable(error));

    QSignalSpy errors(&scheduler, &ReminderScheduler::notificationError);
    scheduler.eventsChanged({QStringLiteral("local-default")});
    QCOMPARE(backend.sent.size(), 1);
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.constFirst().at(1).toString().contains(
        QStringLiteral("injected digest member finish failure")));
    QCOMPARE(notificationDeliveryState(databasePath, failingFingerprint, &error),
             QStringLiteral("claimed"));
    for (const Event& invitation : std::as_const(invitations)) {
      if (invitation.id == failingEventId) {
        QVERIFY(
            !database.hasCompletedNotificationDeliveryForEvent(invitation.id, &error));
      } else {
        QVERIFY(
            database.hasCompletedNotificationDeliveryForEvent(invitation.id, &error));
      }
    }
    QVERIFY(executeSql(databasePath,
                       QStringLiteral("DROP TRIGGER "
                                      "fail_digest_member_finish"),
                       &error));

    database.close();
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    FakeNotificationBackend retryBackend;
    {
      ReminderScheduler restarted(
          &database, &retryBackend, [&current]() { return current; },
          [](const QUrl&) { return true; });
      restarted.start();
      restarted.stop();
      QCoreApplication::processEvents();
    }
    QCOMPARE(retryBackend.sent.size(), 1);
    QCOMPARE(retryBackend.sent.constFirst().fingerprint, failingFingerprint);
    QVERIFY(database.hasCompletedNotificationDeliveryForEvent(failingEventId, &error));

    database.close();
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    FakeNotificationBackend completedBackend;
    ReminderScheduler completed(
        &database, &completedBackend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    completed.start();
    completed.stop();
    QCoreApplication::processEvents();
    QCOMPARE(completedBackend.sent.size(), 0);
  }

  void invitationBaselineSurvivesRestart() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));
    QDateTime current(QDate(2027, 5, 6), QTime(11, 0), QTimeZone::UTC);

    {
      FakeNotificationBackend backend;
      ReminderScheduler scheduler(
          &database, &backend, [&current]() { return current; },
          [](const QUrl&) { return true; });
      scheduler.start();
      scheduler.stop();
      QCoreApplication::processEvents();

      Account account;
      account.id = QStringLiteral("restart-account");
      account.provider = ProviderKind::Google;
      account.displayName = QStringLiteral("Restart account");
      account.principal = QStringLiteral("user@example.test");
      account.enabled = true;
      account.authStatus = QStringLiteral("connected");
      QVERIFY2(database.upsertAccount(account, &error), qPrintable(error));
      Calendar calendar;
      calendar.id = QStringLiteral("restart-calendar");
      calendar.accountId = account.id;
      calendar.remoteId = QStringLiteral("remote-restart-calendar");
      calendar.name = QStringLiteral("Restart calendar");
      calendar.enabled = true;
      QVERIFY2(database.upsertCalendar(calendar, &error), qPrintable(error));

      Event invitation =
          invitationEvent(QStringLiteral("Baseline session"), current.addDays(1));
      invitation.calendarId = calendar.id;
      invitation.remoteId = QStringLiteral("baseline-invitation");
      invitation.uid = QStringLiteral("baseline-invitation@example.test");
      QVERIFY2(database.applyRemoteEvent(invitation, &error), qPrintable(error));
      scheduler.eventsChanged({calendar.id});
      QCOMPARE(backend.sent.size(), 0);
      scheduler.syncCompleted(account.id);
      QCOMPARE(backend.sent.size(), 0);

      // The invitation changes while the daemon is down. The durable baseline
      // must preserve the change instead of swallowing it at startup.
      invitation.summary = QStringLiteral("Baseline session (moved)");
      QVERIFY2(database.applyRemoteEvent(invitation, &error), qPrintable(error));
    }

    FakeNotificationBackend restartedBackend;
    ReminderScheduler restarted(
        &database, &restartedBackend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    restarted.start();
    restarted.stop();
    QCoreApplication::processEvents();
    QCOMPARE(restartedBackend.sent.size(), 1);
    QCOMPARE(restartedBackend.sent.constFirst().summary,
             QStringLiteral("Invitation updated"));
  }

  void claimedInvitationRetriesAcrossCrashWindows() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("store.sqlite"));
    Database database;
    QString error;
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    QDateTime current(QDate(2027, 5, 10), QTime(11, 0), QTimeZone::UTC);

    {
      FakeNotificationBackend baselineBackend;
      ReminderScheduler baseline(
          &database, &baselineBackend, [&current]() { return current; },
          [](const QUrl&) { return true; });
      baseline.start();
      baseline.stop();
      QCoreApplication::processEvents();
    }

    Event invitation =
        invitationEvent(QStringLiteral("Restart invitation"), current.addDays(1));
    invitation.remoteId = QStringLiteral("restart-invitation");
    invitation.uid = QStringLiteral("restart-invitation@example.test");
    QVERIFY2(database.applyRemoteEvent(invitation, &error), qPrintable(error));
    invitation =
        database.eventByRemoteId(invitation.calendarId, invitation.remoteId, &error);
    QVERIFY2(!invitation.id.isEmpty(), qPrintable(error));
    const QString fingerprint = invitationFingerprint(invitation);
    bool claimed = false;
    QVERIFY2(database.claimNotificationDelivery(
                 fingerprint, QStringLiteral("invitation_new"), invitation.id,
                 invitation.localRevision, current, &claimed, &error),
             qPrintable(error));
    QVERIFY(claimed);

    database.close();
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    QVERIFY2(database.recoverClaimedNotificationDeliveries(current, &error),
             qPrintable(error));
    QCOMPARE(notificationDeliveryState(databasePath, fingerprint, &error),
             QStringLiteral("retry_pending"));
    QVERIFY(database.hasAnyNotificationDeliveryForEvent(invitation.id, &error));
    QVERIFY(!database.isNotificationDeliveryCompleted(fingerprint, &error));

    database.close();
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    QVERIFY2(database.recoverClaimedNotificationDeliveries(current, &error),
             qPrintable(error));
    QCOMPARE(notificationDeliveryState(databasePath, fingerprint, &error),
             QStringLiteral("retry_pending"));

    FakeNotificationBackend acceptedBackend;
    acceptedBackend.result = FakeNotificationBackend::Result::Hold;
    {
      ReminderScheduler restarted(
          &database, &acceptedBackend, [&current]() { return current; },
          [](const QUrl&) { return true; });
      restarted.start();
      restarted.stop();
      QCoreApplication::processEvents();
      QCOMPARE(acceptedBackend.sent.size(), 1);
      QCOMPARE(acceptedBackend.sent.constFirst().fingerprint, fingerprint);
      QCOMPARE(acceptedBackend.sent.constFirst().summary,
               QStringLiteral("New calendar invitation"));
    }

    database.close();
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    FakeNotificationBackend retryBackend;
    ReminderScheduler retried(
        &database, &retryBackend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    retried.start();
    retried.stop();
    QCoreApplication::processEvents();
    QCOMPARE(retryBackend.sent.size(), 1);
    QCOMPARE(retryBackend.sent.constFirst().fingerprint, fingerprint);
    QVERIFY(retryBackend.sent.constFirst().deliveryToken !=
            acceptedBackend.sent.constFirst().deliveryToken);
    QVERIFY(database.isNotificationDeliveryCompleted(fingerprint, &error));

    database.close();
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    FakeNotificationBackend completedBackend;
    ReminderScheduler completed(
        &database, &completedBackend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    completed.start();
    completed.stop();
    QCoreApplication::processEvents();
    QCOMPARE(completedBackend.sent.size(), 0);
  }

  void claimedDigestRetriesAcrossCrashWindows() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("store.sqlite"));
    Database database;
    QString error;
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    QDateTime current(QDate(2027, 5, 11), QTime(11, 0), QTimeZone::UTC);

    {
      FakeNotificationBackend baselineBackend;
      ReminderScheduler baseline(
          &database, &baselineBackend, [&current]() { return current; },
          [](const QUrl&) { return true; });
      baseline.start();
      baseline.stop();
      QCoreApplication::processEvents();
    }

    QList<Event> invitations;
    for (int index = 0; index < 6; ++index) {
      Event invitation = invitationEvent(QStringLiteral("Restart digest %1").arg(index),
                                         current.addDays(index + 1));
      invitation.remoteId = QStringLiteral("restart-digest-%1").arg(index);
      invitation.uid = QStringLiteral("restart-digest-%1@example.test").arg(index);
      QVERIFY2(database.applyRemoteEvent(invitation, &error), qPrintable(error));
      invitation =
          database.eventByRemoteId(invitation.calendarId, invitation.remoteId, &error);
      QVERIFY2(!invitation.id.isEmpty(), qPrintable(error));
      invitations.append(invitation);

      bool claimed = false;
      QVERIFY2(
          database.claimNotificationDelivery(
              invitationFingerprint(invitation), QStringLiteral("invitation_digest"),
              invitation.id, invitation.localRevision, current, &claimed, &error),
          qPrintable(error));
      QVERIFY(claimed);
    }

    FakeNotificationBackend acceptedBackend;
    acceptedBackend.result = FakeNotificationBackend::Result::Hold;
    {
      ReminderScheduler restarted(
          &database, &acceptedBackend, [&current]() { return current; },
          [](const QUrl&) { return true; });
      restarted.start();
      restarted.stop();
      QCoreApplication::processEvents();
      QCOMPARE(acceptedBackend.sent.size(), 1);
      QVERIFY(acceptedBackend.sent.constFirst().summary.contains(QStringLiteral("6")));
    }

    database.close();
    QVERIFY2(database.open(databasePath, &error), qPrintable(error));
    FakeNotificationBackend retryBackend;
    ReminderScheduler retried(
        &database, &retryBackend, [&current]() { return current; },
        [](const QUrl&) { return true; });
    retried.start();
    retried.stop();
    QCoreApplication::processEvents();
    QCOMPARE(retryBackend.sent.size(), 1);
    const CalendarNotification accepted = acceptedBackend.sent.constFirst();
    const CalendarNotification retriedNotification = retryBackend.sent.constFirst();
    QCOMPARE(retriedNotification.fingerprint, accepted.fingerprint);
    QVERIFY(retriedNotification.deliveryToken != accepted.deliveryToken);
    QCOMPARE(
        retriedNotification.hints.value(QStringLiteral("x-omacalendar-fingerprint"))
            .toString(),
        retriedNotification.fingerprint);
    for (const Event& invitation : std::as_const(invitations)) {
      QVERIFY2(database.hasCompletedNotificationDeliveryForEvent(invitation.id, &error),
               qPrintable(error));
    }
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
    claimed = true;
    QVERIFY(database.claimNotificationDelivery(
        QStringLiteral("test-invitation-fingerprint"), QStringLiteral("invitation_new"),
        invitation.id, invitation.localRevision, claimedAt, &claimed, &error));
    QVERIFY(!claimed);
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
    QCOMPARE(notificationDeliveryState(
                 databasePath, QStringLiteral("test-invitation-fingerprint"), &error),
             QStringLiteral("retry_pending"));
    QVERIFY(database.hasAnyNotificationDeliveryForEvent(invitation.id, &error));
    QVERIFY(!database.hasCompletedNotificationDeliveryForEvent(invitation.id, &error));
    claimed = false;
    QVERIFY(database.claimNotificationDelivery(
        QStringLiteral("test-invitation-fingerprint"), QStringLiteral("invitation_new"),
        invitation.id, invitation.localRevision, claimedAt, &claimed, &error));
    QVERIFY(claimed);
    QVERIFY(database.finishNotificationDelivery(
        QStringLiteral("test-invitation-fingerprint"), claimedAt, &error));
    claimed = true;
    QVERIFY(database.claimNotificationDelivery(
        QStringLiteral("test-invitation-fingerprint"), QStringLiteral("invitation_new"),
        invitation.id, invitation.localRevision, claimedAt, &claimed, &error));
    QVERIFY(!claimed);
    claimed = false;
    QVERIFY(database.claimNotificationDelivery(
        QStringLiteral("abandoned-invitation-fingerprint"),
        QStringLiteral("invitation_changed"), invitation.id,
        invitation.localRevision + 1, claimedAt, &claimed, &error));
    QVERIFY(claimed);
    QVERIFY(
        database.recoverClaimedNotificationDeliveries(claimedAt.addSecs(30), &error));
    QVERIFY(database.isNotificationDeliveryCompleted(
        QStringLiteral("test-invitation-fingerprint"), &error));
    QVERIFY(!database.isNotificationDeliveryCompleted(
        QStringLiteral("abandoned-invitation-fingerprint"), &error));
    QCOMPARE(
        notificationDeliveryState(
            databasePath, QStringLiteral("abandoned-invitation-fingerprint"), &error),
        QStringLiteral("retry_pending"));
    QVERIFY(database.hasCompletedNotificationDeliveryForEvent(invitation.id, &error));

    Event reminder = reminderEvent(QStringLiteral("Schema repair reminder"),
                                   claimedAt.addSecs(5 * 60), QJsonArray{10});
    QVERIFY2(database.saveLocalEvent(&reminder, OutboxOperation::Create, &error),
             qPrintable(error));
    const ReminderJob job = reminderForEvent(&database, reminder.id);
    QVERIFY(job.id > 0);
    QVERIFY(!job.claimedAt.isValid());
  }
};

int main(int argc, char** argv) {
  const ScopedTimeZone timeZone("America/Los_Angeles");
  QCoreApplication application(argc, argv);
  ReminderSchedulerTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "test_reminders.moc"
