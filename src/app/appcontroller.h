#pragma once

#include <QDate>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <functional>

#include "ipc/ipcclient.h"

namespace omacalendar {

class AppController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
  Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
  Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
  Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)
  Q_PROPERTY(QVariantList calendars READ calendars NOTIFY calendarsChanged)
  Q_PROPERTY(QVariantList events READ events NOTIFY eventsChanged)
  Q_PROPERTY(QDate selectedDate READ selectedDate WRITE setSelectedDate NOTIFY
                 selectedDateChanged)

 public:
  explicit AppController(QObject* parent = nullptr);

  [[nodiscard]] bool connected() const;
  [[nodiscard]] bool busy() const;
  [[nodiscard]] QString statusText() const;
  [[nodiscard]] QString lastError() const;
  [[nodiscard]] QVariantList accounts() const;
  [[nodiscard]] QVariantList calendars() const;
  [[nodiscard]] QVariantList events() const;
  [[nodiscard]] QDate selectedDate() const;

  Q_INVOKABLE void refresh();
  Q_INVOKABLE void loadRange(const QDate& firstDate, const QDate& lastDate);
  Q_INVOKABLE void createEvent(const QVariantMap& event);
  Q_INVOKABLE void updateEvent(const QVariantMap& event);
  Q_INVOKABLE void removeEvent(const QString& eventId);
  Q_INVOKABLE void connectGoogle(const QString& displayName = {});
  Q_INVOKABLE void connectGoogleWithCredentials(const QUrl& credentialsFile,
                                                const QString& displayName = {});
  Q_INVOKABLE void addCalDavAccount(const QString& endpoint, const QString& username,
                                    const QString& password,
                                    const QString& displayName = {});
  Q_INVOKABLE void removeAccount(const QString& accountId);
  Q_INVOKABLE void syncAll();
  Q_INVOKABLE void setSelectedDate(const QDate& date);
  Q_INVOKABLE void reconnect();

 signals:
  void connectedChanged();
  void busyChanged();
  void statusTextChanged();
  void lastErrorChanged();
  void accountsChanged();
  void calendarsChanged();
  void eventsChanged();
  void selectedDateChanged();
  void eventSaved();
  void accountSetupStarted();

 private:
  using ResultHandler = std::function<void(const QJsonValue&)>;

  QString send(const QString& method, const QJsonObject& params,
               ResultHandler handler = {});
  void setError(const QString& message);
  void setStatus(const QString& message);
  void setBusy(bool busy);
  void startDaemonIfNeeded();

  ipc::IpcClient m_client;
  QHash<QString, ResultHandler> m_pending;
  QVariantList m_accounts;
  QVariantList m_calendars;
  QVariantList m_events;
  QDate m_selectedDate = QDate::currentDate();
  QDate m_rangeStart;
  QDate m_rangeEnd;
  QString m_statusText = QStringLiteral("Connecting to calendar service…");
  QString m_lastError;
  int m_activeRequests = 0;
  bool m_daemonStartAttempted = false;
};

}  // namespace omacalendar
