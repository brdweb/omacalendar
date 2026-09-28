// Task lists and tasks. Kept apart from the event storage in database.cpp:
// tasks have no recurrence, reminders or outbox, so writes owed to a
// provider are tracked on the task row itself (pending_operation).

#include <QJsonDocument>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>

#include "core/database.h"

namespace omacalendar {
namespace {

constexpr int kMaximumTaskQueryLimit = 5000;

QString sqlError(const QSqlQuery& query, const QString& context) {
  return QStringLiteral("%1: %2").arg(context, query.lastError().text());
}

QString nonNull(const QString& value) {
  return value.isNull() ? QStringLiteral("") : value;
}

QString compactJson(const QJsonObject& value) {
  return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
}

QJsonObject parseObject(const QString& value) {
  return value.isEmpty() ? QJsonObject{}
                         : QJsonDocument::fromJson(value.toUtf8()).object();
}

void setError(QString* errorMessage, const QString& message) {
  if (errorMessage != nullptr) {
    *errorMessage = message;
  }
}

const QString kTaskListColumns = QStringLiteral(
    "l.id, l.account_id, l.remote_id, l.name, l.color, l.read_only, l.enabled, "
    "l.position, l.sync_token, l.capabilities_json, l.last_sync_at, a.provider");

const QString kTaskColumns = QStringLiteral(
    "id, list_id, remote_id, uid, etag, title, notes, due_date, due_utc, completed, "
    "completed_at, priority, parent_id, position, raw_payload, raw_format, dirty, "
    "deleted, pending_operation, local_revision, created_at, updated_at");

TaskList taskListFromQuery(const QSqlQuery& query) {
  TaskList list;
  list.id = query.value(0).toString();
  list.accountId = query.value(1).toString();
  list.remoteId = query.value(2).toString();
  list.name = query.value(3).toString();
  list.color = query.value(4).toString();
  list.readOnly = query.value(5).toBool();
  list.enabled = query.value(6).toBool();
  list.position = query.value(7).toInt();
  list.syncToken = query.value(8).toString();
  list.capabilities = parseObject(query.value(9).toString());
  list.lastSyncAt = dateTimeFromIso(query.value(10).toString());
  list.capabilities.insert(QStringLiteral("provider"), query.value(11).toString());
  return list;
}

Task taskFromQuery(const QSqlQuery& query) {
  Task task;
  task.id = query.value(0).toString();
  task.listId = query.value(1).toString();
  task.remoteId = query.value(2).toString();
  task.uid = query.value(3).toString();
  task.etag = query.value(4).toString();
  task.title = query.value(5).toString();
  task.notes = query.value(6).toString();
  task.dueDate = QDate::fromString(query.value(7).toString(), Qt::ISODate);
  task.dueUtc = dateTimeFromIso(query.value(8).toString());
  task.completed = query.value(9).toBool();
  task.completedAt = dateTimeFromIso(query.value(10).toString());
  task.priority = query.value(11).toInt();
  task.parentId = query.value(12).toString();
  task.position = query.value(13).toString();
  task.rawPayload = query.value(14).toString();
  task.rawFormat = query.value(15).toString();
  task.dirty = query.value(16).toBool();
  task.deleted = query.value(17).toBool();
  task.pendingOperation = query.value(18).toString();
  task.localRevision = query.value(19).toLongLong();
  task.createdAt = dateTimeFromIso(query.value(20).toString());
  task.updatedAt = dateTimeFromIso(query.value(21).toString());
  return task;
}

}  // namespace

bool Database::ensureTaskSchema(QString* errorMessage) {
  const QStringList statements = {
      QStringLiteral(R"SQL(
        CREATE TABLE IF NOT EXISTS task_lists (
          id TEXT PRIMARY KEY,
          account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
          remote_id TEXT NOT NULL DEFAULT '',
          name TEXT NOT NULL,
          color TEXT NOT NULL DEFAULT '#9ece6a',
          read_only INTEGER NOT NULL DEFAULT 0,
          enabled INTEGER NOT NULL DEFAULT 1,
          position INTEGER NOT NULL DEFAULT 0,
          sync_token TEXT NOT NULL DEFAULT '',
          capabilities_json TEXT NOT NULL DEFAULT '{}',
          last_sync_at TEXT NOT NULL DEFAULT ''
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE UNIQUE INDEX IF NOT EXISTS task_lists_remote_id_unique
        ON task_lists(account_id, remote_id) WHERE remote_id <> ''
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE IF NOT EXISTS tasks (
          id TEXT PRIMARY KEY,
          list_id TEXT NOT NULL REFERENCES task_lists(id) ON DELETE CASCADE,
          remote_id TEXT NOT NULL DEFAULT '',
          uid TEXT NOT NULL DEFAULT '',
          etag TEXT NOT NULL DEFAULT '',
          title TEXT NOT NULL DEFAULT '',
          notes TEXT NOT NULL DEFAULT '',
          due_date TEXT NOT NULL DEFAULT '',
          due_utc TEXT NOT NULL DEFAULT '',
          completed INTEGER NOT NULL DEFAULT 0,
          completed_at TEXT NOT NULL DEFAULT '',
          priority INTEGER NOT NULL DEFAULT 0,
          parent_id TEXT NOT NULL DEFAULT '',
          position TEXT NOT NULL DEFAULT '',
          raw_payload TEXT NOT NULL DEFAULT '',
          raw_format TEXT NOT NULL DEFAULT '',
          dirty INTEGER NOT NULL DEFAULT 0,
          deleted INTEGER NOT NULL DEFAULT 0,
          pending_operation TEXT NOT NULL DEFAULT ''
            CHECK(pending_operation IN ('','create','update','remove')),
          local_revision INTEGER NOT NULL DEFAULT 0,
          created_at TEXT NOT NULL,
          updated_at TEXT NOT NULL
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE UNIQUE INDEX IF NOT EXISTS tasks_remote_id_unique
        ON tasks(list_id, remote_id) WHERE remote_id <> ''
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS tasks_open_due_index
        ON tasks(list_id, completed, due_date) WHERE deleted=0
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS tasks_pending_index
        ON tasks(list_id) WHERE pending_operation <> ''
      )SQL"),
      // Every installation gets a device-only list so tasks work without an
      // account.
      QStringLiteral(R"SQL(
        INSERT OR IGNORE INTO task_lists(id, account_id, name, color, position,
                                         capabilities_json)
        SELECT 'local-tasks', 'local-account', 'Tasks', '#9ece6a', 0,
               '{"createTask":true,"updateTask":true,"removeTask":true}'
        WHERE EXISTS (SELECT 1 FROM accounts WHERE id='local-account')
      )SQL"),
  };
  for (const QString& statement : statements) {
    if (!execute(statement, errorMessage)) {
      return false;
    }
  }
  return true;
}

QList<TaskList> Database::taskLists(QString* errorMessage) const {
  QList<TaskList> result;
  QSqlQuery query(m_database);
  if (!query.exec(QStringLiteral("SELECT %1 FROM task_lists l JOIN accounts a ON "
                                 "a.id=l.account_id ORDER BY l.position, "
                                 "l.name COLLATE NOCASE, l.id")
                      .arg(kTaskListColumns))) {
    setError(errorMessage, sqlError(query, QStringLiteral("list task lists")));
    return result;
  }
  while (query.next()) {
    result.append(taskListFromQuery(query));
  }
  return result;
}

TaskList Database::taskList(const QString& listId, QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("SELECT %1 FROM task_lists l JOIN accounts a ON "
                               "a.id=l.account_id WHERE l.id=?")
                    .arg(kTaskListColumns));
  query.addBindValue(listId);
  if (!query.exec()) {
    setError(errorMessage, sqlError(query, QStringLiteral("get task list")));
    return {};
  }
  if (!query.next()) {
    setError(errorMessage, QStringLiteral("Task list not found"));
    return {};
  }
  return taskListFromQuery(query);
}

bool Database::upsertTaskList(const TaskList& list, QString* errorMessage) {
  if (list.id.isEmpty() || list.accountId.isEmpty() || list.name.isEmpty()) {
    setError(errorMessage, QStringLiteral("A task list needs an id, account and name"));
    return false;
  }
  QJsonObject capabilities = list.capabilities;
  // The provider comes from the account; never store a copy that can drift.
  capabilities.remove(QStringLiteral("provider"));
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO task_lists(id, account_id, remote_id, name, color, read_only, enabled,
                           position, sync_token, capabilities_json, last_sync_at)
    VALUES (?,?,?,?,?,?,?,?,?,?,?)
    ON CONFLICT(id) DO UPDATE SET
      remote_id=excluded.remote_id, name=excluded.name, color=excluded.color,
      read_only=excluded.read_only, enabled=excluded.enabled,
      position=excluded.position, sync_token=excluded.sync_token,
      capabilities_json=excluded.capabilities_json,
      last_sync_at=excluded.last_sync_at
  )SQL"));
  query.addBindValue(list.id);
  query.addBindValue(list.accountId);
  query.addBindValue(nonNull(list.remoteId));
  query.addBindValue(list.name);
  query.addBindValue(nonNull(list.color));
  query.addBindValue(list.readOnly);
  query.addBindValue(list.enabled);
  query.addBindValue(list.position);
  query.addBindValue(nonNull(list.syncToken));
  query.addBindValue(compactJson(capabilities));
  query.addBindValue(isoUtc(list.lastSyncAt));
  if (!query.exec()) {
    setError(errorMessage, sqlError(query, QStringLiteral("save task list")));
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

bool Database::setTaskListEnabled(const QString& listId, const bool enabled,
                                  QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("UPDATE task_lists SET enabled=? WHERE id=?"));
  query.addBindValue(enabled);
  query.addBindValue(listId);
  if (!query.exec()) {
    setError(errorMessage, sqlError(query, QStringLiteral("update task list")));
    return false;
  }
  if (query.numRowsAffected() == 0) {
    setError(errorMessage, QStringLiteral("Task list not found"));
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

QList<Task> Database::tasks(const TaskQuery& filter, QString* errorMessage) const {
  QList<Task> result;
  QStringList conditions{QStringLiteral("deleted=0")};
  QVariantList bindings;
  if (!filter.listIds.isEmpty()) {
    QStringList placeholders;
    for (const QString& id : filter.listIds) {
      placeholders.append(QStringLiteral("?"));
      bindings.append(id);
    }
    conditions.append(
        QStringLiteral("list_id IN (%1)").arg(placeholders.join(QLatin1Char(','))));
  }
  if (!filter.includeCompleted) {
    conditions.append(QStringLiteral("completed=0"));
  }
  if (filter.dueStart.isValid()) {
    conditions.append(QStringLiteral("due_date<>'' AND due_date>=?"));
    bindings.append(filter.dueStart.toString(Qt::ISODate));
  }
  if (filter.dueEnd.isValid()) {
    conditions.append(QStringLiteral("due_date<>'' AND due_date<=?"));
    bindings.append(filter.dueEnd.toString(Qt::ISODate));
  }
  const int limit = std::clamp(filter.limit, 1, kMaximumTaskQueryLimit);
  QSqlQuery query(m_database);
  // Open tasks first, then by due day (undated last), then oldest first.
  query.prepare(QStringLiteral("SELECT %1 FROM tasks WHERE %2 ORDER BY completed, "
                               "due_date='' , due_date, due_utc, created_at, id "
                               "LIMIT %3 OFFSET %4")
                    .arg(kTaskColumns, conditions.join(QStringLiteral(" AND ")))
                    .arg(limit)
                    .arg(std::max(0, filter.offset)));
  for (const QVariant& value : std::as_const(bindings)) {
    query.addBindValue(value);
  }
  if (!query.exec()) {
    setError(errorMessage, sqlError(query, QStringLiteral("list tasks")));
    return result;
  }
  while (query.next()) {
    result.append(taskFromQuery(query));
  }
  return result;
}

Task Database::task(const QString& taskId, QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("SELECT %1 FROM tasks WHERE id=? AND deleted=0")
                    .arg(kTaskColumns));
  query.addBindValue(taskId);
  if (!query.exec()) {
    setError(errorMessage, sqlError(query, QStringLiteral("get task")));
    return {};
  }
  if (!query.next()) {
    setError(errorMessage, QStringLiteral("Task not found"));
    return {};
  }
  return taskFromQuery(query);
}

bool Database::saveLocalTask(Task* task, const qint64 expectedLocalRevision,
                             QString* errorMessage) {
  if (task == nullptr) {
    setError(errorMessage, QStringLiteral("No task to save"));
    return false;
  }
  const QString validation = validateTask(*task);
  if (!validation.isEmpty()) {
    setError(errorMessage, validation);
    return false;
  }
  QString listError;
  const TaskList list = taskList(task->listId, &listError);
  if (list.id.isEmpty()) {
    setError(errorMessage, listError);
    return false;
  }
  if (list.readOnly) {
    setError(errorMessage, QStringLiteral("This task list is read-only"));
    return false;
  }
  const bool local = list.capabilities.value(QStringLiteral("provider")).toString() ==
                     QStringLiteral("local");
  const QDateTime now = QDateTime::currentDateTimeUtc();

  Task existing;
  if (!task->id.isEmpty()) {
    QString lookupError;
    existing = this->task(task->id, &lookupError);
    if (existing.id.isEmpty() && lookupError != QStringLiteral("Task not found")) {
      setError(errorMessage, lookupError);
      return false;
    }
  }
  const bool creating = existing.id.isEmpty();
  if (!creating) {
    if (expectedLocalRevision >= 0 && existing.localRevision != expectedLocalRevision) {
      setError(errorMessage,
               QStringLiteral("The task changed; reload it and try again"));
      return false;
    }
    if (existing.listId != task->listId) {
      setError(errorMessage, QStringLiteral("Moving a task to another list is not "
                                            "supported yet"));
      return false;
    }
    // Provider-owned identity and payload always come from the stored row.
    task->remoteId = existing.remoteId;
    task->uid = existing.uid;
    task->etag = existing.etag;
    task->rawPayload = existing.rawPayload;
    task->rawFormat = existing.rawFormat;
    task->parentId = existing.parentId;
    task->position = existing.position;
    task->createdAt = existing.createdAt;
  } else {
    if (task->id.isEmpty()) {
      task->id = newUuid();
    }
    task->remoteId.clear();
    task->etag.clear();
    task->rawPayload.clear();
    task->rawFormat.clear();
    if (task->uid.isEmpty()) {
      task->uid = newUuid();
    }
    task->createdAt = now;
  }
  task->title = task->title.trimmed();
  if (task->completed && !task->completedAt.isValid()) {
    task->completedAt = now;
  } else if (!task->completed) {
    task->completedAt = {};
  }
  task->deleted = false;
  task->updatedAt = now;
  task->localRevision = existing.localRevision + 1;
  if (local) {
    task->dirty = false;
    task->pendingOperation.clear();
  } else {
    task->dirty = true;
    // A create that has not reached the provider stays a create.
    task->pendingOperation =
        creating || existing.pendingOperation == QStringLiteral("create")
            ? QStringLiteral("create")
            : QStringLiteral("update");
  }

  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO tasks(id, list_id, remote_id, uid, etag, title, notes, due_date, due_utc,
                      completed, completed_at, priority, parent_id, position,
                      raw_payload, raw_format, dirty, deleted, pending_operation,
                      local_revision, created_at, updated_at)
    VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
    ON CONFLICT(id) DO UPDATE SET
      title=excluded.title, notes=excluded.notes, due_date=excluded.due_date,
      due_utc=excluded.due_utc, completed=excluded.completed,
      completed_at=excluded.completed_at, priority=excluded.priority,
      dirty=excluded.dirty, deleted=excluded.deleted,
      pending_operation=excluded.pending_operation,
      local_revision=excluded.local_revision, updated_at=excluded.updated_at
  )SQL"));
  query.addBindValue(task->id);
  query.addBindValue(task->listId);
  query.addBindValue(nonNull(task->remoteId));
  query.addBindValue(nonNull(task->uid));
  query.addBindValue(nonNull(task->etag));
  query.addBindValue(task->title);
  query.addBindValue(nonNull(task->notes));
  query.addBindValue(task->dueDate.isValid() ? task->dueDate.toString(Qt::ISODate)
                                             : QStringLiteral(""));
  query.addBindValue(isoUtc(task->dueUtc));
  query.addBindValue(task->completed);
  query.addBindValue(isoUtc(task->completedAt));
  query.addBindValue(task->priority);
  query.addBindValue(nonNull(task->parentId));
  query.addBindValue(nonNull(task->position));
  query.addBindValue(nonNull(task->rawPayload));
  query.addBindValue(nonNull(task->rawFormat));
  query.addBindValue(task->dirty);
  query.addBindValue(false);
  query.addBindValue(nonNull(task->pendingOperation));
  query.addBindValue(task->localRevision);
  query.addBindValue(isoUtc(task->createdAt));
  query.addBindValue(isoUtc(task->updatedAt));
  if (!query.exec()) {
    setError(errorMessage, sqlError(query, QStringLiteral("save task")));
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

bool Database::removeLocalTask(const QString& taskId, QString* errorMessage) {
  QString lookupError;
  const Task existing = task(taskId, &lookupError);
  if (existing.id.isEmpty()) {
    setError(errorMessage, lookupError);
    return false;
  }
  const TaskList list = taskList(existing.listId, &lookupError);
  if (list.readOnly) {
    setError(errorMessage, QStringLiteral("This task list is read-only"));
    return false;
  }
  const bool local = list.capabilities.value(QStringLiteral("provider")).toString() ==
                     QStringLiteral("local");
  QSqlQuery query(m_database);
  if (local) {
    // Device-only tasks have nothing on a provider to remove.
    query.prepare(QStringLiteral("DELETE FROM tasks WHERE id=?"));
    query.addBindValue(taskId);
  } else {
    // Even a task without a provider identity yet keeps a tombstone: its
    // upload may already be under way, and once that lands the removal
    // still has to reach the provider. A sync finding the tombstone with
    // nothing uploaded simply drops it.
    query.prepare(QStringLiteral(
        "UPDATE tasks SET deleted=1, dirty=1, pending_operation='remove', "
        "local_revision=local_revision+1, updated_at=? WHERE id=?"));
    query.addBindValue(isoUtc(QDateTime::currentDateTimeUtc()));
    query.addBindValue(taskId);
  }
  if (!query.exec()) {
    setError(errorMessage, sqlError(query, QStringLiteral("remove task")));
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

TaskList Database::taskListByRemoteId(const QString& accountId, const QString& remoteId,
                                      QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(
      QStringLiteral("SELECT %1 FROM task_lists l JOIN accounts a ON "
                     "a.id=l.account_id WHERE l.account_id=? AND l.remote_id=?")
          .arg(kTaskListColumns));
  query.addBindValue(accountId);
  query.addBindValue(remoteId);
  if (!query.exec()) {
    setError(errorMessage, sqlError(query, QStringLiteral("find task list")));
    return {};
  }
  return query.next() ? taskListFromQuery(query) : TaskList{};
}

bool Database::removeTaskList(const QString& listId, QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("DELETE FROM task_lists WHERE id=?"));
  query.addBindValue(listId);
  if (!query.exec()) {
    setError(errorMessage, sqlError(query, QStringLiteral("remove task list")));
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

QList<Task> Database::pendingTaskWrites(const QString& listId,
                                        QString* errorMessage) const {
  QList<Task> result;
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("SELECT %1 FROM tasks WHERE list_id=? AND "
                               "pending_operation<>'' ORDER BY updated_at, id")
                    .arg(kTaskColumns));
  query.addBindValue(listId);
  if (!query.exec()) {
    setError(errorMessage, sqlError(query, QStringLiteral("list pending tasks")));
    return result;
  }
  while (query.next()) {
    result.append(taskFromQuery(query));
  }
  return result;
}

bool Database::applyRemoteTasks(const QString& listId, const QList<Task>& remote,
                                const bool complete, bool* changed,
                                QString* errorMessage) {
  if (changed != nullptr) {
    *changed = false;
  }
  if (!m_database.transaction()) {
    setError(errorMessage, m_database.lastError().text());
    return false;
  }
  const auto fail = [this, errorMessage](const QSqlQuery& query,
                                         const QString& context) {
    setError(errorMessage, sqlError(query, context));
    m_database.rollback();
    return false;
  };
  bool anyChange = false;
  QSet<QString> seen;
  const QString now = isoUtc(QDateTime::currentDateTimeUtc());
  for (const Task& incoming : remote) {
    if (incoming.remoteId.isEmpty()) {
      continue;
    }
    seen.insert(incoming.remoteId);
    QSqlQuery existing(m_database);
    existing.prepare(
        QStringLiteral("SELECT id, etag, pending_operation FROM tasks "
                       "WHERE list_id=? AND remote_id=?"));
    existing.addBindValue(listId);
    existing.addBindValue(incoming.remoteId);
    if (!existing.exec()) {
      return fail(existing, QStringLiteral("find remote task"));
    }
    const bool known = existing.next();
    if (known &&
        (!existing.value(2).toString().isEmpty() ||
         (!incoming.etag.isEmpty() && existing.value(1).toString() == incoming.etag))) {
      // A local write is still owed, or nothing changed.
      continue;
    }
    QSqlQuery write(m_database);
    if (known) {
      write.prepare(QStringLiteral(R"SQL(
        UPDATE tasks SET uid=?, etag=?, title=?, notes=?, due_date=?, due_utc=?,
          completed=?, completed_at=?, priority=?, parent_id=?, position=?,
          raw_payload=?, raw_format=?, dirty=0, deleted=0,
          local_revision=local_revision+1, updated_at=?
        WHERE id=?
      )SQL"));
    } else {
      write.prepare(QStringLiteral(R"SQL(
        INSERT INTO tasks(uid, etag, title, notes, due_date, due_utc, completed,
                          completed_at, priority, parent_id, position, raw_payload,
                          raw_format, updated_at, id, list_id, remote_id,
                          local_revision, created_at)
        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?)
      )SQL"));
    }
    write.addBindValue(nonNull(incoming.uid));
    write.addBindValue(nonNull(incoming.etag));
    write.addBindValue(nonNull(incoming.title));
    write.addBindValue(nonNull(incoming.notes));
    write.addBindValue(incoming.dueDate.isValid()
                           ? incoming.dueDate.toString(Qt::ISODate)
                           : QStringLiteral(""));
    write.addBindValue(isoUtc(incoming.dueUtc));
    write.addBindValue(incoming.completed);
    write.addBindValue(isoUtc(incoming.completedAt));
    write.addBindValue(incoming.priority);
    write.addBindValue(nonNull(incoming.parentId));
    write.addBindValue(nonNull(incoming.position));
    write.addBindValue(nonNull(incoming.rawPayload));
    write.addBindValue(nonNull(incoming.rawFormat));
    write.addBindValue(incoming.updatedAt.isValid() ? isoUtc(incoming.updatedAt) : now);
    if (known) {
      write.addBindValue(existing.value(0).toString());
    } else {
      write.addBindValue(newUuid());
      write.addBindValue(listId);
      write.addBindValue(incoming.remoteId);
      write.addBindValue(incoming.createdAt.isValid() ? isoUtc(incoming.createdAt)
                                                      : now);
    }
    if (!write.exec()) {
      return fail(write, QStringLiteral("store remote task"));
    }
    anyChange = true;
  }
  if (complete) {
    QSqlQuery stale(m_database);
    stale.prepare(
        QStringLiteral("SELECT id, remote_id FROM tasks WHERE list_id=? AND "
                       "remote_id<>'' AND pending_operation=''"));
    stale.addBindValue(listId);
    if (!stale.exec()) {
      return fail(stale, QStringLiteral("find removed tasks"));
    }
    QStringList removed;
    while (stale.next()) {
      if (!seen.contains(stale.value(1).toString())) {
        removed.append(stale.value(0).toString());
      }
    }
    for (const QString& id : std::as_const(removed)) {
      QSqlQuery remove(m_database);
      remove.prepare(QStringLiteral("DELETE FROM tasks WHERE id=?"));
      remove.addBindValue(id);
      if (!remove.exec()) {
        return fail(remove, QStringLiteral("remove task"));
      }
      anyChange = true;
    }
  }
  if (anyChange && !bumpChangeRevision(errorMessage)) {
    m_database.rollback();
    return false;
  }
  if (!m_database.commit()) {
    setError(errorMessage, m_database.lastError().text());
    return false;
  }
  if (changed != nullptr) {
    *changed = anyChange;
  }
  return true;
}

bool Database::completeTaskWrite(const QString& taskId, const qint64 sentRevision,
                                 const QString& remoteId, const QString& etag,
                                 const QString& rawPayload, const QString& rawFormat,
                                 const Task* uploaded, QString* errorMessage) {
  QSqlQuery current(m_database);
  current.prepare(
      QStringLiteral("SELECT pending_operation, local_revision FROM tasks WHERE id=?"));
  current.addBindValue(taskId);
  if (!current.exec()) {
    setError(errorMessage, sqlError(current, QStringLiteral("find sent task")));
    return false;
  }
  if (!current.next()) {
    return true;
  }
  const QString operation = current.value(0).toString();
  const bool editedSince = current.value(1).toLongLong() != sentRevision;
  QSqlQuery write(m_database);
  if (operation == QStringLiteral("remove") && !editedSince) {
    write.prepare(QStringLiteral("DELETE FROM tasks WHERE id=?"));
    write.addBindValue(taskId);
  } else {
    // A later edit still has to be sent, now as an update of this resource.
    write.prepare(QStringLiteral(R"SQL(
      UPDATE tasks SET remote_id=?, etag=?, raw_payload=?, raw_format=?,
        dirty=?, pending_operation=?
      WHERE id=?
    )SQL"));
    write.addBindValue(remoteId);
    write.addBindValue(nonNull(etag));
    write.addBindValue(nonNull(rawPayload));
    write.addBindValue(nonNull(rawFormat));
    write.addBindValue(editedSince);
    write.addBindValue(editedSince ? (operation == QStringLiteral("remove")
                                          ? QStringLiteral("remove")
                                          : QStringLiteral("update"))
                                   : QStringLiteral(""));
    write.addBindValue(taskId);
  }
  if (!write.exec()) {
    setError(errorMessage, sqlError(write, QStringLiteral("record task write")));
    return false;
  }
  if (uploaded != nullptr && !editedSince && operation != QStringLiteral("remove")) {
    QSqlQuery fields(m_database);
    fields.prepare(QStringLiteral(R"SQL(
      UPDATE tasks SET title=?, notes=?, due_date=?, due_utc=?, completed=?,
        completed_at=?, priority=? WHERE id=?
    )SQL"));
    fields.addBindValue(nonNull(uploaded->title));
    fields.addBindValue(nonNull(uploaded->notes));
    fields.addBindValue(uploaded->dueDate.isValid()
                            ? uploaded->dueDate.toString(Qt::ISODate)
                            : QStringLiteral(""));
    fields.addBindValue(isoUtc(uploaded->dueUtc));
    fields.addBindValue(uploaded->completed);
    fields.addBindValue(isoUtc(uploaded->completedAt));
    fields.addBindValue(uploaded->priority);
    fields.addBindValue(taskId);
    if (!fields.exec()) {
      setError(errorMessage, sqlError(fields, QStringLiteral("record uploaded task")));
      return false;
    }
  }
  return bumpChangeRevision(errorMessage);
}

}  // namespace omacalendar
