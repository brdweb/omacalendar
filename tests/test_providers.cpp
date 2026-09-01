// Copyright (c) 2026

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUrlQuery>
#include <QtTest/QtTest>
#include <algorithm>
#include <chrono>

#include "core/database.h"
#include "providers/caldav/caldavxml.h"
#include "providers/caldav/icalcodec.h"
#include "providers/google/googleclient.h"
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
  void googleOccurrenceMutationTargeting();
  void googleRequestConstructionAndHydrationBounds();
  void googleResponseClassification();
  void googleMutationIdentityAndRsvpPatch();
  void googleFullSyncReplacementIsAtomic();
  void googleOAuthLoopbackConfiguration();
  void calDavICalendarParsing();
  void calDavICalendarSerializationRoundTrip();
  void davDiscoveryAndCollectionParsing();
  void davResourceAndErrorParsing();
  void retryClassification();
  void retryBackoffAndRetryAfter();
};

void ProviderTest::googleFullSyncReplacementIsAtomic() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  Database database;
  QString error;
  QVERIFY2(database.open(directory.filePath(QStringLiteral("calendar.db")), &error),
           qPrintable(error));

  Account account;
  account.id = QStringLiteral("google-full-sync-account");
  account.provider = ProviderKind::Google;
  account.displayName = QStringLiteral("Google full sync");
  account.authStatus = QStringLiteral("connected");
  QVERIFY2(database.upsertAccount(account, &error), qPrintable(error));

  Calendar calendar;
  calendar.id = QStringLiteral("google-full-sync-calendar");
  calendar.accountId = account.id;
  calendar.remoteId = QStringLiteral("primary@example.test");
  calendar.name = QStringLiteral("Primary");
  calendar.enabled = true;
  calendar.syncToken = QStringLiteral("old-token");
  QVERIFY2(database.upsertCalendar(calendar, &error), qPrintable(error));

  const auto cachedEvent = [&calendar](const QString& remoteId,
                                       const QString& summary) {
    Event event;
    event.calendarId = calendar.id;
    event.remoteId = remoteId;
    event.uid = remoteId + QStringLiteral("@example.test");
    event.summary = summary;
    event.startUtc = utc(2026, 9, 1, 13);
    event.endUtc = utc(2026, 9, 1, 14);
    event.startDate = event.startUtc.date();
    event.endDate = event.endUtc.date();
    event.startTimeZone = QStringLiteral("UTC");
    event.endTimeZone = QStringLiteral("UTC");
    event.etag = QStringLiteral("old-") + remoteId;
    return event;
  };
  const Event retained =
      cachedEvent(QStringLiteral("retained"), QStringLiteral("Old retained value"));
  const Event stale =
      cachedEvent(QStringLiteral("stale"), QStringLiteral("Stale value"));
  QVERIFY2(database.applyRemoteEvent(retained, &error), qPrintable(error));
  QVERIFY2(database.applyRemoteEvent(stale, &error), qPrintable(error));

  const QJsonObject replacementResource{
      {QStringLiteral("id"), retained.remoteId},
      {QStringLiteral("iCalUID"), retained.uid},
      {QStringLiteral("etag"), QStringLiteral("new-retained")},
      {QStringLiteral("summary"), QStringLiteral("Replacement value")},
      {QStringLiteral("start"),
       QJsonObject{{QStringLiteral("dateTime"), QStringLiteral("2026-09-01T13:00:00Z")},
                   {QStringLiteral("timeZone"), QStringLiteral("UTC")}}},
      {QStringLiteral("end"),
       QJsonObject{{QStringLiteral("dateTime"), QStringLiteral("2026-09-01T14:00:00Z")},
                   {QStringLiteral("timeZone"), QStringLiteral("UTC")}}},
  };
  const Event replacement =
      google::eventFromGoogleJson(replacementResource, calendar.id);
  const QList<Event> covered = database.eventsBetween(
      utc(2026, 1, 1, 0), utc(2027, 1, 1, 0), {calendar.id}, &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  const QStringList pruned = google::googleFullSyncPruneCandidates(
      covered, QSet<QString>{replacement.remoteId});
  QCOMPARE(pruned, QStringList{stale.remoteId});

  Calendar completedCalendar = calendar;
  completedCalendar.syncToken = QStringLiteral("new-token");
  Event invalid = replacement;
  invalid.calendarId = QStringLiteral("another-calendar");
  const QDateTime coverageStart = utc(2026, 1, 1, 0);
  const QDateTime coverageEnd = utc(2027, 1, 1, 0);
  QVERIFY(!database.applyRemoteRangeSyncBatch(completedCalendar, {replacement, invalid},
                                              {}, pruned, coverageStart, coverageEnd,
                                              &error));
  QCOMPARE(database.calendar(calendar.id).syncToken, QStringLiteral("old-token"));
  QCOMPARE(database.eventByRemoteId(calendar.id, retained.remoteId).summary,
           QStringLiteral("Old retained value"));
  QVERIFY(!database.eventByRemoteId(calendar.id, stale.remoteId).id.isEmpty());
  error.clear();
  QVERIFY(
      !database.isSyncRangeCovered(calendar.id, coverageStart, coverageEnd, &error));
  QVERIFY2(error.isEmpty(), qPrintable(error));

  error.clear();
  QVERIFY2(
      database.applyRemoteRangeSyncBatch(completedCalendar, {replacement}, {}, pruned,
                                         coverageStart, coverageEnd, &error),
      qPrintable(error));
  QCOMPARE(database.calendar(calendar.id).syncToken, QStringLiteral("new-token"));
  QCOMPARE(database.eventByRemoteId(calendar.id, retained.remoteId).summary,
           QStringLiteral("Replacement value"));
  QVERIFY(database.eventByRemoteId(calendar.id, stale.remoteId).id.isEmpty());
  QVERIFY(database.isSyncRangeCovered(calendar.id, coverageStart, coverageEnd, &error));
}

