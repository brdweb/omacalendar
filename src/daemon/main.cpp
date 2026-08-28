#include <QCoreApplication>
#include <QDateTime>
#include <QTextStream>

#include "daemon.h"

namespace {

void printError(const QString& message) {
  QTextStream err(stderr);
  err << message << Qt::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("OmaCalendar"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("omacalendar.app"));
  QCoreApplication::setApplicationName(QStringLiteral("omacalendard"));

  omacalendar::Daemon daemon;
  QString error;
  if (!daemon.start(&error)) {
    printError(QStringLiteral("Failed to start omacalendard: %1").arg(error));
    return 1;
  }

  QTextStream out(stdout);
  out << QStringLiteral("omacalendard started at ") + daemon.socketPath() +
             QStringLiteral(" (") + QString::number(daemon.connectedClients()) +
             QStringLiteral(" clients)")
      << Qt::endl;

  return app.exec();
}
