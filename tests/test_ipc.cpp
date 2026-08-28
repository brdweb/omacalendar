// Copyright (c) 2026

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest/QtTest>

#include "core/domain.h"
#include "ipc/ipcclient.h"
#include "ipc/ipcprotocol.h"
#include "ipc/ipcserver.h"
#include "ipc/requestrouter.h"

using namespace omacalendar;
using namespace omacalendar::ipc;

namespace {

QString uniqueSocketPath() {
  return QStringLiteral("omacalendar-test-") +
         QUuid::createUuid().toString(QUuid::WithoutBraces);
}

}  // namespace

class IpcTest final : public QObject {
  Q_OBJECT

 private:
  static QJsonObject addOneHandler(const QJsonObject& params, Error* error) {
    if (!params.contains("value") || !params.value("value").isDouble()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_params"),
                  QStringLiteral("value must be a number"), false};
      }
      return {};
    }
    return QJsonObject{
        {QStringLiteral("value"), params.value(QStringLiteral("value")).toInt() + 1}};
  }

 private slots:
  void protocolHelpers() {
    const QJsonObject expectedSuccess{{QStringLiteral("id"), 1},
                                      {QStringLiteral("result"), QStringLiteral("ok")}};
    QCOMPARE(successResponse(1, QStringLiteral("ok")), expectedSuccess);
    const QJsonObject failure =
        errorResponse(2, {QStringLiteral("bad"), QStringLiteral("bad request"), false});
    QCOMPARE(failure.value(QStringLiteral("id")).toInt(), 2);
    QCOMPARE(failure.value(QStringLiteral("error"))
                 .toObject()
                 .value(QStringLiteral("code"))
                 .toString(),
             QStringLiteral("bad"));

    const QByteArray framed = frame(QJsonObject{{QStringLiteral("hello"), 42}});
    QVERIFY(framed.endsWith('\n'));

    QCOMPARE(
        notification(QStringLiteral("sync"), QJsonObject{{QStringLiteral("ok"), true}})
            .value(QStringLiteral("event"))
            .toString(),
        QStringLiteral("sync"));
  }

  void requestRouter() {
    RequestRouter router;
    router.registerHandler(QStringLiteral("add"), addOneHandler);

    QJsonObject request{
        {QStringLiteral("id"), 17},
        {QStringLiteral("protocolMajor"), kIpcProtocolMajor},
        {QStringLiteral("method"), QStringLiteral("add")},
        {QStringLiteral("params"), QJsonObject{{QStringLiteral("value"), 3}}},
    };
    const QJsonObject response = router.route(request);
    QCOMPARE(response.value(QStringLiteral("result"))
                 .toObject()
                 .value(QStringLiteral("value"))
                 .toInt(),
             4);

    QJsonObject missingId = request;
    missingId.remove(QStringLiteral("id"));
    const QJsonObject missingIdResponse = router.route(missingId);
    QCOMPARE(missingIdResponse.value(QStringLiteral("error"))
                 .toObject()
                 .value(QStringLiteral("code"))
                 .toString(),
             QStringLiteral("invalid_request"));

    QJsonObject unknown = request;
    unknown[QStringLiteral("method")] = QStringLiteral("missing");
    const QJsonObject unknownResponse = router.route(unknown);
    QCOMPARE(unknownResponse.value(QStringLiteral("error"))
                 .toObject()
                 .value(QStringLiteral("code"))
                 .toString(),
             QStringLiteral("method_not_found"));
  }

  void serverClientRoundTrip() {
    RequestRouter router;
    router.registerHandler(QStringLiteral("add"), addOneHandler);
    IpcServer server(&router);
    QString path = uniqueSocketPath();
    QString serverError;
    QVERIFY2(server.listen(path, &serverError), qPrintable(serverError));
    QCOMPARE(server.isListening(), true);
    QCOMPARE(server.path(), path);

    IpcClient client;
    QSignalSpy connectedSpy(&client, &IpcClient::connectedChanged);
    client.setAutoReconnect(false);
    client.connectTo(path);
    QTRY_VERIFY(client.isConnected());

    QSignalSpy responseSpy(&client, &IpcClient::responseReceived);
    const QString id = client.request(QStringLiteral("add"),
                                      QJsonObject{{QStringLiteral("value"), 8}});
    QCOMPARE(id.isEmpty(), false);
    QTRY_COMPARE(responseSpy.count(), 1);

    const QList<QVariant> responseArgs = responseSpy.takeFirst();
    QCOMPARE(responseArgs.at(0).toString(), id);
    const QJsonObject result = qvariant_cast<QJsonValue>(responseArgs.at(1)).toObject();
    QCOMPARE(result.value(QStringLiteral("value")).toInt(), 9);

    QSignalSpy protocolSpy(&client, &IpcClient::protocolError);
    QCOMPARE(protocolSpy.isEmpty(), true);
    connectedSpy.clear();
    client.disconnectFromServer();
    QTRY_VERIFY(!client.isConnected());

    server.close();
    QCOMPARE(server.isListening(), false);
  }

  void serverFrameViolation() {
    RequestRouter router;
    IpcServer server(&router);
    QString path = uniqueSocketPath();
    QString error;
    QVERIFY(server.listen(path, &error));

    QSignalSpy protocolSpy(&server, &IpcServer::protocolViolation);
    QSignalSpy clientCountSpy(&server, &IpcServer::clientCountChanged);

    QLocalSocket rogue;
    rogue.connectToServer(path);
    QVERIFY2(rogue.waitForConnected(), qPrintable(rogue.errorString()));
    const QByteArray largePayload(kMaximumFrameBytes + 1, 'x');
    rogue.write(largePayload + '\n');
    rogue.flush();

    QTRY_VERIFY(!protocolSpy.isEmpty());
    QTRY_COMPARE(server.clientCount(), 0);

    rogue.disconnectFromServer();
    server.close();
    protocolSpy.clear();
    clientCountSpy.clear();
  }

  void clientNotification() {
    RequestRouter router;
    IpcServer server(&router);
    QString path = uniqueSocketPath();
    QString error;
    QVERIFY(server.listen(path, &error));

    IpcClient client;
    QSignalSpy notifications(&client, &IpcClient::notificationReceived);
    client.connectTo(path);
    QTRY_COMPARE(server.clientCount(), 1);
    QTRY_VERIFY(client.isConnected());

    server.broadcast(QStringLiteral("refresh"),
                     QJsonObject{{QStringLiteral("state"), true}});
    QTRY_COMPARE(notifications.count(), 1);
    const QList<QVariant> args = notifications.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("refresh"));
    QCOMPARE(
        args.at(1).toJsonValue().toObject().value(QStringLiteral("state")).toBool(),
        true);

    client.disconnectFromServer();
    server.close();
  }
};

QTEST_MAIN(IpcTest)
#include "test_ipc.moc"
