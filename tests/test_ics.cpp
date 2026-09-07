#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "core/database.h"
#include "providers/caldav/icalcodec.h"
#include "providers/ics/icsservice.h"

using namespace omacalendar;

namespace {

QByteArray twoEventCalendar(const QString& firstTitle = QStringLiteral("Planning")) {
  return QStringLiteral(
             "BEGIN:VCALENDAR\r\n"
             "VERSION:2.0\r\n"
             "PRODID:-//OmaCalendar Test//EN\r\n"
             "BEGIN:VEVENT\r\n"
             "UID:one@example.test\r\n"
             "DTSTAMP:20260828T120000Z\r\n"
             "DTSTART:20260901T130000Z\r\n"
             "DTEND:20260901T140000Z\r\n"
             "SUMMARY:%1\r\n"
             "LOCATION:Studio\r\n"
             "END:VEVENT\r\n"
             "BEGIN:VEVENT\r\n"
             "UID:two@example.test\r\n"
             "DTSTAMP:20260828T120000Z\r\n"
             "DTSTART;VALUE=DATE:20260903\r\n"
             "DTEND;VALUE=DATE:20260905\r\n"
             "SUMMARY:Conference\r\n"
             "END:VEVENT\r\n"
             "END:VCALENDAR\r\n")
      .arg(firstTitle)
      .toUtf8();
}

}  // namespace

class IcsServiceTest final : public QObject {
  Q_OBJECT

 private slots:
  void validatesSubscriptionUrls();
  void previewsAndCommitsDuplicatePolicies();
  void exportsOneEventAndLocalCalendar();
  void keepsSubscriptionMetadataPrivate();
  void rejectsMalformedPayloads();
  void credentialStorageDoesNotBlockEventLoop();
  void rejectsCrossOriginRedirects();
  void boundsNetworkPayloadBeforeAppend();
  void preservesCompleteRecurrenceAcrossFeedRefreshAndImport();
};

