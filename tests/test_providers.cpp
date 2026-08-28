// Copyright (c) 2026

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUrlQuery>
#include <QtTest/QtTest>
#include <chrono>

#include "core/database.h"
#include "providers/caldav/caldavxml.h"
#include "providers/caldav/icalcodec.h"
#include "providers/google/googlemapper.h"
#include "providers/google/googleoauthconfig.h"
#include "providers/google/googlesync.h"
#include "sync/retrypolicy.h"

using namespace omacalendar;

namespace {

QDateTime utc(const int year, const int month, const int day, const int hour,
              const int minute = 0) {
  return QDateTime(QDate(year, month, day), QTime(hour, minute), QTimeZone::UTC);
}

}  // namespace

class ProviderTest final : public QObject {
  Q_OBJECT

 private slots:
  void googleCalendarMapping();
  void googleEventMappingAndWriteSanitization();
  void googleAllDayAndCancellationMapping();
  void googleOAuthLoopbackConfiguration();
  void calDavICalendarParsing();
  void calDavICalendarSerializationRoundTrip();
  void davDiscoveryAndCollectionParsing();
  void davResourceAndErrorParsing();
  void retryClassification();
  void retryBackoffAndRetryAfter();
};

void ProviderTest::googleOAuthLoopbackConfiguration() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  Database database;
  QString error;
  QVERIFY2(database.open(directory.filePath(QStringLiteral("calendar.db")), &error),
           qPrintable(error));

  google::GoogleSync sync(&database);
  QVERIFY2(sync.restoreAccounts(&error), qPrintable(error));
  QVERIFY(sync.isConfigured());
  QSignalSpy authorizationUrl(&sync, &google::GoogleSync::authorizationUrlReady);
  const QString accountId =
      sync.beginAuthorization(QStringLiteral("OAuth test"), &error);
  QVERIFY2(!accountId.isEmpty(), qPrintable(error));
  QCOMPARE(authorizationUrl.count(), 1);

  const QList<QVariant> arguments = authorizationUrl.takeFirst();
  QCOMPARE(arguments.at(0).toString(), accountId);
  const QUrl authorization = arguments.at(1).toUrl();
  QCOMPARE(authorization.scheme(), QStringLiteral("https"));
  QCOMPARE(authorization.host(), QStringLiteral("accounts.google.com"));

  const QUrlQuery query(authorization);
  QCOMPARE(query.queryItemValue(QStringLiteral("client_id")),
           google::defaultOAuthClientId());
  const QUrl redirect(query.queryItemValue(QStringLiteral("redirect_uri")));
  QCOMPARE(redirect.scheme(), QStringLiteral("http"));
  QCOMPARE(redirect.host(), QStringLiteral("127.0.0.1"));
  QVERIFY(redirect.port() > 0);
  QCOMPARE(redirect.path(), QStringLiteral("/oauth2callback"));
  QCOMPARE(query.queryItemValue(QStringLiteral("code_challenge_method")),
           QStringLiteral("S256"));
  QVERIFY(!query.queryItemValue(QStringLiteral("code_challenge")).isEmpty());
  const QString scopes = query.queryItemValue(QStringLiteral("scope"));
  QVERIFY(scopes.contains(
      QStringLiteral("https://www.googleapis.com/auth/calendar.events")));
  QVERIFY(scopes.contains(QStringLiteral(
      "https://www.googleapis.com/auth/calendar.calendarlist.readonly")));

  sync.cancelAuthorization(accountId);
  QVERIFY(database.account(accountId).id.isEmpty());
}

