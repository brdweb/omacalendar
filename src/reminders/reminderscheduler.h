#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
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
  // From GetCapabilities; servers without "actions" get only the default
  // action, which clicking the notification still triggers.
  bool m_supportsActions = true;
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
  void syncCompleted(const QString& accountId);
  void handlePrepareForSleep(bool sleeping);
  // Rebases pending all-day reminders when the system time zone differs from
  // the one they were scheduled under. Runs on start and on every tick.
  void checkTimeZone();
  // Starts a chunked pass that re-expands long-lived series' reminders, unless
  // one completed within the last kHorizonRefreshDays. Runs a minute after
  // start and then daily.
  void refreshReminderHorizons();

  static constexpr int kHorizonRefreshDays = 7;
  static constexpr int kHorizonChunkSize = 100;

 signals:
  void reminderStateChanged();
  void notificationError(const QString& fingerprint, const QString& message);

 private slots:
  void onNotificationResult(const QString& fingerprint, const QString& deliveryToken,
                            uint notificationId, const QString& errorMessage);
  void onActionInvoked(uint notificationId, const QString& action);
  void onPrepareForSleep(bool sleeping);

 private:
  enum class DeliveryKind { Reminder, Invitation, InvitationDigest };
  struct PendingDelivery {
    DeliveryKind kind = DeliveryKind::Reminder;
    qint64 reminderId = 0;
    QString eventId;
    QString occurrenceId;
    QString fingerprint;
    QString deliveryToken;
    QDateTime leaseExpiresAt;
    // Reminders only: the validated meeting link behind "Join", and the
    // occurrence start behind "Snooze until start".
    QUrl joinUrl;
    QDateTime occurrenceStart;
    // Digest members only: per-event fingerprints claimed for the digest.
    QStringList memberFingerprints;
    QStringList memberEventIds;
  };

  [[nodiscard]] QDateTime now() const;
  void initializeBackend();
  void connectSystemSleep();
  [[nodiscard]] bool deliver(const ReminderJob& reminder);
  [[nodiscard]] bool baselineInvitations(const QStringList& calendarIds);
  void scanInvitations(const QStringList& calendarIds);
  void deliverInvitation(const Event& event, bool changed, const QString& fingerprint);
  [[nodiscard]] CalendarNotification reminderNotification(
      const Event& event, const ReminderJob& reminder, const QUrl& joinUrl,
      const QDateTime& occurrenceStart) const;
  [[nodiscard]] CalendarNotification invitationNotification(
      const Event& event, bool changed, const QString& fingerprint) const;
  [[nodiscard]] CalendarNotification invitationDigestNotification(
      const QList<Event>& events, bool changedOnly) const;
  [[nodiscard]] bool invitationEndedBeforeCutoff(const Event& event,
                                                 const QDateTime& current) const;
  [[nodiscard]] bool finishDigestMembers(const PendingDelivery& digest,
                                         QString* errorMessage);
  [[nodiscard]] QUrl eventDeepLink(const PendingDelivery& delivery) const;
  void extendHorizonChunk();

  Database* m_database = nullptr;
  NotificationBackend* m_backend = nullptr;
  NowProvider m_nowProvider;
  LinkOpener m_linkOpener;
  QTimer m_timer;
  QTimer m_horizonTimer;
  QString m_horizonCursor;
  bool m_horizonRunning = false;
  QByteArray m_reminderTimeZone;
  QHash<QString, PendingDelivery> m_pending;
  QHash<uint, PendingDelivery> m_active;
  QHash<QString, QString> m_invitationBaseline;
  QSet<QString> m_initializedInvitationCalendars;
  QStringList m_deferredCalendarIds;
  bool m_sleeping = false;
};

}  // namespace omacalendar
