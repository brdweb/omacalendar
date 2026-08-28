#include "ipc/ipcserver.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>

#include "ipc/ipcprotocol.h"

namespace omacalendar::ipc {

IpcServer::IpcServer(RequestRouter* router, QObject* parent)
    : QObject(parent), m_router(router) {
  connect(&m_server, &QLocalServer::newConnection, this, &IpcServer::acceptConnections);
}

IpcServer::~IpcServer() { close(); }

bool IpcServer::listen(const QString& path, QString* errorMessage) {
  close();

  QLocalSocket probe;
  probe.connectToServer(path);
  if (probe.waitForConnected(150)) {
    probe.disconnectFromServer();
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Another OmaCalendar daemon is running");
    }
    return false;
  }
  QLocalServer::removeServer(path);
  m_server.setSocketOptions(QLocalServer::UserAccessOption);
  if (!m_server.listen(path)) {
    if (errorMessage != nullptr) {
      *errorMessage = m_server.errorString();
    }
    return false;
  }
  m_path = path;
  return true;
}

void IpcServer::close() {
  const auto sockets = m_buffers.keys();
  for (QLocalSocket* socket : sockets) {
    socket->disconnect(this);
    socket->disconnectFromServer();
    socket->deleteLater();
  }
  m_buffers.clear();
  if (m_server.isListening()) {
    m_server.close();
  }
  if (!m_path.isEmpty()) {
    QLocalServer::removeServer(m_path);
    m_path.clear();
  }
}

bool IpcServer::isListening() const { return m_server.isListening(); }

QString IpcServer::path() const { return m_path; }

int IpcServer::clientCount() const { return static_cast<int>(m_buffers.size()); }

void IpcServer::acceptConnections() {
  while (m_server.hasPendingConnections()) {
    attach(m_server.nextPendingConnection());
  }
}

void IpcServer::attach(QLocalSocket* socket) {
  if (socket == nullptr) {
    return;
  }
  m_buffers.insert(socket, {});
  connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { read(socket); });
  connect(socket, &QLocalSocket::disconnected, this,
          [this, socket]() { detach(socket); });
  emit clientCountChanged();
}

void IpcServer::read(QLocalSocket* socket) {
  auto iterator = m_buffers.find(socket);
  if (iterator == m_buffers.end()) {
    return;
  }
  iterator.value().append(socket->readAll());
  if (iterator.value().size() > kMaximumFrameBytes) {
    emit protocolViolation(QStringLiteral("Client frame exceeded size limit"));
    socket->disconnectFromServer();
    return;
  }

  qsizetype newline = -1;
  while ((newline = iterator.value().indexOf('\n')) >= 0) {
    const QByteArray raw = iterator.value().left(newline);
    iterator.value().remove(0, newline + 1);
    if (raw.trimmed().isEmpty()) {
      continue;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      send(socket, errorResponse(QJsonValue::Null,
                                 {QStringLiteral("parse_error"),
                                  QStringLiteral("Request is not valid JSON"), false}));
      continue;
    }
    send(socket, m_router->route(document.object()));
  }
}

void IpcServer::detach(QLocalSocket* socket) {
  if (m_buffers.remove(socket) > 0) {
    emit clientCountChanged();
  }
  socket->deleteLater();
}

void IpcServer::send(QLocalSocket* socket, const QJsonObject& message) {
  const QByteArray bytes = frame(message);
  if (bytes.size() > kMaximumFrameBytes) {
    socket->write(frame(errorResponse(
        message.value(QStringLiteral("id")),
        {QStringLiteral("response_too_large"),
         QStringLiteral("Response must be requested in smaller pages"), false})));
    return;
  }
  socket->write(bytes);
  socket->flush();
}

void IpcServer::broadcast(const QString& event, const QJsonObject& data) {
  // A client can finish its local connect before Qt has delivered the server's
  // newConnection signal. Drain that queue so an immediate first broadcast is
  // not lost in the connection handshake race.
  acceptConnections();
  const QByteArray bytes = frame(notification(event, data));
  for (QLocalSocket* socket : m_buffers.keys()) {
    socket->write(bytes);
    socket->flush();
  }
}

}  // namespace omacalendar::ipc