void ProviderTest::googleOAuthLoopbackConfiguration() {
  const QByteArray previousClientId = qgetenv("OMACALENDAR_GOOGLE_CLIENT_ID");
  const QByteArray previousClientSecret = qgetenv("OMACALENDAR_GOOGLE_CLIENT_SECRET");
  qputenv("OMACALENDAR_GOOGLE_CLIENT_ID", "test-client.apps.example.invalid");
  qputenv("OMACALENDAR_GOOGLE_CLIENT_SECRET", "test-public-client-key");
  const auto restoreEnvironment = qScopeGuard([&]() {
    if (previousClientId.isNull()) {
      qunsetenv("OMACALENDAR_GOOGLE_CLIENT_ID");
    } else {
      qputenv("OMACALENDAR_GOOGLE_CLIENT_ID", previousClientId);
    }
    if (previousClientSecret.isNull()) {
      qunsetenv("OMACALENDAR_GOOGLE_CLIENT_SECRET");
    } else {
      qputenv("OMACALENDAR_GOOGLE_CLIENT_SECRET", previousClientSecret);
    }
  });
  QVERIFY(!google::defaultOAuthClientId().isEmpty());
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  Database database;
  QString error;
  QVERIFY2(database.open(directory.filePath(QStringLiteral("calendar.db")), &error),
           qPrintable(error));

  Account existing;
  existing.id = QStringLiteral("existing-google-account");
  existing.provider = ProviderKind::Google;
  existing.displayName = QStringLiteral("Existing Google");
  existing.authStatus = QStringLiteral("connected");
  QVERIFY2(database.upsertAccount(existing, &error), qPrintable(error));

  google::GoogleSync sync(&database);
  QVERIFY2(sync.restoreAccounts(&error), qPrintable(error));
  QVERIFY(sync.isConfigured());
  QCOMPARE(database.account(existing.id).authStatus,
           QStringLiteral("reauthorization_required"));
  QCOMPARE(database.setting(QStringLiteral("google.oauth.scopeVersion")).toInt(), 2);
  QSignalSpy authorizationUrl(&sync, &google::GoogleSync::authorizationUrlReady);

  const auto googleAccountCount = [&database]() {
    const QList<Account> accounts = database.accounts();
    return std::count_if(accounts.cbegin(), accounts.cend(),
                         [](const Account& account) {
                           return account.provider == ProviderKind::Google;
                         });
  };
  QCOMPARE(googleAccountCount(), 1);
  QVERIFY2(sync.reauthorizeAccount(existing.id, &error), qPrintable(error));
  QTRY_COMPARE_WITH_TIMEOUT(authorizationUrl.count(), 1, 2000);
  QCOMPARE(authorizationUrl.first().at(0).toString(), existing.id);
  QCOMPARE(googleAccountCount(), 1);
  QCOMPARE(database.account(existing.id).authStatus, QStringLiteral("authorizing"));
  sync.cancelAuthorization(existing.id);
  QCOMPARE(database.account(existing.id).id, existing.id);
  QCOMPARE(database.account(existing.id).authStatus,
           QStringLiteral("reauthorization_required"));
  QCOMPARE(googleAccountCount(), 1);
  authorizationUrl.clear();

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
  QVERIFY(!query.hasQueryItem(QStringLiteral("include_granted_scopes")));
  const QString scopes = query.queryItemValue(QStringLiteral("scope"));
  QVERIFY(scopes.contains(
      QStringLiteral("https://www.googleapis.com/auth/calendar.events")));
  QVERIFY(scopes.contains(
      QStringLiteral("https://www.googleapis.com/auth/calendar.calendars")));
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
  QVERIFY(!calendar.capabilities.value(QStringLiteral("canDeleteCalendar")).toBool());

  QJsonObject hiddenResource = resource;
  hiddenResource.insert(QStringLiteral("hidden"), true);
  hiddenResource.insert(QStringLiteral("accessRole"), QStringLiteral("writer"));
  const Calendar hidden =
      google::calendarFromGoogleJson(hiddenResource, QStringLiteral("account-1"));
  QVERIFY(!hidden.readOnly);
  QVERIFY(!hidden.enabled);

  QJsonObject ownedSecondary = resource;
  ownedSecondary.insert(QStringLiteral("accessRole"), QStringLiteral("owner"));
  ownedSecondary.insert(QStringLiteral("primary"), false);
  const Calendar deletable =
      google::calendarFromGoogleJson(ownedSecondary, QStringLiteral("account-1"));
  QVERIFY(deletable.capabilities.value(QStringLiteral("canDeleteCalendar")).toBool());
}

