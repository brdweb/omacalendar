// Copyright (c) 2026

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "core/database.h"

using namespace omacalendar;

namespace {

Account makeAccount(const QString& id, const QString& name) {
  Account account;
  account.id = id;
  account.provider = ProviderKind::Google;
  account.displayName = name;
  account.principal = "user@example.com";
  account.endpoint = "https://example.local/";
  account.enabled = true;
  account.authStatus = QStringLiteral("connected");
  account.createdAt = QDateTime(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::utc());
  account.updatedAt = account.createdAt;
  return account;
}

Calendar makeCalendar(const QString& id, const QString& accountId) {
  Calendar calendar;
  calendar.id = id;
  calendar.accountId = accountId;
  calendar.remoteId = QStringLiteral("remote-") + id;
  calendar.href = QStringLiteral("/cal/") + id;
  calendar.name = QStringLiteral("Calendar ") + id;
  calendar.description = QStringLiteral("Test calendar");
  calendar.color = QStringLiteral("#7aa2f7");
  calendar.timeZone = "UTC";
  calendar.readOnly = false;
  calendar.enabled = true;
  calendar.etag = QStringLiteral("etag-") + id;
  calendar.syncToken = QStringLiteral("sync-") + id;
  calendar.capabilities = QJsonObject{{"read", true}};
  calendar.lastSyncAt = QDateTime(QDate(2026, 1, 2), QTime(0, 0), QTimeZone::utc());
  return calendar;
}

Event makeRemoteEvent(const QString& calendarId, const QString& id, int offsetHours) {
  Event event;
  event.id = id;
  event.calendarId = calendarId;
  event.remoteId = QStringLiteral("remote-") + id;
  event.uid = QStringLiteral("uid-") + id;
  event.summary = QStringLiteral("Remote ");
  event.summary.append(id);
  event.description = QStringLiteral("");
  event.location = QStringLiteral("");
  event.etag = QStringLiteral("etag-") + id;
  event.recurrenceRule = QStringLiteral("");
  event.recurrenceId = QStringLiteral("");
  event.rawPayload = QStringLiteral("");
  event.rawFormat = QStringLiteral("");
  event.startUtc =
      QDateTime(QDate(2026, 2, 1), QTime(8 + offsetHours, 0), QTimeZone::utc());
  event.endUtc = event.startUtc.addSecs(3600);
  event.startDate = event.startUtc.date();
  event.endDate = event.endUtc.date();
  event.startTimeZone = QStringLiteral("UTC");
  event.endTimeZone = QStringLiteral("UTC");
  event.status = QStringLiteral("confirmed");
  event.transparency = QStringLiteral("opaque");
  event.allDay = false;
  event.createdAt = QDateTime(QDate(2026, 1, 3), QTime(9, 0), QTimeZone::utc());
  event.updatedAt = event.createdAt;
  return event;
}

Event makeLocalEvent(const QString& calendarId, const QString& id) {
  Event event;
  event.id = id;
  event.calendarId = calendarId;
  event.remoteId = QStringLiteral("");
  event.uid = id.isEmpty() ? QStringLiteral("") : QStringLiteral("local-uid-") + id;
  event.summary = QStringLiteral("Local ") + id;
  event.description = QStringLiteral("");
  event.location = QStringLiteral("");
  event.etag = QStringLiteral("etag-") + id;
  event.recurrenceRule = QStringLiteral("");
  event.recurrenceId = QStringLiteral("");
  event.rawPayload = QStringLiteral("");
  event.rawFormat = QStringLiteral("");
  event.startUtc = QDateTime(QDate(2026, 3, 1), QTime(10, 0), QTimeZone::utc());
  event.endUtc = event.startUtc.addSecs(7200);
  event.startDate = event.startUtc.date();
  event.endDate = event.endUtc.date();
  event.startTimeZone = QStringLiteral("UTC");
  event.endTimeZone = QStringLiteral("UTC");
  event.status = QStringLiteral("confirmed");
  event.transparency = QStringLiteral("opaque");
  event.allDay = false;
  return event;
}

}  // namespace

class DatabaseTest final : public QObject {
  Q_OBJECT

 private slots:
  void schemaAndSettings() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QCOMPARE(db.isOpen(), false);

