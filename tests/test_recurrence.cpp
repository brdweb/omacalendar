#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest/QtTest>

#include "core/database.h"
#include "core/recurrenceexpander.h"

using namespace omacalendar;

namespace {

QDateTime utc(const int year, const int month, const int day, const int hour,
              const int minute = 0) {
  return QDateTime(QDate(year, month, day), QTime(hour, minute), QTimeZone::UTC);
}

Event timedEvent(const QString& id, const QDateTime& start,
                 const int durationSeconds = 3600) {
  Event event;
  event.id = id;
  event.calendarId = QStringLiteral("calendar");
  event.remoteId = QStringLiteral("remote-") + id;
  event.uid = QStringLiteral("uid-") + id;
  event.summary = id;
  event.startUtc = start;
  event.endUtc = start.addSecs(durationSeconds);
  event.startTimeZone = QStringLiteral("UTC");
  event.endTimeZone = QStringLiteral("UTC");
  return event;
}

Account account() {
  Account result;
  result.id = QStringLiteral("account");
  result.provider = ProviderKind::CalDav;
  result.displayName = QStringLiteral("Test account");
  result.endpoint = QStringLiteral("https://calendar.example.test/");
  return result;
}

Calendar calendar() {
  Calendar result;
  result.id = QStringLiteral("calendar");
  result.accountId = QStringLiteral("account");
  result.remoteId = QStringLiteral("calendar-remote");
  result.name = QStringLiteral("Test calendar");
  return result;
}

}  // namespace

class RecurrenceTest final : public QObject {
  Q_OBJECT

 private slots:
  void expandsSeriesWithoutChangingOrdinaryEvents() {
    Event master = timedEvent(QStringLiteral("series"), utc(2026, 8, 28, 13));
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=3");
    Event ordinary = timedEvent(QStringLiteral("ordinary"), utc(2026, 8, 29, 17));

    const RecurrenceExpansionResult result = RecurrenceExpander::expand(
        {master, ordinary}, utc(2026, 8, 28, 0), utc(2026, 9, 1, 0));

    QVERIFY2(result.warnings.isEmpty(),
             qPrintable(result.warnings.join(QLatin1Char(','))));
    QVERIFY(!result.truncated);
    QCOMPARE(result.occurrences.size(), 4);

    QList<QDateTime> seriesStarts;
    int ordinaryCount = 0;
    for (const Event& event : result.occurrences) {
      if (event.id == master.id) {
        seriesStarts.append(event.startUtc);
        QVERIFY(!event.recurrenceId.isEmpty());
      } else if (event.id == ordinary.id) {
        ++ordinaryCount;
        QCOMPARE(event.startUtc, ordinary.startUtc);
        QVERIFY(event.recurrenceId.isEmpty());
      }
    }
    QCOMPARE(ordinaryCount, 1);
    QCOMPARE(seriesStarts, QList<QDateTime>({utc(2026, 8, 28, 13), utc(2026, 8, 29, 13),
                                             utc(2026, 8, 30, 13)}));
  }

  void appliesExdatesMovedExceptionsAndCancellations() {
    Event master = timedEvent(QStringLiteral("series"), utc(2026, 8, 28, 13));
    master.uid = QStringLiteral("shared-uid");
    master.recurrenceRule =
        QStringLiteral("RRULE:FREQ=DAILY;COUNT=4\nEXDATE:20260830T130000Z");

    Event moved = timedEvent(QStringLiteral("moved"), utc(2026, 8, 29, 16));
    moved.uid = master.uid;
    moved.summary = QStringLiteral("Moved instance");
    moved.recurrenceId = QStringLiteral("2026-08-29T13:00:00.000Z");

    Event cancelled = timedEvent(QStringLiteral("cancelled"), utc(2026, 8, 31, 13));
    cancelled.uid = master.uid;
    cancelled.recurrenceId = QStringLiteral("20260831T130000Z");
    cancelled.deleted = true;
    cancelled.status = QStringLiteral("cancelled");

    const RecurrenceExpansionResult result = RecurrenceExpander::expand(
        {master, moved, cancelled}, utc(2026, 8, 28, 0), utc(2026, 9, 1, 0));

    QVERIFY2(result.warnings.isEmpty(),
             qPrintable(result.warnings.join(QLatin1Char(','))));
    QCOMPARE(result.occurrences.size(), 2);
    QCOMPARE(result.occurrences.at(0).startUtc, utc(2026, 8, 28, 13));
    QCOMPARE(result.occurrences.at(1).id, moved.id);
    QCOMPARE(result.occurrences.at(1).startUtc, utc(2026, 8, 29, 16));
  }

