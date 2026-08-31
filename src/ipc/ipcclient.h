#pragma once

#include <QHash>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>

namespace omacalendar::ipc {

class IpcClient final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)

 public:
  explicit IpcClient(QObject* parent = nullptr);
  ~IpcClient() override;

  void connectTo(const QString& path);
  void disconnectFromServer();
  void setAutoReconnect(bool enabled);
  [[nodiscard]] bool isConnected() const;
  [[nodiscard]] QString path() const;

  Q_INVOKABLE QString request(const QString& method, const QJsonObject& params = {});

 signals:
  void connectedChanged();
  void responseReceived(const QString& id, const QJsonValue& result);
  void errorReceived(const QString& id, const QJsonObject& error);
  void notificationReceived(const QString& event, const QJsonObject& data);
  void protocolError(const QString& message);

 private:
  void read();
  void process(const QJsonObject& message);
  void scheduleReconnect();

  QLocalSocket m_socket;
  QTimer m_reconnectTimer;
  QByteArray m_buffer;
  QString m_path;
  bool m_autoReconnect = false;
  bool m_disconnectRequested = false;
};

}  // namespace omacalendar::ipc