void ProviderTest::googleEventMappingAndWriteSanitization() {
  const QJsonObject resource{
      {QStringLiteral("id"), QStringLiteral("google-event-1")},
      {QStringLiteral("iCalUID"), QStringLiteral("uid-1@example.test")},
      {QStringLiteral("etag"), QStringLiteral("\"event-etag\"")},
      {QStringLiteral("summary"), QStringLiteral("Roadmap review")},
      {QStringLiteral("description"), QStringLiteral("Review Q4 scope")},
      {QStringLiteral("location"), QStringLiteral("Room 4")},
      {QStringLiteral("source"),
       QJsonObject{
           {QStringLiteral("title"), QStringLiteral("Project page")},
           {QStringLiteral("url"), QStringLiteral("http://example.test/project?q=1")}}},
      {QStringLiteral("hangoutLink"),
       QStringLiteral("https://meet.google.com/abc-defg-hij")},
      {QStringLiteral("visibility"), QStringLiteral("private")},
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
       QJsonObject{{QStringLiteral("conferenceId"), QStringLiteral("safe-existing-id")},
                   {QStringLiteral("entryPoints"),
                    QJsonArray{QJsonObject{
                        {QStringLiteral("entryPointType"), QStringLiteral("video")},
                        {QStringLiteral("uri"),
                         QStringLiteral("https://video.example.test/room")}}}}}},
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
  QCOMPARE(event.timeKind, TimeKind::Zoned);
  QCOMPARE(event.url, QStringLiteral("http://example.test/project?q=1"));
  QCOMPARE(event.conferenceUrl, QStringLiteral("https://meet.google.com/abc-defg-hij"));
  QCOMPARE(event.visibility, QStringLiteral("private"));
  QCOMPARE(event.recurrenceRule, QStringLiteral("RRULE:FREQ=WEEKLY;COUNT=3\n"
                                                "EXDATE:20260904T133000Z"));
  QCOMPARE(event.recurrenceId, QStringLiteral("2026-08-28T13:30:00.000Z"));
  QCOMPARE(event.rawFormat, QStringLiteral("google-json"));
  QCOMPARE(event.sequence, 4);
  QCOMPARE(event.organizer.value(QStringLiteral("email")).toString(),
           QStringLiteral("owner@example.test"));
  QCOMPARE(
      event.attendees.first().toObject().value(QStringLiteral("partstat")).toString(),
      QStringLiteral("ACCEPTED"));
  QCOMPARE(event.reminders.first().toObject().value(QStringLiteral("minutes")).toInt(),
           15);

  const QJsonObject writable = google::eventToGoogleJson(event);
  QCOMPARE(writable.value(QStringLiteral("summary")).toString(), event.summary);
  QCOMPARE(writable.value(QStringLiteral("start"))
               .toObject()
               .value(QStringLiteral("timeZone"))
               .toString(),
           QStringLiteral("America/New_York"));
  QCOMPARE(writable.value(QStringLiteral("recurrence")).toArray().size(), 2);
  QVERIFY(writable.contains(QStringLiteral("conferenceData")));
  QCOMPARE(writable.value(QStringLiteral("source"))
               .toObject()
               .value(QStringLiteral("url"))
               .toString(),
           event.url);
  QCOMPARE(writable.value(QStringLiteral("visibility")).toString(),
           QStringLiteral("private"));
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

  QJsonObject defaultReminderResource = resource;
  defaultReminderResource.insert(QStringLiteral("reminders"),
                                 QJsonObject{{QStringLiteral("useDefault"), true}});
  const QJsonArray calendarDefaults{
      QJsonObject{{QStringLiteral("method"), QStringLiteral("popup")},
                  {QStringLiteral("minutes"), 30}}};
  Event inherited = google::eventFromGoogleJson(
      defaultReminderResource, QStringLiteral("cal-1"), calendarDefaults);
  QCOMPARE(inherited.reminders.size(), 1);
  QVERIFY(inherited.reminders.first()
              .toObject()
              .value(QStringLiteral("providerDefault"))
              .toBool());
  const QJsonObject inheritedWrite = google::eventToGoogleJson(inherited);
  QCOMPARE(inheritedWrite.value(QStringLiteral("reminders"))
               .toObject()
               .value(QStringLiteral("useDefault"))
               .toBool(),
           true);
  QVERIFY(!inheritedWrite.value(QStringLiteral("reminders"))
               .toObject()
               .contains(QStringLiteral("overrides")));
  QJsonObject explicitReminder = inherited.reminders.first().toObject();
  explicitReminder.remove(QStringLiteral("providerDefault"));
  inherited.reminders = QJsonArray{explicitReminder};
  const QJsonObject explicitWrite = google::eventToGoogleJson(inherited);
  QCOMPARE(explicitWrite.value(QStringLiteral("reminders"))
               .toObject()
               .value(QStringLiteral("useDefault"))
               .toBool(),
           false);

  // New events must not copy a source event's conferencing entry points.
  event.remoteId.clear();
  const QJsonObject createBody = google::eventToGoogleJson(event);
  QVERIFY(!createBody.contains(QStringLiteral("conferenceData")));
  QCOMPARE(createBody.value(QStringLiteral("source"))
               .toObject()
               .value(QStringLiteral("url"))
               .toString(),
           event.url);
}