  void matchesSparseGoogleCancellationByParentId() {
    Event master = timedEvent(QStringLiteral("google-parent"), utc(2026, 8, 28, 13));
    master.remoteId = QStringLiteral("google-parent");
    master.uid = QStringLiteral("ical-series-uid");
    master.recurrenceRule = QStringLiteral("RRULE:FREQ=DAILY;COUNT=2");

    Event cancelled =
        timedEvent(QStringLiteral("google-instance"), utc(2026, 8, 29, 13));
    // Sparse Google cancellation resources can omit iCalUID. The mapper then
    // falls back to recurringEventId, which differs from the master's UID.
    cancelled.uid = QStringLiteral("google-parent");
    cancelled.recurrenceId = QStringLiteral("2026-08-29T13:00:00.000Z");
    cancelled.deleted = true;
    cancelled.status = QStringLiteral("cancelled");
    cancelled.rawFormat = QStringLiteral("google-json");
    cancelled.rawPayload = QStringLiteral("{\"recurringEventId\":\"google-parent\"}");

    const RecurrenceExpansionResult result = RecurrenceExpander::expand(
        {master, cancelled}, utc(2026, 8, 28, 0), utc(2026, 8, 31, 0));

    QCOMPARE(result.occurrences.size(), 1);
    QCOMPARE(result.occurrences.first().startUtc, utc(2026, 8, 28, 13));
  }

  void preservesLocalWallTimeAcrossDst() {
    const QTimeZone newYork("America/New_York");
    QVERIFY(newYork.isValid());
    Event master =
        timedEvent(QStringLiteral("dst"),
                   QDateTime(QDate(2026, 10, 31), QTime(9, 0), newYork).toUTC());
    master.startTimeZone = QStringLiteral("America/New_York");
    master.endTimeZone = master.startTimeZone;
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=3");

    const RecurrenceExpansionResult result =
        RecurrenceExpander::expand({master}, utc(2026, 10, 31, 0), utc(2026, 11, 4, 0));

    QCOMPARE(result.occurrences.size(), 3);
    for (const Event& event : result.occurrences) {
      QCOMPARE(event.startUtc.toTimeZone(newYork).time(), QTime(9, 0));
    }
    QCOMPARE(result.occurrences.at(0).startUtc, utc(2026, 10, 31, 13));
    QCOMPARE(result.occurrences.at(1).startUtc, utc(2026, 11, 1, 14));
    QCOMPARE(result.occurrences.at(2).startUtc, utc(2026, 11, 2, 14));
  }

