#include "providers/caldav/caldavtasksync.h"

#include <QTimer>
#include <algorithm>

#include "providers/caldav/vtodocodec.h"

namespace omacalendar::caldav {
namespace {

const QString kRawFormat = QStringLiteral("text/calendar");

// Applies the fields a local edit changed (compared with the provider copy
// it was based on) on top of the provider's current copy, so a concurrent
// change to another field is not lost.
Task mergeEdit(const Task& base, const Task& local, const Task& remote) {
  Task merged = remote;
  if (local.title != base.title) {
    merged.title = local.title;
  }
  if (local.notes != base.notes) {
    merged.notes = local.notes;
  }
  if (local.dueDate != base.dueDate || local.dueUtc != base.dueUtc) {
    merged.dueDate = local.dueDate;
    merged.dueUtc = local.dueUtc;
  }
  if (local.completed != base.completed) {
    merged.completed = local.completed;
    merged.completedAt = local.completedAt;
  }
  if (local.priority != base.priority) {
    merged.priority = local.priority;
  }
  return merged;
}

}  // namespace

struct CalDavTaskSync::Job {
  QString accountId;
  QList<TaskList> lists;
  // Collection ctags from discovery, by list id. Empty without discovery.
  QHash<QString, QString> ctags;
  int listIndex = -1;
  QList<Task> writes;
  int writeIndex = 0;
  bool wroteToList = false;
  QStringList changed;
};

CalDavTaskSync::CalDavTaskSync(Database* database, CalDavClient* client,
                               QObject* parent)
    : QObject(parent), m_database(database), m_client(client) {}

CalDavTaskSync::~CalDavTaskSync() = default;

bool CalDavTaskSync::current(const std::shared_ptr<Job>& job) const {
  return job && m_jobs.value(job->accountId) == job;
}

bool CalDavTaskSync::isSyncing(const QString& accountId) const {
  return m_jobs.contains(accountId);
}

void CalDavTaskSync::cancel(const QString& accountId) {
  m_jobs.remove(accountId);
  m_again.remove(accountId);
}

void CalDavTaskSync::syncDiscovered(const QString& accountId, const QUrl& homeUrl,
                                    const QList<CalDavCollection>& collections,
                                    const bool complete) {
  auto job = std::make_shared<Job>();
  job->accountId = accountId;
  QSet<QString> present;
  QSet<QString> withoutTasks;
  int position = 0;
  for (const CalDavCollection& collection : collections) {
    const QString remoteId =
        CalDavClient::canonicalResourceId(homeUrl, collection.href);
    present.insert(remoteId);
    if (!collection.holdsTasks()) {
      // Only an explicit component list proves the collection holds no
      // tasks; a missing one (say, a failed propstat) proves nothing.
      if (!collection.supportedComponents.isEmpty()) {
        withoutTasks.insert(remoteId);
      }
      continue;
    }
    const TaskList existing = m_database->taskListByRemoteId(accountId, remoteId);
    TaskList list = existing;
    if (list.id.isEmpty()) {
      list.id = newUuid();
      list.accountId = accountId;
      list.remoteId = remoteId;
    }
    list.name = collection.displayName.isEmpty() ? QStringLiteral("Tasks")
                                                 : collection.displayName;
    if (!collection.color.isEmpty()) {
      list.color = collection.color.left(7);
    }
    list.readOnly = collection.readOnly;
    list.position = position++;
    list.capabilities = {{QStringLiteral("createTask"), !collection.readOnly},
                         {QStringLiteral("updateTask"), !collection.readOnly},
                         {QStringLiteral("removeTask"), !collection.readOnly}};
    QString error;
    if (!m_database->upsertTaskList(list, &error)) {
      emit listFailed(accountId, list.id, QStringLiteral("database_error"), error);
      continue;
    }
    if (existing.id.isEmpty()) {
      job->changed.append(list.id);
    }
    job->ctags.insert(list.id, collection.ctag);
  }
  // Lists whose collection is gone, or no longer holds tasks, go with it.
  for (const TaskList& list : m_database->taskLists()) {
    if (list.accountId == accountId &&
        (withoutTasks.contains(list.remoteId) ||
         (complete && !present.contains(list.remoteId)))) {
      QString error;
      if (m_database->removeTaskList(list.id, &error)) {
        job->changed.append(list.id);
      }
    }
  }
  if (m_jobs.contains(accountId)) {
    m_again.insert(accountId);
    if (!job->changed.isEmpty()) {
      emit tasksChanged(job->changed);
    }
    return;
  }
  for (const TaskList& list : m_database->taskLists()) {
    if (list.accountId == accountId) {
      job->lists.append(list);
    }
  }
  start(job);
}

void CalDavTaskSync::syncStored(const QString& accountId) {
  if (m_jobs.contains(accountId)) {
    m_again.insert(accountId);
    return;
  }
  auto job = std::make_shared<Job>();
  job->accountId = accountId;
  for (const TaskList& list : m_database->taskLists()) {
    if (list.accountId == accountId) {
      job->lists.append(list);
    }
  }
  if (job->lists.isEmpty()) {
    return;
  }
  start(job);
}

void CalDavTaskSync::start(const std::shared_ptr<Job>& job) {
  m_jobs.insert(job->accountId, job);
  syncNextList(job);
}

void CalDavTaskSync::syncNextList(const std::shared_ptr<Job>& job) {
  if (!current(job)) {
    return;
  }
  ++job->listIndex;
  if (job->listIndex >= job->lists.size()) {
    finish(job);
    return;
  }
  QString error;
  job->writes = m_database->pendingTaskWrites(job->lists.at(job->listIndex).id, &error);
  job->writeIndex = 0;
  job->wroteToList = false;
  if (!error.isEmpty()) {
    failList(job, QStringLiteral("database_error"), error);
    return;
  }
  sendNextWrite(job);
}

void CalDavTaskSync::sendNextWrite(const std::shared_ptr<Job>& job) {
  if (!current(job)) {
    return;
  }
  const TaskList& list = job->lists.at(job->listIndex);
  if (job->writeIndex >= job->writes.size() || list.readOnly) {
    pull(job);
    return;
  }
  const Task task = job->writes.at(job->writeIndex++);
  if (task.pendingOperation == QStringLiteral("remove") && task.remoteId.isEmpty()) {
    // Removed before it was ever uploaded: nothing to tell the server.
    recordWrite(job, task, {}, {}, {});
    sendNextWrite(job);
  } else if (task.pendingOperation == QStringLiteral("remove")) {
    sendRemove(job, task, false);
  } else if (task.pendingOperation == QStringLiteral("create") ||
             task.remoteId.isEmpty()) {
    sendCreate(job, task);
  } else {
    sendUpdate(job, task, false);
  }
}

void CalDavTaskSync::sendCreate(const std::shared_ptr<Job>& job, const Task& task) {
  const TaskList& list = job->lists.at(job->listIndex);
  QString error;
  const QByteArray payload = VTodoCodec::serialize(task, {}, &error);
  if (payload.isEmpty()) {
    failList(job, QStringLiteral("invalid_task"), error);
    return;
  }
  // A resource that was created before keeps its address; a new one is named
  // after the task's UID inside the collection.
  QUrl collection(list.remoteId);
  if (!collection.path().endsWith(QLatin1Char('/'))) {
    collection.setPath(collection.path() + QLatin1Char('/'));
  }
  const QUrl url =
      task.remoteId.isEmpty()
          ? collection.resolved(QUrl(QString::fromLatin1(
                QUrl::toPercentEncoding(task.uid) + QByteArrayLiteral(".ics"))))
          : QUrl(task.remoteId);
  const QString remoteId = CalDavClient::canonicalResourceId(url, url.toString());
  m_client->createEvent(
      job->accountId, url, payload,
      [this, job, task, remoteId, payload](const DavResponse& response) {
        if (!current(job)) {
          return;
        }
        // 412: an earlier upload arrived but its answer was lost. Keep the
        // address; the read that follows brings the stored copy.
        if (response.ok || response.httpStatus == 412) {
          recordWrite(job, task, remoteId, response.ok ? response.etag : QString(),
                      payload);
          sendNextWrite(job);
          return;
        }
        failList(job, response.errorCode, response.errorMessage);
      });
}

void CalDavTaskSync::sendUpdate(const std::shared_ptr<Job>& job, const Task& task,
                                const bool retried) {
  QString error;
  const QByteArray payload =
      VTodoCodec::serialize(task, task.rawPayload.toUtf8(), &error);
  if (payload.isEmpty()) {
    failList(job, QStringLiteral("invalid_task"), error);
    return;
  }
  const QUrl url(task.remoteId);
  m_client->updateEvent(
      job->accountId, url, task.etag, payload,
      [this, job, task, retried, payload, url](const DavResponse& response) {
        if (!current(job)) {
          return;
        }
        if (response.ok) {
          recordWrite(job, task, task.remoteId, response.etag, payload);
          sendNextWrite(job);
          return;
        }
        if (response.httpStatus == 404 || response.httpStatus == 410) {
          // Removed on the server meanwhile: the edit brings it back.
          sendCreate(job, task);
          return;
        }
        if (response.httpStatus != 412 || retried) {
          failList(job, response.errorCode, response.errorMessage);
          return;
        }
        // Changed on the server meanwhile: merge the edit into its copy.
        m_client->readResource(
            job->accountId, url, [this, job, task](const DavResponse& read) {
              if (!current(job)) {
                return;
              }
              const CalDavMultiStatusResult parsed =
                  CalDavXml::parseMultiStatus(read.body);
              const QList<CalDavResource> resources = read.ok && parsed.ok()
                                                          ? CalDavXml::resources(parsed)
                                                          : QList<CalDavResource>{};
              if (!read.ok || resources.isEmpty()) {
                failList(job, read.ok ? QStringLiteral("conflict") : read.errorCode,
                         read.ok ? QStringLiteral("The task changed on the server")
                                 : read.errorMessage);
                return;
              }
              const CalDavResource& remote = resources.first();
              if (remote.deleted()) {
                sendCreate(job, task);
                return;
              }
              const std::optional<Task> remoteTask =
                  VTodoCodec::parse(remote.calendarData.toUtf8());
              const std::optional<Task> base =
                  VTodoCodec::parse(task.rawPayload.toUtf8());
              if (!remoteTask.has_value()) {
                failList(job, QStringLiteral("invalid_task"),
                         QStringLiteral("The server's copy of a task is unreadable"));
                return;
              }
              Task merged = mergeEdit(base.value_or(Task{}), task, *remoteTask);
              merged.id = task.id;
              merged.listId = task.listId;
              merged.remoteId = task.remoteId;
              merged.localRevision = task.localRevision;
              merged.pendingOperation = task.pendingOperation;
              merged.etag = remote.etag;
              merged.rawPayload = remote.calendarData;
              sendUpdate(job, merged, true);
            });
      });
}

void CalDavTaskSync::sendRemove(const std::shared_ptr<Job>& job, const Task& task,
                                const bool retried) {
  const QUrl url(task.remoteId);
  m_client->deleteEvent(
      job->accountId, url, task.etag,
      [this, job, task, retried, url](const DavResponse& response) {
        if (!current(job)) {
          return;
        }
        if (response.ok || response.httpStatus == 404 || response.httpStatus == 410) {
          recordWrite(job, task, task.remoteId, {}, {});
          sendNextWrite(job);
          return;
        }
        if (response.httpStatus != 412 || retried) {
          failList(job, response.errorCode, response.errorMessage);
          return;
        }
        // Changed on the server meanwhile; the removal still stands.
        m_client->readResource(
            job->accountId, url, [this, job, task](const DavResponse& read) {
              if (!current(job)) {
                return;
              }
              const CalDavMultiStatusResult parsed =
                  CalDavXml::parseMultiStatus(read.body);
              const QList<CalDavResource> resources = read.ok && parsed.ok()
                                                          ? CalDavXml::resources(parsed)
                                                          : QList<CalDavResource>{};
              if (read.ok && (resources.isEmpty() || resources.first().deleted())) {
                recordWrite(job, task, task.remoteId, {}, {});
                sendNextWrite(job);
                return;
              }
              if (!read.ok) {
                failList(job, read.errorCode, read.errorMessage);
                return;
              }
              Task fresh = task;
              fresh.etag = resources.first().etag;
              sendRemove(job, fresh, true);
            });
      });
}

void CalDavTaskSync::recordWrite(const std::shared_ptr<Job>& job, const Task& task,
                                 const QString& remoteId, const QString& etag,
                                 const QByteArray& payload) {
  QString error;
  const std::optional<Task> uploaded =
      payload.isEmpty() ? std::nullopt : VTodoCodec::parse(payload);
  if (!m_database->completeTaskWrite(
          task.id, task.localRevision, remoteId, etag, QString::fromUtf8(payload),
          kRawFormat, uploaded.has_value() ? &*uploaded : nullptr, &error)) {
    emit listFailed(job->accountId, task.listId, QStringLiteral("database_error"),
                    error);
  }
  job->wroteToList = true;
  job->changed.append(task.listId);
}

void CalDavTaskSync::pull(const std::shared_ptr<Job>& job) {
  TaskList list = job->lists.at(job->listIndex);
  const QString ctag = job->ctags.value(list.id);
  if (!job->wroteToList && !ctag.isEmpty() && ctag == list.syncToken) {
    syncNextList(job);
    return;
  }
  const QUrl collection(list.remoteId);
  m_client->queryTasks(
      job->accountId, collection,
      [this, job, list, ctag, collection](const DavResponse& response) mutable {
        if (!current(job)) {
          return;
        }
        if (!response.ok) {
          failList(job, response.errorCode, response.errorMessage);
          return;
        }
        const CalDavMultiStatusResult parsed =
            CalDavXml::parseMultiStatus(response.body);
        if (!parsed.ok()) {
          failList(job, parsed.error.code, parsed.error.message);
          return;
        }
        QList<Task> remote;
        // A resource the server failed to report (a 403 or 5xx inside the
        // multistatus, or no data) is not proof that its task was removed.
        bool complete =
            std::all_of(parsed.responses.cbegin(), parsed.responses.cend(),
                        [](const CalDavResponse& entry) {
                          return entry.statusCode == 404 || entry.statusCode == 410 ||
                                 (entry.isSuccess() && !entry.calendarData.isEmpty());
                        });
        for (const CalDavResource& resource : CalDavXml::resources(parsed)) {
          if (resource.deleted()) {
            continue;
          }
          std::optional<Task> task =
              resource.calendarData.isEmpty()
                  ? std::nullopt
                  : VTodoCodec::parse(resource.calendarData.toUtf8());
          if (!task.has_value()) {
            // Unreadable resources are not proof that a task was removed.
            complete = false;
            continue;
          }
          task->remoteId = CalDavClient::canonicalResourceId(collection, resource.href);
          task->etag = resource.etag;
          task->rawPayload = resource.calendarData;
          task->rawFormat = kRawFormat;
          remote.append(*task);
        }
        bool changed = false;
        QString error;
        if (!m_database->applyRemoteTasks(list.id, remote, complete, &changed,
                                          &error)) {
          failList(job, QStringLiteral("database_error"), error);
          return;
        }
        if (changed) {
          job->changed.append(list.id);
        }
        list.lastSyncAt = QDateTime::currentDateTimeUtc();
        if (!ctag.isEmpty() && complete) {
          list.syncToken = ctag;
        }
        m_database->upsertTaskList(list, &error);
        syncNextList(job);
      });
}

void CalDavTaskSync::failList(const std::shared_ptr<Job>& job, const QString& errorCode,
                              const QString& errorMessage) {
  if (!current(job)) {
    return;
  }
  emit listFailed(job->accountId, job->lists.value(job->listIndex).id, errorCode,
                  errorMessage);
  syncNextList(job);
}

void CalDavTaskSync::finish(const std::shared_ptr<Job>& job) {
  const QString accountId = job->accountId;
  m_jobs.remove(accountId);
  QStringList changed = job->changed;
  changed.removeDuplicates();
  if (!changed.isEmpty()) {
    emit tasksChanged(changed);
  }
  if (m_again.remove(accountId)) {
    QTimer::singleShot(0, this, [this, accountId]() { syncStored(accountId); });
  }
}

}  // namespace omacalendar::caldav