void ProviderTest::googleAllDayAndCancellationMapping() {
  const QJsonObject allDayResource{
      {QStringLiteral("id"), QStringLiteral("away")},
      {QStringLiteral("iCalUID"), QStringLiteral("away@example.test")},
      {QStringLiteral("summary"), QStringLiteral("Away")},
      {QStringLiteral("description"),
       QStringLiteral("Join https://acme.zoom.us/j/12345.")},
      {QStringLiteral("start"),
       QJsonObject{{QStringLiteral("date"), QStringLiteral("2026-09-10")}}},
      {QStringLiteral("end"),
       QJsonObject{{QStringLiteral("date"), QStringLiteral("2026-09-12")}}},
  };
  const Event allDay =
      google::eventFromGoogleJson(allDayResource, QStringLiteral("calendar"));
  QVERIFY(allDay.allDay);
  QCOMPARE(allDay.timeKind, TimeKind::AllDay);
  QCOMPARE(allDay.startDate, QDate(2026, 9, 10));
  QCOMPARE(allDay.endDate, QDate(2026, 9, 12));
  QCOMPARE(allDay.conferenceUrl, QStringLiteral("https://acme.zoom.us/j/12345"));
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

  const QJsonObject floatingResource{
      {QStringLiteral("id"), QStringLiteral("floating")},
      {QStringLiteral("start"), QJsonObject{{QStringLiteral("dateTime"),
                                             QStringLiteral("2026-09-20T09:15:00")}}},
      {QStringLiteral("end"), QJsonObject{{QStringLiteral("dateTime"),
                                           QStringLiteral("2026-09-20T10:00:00")}}},
  };
  const Event floating =
      google::eventFromGoogleJson(floatingResource, QStringLiteral("calendar"));
  QCOMPARE(floating.timeKind, TimeKind::Floating);
  const QJsonObject floatingWrite = google::eventToGoogleJson(floating);
  const QString floatingStart = floatingWrite.value(QStringLiteral("start"))
                                    .toObject()
                                    .value(QStringLiteral("dateTime"))
                                    .toString();
  QCOMPARE(floatingStart, QStringLiteral("2026-09-20T09:15:00.000"));
  QVERIFY(!floatingWrite.value(QStringLiteral("start"))
               .toObject()
               .contains(QStringLiteral("timeZone")));
}

