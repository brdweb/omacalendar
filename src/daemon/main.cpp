#include <unistd.h>

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

qintptr inheritedSocketDescriptor() {
  bool pidOk = false;
  bool countOk = false;
  const qlonglong listenPid = qEnvironmentVariable("LISTEN_PID").toLongLong(&pidOk);
  const int listenFds = qEnvironmentVariableIntValue("LISTEN_FDS", &countOk);
  const QByteArray descriptorNames = qgetenv("LISTEN_FDNAMES");
  qunsetenv("LISTEN_PID");
  qunsetenv("LISTEN_FDS");
  qunsetenv("LISTEN_FDNAMES");
  if (!pidOk || !countOk || listenPid != static_cast<qlonglong>(getpid()) ||
      listenFds != 1 ||
      (!descriptorNames.isEmpty() &&
       descriptorNames != QByteArrayLiteral("omacalendar"))) {
    return -1;
  }
  return 3;
}

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("OmaCalendar"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("omacalendar.app"));
  QCoreApplication::setApplicationName(QStringLiteral("omacalendard"));

  omacalendar::Daemon daemon;
  QString error;
  if (!daemon.start(&error, inheritedSocketDescriptor())) {
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