void IcsServiceTest::preservesCompleteRecurrenceAcrossFeedRefreshAndImport() {
  const QByteArray payload = QByteArrayLiteral(
      "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\n"
      "UID:recurrence-set@example.test\r\n"
      "DTSTART:20260901T130000Z\r\nDTEND:20260901T140000Z\r\n"
      "RRULE:FREQ=DAILY;COUNT=4\r\nEXDATE:20260902T130000Z\r\n"
      "EXRULE:FREQ=DAILY;INTERVAL=2;COUNT=2\r\n"
      "RDATE:20260908T130000Z,\r\n 20260909T130000Z\r\n"
      "SUMMARY:Recurrence fixture\r\nEND:VEVENT\r\n"
      "BEGIN:VEVENT\r\nUID:rdate-only@example.test\r\n"
      "DTSTART;VALUE=DATE:20260905\r\nDTEND;VALUE=DATE:20260906\r\n"
      "RDATE;VALUE=DATE:20260910\r\nSUMMARY:RDATE fixture\r\n"
      "END:VEVENT\r\nEND:VCALENDAR\r\n");
  QTemporaryDir directory;
  const QString path = directory.filePath(QStringLiteral("calendar.sqlite"));
  const auto verify = [](Database& database, const QString& calendarId) {
    QString error;
    const auto events = database.eventsBetween(
        QDateTime::fromString(QStringLiteral("2026-09-01T00:00:00Z"), Qt::ISODate),
        QDateTime::fromString(QStringLiteral("2026-09-12T00:00:00Z"), Qt::ISODate),
        {calendarId}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QList<int> dates;
    for (const auto& event : events) {
      dates.append(event.allDay ? event.startDate.day() : event.startUtc.date().day());
    }
    std::sort(dates.begin(), dates.end());
    QCOMPARE(dates, QList<int>({4, 5, 8, 9, 10}));
  };
  {
    Database database;
    QString error;
    QVERIFY2(database.open(path, &error), qPrintable(error));
    Account account;
    account.id = QStringLiteral("recurrence-feed");
    account.provider = ProviderKind::Ics;
    QVERIFY(database.upsertAccount(account, &error));
    Calendar calendar;
    calendar.id = QStringLiteral("recurrence-feed-calendar");
    calendar.accountId = account.id;
    calendar.name = QStringLiteral("Recurrence fixture");
    calendar.readOnly = true;
    QVERIFY2(database.upsertCalendar(calendar, &error), qPrintable(error));
    IcsSubscription subscription;
    subscription.accountId = account.id;
    subscription.url = QStringLiteral("https://calendar.example.test/fixture.ics");
    QVERIFY(database.upsertIcsSubscription(subscription, &error));
    ics::IcsService service(&database);
    QVERIFY2(
        service.applyFeed(subscription, payload, QStringLiteral("one"), {}, &error),
        qPrintable(error));
    verify(database, calendar.id);
    QVERIFY2(
        service.applyFeed(subscription, payload, QStringLiteral("two"), {}, &error),
        qPrintable(error));
    verify(database, calendar.id);
    ics::IcsError operationError;
    const auto imported = service.commitImport(payload, QStringLiteral("local-default"),
                                               QStringLiteral("skip"), &operationError);
    QVERIFY2(operationError.isEmpty(), qPrintable(operationError.message));
    QCOMPARE(imported.value(QStringLiteral("imported")).toInt(), 2);
    verify(database, QStringLiteral("local-default"));
    const auto exported = service.exportCalendar(
        {{QStringLiteral("calendarId"), QStringLiteral("local-default")}},
        &operationError);
    QVERIFY2(operationError.isEmpty(), qPrintable(operationError.message));
    const auto reparsed = caldav::ICalendarCodec::parse(
        exported.value(QStringLiteral("content")).toString().toUtf8());
    QVERIFY(reparsed.ok());
    QVERIFY(reparsed.events.first().recurrenceRule.contains(QStringLiteral("RDATE")));
    Calendar roundTrip;
    roundTrip.id = QStringLiteral("round-trip");
    roundTrip.accountId = QStringLiteral("local-account");
    roundTrip.name = QStringLiteral("Round trip");
    QVERIFY2(database.upsertCalendar(roundTrip, &error), qPrintable(error));
    const auto reimported = service.commitImport(
        exported.value(QStringLiteral("content")).toString().toUtf8(), roundTrip.id,
        QStringLiteral("skip"), &operationError);
    QVERIFY2(operationError.isEmpty(), qPrintable(operationError.message));
    QCOMPARE(reimported.value(QStringLiteral("imported")).toInt(), 2);
    verify(database, roundTrip.id);
    for (const auto& event : database.eventsForCalendars({calendar.id})) {
      QVERIFY(event.rawPayload.isEmpty());
    }
    Event malformed = reparsed.events.first();
    malformed.recurrenceRule = QStringLiteral("RRULE:FREQ=DAILY\nSUMMARY:injected");
    QVERIFY(!caldav::ICalendarCodec::serialize(malformed).ok());
  }
  Database reopened;
  QString error;
  QVERIFY2(reopened.open(path, &error), qPrintable(error));
  verify(reopened, QStringLiteral("recurrence-feed-calendar"));
  verify(reopened, QStringLiteral("local-default"));
  verify(reopened, QStringLiteral("round-trip"));
}

void IcsServiceTest::rejectsCrossOriginRedirects() {
  const QUrl origin(QStringLiteral("https://calendar.example.test/feed.ics"));
  const QUrl sameOrigin(QStringLiteral("https://CALENDAR.example.test:443/moved.ics"));
  const QUrl otherHost(QStringLiteral("https://other.example.test/feed.ics"));
  const QUrl otherPort(QStringLiteral("https://calendar.example.test:444/feed.ics"));
  QVERIFY(ics::IcsService::redirectErrorCode(origin, sameOrigin, false).isEmpty());
  QCOMPARE(ics::IcsService::redirectErrorCode(origin, otherHost, false),
           QStringLiteral("cross_origin_redirect_blocked"));
  QCOMPARE(ics::IcsService::redirectErrorCode(origin, otherPort, true),
           QStringLiteral("credential_redirect_blocked"));
}

void IcsServiceTest::boundsNetworkPayloadBeforeAppend() {
  constexpr qsizetype maximum = 16 * 1024 * 1024;
  QByteArray destination(maximum - 2, 'a');
  QByteArray finalBytes(2, 'b');
  QBuffer exact(&finalBytes);
  QVERIFY(exact.open(QIODevice::ReadOnly));
  QVERIFY(ics::IcsService::consumeReplyBytes(&exact, &destination));
  QCOMPARE(destination.size(), maximum);

  QByteArray overflowByte(1, 'c');
  QBuffer overflow(&overflowByte);
  QVERIFY(overflow.open(QIODevice::ReadOnly));
  QVERIFY(!ics::IcsService::consumeReplyBytes(&overflow, &destination));
  QVERIFY(destination.isEmpty());

  QByteArray oversizedBytes(maximum + 1, 'd');
  QBuffer oversized(&oversizedBytes);
  QVERIFY(oversized.open(QIODevice::ReadOnly));
  QVERIFY(!ics::IcsService::consumeReplyBytes(&oversized, &destination));
  QVERIFY(destination.isEmpty());
  QCOMPARE(oversized.pos(), qint64(maximum + 1));
}

void IcsServiceTest::validatesSubscriptionUrls() {
  QUrl normalized;
  QString error;
  QVERIFY(ics::IcsService::normalizeSubscriptionUrl(
      QStringLiteral("webcal://calendar.example.test/team.ics"), &normalized, &error));
  QCOMPARE(normalized.scheme(), QStringLiteral("https"));
  QCOMPARE(normalized.host(), QStringLiteral("calendar.example.test"));

  QVERIFY(!ics::IcsService::normalizeSubscriptionUrl(
      QStringLiteral("http://calendar.example.test/team.ics"), &normalized, &error));
  QVERIFY(error.contains(QStringLiteral("HTTPS")));
  QVERIFY(!ics::IcsService::normalizeSubscriptionUrl(
      QStringLiteral("https://user:secret@calendar.example.test/team.ics"), &normalized,
      &error));
  QVERIFY(!ics::IcsService::normalizeSubscriptionUrl(
      QStringLiteral("https://calendar.example.test/team.ics#token"), &normalized,
      &error));
}

void IcsServiceTest::previewsAndCommitsDuplicatePolicies() {
  Database database;
  QString error;
  QVERIFY2(database.open(QStringLiteral(":memory:"), &error), qPrintable(error));
  ics::IcsService service(&database);
  ics::IcsError operationError;

  QJsonObject preview = service.previewImport(
      twoEventCalendar(), QStringLiteral("local-default"), &operationError);
  QVERIFY2(operationError.isEmpty(), qPrintable(operationError.message));
  QCOMPARE(preview.value(QStringLiteral("count")).toInt(), 2);
  QCOMPARE(preview.value(QStringLiteral("duplicateCount")).toInt(), 0);

  QJsonObject committed =
      service.commitImport(twoEventCalendar(), QStringLiteral("local-default"),
                           QStringLiteral("skip"), &operationError);
  QVERIFY2(operationError.isEmpty(), qPrintable(operationError.message));
  QCOMPARE(committed.value(QStringLiteral("imported")).toInt(), 2);
  QCOMPARE(database.eventsForCalendars({QStringLiteral("local-default")}).size(), 2);

  preview = service.previewImport(twoEventCalendar(), QStringLiteral("local-default"),
                                  &operationError);
  QCOMPARE(preview.value(QStringLiteral("duplicateCount")).toInt(), 2);
  committed = service.commitImport(twoEventCalendar(), QStringLiteral("local-default"),
                                   QStringLiteral("skip"), &operationError);
  QCOMPARE(committed.value(QStringLiteral("imported")).toInt(), 0);
  QCOMPARE(committed.value(QStringLiteral("skipped")).toInt(), 2);

  committed = service.commitImport(twoEventCalendar(), QStringLiteral("local-default"),
                                   QStringLiteral("copy"), &operationError);
  QCOMPARE(committed.value(QStringLiteral("imported")).toInt(), 2);
  QCOMPARE(database.eventsForCalendars({QStringLiteral("local-default")}).size(), 4);

  committed = service.commitImport(twoEventCalendar(QStringLiteral("Updated planning")),
                                   QStringLiteral("local-default"),
                                   QStringLiteral("replace"), &operationError);
  QCOMPARE(committed.value(QStringLiteral("replaced")).toInt(), 2);
  QCOMPARE(database
               .eventByUid(QStringLiteral("local-default"),
                           QStringLiteral("one@example.test"))
               .summary,
           QStringLiteral("Updated planning"));
}

void IcsServiceTest::exportsOneEventAndLocalCalendar() {
  Database database;
  QString error;
  QVERIFY2(database.open(QStringLiteral(":memory:"), &error), qPrintable(error));
  ics::IcsService service(&database);
  ics::IcsError operationError;
  const QJsonObject committed =
      service.commitImport(twoEventCalendar(), QStringLiteral("local-default"),
                           QStringLiteral("skip"), &operationError);
  QVERIFY2(operationError.isEmpty(), qPrintable(operationError.message));
  const QString firstId =
      committed.value(QStringLiteral("eventIds")).toArray().first().toString();

  QJsonObject exported =
      service.exportCalendar({{QStringLiteral("eventId"), firstId}}, &operationError);
  QVERIFY2(operationError.isEmpty(), qPrintable(operationError.message));
  QCOMPARE(exported.value(QStringLiteral("count")).toInt(), 1);
  const auto parsed = caldav::ICalendarCodec::parse(
      exported.value(QStringLiteral("content")).toString().toUtf8());
  QVERIFY2(parsed.ok(), qPrintable(parsed.error.message));
  QCOMPARE(parsed.events.size(), 1);
  QCOMPARE(parsed.events.first().summary, QStringLiteral("Planning"));

  QTemporaryDir outputDirectory;
  QVERIFY(outputDirectory.isValid());
  const QString outputPath = outputDirectory.filePath(QStringLiteral("all.ics"));
  exported = service.exportCalendar(
      {{QStringLiteral("calendarId"), QStringLiteral("local-default")},
       {QStringLiteral("outputPath"), outputPath}},
      &operationError);
  QVERIFY2(operationError.isEmpty(), qPrintable(operationError.message));
  QCOMPARE(exported.value(QStringLiteral("count")).toInt(), 2);
  QVERIFY(QFile::exists(outputPath));
  QFile written(outputPath);
  QVERIFY(written.open(QIODevice::ReadOnly));
  const auto allParsed = caldav::ICalendarCodec::parse(written.readAll());
  QVERIFY2(allParsed.ok(), qPrintable(allParsed.error.message));
  QCOMPARE(allParsed.events.size(), 2);

  operationError = {};
  QVERIFY(service
              .exportCalendar(
                  {{QStringLiteral("calendarId"), QStringLiteral("local-default")},
                   {QStringLiteral("outputPath"), outputPath}},
                  &operationError)
              .isEmpty());
  QCOMPARE(operationError.code, QStringLiteral("invalid_output_path"));

  Calendar empty;
  empty.id = QStringLiteral("empty-local");
  empty.accountId = QStringLiteral("local-account");
  empty.name = QStringLiteral("Empty");
  QVERIFY(database.upsertCalendar(empty, &error));
  operationError = {};
  exported = service.exportCalendar({{QStringLiteral("calendarId"), empty.id}},
                                    &operationError);
  QVERIFY2(operationError.isEmpty(), qPrintable(operationError.message));
  QCOMPARE(exported.value(QStringLiteral("count")).toInt(), 0);
  QVERIFY(exported.value(QStringLiteral("content"))
              .toString()
              .contains(QStringLiteral("BEGIN:VCALENDAR")));
}

void IcsServiceTest::keepsSubscriptionMetadataPrivate() {
  Database database;
  QString error;
  QVERIFY2(database.open(QStringLiteral(":memory:"), &error), qPrintable(error));
  Account account;
  account.id = QStringLiteral("feed-account");
  account.provider = ProviderKind::Ics;
  account.displayName = QStringLiteral("Team feed");
  account.endpoint = QStringLiteral("https://calendar.example.test/private-token.ics");
  account.authStatus = QStringLiteral("connected");
  QVERIFY(database.upsertAccount(account, &error));
  IcsSubscription subscription;
  subscription.accountId = account.id;
  subscription.url = account.endpoint;
  subscription.etag = QStringLiteral("secret-validator");
  subscription.lastModified = QStringLiteral("Wed, 26 Aug 2026 12:00:00 GMT");
  subscription.lastSuccessAt = QDateTime::currentDateTimeUtc();
  QVERIFY(database.upsertIcsSubscription(subscription, &error));
  const QDateTime updatedAt = subscription.lastSuccessAt.addSecs(30);
  QVERIFY(database.updateIcsSubscriptionResult(
      account.id, QStringLiteral("next-validator"),
      QStringLiteral("Thu, 27 Aug 2026 12:00:00 GMT"), updatedAt, {}, {}, &error));
  IcsSubscription updated = database.icsSubscription(account.id, &error);
  QCOMPARE(updated.etag, QStringLiteral("next-validator"));
  QCOMPARE(updated.lastSuccessAt, updatedAt);
  QVERIFY(database.updateIcsSubscriptionResult(
      account.id, {}, {}, {}, QStringLiteral("network_error"),
      QStringLiteral("The feed could not be reached"), &error));
  updated = database.icsSubscription(account.id, &error);
  QCOMPARE(updated.etag, QStringLiteral("next-validator"));
  QCOMPARE(updated.lastErrorCode, QStringLiteral("network_error"));

  const QJsonObject accountJson = toJson(database.account(account.id));
  QVERIFY(!accountJson.contains(QStringLiteral("endpoint")));
  ics::IcsService service(&database);
  const QJsonObject status = service.status(account.id);
  QVERIFY(!status.contains(QStringLiteral("url")));
  QVERIFY(!status.contains(QStringLiteral("etag")));
  QVERIFY(!status.contains(QStringLiteral("lastModified")));
  QCOMPARE(status.value(QStringLiteral("state")).toString(), QStringLiteral("error"));
  QCOMPARE(status.value(QStringLiteral("errorCode")).toString(),
           QStringLiteral("network_error"));
}

void IcsServiceTest::rejectsMalformedPayloads() {
  ics::IcsError error;
  QByteArray payload;
  QVERIFY(!ics::IcsService::payloadFromRequest(
      {{QStringLiteral("contentBase64"), QStringLiteral("%%%")}}, &payload, &error));
  QCOMPARE(error.code, QStringLiteral("invalid_base64"));

  Database database;
  QString databaseError;
  QVERIFY(database.open(QStringLiteral(":memory:"), &databaseError));
  ics::IcsService service(&database);
  error = {};
  QVERIFY(service
              .previewImport(QByteArrayLiteral("not a calendar"),
                             QStringLiteral("local-default"), &error)
              .isEmpty());
  QVERIFY(!error.code.isEmpty());
}

void IcsServiceTest::credentialStorageDoesNotBlockEventLoop() {
  QTemporaryDir helperDirectory;
  QVERIFY(helperDirectory.isValid());
  const QString helperPath = helperDirectory.filePath(QStringLiteral("secret-tool"));
  QFile helper(helperPath);
  QVERIFY(helper.open(QIODevice::WriteOnly));
  const QByteArray script = QByteArrayLiteral(R"SH(#!/bin/sh
case "$1" in
  store) /bin/sleep 0.15; IFS= read -r ignored; exit 0 ;;
  lookup) printf '%s\n' 'fixture-credential'; exit 0 ;;
  clear) exit 0 ;;
  *) exit 2 ;;