void ProviderTest::googleOccurrenceMutationTargeting() {
  const QString recurrenceId = QStringLiteral("2026-09-17T13:00:00.000Z");

  Event synthetic;
  synthetic.recurrenceId = recurrenceId;
  synthetic.remoteId = QStringLiteral("series-id#") + recurrenceId;
  synthetic.rawFormat = QStringLiteral("google-json");
  synthetic.rawPayload = QString::fromUtf8(
      QJsonDocument(QJsonObject{{QStringLiteral("id"), QStringLiteral("series-id")},
                                {QStringLiteral("recurrence"),
                                 QJsonArray{QStringLiteral("RRULE:FREQ=WEEKLY")}}})
          .toJson(QJsonDocument::Compact));
  google::GoogleOccurrenceMutationTarget target =
      google::googleOccurrenceMutationTarget(synthetic);
  QCOMPARE(target.kind, google::GoogleOccurrenceMutationTargetKind::ResolveInstance);
  QCOMPARE(target.remoteId, QStringLiteral("series-id"));

  // Update, delete, RSVP, and move-source dispatch all consume this same
  // operation-independent target.
  for (const OutboxOperation operation :
       {OutboxOperation::Update, OutboxOperation::Remove, OutboxOperation::Move}) {
    Q_UNUSED(operation);
    target = google::googleOccurrenceMutationTarget(synthetic);
    QCOMPARE(target.kind, google::GoogleOccurrenceMutationTargetKind::ResolveInstance);
    QCOMPARE(target.remoteId, QStringLiteral("series-id"));
  }

  Event generated = synthetic;
  generated.remoteId = QStringLiteral("series-id");
  target = google::googleOccurrenceMutationTarget(generated);
  QCOMPARE(target.kind, google::GoogleOccurrenceMutationTargetKind::ResolveInstance);
  QCOMPARE(target.remoteId, QStringLiteral("series-id"));

  Event materialized = synthetic;
  materialized.remoteId = QStringLiteral("materialized-instance-id");
  materialized.rawPayload = QString::fromUtf8(
      QJsonDocument(
          QJsonObject{
              {QStringLiteral("id"), QStringLiteral("materialized-instance-id")},
              {QStringLiteral("recurringEventId"), QStringLiteral("series-id")},
              {QStringLiteral("originalStartTime"),
               QJsonObject{{QStringLiteral("dateTime"), recurrenceId}}}})
          .toJson(QJsonDocument::Compact));
  target = google::googleOccurrenceMutationTarget(materialized);
  QCOMPARE(target.kind, google::GoogleOccurrenceMutationTargetKind::DirectInstance);
  QCOMPARE(target.remoteId, QStringLiteral("materialized-instance-id"));

  Event parentOnly = materialized;
  parentOnly.remoteId = QStringLiteral("series-id");
  target = google::googleOccurrenceMutationTarget(parentOnly);
  QCOMPARE(target.kind, google::GoogleOccurrenceMutationTargetKind::ResolveInstance);
  QCOMPARE(target.remoteId, QStringLiteral("series-id"));

  Event invalid = synthetic;
  invalid.recurrenceId.clear();
  target = google::googleOccurrenceMutationTarget(invalid);
  QCOMPARE(target.kind, google::GoogleOccurrenceMutationTargetKind::Invalid);
  QVERIFY(target.remoteId.isEmpty());
}

