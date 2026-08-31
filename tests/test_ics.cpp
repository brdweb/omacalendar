#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

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
};

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
