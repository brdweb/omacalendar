// Direct coverage of the Google Calendar event mapper: Events resource JSON to
// the canonical Event and back to a write body. Every address is synthetic.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QtTest/QtTest>

#include "core/domain.h"
#include "providers/google/googlemapper.h"

using namespace omacalendar;

namespace {

const QString kCalendarId = QStringLiteral("cal-mapper");

QDateTime utc(const int year, const int month, const int day, const int hour,
              const int minute = 0) {
  return QDateTime(QDate(year, month, day), QTime(hour, minute), QTimeZone::UTC);
}

QJsonObject timedResource() {
  return QJsonObject{
      {QStringLiteral("kind"), QStringLiteral("calendar#event")},
      {QStringLiteral("id"), QStringLiteral("evt-timed")},
      {QStringLiteral("iCalUID"), QStringLiteral("evt-timed@example.invalid")},
      {QStringLiteral("etag"), QStringLiteral("\"3300000000000000\"")},
      {QStringLiteral("htmlLink"),
       QStringLiteral("https://calendar.example.invalid/event?eid=1")},
      {QStringLiteral("created"), QStringLiteral("2026-09-01T08:00:00.000Z")},
      {QStringLiteral("updated"), QStringLiteral("2026-09-02T08:00:00.000Z")},
      {QStringLiteral("summary"), QStringLiteral("Planning")},
      {QStringLiteral("description"), QStringLiteral("Agenda in the doc")},
      {QStringLiteral("location"), QStringLiteral("Room 2")},
      {QStringLiteral("creator"),
       QJsonObject{{QStringLiteral("email"), QStringLiteral("owner@example.invalid")}}},
      {QStringLiteral("organizer"),
       QJsonObject{{QStringLiteral("email"), QStringLiteral("owner@example.invalid")},
                   {QStringLiteral("self"), true}}},
      {QStringLiteral("start"),
       QJsonObject{
           {QStringLiteral("dateTime"), QStringLiteral("2026-10-12T09:30:00+02:00")},
           {QStringLiteral("timeZone"), QStringLiteral("Europe/Berlin")}}},
      {QStringLiteral("end"),
       QJsonObject{
           {QStringLiteral("dateTime"), QStringLiteral("2026-10-12T10:15:00+02:00")},
           {QStringLiteral("timeZone"), QStringLiteral("Europe/Berlin")}}},
      {QStringLiteral("status"), QStringLiteral("confirmed")},
      {QStringLiteral("sequence"), 2},
      {QStringLiteral("reminders"), QJsonObject{{QStringLiteral("useDefault"), true}}},
  };
}

// Maps a resource, writes it back and maps the write body again with the
// response-only identity Google would add, as a sync round trip would.
Event roundTrip(const Event& event) {
  QJsonObject written = google::eventToGoogleJson(event);
  written.insert(QStringLiteral("id"), event.remoteId);
  written.insert(QStringLiteral("iCalUID"), event.uid);
  return google::eventFromGoogleJson(written, event.calendarId);
}

void compareCanonical(const Event& actual, const Event& expected) {
  QCOMPARE(actual.remoteId, expected.remoteId);
  QCOMPARE(actual.uid, expected.uid);
  QCOMPARE(actual.summary, expected.summary);
  QCOMPARE(actual.description, expected.description);
  QCOMPARE(actual.location, expected.location);
  QCOMPARE(actual.allDay, expected.allDay);
  QCOMPARE(actual.timeKind, expected.timeKind);
  QCOMPARE(actual.startUtc, expected.startUtc);
  QCOMPARE(actual.endUtc, expected.endUtc);
  QCOMPARE(actual.startDate, expected.startDate);
  QCOMPARE(actual.endDate, expected.endDate);
  QCOMPARE(actual.startTimeZone, expected.startTimeZone);
  QCOMPARE(actual.endTimeZone, expected.endTimeZone);
  QCOMPARE(actual.status, expected.status);
  QCOMPARE(actual.transparency, expected.transparency);
  QCOMPARE(actual.visibility, expected.visibility);
  QCOMPARE(actual.recurrenceRule, expected.recurrenceRule);
  QCOMPARE(actual.sequence, expected.sequence);
}

QJsonObject attendeeByEmail(const QJsonArray& attendees, const QString& email) {
  for (const QJsonValue& value : attendees) {
    if (value.toObject().value(QStringLiteral("email")).toString() == email) {
      return value.toObject();
    }
  }
  return {};
}

}  // namespace

