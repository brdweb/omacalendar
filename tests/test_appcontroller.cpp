#include <QFileInfo>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QTimeZone>
#include <QtTest/QtTest>
#include <ctime>

#include "app/appcontroller.h"
#include "app/applicationinstance.h"
#include "app/startuprequest.h"
#include "core/paths.h"
#include "ipc/ipcprotocol.h"

using namespace omacalendar;

namespace {

// Answers the desktop controller like omacalendard does, with events.list
// capped at the daemon's default page size so multi-page ranges are
// exercised. Every other method succeeds with an empty result.
class FakeDaemon final : public QObject {
 public:
  static constexpr int kPageCap = 500;

  // Pages holding more than maxEventsPerResponse events fail the way
  // omacalendard does when a response exceeds the IPC frame limit.
  explicit FakeDaemon(const int eventsPerRange,
                      const int maxEventsPerResponse = kPageCap)
      : m_eventsPerRange(eventsPerRange), m_maxEventsPerResponse(maxEventsPerResponse) {
    connect(&m_server, &QLocalServer::newConnection, this, [this]() {
      while (QLocalSocket* socket = m_server.nextPendingConnection()) {
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket]() { read(socket); });
      }
    });
  }

  bool listen() {
    const QString path = paths::socketFile();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QLocalServer::removeServer(path);
    return m_server.listen(path);
  }

  [[nodiscard]] QList<QJsonObject> eventListRequests() const {
    return m_eventListRequests;
  }

 private:
  void read(QLocalSocket* socket) {
    QByteArray& buffer = m_buffers[socket];
    buffer.append(socket->readAll());
    qsizetype newline = -1;
    while ((newline = buffer.indexOf('\n')) >= 0) {
      const QJsonObject request =
          QJsonDocument::fromJson(buffer.left(newline)).object();
      buffer.remove(0, newline + 1);
      const QJsonObject params = request.value(QStringLiteral("params")).toObject();
      QJsonObject response{{QStringLiteral("id"), request.value("id")},
                           {QStringLiteral("result"), QJsonObject()}};
      if (request.value(QStringLiteral("method")).toString() ==
          QStringLiteral("events.list")) {
        m_eventListRequests.append(params);
        const QJsonObject page = eventPage(params);
        if (page.value(QStringLiteral("events")).toArray().size() >
            m_maxEventsPerResponse) {
          response.remove(QStringLiteral("result"));
          response.insert(
              QStringLiteral("error"),
              QJsonObject{
                  {QStringLiteral("code"), QStringLiteral("response_too_large")},
                  {QStringLiteral("message"),
                   QStringLiteral("Response must be requested in smaller pages")},
                  {QStringLiteral("retryable"), false}});
        } else {
          response.insert(QStringLiteral("result"), page);
        }
      }
      socket->write(ipc::frame(response));
    }
  }

  QJsonObject eventPage(const QJsonObject& params) const {
    // Event ids carry the requested range start so a test can tell which
    // range produced them.
    const QString rangeStart = params.value(QStringLiteral("start")).toString();
    const int offset =
        qBound(0, params.value(QStringLiteral("offset")).toInt(), m_eventsPerRange);
    const int limit =
        qMin(params.value(QStringLiteral("limit")).toInt(kPageCap), kPageCap);
    const int count = qMin(limit, m_eventsPerRange - offset);
    QJsonArray events;
    for (int index = offset; index < offset + count; ++index) {
      events.append(QJsonObject{
          {QStringLiteral("id"), QStringLiteral("%1/%2").arg(rangeStart).arg(index)},
          {QStringLiteral("calendarId"), QStringLiteral("local-default")},
          {QStringLiteral("summary"), QStringLiteral("Event %1").arg(index)},
          {QStringLiteral("allDay"), false},
          {QStringLiteral("timeKind"), QStringLiteral("zoned")},
          {QStringLiteral("startTimeZone"), QStringLiteral("UTC")},
          {QStringLiteral("startUtc"), rangeStart},
          {QStringLiteral("endUtc"), rangeStart},
      });
    }
    const int next = offset + count;
    return {{QStringLiteral("events"), events},
            {QStringLiteral("offset"), offset},
            {QStringLiteral("limit"), limit},
            {QStringLiteral("total"), m_eventsPerRange},
            {QStringLiteral("hasMore"), next < m_eventsPerRange},
            {QStringLiteral("nextOffset"), next}};
  }

  QLocalServer m_server;
  QHash<QLocalSocket*, QByteArray> m_buffers;
  QList<QJsonObject> m_eventListRequests;
  int m_eventsPerRange = 0;
  int m_maxEventsPerResponse = kPageCap;
};

}  // namespace

