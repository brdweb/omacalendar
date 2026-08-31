#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QUuid>
#include <algorithm>

#include "core/database.h"
#include "core/widgeteventquery.h"

using namespace omacalendar;

namespace {

QDateTime utc(const int year, const int month, const int day, const int hour = 0,
              const int minute = 0) {
  return QDateTime(QDate(year, month, day), QTime(hour, minute), QTimeZone::UTC);
}

Event timedEvent(const QString& id, const QDateTime& start,
                 const bool invitation = true,
                 const QString& calendarId = QStringLiteral("local-default")) {
  Event event;
  event.id = id;
  event.calendarId = calendarId;
  event.remoteId = QStringLiteral("remote-") + id;
  event.uid = QStringLiteral("uid-") + id;
  event.summary = id;
  event.startUtc = start;
  event.endUtc = start.addSecs(3600);
  event.startTimeZone = QStringLiteral("UTC");
  event.endTimeZone = QStringLiteral("UTC");
  if (invitation) {
    event.organizer = {
        {QStringLiteral("email"), QStringLiteral("organizer@example.test")}};
    event.attendees = QJsonArray{
        QJsonObject{{QStringLiteral("email"), QStringLiteral("owner@example.test")},
                    {QStringLiteral("self"), true},
                    {QStringLiteral("responseStatus"), QStringLiteral("needsAction")}}};
  }
  return event;
}

}  // namespace

class WidgetQueriesTest final : public QObject {
  Q_OBJECT

