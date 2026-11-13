#pragma once

#include <QHash>
#include <QLocalServer>
#include <QObject>
#include <QSet>

#include "ipc/requestrouter.h"

class QLocalSocket;
class QSocketNotifier;

namespace omacalendar::ipc {

class IpcServer final : public QObject {
  Q_OBJECT

 public:
  explicit IpcServer(RequestRouter* router, QObject* parent = nullptr);
  ~IpcServer() override;

  bool listen(const QString& path, QString* errorMessage = nullptr);
  bool listen(qintptr socketDescriptor, const QString& path,
              QString* errorMessage = nullptr);
  void close();
  [[nodiscard]] bool isListening() const;
  [[nodiscard]] QString path() const;
  [[nodiscard]] int clientCount() const;
  // A client that stops reading is disconnected once this many bytes of
  // responses and notifications are queued for it, so one stalled subscriber
  // cannot grow the daemon's memory without bound.
  [[nodiscard]] static constexpr qint64 maximumClientBacklogBytes() {
    return 8 * 1024 * 1024;
  }

  void broadcast(const QString& event, const QJsonObject& data);

 signals:
  void clientCountChanged();
  void protocolViolation(const QString& reason);

 private slots:
  void acceptConnections();
  void acceptInheritedConnections();

 private:
  struct ClientState {
    QByteArray buffer;
    QSet<QString> topics;
    bool subscribed = false;
  };

  void attach(QLocalSocket* socket);
  void read(QLocalSocket* socket);
  void detach(QLocalSocket* socket);
  void send(QLocalSocket* socket, const QJsonObject& message);
  bool writeFrame(QLocalSocket* socket, const QByteArray& bytes);
  void dropClient(QLocalSocket* socket, const QString& reason);
  void applySubscription(QLocalSocket* socket, const QJsonObject& request,
                         const QJsonObject& response);
  [[nodiscard]] static bool topicMatches(const QSet<QString>& topics,
                                         const QString& event);

  RequestRouter* m_router = nullptr;
  QLocalServer m_server;
  QSocketNotifier* m_inheritedNotifier = nullptr;
  qintptr m_inheritedDescriptor = -1;
  QHash<QLocalSocket*, ClientState> m_clients;
  QString m_path;
  bool m_ownsEndpoint = false;
};

}  // namespace omacalendar::ipc
