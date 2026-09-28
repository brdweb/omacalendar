#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <memory>

#include "core/database.h"
#include "providers/caldav/caldavclient.h"
#include "providers/caldav/caldavxml.h"

namespace omacalendar::caldav {

// Keeps CalDAV VTODO collections and the task lists stored for them in step.
// Local writes are sent first (creates with If-None-Match, updates and
// removals with If-Match); then every list is read back in full.
class CalDavTaskSync final : public QObject {
  Q_OBJECT

 public:
  CalDavTaskSync(Database* database, CalDavClient* client, QObject* parent = nullptr);
  ~CalDavTaskSync() override;

  // Matches the account's task lists to freshly discovered collections
  // (adding, updating and removing lists), then syncs every list. A list is
  // only removed on clear evidence: its collection now lists components
  // without VTODO, or it is missing from a discovery in which every
  // response succeeded (complete).
  void syncDiscovered(const QString& accountId, const QUrl& homeUrl,
                      const QList<CalDavCollection>& collections, bool complete);
  // Syncs the account's stored lists without discovery, for example right
  // after a local edit.
  void syncStored(const QString& accountId);
  void cancel(const QString& accountId);
  [[nodiscard]] bool isSyncing(const QString& accountId) const;

 signals:
  void tasksChanged(const QStringList& listIds);
  // One list could not be synced; the others carry on.
  void listFailed(const QString& accountId, const QString& listId,
                  const QString& errorCode, const QString& errorMessage);

 private:
  struct Job;
  void start(const std::shared_ptr<Job>& job);
  void syncNextList(const std::shared_ptr<Job>& job);
  void sendNextWrite(const std::shared_ptr<Job>& job);
  void sendCreate(const std::shared_ptr<Job>& job, const Task& task);
  void sendUpdate(const std::shared_ptr<Job>& job, const Task& task, bool retried);
  void sendRemove(const std::shared_ptr<Job>& job, const Task& task, bool retried);
  void recordWrite(const std::shared_ptr<Job>& job, const Task& task,
                   const QString& remoteId, const QString& etag,
                   const QByteArray& payload);
  void pull(const std::shared_ptr<Job>& job);
  void failList(const std::shared_ptr<Job>& job, const QString& errorCode,
                const QString& errorMessage);
  void finish(const std::shared_ptr<Job>& job);
  [[nodiscard]] bool current(const std::shared_ptr<Job>& job) const;

  Database* m_database;
  CalDavClient* m_client;
  QHash<QString, std::shared_ptr<Job>> m_jobs;
  // Accounts to sync again once their running job ends.
  QSet<QString> m_again;
};

}  // namespace omacalendar::caldav
