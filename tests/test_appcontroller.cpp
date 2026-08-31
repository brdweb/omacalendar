#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimeZone>
#include <QtTest/QtTest>

#include "app/appcontroller.h"
#include "app/applicationinstance.h"
#include "app/startuprequest.h"

using namespace omacalendar;

class AppControllerTest final : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void wallTimeConversionRejectsDstGap();
  void wallTimeConversionResolvesDstOverlapToStandardTime();
  void wallTimeConversionRejectsInvalidInput();
  void exposesSystemTimeZoneChoices();
  void browserGoogleFlowRejectsEmptyClientId();
  void startupArgumentsRouteDeepLinks();
  void startupArgumentsRouteLocalIcsFiles();
  void startupArgumentsRejectUnsafeImportTargets();
  void controllerDispatchesValidatedIcsFile();
  void applicationInstanceAllowsOnePrimary();
  void applicationInstanceRoutesActivation();

 private:
  QTemporaryDir m_xdgRoot;
};

void AppControllerTest::initTestCase() {
  QVERIFY(m_xdgRoot.isValid());
  qputenv("XDG_DATA_HOME", m_xdgRoot.filePath(QStringLiteral("data")).toUtf8());
  qputenv("XDG_CACHE_HOME", m_xdgRoot.filePath(QStringLiteral("cache")).toUtf8());
  qputenv("XDG_CONFIG_HOME", m_xdgRoot.filePath(QStringLiteral("config")).toUtf8());
  qputenv("XDG_RUNTIME_DIR", m_xdgRoot.filePath(QStringLiteral("runtime")).toUtf8());
  qputenv("OMACALENDAR_DISABLE_DAEMON_AUTOSTART", "1");
}

void AppControllerTest::wallTimeConversionRejectsDstGap() {
  if (!QTimeZone::isTimeZoneIdAvailable(QByteArrayLiteral("America/New_York"))) {
    QSKIP("IANA time-zone database unavailable");
  }
  AppController controller;
  QCOMPARE(
      controller.wallTimeToUtc(QStringLiteral("2026-03-08"), QStringLiteral("02:30"),
                               QStringLiteral("America/New_York")),
      QString());
}

void AppControllerTest::wallTimeConversionResolvesDstOverlapToStandardTime() {
  if (!QTimeZone::isTimeZoneIdAvailable(QByteArrayLiteral("America/New_York"))) {
    QSKIP("IANA time-zone database unavailable");
  }
  AppController controller;
  const QString utc =
      controller.wallTimeToUtc(QStringLiteral("2026-11-01"), QStringLiteral("01:30"),
                               QStringLiteral("America/New_York"));
  QCOMPARE(utc, QStringLiteral("2026-11-01T06:30:00.000Z"));
  QCOMPARE(controller.utcToWallTime(utc, QStringLiteral("America/New_York")),
           QStringLiteral("2026-11-01T01:30:00.000"));
}

void AppControllerTest::wallTimeConversionRejectsInvalidInput() {
  AppController controller;
  QVERIFY(!controller.isValidTimeZone(QStringLiteral("Mars/Olympus_Mons")));
  QCOMPARE(controller.wallTimeToUtc(QStringLiteral("2026-02-30"),
                                    QStringLiteral("09:00"), QStringLiteral("UTC")),
           QString());
  QCOMPARE(
      controller.utcToWallTime(QStringLiteral("not-a-date"), QStringLiteral("UTC")),
      QString());
}

void AppControllerTest::exposesSystemTimeZoneChoices() {
  AppController controller;
  QVERIFY(!controller.systemTimeZoneId().isEmpty());
  QVERIFY(controller.isValidTimeZone(controller.systemTimeZoneId()));
  const QStringList choices = controller.availableTimeZoneIds();
  QVERIFY(!choices.isEmpty());
  QCOMPARE(choices.first(), controller.systemTimeZoneId());
  QVERIFY(choices.contains(QStringLiteral("UTC")));
}

void AppControllerTest::browserGoogleFlowRejectsEmptyClientId() {
  AppController controller;
  controller.connectGoogleWithClientId(QStringLiteral("   "),
                                       QStringLiteral("Test account"));
  QCOMPARE(controller.lastError(),
           QStringLiteral("Enter a Google Desktop OAuth client ID"));
}