esac
)SH");
  QCOMPARE(helper.write(script), script.size());
  helper.close();
  QVERIFY(QFile::setPermissions(helperPath, QFileDevice::ReadOwner |
                                                QFileDevice::WriteOwner |
                                                QFileDevice::ExeOwner));

  const QByteArray originalPath = qgetenv("PATH");
  qputenv("PATH", helperDirectory.path().toUtf8() + ':' + originalPath);

  Database database;
  QString error;
  QVERIFY2(database.open(QStringLiteral(":memory:"), &error), qPrintable(error));
  ics::IcsService service(&database);
  QSignalSpy accountChanged(&service, &ics::IcsService::accountChanged);
  QElapsedTimer elapsed;
  elapsed.start();
  Account account;
  QVERIFY2(service.addSubscription(
               QStringLiteral("https://calendar.invalid/private.ics"),
               QStringLiteral("Private fixture"), 3600, QStringLiteral("fixture-user"),
               QStringLiteral("fixture-password"), &account, &error),
           qPrintable(error));
  QVERIFY2(elapsed.elapsed() < 100,
           "addSubscription waited for Secret Service instead of returning");
  QCOMPARE(account.authStatus, QStringLiteral("credential_storage_pending"));

  // This test exercises Secret Service scheduling, not feed networking. Pause the
  // fixture account before the credential callback tries its automatic first refresh
  // so no unrelated QNetworkAccessManager work remains in flight at process shutdown.
  Account pausedAccount = database.account(account.id);
  pausedAccount.enabled = false;
  QVERIFY(database.upsertAccount(pausedAccount, &error));

  QTRY_COMPARE_WITH_TIMEOUT(database.account(account.id).authStatus,
                            QStringLiteral("connected"), 2000);
  QVERIFY(accountChanged.count() >= 2);
  QVERIFY(
      database.providerState(account.id, {}, QStringLiteral("has_credentials"), false)
          .toBool());
  qputenv("PATH", originalPath);
}

QTEST_MAIN(IcsServiceTest)

#include "test_ics.moc"
