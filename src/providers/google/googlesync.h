#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>
#include <functional>

#include "core/database.h"
#include "providers/google/googleauth.h"
#include "providers/google/googleclient.h"
#include "sync/retrypolicy.h"

namespace omacalendar::google {

class GoogleSync final : public QObject {
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
  bool restoreAccounts(QString* errorMessage = nullptr);
  void cancelAuthorization(const QString& accountId);
  bool disconnectAccount(const QString& accountId, bool removeCachedData,
                         QString* errorMessage = nullptr);

  void syncAll();
  void syncAccount(const QString& accountId);
  [[nodiscard]] QJsonObject status(const QString& accountId = {}) const;

 signals:
  void authorizationUrlReady(const QString& accountId, const QUrl& url);
  void accountChanged(const QString& accountId);
  void calendarsChanged(const QString& accountId);
  void eventsChanged(const QStringList& calendarIds);
  void syncStatusChanged(const QString& accountId, const QJsonObject& status);

 private:
  struct SyncJob;

  [[nodiscard]] SyncJob* activeJob(const QString& accountId, quint64 generation) const;
  bool refreshAndResume(SyncJob* job, std::function<void(SyncJob*)> resume,
                        QString* errorMessage = nullptr);
  void requireReauthorization(SyncJob* job, const QString& errorCode,
                              const QString& errorMessage);
  void setAccountAuthStatus(const QString& accountId, const QString& status);
  void startCalendarList(SyncJob* job, const QString& pageToken = {});
  void syncNextCalendar(SyncJob* job);
  void startEventPage(SyncJob* job, const QString& pageToken = {});
  void drainOutbox(SyncJob* job);
  void dispatchNextOutbox(SyncJob* job);
  void finish(SyncJob* job, const QString& errorCode = {},
              const QString& errorMessage = {});
  void handleMutationResult(SyncJob* job, const OutboxItem& item,
                            const Event& localEvent, const ApiResponse& response);

  Database* m_database = nullptr;
  GoogleAuthManager m_auth;
  GoogleClient m_client;
  RetryPolicy m_retryPolicy;
  QTimer m_pollTimer;
  QHash<QString, SyncJob*> m_jobs;
  QHash<QString, QJsonObject> m_status;
  quint64 m_nextJobGeneration = 0;
  bool m_shuttingDown = false;
};

}  // namespace omacalendar::google