  void matchesRangeExceptionWithEquivalentParameterForms() {
    const QTimeZone newYork("America/New_York");
    QVERIFY(newYork.isValid());
    Event master =
        timedEvent(QStringLiteral("range-identity"),
                   QDateTime(QDate(2026, 8, 28), QTime(9, 0), newYork).toUTC());
    master.uid = QStringLiteral("range-identity-series");
    master.startTimeZone = QStringLiteral("America/New_York");
    master.endTimeZone = master.startTimeZone;
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=3");

    Event range = master;
    range.id = QStringLiteral("range-identity-exception");
    range.recurrenceRule.clear();
    range.recurrenceId =
        QStringLiteral("RANGE=THISANDFUTURE;TZID=America/New_York:20260829T090000");
    range.startUtc = utc(2026, 8, 29, 11);
    range.endUtc = range.startUtc.addSecs(5400);

    const RecurrenceExpansionResult result = RecurrenceExpander::expand(
        {master, range}, utc(2026, 8, 28, 0), utc(2026, 9, 1, 0));

    QCOMPARE(result.occurrences.size(), 3);
    QCOMPARE(result.occurrences.at(0).startUtc, utc(2026, 8, 28, 13));
    QCOMPARE(result.occurrences.at(1).startUtc, utc(2026, 8, 29, 11));
    QCOMPARE(result.occurrences.at(2).startUtc, utc(2026, 8, 30, 11));
    QCOMPARE(
        (result.occurrences.at(1).endUtc - result.occurrences.at(1).startUtc).count(),
        5400000);
  }

  void expandsAllDayEventsWithExclusiveEndDates() {
    Event master;
    master.id = QStringLiteral("all-day");
    master.calendarId = QStringLiteral("calendar");
    master.remoteId = QStringLiteral("remote-all-day");
    master.uid = QStringLiteral("all-day-uid");
    master.summary = QStringLiteral("All day");
    master.allDay = true;
    master.startDate = QDate(2026, 8, 28);
    master.endDate = QDate(2026, 8, 29);
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=4");

    const RecurrenceExpansionResult result =
        RecurrenceExpander::expand({master}, utc(2026, 8, 29, 0), utc(2026, 8, 31, 0));

    QCOMPARE(result.occurrences.size(), 2);
    QCOMPARE(result.occurrences.at(0).startDate, QDate(2026, 8, 29));
    QCOMPARE(result.occurrences.at(0).endDate, QDate(2026, 8, 30));
    QCOMPARE(result.occurrences.at(1).startDate, QDate(2026, 8, 30));
    QCOMPARE(result.occurrences.at(1).endDate, QDate(2026, 8, 31));
  }