void ProviderTest::googleCalendarMapping() {
  const QJsonObject resource{
      {QStringLiteral("id"), QStringLiteral("primary@example.test")},
      {QStringLiteral("etag"), QStringLiteral("\"calendar-etag\"")},
      {QStringLiteral("summary"), QStringLiteral("Default")},
      {QStringLiteral("summaryOverride"), QStringLiteral("Work")},
      {QStringLiteral("description"), QStringLiteral("Work calendar")},
      {QStringLiteral("timeZone"), QStringLiteral("America/New_York")},
      {QStringLiteral("backgroundColor"), QStringLiteral("#336699")},
      {QStringLiteral("foregroundColor"), QStringLiteral("#ffffff")},
      {QStringLiteral("accessRole"), QStringLiteral("reader")},
      {QStringLiteral("selected"), true},
      {QStringLiteral("primary"), true},
  };

  const Calendar calendar =
      google::calendarFromGoogleJson(resource, QStringLiteral("account-1"));
  QCOMPARE(calendar.accountId, QStringLiteral("account-1"));
  QCOMPARE(calendar.remoteId, QStringLiteral("primary@example.test"));
  QCOMPARE(calendar.name, QStringLiteral("Work"));
  QCOMPARE(calendar.description, QStringLiteral("Work calendar"));
  QCOMPARE(calendar.color, QStringLiteral("#336699"));
  QCOMPARE(calendar.timeZone, QStringLiteral("America/New_York"));
  QVERIFY(calendar.readOnly);
  QVERIFY(calendar.enabled);
  QCOMPARE(calendar.capabilities.value(QStringLiteral("provider")).toString(),
           QStringLiteral("google"));
  QVERIFY(calendar.capabilities.value(QStringLiteral("primary")).toBool());

  QJsonObject hiddenResource = resource;
  hiddenResource.insert(QStringLiteral("hidden"), true);
  hiddenResource.insert(QStringLiteral("accessRole"), QStringLiteral("writer"));
  const Calendar hidden =
      google::calendarFromGoogleJson(hiddenResource, QStringLiteral("account-1"));
  QVERIFY(!hidden.readOnly);
  QVERIFY(!hidden.enabled);
}