class GoogleMapperTest final : public QObject {
  Q_OBJECT

 private slots:
  void timedEventKeepsItsZoneThroughARoundTrip() {
    const Event event = google::eventFromGoogleJson(timedResource(), kCalendarId);
    QCOMPARE(event.calendarId, kCalendarId);
    QCOMPARE(event.timeKind, TimeKind::Zoned);
    QVERIFY(!event.allDay);
    QCOMPARE(event.startUtc, utc(2026, 10, 12, 7, 30));
    QCOMPARE(event.endUtc, utc(2026, 10, 12, 8, 15));
    QCOMPARE(event.startTimeZone, QStringLiteral("Europe/Berlin"));
    QCOMPARE(event.endTimeZone, QStringLiteral("Europe/Berlin"));
    QCOMPARE(event.createdAt, utc(2026, 9, 1, 8));
    QCOMPARE(event.updatedAt, utc(2026, 9, 2, 8));

    // The write carries wall-clock time with the zone's own offset and the
    // IANA zone, not a bare UTC instant.
    const QJsonObject body = google::eventToGoogleJson(event);
    const QJsonObject start = body.value(QStringLiteral("start")).toObject();
    QCOMPARE(start.value(QStringLiteral("dateTime")).toString(),
             QStringLiteral("2026-10-12T09:30:00.000+02:00"));
    QCOMPARE(start.value(QStringLiteral("timeZone")).toString(),
             QStringLiteral("Europe/Berlin"));
    QVERIFY(!start.contains(QStringLiteral("date")));
    QCOMPARE(body.value(QStringLiteral("sequence")).toInt(), 2);

    compareCanonical(roundTrip(event), event);
  }