  void honorsExdateFromRetainedCalDavPayload() {
    Event master = timedEvent(QStringLiteral("raw-series"), utc(2026, 8, 28, 13));
    master.uid = QStringLiteral("raw-series-uid");
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=3");
    master.rawFormat = QStringLiteral("text/calendar");
    master.rawPayload = QStringLiteral(
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:raw-series-uid\r\n"
        "DTSTART:20260828T130000Z\r\n"
        "DTEND:20260828T140000Z\r\n"
        "RRULE:FREQ=DAILY;COUNT=3\r\n"
        "EXDATE:20260829T130000Z\r\n"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n");

    const RecurrenceExpansionResult result =
        RecurrenceExpander::expand({master}, utc(2026, 8, 28, 0), utc(2026, 9, 1, 0));

    QCOMPARE(result.occurrences.size(), 2);
    QCOMPARE(result.occurrences.at(0).startUtc, utc(2026, 8, 28, 13));
    QCOMPARE(result.occurrences.at(1).startUtc, utc(2026, 8, 30, 13));
  }

  void dirtyLocalRuleTakesPrecedenceOverStaleRawPayload() {
    Event master = timedEvent(QStringLiteral("edited-series"), utc(2026, 8, 28, 13));
    master.uid = QStringLiteral("edited-series-uid");
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=2");
    master.dirty = true;
    master.rawFormat = QStringLiteral("text/calendar");
    master.rawPayload = QStringLiteral(
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:edited-series-uid\r\n"
        "DTSTART:20260828T130000Z\r\n"
        "DTEND:20260828T140000Z\r\n"
        "RRULE:FREQ=WEEKLY;COUNT=2\r\n"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n");

    const RecurrenceExpansionResult result =
        RecurrenceExpander::expand({master}, utc(2026, 8, 28, 0), utc(2026, 9, 1, 0));

    QCOMPARE(result.occurrences.size(), 2);
    QCOMPARE(result.occurrences.at(0).startUtc, utc(2026, 8, 28, 13));
    QCOMPARE(result.occurrences.at(1).startUtc, utc(2026, 8, 29, 13));
  }

  void databaseQueryReturnsExpandedOccurrences() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("calendar.db")), &error),
             qPrintable(error));
    QVERIFY(database.upsertAccount(account(), &error));
    QVERIFY(database.upsertCalendar(calendar(), &error));

    Event master = timedEvent(QStringLiteral("db-series"), utc(2026, 8, 1, 9));
    master.calendarId = QStringLiteral("calendar");
    master.remoteId = QStringLiteral("/events/series.ics");
    master.uid = QStringLiteral("db-series-uid");
    master.recurrenceRule = QStringLiteral("FREQ=WEEKLY;COUNT=6");
    QVERIFY2(database.applyRemoteEvent(master, &error), qPrintable(error));

    const QList<Event> events =
        database.eventsBetween(utc(2026, 8, 28, 0), utc(2026, 8, 30, 0), {}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().startUtc, utc(2026, 8, 29, 9));
    QCOMPARE(events.first().id, master.id);
    QVERIFY(!events.first().recurrenceId.isEmpty());
  }

  void caldavSeriesDeletionPreservesDirtyExceptions() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("calendar.db")), &error),
             qPrintable(error));
    QVERIFY(database.upsertAccount(account(), &error));
    QVERIFY(database.upsertCalendar(calendar(), &error));

    const QString href = QStringLiteral("/events/series.ics");
    Event master = timedEvent(QStringLiteral("delete-series"), utc(2026, 8, 28, 13));
    master.remoteId = href;
    master.uid = QStringLiteral("delete-series-uid");
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=3");

    Event cleanException =
        timedEvent(QStringLiteral("clean-exception"), utc(2026, 8, 29, 15));
    cleanException.uid = master.uid;
    cleanException.recurrenceId = QStringLiteral("20260829T130000Z");
    cleanException.remoteId = href + QLatin1Char('#') + cleanException.recurrenceId;

    Event dirtyException =
        timedEvent(QStringLiteral("dirty-exception"), utc(2026, 8, 30, 16));
    dirtyException.uid = master.uid;
    dirtyException.recurrenceId = QStringLiteral("20260830T130000Z");
    dirtyException.remoteId = href + QLatin1Char('#') + dirtyException.recurrenceId;

    QVERIFY(database.applyRemoteEvent(master, &error));
    QVERIFY(database.applyRemoteEvent(cleanException, &error));
    QVERIFY(database.applyRemoteEvent(dirtyException, &error));
    dirtyException.summary = QStringLiteral("Locally edited exception");
    QVERIFY(database.saveLocalEvent(&dirtyException, OutboxOperation::Update, &error));

    bool conflicted = false;
    QVERIFY2(database.removeRemoteEvent(QStringLiteral("calendar"), href,
                                        QStringLiteral("remote series was removed"),
                                        &error, &conflicted),
             qPrintable(error));
    QVERIFY(conflicted);

    QString lookupError;
    QVERIFY(database.event(master.id, &lookupError).id.isEmpty());
    lookupError.clear();
    QVERIFY(database.event(cleanException.id, &lookupError).id.isEmpty());
    lookupError.clear();
    const Event preserved = database.event(dirtyException.id, &lookupError);
    QVERIFY2(lookupError.isEmpty(), qPrintable(lookupError));
    QCOMPARE(preserved.id, dirtyException.id);
    QVERIFY(preserved.dirty);
    QCOMPARE(preserved.summary, QStringLiteral("Locally edited exception"));
  }

  void googleMasterDeletionFindsExceptionsByUid() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("calendar.db")), &error),
             qPrintable(error));
    QVERIFY(database.upsertAccount(account(), &error));
    QVERIFY(database.upsertCalendar(calendar(), &error));

    Event master = timedEvent(QStringLiteral("google-series"), utc(2026, 8, 28, 13));
    master.remoteId = QStringLiteral("google-master-id");
    master.uid = QStringLiteral("google-series-ical-uid");
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=3");

    Event cleanException =
        timedEvent(QStringLiteral("google-clean"), utc(2026, 8, 29, 15));
    cleanException.remoteId = QStringLiteral("google-instance-a");
    cleanException.uid = master.uid;
    cleanException.recurrenceId = QStringLiteral("2026-08-29T13:00:00.000Z");

    Event dirtyException =
        timedEvent(QStringLiteral("google-dirty"), utc(2026, 8, 30, 16));
    dirtyException.remoteId = QStringLiteral("google-instance-b");
    dirtyException.uid = master.uid;
    dirtyException.recurrenceId = QStringLiteral("2026-08-30T13:00:00.000Z");

    QVERIFY(database.applyRemoteEvent(master, &error));
    QVERIFY(database.applyRemoteEvent(cleanException, &error));
    QVERIFY(database.applyRemoteEvent(dirtyException, &error));
    dirtyException.summary = QStringLiteral("Keep my local edit");
    QVERIFY(database.saveLocalEvent(&dirtyException, OutboxOperation::Update, &error));

    bool conflicted = false;
    QVERIFY2(database.removeRemoteEvent(QStringLiteral("calendar"), master.remoteId,
                                        QStringLiteral("remote series was removed"),
                                        &error, &conflicted),
             qPrintable(error));
    QVERIFY(conflicted);

    QString lookupError;
    QVERIFY(database.event(master.id, &lookupError).id.isEmpty());
    lookupError.clear();
    QVERIFY(database.event(cleanException.id, &lookupError).id.isEmpty());
    lookupError.clear();
    const Event preserved = database.event(dirtyException.id, &lookupError);
    QVERIFY2(lookupError.isEmpty(), qPrintable(lookupError));
    QCOMPARE(preserved.id, dirtyException.id);
    QVERIFY(preserved.dirty);
    QCOMPARE(preserved.summary, QStringLiteral("Keep my local edit"));
  }

  void deletingOneGoogleExceptionDoesNotPurgeSeries() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("calendar.db")), &error),
             qPrintable(error));
    QVERIFY(database.upsertAccount(account(), &error));
    QVERIFY(database.upsertCalendar(calendar(), &error));

    Event master =
        timedEvent(QStringLiteral("single-delete-series"), utc(2026, 8, 28, 13));
    master.remoteId = QStringLiteral("single-delete-master");
    master.uid = QStringLiteral("single-delete-uid");
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=3");
    Event removed =
        timedEvent(QStringLiteral("removed-instance"), utc(2026, 8, 29, 15));
    removed.remoteId = QStringLiteral("google-removed-instance");
    removed.uid = master.uid;
    removed.recurrenceId = QStringLiteral("20260829T130000Z");
    Event sibling =
        timedEvent(QStringLiteral("sibling-instance"), utc(2026, 8, 30, 16));
    sibling.remoteId = QStringLiteral("google-sibling-instance");
    sibling.uid = master.uid;
    sibling.recurrenceId = QStringLiteral("20260830T130000Z");

    QVERIFY(database.applyRemoteEvent(master, &error));
    QVERIFY(database.applyRemoteEvent(removed, &error));
    QVERIFY(database.applyRemoteEvent(sibling, &error));
    QVERIFY(database.removeRemoteEvent(QStringLiteral("calendar"), removed.remoteId, {},
                                       &error));

    QString lookupError;
    QVERIFY(database.event(removed.id, &lookupError).id.isEmpty());
    lookupError.clear();
    QCOMPARE(database.event(master.id, &lookupError).id, master.id);
    QVERIFY2(lookupError.isEmpty(), qPrintable(lookupError));
    QCOMPARE(database.event(sibling.id, &lookupError).id, sibling.id);
  }
};

QTEST_MAIN(RecurrenceTest)
#include "test_recurrence.moc"