void ProviderTest::googleEventMappingAndWriteSanitization() {
  const QJsonObject resource{
      {QStringLiteral("id"), QStringLiteral("google-event-1")},
      {QStringLiteral("iCalUID"), QStringLiteral("uid-1@example.test")},
      {QStringLiteral("etag"), QStringLiteral("\"event-etag\"")},
      {QStringLiteral("summary"), QStringLiteral("Roadmap review")},
      {QStringLiteral("description"), QStringLiteral("Review Q4 scope")},
      {QStringLiteral("location"), QStringLiteral("Room 4")},
      {QStringLiteral("start"),
       QJsonObject{
           {QStringLiteral("dateTime"), QStringLiteral("2026-08-28T09:30:00-04:00")},
           {QStringLiteral("timeZone"), QStringLiteral("America/New_York")}}},
      {QStringLiteral("end"),
       QJsonObject{
           {QStringLiteral("dateTime"), QStringLiteral("2026-08-28T10:45:00-04:00")},
           {QStringLiteral("timeZone"), QStringLiteral("America/New_York")}}},
      {QStringLiteral("status"), QStringLiteral("confirmed")},
      {QStringLiteral("transparency"), QStringLiteral("transparent")},
      {QStringLiteral("sequence"), 4},
      {QStringLiteral("recurrence"),
       QJsonArray{QStringLiteral("RRULE:FREQ=WEEKLY;COUNT=3"),
                  QStringLiteral("EXDATE:20260904T133000Z")}},
      {QStringLiteral("originalStartTime"),
       QJsonObject{
           {QStringLiteral("dateTime"), QStringLiteral("2026-08-28T09:30:00-04:00")},
           {QStringLiteral("timeZone"), QStringLiteral("America/New_York")}}},
      {QStringLiteral("organizer"),
       QJsonObject{{QStringLiteral("email"), QStringLiteral("owner@example.test")},
                   {QStringLiteral("self"), true}}},
      {QStringLiteral("attendees"),
       QJsonArray{
           QJsonObject{{QStringLiteral("email"), QStringLiteral("a@example.test")},
                       {QStringLiteral("displayName"), QStringLiteral("A")},
                       {QStringLiteral("responseStatus"), QStringLiteral("accepted")},
                       {QStringLiteral("self"), true},
                       {QStringLiteral("organizer"), true}}}},
      {QStringLiteral("reminders"),
       QJsonObject{
           {QStringLiteral("useDefault"), false},
           {QStringLiteral("overrides"),
            QJsonArray{QJsonObject{{QStringLiteral("method"), QStringLiteral("popup")},
                                   {QStringLiteral("minutes"), 15}}}}}},
      {QStringLiteral("conferenceData"),
       QJsonObject{
           {QStringLiteral("conferenceId"), QStringLiteral("safe-existing-id")}}},
      {QStringLiteral("htmlLink"),
       QStringLiteral("https://calendar.example.test/event")},
      {QStringLiteral("created"), QStringLiteral("2026-08-01T12:00:00Z")},
      {QStringLiteral("updated"), QStringLiteral("2026-08-02T12:00:00.500Z")},
  };

  Event event = google::eventFromGoogleJson(resource, QStringLiteral("cal-1"));
  QCOMPARE(event.calendarId, QStringLiteral("cal-1"));
  QCOMPARE(event.remoteId, QStringLiteral("google-event-1"));
  QCOMPARE(event.uid, QStringLiteral("uid-1@example.test"));
  QCOMPARE(event.startUtc, utc(2026, 8, 28, 13, 30));
  QCOMPARE(event.endUtc, utc(2026, 8, 28, 14, 45));
  QCOMPARE(event.startTimeZone, QStringLiteral("America/New_York"));
  QCOMPARE(event.recurrenceRule, QStringLiteral("RRULE:FREQ=WEEKLY;COUNT=3\n"
                                                "EXDATE:20260904T133000Z"));
  QCOMPARE(event.recurrenceId, QStringLiteral("2026-08-28T13:30:00.000Z"));
  QCOMPARE(event.rawFormat, QStringLiteral("google-json"));
  QCOMPARE(event.sequence, 4);

  const QJsonObject writable = google::eventToGoogleJson(event);
  QCOMPARE(writable.value(QStringLiteral("summary")).toString(), event.summary);
  QCOMPARE(writable.value(QStringLiteral("start"))
               .toObject()
               .value(QStringLiteral("timeZone"))
               .toString(),
           QStringLiteral("America/New_York"));
  QCOMPARE(writable.value(QStringLiteral("recurrence")).toArray().size(), 2);
  QVERIFY(writable.contains(QStringLiteral("conferenceData")));
  QVERIFY(!writable.contains(QStringLiteral("id")));
  QVERIFY(!writable.contains(QStringLiteral("etag")));
  QVERIFY(!writable.contains(QStringLiteral("htmlLink")));
  QVERIFY(!writable.contains(QStringLiteral("organizer")));

  const QJsonObject attendee =
      writable.value(QStringLiteral("attendees")).toArray().first().toObject();
  QCOMPARE(attendee.value(QStringLiteral("email")).toString(),
           QStringLiteral("a@example.test"));
  QVERIFY(!attendee.contains(QStringLiteral("self")));
  QVERIFY(!attendee.contains(QStringLiteral("organizer")));
  QCOMPARE(writable.value(QStringLiteral("reminders"))
               .toObject()
               .value(QStringLiteral("overrides"))
               .toArray()
               .first()
               .toObject()
               .value(QStringLiteral("minutes"))
               .toInt(),
           15);

  // New events must not copy a source event's conferencing entry points.
  event.remoteId.clear();
  QVERIFY(!google::eventToGoogleJson(event).contains(QStringLiteral("conferenceData")));
}

