// tasks.* and taskLists.* IPC handlers.

#include <QJsonArray>
#include <algorithm>

#include "daemon.h"

namespace omacalendar {
namespace {

void invalid(ipc::Error* error, const QString& message) {
  if (error != nullptr) {
    *error = {QStringLiteral("invalid_params"), message, false};
  }
}

// Reads an optional yyyy-MM-dd bound. Returns false when present but invalid.
bool optionalDate(const QJsonObject& params, const QString& key, QDate* value,
                  ipc::Error* error) {
  const QJsonValue raw = params.value(key);
  if (raw.isUndefined() || raw.isNull() || raw.toString().isEmpty()) {
    return true;
  }
  *value = QDate::fromString(raw.toString(), Qt::ISODate);
  if (!raw.isString() || !value->isValid()) {
    invalid(error, QStringLiteral("%1 must be a yyyy-MM-dd date").arg(key));
    return false;
  }
  return true;
}

// Copies the client-editable fields of a draft onto a task. Identity,
// provider data and bookkeeping are never taken from a client.
bool applyTaskDraft(const QJsonObject& draft, Task* task, ipc::Error* error) {
  if (draft.contains(QStringLiteral("title"))) {
    task->title = draft.value(QStringLiteral("title")).toString();
  }
  if (draft.contains(QStringLiteral("notes"))) {
    task->notes = draft.value(QStringLiteral("notes")).toString();
  }
  const QDate previousDueDate = task->dueDate;
  if (draft.contains(QStringLiteral("dueDate"))) {
    const QString due = draft.value(QStringLiteral("dueDate")).toString();
    task->dueDate = QDate::fromString(due, Qt::ISODate);
    if (!due.isEmpty() && !task->dueDate.isValid()) {
      invalid(error, QStringLiteral("dueDate must be a yyyy-MM-dd date"));
      return false;
    }
    // A new day without a new time drops the old time, which belonged to the
    // old day.
    if (!task->dueDate.isValid() ||
        (!draft.contains(QStringLiteral("dueUtc")) && task->dueUtc.isValid() &&
         task->dueDate != previousDueDate)) {
      task->dueUtc = {};
    }
  }
  if (draft.contains(QStringLiteral("dueUtc"))) {
    const QString due = draft.value(QStringLiteral("dueUtc")).toString();
    task->dueUtc = dateTimeFromIso(due);
    if (!due.isEmpty() && !task->dueUtc.isValid()) {
      invalid(error, QStringLiteral("dueUtc must be an ISO-8601 date-time"));
      return false;
    }
  }
  if (draft.contains(QStringLiteral("completed"))) {
    task->completed = draft.value(QStringLiteral("completed")).toBool();
  }
  if (draft.contains(QStringLiteral("priority"))) {
    task->priority = draft.value(QStringLiteral("priority")).toInt();
  }
  return true;
}

}  // namespace

void Daemon::syncTaskList(const QString& listId) {
  const TaskList list = m_database.taskList(listId);
  if (list.capabilities.value(QStringLiteral("provider")).toString() ==
      QStringLiteral("caldav")) {
    m_caldav.syncTasks(list.accountId);
  }
}

void Daemon::emitTasksChanged(const QStringList& listIds) {
  m_server.broadcast(QStringLiteral("tasks.changed"),
                     {{QStringLiteral("listIds"), QJsonArray::fromStringList(listIds)},
                      {QStringLiteral("revision"), m_database.changeRevision()}});
}

QJsonValue Daemon::onTaskListsList(const QJsonObject&, ipc::Error* error) const {
  QString dbError;
  const QList<TaskList> lists = m_database.taskLists(&dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  QJsonArray encoded;
  for (const TaskList& list : lists) {
    encoded.append(toJson(list));
  }
  return QJsonObject{{QStringLiteral("lists"), encoded}};
}

QJsonValue Daemon::onTaskListsSetEnabled(const QJsonObject& params, ipc::Error* error) {
  QString listId;
  if (!validateRequiredString(params, QStringLiteral("listId"), &listId, error)) {
    return {};
  }
  if (!params.value(QStringLiteral("enabled")).isBool()) {
    invalid(error, QStringLiteral("enabled must be a boolean"));
    return {};
  }
  QString dbError;
  if (!m_database.setTaskListEnabled(
          listId, params.value(QStringLiteral("enabled")).toBool(), &dbError)) {
    if (error != nullptr) {
      *error = {dbError == QStringLiteral("Task list not found")
                    ? QStringLiteral("not_found")
                    : QStringLiteral("database_error"),
                dbError, false};
    }
    return {};
  }
  emitTasksChanged({listId});
  return QJsonObject{{QStringLiteral("listId"), listId}};
}

QJsonValue Daemon::onTasksList(const QJsonObject& params, ipc::Error* error) const {
  TaskQuery filter;
  const QJsonValue listIds = params.value(QStringLiteral("listIds"));
  if (!listIds.isUndefined() && !listIds.isNull()) {
    if (!listIds.isArray()) {
      invalid(error, QStringLiteral("listIds must be an array of list ids"));
      return {};
    }
    for (const QJsonValue& value : listIds.toArray()) {
      if (!value.isString() || value.toString().isEmpty()) {
        invalid(error, QStringLiteral("listIds must be an array of list ids"));
        return {};
      }
      filter.listIds.append(value.toString());
    }
    if (filter.listIds.isEmpty()) {
      return QJsonObject{{QStringLiteral("tasks"), QJsonArray{}}};
    }
  }
  filter.includeCompleted =
      params.value(QStringLiteral("includeCompleted")).toBool(true);
  if (!optionalDate(params, QStringLiteral("dueStart"), &filter.dueStart, error) ||
      !optionalDate(params, QStringLiteral("dueEnd"), &filter.dueEnd, error)) {
    return {};
  }
  if (filter.dueStart.isValid() && filter.dueEnd.isValid() &&
      filter.dueEnd < filter.dueStart) {
    invalid(error, QStringLiteral("dueEnd must not be before dueStart"));
    return {};
  }
  const int limit =
      std::clamp(params.value(QStringLiteral("limit")).toInt(filter.limit), 1, 2000);
  filter.offset = std::max(0, params.value(QStringLiteral("offset")).toInt());
  // One extra row tells whether another page follows.
  filter.limit = limit + 1;
  QString dbError;
  QList<Task> tasks = m_database.tasks(filter, &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  const bool hasMore = tasks.size() > limit;
  if (hasMore) {
    tasks.removeLast();
  }
  QJsonArray encoded;
  for (const Task& task : tasks) {
    encoded.append(toJson(task));
  }
  return QJsonObject{
      {QStringLiteral("tasks"), encoded},
      {QStringLiteral("offset"), filter.offset},
      {QStringLiteral("hasMore"), hasMore},
      {QStringLiteral("nextOffset"), filter.offset + static_cast<int>(tasks.size())},
  };
}

QJsonValue Daemon::onTasksCreate(const QJsonObject& params, ipc::Error* error) {
  const QJsonObject draft = params.value(QStringLiteral("task")).toObject();
  Task task;
  task.listId = draft.value(QStringLiteral("listId"))
                    .toString(QString::fromLatin1(kLocalTaskListId));
  if (!applyTaskDraft(draft, &task, error)) {
    return {};
  }
  QString dbError;
  if (!m_database.saveLocalTask(&task, -1, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("task_rejected"), dbError, false};
    }
    return {};
  }
  emitTasksChanged({task.listId});
  syncTaskList(task.listId);
  return toJson(task);
}

QJsonValue Daemon::onTasksUpdate(const QJsonObject& params, ipc::Error* error) {
  const QJsonObject draft = params.value(QStringLiteral("task")).toObject();
  const QString taskId = draft.value(QStringLiteral("id")).toString();
  if (taskId.isEmpty()) {
    invalid(error, QStringLiteral("task.id is required"));
    return {};
  }
  QString dbError;
  Task task = m_database.task(taskId, &dbError);
  if (task.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), dbError, false};
    }
    return {};
  }
  if (draft.contains(QStringLiteral("listId")) &&
      draft.value(QStringLiteral("listId")).toString() != task.listId) {
    invalid(error,
            QStringLiteral("Moving a task to another list is not supported yet"));
    return {};
  }
  if (!applyTaskDraft(draft, &task, error)) {
    return {};
  }
  const qint64 expected =
      params.value(QStringLiteral("expectedLocalRevision")).toInteger(-1);
  if (!m_database.saveLocalTask(&task, expected, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("task_rejected"), dbError, false};
    }
    return {};
  }
  emitTasksChanged({task.listId});
  syncTaskList(task.listId);
  return toJson(task);
}

