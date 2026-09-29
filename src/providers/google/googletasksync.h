#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <memory>

#include "core/database.h"
#include "providers/google/googleclient.h"

namespace omacalendar::google {

inline constexpr auto kGoogleTaskFormat = "google-tasks-json";

// Maps one Tasks API task. Google keeps only the day of a due date.
[[nodiscard]] Task taskFromGoogleJson(const QJsonObject& resource);
// The fields OmaCalendar edits, for inserts and patches.
[[nodiscard]] QJsonObject taskToGoogleJson(const Task& task);

// A create whose response never arrived may still have reached Google. Pairs
// each such local write with an unclaimed Google task holding the same
// content, taking it from candidates. The Tasks API has no idempotent insert,
// so an identical task added elsewhere in the meantime is taken as that copy. Returns
// local task id -> Google task id.
[[nodiscard]] QHash<QString, QString> matchUnconfirmedCreates(const QList<Task>& writes,
                                                              QList<Task>* candidates);

// Keeps an account's Google task lists and the stored copies in step: owed
// local writes are sent first, then every list is read back in full. The
// Tasks API has no conditional writes, so the last write wins.
class GoogleTaskSync final : public QObject {
  Q_OBJECT

 public:
  GoogleTaskSync(Database* database, GoogleClient* client, QObject* parent = nullptr);
  ~GoogleTaskSync() override;

  void syncAccount(const QString& accountId);
  void cancel(const QString& accountId);
  [[nodiscard]] bool isSyncing(const QString& accountId) const;

 signals:
  void tasksChanged(const QStringList& listIds);
  // The sync stopped for this account or list; others carry on.
  void syncFailed(const QString& accountId, const QString& listId,
                  const QString& errorCode, const QString& errorMessage);

 private:
  struct Job;
  void readListPage(const std::shared_ptr<Job>& job, const QString& pageToken);
  void storeLists(const std::shared_ptr<Job>& job);
  void syncNextList(const std::shared_ptr<Job>& job);
  void sendNextWrite(const std::shared_ptr<Job>& job);
  void readTaskPage(const std::shared_ptr<Job>& job, const QString& pageToken);
  void storeRemoteTasks(const std::shared_ptr<Job>& job);
  void reconcileCreates(const std::shared_ptr<Job>& job);
  void recordWrite(const std::shared_ptr<Job>& job, const Task& task,
                   const ApiResponse& response);
  void failList(const std::shared_ptr<Job>& job, const ApiResponse& response);
  void finish(const std::shared_ptr<Job>& job);
  [[nodiscard]] bool current(const std::shared_ptr<Job>& job) const;

  Database* m_database;
  GoogleClient* m_client;
  QHash<QString, std::shared_ptr<Job>> m_jobs;
  QSet<QString> m_again;
  // A rerun owed after a finished job, until it starts; counts as syncing.
  QSet<QString> m_rerunQueued;
};

}  // namespace omacalendar::google
