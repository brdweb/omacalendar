#include "applicationinstance.h"

#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>

namespace omacalendar {
namespace {

constexpr auto kAck = "ok\n";
constexpr qsizetype kMaximumRequestBytes = 64 * 1024;

}  // namespace

ApplicationInstance::ApplicationInstance(QObject* parent)
    : QObject(parent),
      m_serverPath(
          QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation))
              .filePath(QStringLiteral("omacalendar/app-instance.sock"))) {
  connect(&m_server, &QLocalServer::newConnection, this,
          &ApplicationInstance::acceptConnections);
}

ApplicationInstance::~ApplicationInstance() {
  if (!m_primary) {
    return;
  }
  m_server.close();
  QLocalServer::removeServer(m_serverPath);
}

bool ApplicationInstance::claimPrimary() {
  const QFileInfo socketInfo(m_serverPath);
  if (!QDir().mkpath(socketInfo.absolutePath())) {
    return false;
  }

  m_lock = std::make_unique<QLockFile>(m_serverPath + QStringLiteral(".lock"));
  m_lock->setStaleLockTime(0);
  if (!m_lock->tryLock()) {
    return false;
  }

  // Owning the lock proves no live primary owns this endpoint.
  QLocalServer::removeServer(m_serverPath);
  if (!m_server.listen(m_serverPath)) {
    m_lock->unlock();
    m_lock.reset();
    return false;
  }

  m_primary = true;
  return true;
}

bool ApplicationInstance::forwardToPrimary(const QString& startupArgument) const {
  QLocalSocket socket;
  for (int attempt = 0; attempt < 20; ++attempt) {
    socket.connectToServer(m_serverPath, QIODevice::ReadWrite);
    if (socket.waitForConnected(100)) {
      break;
    }
    socket.abort();
    QThread::msleep(25);
  }
  if (socket.state() != QLocalSocket::ConnectedState) {
    return false;
  }
  socket.write(startupArgument.toUtf8());
  socket.write("\n");
  if (!socket.waitForBytesWritten(1000) || !socket.waitForReadyRead(1000)) {
    return false;
  }
  return socket.readAll().startsWith(kAck);
}

bool ApplicationInstance::activate(const QString& startupArgument) {
  emit activationRequested(startupArgument);
  return true;
}

void ApplicationInstance::acceptConnections() {
  while (m_server.hasPendingConnections()) {
    QLocalSocket* socket = m_server.nextPendingConnection();
    if (!socket) {
      continue;
    }
    connect(socket, &QLocalSocket::readyRead, socket, [this, socket]() {
      QByteArray request = socket->property("requestBuffer").toByteArray();
      request.append(socket->readAll());
      if (request.size() > kMaximumRequestBytes) {
        socket->disconnectFromServer();
        return;
      }
      const qsizetype newline = request.indexOf('\n');
      if (newline < 0) {
        socket->setProperty("requestBuffer", request);
        return;
      }
      activate(QString::fromUtf8(request.first(newline)));
      socket->write(kAck);
      socket->flush();
      socket->disconnectFromServer();
    });
    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
  }
}

}  // namespace omacalendar