void ProviderTest::googleAllDayAndCancellationMapping() {
  const QJsonObject allDayResource{
      {QStringLiteral("id"), QStringLiteral("away")},
      {QStringLiteral("iCalUID"), QStringLiteral("away@example.test")},
      {QStringLiteral("summary"), QStringLiteral("Away")},
      {QStringLiteral("start"),
       QJsonObject{{QStringLiteral("date"), QStringLiteral("2026-09-10")}}},
      {QStringLiteral("end"),
       QJsonObject{{QStringLiteral("date"), QStringLiteral("2026-09-12")}}},
  };
  const Event allDay =
      google::eventFromGoogleJson(allDayResource, QStringLiteral("calendar"));
  QVERIFY(allDay.allDay);
  QCOMPARE(allDay.startDate, QDate(2026, 9, 10));
  QCOMPARE(allDay.endDate, QDate(2026, 9, 12));
  const QJsonObject allDayWrite = google::eventToGoogleJson(allDay);
  QCOMPARE(allDayWrite.value(QStringLiteral("end"))
               .toObject()
               .value(QStringLiteral("date"))
               .toString(),
           QStringLiteral("2026-09-12"));

  const QJsonObject tombstone{
      {QStringLiteral("id"), QStringLiteral("cancelled-instance")},
      {QStringLiteral("status"), QStringLiteral("cancelled")},
      {QStringLiteral("recurringEventId"), QStringLiteral("series-id")},
      {QStringLiteral("originalStartTime"),
       QJsonObject{{QStringLiteral("date"), QStringLiteral("2026-09-17")}}},
  };
  const Event cancelled =
      google::eventFromGoogleJson(tombstone, QStringLiteral("calendar"));
  QCOMPARE(cancelled.uid, QStringLiteral("series-id"));
  QCOMPARE(cancelled.recurrenceId, QStringLiteral("2026-09-17"));
  QVERIFY(cancelled.deleted);
}

void ProviderTest::calDavICalendarParsing() {
  const QByteArray payload =
      "BEGIN:VCALENDAR\r\n"
      "VERSION:2.0\r\n"
      "PRODID:-//OmaCalendar Tests//EN\r\n"
      "BEGIN:VEVENT\r\n"
      "UID:all-day@example.test\r\n"
      "DTSTART;VALUE=DATE:20260828\r\n"
      "DTEND;VALUE=DATE:20260830\r\n"
      "SUMMARY:Out\\, all day\r\n"
      "STATUS:TENTATIVE\r\n"
      "TRANSP:TRANSPARENT\r\n"
      "SEQUENCE:2\r\n"
      "END:VEVENT\r\n"
      "BEGIN:VEVENT\r\n"
      "UID:weekly@example.test\r\n"
      "DTSTART;TZID=America/New_York:20260828T090000\r\n"
      "DTEND;TZID=America/New_York:20260828T101500\r\n"
      "RRULE:FREQ=WEEKLY;COUNT=3\r\n"
      "SUMMARY:Roadmap review\r\n"
      "DESCRIPTION:Line one\\nLine two\r\n"
      "LOCATION:Room 4\r\n"
      "STATUS:CONFIRMED\r\n"
      "TRANSP:OPAQUE\r\n"
      "SEQUENCE:7\r\n"
      "END:VEVENT\r\n"
      "END:VCALENDAR\r\n";

  const caldav::ICalendarParseResult parsed = caldav::ICalendarCodec::parse(payload);
  QVERIFY2(parsed.ok(), qPrintable(parsed.error.message));
  QCOMPARE(parsed.events.size(), 2);

  const Event allDay = parsed.events.at(0);
  QVERIFY(allDay.allDay);
  QCOMPARE(allDay.uid, QStringLiteral("all-day@example.test"));
  QCOMPARE(allDay.startDate, QDate(2026, 8, 28));
  QCOMPARE(allDay.endDate, QDate(2026, 8, 30));
  QCOMPARE(allDay.summary, QStringLiteral("Out, all day"));
  QCOMPARE(allDay.status, QStringLiteral("tentative"));
  QCOMPARE(allDay.transparency, QStringLiteral("transparent"));
  QCOMPARE(allDay.sequence, 2);
  QCOMPARE(allDay.rawPayload, QString::fromUtf8(payload));
  QCOMPARE(allDay.rawFormat, QStringLiteral("text/calendar"));

  const Event timed = parsed.events.at(1);
  QVERIFY(!timed.allDay);
  QCOMPARE(timed.startUtc, utc(2026, 8, 28, 13));
  QCOMPARE(timed.endUtc, utc(2026, 8, 28, 14, 15));
  QCOMPARE(timed.startTimeZone, QStringLiteral("America/New_York"));
  QCOMPARE(timed.endTimeZone, QStringLiteral("America/New_York"));
  QCOMPARE(timed.recurrenceRule, QStringLiteral("FREQ=WEEKLY;COUNT=3"));
  QCOMPARE(timed.description, QStringLiteral("Line one\nLine two"));
  QCOMPARE(timed.location, QStringLiteral("Room 4"));
  QCOMPARE(timed.sequence, 7);
}