class AppControllerTest final : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void selectedDateKeepsLocalCalendarDate_data();
  void selectedDateKeepsLocalCalendarDate();
  void wallTimeConversionRejectsDstGap();
  void wallTimeConversionResolvesDstOverlapToStandardTime();
  void wallTimeConversionRejectsInvalidInput();
  void localTimeConversionsMatchDesktopTime();
  void exposesSystemTimeZoneChoices();
  void freshPreferencesDefaultToGenericNotifications();
  void browserGoogleFlowRejectsEmptyClientId();
  void externalEventUrlValidation();
  void rejectedExternalEventUrlIsUserVisible();
  void startupArgumentsRouteDeepLinks();
  void startupArgumentsRouteLocalIcsFiles();
  void startupArgumentsRejectUnsafeImportTargets();
  void controllerDispatchesValidatedIcsFile();
  void applicationInstanceAllowsOnePrimary();
  void applicationInstanceRoutesActivation();
  void nativeAndFlatpakInstancesAreIndependent();
  void flatpakActivationRecoversAfterKilledPrimary();
  void rangeLoadingFollowsEveryEventPage();
  void staleRangePagesAreDiscarded();
  void oversizedRangePagesAreRequestedInSmallerPages();

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

void AppControllerTest::selectedDateKeepsLocalCalendarDate_data() {
  QTest::addColumn<QByteArray>("zone");
  for (const auto& zone : {"UTC", "America/New_York", "America/Los_Angeles",
                           "Europe/Berlin", "Pacific/Kiritimati"}) {
    QTest::newRow(zone) << QByteArray(zone);
  }
}

void AppControllerTest::selectedDateKeepsLocalCalendarDate() {
  QFETCH(QByteArray, zone);
  const auto originalZone = qgetenv("TZ");
  const auto restore = qScopeGuard([&] {
    originalZone.isNull() ? qunsetenv("TZ") : qputenv("TZ", originalZone);
    tzset();
  });
  qputenv("TZ", zone);
  tzset();
  AppController controller;
  QJSEngine engine;
  QJSEngine::setObjectOwnership(&controller, QJSEngine::CppOwnership);
  engine.globalObject().setProperty(QStringLiteral("controller"),
                                    engine.newQObject(&controller));
  // Exercise the real native property conversion used by QML Date getters,
  // including navigation across year boundaries and both US DST transitions.
  for (const auto& date :
       {QDate(2026, 9, 9), QDate(2027, 1, 1), QDate(2026, 3, 8), QDate(2026, 11, 1)}) {
    for (const bool propertyWrite : {false, true}) {
      const auto value = QStringLiteral("new Date(%1, %2, %3)")
                             .arg(date.year())
                             .arg(date.month() - 1)
                             .arg(date.day());
      const auto script =
          propertyWrite ? QStringLiteral("controller.selectedDate = %1").arg(value)
                        : QStringLiteral("controller.setSelectedDate(%1)").arg(value);
      const auto result = engine.evaluate(script);
      QVERIFY2(!result.isError(), qPrintable(result.toString()));
      QCOMPARE(engine.evaluate(QStringLiteral("controller.selectedDate.getFullYear()"))
                   .toInt(),
               date.year());
      QCOMPARE(
          engine.evaluate(QStringLiteral("controller.selectedDate.getMonth()")).toInt(),
          date.month() - 1);
      QCOMPARE(
          engine.evaluate(QStringLiteral("controller.selectedDate.getDate()")).toInt(),
          date.day());
    }
  }
}

