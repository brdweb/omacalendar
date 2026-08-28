#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>

#include "core/database.h"
#include "core/secretstore.h"
#include "providers/caldav/caldavclient.h"
#include "sync/retrypolicy.h"

namespace omacalendar::caldav {

class CalDavSync final : public QObject {
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

  void syncAll();
  void syncAccount(const QString& accountId);
  [[nodiscard]] QJsonObject status(const QString& accountId = {}) const;

 signals:
  void accountChanged(const QString& accountId);
  void calendarsChanged(const QString& accountId);
  void eventsChanged(const QStringList& calendarIds);
  void syncStatusChanged(const QString& accountId, const QJsonObject& status);

 private:
  struct SyncJob;

  bool loadCredentials(const Account& account, QString* errorMessage);
  void discoverPrincipal(SyncJob* job);
  void discoverHome(SyncJob* job, const QUrl& principalUrl);
  void discoverCollections(SyncJob* job, const QUrl& homeUrl);
  void syncNextCalendar(SyncJob* job);
  void applyCalendarResponse(SyncJob* job, const DavResponse& response, bool fullSync);
  void drainOutbox(SyncJob* job);
  void dispatchNextOutbox(SyncJob* job);
  void handleMutationResult(SyncJob* job, const OutboxItem& item,
                            const Event& localEvent, const QUrl& resourceUrl,
                            const DavResponse& response);
  void finish(SyncJob* job, const QString& errorCode = {},
              const QString& errorMessage = {});
  void setAccountAuthStatus(const QString& accountId, const QString& status);

  Database* m_database = nullptr;
  SecretStore m_secrets;
  CalDavClient m_client;
  RetryPolicy m_retryPolicy;
  QTimer m_pollTimer;
  QHash<QString, SyncJob*> m_jobs;
  QHash<QString, QJsonObject> m_status;
};

}  // namespace omacalendar::caldav
