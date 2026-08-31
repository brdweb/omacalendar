#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTimer>

#include "appcontroller.h"
#include "applicationinstance.h"
#include "startuprequest.h"
#include "themebridge.h"

namespace {

QString startupArgument(const omacalendar::StartupRequest& request) {
  return request.type == omacalendar::StartupRequestType::None
             ? QString()
             : request.url.toString(QUrl::FullyEncoded);
}

void dispatchStartupRequest(omacalendar::AppController* controller,
                            const omacalendar::StartupRequest& request) {
  if (request.type == omacalendar::StartupRequestType::DeepLink) {
    controller->handleDeepLink(request.url);
  } else if (request.type == omacalendar::StartupRequestType::IcsImport) {
    controller->handleIcsImportFile(request.url);
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication::setOrganizationName(QStringLiteral("OmaCalendar"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("omacalendar.app"));
  QCoreApplication::setApplicationName(QStringLiteral("OmaCalendar"));
  QCoreApplication::setApplicationVersion(QStringLiteral(OMACALENDAR_VERSION));

  QGuiApplication application(argc, argv);
  QGuiApplication::setDesktopFileName(QStringLiteral("org.omacalendar.OmaCalendar"));
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  const omacalendar::StartupRequest startupRequest =
      omacalendar::startupRequestFromArguments(application.arguments());
  omacalendar::ApplicationInstance applicationInstance;
  if (!applicationInstance.claimPrimary()) {
    return applicationInstance.forwardToPrimary(startupArgument(startupRequest))
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
  }

  omacalendar::AppController controller;
  omacalendar::ThemeBridge theme;

  QObject::connect(&applicationInstance,
                   &omacalendar::ApplicationInstance::activationRequested, &controller,
                   [&controller](const QString& argument) {
                     controller.activateWindow();
                     dispatchStartupRequest(
                         &controller, omacalendar::startupRequestFromArguments(
                                          {QStringLiteral("omacalendar"), argument}));
                   });

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
  engine.rootContext()->setContextProperty(QStringLiteral("OmarchyTheme"), &theme);
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
      []() { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
  engine.loadFromModule(QStringLiteral("OmaCalendar"), QStringLiteral("Main"));

  QTimer::singleShot(0, &controller, [&controller, startupRequest]() {
    dispatchStartupRequest(&controller, startupRequest);
  });

  return application.exec();
}