void AppControllerTest::nativeAndFlatpakInstancesAreIndependent() {
  // Unix sockets are limited to 108 bytes, including the temporary prefix.
  QTemporaryDir runtime(QStringLiteral("/tmp/omac-instance-XXXXXX"));
  QVERIFY(runtime.isValid());
  const QByteArray originalFlatpak = qgetenv("FLATPAK_ID");
  const QByteArray originalRuntime = qgetenv("XDG_RUNTIME_DIR");
  const auto restore = qScopeGuard([&]() {
    originalFlatpak.isNull() ? qunsetenv("FLATPAK_ID")
                             : qputenv("FLATPAK_ID", originalFlatpak);
    originalRuntime.isNull() ? qunsetenv("XDG_RUNTIME_DIR")
                             : qputenv("XDG_RUNTIME_DIR", originalRuntime);
  });
  qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
  qunsetenv("FLATPAK_ID");
  ApplicationInstance native;
  QVERIFY(native.claimPrimary());
  qputenv("FLATPAK_ID", "org.omacalendar.OmaCalendar");
  ApplicationInstance sandbox;
  QVERIFY(sandbox.claimPrimary());
  ApplicationInstance secondSandbox;
  QVERIFY(!secondSandbox.claimPrimary());
  qunsetenv("FLATPAK_ID");
  ApplicationInstance secondNative;
  QVERIFY(!secondNative.claimPrimary());
}

