#include "ipc/ipcclient.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>

#include "core/domain.h"
#include "ipc/ipcprotocol.h"

namespace omacalendar::ipc {

IpcClient::IpcClient(QObject* parent) : QObject(parent) {
  m_reconnectTimer.setSingleShot(true);
  m_reconnectTimer.setInterval(1000);
  connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() { connectTo(m_path); });
  connect(&m_socket, &QLocalSocket::connected, this,
          [this]() { emit connectedChanged(); });
  connect(&m_socket, &QLocalSocket::disconnected, this, [this]() {
    emit connectedChanged();
    scheduleReconnect();
  });
  connect(&m_socket, &QLocalSocket::readyRead, this, &IpcClient::read);
  connect(&m_socket, &QLocalSocket::errorOccurred, this,
          [this](QLocalSocket::LocalSocketError) { scheduleReconnect(); });
}

void IpcClient::connectTo(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  m_path = path;
  m_disconnectRequested = false;
  if (m_socket.state() != QLocalSocket::UnconnectedState) {
    m_socket.abort();
  }
  m_socket.connectToServer(path);
}

void IpcClient::disconnectFromServer() {
  m_disconnectRequested = true;
  m_reconnectTimer.stop();
  m_socket.disconnectFromServer();
}

void IpcClient::setAutoReconnect(const bool enabled) {
  m_autoReconnect = enabled;
  if (!enabled) {
    m_reconnectTimer.stop();
  }
}

bool IpcClient::isConnected() const {
  return m_socket.state() == QLocalSocket::ConnectedState;
}

QString IpcClient::path() const { return m_path; }

QString IpcClient::request(const QString& method, const QJsonObject& params) {
  if (!isConnected()) {
    return {};
  }
  const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QJsonObject object = {
      {QStringLiteral("id"), id},
      {QStringLiteral("protocolMajor"), kIpcProtocolMajor},
      {QStringLiteral("method"), method},
      {QStringLiteral("params"), params},
  };
  m_socket.write(frame(object));
  m_socket.flush();
  return id;
}

void IpcClient::read() {
  m_buffer.append(m_socket.readAll());
  if (m_buffer.size() > kMaximumFrameBytes) {
    emit protocolError(QStringLiteral("Server frame exceeded size limit"));
    m_buffer.clear();
    m_socket.disconnectFromServer();
    return;
  }
  qsizetype newline = -1;
  while ((newline = m_buffer.indexOf('\n')) >= 0) {
    const QByteArray raw = m_buffer.left(newline);
    m_buffer.remove(0, newline + 1);
    if (raw.trimmed().isEmpty()) {
      continue;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      emit protocolError(QStringLiteral("Server response is not valid JSON"));
      continue;
    }
    process(document.object());
  }
}

void IpcClient::process(const QJsonObject& message) {
  if (message.contains(QStringLiteral("event"))) {
    emit notificationReceived(message.value(QStringLiteral("event")).toString(),
                              message.value(QStringLiteral("data")).toObject());
    return;
  }
  const QString id = message.value(QStringLiteral("id")).toString();
  if (message.contains(QStringLiteral("error"))) {
    emit errorReceived(id, message.value(QStringLiteral("error")).toObject());
    return;
  }
  emit responseReceived(id, message.value(QStringLiteral("result")));
}

void IpcClient::scheduleReconnect() {
  if (m_autoReconnect && !m_disconnectRequested && !m_path.isEmpty() &&
      !m_reconnectTimer.isActive()) {
    m_reconnectTimer.start();
  }
}

}  // namespace omacalendar::ipc