void ProviderTest::calDavICalendarSerializationRoundTrip() {
  Event input;
  input.uid = QStringLiteral("exception@example.test");
  input.summary = QStringLiteral("Review, plan; ship");
  input.description = QStringLiteral("First line\nSecond line");
  input.location = QStringLiteral("Desk \\ 2");
  input.startUtc = utc(2026, 9, 4, 13);
  input.endUtc = utc(2026, 9, 4, 14);
  input.startTimeZone = QStringLiteral("America/New_York");
  input.endTimeZone = QStringLiteral("America/New_York");
  input.status = QStringLiteral("confirmed");
  input.transparency = QStringLiteral("opaque");
  input.recurrenceRule = QStringLiteral("FREQ=WEEKLY;COUNT=4");
  input.recurrenceId = QStringLiteral("TZID=America/New_York:20260904T090000");
  input.sequence = 3;
  input.updatedAt = utc(2026, 8, 28, 12);

  const caldav::ICalendarSerializeResult encoded =
      caldav::ICalendarCodec::serialize(input);
  QVERIFY2(encoded.ok(), qPrintable(encoded.error.message));
  QVERIFY(encoded.payload.contains("BEGIN:VCALENDAR\r\n"));
  QVERIFY(encoded.payload.contains("BEGIN:VTIMEZONE\r\n"));
  QVERIFY(
      encoded.payload.contains("DTSTART;TZID=America/New_York:20260904T090000\r\n"));

  const caldav::ICalendarParseResult decoded =
      caldav::ICalendarCodec::parse(encoded.payload);
  QVERIFY2(decoded.ok(), qPrintable(decoded.error.message));
  QCOMPARE(decoded.events.size(), 1);
  const Event output = decoded.events.first();
  QCOMPARE(output.uid, input.uid);
  QCOMPARE(output.summary, input.summary);
  QCOMPARE(output.description, input.description);
  QCOMPARE(output.location, input.location);
  QCOMPARE(output.startUtc, input.startUtc);
  QCOMPARE(output.endUtc, input.endUtc);
  QCOMPARE(output.startTimeZone, input.startTimeZone);
  QCOMPARE(output.recurrenceRule, input.recurrenceRule);
  QCOMPARE(output.recurrenceId, input.recurrenceId);
  QCOMPARE(output.status, input.status);
  QCOMPARE(output.transparency, input.transparency);
  QCOMPARE(output.sequence, input.sequence);

  QVERIFY(!caldav::ICalendarCodec::parse({}).ok());
  Event invalid;
  QVERIFY(!caldav::ICalendarCodec::serialize(invalid).ok());
}

