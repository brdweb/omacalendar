#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>

#include "core/database.h"
#include "providers/google/googleauth.h"
#include "providers/google/googleclient.h"
#include "sync/provider.h"
#include "sync/retrypolicy.h"

namespace omacalendar::google {

// Returns cached provider identities that were inside a completed full-sync
// window but were not present in the replacement result. Kept as a pure helper
// so the destructive part of a rebuild can be contract-tested independently.
[[nodiscard]] QStringList googleFullSyncPruneCandidates(
    const QList<Event>& cachedEvents, const QSet<QString>& retainedRemoteIds);

class GoogleSync final : public Provider {
  Q_OBJECT

 public:
  explicit GoogleSync(Database* database, QObject* parent = nullptr);
  ~GoogleSync() override;

  bool loadConfiguration(QString* errorMessage = nullptr);
  bool configureClient(const QString& clientId, const QString& clientSecret,
                       QString* errorMessage = nullptr);
  [[nodiscard]] bool isConfigured() const;

  QString beginAuthorization(const QString& displayName,
                             QString* errorMessage = nullptr);
  bool reauthorizeAccount(const QString& accountId, QString* errorMessage = nullptr);
  bool restoreAccounts(QString* errorMessage = nullptr);
  void cancelAuthorization(const QString& accountId);
  bool disconnectAccount(const QString& accountId, bool removeCachedData,
                         QString* errorMessage = nullptr);
  bool deleteCalendar(
      const Calendar& calendar,
      std::function<void(bool, const QString&, const QString&)> callback,
      QString* errorMessage = nullptr);

  [[nodiscard]] ProviderCapabilities capabilities() const override;
  void syncAll() override;
  void syncAccount(const QString& accountId) override;
  bool syncRange(const RangeSyncRequest& request,
                 QString* errorMessage = nullptr) override;
  [[nodiscard]] QJsonObject status(const QString& accountId = {}) const override;

 signals:
  void authorizationUrlReady(const QString& accountId, const QUrl& url);

 private:
  struct SyncJob;
  struct PendingDisconnect;

  [[nodiscard]] SyncJob* activeJob(const QString& accountId, quint64 generation) const;
  bool refreshAndResume(SyncJob* job, std::function<void(SyncJob*)> resume,
                        QString* errorMessage = nullptr);
  bool retryReadRequest(SyncJob* job, const ApiResponse& response,
                        const QString& operationKey,
                        std::function<void(SyncJob*)> resume);
  void scheduleRetryWake(const QString& accountId, const QDateTime& wakeAt);
  void requireReauthorization(SyncJob* job, const QString& errorCode,
                              const QString& errorMessage);
  void setAccountAuthStatus(const QString& accountId, const QString& status);
  void startCalendarList(SyncJob* job, const QString& pageToken = {});
  void syncNextCalendar(SyncJob* job);
  void syncNextHydration(SyncJob* job);
  void startEventPage(SyncJob* job, const QString& pageToken = {});
  void drainOutbox(SyncJob* job);
  void dispatchNextOutbox(SyncJob* job);
  void resolveOccurrenceAndDispatch(SyncJob* job, const OutboxItem& item,
                                    const Event& event, const Calendar& calendar,
                                    const QString& recurringEventRemoteId,
                                    const QString& pageToken = {});
  void resolveSeriesAndDispatch(SyncJob* job, const OutboxItem& item,
                                const Event& event, const Calendar& calendar,
                                const QString& recurringEventRemoteId);
  void dispatchMutation(SyncJob* job, const OutboxItem& item, const Event& event,
                        const Calendar& calendar, const QString& remoteId);
  void handleMoveHydration(SyncJob* job, const OutboxItem& item,
                           const Event& desiredEvent, const ApiResponse& response);
  void finish(SyncJob* job, const QString& errorCode = {},
              const QString& errorMessage = {});
  void handleMutationResult(SyncJob* job, const OutboxItem& item,
                            const Event& localEvent, const ApiResponse& response,
                            bool allowCreateReconciliation = true,
                            bool allowConflictHydration = true);
  void finishDisconnect(const QString& accountId, bool secretRemoved,
                        const QString& errorCode, const QString& errorMessage);

  Database* m_database = nullptr;
  GoogleAuthManager m_auth;
  GoogleClient m_client;
  RetryPolicy m_retryPolicy;
  QTimer m_pollTimer;
  QHash<QString, SyncJob*> m_jobs;
  QHash<QString, PendingDisconnect*> m_pendingDisconnects;
  QHash<QString, QJsonObject> m_status;
  QHash<QString, QDateTime> m_retryWakeAt;
  QHash<QString, QList<RangeSyncRequest>> m_pendingHydrations;
  QSet<QString> m_provisionalAuthorizationIds;
  QHash<QString, Account> m_reauthorizationSnapshots;
  quint64 m_nextJobGeneration = 0;
  bool m_shuttingDown = false;
};

}  // namespace omacalendar::google
