#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include "appcontroller.h"
#include "themebridge.h"

int main(int argc, char* argv[]) {
  QCoreApplication::setOrganizationName(QStringLiteral("OmaCalendar"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("omacalendar.app"));
  QCoreApplication::setApplicationName(QStringLiteral("OmaCalendar"));
  QCoreApplication::setApplicationVersion(QStringLiteral(OMACALENDAR_VERSION));

  QGuiApplication application(argc, argv);
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  omacalendar::AppController controller;
  omacalendar::ThemeBridge theme;

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
  engine.rootContext()->setContextProperty(QStringLiteral("OmarchyTheme"), &theme);
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
      []() { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
  engine.loadFromModule(QStringLiteral("OmaCalendar"), QStringLiteral("Main"));

  return application.exec();
}