void ProviderTest::davDiscoveryAndCollectionParsing() {
  const QByteArray xml = R"XML(<?xml version="1.0" encoding="utf-8"?>
<x:multistatus xmlns:x="DAV:"
               xmlns:cal="urn:ietf:params:xml:ns:caldav"
               xmlns:apple="http://apple.com/ns/ical/"
               xmlns:server="http://calendarserver.org/ns/">
  <x:response>
    <x:href>/seed/</x:href>
    <x:propstat><x:prop>
      <x:current-user-principal><x:href>/principals/alice/</x:href></x:current-user-principal>
      <cal:calendar-home-set><x:href>/calendars/alice/</x:href></cal:calendar-home-set>
    </x:prop><x:status>HTTP/1.1 200 OK</x:status></x:propstat>
  </x:response>
  <x:response>
    <x:href>/calendars/alice/work/</x:href>
    <x:propstat><x:prop><x:owner/></x:prop><x:status>HTTP/1.1 404 Not Found</x:status></x:propstat>
    <x:propstat><x:prop>
      <x:resourcetype><x:collection/><cal:calendar/></x:resourcetype>
      <x:displayname>Work</x:displayname>
      <cal:calendar-description>Team schedule</cal:calendar-description>
      <apple:calendar-color>#336699FF</apple:calendar-color>
      <server:getctag>ctag-1</server:getctag>
      <x:sync-token>collection-token</x:sync-token>
      <x:current-user-privilege-set>
        <x:privilege><x:read/></x:privilege>
        <x:privilege><x:write-content/></x:privilege>
      </x:current-user-privilege-set>
    </x:prop><x:status>HTTP/1.1 200 OK</x:status></x:propstat>
  </x:response>
  <x:response>
    <x:href>/calendars/alice/holidays/</x:href>
    <x:propstat><x:prop>
      <x:resourcetype><x:collection/><cal:calendar/></x:resourcetype>
      <x:displayname>Holidays</x:displayname>
      <x:current-user-privilege-set><x:privilege><x:read/></x:privilege></x:current-user-privilege-set>
    </x:prop><x:status>HTTP/1.1 200 OK</x:status></x:propstat>
  </x:response>
</x:multistatus>)XML";

  const caldav::CalDavMultiStatusResult parsed =
      caldav::CalDavXml::parseMultiStatus(xml);
  QVERIFY2(parsed.ok(), qPrintable(parsed.error.message));
  QCOMPARE(caldav::CalDavXml::principalHref(parsed),
           QStringLiteral("/principals/alice/"));
  QCOMPARE(caldav::CalDavXml::calendarHomeSetHref(parsed),
           QStringLiteral("/calendars/alice/"));

  const QList<caldav::CalDavCollection> collections =
      caldav::CalDavXml::collections(parsed);
  QCOMPARE(collections.size(), 2);
  QCOMPARE(collections.at(0).href, QStringLiteral("/calendars/alice/work/"));
  QCOMPARE(collections.at(0).displayName, QStringLiteral("Work"));
  QCOMPARE(collections.at(0).description, QStringLiteral("Team schedule"));
  QCOMPARE(collections.at(0).color, QStringLiteral("#336699FF"));
  QCOMPARE(collections.at(0).ctag, QStringLiteral("ctag-1"));
  QCOMPARE(collections.at(0).syncToken, QStringLiteral("collection-token"));
  QVERIFY(!collections.at(0).readOnly);
  QVERIFY(collections.at(1).readOnly);
}

void ProviderTest::davResourceAndErrorParsing() {
  const QByteArray xml =
      R"XML(<dav:multistatus xmlns:dav="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav">
  <dav:response><dav:href>/cal/event.ics</dav:href><dav:propstat><dav:prop>
    <dav:getetag>"event-etag"</dav:getetag>
    <c:calendar-data><![CDATA[BEGIN:VCALENDAR
VERSION:2.0
END:VCALENDAR
]]></c:calendar-data>
  </dav:prop><dav:status>HTTP/1.1 200 OK</dav:status></dav:propstat></dav:response>
  <dav:response><dav:href>/cal/deleted.ics</dav:href><dav:status>HTTP/1.1 410 Gone</dav:status></dav:response>
  <dav:sync-token>sync-2</dav:sync-token>
</dav:multistatus>)XML";

  const caldav::CalDavMultiStatusResult parsed =
      caldav::CalDavXml::parseMultiStatus(xml);
  QVERIFY2(parsed.ok(), qPrintable(parsed.error.message));
  QCOMPARE(parsed.syncToken, QStringLiteral("sync-2"));
  const QList<caldav::CalDavResource> resources = caldav::CalDavXml::resources(parsed);
  QCOMPARE(resources.size(), 2);
  QCOMPARE(resources.at(0).etag, QStringLiteral("\"event-etag\""));
  QVERIFY(resources.at(0).calendarData.contains(QStringLiteral("BEGIN:VCALENDAR")));
  QVERIFY(!resources.at(0).deleted());
  QCOMPARE(resources.at(1).href, QStringLiteral("/cal/deleted.ics"));
  QVERIFY(resources.at(1).deleted());

  const QByteArray dtd = "<!DOCTYPE multistatus><d:multistatus xmlns:d='DAV:'/>";
  const caldav::CalDavMultiStatusResult rejected =
      caldav::CalDavXml::parseMultiStatus(dtd);
  QVERIFY(!rejected.ok());
  QCOMPARE(rejected.error.code, QStringLiteral("doctype_not_allowed"));
  QVERIFY(!caldav::CalDavXml::parseMultiStatus({}).ok());
}