void ProviderTest::googleRequestConstructionAndHydrationBounds() {
  const QUrl calendarList = google::calendarListRequestUrl(
      QStringLiteral("page token"), QStringLiteral("sync/token"));
  QCOMPARE(calendarList.scheme(), QStringLiteral("https"));
  const QUrlQuery calendarQuery(calendarList);
  QCOMPARE(calendarQuery.queryItemValue(QStringLiteral("pageToken")),
           QStringLiteral("page token"));
  QCOMPARE(calendarQuery.queryItemValue(QStringLiteral("syncToken")),
           QStringLiteral("sync/token"));
  QCOMPARE(calendarQuery.queryItemValue(QStringLiteral("showDeleted")),
           QStringLiteral("true"));
  QCOMPARE(calendarQuery.queryItemValue(QStringLiteral("showHidden")),
           QStringLiteral("true"));
  QVERIFY(google::calendarDeleteRequestUrl(QStringLiteral("owner/test@example.test"))
              .toEncoded()
              .contains("calendars/owner%2Ftest%40example.test"));

  const QDateTime lower = utc(2024, 8, 28, 12);
  const QDateTime upper = utc(2031, 8, 28, 12);
  const QUrl initial = google::eventsListRequestUrl(
      QStringLiteral("team/a@example.test"), QStringLiteral("page"), {}, lower, upper);
  QVERIFY(initial.toEncoded().contains("team%2Fa%40example.test"));
  const QUrlQuery initialQuery(initial);
  QCOMPARE(initialQuery.queryItemValue(QStringLiteral("pageToken")),
           QStringLiteral("page"));
  QVERIFY(!initialQuery.queryItemValue(QStringLiteral("timeMin")).isEmpty());
  QVERIFY(!initialQuery.queryItemValue(QStringLiteral("timeMax")).isEmpty());
  QCOMPARE(initialQuery.queryItemValue(QStringLiteral("singleEvents")),
           QStringLiteral("false"));

  const QUrl incremental = google::eventsListRequestUrl(
      QStringLiteral("calendar"), {}, QStringLiteral("next-sync"), lower, upper);
  const QUrlQuery incrementalQuery(incremental);
  QCOMPARE(incrementalQuery.queryItemValue(QStringLiteral("syncToken")),
           QStringLiteral("next-sync"));
  QVERIFY(!incrementalQuery.hasQueryItem(QStringLiteral("timeMin")));
  QVERIFY(!incrementalQuery.hasQueryItem(QStringLiteral("timeMax")));

  const QUrl instances = google::eventInstancesRequestUrl(
      QStringLiteral("calendar"), QStringLiteral("series/id"), {}, lower, upper);
  QVERIFY(instances.toEncoded().contains("series%2Fid/instances"));
  const QUrlQuery instancesQuery(instances);
  QVERIFY(instancesQuery.hasQueryItem(QStringLiteral("timeMin")));
  QVERIFY(instancesQuery.hasQueryItem(QStringLiteral("timeMax")));

  const QUrl move = google::eventMoveRequestUrl(
      QStringLiteral("source/team@example.test"), QStringLiteral("event/id"),
      QStringLiteral("destination/team@example.test"), QStringLiteral("externalOnly"));
  QVERIFY(
      move.toEncoded().contains("source%2Fteam%40example.test/events/event%2Fid/move"));
  const QUrlQuery moveQuery(move);
  QCOMPARE(moveQuery.queryItemValue(QStringLiteral("destination")),
           QStringLiteral("destination/team@example.test"));
  QCOMPARE(moveQuery.queryItemValue(QStringLiteral("sendUpdates")),
           QStringLiteral("externalOnly"));

  QVERIFY(google::isValidGuestNotificationPolicy(QStringLiteral("none")));
  QVERIFY(google::isValidGuestNotificationPolicy(QStringLiteral("all")));
  QVERIFY(google::isValidGuestNotificationPolicy(QStringLiteral("externalOnly")));
  QVERIFY(!google::isValidGuestNotificationPolicy({}));
  QVERIFY(!google::isValidGuestNotificationPolicy(QStringLiteral("true")));
}

