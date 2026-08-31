#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <memory>

#include "core/database.h"
#include "core/secretstore.h"
#include "providers/caldav/caldavclient.h"
#include "sync/provider.h"
#include "sync/retrypolicy.h"

namespace omacalendar::caldav {

struct CalDavResource;

class CalDavSync final : public Provider {
  Q_OBJECT

 public:
  explicit CalDavSync(Database* database, QObject* parent = nullptr);
  ~CalDavSync() override;

  QString createAccount(const QString& endpoint, const QString& username,
                        const QString& password, const QString& displayName,
                        QString* errorMessage = nullptr);
  bool restoreAccounts(QString* errorMessage = nullptr);
  bool disconnectAccount(const QString& accountId, bool removeCachedData,
                         QString* errorMessage = nullptr);
  bool updateCredentials(const QString& accountId, const QString& username,
                         const QString& password, QString* errorMessage = nullptr);

  [[nodiscard]] ProviderCapabilities capabilities() const override;
  void syncAll() override;
  void syncAccount(const QString& accountId) override;
  bool syncRange(const RangeSyncRequest& request,
                 QString* errorMessage = nullptr) override;
  [[nodiscard]] QJsonObject status(const QString& accountId = {}) const override;

 private:
  struct SyncJob;
  struct FutureCapabilityProbe;

  void loadCredentialsAsync(const Account& account);
  void storeCredentialsAsync(const Account& account, const QString& password);
  void handleCredentialFailure(const QString& accountId, const QString& message);
  void discoverPrincipal(SyncJob* job);
  void discoverHome(SyncJob* job, const QUrl& principalUrl);
  void discoverCollections(SyncJob* job, const QUrl& homeUrl);
  void syncNextCalendar(SyncJob* job);
  void syncNextHydration(SyncJob* job);
  void scheduleRetryWake(const QString& accountId, const QDateTime& wakeAt);
  void syncCalendarWithEtags(SyncJob* job, const QUrl& calendarUrl);
  using ResourceBatchCallback =
      std::function<void(QList<CalDavResource>, QString, QString)>;
  void fetchResourceBatches(SyncJob* job, const QUrl& calendarUrl,
                            const QStringList& hrefs, ResourceBatchCallback callback);
  void applyCalendarResponse(SyncJob* job, const DavResponse& response, bool fullSync);
  void applyCalendarResources(SyncJob* job, const QList<CalDavResource>& resources,
                              const QString& responseSyncToken, bool fullSync);
  void drainOutbox(SyncJob* job);
  void dispatchNextOutbox(SyncJob* job);
  void startFutureCapabilityProbe(SyncJob* job, const OutboxItem& item,
                                  const Calendar& calendar, const QUrl& calendarUrl);
  void probeReadCreated(SyncJob* job, std::shared_ptr<FutureCapabilityProbe> probe,
                        const DavResponse& response);
  void probeReadUpdated(SyncJob* job, std::shared_ptr<FutureCapabilityProbe> probe,
                        const DavResponse& response);
  void finishFutureCapabilityProbe(SyncJob* job,
                                   std::shared_ptr<FutureCapabilityProbe> probe,
                                   bool verified, QString errorCode,
                                   QString errorMessage);
  void dispatchMove(SyncJob* job, const OutboxItem& item, const Event& targetEvent,
                    const Event& sourceEvent, const Calendar& sourceCalendar,
                    const Calendar& targetCalendar, const QByteArray& scheduleReply);
  void startFallbackMove(SyncJob* job, const OutboxItem& item, const Event& targetEvent,
                         const Event& sourceEvent, const QUrl& sourceResourceUrl,
                         const QUrl& targetResourceUrl, const QByteArray& targetPayload,
                         const QByteArray& scheduleReply);
  void reconcileMoveTarget(SyncJob* job, const OutboxItem& item,
                           const Event& targetEvent, const Event& sourceEvent,
                           const QUrl& sourceResourceUrl, const QUrl& targetResourceUrl,
                           const QByteArray& targetPayload,
                           const QByteArray& scheduleReply, bool directMove,
                           const DavResponse& originalResponse);
  void deleteMoveSource(SyncJob* job, const OutboxItem& item, const Event& targetEvent,
                        const QUrl& sourceResourceUrl, const QUrl& targetResourceUrl,
                        const QByteArray& targetPayload,
                        const QByteArray& scheduleReply);
  void hydrateMoveTarget(SyncJob* job, const OutboxItem& item, const Event& targetEvent,
                         const QUrl& targetResourceUrl,
                         const QByteArray& targetPayload);
  void handleMutationResult(SyncJob* job, const OutboxItem& item,
                            const Event& localEvent, const QUrl& resourceUrl,
                            const QByteArray& sentPayload, const DavResponse& response);
  void reconcileAmbiguousCreate(SyncJob* job, const OutboxItem& item,
                                const Event& localEvent, const QUrl& resourceUrl,
                                const QByteArray& sentPayload,
                                const DavResponse& originalResponse);
  void completeSuccessfulMutation(SyncJob* job, const OutboxItem& item,
                                  const Event& localEvent, const QUrl& resourceUrl,
                                  const QByteArray& sentPayload,
                                  const DavResponse& response);
  void finish(SyncJob* job, const QString& errorCode = {},
              const QString& errorMessage = {});
  void setAccountAuthStatus(const QString& accountId, const QString& status);

  Database* m_database = nullptr;
  AsyncSecretStore m_secrets;
  CalDavClient m_client;
  RetryPolicy m_retryPolicy;
  QTimer m_pollTimer;
  QHash<QString, SyncJob*> m_jobs;
  QHash<QString, QJsonObject> m_status;
  QHash<QString, quint64> m_credentialGeneration;
  QHash<QString, SecretStoreOperationId> m_secretOperations;
  QSet<QString> m_loadedCredentials;
  QSet<QString> m_loadingCredentials;
  QSet<QString> m_syncAfterCurrentJob;
  QHash<QString, QList<RangeSyncRequest>> m_pendingHydrations;
  QHash<QString, QDateTime> m_retryWakeAt;
  QHash<QString, quint64> m_retryWakeGeneration;
  quint64 m_nextRetryWakeGeneration = 0;
  bool m_shuttingDown = false;
};

}  // namespace omacalendar::caldav