void ProviderTest::retryClassification() {
  RetryInput input;
  input.error.kind = ProviderErrorKind::Unknown;
  input.error.httpStatus = 401;
  QCOMPARE(RetryPolicy::classify(input), RetryAction::Reauthenticate);

  input.error.httpStatus = 412;
  QCOMPARE(RetryPolicy::classify(input), RetryAction::ResolveConflict);

  input.error.kind = ProviderErrorKind::SyncTokenExpired;
  input.error.httpStatus = 0;
  QCOMPARE(RetryPolicy::classify(input), RetryAction::ResetSync);

  input.error.kind = ProviderErrorKind::Network;
  input.operationIsIdempotent = false;
  input.requestMayHaveReachedServer = true;
  QCOMPARE(RetryPolicy::classify(input), RetryAction::ReconcileBeforeRetry);

  input.operationIsIdempotent = true;
  QCOMPARE(RetryPolicy::classify(input), RetryAction::Retry);

  input.error.kind = ProviderErrorKind::Permission;
  QCOMPARE(RetryPolicy::classify(input), RetryAction::DoNotRetry);

  input.error.kind = ProviderErrorKind::Cancelled;
  QCOMPARE(RetryPolicy::classify(input), RetryAction::Cancelled);
}

void ProviderTest::retryBackoffAndRetryAfter() {
  RetryOptions options;
  options.maximumAttempts = 4;
  options.initialDelay = std::chrono::milliseconds(1000);
  options.maximumDelay = std::chrono::milliseconds(5000);
  options.maximumRetryAfter = std::chrono::milliseconds(8000);
  options.jitterPermille = 0;
  const RetryPolicy policy(options);

  QCOMPARE(policy.delayForAttempt(1, 99).count(), qint64{1000});
  QCOMPARE(policy.delayForAttempt(2, 99).count(), qint64{2000});
  QCOMPARE(policy.delayForAttempt(3, 99).count(), qint64{4000});
  QCOMPARE(policy.delayForAttempt(20, 99).count(), qint64{5000});

  RetryInput input;
  input.error.kind = ProviderErrorKind::RateLimited;
  input.error.httpStatus = 429;
  input.error.retryAfterMs = 7000;
  input.completedAttempts = 2;
  input.jitterKey = 123;
  RetryDecision decision = policy.evaluate(input);
  QCOMPARE(decision.action, RetryAction::Retry);
  QCOMPARE(decision.delay.count(), qint64{7000});
  QVERIFY(decision.shouldScheduleRetry());

  input.error.retryAfterMs = 20000;
  decision = policy.evaluate(input);
  QCOMPARE(decision.delay.count(), qint64{8000});

  input.completedAttempts = 4;
  decision = policy.evaluate(input);
  QCOMPARE(decision.action, RetryAction::AttemptsExhausted);
  QCOMPARE(decision.delay.count(), qint64{0});
  QVERIFY(!decision.shouldScheduleRetry());
}

QTEST_GUILESS_MAIN(ProviderTest)
#include "test_providers.moc"