 private slots:
  void currentAndUpNextIgnorePastAndFutureBrowseRanges() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));

    const QDateTime now = utc(2026, 9, 10, 12);
    Event past = timedEvent(QStringLiteral("past-browse"), utc(2024, 3, 1, 9), false);
    Event future =
        timedEvent(QStringLiteral("future-browse"), utc(2030, 11, 20, 9), false);
    Event ongoing =
        timedEvent(QStringLiteral("ongoing-now"), utc(2026, 9, 10, 11, 30), false);
    Event later =
        timedEvent(QStringLiteral("later-today"), utc(2026, 9, 10, 14), false);
    for (const Event& event : {past, future, ongoing, later}) {
      QVERIFY2(database.applyRemoteEvent(event, &error), qPrintable(error));
    }

    const WidgetEventQueryResult pastBrowse =
        queryWidgetEvents(database, utc(2024, 3, 1), utc(2024, 3, 2), now,
                          {QStringLiteral("local-default")}, {}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(pastBrowse.events.size(), 1);
    QCOMPARE(pastBrowse.events.first().id, QStringLiteral("past-browse"));
    QCOMPARE(pastBrowse.currentEvent.id, QStringLiteral("ongoing-now"));
    QCOMPARE(pastBrowse.upNext.id, QStringLiteral("ongoing-now"));

    const WidgetEventQueryResult futureBrowse =
        queryWidgetEvents(database, utc(2030, 11, 20), utc(2030, 11, 21), now,
                          {QStringLiteral("local-default")}, {}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(futureBrowse.events.size(), 1);
    QCOMPARE(futureBrowse.events.first().id, QStringLiteral("future-browse"));
    QCOMPARE(futureBrowse.currentEvent.id, pastBrowse.currentEvent.id);
    QCOMPARE(futureBrowse.upNext.id, pastBrowse.upNext.id);

    // Searching the popup is range-independent too; it must not retarget the
    // bar to the matching distant event.
    const WidgetEventQueryResult searchedBrowse = queryWidgetEvents(
        database, utc(2030, 11, 20), utc(2030, 11, 21), now,
        {QStringLiteral("local-default")}, QStringLiteral("future-browse"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(searchedBrowse.events.size(), 1);
    QCOMPARE(searchedBrowse.events.first().id, QStringLiteral("future-browse"));
    QCOMPARE(searchedBrowse.currentEvent.id, QStringLiteral("ongoing-now"));
    QCOMPARE(searchedBrowse.upNext.id, QStringLiteral("ongoing-now"));
  }

  void nowSelectionHonorsActiveCalendarSetMembership() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));

    Calendar focusedCalendar;
    focusedCalendar.id = QStringLiteral("focused-calendar");
    focusedCalendar.accountId = QStringLiteral("local-account");
    focusedCalendar.name = QStringLiteral("Focused");
    focusedCalendar.timeZone = QStringLiteral("UTC");
    QVERIFY2(database.upsertCalendar(focusedCalendar, &error), qPrintable(error));

    CalendarSet focusedSet;
    focusedSet.id = QStringLiteral("focused-set");
    focusedSet.name = QStringLiteral("Focused set");
    focusedSet.calendarIds = {focusedCalendar.id};
    focusedSet.defaultCalendarId = focusedCalendar.id;
    QVERIFY2(database.upsertCalendarSet(&focusedSet, &error), qPrintable(error));
    QVERIFY2(database.activateCalendarSet(focusedSet.id, &error), qPrintable(error));

    const QDateTime now = utc(2026, 9, 10, 12);
    Event excluded =
        timedEvent(QStringLiteral("excluded-ongoing"), utc(2026, 9, 10, 11, 15), false);
    excluded.endUtc = utc(2026, 9, 10, 12, 45);
    Event included = timedEvent(QStringLiteral("included-up-next"),
                                utc(2026, 9, 10, 12, 30), false, focusedCalendar.id);
    QVERIFY2(database.applyRemoteEvent(excluded, &error), qPrintable(error));
    QVERIFY2(database.applyRemoteEvent(included, &error), qPrintable(error));

    const WidgetEventQueryResult focused =
        queryWidgetEvents(database, utc(2032, 1, 1), utc(2032, 2, 1), now,
                          focusedSet.calendarIds, {}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(focused.currentEvent.id.isEmpty());
    QCOMPARE(focused.upNext.id, QStringLiteral("included-up-next"));
    QCOMPARE(focused.upNext.calendarId, focusedCalendar.id);

    const WidgetEventQueryResult allCalendars = queryWidgetEvents(
        database, utc(2032, 1, 1), utc(2032, 2, 1), now, {}, {}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(allCalendars.currentEvent.id, QStringLiteral("excluded-ongoing"));
    QCOMPARE(allCalendars.upNext.id, QStringLiteral("excluded-ongoing"));
  }

  void nowSelectionHandlesNoEventAndChoosesSoonestEndingOngoingEvent() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));

    const QDateTime now = utc(2026, 9, 10, 12);
    WidgetEventQueryResult selection =
        queryWidgetEvents(database, utc(2026, 1, 1), utc(2026, 2, 1), now,
                          {QStringLiteral("local-default")}, {}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(selection.currentEvent.id.isEmpty());
    QVERIFY(selection.upNext.id.isEmpty());

    Event laterEnding =
        timedEvent(QStringLiteral("later-ending"), utc(2026, 9, 10, 11), false);
    laterEnding.endUtc = utc(2026, 9, 10, 13);
    Event soonerEnding =
        timedEvent(QStringLiteral("sooner-ending"), utc(2026, 9, 10, 11, 30), false);
    soonerEnding.endUtc = utc(2026, 9, 10, 12, 15);
    QVERIFY2(database.applyRemoteEvent(laterEnding, &error), qPrintable(error));
    QVERIFY2(database.applyRemoteEvent(soonerEnding, &error), qPrintable(error));

    const qint64 revisionBeforeRead = database.changeRevision();
    selection = queryWidgetEvents(database, utc(2026, 1, 1), utc(2026, 2, 1), now,
                                  {QStringLiteral("local-default")}, {}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(selection.currentEvent.id, QStringLiteral("sooner-ending"));
    // Up Next retains its established earliest-start behavior for compatibility.
    QCOMPARE(selection.upNext.id, QStringLiteral("later-ending"));
    QCOMPARE(database.changeRevision(), revisionBeforeRead);
  }

  void invitationCandidatesPreserveRangeRecurrenceAndSeenState() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));

    Event invited = timedEvent(QStringLiteral("invited"), utc(2026, 9, 1, 8));
    QVERIFY2(database.applyRemoteEvent(invited, &error), qPrintable(error));
    Event ordinary = timedEvent(QStringLiteral("ordinary"), utc(2026, 9, 1, 9), false);
    QVERIFY2(database.applyRemoteEvent(ordinary, &error), qPrintable(error));
    Event outside = timedEvent(QStringLiteral("outside"), utc(2027, 1, 1, 9));
    QVERIFY2(database.applyRemoteEvent(outside, &error), qPrintable(error));

    Event master = timedEvent(QStringLiteral("series"), utc(2026, 9, 1, 10));
    master.uid = QStringLiteral("series-uid");
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=3");
    QVERIFY2(database.applyRemoteEvent(master, &error), qPrintable(error));
    Event nonInvitationException = master;
    nonInvitationException.id = QStringLiteral("series-exception");
    nonInvitationException.remoteId = QStringLiteral("remote-series-exception");
    nonInvitationException.recurrenceRule.clear();
    nonInvitationException.recurrenceId = QStringLiteral("20260902T100000Z");
    nonInvitationException.startUtc = utc(2026, 9, 2, 11);
    nonInvitationException.endUtc = utc(2026, 9, 2, 12);
    nonInvitationException.organizer = {};
    nonInvitationException.attendees = {};
    QVERIFY2(database.applyRemoteEvent(nonInvitationException, &error),
             qPrintable(error));

    const QList<Event> invitations =
        database.invitationEventsBetween(utc(2026, 9, 1), utc(2026, 9, 5), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(invitations.size(), 3);
    QCOMPARE(std::count_if(invitations.cbegin(), invitations.cend(),
                           [](const Event& event) {
                             return event.id == QStringLiteral("series");
                           }),
             2);
    QVERIFY(std::any_of(
        invitations.cbegin(), invitations.cend(),
        [](const Event& event) { return event.id == QStringLiteral("invited"); }));
    QVERIFY(
        std::none_of(invitations.cbegin(), invitations.cend(), [](const Event& event) {
          return event.id == QStringLiteral("ordinary") ||
                 event.id == QStringLiteral("outside") ||
                 event.id == QStringLiteral("series-exception");
        }));

    QVERIFY(
        database.setSetting(QStringLiteral("invitation_seen_invited"), true, &error));
    QVERIFY(
        database.setSetting(QStringLiteral("invitation_seen_series"), false, &error));
    QStringList ids;
    ids.reserve(805);
    ids.append(QStringLiteral("invited"));
    ids.append(QStringLiteral("series"));
    for (int index = 0; index < 803; ++index) {
      ids.append(QStringLiteral("missing-%1").arg(index));
    }
    const QHash<QString, bool> seen = database.invitationSeenStates(ids, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(seen.size(), 805);
    QCOMPARE(seen.value(QStringLiteral("invited")), true);
    QCOMPARE(seen.value(QStringLiteral("series")), false);
    QCOMPARE(seen.value(QStringLiteral("missing-802")), false);
  }

  void boundedEventPlanUsesKindSpecificIndexes() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("store.sqlite"));
    Database database;
    QString error;
    QVERIFY2(database.open(path, &error), qPrintable(error));

    const QString connectionName =
        QStringLiteral("widget-plan-%1").arg(QUuid::createUuid().toString());
    QStringList plan;
    QStringList invitationPlan;
    {
      QSqlDatabase reader =
          QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
      reader.setDatabaseName(path);
      QVERIFY(reader.open());
      QSqlQuery query(reader);
      QVERIFY(query.exec(QStringLiteral(R"SQL(
        EXPLAIN QUERY PLAN
        SELECT * FROM (
          SELECT e.* FROM events AS e
          WHERE e.deleted=0 AND e.all_day=0
            AND e.recurrence_rule='' AND e.recurrence_id=''
            AND e.end_utc>'2026-09-01T00:00:00.000Z'
            AND e.start_utc<'2026-09-08T00:00:00.000Z'
          UNION ALL
          SELECT e.* FROM events AS e
          WHERE e.deleted=0 AND e.all_day=1
            AND e.recurrence_rule='' AND e.recurrence_id=''
            AND e.end_date>'2026-09-01' AND e.start_date<'2026-09-08'
          UNION ALL
          SELECT e.* FROM events AS e
          WHERE e.deleted=0
            AND (e.recurrence_rule<>'' OR e.recurrence_id<>'')
        ) AS bounded_events
        ORDER BY all_day DESC, COALESCE(NULLIF(start_utc,''),start_date),id
      )SQL")));
      while (query.next()) {
        plan.append(query.value(3).toString());
      }
      QVERIFY(query.exec(QStringLiteral(R"SQL(
        EXPLAIN QUERY PLAN
        SELECT * FROM (
          SELECT e.* FROM events AS e
          WHERE e.deleted=0 AND e.all_day=0
            AND e.recurrence_rule='' AND e.recurrence_id=''
            AND e.end_utc>'2024-09-01T00:00:00.000Z'
            AND e.start_utc<'2031-09-01T00:00:00.000Z'
            AND e.organizer_json NOT IN ('','{}')
            AND e.attendees_json NOT IN ('','[]')
          UNION ALL
          SELECT e.* FROM events AS e
          WHERE e.deleted=0 AND e.all_day=1
            AND e.recurrence_rule='' AND e.recurrence_id=''
            AND e.end_date>'2024-09-01' AND e.start_date<'2031-09-01'
            AND e.organizer_json NOT IN ('','{}')
            AND e.attendees_json NOT IN ('','[]')
          UNION ALL
          SELECT e.* FROM events AS e
          WHERE e.deleted=0
            AND (e.recurrence_rule<>'' OR e.recurrence_id<>'')
            AND (e.recurrence_id<>'' OR (e.recurrence_rule<>'' AND (
              (e.organizer_json NOT IN ('','{}')
               AND e.attendees_json NOT IN ('','[]'))
              OR EXISTS (
                SELECT 1 FROM events AS invite_exception
                WHERE invite_exception.deleted=0
                  AND invite_exception.calendar_id=e.calendar_id
                  AND invite_exception.uid=e.uid
                  AND invite_exception.recurrence_id<>''
                  AND invite_exception.organizer_json NOT IN ('','{}')
                  AND invite_exception.attendees_json NOT IN ('','[]')
              )
            )))
        ) AS bounded_invitations
        ORDER BY all_day DESC, COALESCE(NULLIF(start_utc,''),start_date),id
      )SQL")));
      while (query.next()) {
        invitationPlan.append(query.value(3).toString());
      }
      reader.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    const QString joined = plan.join(QLatin1Char('\n'));
    QVERIFY2(joined.contains(QStringLiteral("events_timed_range_active_index")),
             qPrintable(joined));
    QVERIFY2(joined.contains(QStringLiteral("events_all_day_range_active_index")),
             qPrintable(joined));
    QVERIFY2(joined.contains(QStringLiteral("events_recurrence_active_index")),
             qPrintable(joined));
    for (const QString& step : plan) {
      const QString upper = step.toUpper();
      QVERIFY2(!(upper.startsWith(QStringLiteral("SCAN E")) &&
                 !upper.contains(QStringLiteral("USING INDEX"))),
               qPrintable(joined));
    }

    const QString invitationJoined = invitationPlan.join(QLatin1Char('\n'));
    QVERIFY2(
        invitationJoined.contains(QStringLiteral("events_timed_range_active_index")),
        qPrintable(invitationJoined));
    QVERIFY2(
        invitationJoined.contains(QStringLiteral("events_all_day_range_active_index")),
        qPrintable(invitationJoined));
    QVERIFY2(
        invitationJoined.count(QStringLiteral("events_recurrence_active_index")) >= 2,
        qPrintable(invitationJoined));
    for (const QString& step : invitationPlan) {
      const QString upper = step.toUpper();
      QVERIFY2(!(upper.startsWith(QStringLiteral("SCAN E")) &&
                 !upper.contains(QStringLiteral("USING INDEX"))),
               qPrintable(invitationJoined));
      QVERIFY2(!(upper.startsWith(QStringLiteral("SCAN INVITE_EXCEPTION")) &&
                 !upper.contains(QStringLiteral("USING INDEX"))),
               qPrintable(invitationJoined));
    }
  }
};

QTEST_MAIN(WidgetQueriesTest)
#include "test_widget_queries.moc"
