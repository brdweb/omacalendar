#include "providers/google/googletasksync.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

namespace omacalendar::google {
namespace {

QString text(const QJsonObject& object, const char* key) {
  return object.value(QString::fromLatin1(key)).toString();
}

QDateTime rfc3339(const QString& value) {
  QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
  if (!parsed.isValid()) {
    parsed = QDateTime::fromString(value, Qt::ISODate);
  }
  return parsed.isValid() ? parsed.toUTC() : QDateTime{};
}

}  // namespace

Task taskFromGoogleJson(const QJsonObject& resource) {
  Task task;
  task.remoteId = text(resource, "id");
  task.uid = task.remoteId;
  task.etag = text(resource, "etag");
  task.title = text(resource, "title");
  task.notes = text(resource, "notes");
  // Google stores only the day, as midnight UTC; the time part is meaningless.
  task.dueDate = QDate::fromString(text(resource, "due").left(10), Qt::ISODate);
  task.completed = text(resource, "status") == QStringLiteral("completed");
  task.completedAt = rfc3339(text(resource, "completed"));
  task.parentId = text(resource, "parent");
  task.position = text(resource, "position");
  task.updatedAt = rfc3339(text(resource, "updated"));
  task.rawPayload =
      QString::fromUtf8(QJsonDocument(resource).toJson(QJsonDocument::Compact));
  task.rawFormat = QString::fromLatin1(kGoogleTaskFormat);
  return task;
}

QJsonObject taskToGoogleJson(const Task& task) {
  QJsonObject body{
      {QStringLiteral("title"), task.title},
      {QStringLiteral("notes"), task.notes},
      {QStringLiteral("status"),
       task.completed ? QStringLiteral("completed") : QStringLiteral("needsAction")},
  };
  body.insert(QStringLiteral("due"),
              task.dueDate.isValid() ? QJsonValue(task.dueDate.toString(Qt::ISODate) +
                                                  QStringLiteral("T00:00:00.000Z"))
                                     : QJsonValue(QJsonValue::Null));
  body.insert(QStringLiteral("completed"),
              task.completed && task.completedAt.isValid()
                  ? QJsonValue(task.completedAt.toUTC().toString(Qt::ISODateWithMs))
                  : QJsonValue(QJsonValue::Null));
  return body;
}

struct GoogleTaskSync::Job {
  QString accountId;
  QList<QJsonObject> remoteLists;
  QList<TaskList> lists;
  int listIndex = -1;
  QList<Task> writes;
  int writeIndex = 0;
  QList<Task> remoteTasks;
  QStringList changed;
};

GoogleTaskSync::GoogleTaskSync(Database* database, GoogleClient* client,
                               QObject* parent)
    : QObject(parent), m_database(database), m_client(client) {}

GoogleTaskSync::~GoogleTaskSync() = default;

bool GoogleTaskSync::current(const std::shared_ptr<Job>& job) const {
  return job && m_jobs.value(job->accountId) == job;
}

bool GoogleTaskSync::isSyncing(const QString& accountId) const {
  return m_jobs.contains(accountId);
}

void GoogleTaskSync::cancel(const QString& accountId) {
  m_jobs.remove(accountId);
  m_again.remove(accountId);
}

void GoogleTaskSync::syncAccount(const QString& accountId) {
  if (m_jobs.contains(accountId)) {
    m_again.insert(accountId);
    return;
  }
  auto job = std::make_shared<Job>();
  job->accountId = accountId;
  m_jobs.insert(accountId, job);
  readListPage(job, {});
}

void GoogleTaskSync::readListPage(const std::shared_ptr<Job>& job,
                                  const QString& pageToken) {
  m_client->listTaskLists(
      job->accountId, pageToken, [this, job](const ApiResponse& response) {
        if (!current(job)) {
          return;
        }
        if (!response.ok) {
          // Without the list of lists nothing can be synced.
          emit syncFailed(job->accountId, {}, response.errorCode,
                          response.errorMessage);
          finish(job);
          return;
        }
        for (const QJsonValue& value :
             response.body.value(QStringLiteral("items")).toArray()) {
          job->remoteLists.append(value.toObject());
        }
        const QString next = text(response.body, "nextPageToken");
        if (!next.isEmpty() && next != text(response.body, "pageToken")) {
          readListPage(job, next);
          return;
        }
        storeLists(job);
      });
}

void GoogleTaskSync::storeLists(const std::shared_ptr<Job>& job) {
  QSet<QString> seen;
  int position = 0;
  for (const QJsonObject& remote : std::as_const(job->remoteLists)) {
    const QString remoteId = text(remote, "id");
    if (remoteId.isEmpty()) {
      continue;
    }
    seen.insert(remoteId);
    TaskList list = m_database->taskListByRemoteId(job->accountId, remoteId);
    const bool added = list.id.isEmpty();
    if (added) {
      list.id = newUuid();
      list.accountId = job->accountId;
      list.remoteId = remoteId;
    }
    const QString title = text(remote, "title");
    list.name = title.isEmpty() ? QStringLiteral("Tasks") : title;
    list.readOnly = false;
    list.position = position++;
    list.capabilities = {{QStringLiteral("createTask"), true},
                         {QStringLiteral("updateTask"), true},
                         {QStringLiteral("removeTask"), true}};
    QString error;
    if (!m_database->upsertTaskList(list, &error)) {
      emit syncFailed(job->accountId, list.id, QStringLiteral("database_error"), error);
      continue;
    }
    if (added) {
      job->changed.append(list.id);
    }
  }
  for (const TaskList& list : m_database->taskLists()) {
    if (list.accountId != job->accountId) {
      continue;
    }
    if (!seen.contains(list.remoteId)) {
      if (m_database->removeTaskList(list.id)) {
        job->changed.append(list.id);
      }
      continue;
    }
    job->lists.append(list);
  }
  syncNextList(job);
}

void GoogleTaskSync::syncNextList(const std::shared_ptr<Job>& job) {
  if (!current(job)) {
    return;
  }
  ++job->listIndex;
  if (job->listIndex >= job->lists.size()) {
    finish(job);
    return;
  }
  job->writes = m_database->pendingTaskWrites(job->lists.at(job->listIndex).id);
  job->writeIndex = 0;
  job->remoteTasks.clear();
  sendNextWrite(job);
}

void GoogleTaskSync::sendNextWrite(const std::shared_ptr<Job>& job) {
  if (!current(job)) {
    return;
  }
  const TaskList& list = job->lists.at(job->listIndex);
  if (job->writeIndex >= job->writes.size()) {
    readTaskPage(job, {});
    return;
  }
  const Task task = job->writes.at(job->writeIndex++);
  const auto done = [this, job, task](const ApiResponse& response) {
    if (!current(job)) {
      return;
    }
    if (response.ok || (task.pendingOperation == QStringLiteral("remove") &&
                        (response.notFound || response.httpStatus == 410))) {
      recordWrite(job, task, response);
      sendNextWrite(job);
      return;
    }
    failList(job, response);
  };
  if (task.pendingOperation == QStringLiteral("remove") && task.remoteId.isEmpty()) {
    // Removed before it was ever uploaded: nothing to tell Google.
    recordWrite(job, task, ApiResponse{});
    sendNextWrite(job);
  } else if (task.pendingOperation == QStringLiteral("remove")) {
    m_client->deleteTask(job->accountId, list.remoteId, task.remoteId, done);
  } else if (task.pendingOperation == QStringLiteral("create") ||
             task.remoteId.isEmpty()) {
    m_client->createTask(job->accountId, list.remoteId, taskToGoogleJson(task), done);
  } else {
    m_client->updateTask(job->accountId, list.remoteId, task.remoteId,
                         taskToGoogleJson(task),
                         [this, job, task, done,
                          listRemoteId = list.remoteId](const ApiResponse& response) {
                           if (!current(job)) {
                             return;
                           }
                           if (response.notFound || response.httpStatus == 410) {
                             // Removed on Google meanwhile: the edit brings it back.
                             m_client->createTask(job->accountId, listRemoteId,
                                                  taskToGoogleJson(task), done);
                             return;
                           }
                           done(response);
                         });
  }
}

void GoogleTaskSync::recordWrite(const std::shared_ptr<Job>& job, const Task& task,
                                 const ApiResponse& response) {
  QString error;
  if (task.pendingOperation == QStringLiteral("remove")) {
    m_database->completeTaskWrite(task.id, task.localRevision, task.remoteId, {}, {},
                                  {}, nullptr, &error);
  } else {
    const Task uploaded = taskFromGoogleJson(response.body);
    m_database->completeTaskWrite(
        task.id, task.localRevision,
        uploaded.remoteId.isEmpty() ? task.remoteId : uploaded.remoteId, uploaded.etag,
        uploaded.rawPayload, uploaded.rawFormat, &uploaded, &error);
  }
  if (!error.isEmpty()) {
    emit syncFailed(job->accountId, task.listId, QStringLiteral("database_error"),
                    error);
  }
  job->changed.append(task.listId);
}

void GoogleTaskSync::readTaskPage(const std::shared_ptr<Job>& job,
                                  const QString& pageToken) {
  const TaskList list = job->lists.at(job->listIndex);
  m_client->listTasks(job->accountId, list.remoteId, pageToken,
                      [this, job, list, pageToken](const ApiResponse& response) {
                        if (!current(job)) {
                          return;
                        }
                        if (!response.ok) {
                          failList(job, response);
                          return;
                        }
                        for (const QJsonValue& value :
                             response.body.value(QStringLiteral("items")).toArray()) {
                          const QJsonObject resource = value.toObject();
                          if (!resource.value(QStringLiteral("deleted")).toBool()) {
                            job->remoteTasks.append(taskFromGoogleJson(resource));
                          }
                        }
                        const QString next = text(response.body, "nextPageToken");
                        if (!next.isEmpty() && next != pageToken) {
                          readTaskPage(job, next);
                          return;
                        }
                        bool changed = false;
                        QString error;
                        if (!m_database->applyRemoteTasks(list.id, job->remoteTasks,
                                                          true, &changed, &error)) {
                          ApiResponse failure;
                          failure.errorCode = QStringLiteral("database_error");
                          failure.errorMessage = error;
                          failList(job, failure);
                          return;
                        }
                        if (changed) {
                          job->changed.append(list.id);
                        }
                        TaskList synced = list;
                        synced.lastSyncAt = QDateTime::currentDateTimeUtc();
                        m_database->upsertTaskList(synced);
                        syncNextList(job);
                      });
}

void GoogleTaskSync::failList(const std::shared_ptr<Job>& job,
                              const ApiResponse& response) {
  if (!current(job)) {
    return;
  }
  emit syncFailed(
      job->accountId, job->lists.value(job->listIndex).id,
      response.insufficientScope ? QStringLiteral("permission") : response.errorCode,
      response.errorMessage);
  syncNextList(job);
}

void GoogleTaskSync::finish(const std::shared_ptr<Job>& job) {
  const QString accountId = job->accountId;
  m_jobs.remove(accountId);
  QStringList changed = job->changed;
  changed.removeDuplicates();
  if (!changed.isEmpty()) {
    emit tasksChanged(changed);
  }
  if (m_again.remove(accountId)) {
    QTimer::singleShot(0, this, [this, accountId]() { syncAccount(accountId); });
  }
}

}  // namespace omacalendar::google