void AppControllerTest::flatpakActivationRecoversAfterKilledPrimary() {
  QTemporaryDir runtime(QStringLiteral("/tmp/omac-crash-XXXXXX"));
  QVERIFY(runtime.isValid());
  const QByteArray originalFlatpak = qgetenv("FLATPAK_ID");
  const QByteArray originalRuntime = qgetenv("XDG_RUNTIME_DIR");
  const auto restore = qScopeGuard([&]() {
    originalFlatpak.isNull() ? qunsetenv("FLATPAK_ID")
                             : qputenv("FLATPAK_ID", originalFlatpak);
    originalRuntime.isNull() ? qunsetenv("XDG_RUNTIME_DIR")
                             : qputenv("XDG_RUNTIME_DIR", originalRuntime);
  });
  qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
  qputenv("FLATPAK_ID", "org.omacalendar.OmaCalendar");
  QVERIFY(QDir().mkpath(paths::runtimeDirectory()));
  // Reproduce a legacy PID lock that appears owned by a currently live process
  // after PID reuse. Flatpak primary ownership must not depend on this data.
  QFile legacy(QDir(paths::runtimeDirectory())
                   .filePath(QStringLiteral("app-instance.sock.lock")));
  QVERIFY(legacy.open(QIODevice::WriteOnly));
  QTextStream(&legacy) << QCoreApplication::applicationPid() << '\n'
                       << QFileInfo(QCoreApplication::applicationFilePath()).fileName()
                       << '\n'
                       << QSysInfo::machineHostName() << "\n\n"
                       << QSysInfo::bootUniqueId() << '\n';
  legacy.close();

  QProcess child;
  child.start(QCoreApplication::applicationFilePath(),
              {QStringLiteral("--hold-flatpak-activation-lock")});
  QVERIFY(child.waitForStarted(2000));
  QVERIFY(child.waitForReadyRead(2000));
  QCOMPARE(child.readAllStandardOutput().trimmed(), QByteArray("ready"));
  ApplicationInstance contender;
  QVERIFY(!contender.claimPrimary());
  child.kill();  // No Qt destructors: the kernel must release the file lock.
  QVERIFY(child.waitForFinished(2000));
  QCOMPARE(child.exitStatus(), QProcess::CrashExit);
  {
    ApplicationInstance recovered;
    QVERIFY(recovered.claimPrimary());
  }
  ApplicationInstance reopened;
  QVERIFY(reopened.claimPrimary());
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

void AppControllerTest::localTimeConversionsMatchDesktopTime() {
  AppController controller;
  // Do not force TZ: this also covers minimal /etc environments where the
  // named system-zone fallback can disagree with Qt's actual local clock.
  for (const auto& date : {QDate(2030, 1, 15), QDate(2030, 7, 15)}) {
    const QDateTime local(date, QTime(9, 30), QTimeZone::LocalTime);
    const QString utc = local.toUTC().toString(Qt::ISODateWithMs);
    QCOMPARE(controller.wallTimeToUtc(date.toString(Qt::ISODate),
                                      QStringLiteral("09:30"), {}),
             utc);
    QCOMPARE(controller.utcToWallTime(utc, {}),
             local.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz")));
    // An explicit named zone is independent of the desktop's local clock.
    QCOMPARE(controller.wallTimeToUtc(date.toString(Qt::ISODate),
                                      QStringLiteral("09:30"), QStringLiteral("UTC")),
             date.toString(Qt::ISODate) + QStringLiteral("T09:30:00.000Z"));
  }
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

void AppControllerTest::freshPreferencesDefaultToGenericNotifications() {
  AppController controller;
  QCOMPARE(
      controller.preferences().value(QStringLiteral("notificationPrivacy")).toString(),
      QStringLiteral("generic"));
}

void AppControllerTest::browserGoogleFlowRejectsEmptyClientId() {
  AppController controller;
  controller.connectGoogleWithClientId(QStringLiteral("   "),
                                       QStringLiteral("Test account"));
  QCOMPARE(controller.lastError(),
           QStringLiteral("Enter a Google Desktop OAuth client ID"));
}

void AppControllerTest::externalEventUrlValidation() {
  AppController controller;
  const QStringList accepted{
      QStringLiteral("https://meet.example.test/rooms/123?auth=a%20b#join"),
      QStringLiteral("http://localhost:8080/conference"),
      QStringLiteral("HTTPS://calendar.example.test/event"),
  };
  for (const QString& value : accepted) {
    QVERIFY2(controller.canOpenExternalEventUrl(value), qPrintable(value));
  }

  const QStringList rejected{
      QString(),
      QStringLiteral(" https://example.test/meeting"),
      QStringLiteral("https://example.test/meeting "),
      QStringLiteral("https:example.test/meeting"),
      QStringLiteral("https:///missing-host"),
      QStringLiteral("//example.test/meeting"),
      QStringLiteral("meeting-room"),
      QStringLiteral("file:///etc/passwd"),
      QStringLiteral("javascript:alert(1)"),
      QStringLiteral("data:text/html,hello"),
      QStringLiteral("mailto:host@example.test"),
      QStringLiteral("tel:+15555550123"),
      QStringLiteral("webcal://example.test/calendar"),
      QStringLiteral("omacalendar://event/123"),
      QStringLiteral("https://exa mple.test/meeting"),
      QStringLiteral("https://example.test/%ZZ"),
  };
  for (const QString& value : rejected) {
    QVERIFY2(!controller.canOpenExternalEventUrl(value), qPrintable(value));
  }
}

void AppControllerTest::rejectedExternalEventUrlIsUserVisible() {
  AppController controller;
  controller.openExternalEventUrl(QStringLiteral("file:///etc/passwd"));
  QCOMPARE(controller.lastError(),
           QStringLiteral("Only valid HTTP or HTTPS event links can be opened"));
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

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  if (application.arguments().contains(
          QStringLiteral("--hold-flatpak-activation-lock"))) {
    ApplicationInstance instance;
    if (!instance.claimPrimary()) {
      return 2;
    }
    QTextStream(stdout) << "ready" << Qt::endl;
    return application.exec();
  }
  AppControllerTest test;
  return QTest::qExec(&test, argc, argv);
}

void AppControllerTest::rangeLoadingFollowsEveryEventPage() {
  // More events than one daemon page holds: the controller must keep reading
  // instead of showing only the first page.
  constexpr int kEvents = 2 * FakeDaemon::kPageCap + 3;
  FakeDaemon daemon(kEvents);
  QVERIFY(daemon.listen());
  AppController controller;
  QTRY_VERIFY(controller.connected());

  controller.loadRange(QDate(2026, 1, 1), QDate(2026, 12, 31));
  const QString prefix = QStringLiteral("2026-01-01");
  QTRY_COMPARE(controller.eventsModel()->rowCount(), kEvents);
  const QVariantList events = controller.events();
  QCOMPARE(events.size(), kEvents);
  QSet<QString> ids;
  for (const QVariant& event : events) {
    const QString id = event.toMap().value(QStringLiteral("id")).toString();
    QVERIFY2(id.startsWith(prefix), qPrintable(id));
    ids.insert(id);
  }
  QCOMPARE(ids.size(), kEvents);

  QList<int> offsets;
  for (const QJsonObject& request : daemon.eventListRequests()) {
    if (request.value(QStringLiteral("start")).toString().startsWith(prefix)) {
      offsets.append(request.value(QStringLiteral("offset")).toInt());
      QVERIFY(request.value(QStringLiteral("limit")).toInt() >= FakeDaemon::kPageCap);
    }
  }
  QCOMPARE(offsets, (QList<int>{0, FakeDaemon::kPageCap, 2 * FakeDaemon::kPageCap}));
}

void AppControllerTest::staleRangePagesAreDiscarded() {
  constexpr int kEvents = FakeDaemon::kPageCap + 10;
  FakeDaemon daemon(kEvents);
  QVERIFY(daemon.listen());
  AppController controller;
  QTRY_VERIFY(controller.connected());

  // The first range is superseded before its first page arrives. Neither its
  // pages nor its follow-up requests may reach the model.
  controller.loadRange(QDate(2026, 3, 1), QDate(2026, 3, 31));
  controller.loadRange(QDate(2026, 5, 1), QDate(2026, 5, 31));
  const QString current = QStringLiteral("2026-05-01");
  QTRY_COMPARE(controller.eventsModel()->rowCount(), kEvents);
  for (const QVariant& event : controller.events()) {
    const QString id = event.toMap().value(QStringLiteral("id")).toString();
    QVERIFY2(id.startsWith(current), qPrintable(id));
  }
  int supersededPages = 0;
  for (const QJsonObject& request : daemon.eventListRequests()) {
    if (request.value(QStringLiteral("start"))
            .toString()
            .startsWith(QStringLiteral("2026-03-01"))) {
      ++supersededPages;
    }
  }
  QCOMPARE(supersededPages, 1);
}

void AppControllerTest::oversizedRangePagesAreRequestedInSmallerPages() {
  // Large events can make a full page exceed the IPC frame limit. The
  // controller must shrink its pages instead of abandoning the range.
  constexpr int kEvents = 2 * FakeDaemon::kPageCap + 3;
  constexpr int kMaxEventsPerResponse = 120;
  FakeDaemon daemon(kEvents, kMaxEventsPerResponse);
  QVERIFY(daemon.listen());
  AppController controller;
  QTRY_VERIFY(controller.connected());

  controller.loadRange(QDate(2026, 1, 1), QDate(2026, 12, 31));
  const QString prefix = QStringLiteral("2026-01-01");
  QTRY_COMPARE(controller.eventsModel()->rowCount(), kEvents);
  QSet<QString> ids;
  for (const QVariant& event : controller.events()) {
    const QString id = event.toMap().value(QStringLiteral("id")).toString();
    QVERIFY2(id.startsWith(prefix), qPrintable(id));
    ids.insert(id);
  }
  QCOMPARE(ids.size(), kEvents);
  QVERIFY2(controller.lastError().isEmpty(), qPrintable(controller.lastError()));

  QList<int> limits;
  for (const QJsonObject& request : daemon.eventListRequests()) {
    if (request.value(QStringLiteral("start")).toString().startsWith(prefix)) {
      limits.append(request.value(QStringLiteral("limit")).toInt());
    }
  }
  // 500 → 250 → 125 are rejected; 62-event pages then cover the range.
  QCOMPARE(limits.mid(0, 4), (QList<int>{500, 250, 125, 62}));
  QVERIFY(limits.size() > 4);
  for (int index = 3; index < limits.size(); ++index) {
    QCOMPARE(limits.at(index), 62);
  }
}

#include "test_appcontroller.moc"