void ProviderTest::googleResponseClassification() {
  const auto errorBody = [](const QString& reason, const QString& message) {
    return QJsonDocument(
               QJsonObject{{QStringLiteral("error"),
                            QJsonObject{{QStringLiteral("message"), message},
                                        {QStringLiteral("errors"),
                                         QJsonArray{QJsonObject{
                                             {QStringLiteral("reason"), reason}}}}}}})
        .toJson(QJsonDocument::Compact);
  };
  const QDateTime received = utc(2026, 8, 28, 16);

  google::ApiResponse response = google::parseGoogleApiResponse(
      401, QNetworkReply::AuthenticationRequiredError,
      errorBody(QStringLiteral("authError"), QStringLiteral("expired")), {}, {},
      received);
  QVERIFY(response.authenticationRequired);
  QVERIFY(!response.retryable);
  QVERIFY(!response.insufficientScope);

  response = google::parseGoogleApiResponse(
      403, QNetworkReply::ContentAccessDenied,
      errorBody(QStringLiteral("insufficientPermissions"),
                QStringLiteral("Request had insufficient authentication scopes.")),
      {}, {}, received);
  QVERIFY(response.insufficientScope);
  QVERIFY(!response.authenticationRequired);
  QVERIFY(!response.retryable);

  response = google::parseGoogleApiResponse(
      403, QNetworkReply::ContentAccessDenied,
      errorBody(QStringLiteral("forbidden"), QStringLiteral("denied")), {}, {},
      received);
  QVERIFY(!response.retryable);

  response = google::parseGoogleApiResponse(
      403, QNetworkReply::ContentAccessDenied,
      errorBody(QStringLiteral("rateLimitExceeded"), QStringLiteral("slow")), {},
      QByteArrayLiteral("7"), received);
  QVERIFY(response.retryable);
  QCOMPARE(response.retryAfterMs, qint64{7000});

  response = google::parseGoogleApiResponse(
      429, QNetworkReply::UnknownContentError,
      errorBody(QStringLiteral("rateLimitExceeded"), QStringLiteral("later")), {},
      QByteArrayLiteral("3"), received);
  QVERIFY(response.retryable);
  QCOMPARE(response.retryAfterMs, qint64{3000});

  response = google::parseGoogleApiResponse(
      404, QNetworkReply::ContentNotFoundError,
      errorBody(QStringLiteral("notFound"), QStringLiteral("gone")), {}, {}, received);
  QVERIFY(response.notFound);
  QVERIFY(!response.conflict);

  response = google::parseGoogleApiResponse(
      409, QNetworkReply::UnknownContentError,
      errorBody(QStringLiteral("duplicate"), QStringLiteral("exists")), {}, {},
      received);
  QVERIFY(response.conflict);

  response = google::parseGoogleApiResponse(
      412, QNetworkReply::UnknownContentError,
      errorBody(QStringLiteral("conditionNotMet"), QStringLiteral("changed")), {}, {},
      received);
  QVERIFY(response.conflict);

  response = google::parseGoogleApiResponse(
      410, QNetworkReply::UnknownContentError,
      errorBody(QStringLiteral("fullSyncRequired"), QStringLiteral("reset")), {}, {},
      received);
  QVERIFY(response.syncTokenExpired);

  response = google::parseGoogleApiResponse(
      503, QNetworkReply::ServiceUnavailableError,
      errorBody(QStringLiteral("backendError"), QStringLiteral("retry")), {},
      QByteArrayLiteral("Fri, 28 Aug 2026 16:00:10 GMT"), received);
  QVERIFY(response.retryable);
  QCOMPARE(response.retryAfterMs, qint64{10000});

  response = google::parseGoogleApiResponse(0, QNetworkReply::TimeoutError, {}, {}, {},
                                            received);
  QVERIFY(response.retryable);
  QCOMPARE(response.networkError, static_cast<int>(QNetworkReply::TimeoutError));

  response = google::parseGoogleApiResponse(200, QNetworkReply::NoError,
                                            QByteArrayLiteral("[]"), {}, {}, received);
  QVERIFY(!response.ok);
  QCOMPARE(response.errorCode, QStringLiteral("malformed_response"));
  QVERIFY(response.retryable);

  response = google::parseGoogleApiResponse(204, QNetworkReply::NoError, {}, {}, {},
                                            received, true);
  QVERIFY(response.ok);

  response = google::parseGoogleApiResponse(
      400, QNetworkReply::ProtocolInvalidOperationError,
      errorBody(QStringLiteral("badRequest"),
                QStringLiteral("oops access_") +
                    QStringLiteral("token=secret Authorization: Bearer "
                                   "also-secret\nnext")),
      {}, {}, received);
  QVERIFY(!response.errorMessage.contains(QStringLiteral("secret")));
  QVERIFY(!response.errorMessage.contains(QLatin1Char('\n')));
}

