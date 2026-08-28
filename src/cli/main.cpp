#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QTextStream>
#include <QTimer>

#include "core/paths.h"
#include "ipc/ipcclient.h"

namespace {

void printUsage(QTextStream& out) {
  out << "Usage: omacalendarctl <method> [json_params]\n"
         "Example: omacalendarctl system.ping {}\n"
         "Example: omacalendarctl events.list '{\"start\":\""
         "2026-08-01T00:00:00Z\",\"end\":\"2026-08-02T00:00:00Z\"}'\n";
}

void printValue(const QJsonValue& value, QTextStream& out) {
  const QJsonDocument document = value.isObject() ? QJsonDocument(value.toObject())
                                                  : QJsonDocument(value.toArray());
  out << document.toJson(QJsonDocument::Compact);
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  const QStringList args = app.arguments();
  QTextStream out(stdout);
  QTextStream err(stderr);

  if (args.size() < 2 || args.at(1) == QStringLiteral("--help") ||
      args.at(1) == QStringLiteral("-h")) {
    printUsage(args.size() < 2 ? err : out);
    return args.size() < 2 ? 1 : 0;
  }

  const QString method = args.at(1);
  QJsonObject params;
  if (args.size() >= 3) {
    const QByteArray payload = args.mid(2).join(QLatin1Char(' ')).toUtf8();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      err << "Invalid JSON params: " << parseError.errorString() << Qt::endl;
      return 1;
    }
    params = document.object();
  }

  omacalendar::ipc::IpcClient client;
  client.setAutoReconnect(false);

  QEventLoop connectLoop;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &connectLoop, &QEventLoop::quit);
  QObject::connect(&client, &omacalendar::ipc::IpcClient::connectedChanged, [&]() {
    if (client.isConnected()) {
      connectLoop.quit();
    }
  });
  client.connectTo(omacalendar::paths::socketFile());
  timeout.start(3000);
  if (!client.isConnected()) {
    connectLoop.exec();
  }
  if (!client.isConnected()) {
    err << "Unable to connect to daemon at " << omacalendar::paths::socketFile()
        << Qt::endl;
    return 1;
  }

  const QString requestId = client.request(method, params);
  if (requestId.isEmpty()) {
    err << "Could not send request" << Qt::endl;
    return 1;
  }

  int exitCode = 1;
  QEventLoop responseLoop;
  QObject::connect(&client, &omacalendar::ipc::IpcClient::responseReceived,
                   [&](const QString& id, const QJsonValue& result) {
                     if (id != requestId) {
                       return;
                     }
                     QJsonObject wrapper{
                         {QStringLiteral("id"), id},
                         {QStringLiteral("result"), result},
                     };
                     printValue(wrapper, out);
                     out << Qt::endl;
                     exitCode = 0;
                     responseLoop.quit();
                   });
  QObject::connect(&client, &omacalendar::ipc::IpcClient::errorReceived,
                   [&](const QString& id, const QJsonObject& error) {
                     if (id != requestId) {
                       return;
                     }
                     QJsonObject wrapper{
                         {QStringLiteral("id"), id},
                         {QStringLiteral("error"), error},
                     };
                     printValue(wrapper, err);
                     err << Qt::endl;
                     exitCode = 1;
                     responseLoop.quit();
                   });
  QObject::connect(&client, &omacalendar::ipc::IpcClient::protocolError,
                   [&](const QString& message) {
                     err << "Protocol error: " << message << Qt::endl;
                     exitCode = 1;
                     responseLoop.quit();
                   });
  QObject::connect(&client, &omacalendar::ipc::IpcClient::notificationReceived,
                   [&](const QString& event, const QJsonObject& data) {
                     QJsonObject payload{
                         {QStringLiteral("event"), event},
                         {QStringLiteral("data"), data},
                     };
                     printValue(payload, out);
                     out << Qt::endl;
                   });

  QTimer responseTimeout;
  responseTimeout.setSingleShot(true);
  responseTimeout.start(5000);
  QObject::connect(&responseTimeout, &QTimer::timeout, [&]() {
    err << "Request timed out" << Qt::endl;
    responseLoop.quit();
  });
  responseLoop.exec();
  client.disconnectFromServer();
  return exitCode;
}