  void timedEventWithoutOffsetUsesItsZone() {
    QJsonObject resource = timedResource();
    // Google may send a local wall time plus a zone; the end zone defaults to
    // the start zone when omitted.
    resource.insert(
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("dateTime"), QStringLiteral("2027-01-05T09:00:00")},
                    {QStringLiteral("timeZone"), QStringLiteral("America/New_York")}});
    resource.insert(QStringLiteral("end"),
                    QJsonObject{{QStringLiteral("dateTime"),
                                 QStringLiteral("2027-01-05T10:00:00")}});
    const Event event = google::eventFromGoogleJson(resource, kCalendarId);
    QCOMPARE(event.timeKind, TimeKind::Zoned);
    QCOMPARE(event.startUtc, utc(2027, 1, 5, 14));
    QCOMPARE(event.endTimeZone, QStringLiteral("America/New_York"));
    QCOMPARE(event.endUtc, utc(2027, 1, 5, 15));
    compareCanonical(roundTrip(event), event);
  }

  void floatingEventStaysFloating() {
    QJsonObject resource = timedResource();
    resource.insert(QStringLiteral("start"),
                    QJsonObject{{QStringLiteral("dateTime"),
                                 QStringLiteral("2027-02-01T08:00:00")}});
    resource.insert(QStringLiteral("end"),
                    QJsonObject{{QStringLiteral("dateTime"),
                                 QStringLiteral("2027-02-01T08:30:00")}});
    const Event event = google::eventFromGoogleJson(resource, kCalendarId);
    QCOMPARE(event.timeKind, TimeKind::Floating);
    QVERIFY(event.startTimeZone.isEmpty());

    const QJsonObject start =
        google::eventToGoogleJson(event).value(QStringLiteral("start")).toObject();
    QCOMPARE(start.value(QStringLiteral("dateTime")).toString(),
             QStringLiteral("2027-02-01T08:00:00.000"));
    QVERIFY(!start.contains(QStringLiteral("timeZone")));
    compareCanonical(roundTrip(event), event);
  }

  void allDayEventKeepsDatesThroughARoundTrip() {
    QJsonObject resource = timedResource();
    resource.insert(
        QStringLiteral("start"),
        QJsonObject{{QStringLiteral("date"), QStringLiteral("2026-12-24")}});
    resource.insert(QStringLiteral("end"), QJsonObject{{QStringLiteral("date"),
                                                        QStringLiteral("2026-12-27")}});
    resource.insert(QStringLiteral("transparency"), QStringLiteral("transparent"));
    const Event event = google::eventFromGoogleJson(resource, kCalendarId);
    QVERIFY(event.allDay);
    QCOMPARE(event.timeKind, TimeKind::AllDay);
    QCOMPARE(event.startDate, QDate(2026, 12, 24));
    QCOMPARE(event.endDate, QDate(2026, 12, 27));
    QVERIFY(!event.startUtc.isValid());

    const QJsonObject body = google::eventToGoogleJson(event);
    QCOMPARE(body.value(QStringLiteral("start")).toObject(),
             QJsonObject({{QStringLiteral("date"), QStringLiteral("2026-12-24")}}));
    QCOMPARE(body.value(QStringLiteral("end")).toObject(),
             QJsonObject({{QStringLiteral("date"), QStringLiteral("2026-12-27")}}));
    QCOMPARE(body.value(QStringLiteral("transparency")).toString(),
             QStringLiteral("transparent"));
    compareCanonical(roundTrip(event), event);

    // A missing end is a one-day event on the way in and the way out.
    resource.remove(QStringLiteral("end"));
    const Event oneDay = google::eventFromGoogleJson(resource, kCalendarId);
    QCOMPARE(oneDay.endDate, QDate(2026, 12, 25));
    Event noEnd = oneDay;
    noEnd.endDate = {};
    QCOMPARE(google::eventToGoogleJson(noEnd)
                 .value(QStringLiteral("end"))
                 .toObject()
                 .value(QStringLiteral("date"))
                 .toString(),
             QStringLiteral("2026-12-25"));
  }

  void recurrenceLinesRoundTrip() {
    QJsonObject resource = timedResource();
    resource.insert(QStringLiteral("recurrence"),
                    QJsonArray{QStringLiteral("RRULE:FREQ=WEEKLY;BYDAY=MO;COUNT=10"),
                               QStringLiteral("EXDATE:20261019T073000Z"),
                               QStringLiteral("RDATE:20261021T073000Z")});
    const Event event = google::eventFromGoogleJson(resource, kCalendarId);
    QCOMPARE(event.recurrenceRule,
             QStringLiteral("RRULE:FREQ=WEEKLY;BYDAY=MO;COUNT=10\n"
                            "EXDATE:20261019T073000Z\nRDATE:20261021T073000Z"));
    QVERIFY(event.recurrenceId.isEmpty());

    const QJsonArray written =
        google::eventToGoogleJson(event).value(QStringLiteral("recurrence")).toArray();
    QCOMPARE(written, QJsonArray({QStringLiteral("RRULE:FREQ=WEEKLY;BYDAY=MO;COUNT=10"),
                                  QStringLiteral("EXDATE:20261019T073000Z"),
                                  QStringLiteral("RDATE:20261021T073000Z")}));
    compareCanonical(roundTrip(event), event);

    // A bare rule from a local edit gains its RRULE: name; lines that are not
    // recurrence properties never reach Google.
    Event local = event;
    local.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=2\nX-UNKNOWN:1");
    QCOMPARE(
        google::eventToGoogleJson(local).value(QStringLiteral("recurrence")).toArray(),
        QJsonArray({QStringLiteral("RRULE:FREQ=DAILY;COUNT=2")}));
  }

  void recurrenceLinesWithParametersSurviveAWrite() {
    // Google returns zoned and all-day exclusions with parameters. Updates are
    // PATCHes, so a recurrence array that loses these lines brings the
    // excluded occurrences back on Google.
    QJsonObject resource = timedResource();
    const QJsonArray recurrence{
        QStringLiteral("RRULE:FREQ=WEEKLY;BYDAY=MO;COUNT=10"),
        QStringLiteral("EXDATE;TZID=Europe/Berlin:20261019T093000"),
        QStringLiteral("RDATE;VALUE=DATE:20261021"),
    };
    resource.insert(QStringLiteral("recurrence"), recurrence);
    const Event event = google::eventFromGoogleJson(resource, kCalendarId);
    QCOMPARE(event.recurrenceRule,
             QStringLiteral("RRULE:FREQ=WEEKLY;BYDAY=MO;COUNT=10\n"
                            "EXDATE;TZID=Europe/Berlin:20261019T093000\n"
                            "RDATE;VALUE=DATE:20261021"));
    const QJsonArray written =
        google::eventToGoogleJson(event).value(QStringLiteral("recurrence")).toArray();
    QEXPECT_FAIL("",
                 "Bug: recurrenceLines() in googlemapper.cpp compares the text before "
                 "the first ':' to \"EXDATE:\"/\"RDATE:\", so lines with parameters "
                 "(;TZID=, ;VALUE=DATE) are dropped from the write body",
                 Continue);
    QCOMPARE(written, recurrence);
  }

  void occurrenceIdentityAndTargeting() {
    // A materialized instance of a timed series.
    QJsonObject instance = timedResource();
    instance.insert(QStringLiteral("id"), QStringLiteral("series_20261019T073000Z"));
    instance.insert(QStringLiteral("iCalUID"),
                    QStringLiteral("series@example.invalid"));
    instance.insert(QStringLiteral("recurringEventId"), QStringLiteral("series"));
    instance.insert(
        QStringLiteral("originalStartTime"),
        QJsonObject{
            {QStringLiteral("dateTime"), QStringLiteral("2026-10-19T09:30:00+02:00")},
            {QStringLiteral("timeZone"), QStringLiteral("Europe/Berlin")}});
    const Event occurrence = google::eventFromGoogleJson(instance, kCalendarId);
    QCOMPARE(occurrence.recurrenceId, QStringLiteral("2026-10-19T07:30:00.000Z"));
    QCOMPARE(occurrence.uid, QStringLiteral("series@example.invalid"));
    google::GoogleOccurrenceMutationTarget target =
        google::googleOccurrenceMutationTarget(occurrence);
    QCOMPARE(target.kind, google::GoogleOccurrenceMutationTargetKind::DirectInstance);
    QCOMPARE(target.remoteId, QStringLiteral("series_20261019T073000Z"));
    // The write body never names the series or the original start.
    const QJsonObject body = google::eventToGoogleJson(occurrence);
    QVERIFY(!body.contains(QStringLiteral("recurringEventId")));
    QVERIFY(!body.contains(QStringLiteral("originalStartTime")));

    // An all-day original start is a plain date identity.
    QCOMPARE(google::recurrenceInstanceId(
                 QJsonObject{{QStringLiteral("date"), QStringLiteral("2026-10-19")}}),
             QStringLiteral("2026-10-19"));
    // A wall time with a zone and no offset is resolved in that zone.
    QCOMPARE(google::recurrenceInstanceId(QJsonObject{
                 {QStringLiteral("dateTime"), QStringLiteral("2026-10-19T09:30:00")},
                 {QStringLiteral("timeZone"), QStringLiteral("Europe/Berlin")}}),
             occurrence.recurrenceId);

    // A sparse cancellation keeps the series identity and is a deletion.
    const QJsonObject tombstone{
        {QStringLiteral("id"), QStringLiteral("series_20261026T073000Z")},
        {QStringLiteral("recurringEventId"), QStringLiteral("series")},
        {QStringLiteral("status"), QStringLiteral("cancelled")},
        {QStringLiteral("originalStartTime"),
         QJsonObject{{QStringLiteral("dateTime"),
                      QStringLiteral("2026-10-26T08:30:00+01:00")}}},
    };
    const Event cancelled = google::eventFromGoogleJson(tombstone, kCalendarId);
    QVERIFY(cancelled.deleted);
    QCOMPARE(cancelled.uid, QStringLiteral("series"));
    QCOMPARE(cancelled.recurrenceId, QStringLiteral("2026-10-26T07:30:00.000Z"));
    QCOMPARE(google::eventToGoogleJson(cancelled).value(QStringLiteral("status")),
             QJsonValue(QStringLiteral("cancelled")));

    // A locally expanded occurrence resolves through the master id only.
    Event generated = occurrence;
    generated.remoteId = QStringLiteral("series#") + occurrence.recurrenceId;
    target = google::googleOccurrenceMutationTarget(generated);
    QCOMPARE(target.kind, google::GoogleOccurrenceMutationTargetKind::ResolveInstance);
    QCOMPARE(target.remoteId, QStringLiteral("series"));
  }

  void remindersOverrideVersusDefault() {
    QJsonObject resource = timedResource();
    const QJsonArray defaults{
        QJsonObject{{QStringLiteral("method"), QStringLiteral("popup")},
                    {QStringLiteral("minutes"), 10}},
        QJsonObject{{QStringLiteral("method"), QStringLiteral("email")},
                    {QStringLiteral("minutes"), 60}},
    };

    // useDefault: the calendar defaults are inherited and marked as such, and
    // an untouched event writes useDefault back without overrides.
    const Event inherited =
        google::eventFromGoogleJson(resource, kCalendarId, defaults);
    QCOMPARE(inherited.reminders.size(), 2);
    for (const QJsonValue& value : inherited.reminders) {
      QVERIFY(value.toObject().value(QStringLiteral("providerDefault")).toBool());
    }
    QCOMPARE(google::eventToGoogleJson(inherited).value(QStringLiteral("reminders")),
             QJsonValue(QJsonObject{{QStringLiteral("useDefault"), true}}));

    // Without defaults known locally, useDefault still survives.
    const Event noDefaults = google::eventFromGoogleJson(resource, kCalendarId);
    QVERIFY(noDefaults.reminders.isEmpty());
    QCOMPARE(google::eventToGoogleJson(noDefaults).value(QStringLiteral("reminders")),
             QJsonValue(QJsonObject{{QStringLiteral("useDefault"), true}}));

    // Overrides: invalid methods and out-of-range offsets are dropped, and
    // Google's limit of five overrides is kept.
    resource.insert(
        QStringLiteral("reminders"),
        QJsonObject{
            {QStringLiteral("useDefault"), false},
            {QStringLiteral("overrides"),
             QJsonArray{QJsonObject{{QStringLiteral("method"), QStringLiteral("popup")},
                                    {QStringLiteral("minutes"), 5}}}}});
    Event overridden = google::eventFromGoogleJson(resource, kCalendarId, defaults);
    QCOMPARE(overridden.reminders.size(), 1);
    QVERIFY(!overridden.reminders.first().toObject().contains(
        QStringLiteral("providerDefault")));
    QJsonArray many;
    many.append(QJsonObject{{QStringLiteral("method"), QStringLiteral("sms")},
                            {QStringLiteral("minutes"), 1}});
    many.append(QJsonObject{{QStringLiteral("method"), QStringLiteral("popup")},
                            {QStringLiteral("minutes"), 40321}});
    many.append(QJsonObject{{QStringLiteral("method"), QStringLiteral("popup")},
                            {QStringLiteral("minutes"), -1}});
    for (int minutes : {0, 5, 10, 15, 20, 25}) {
      many.append(QJsonObject{{QStringLiteral("method"), QStringLiteral("popup")},
                              {QStringLiteral("minutes"), minutes}});
    }
    overridden.reminders = many;
    QJsonObject reminders = google::eventToGoogleJson(overridden)
                                .value(QStringLiteral("reminders"))
                                .toObject();
    QCOMPARE(reminders.value(QStringLiteral("useDefault")).toBool(), false);
    const QJsonArray overrides = reminders.value(QStringLiteral("overrides")).toArray();
    QCOMPARE(overrides.size(), 5);
    QCOMPARE(overrides.first().toObject().value(QStringLiteral("minutes")).toInt(), 0);
    QCOMPARE(overrides.last().toObject().value(QStringLiteral("minutes")).toInt(), 20);

    // Clearing every override on an override event sends an empty override
    // list, not useDefault, so Google stops reminding.
    overridden.reminders = {};
    reminders = google::eventToGoogleJson(overridden)
                    .value(QStringLiteral("reminders"))
                    .toObject();
    QCOMPARE(reminders.value(QStringLiteral("useDefault")).toBool(true), false);
    QCOMPARE(reminders.value(QStringLiteral("overrides")).toArray(), QJsonArray{});

    // Editing one inherited reminder turns the set into explicit overrides
    // without the local providerDefault marker.
    Event edited = inherited;
    QJsonObject changed = edited.reminders.at(0).toObject();
    changed.remove(QStringLiteral("providerDefault"));
    changed.insert(QStringLiteral("minutes"), 30);
    edited.reminders.replace(0, changed);
    reminders =
        google::eventToGoogleJson(edited).value(QStringLiteral("reminders")).toObject();
    QCOMPARE(reminders.value(QStringLiteral("useDefault")).toBool(true), false);
    const QJsonArray explicitOverrides =
        reminders.value(QStringLiteral("overrides")).toArray();
    QCOMPARE(explicitOverrides.size(), 2);
    for (const QJsonValue& value : explicitOverrides) {
      QVERIFY(!value.toObject().contains(QStringLiteral("providerDefault")));
    }
  }

  void attendeesKeepOnlyWritableFields() {
    QJsonObject resource = timedResource();
    resource.insert(
        QStringLiteral("attendees"),
        QJsonArray{
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("attendee-id-1")},
                {QStringLiteral("email"), QStringLiteral("owner@example.invalid")},
                {QStringLiteral("organizer"), true},
                {QStringLiteral("self"), true},
                {QStringLiteral("responseStatus"), QStringLiteral("accepted")}},
            QJsonObject{
                {QStringLiteral("email"), QStringLiteral("guest@example.invalid")},
                {QStringLiteral("displayName"), QStringLiteral("Guest")},
                {QStringLiteral("optional"), true},
                {QStringLiteral("comment"), QStringLiteral("Maybe late")},
                {QStringLiteral("additionalGuests"), 1},
                {QStringLiteral("responseStatus"), QStringLiteral("tentative")}},
            QJsonObject{
                {QStringLiteral("email"),
                 QStringLiteral("room-4@resource.example.invalid")},
                {QStringLiteral("displayName"), QStringLiteral("Room 4")},
                {QStringLiteral("resource"), true},
                {QStringLiteral("responseStatus"), QStringLiteral("needsAction")}},
            // A guest without an address cannot be written back.
            QJsonObject{{QStringLiteral("displayName"), QStringLiteral("No address")}},
        });
    const Event event = google::eventFromGoogleJson(resource, kCalendarId);
    QCOMPARE(event.attendees.size(), 4);
    const QJsonObject room = attendeeByEmail(
        event.attendees, QStringLiteral("room-4@resource.example.invalid"));
    QCOMPARE(room.value(QStringLiteral("partstat")).toString(),
             QStringLiteral("NEEDSACTION"));
    QVERIFY(room.value(QStringLiteral("resource")).toBool());
    QVERIFY(attendeeByEmail(event.attendees, QStringLiteral("owner@example.invalid"))
                .value(QStringLiteral("self"))
                .toBool());

    const QJsonArray written =
        google::eventToGoogleJson(event).value(QStringLiteral("attendees")).toArray();
    QCOMPARE(written.size(), 3);
    QCOMPARE(
        attendeeByEmail(written, QStringLiteral("owner@example.invalid")),
        QJsonObject({{QStringLiteral("email"), QStringLiteral("owner@example.invalid")},
                     {QStringLiteral("responseStatus"), QStringLiteral("accepted")}}));
    QCOMPARE(
        attendeeByEmail(written, QStringLiteral("guest@example.invalid")),
        QJsonObject({{QStringLiteral("email"), QStringLiteral("guest@example.invalid")},
                     {QStringLiteral("displayName"), QStringLiteral("Guest")},
                     {QStringLiteral("optional"), true},
                     {QStringLiteral("comment"), QStringLiteral("Maybe late")},
                     {QStringLiteral("additionalGuests"), 1},
                     {QStringLiteral("responseStatus"), QStringLiteral("tentative")}}));
    const QJsonObject writtenRoom =
        attendeeByEmail(written, QStringLiteral("room-4@resource.example.invalid"));
    QVERIFY(writtenRoom.value(QStringLiteral("resource")).toBool());
    QVERIFY(!writtenRoom.contains(QStringLiteral("partstat")));
    // The organizer is response metadata and is never written.
    QVERIFY(!google::eventToGoogleJson(event).contains(QStringLiteral("organizer")));

    // An attendee carrying only an iCalendar participation status (a local
    // edit) gets the matching Google response.
    Event local = event;
    local.attendees = QJsonArray{
        QJsonObject{{QStringLiteral("email"), QStringLiteral("new@example.invalid")},
                    {QStringLiteral("partstat"), QStringLiteral("DECLINED")}}};
    QCOMPARE(google::eventToGoogleJson(local).value(QStringLiteral("attendees")),
             QJsonValue(QJsonArray{QJsonObject{
                 {QStringLiteral("email"), QStringLiteral("new@example.invalid")},
                 {QStringLiteral("responseStatus"), QStringLiteral("declined")}}}));
  }

  void conferenceDataIsKeptAndNeverInvented() {
    QJsonObject resource = timedResource();
    const QJsonObject conference{
        {QStringLiteral("conferenceId"), QStringLiteral("abc-defg-hij")},
        {QStringLiteral("conferenceSolution"),
         QJsonObject{
             {QStringLiteral("key"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("hangoutsMeet")}}}}},
        {QStringLiteral("entryPoints"),
         QJsonArray{
             QJsonObject{{QStringLiteral("entryPointType"), QStringLiteral("phone")},
                         {QStringLiteral("uri"), QStringLiteral("tel:+1-555-0100")}},
             QJsonObject{{QStringLiteral("entryPointType"), QStringLiteral("more")},
                         {QStringLiteral("uri"),
                          QStringLiteral("https://more.example.invalid/dial")}},
             QJsonObject{{QStringLiteral("entryPointType"), QStringLiteral("video")},
                         {QStringLiteral("uri"),
                          QStringLiteral("https://video.example.invalid/abc")}}}},
    };
    resource.insert(QStringLiteral("conferenceData"), conference);
    Event event = google::eventFromGoogleJson(resource, kCalendarId);
    // The video entry point wins over earlier non-video ones.
    QCOMPARE(event.conferenceUrl, QStringLiteral("https://video.example.invalid/abc"));

    // An update keeps the existing conference untouched, even when a new
    // conference was requested meanwhile.
    event.conferenceRequestId = QStringLiteral("request-1");
    QJsonObject body = google::eventToGoogleJson(event);
    QCOMPARE(body.value(QStringLiteral("conferenceData")).toObject(), conference);

    // A create never copies another event's conference; a request becomes a
    // createRequest for Meet.
    Event created = event;
    created.remoteId.clear();
    created.conferenceUrl.clear();
    created.rawPayload.clear();
    body = google::eventToGoogleJson(created);
    const QJsonObject request = body.value(QStringLiteral("conferenceData"))
                                    .toObject()
                                    .value(QStringLiteral("createRequest"))
                                    .toObject();
    QCOMPARE(request.value(QStringLiteral("requestId")).toString(),
             QStringLiteral("request-1"));
    QCOMPARE(request.value(QStringLiteral("conferenceSolutionKey"))
                 .toObject()
                 .value(QStringLiteral("type"))
                 .toString(),
             QStringLiteral("hangoutsMeet"));
    created.conferenceRequestId.clear();
    QVERIFY(
        !google::eventToGoogleJson(created).contains(QStringLiteral("conferenceData")));

    // Without conference data, a known meeting link in the location is used;
    // plain HTTP and unknown hosts are not.
    QJsonObject located = timedResource();
    located.insert(QStringLiteral("location"),
                   QStringLiteral("http://meet.google.com/insecure, "
                                  "https://unknown.example.invalid/x, "
                                  "https://meet.google.com/abc-defg-hij)."));
    QCOMPARE(google::eventFromGoogleJson(located, kCalendarId).conferenceUrl,
             QStringLiteral("https://meet.google.com/abc-defg-hij"));
  }

  void providerOnlyWriteFieldsArePreserved() {
    QJsonObject resource = timedResource();
    const QJsonObject extended{
        {QStringLiteral("private"),
         QJsonObject{{QStringLiteral("app-key"), QStringLiteral("app-value")}}},
        {QStringLiteral("shared"),
         QJsonObject{{QStringLiteral("team"), QStringLiteral("blue")}}},
    };
    const QJsonArray attachments{
        QJsonObject{{QStringLiteral("fileUrl"),
                     QStringLiteral("https://files.example.invalid/doc")},
                    {QStringLiteral("title"), QStringLiteral("Doc")}}};
    resource.insert(QStringLiteral("colorId"), QStringLiteral("11"));
    resource.insert(QStringLiteral("extendedProperties"), extended);
    resource.insert(QStringLiteral("attachments"), attachments);
    resource.insert(QStringLiteral("guestsCanModify"), true);
    resource.insert(QStringLiteral("guestsCanSeeOtherGuests"), false);
    resource.insert(QStringLiteral("anyoneCanAddSelf"), true);
    resource.insert(QStringLiteral("eventType"), QStringLiteral("default"));
    resource.insert(QStringLiteral("visibility"), QStringLiteral("confidential"));
    resource.insert(QStringLiteral("hangoutLink"),
                    QStringLiteral("https://meet.google.com/aaa-bbbb-ccc"));
    resource.insert(QStringLiteral("locked"), true);
    resource.insert(QStringLiteral("privateCopy"), false);
    resource.insert(QStringLiteral("recurringEventId"), QStringLiteral("not-written"));
    resource.insert(QStringLiteral("unknownFutureField"), QStringLiteral("ignored"));
    // An explicit null is not a value to preserve.
    resource.insert(QStringLiteral("workingLocationProperties"), QJsonValue::Null);

    Event event = google::eventFromGoogleJson(resource, kCalendarId);
    // A user edit to a canonical field must not lose the provider-only data.
    event.summary = QStringLiteral("Planning (moved)");
    event.startUtc = event.startUtc.addSecs(3600);
    event.endUtc = event.endUtc.addSecs(3600);
    const QJsonObject body = google::eventToGoogleJson(event);
    QCOMPARE(body.value(QStringLiteral("summary")).toString(),
             QStringLiteral("Planning (moved)"));
    QCOMPARE(body.value(QStringLiteral("colorId")).toString(), QStringLiteral("11"));
    QCOMPARE(body.value(QStringLiteral("extendedProperties")).toObject(), extended);
    QCOMPARE(body.value(QStringLiteral("attachments")).toArray(), attachments);
    QCOMPARE(body.value(QStringLiteral("guestsCanModify")), QJsonValue(true));
    QCOMPARE(body.value(QStringLiteral("guestsCanSeeOtherGuests")), QJsonValue(false));
    QCOMPARE(body.value(QStringLiteral("anyoneCanAddSelf")), QJsonValue(true));
    QCOMPARE(body.value(QStringLiteral("eventType")).toString(),
             QStringLiteral("default"));
    QCOMPARE(body.value(QStringLiteral("visibility")).toString(),
             QStringLiteral("confidential"));
    QVERIFY(!body.contains(QStringLiteral("workingLocationProperties")));

    // Response metadata and unknown fields are never written.
    for (const char* key :
         {"id", "iCalUID", "etag", "kind", "htmlLink", "created", "updated", "creator",
          "organizer", "hangoutLink", "locked", "privateCopy", "recurringEventId",
          "originalStartTime", "unknownFutureField"}) {
      QVERIFY2(!body.contains(QLatin1StringView(key)), key);
    }

    // A create body starts clean: nothing provider-only is copied from the
    // event it was made from, but the mutation identity is added.
    Event duplicate = event;
    duplicate.remoteId.clear();
    const QJsonObject create =
        google::eventToGoogleCreateJson(duplicate, QStringLiteral("mutation-1"));
    QVERIFY(!create.contains(QStringLiteral("colorId")));
    QVERIFY(!create.contains(QStringLiteral("attachments")));
    QCOMPARE(create.value(QStringLiteral("id")).toString(),
             google::eventIdForClientMutation(QStringLiteral("mutation-1")));
    const QJsonObject createExtended =
        create.value(QStringLiteral("extendedProperties")).toObject();
    QCOMPARE(createExtended.value(QStringLiteral("private")).toObject(),
             QJsonObject({{QStringLiteral("omacalendarMutationId"),
                           QStringLiteral("mutation-1")}}));
    QVERIFY(
        google::googleEventHasMutationIdentity(create, QStringLiteral("mutation-1")));
    QVERIFY(!google::googleEventHasMutationIdentity(create, QStringLiteral("other")));
  }

  void clearedFieldsAreSentExplicitly() {
    QJsonObject resource = timedResource();
    resource.insert(QStringLiteral("source"),
                    QJsonObject{{QStringLiteral("title"), QStringLiteral("Ticket")},
                                {QStringLiteral("url"),
                                 QStringLiteral("https://tracker.example.invalid/1")}});
    Event event = google::eventFromGoogleJson(resource, kCalendarId);
    QCOMPARE(event.url, QStringLiteral("https://tracker.example.invalid/1"));
    // An unchanged source is written back as Google sent it, title included.
    QCOMPARE(google::eventToGoogleJson(event).value(QStringLiteral("source")),
             resource.value(QStringLiteral("source")));

    event.description.clear();
    event.location.clear();
    event.url.clear();
    const QJsonObject body = google::eventToGoogleJson(event);
    QCOMPARE(body.value(QStringLiteral("description")), QJsonValue(QString()));
    QCOMPARE(body.value(QStringLiteral("location")), QJsonValue(QString()));
    QVERIFY(body.value(QStringLiteral("source")).isNull());

    // A changed link keeps the existing source title.
    event.url = QStringLiteral("https://tracker.example.invalid/2");
    QCOMPARE(
        google::eventToGoogleJson(event).value(QStringLiteral("source")),
        QJsonValue(QJsonObject{{QStringLiteral("url"),
                                QStringLiteral("https://tracker.example.invalid/2")},
                               {QStringLiteral("title"), QStringLiteral("Ticket")}}));
    // A non-web link is never sent as a source: Google's own copy stays.
    event.url = QStringLiteral("javascript:alert(1)");
    QCOMPARE(google::eventToGoogleJson(event).value(QStringLiteral("source")),
             resource.value(QStringLiteral("source")));
    Event fresh = event;
    fresh.remoteId.clear();
    QVERIFY(!google::eventToGoogleJson(fresh).contains(QStringLiteral("source")));
  }
};

QTEST_GUILESS_MAIN(GoogleMapperTest)

#include "test_googlemapper.moc"