QJsonValue Daemon::onTasksRemove(const QJsonObject& params, ipc::Error* error) {
  QString taskId;
  if (!validateRequiredString(params, QStringLiteral("taskId"), &taskId, error)) {
    return {};
  }
  QString dbError;
  const Task task = m_database.task(taskId, &dbError);
  if (task.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), dbError, false};
    }
    return {};
  }
  if (!m_database.removeLocalTask(taskId, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("task_rejected"), dbError, false};
    }
    return {};
  }
  emitTasksChanged({task.listId});
  syncTaskList(task.listId);
  return QJsonObject{{QStringLiteral("taskId"), taskId}};
}

void Daemon::registerTaskHandlers() {
  m_router.registerHandler(QStringLiteral("taskLists.list"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onTaskListsList(params, error);
                           });
  m_router.registerHandler(QStringLiteral("taskLists.setEnabled"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onTaskListsSetEnabled(params, error);
                           });
  m_router.registerHandler(QStringLiteral("tasks.list"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onTasksList(params, error);
                           });
  m_router.registerHandler(QStringLiteral("tasks.create"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onTasksCreate(params, error);
                           });
  m_router.registerHandler(QStringLiteral("tasks.update"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onTasksUpdate(params, error);
                           });
  m_router.registerHandler(QStringLiteral("tasks.remove"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onTasksRemove(params, error);
                           });
}

}  // namespace omacalendar
