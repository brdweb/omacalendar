#pragma once

#include <QHash>
#include <QLocalServer>
#include <QObject>

#include "ipc/requestrouter.h"

class QLocalSocket;

namespace omacalendar::ipc {

class IpcServer final : public QObject {
  Q_OBJECT

 public:
  explicit IpcServer(RequestRouter* router, QObject* parent = nullptr);
  ~IpcServer() override;

  bool listen(const QString& path, QString* errorMessage = nullptr);
  void close();
  [[nodiscard]] bool isListening() const;
  [[nodiscard]] QString path() const;
  [[nodiscard]] int clientCount() const;

  void broadcast(const QString& event, const QJsonObject& data);

 signals:
  void clientCountChanged();
  void protocolViolation(const QString& reason);

 private slots:
  void acceptConnections();

 private:
  void attach(QLocalSocket* socket);
  void read(QLocalSocket* socket);
  void detach(QLocalSocket* socket);
  void send(QLocalSocket* socket, const QJsonObject& message);

  RequestRouter* m_router = nullptr;
  QLocalServer m_server;
  QHash<QLocalSocket*, QByteArray> m_buffers;
  QString m_path;
};

}  // namespace omacalendar::ipc
