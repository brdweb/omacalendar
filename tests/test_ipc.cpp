// Copyright (c) 2026

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest/QtTest>
#include <algorithm>

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

  static QJsonObject subscribeHandler(const QJsonObject& params, Error*) {
    QJsonValue topics = params.value(QStringLiteral("topics"));
    if (!topics.isArray()) {
      topics = QJsonArray{QStringLiteral("*")};
    }
    return QJsonObject{{QStringLiteral("subscribed"), true},
                       {QStringLiteral("topics"), topics},
                       {QStringLiteral("revision"), 1},
                       {QStringLiteral("catchUpRequired"), false}};
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

  void connectedClientCanBeDestroyedWithoutRunningDisconnectCallbacks() {
    RequestRouter router;
    IpcServer server(&router);
    const QString path = uniqueSocketPath();
    QString serverError;
    QVERIFY2(server.listen(path, &serverError), qPrintable(serverError));

    {
      IpcClient client;
      client.setAutoReconnect(true);
      client.connectTo(path);
      QTRY_VERIFY(client.isConnected());
    }

    server.close();
  }

  void inheritedServerSocketRoundTrip() {
    RequestRouter router;
    router.registerHandler(QStringLiteral("add"), addOneHandler);
    const QString path = QDir::temp().filePath(uniqueSocketPath());
    const QByteArray encodedPath = QFile::encodeName(path);
    const int activationDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(activationDescriptor >= 0);
    sockaddr_un address{};
    QVERIFY(encodedPath.size() < static_cast<qsizetype>(sizeof(address.sun_path)));
    address.sun_family = AF_UNIX;
    std::copy(encodedPath.cbegin(), encodedPath.cend(), address.sun_path);
    address.sun_path[encodedPath.size()] = '\0';
    QVERIFY(::bind(activationDescriptor, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == 0);
    QVERIFY(::listen(activationDescriptor, 16) == 0);
    const qintptr inheritedDescriptor = ::dup(activationDescriptor);
    QVERIFY(inheritedDescriptor >= 0);

    IpcServer server(&router);
    QString serverError;
    QVERIFY2(server.listen(inheritedDescriptor, path, &serverError),
             qPrintable(serverError));
    QCOMPARE(server.path(), path);

    IpcClient client;
    client.setAutoReconnect(false);
    client.connectTo(path);
    QTRY_VERIFY(client.isConnected());
    QSignalSpy responseSpy(&client, &IpcClient::responseReceived);
    QVERIFY(
        !client
             .request(QStringLiteral("add"), QJsonObject{{QStringLiteral("value"), 4}})
             .isEmpty());
    QTRY_COMPARE(responseSpy.count(), 1);
    QCOMPARE(
        responseSpy.takeFirst().at(1).toJsonValue().toObject().value("value").toInt(),
        5);

    client.disconnectFromServer();
    server.close();
    QLocalSocket activationProbe;
    activationProbe.connectToServer(path);
    QVERIFY(activationProbe.waitForConnected(1000));
    activationProbe.disconnectFromServer();
    QVERIFY(::close(activationDescriptor) == 0);
    QVERIFY(QFile::remove(path));
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

  void fragmentedMalformedAndBatchedFramesRemainIsolated() {
    RequestRouter router;
    router.registerHandler(QStringLiteral("add"), addOneHandler);
    IpcServer server(&router);
    QString error;
    QVERIFY2(server.listen(uniqueSocketPath(), &error), qPrintable(error));

    QLocalSocket client;
    QByteArray responses;
    connect(&client, &QLocalSocket::readyRead, &client,
            [&client, &responses]() { responses.append(client.readAll()); });
    client.connectToServer(server.path());
    QVERIFY2(client.waitForConnected(), qPrintable(client.errorString()));
    QTRY_COMPARE(server.clientCount(), 1);

    const QByteArray fragmented =
        frame({{QStringLiteral("id"), QStringLiteral("fragmented")},
               {QStringLiteral("protocolMajor"), kIpcProtocolMajor},
               {QStringLiteral("method"), QStringLiteral("add")},
               {QStringLiteral("params"), QJsonObject{{QStringLiteral("value"), 8}}}});
    const qsizetype split = fragmented.size() / 2;
    QCOMPARE(client.write(fragmented.left(split)), split);
    client.flush();
    QTest::qWait(10);
    QCOMPARE(responses.count('\n'), 0);

    QCOMPARE(client.write(fragmented.mid(split)), fragmented.size() - split);
    client.flush();
    QTRY_COMPARE(responses.count('\n'), 1);
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(responses.trimmed(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QCOMPARE(document.object().value(QStringLiteral("id")).toString(),
             QStringLiteral("fragmented"));
    QCOMPARE(document.object()
                 .value(QStringLiteral("result"))
                 .toObject()
                 .value(QStringLiteral("value"))
                 .toInt(),
             9);

    responses.clear();
    constexpr int kBatchSize = 32;
    QByteArray batch = QByteArrayLiteral("{malformed-json\n");
    for (int index = 0; index < kBatchSize; ++index) {
      batch.append(frame(
          {{QStringLiteral("id"), index},
           {QStringLiteral("protocolMajor"), kIpcProtocolMajor},
           {QStringLiteral("method"), QStringLiteral("add")},
           {QStringLiteral("params"), QJsonObject{{QStringLiteral("value"), index}}}}));
    }
    QCOMPARE(client.write(batch), batch.size());
    client.flush();
    QTRY_COMPARE(responses.count('\n'), kBatchSize + 1);

    QList<QByteArray> responseFrames = responses.split('\n');
    QVERIFY(responseFrames.last().isEmpty());
    responseFrames.removeLast();
    QCOMPARE(responseFrames.size(), kBatchSize + 1);

    document = QJsonDocument::fromJson(responseFrames.first(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QCOMPARE(document.object()
                 .value(QStringLiteral("error"))
                 .toObject()
                 .value(QStringLiteral("code"))
                 .toString(),
             QStringLiteral("parse_error"));
    for (int index = 0; index < kBatchSize; ++index) {
      document = QJsonDocument::fromJson(responseFrames.at(index + 1), &parseError);
      QCOMPARE(parseError.error, QJsonParseError::NoError);
      QCOMPARE(document.object().value(QStringLiteral("id")).toInt(), index);
      QCOMPARE(document.object()
                   .value(QStringLiteral("result"))
                   .toObject()
                   .value(QStringLiteral("value"))
                   .toInt(),
               index + 1);
    }

    client.disconnectFromServer();
    server.close();
  }

  void disconnectMidFrameAndRapidRequestChurnAreSafe() {
    RequestRouter router;
    int routeCount = 0;
    router.registerHandler(QStringLiteral("count"),
                           [&routeCount](const QJsonObject&, Error*) {
                             ++routeCount;
                             return QJsonObject{{QStringLiteral("count"), routeCount}};
                           });
    IpcServer server(&router);
    QString error;
    QVERIFY2(server.listen(uniqueSocketPath(), &error), qPrintable(error));

    constexpr int kPartialDisconnects = 16;
    for (int index = 0; index < kPartialDisconnects; ++index) {
      QLocalSocket client;
      client.connectToServer(server.path());
      QVERIFY2(client.waitForConnected(), qPrintable(client.errorString()));
      QTRY_COMPARE(server.clientCount(), 1);
      const QByteArray partial =
          QByteArrayLiteral("{\"id\":\"partial-") + QByteArray::number(index);
      QCOMPARE(client.write(partial), partial.size());
      client.flush();
      client.abort();
      QTRY_COMPARE(server.clientCount(), 0);
    }

    constexpr int kRequestCycles = 32;
    for (int index = 0; index < kRequestCycles; ++index) {
      QLocalSocket client;
      client.connectToServer(server.path());
      QVERIFY2(client.waitForConnected(), qPrintable(client.errorString()));
      QTRY_COMPARE(server.clientCount(), 1);
      const QByteArray request =
          frame({{QStringLiteral("id"), index},
                 {QStringLiteral("protocolMajor"), kIpcProtocolMajor},
                 {QStringLiteral("method"), QStringLiteral("count")},
                 {QStringLiteral("params"), QJsonObject{}}});
      QCOMPARE(client.write(request), request.size());
      client.flush();
      QTRY_COMPARE(routeCount, index + 1);
      client.abort();
      QTRY_COMPARE(server.clientCount(), 0);
    }

    QCOMPARE(routeCount, kRequestCycles);
    server.close();
  }

  void clientDropsPartialFrameAcrossReconnect() {
    const QString path = uniqueSocketPath();
    QLocalServer rawServer;
    QLocalServer::removeServer(path);
    QVERIFY2(rawServer.listen(path), qPrintable(rawServer.errorString()));

    IpcClient client;
    client.setAutoReconnect(false);
    QSignalSpy responses(&client, &IpcClient::responseReceived);
    QSignalSpy protocolErrors(&client, &IpcClient::protocolError);
    client.connectTo(path);
    QTRY_VERIFY(rawServer.hasPendingConnections());
    QLocalSocket* firstConnection = rawServer.nextPendingConnection();
    QVERIFY(firstConnection != nullptr);
    QTRY_VERIFY(client.isConnected());

    const QByteArray partial = QByteArrayLiteral("{\"id\":\"stale");
    QCOMPARE(firstConnection->write(partial), partial.size());
    firstConnection->flush();
    QTRY_COMPARE(firstConnection->bytesToWrite(), 0);
    // Give the client event loop a turn to retain the incomplete frame before
    // the peer closes the first connection.
    QTest::qWait(10);
    firstConnection->disconnectFromServer();
    QTRY_VERIFY(!client.isConnected());

    client.connectTo(path);
    QTRY_VERIFY(rawServer.hasPendingConnections());
    QLocalSocket* secondConnection = rawServer.nextPendingConnection();
    QVERIFY(secondConnection != nullptr);
    QTRY_VERIFY(client.isConnected());

    const QByteArray fresh = frame(successResponse(
        QStringLiteral("fresh"), QJsonObject{{QStringLiteral("value"), 42}}));
    QCOMPARE(secondConnection->write(fresh), fresh.size());
    secondConnection->flush();
    QTRY_COMPARE(responses.count(), 1);
    QCOMPARE(protocolErrors.count(), 0);
    const QList<QVariant> response = responses.takeFirst();
    QCOMPARE(response.at(0).toString(), QStringLiteral("fresh"));
    QCOMPARE(
        response.at(1).toJsonValue().toObject().value(QStringLiteral("value")).toInt(),
        42);

    client.disconnectFromServer();
    secondConnection->disconnectFromServer();
    rawServer.close();
  }

  void explicitReconnectDoesNotLeaveADeferredRetry() {
    RequestRouter router;
    IpcServer server(&router);
    QString error;
    QVERIFY2(server.listen(uniqueSocketPath(), &error), qPrintable(error));

    IpcClient client;
    client.setAutoReconnect(true);
    QSignalSpy connectedChanges(&client, &IpcClient::connectedChanged);
    client.connectTo(server.path());
    QTRY_VERIFY(client.isConnected());
    QTRY_COMPARE(server.clientCount(), 1);

    connectedChanges.clear();
    client.connectTo(server.path());
    QTRY_VERIFY(client.isConnected());
    QTRY_COMPARE(server.clientCount(), 1);
    QTRY_COMPARE(connectedChanges.count(), 2);

    // Replacing an active connection emits a disconnect and a connect. The
    // disconnect must not leave a one-second retry armed after the replacement
    // connection has already succeeded.
    QTest::qWait(1250);
    QVERIFY(client.isConnected());
    QCOMPARE(server.clientCount(), 1);
    QCOMPARE(connectedChanges.count(), 2);

    client.disconnectFromServer();
    server.close();
  }

  void routeMayReentrantlyDetachTheReadingClient() {
    RequestRouter router;
    IpcServer server(&router);
    router.registerHandler(QStringLiteral("close"),
                           [&server](const QJsonObject&, Error*) {
                             server.close();
                             return QJsonObject{{QStringLiteral("closed"), true}};
                           });
    QString error;
    QVERIFY2(server.listen(uniqueSocketPath(), &error), qPrintable(error));

    QLocalSocket client;
    client.connectToServer(server.path());
    QVERIFY2(client.waitForConnected(), qPrintable(client.errorString()));
    QTRY_COMPARE(server.clientCount(), 1);

    const auto request = [](const int id) {
      return frame({{QStringLiteral("id"), id},
                    {QStringLiteral("protocolMajor"), kIpcProtocolMajor},
                    {QStringLiteral("method"), QStringLiteral("close")},
                    {QStringLiteral("params"), QJsonObject{}}});
    };
    // Both frames are buffered by one readyRead callback. The first handler
    // clears the server's socket map; processing must stop without touching the
    // invalidated buffer iterator or attempting to route the second frame.
    client.write(request(1) + request(2));
    client.flush();

    QTRY_VERIFY(!server.isListening());
    QTRY_COMPARE(server.clientCount(), 0);
  }

  void clientNotification() {
    RequestRouter router;
    router.registerHandler(QStringLiteral("system.subscribe"), subscribeHandler);
    IpcServer server(&router);
    QString path = uniqueSocketPath();
    QString error;
    QVERIFY(server.listen(path, &error));

    IpcClient client;
    QSignalSpy notifications(&client, &IpcClient::notificationReceived);
    client.connectTo(path);
    QTRY_COMPARE(server.clientCount(), 1);
    QTRY_VERIFY(client.isConnected());

    // Ordinary request/response traffic works before subscription, while
    // asynchronous domain notifications do not leak to a new connection.
    server.broadcast(QStringLiteral("refresh"),
                     QJsonObject{{QStringLiteral("state"), false}});
    QTest::qWait(50);
    QCOMPARE(notifications.count(), 0);

    QSignalSpy responses(&client, &IpcClient::responseReceived);
    const QString subscriptionId = client.request(QStringLiteral("system.subscribe"));
    QVERIFY(!subscriptionId.isEmpty());
    QTRY_COMPARE(responses.count(), 1);

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

  void filtersNotificationsByConnectionTopicAndResetsOnReconnect() {
    RequestRouter router;
    router.registerHandler(QStringLiteral("system.subscribe"), subscribeHandler);
    IpcServer server(&router);
    QString error;
    QVERIFY2(server.listen(uniqueSocketPath(), &error), qPrintable(error));

    IpcClient eventClient;
    IpcClient calendarClient;
    eventClient.setAutoReconnect(false);
    calendarClient.setAutoReconnect(false);
    QSignalSpy eventNotifications(&eventClient, &IpcClient::notificationReceived);
    QSignalSpy calendarNotifications(&calendarClient, &IpcClient::notificationReceived);
    QSignalSpy eventResponses(&eventClient, &IpcClient::responseReceived);
    QSignalSpy calendarResponses(&calendarClient, &IpcClient::responseReceived);

    eventClient.connectTo(server.path());
    calendarClient.connectTo(server.path());
    QTRY_COMPARE(server.clientCount(), 2);
    QTRY_VERIFY(eventClient.isConnected());
    QTRY_VERIFY(calendarClient.isConnected());

    QVERIFY(!eventClient
                 .request(
                     QStringLiteral("system.subscribe"),
                     {{QStringLiteral("topics"), QJsonArray{QStringLiteral("events")}}})
                 .isEmpty());
    QVERIFY(!calendarClient
                 .request(QStringLiteral("system.subscribe"),
                          {{QStringLiteral("topics"),
                            QJsonArray{QStringLiteral("calendars.changed")}}})
                 .isEmpty());
    QTRY_COMPARE(eventResponses.count(), 1);
    QTRY_COMPARE(calendarResponses.count(), 1);

    server.broadcast(QStringLiteral("events.changed"),
                     {{QStringLiteral("revision"), 2}});
    QTRY_COMPARE(eventNotifications.count(), 1);
    QTest::qWait(50);
    QCOMPARE(calendarNotifications.count(), 0);

    server.broadcast(QStringLiteral("calendars.changed"),
                     {{QStringLiteral("revision"), 3}});
    QTRY_COMPARE(calendarNotifications.count(), 1);
    QTest::qWait(50);
    QCOMPARE(eventNotifications.count(), 1);

    // Re-subscribing replaces the old topic set rather than accumulating it.
    QVERIFY(!eventClient
                 .request(QStringLiteral("system.subscribe"),
                          {{QStringLiteral("topics"),
                            QJsonArray{QStringLiteral("calendars.*")}}})
                 .isEmpty());
    QTRY_COMPARE(eventResponses.count(), 2);
    server.broadcast(QStringLiteral("events.changed"),
                     {{QStringLiteral("revision"), 4}});
    QTest::qWait(50);
    QCOMPARE(eventNotifications.count(), 1);
    server.broadcast(QStringLiteral("calendars.changed"),
                     {{QStringLiteral("revision"), 5}});
    QTRY_COMPARE(eventNotifications.count(), 2);

    // A reconnect is a new subscription identity. No topic state survives the
    // old local-socket connection.
    eventClient.disconnectFromServer();
    QTRY_VERIFY(!eventClient.isConnected());
    QTRY_COMPARE(server.clientCount(), 1);
    eventClient.connectTo(server.path());
    QTRY_VERIFY(eventClient.isConnected());
    QTRY_COMPARE(server.clientCount(), 2);
    server.broadcast(QStringLiteral("calendars.changed"),
                     {{QStringLiteral("revision"), 6}});
    QTest::qWait(50);
    QCOMPARE(eventNotifications.count(), 2);

    eventClient.disconnectFromServer();
    calendarClient.disconnectFromServer();
    server.close();
  }
};

QTEST_MAIN(IpcTest)
#include "test_ipc.moc"