void AppControllerTest::startupArgumentsRouteDeepLinks() {
  const StartupRequest request = startupRequestFromArguments(
      {QStringLiteral("omacalendar"), QStringLiteral("omacalendar://invitations")});
  QVERIFY(request.type == StartupRequestType::DeepLink);
  QCOMPARE(request.url, QUrl(QStringLiteral("omacalendar://invitations")));
}

void AppControllerTest::startupArgumentsRouteLocalIcsFiles() {
  QTemporaryFile file(m_xdgRoot.filePath(QStringLiteral("calendar-XXXXXX.ICS")));
  QVERIFY(file.open());
  QCOMPARE(file.write("BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n"), 32);
  QVERIFY(file.flush());

  StartupRequest request = startupRequestFromArguments(
      {QStringLiteral("omacalendar"), QUrl::fromLocalFile(file.fileName()).toString()});
  QVERIFY(request.type == StartupRequestType::IcsImport);
  QCOMPARE(request.url.toLocalFile(), QFileInfo(file.fileName()).canonicalFilePath());

  request =
      startupRequestFromArguments({QStringLiteral("omacalendar"), file.fileName()});
  QVERIFY(request.type == StartupRequestType::IcsImport);
  QCOMPARE(request.url.toLocalFile(), QFileInfo(file.fileName()).canonicalFilePath());
}

void AppControllerTest::startupArgumentsRejectUnsafeImportTargets() {
  QTemporaryFile wrongExtension(
      m_xdgRoot.filePath(QStringLiteral("calendar-XXXXXX.txt")));
  QVERIFY(wrongExtension.open());
  wrongExtension.write("BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n");
  QVERIFY(wrongExtension.flush());

  const QString missingPath = m_xdgRoot.filePath(QStringLiteral("does-not-exist.ics"));
  const QList<QStringList> rejectedArguments{
      {QStringLiteral("omacalendar"),
       QStringLiteral("https://calendar.example.test/events.ics")},
      {QStringLiteral("omacalendar"),
       QUrl::fromLocalFile(wrongExtension.fileName()).toString()},
      {QStringLiteral("omacalendar"), QUrl::fromLocalFile(missingPath).toString()},
      {QStringLiteral("omacalendar"), QStringLiteral("file://remote/events.ics")},
      {QStringLiteral("omacalendar"), QStringLiteral("file:///%ZZ.ics")},
  };

  for (const QStringList& arguments : rejectedArguments) {
    const StartupRequest request = startupRequestFromArguments(arguments);
    QVERIFY2(request.type == StartupRequestType::None,
             qPrintable(arguments.constLast()));
    QVERIFY(!request.url.isValid());
  }
}

void AppControllerTest::controllerDispatchesValidatedIcsFile() {
  QTemporaryFile file(m_xdgRoot.filePath(QStringLiteral("calendar-XXXXXX.ics")));
  QVERIFY(file.open());
  file.write("BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n");
  QVERIFY(file.flush());

  AppController controller;
  QSignalSpy importSpy(&controller, &AppController::openIcsImportRequested);
  controller.handleIcsImportFile(QUrl::fromLocalFile(file.fileName()));
  QCOMPARE(importSpy.count(), 1);
  QCOMPARE(importSpy.constFirst().constFirst().toUrl().toLocalFile(),
           QFileInfo(file.fileName()).canonicalFilePath());

  controller.handleIcsImportFile(
      QUrl(QStringLiteral("https://calendar.example.test/events.ics")));
  QCOMPARE(importSpy.count(), 1);
  QCOMPARE(controller.lastError(),
           QStringLiteral("Only a readable local .ics file can be imported"));
}

void AppControllerTest::applicationInstanceRoutesActivation() {
  ApplicationInstance instance;
  QSignalSpy activationSpy(&instance, &ApplicationInstance::activationRequested);
  const QString deepLink = QStringLiteral("omacalendar://settings/accounts");
  QVERIFY(instance.activate(deepLink));
  QCOMPARE(activationSpy.count(), 1);
  QCOMPARE(activationSpy.constFirst().constFirst().toString(), deepLink);
}

void AppControllerTest::applicationInstanceAllowsOnePrimary() {
  ApplicationInstance primary;
  ApplicationInstance secondary;
  QVERIFY(primary.claimPrimary());
  QVERIFY(!secondary.claimPrimary());
}

QTEST_GUILESS_MAIN(AppControllerTest)

#include "test_appcontroller.moc"