void ProviderTest::googleMutationIdentityAndRsvpPatch() {
  const QString mutationId = QStringLiteral("client-mutation-123");
  const QString remoteId = google::eventIdForClientMutation(mutationId);
  QCOMPARE(remoteId.size(), 32);
  QVERIFY(
      QRegularExpression(QStringLiteral("^[0-9a-f]{32}$")).match(remoteId).hasMatch());
  QCOMPARE(remoteId, google::eventIdForClientMutation(mutationId));
  QVERIFY(remoteId !=
          google::eventIdForClientMutation(QStringLiteral("another-mutation")));

  Event create;
  create.summary = QStringLiteral("Stable create");
  create.startUtc = utc(2026, 8, 28, 13);
  create.endUtc = utc(2026, 8, 28, 14);
  const QJsonObject createBody = google::eventToGoogleCreateJson(create, mutationId);
  QCOMPARE(createBody.value(QStringLiteral("id")).toString(), remoteId);
  QVERIFY(google::googleEventHasMutationIdentity(createBody, mutationId));
  QVERIFY(
      !google::googleEventHasMutationIdentity(createBody, QStringLiteral("different")));

  const QJsonObject resource{
      {QStringLiteral("id"), QStringLiteral("invitation")},
      {QStringLiteral("summary"), QStringLiteral("Review")},
      {QStringLiteral("start"), QJsonObject{{QStringLiteral("dateTime"),
                                             QStringLiteral("2026-08-28T13:00:00Z")}}},
      {QStringLiteral("end"), QJsonObject{{QStringLiteral("dateTime"),
                                           QStringLiteral("2026-08-28T14:00:00Z")}}},
      {QStringLiteral("attendees"),
       QJsonArray{
           QJsonObject{
               {QStringLiteral("email"), QStringLiteral("me@example.test")},
               {QStringLiteral("self"), true},
               {QStringLiteral("responseStatus"), QStringLiteral("needsAction")}},
           QJsonObject{
               {QStringLiteral("email"), QStringLiteral("other@example.test")},
               {QStringLiteral("responseStatus"), QStringLiteral("accepted")}}}},
  };
  Event invitation = google::eventFromGoogleJson(resource, QStringLiteral("calendar"));
  QJsonObject me = invitation.attendees.first().toObject();
  me.insert(QStringLiteral("responseStatus"), QStringLiteral("accepted"));
  me.insert(QStringLiteral("partstat"), QStringLiteral("ACCEPTED"));
  invitation.attendees.replace(0, me);
  const QJsonObject rsvp = google::rsvpPatchForGoogleEvent(invitation);
  QVERIFY(!rsvp.isEmpty());
  QVERIFY(rsvp.value(QStringLiteral("attendeesOmitted")).toBool());
  QCOMPARE(rsvp.value(QStringLiteral("attendees"))
               .toArray()
               .first()
               .toObject()
               .value(QStringLiteral("responseStatus"))
               .toString(),
           QStringLiteral("accepted"));

  invitation.summary = QStringLiteral("Changed locally too");
  QVERIFY(google::rsvpPatchForGoogleEvent(invitation).isEmpty());
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
