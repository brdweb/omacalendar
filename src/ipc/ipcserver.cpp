#include "ipc/ipcserver.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QPointer>
#include <QSocketNotifier>
#include <cerrno>
#include <utility>

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
  m_ownsEndpoint = true;
  // QLocalServer accepts both filesystem paths and portable server names.  On
  // Unix a portable name is expanded to a path (or an abstract socket), so
  // chmod the actual endpoint instead of the caller-facing name.  Abstract
  // sockets have no inode; UserAccessOption above is their access control.
  const QString endpoint = m_server.fullServerName();
  const QFileInfo endpointInfo(endpoint);
  if (endpointInfo.exists() &&
      !QFile::setPermissions(endpoint,
                             QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
    close();
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Could not restrict daemon socket permissions");
    }
    return false;
  }
  return true;
}

bool IpcServer::listen(const qintptr socketDescriptor, const QString& path,
                       QString* errorMessage) {
  close();
  if (socketDescriptor < 0) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Invalid inherited daemon socket");
    }
    return false;
  }
  int accepting = 0;
  socklen_t acceptingSize = sizeof(accepting);
  if (::getsockopt(static_cast<int>(socketDescriptor), SOL_SOCKET, SO_ACCEPTCONN,
                   &accepting, &acceptingSize) != 0 ||
      accepting == 0) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Inherited descriptor is not a listening socket");
    }
    return false;
  }
  const int descriptor = static_cast<int>(socketDescriptor);
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Could not configure inherited daemon socket");
    }
    return false;
  }
  m_inheritedDescriptor = socketDescriptor;
  m_inheritedNotifier =
      new QSocketNotifier(socketDescriptor, QSocketNotifier::Read, this);
  connect(m_inheritedNotifier, &QSocketNotifier::activated, this,
          &IpcServer::acceptInheritedConnections);
  m_path = path;
  m_ownsEndpoint = false;
  return true;
}

void IpcServer::close() {
  const auto sockets = m_clients.keys();
  for (QLocalSocket* socket : sockets) {
    socket->disconnect(this);
    socket->disconnectFromServer();
    socket->deleteLater();
  }
  m_clients.clear();
  if (m_inheritedNotifier != nullptr) {
    m_inheritedNotifier->setEnabled(false);
    delete m_inheritedNotifier;
    m_inheritedNotifier = nullptr;
  }
  if (m_inheritedDescriptor >= 0) {
    ::close(static_cast<int>(m_inheritedDescriptor));
    m_inheritedDescriptor = -1;
  }
  if (m_server.isListening()) {
    m_server.close();
  }
  if (m_ownsEndpoint && !m_path.isEmpty()) {
    QLocalServer::removeServer(m_path);
  }
  m_path.clear();
  m_ownsEndpoint = false;
}

bool IpcServer::isListening() const {
  return m_server.isListening() || m_inheritedDescriptor >= 0;
}

QString IpcServer::path() const { return m_path; }

int IpcServer::clientCount() const { return static_cast<int>(m_clients.size()); }

void IpcServer::acceptConnections() {
  acceptInheritedConnections();
  while (m_server.hasPendingConnections()) {
    attach(m_server.nextPendingConnection());
  }
}

