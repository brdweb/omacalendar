#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <functional>

#include "core/database.h"

class QDBusInterface;

namespace omacalendar {

struct CalendarNotification {
  QString fingerprint;
  QString deliveryToken;
  QString summary;
  QString body;
  QStringList actions;
  QVariantMap hints;
  int timeoutMilliseconds = -1;
};

class NotificationBackend : public QObject {
  Q_OBJECT

 public:
  using QObject::QObject;
  ~NotificationBackend() override = default;

  virtual void send(const CalendarNotification& notification) = 0;

 signals:
  void notificationResult(const QString& fingerprint, const QString& deliveryToken,
                          uint notificationId, const QString& errorMessage);
  void actionInvoked(uint notificationId, const QString& action);
};

class FreedesktopNotificationBackend final : public NotificationBackend {
  Q_OBJECT

 public:
  explicit FreedesktopNotificationBackend(QObject* parent = nullptr);
  ~FreedesktopNotificationBackend() override;

  void send(const CalendarNotification& notification) override;

 private slots:
  void onActionInvoked(uint notificationId, const QString& action);

 private:
  QDBusInterface* m_interface = nullptr;
};

class ReminderScheduler final : public QObject {
  Q_OBJECT

 public:
  using NowProvider = std::function<QDateTime()>;
  using LinkOpener = std::function<bool(const QUrl&)>;

  explicit ReminderScheduler(Database* database, QObject* parent = nullptr);
  ReminderScheduler(Database* database, NotificationBackend* backend,
                    NowProvider nowProvider, LinkOpener linkOpener,
                    QObject* parent = nullptr);

  void start();
  void stop();
  void checkNow();
  void eventsChanged(const QStringList& calendarIds);
  void handlePrepareForSleep(bool sleeping);

 signals:
  void reminderStateChanged();
  void notificationError(const QString& fingerprint, const QString& message);

 private slots:
  void onNotificationResult(const QString& fingerprint, const QString& deliveryToken,
                            uint notificationId, const QString& errorMessage);
  void onActionInvoked(uint notificationId, const QString& action);
  void onPrepareForSleep(bool sleeping);

 private:
  enum class DeliveryKind { Reminder, Invitation };
  struct PendingDelivery {
    DeliveryKind kind = DeliveryKind::Reminder;
    qint64 reminderId = 0;
    QString eventId;
    QString occurrenceId;
    QString fingerprint;
    QString deliveryToken;
    QDateTime leaseExpiresAt;
  };

  [[nodiscard]] QDateTime now() const;
  void initializeBackend();
  void connectSystemSleep();
  void deliver(const ReminderJob& reminder);
  void baselineInvitations(const QStringList& calendarIds);
  void scanInvitations(const QStringList& calendarIds);
  void deliverInvitation(const Event& event, bool changed, const QString& fingerprint);
  [[nodiscard]] CalendarNotification reminderNotification(
      const Event& event, const ReminderJob& reminder) const;
  [[nodiscard]] CalendarNotification invitationNotification(
      const Event& event, bool changed, const QString& fingerprint) const;
  [[nodiscard]] QUrl eventDeepLink(const PendingDelivery& delivery) const;

  Database* m_database = nullptr;
  NotificationBackend* m_backend = nullptr;
  NowProvider m_nowProvider;
  LinkOpener m_linkOpener;
  QTimer m_timer;
  QHash<QString, PendingDelivery> m_pending;
  QHash<uint, PendingDelivery> m_active;
  QHash<QString, QString> m_invitationBaseline;
  QStringList m_deferredCalendarIds;
  bool m_sleeping = false;
};

}  // namespace omacalendar