    QString error;
    QVERIFY(db.open(directory.filePath("store.sqlite"), &error));
    QVERIFY(db.isOpen());
    QCOMPARE(error, QString());
    QCOMPARE(db.schemaVersion(), 1);
    QVERIFY(db.setSetting("timezone", QStringLiteral("UTC"), &error));
    QCOMPARE(db.setting("timezone", QStringLiteral("auto"), &error).toString(),
             QStringLiteral("UTC"));
    QCOMPARE(db.setting("missing", 123, &error).toInt(), 123);
  }

  void accountAndCalendarCrud() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QVERIFY(db.open(directory.filePath("store.sqlite")));

    Account google = makeAccount("acc-google", "Google");
    Account caldav = makeAccount("acc-caldav", "Caldav");
    QString error;
    QVERIFY(db.upsertAccount(google, &error));
    QVERIFY(db.upsertAccount(caldav, &error));
    const QList<Account> accounts = db.accounts(&error);
    QCOMPARE(accounts.size(), 2);

    Calendar gcal = makeCalendar("cal-google", google.id);
    Calendar ccal = makeCalendar("cal-caldav", caldav.id);
    QVERIFY(db.upsertCalendar(gcal, &error));
    QVERIFY(db.upsertCalendar(ccal, &error));

    const QList<Calendar> gcalendars = db.calendars(google.id, &error);
    const QList<Calendar> ccalendars = db.calendars(caldav.id, &error);
    QCOMPARE(gcalendars.size(), 1);
    QCOMPARE(ccalendars.size(), 1);
    QCOMPARE(gcalendars.first().accountId, google.id);
    QCOMPARE(ccalendars.first().id, ccal.id);

    QCOMPARE(db.removeAccount(google.id, &error), true);
    const QList<Calendar> nowForGoogle = db.calendars(google.id, &error);
    QVERIFY(nowForGoogle.isEmpty());
    const QList<Calendar> all = db.calendars({}, &error);
    QCOMPARE(all.size(), 1);
  }

  void eventsOutboxFlow() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QVERIFY(db.open(directory.filePath("store.sqlite")));
    QString error;

    Account account = makeAccount("acc-events", "Events");
    QVERIFY(db.upsertAccount(account, &error));
    Calendar cal = makeCalendar("cal-events", account.id);
    QVERIFY(db.upsertCalendar(cal, &error));

    Event remote1 = makeRemoteEvent(cal.id, "remote-1", 0);
    Event remote2 = makeRemoteEvent(cal.id, "remote-2", 3);
    QVERIFY2(db.applyRemoteEvent(remote1, &error),
             qPrintable(error.isEmpty() ? QStringLiteral("missing error") : error));
    QVERIFY2(db.applyRemoteEvent(remote2, &error),
             qPrintable(error.isEmpty() ? QStringLiteral("missing error") : error));

    const QDateTime queryStart(QDate(2026, 2, 1), QTime(7, 30), QTimeZone::utc());
    const QDateTime queryEnd(QDate(2026, 2, 1), QTime(12, 0), QTimeZone::utc());
    const QList<Event> overlap = db.eventsBetween(queryStart, queryEnd, {}, &error);
    QCOMPARE(overlap.size(), 2);

    Event allDay = makeRemoteEvent(cal.id, "all-day", 0);
    allDay.summary = QStringLiteral("All Day");
    allDay.allDay = true;
    allDay.startUtc = QDateTime(QDate(2026, 2, 1), QTime(0, 0), QTimeZone::utc());
    allDay.endUtc = QDateTime(QDate(2026, 2, 2), QTime(0, 0), QTimeZone::utc());
    allDay.startDate = QDate(2026, 2, 1);
    allDay.endDate = QDate(2026, 2, 2);
    QVERIFY2(db.applyRemoteEvent(allDay, &error),
             qPrintable(error.isEmpty() ? QStringLiteral("missing error") : error));

    const QList<Event> allDayOverlap = db.eventsBetween(
        QDateTime(QDate(2026, 2, 1), QTime(0, 0), QTimeZone::utc()),
        QDateTime(QDate(2026, 2, 3), QTime(0, 0), QTimeZone::utc()), {cal.id}, &error);
    QCOMPARE(allDayOverlap.size(), 3);

    QList<Event> outOfRange = db.eventsBetween(queryEnd, queryStart, {}, &error);
    QVERIFY(outOfRange.isEmpty());
    QVERIFY(!error.isEmpty());

    Event localOne = makeLocalEvent(cal.id, QString());
    QVERIFY2(db.saveLocalEvent(&localOne, OutboxOperation::Create, &error),
             qPrintable(error.isEmpty() ? QStringLiteral("missing error") : error));

    Event localTwo = makeLocalEvent(cal.id, QString());
    localTwo.summary = QStringLiteral("Local Two");
    QVERIFY2(db.saveLocalEvent(&localTwo, OutboxOperation::Create, &error),
             qPrintable(error.isEmpty() ? QStringLiteral("missing error") : error));

    QList<OutboxItem> readyAll = db.readyOutbox(100, &error);
    QCOMPARE(readyAll.size(), 2);
    const OutboxItem first = readyAll.first();
    QCOMPARE(first.operation, OutboxOperation::Create);
    QCOMPARE(first.state, OutboxState::Pending);
    QVERIFY2(db.updateOutboxState(first.id, OutboxState::Sending, 1,
                                  QDateTime::currentDateTimeUtc(), QStringLiteral(""),
                                  QStringLiteral(""), &error),
             qPrintable(error.isEmpty() ? QStringLiteral("missing error") : error));

    QList<OutboxItem> readyLimit = db.readyOutbox(1, &error);
    QCOMPARE(readyLimit.size(), 1);
    QCOMPARE(readyLimit.first().id, readyAll[1].id);

    Event removed = localTwo;
    QVERIFY(db.markLocalEventRemoved(removed.id, &error));
    QString lookupError;
    const Event reread = db.event(removed.id, &lookupError);
    QVERIFY(reread.id.isEmpty());
    QVERIFY(!lookupError.isEmpty());

    // A create that has never left the pending queue can be cancelled
    // locally. Neither a tombstone nor a remote Remove operation is needed.
    const QList<OutboxItem> readyAfterCancellation = db.readyOutbox(20, &error);
    QVERIFY(readyAfterCancellation.isEmpty());
  }
};

QTEST_MAIN(DatabaseTest)
#include "test_database.moc"