void IpcServer::acceptInheritedConnections() {
  if (m_inheritedDescriptor < 0) {
    return;
  }
  while (true) {
    const int descriptor = ::accept4(static_cast<int>(m_inheritedDescriptor), nullptr,
                                     nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (descriptor < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    auto* socket = new QLocalSocket(this);
    if (!socket->setSocketDescriptor(descriptor, QLocalSocket::ConnectedState,
                                     QIODevice::ReadWrite)) {
      delete socket;
      ::close(descriptor);
      continue;
    }
    attach(socket);
  }
}

void IpcServer::attach(QLocalSocket* socket) {
  if (socket == nullptr) {
    return;
  }
  m_clients.insert(socket, {});
  connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { read(socket); });
  connect(socket, &QLocalSocket::disconnected, this,
          [this, socket]() { detach(socket); });
  emit clientCountChanged();
}

void IpcServer::read(QLocalSocket* socket) {
  QPointer<QLocalSocket> socketGuard(socket);
  QList<QByteArray> frames;
  bool oversizedFrame = false;
  {
    auto iterator = m_clients.find(socket);
    if (iterator == m_clients.end()) {
      return;
    }
    QByteArray& buffer = iterator.value().buffer;
    buffer.append(socket->readAll());

    // Extract complete frames before routing any of them. Handlers can emit
    // signals, close the server, or otherwise re-enter Qt's socket event loop;
    // retaining a QHash iterator across that boundary makes a disconnect erase
    // the iterator while this function is still using it.
    qsizetype newline = -1;
    while ((newline = buffer.indexOf('\n')) >= 0) {
      if (newline > kMaximumFrameBytes) {
        oversizedFrame = true;
        buffer.clear();
        break;
      }
      frames.append(buffer.left(newline));
      buffer.remove(0, newline + 1);
    }
    if (!oversizedFrame && buffer.size() > kMaximumFrameBytes) {
      oversizedFrame = true;
      buffer.clear();
    }
  }

  if (oversizedFrame) {
    emit protocolViolation(QStringLiteral("Client frame exceeded size limit"));
    if (!socketGuard.isNull() && m_clients.contains(socket)) {
      socket->disconnectFromServer();
    }
    return;
  }

  for (const QByteArray& raw : std::as_const(frames)) {
    if (socketGuard.isNull() || !m_clients.contains(socket)) {
      return;
    }
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
    const QJsonObject request = document.object();
    const QJsonObject response = m_router->route(request);
    if (socketGuard.isNull() || !m_clients.contains(socket)) {
      return;
    }
    applySubscription(socket, request, response);
    send(socket, response);
  }
}

void IpcServer::detach(QLocalSocket* socket) {
  if (m_clients.remove(socket) > 0) {
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
  for (QLocalSocket* socket : m_clients.keys()) {
    const auto iterator = m_clients.constFind(socket);
    if (iterator == m_clients.constEnd() || !iterator->subscribed ||
        !topicMatches(iterator->topics, event)) {
      continue;
    }
    socket->write(bytes);
    socket->flush();
  }
}

void IpcServer::applySubscription(QLocalSocket* socket, const QJsonObject& request,
                                  const QJsonObject& response) {
  if (request.value(QStringLiteral("method")).toString() !=
          QStringLiteral("system.subscribe") ||
      response.contains(QStringLiteral("error"))) {
    return;
  }
  const QJsonObject result = response.value(QStringLiteral("result")).toObject();
  if (!result.value(QStringLiteral("subscribed")).toBool()) {
    return;
  }

  QSet<QString> topics;
  QJsonValue topicsValue = result.value(QStringLiteral("topics"));
  if (!topicsValue.isArray()) {
    topicsValue = request.value(QStringLiteral("params"))
                      .toObject()
                      .value(QStringLiteral("topics"));
  }
  if (topicsValue.isArray()) {
    for (const QJsonValue& topic : topicsValue.toArray()) {
      const QString value = topic.toString().trimmed();
      if (!value.isEmpty()) {
        topics.insert(value);
      }
    }
  } else {
    // IPC 2.0 originally documented topics as optional. Treat omission as a
    // wildcard so existing clients retain live updates while explicit topic
    // lists receive the isolation they requested.
    topics.insert(QStringLiteral("*"));
  }

  auto iterator = m_clients.find(socket);
  if (iterator == m_clients.end()) {
    return;
  }
  iterator->topics = std::move(topics);
  iterator->subscribed = true;
}

bool IpcServer::topicMatches(const QSet<QString>& topics, const QString& event) {
  if (topics.contains(QStringLiteral("*")) || topics.contains(event)) {
    return true;
  }
  const qsizetype separator = event.indexOf(QLatin1Char('.'));
  const QString family = separator < 0 ? event : event.left(separator);
  return topics.contains(family) || topics.contains(family + QStringLiteral(".*"));
}

}  // namespace omacalendar::ipc
