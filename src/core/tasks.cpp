#include "core/tasks.h"

#include "core/domain.h"

namespace omacalendar {
namespace {

constexpr qsizetype kMaximumTitleLength = 1024;
constexpr qsizetype kMaximumNotesLength = 16 * 1024;

QString jsonString(const QJsonObject& object, const char* key) {
  return object.value(QString::fromLatin1(key)).toString();
}

}  // namespace

QJsonObject toJson(const TaskList& list) {
  return {
      {QStringLiteral("id"), list.id},
      {QStringLiteral("accountId"), list.accountId},
      {QStringLiteral("name"), list.name},
      {QStringLiteral("color"), list.color},
      {QStringLiteral("readOnly"), list.readOnly},
      {QStringLiteral("enabled"), list.enabled},
      {QStringLiteral("position"), list.position},
      {QStringLiteral("capabilities"), list.capabilities},
      {QStringLiteral("lastSyncAt"), isoUtc(list.lastSyncAt)},
  };
}

QJsonObject toJson(const Task& task) {
  return {
      {QStringLiteral("id"), task.id},
      {QStringLiteral("listId"), task.listId},
      {QStringLiteral("title"), task.title},
      {QStringLiteral("notes"), task.notes},
      {QStringLiteral("dueDate"),
       task.dueDate.isValid() ? task.dueDate.toString(Qt::ISODate) : QString()},
      {QStringLiteral("dueUtc"), isoUtc(task.dueUtc)},
      {QStringLiteral("completed"), task.completed},
      {QStringLiteral("completedAt"), isoUtc(task.completedAt)},
      {QStringLiteral("priority"), task.priority},
      {QStringLiteral("parentId"), task.parentId},
      {QStringLiteral("position"), task.position},
      {QStringLiteral("dirty"), task.dirty},
      {QStringLiteral("localRevision"), task.localRevision},
      {QStringLiteral("createdAt"), isoUtc(task.createdAt)},
      {QStringLiteral("updatedAt"), isoUtc(task.updatedAt)},
  };
}

QJsonObject toStorageJson(const Task& task) {
  QJsonObject result = toJson(task);
  result.insert(QStringLiteral("remoteId"), task.remoteId);
  result.insert(QStringLiteral("uid"), task.uid);
  result.insert(QStringLiteral("etag"), task.etag);
  result.insert(QStringLiteral("rawPayload"), task.rawPayload);
  result.insert(QStringLiteral("rawFormat"), task.rawFormat);
  result.insert(QStringLiteral("deleted"), task.deleted);
  result.insert(QStringLiteral("pendingOperation"), task.pendingOperation);
  return result;
}

TaskList taskListFromJson(const QJsonObject& object) {
  TaskList list;
  list.id = jsonString(object, "id");
  list.accountId = jsonString(object, "accountId");
  list.remoteId = jsonString(object, "remoteId");
  list.name = jsonString(object, "name");
  const QString color = jsonString(object, "color");
  if (!color.isEmpty()) {
    list.color = color;
  }
  list.readOnly = object.value(QStringLiteral("readOnly")).toBool();
  list.enabled = object.value(QStringLiteral("enabled")).toBool(true);
  list.position = object.value(QStringLiteral("position")).toInt();
  list.syncToken = jsonString(object, "syncToken");
  list.capabilities = object.value(QStringLiteral("capabilities")).toObject();
  list.lastSyncAt = dateTimeFromIso(jsonString(object, "lastSyncAt"));
  return list;
}

Task taskFromJson(const QJsonObject& object) {
  Task task;
  task.id = jsonString(object, "id");
  task.listId = jsonString(object, "listId");
  task.remoteId = jsonString(object, "remoteId");
  task.uid = jsonString(object, "uid");
  task.etag = jsonString(object, "etag");
  task.title = jsonString(object, "title");
  task.notes = jsonString(object, "notes");
  task.dueDate = QDate::fromString(jsonString(object, "dueDate"), Qt::ISODate);
  task.dueUtc = dateTimeFromIso(jsonString(object, "dueUtc"));
  task.completed = object.value(QStringLiteral("completed")).toBool();
  task.completedAt = dateTimeFromIso(jsonString(object, "completedAt"));
  task.priority = object.value(QStringLiteral("priority")).toInt();
  task.parentId = jsonString(object, "parentId");
  task.position = jsonString(object, "position");
  task.rawPayload = jsonString(object, "rawPayload");
  task.rawFormat = jsonString(object, "rawFormat");
  task.dirty = object.value(QStringLiteral("dirty")).toBool();
  task.deleted = object.value(QStringLiteral("deleted")).toBool();
  task.pendingOperation = jsonString(object, "pendingOperation");
  task.localRevision = object.value(QStringLiteral("localRevision")).toInteger();
  task.createdAt = dateTimeFromIso(jsonString(object, "createdAt"));
  task.updatedAt = dateTimeFromIso(jsonString(object, "updatedAt"));
  return task;
}

QString validateTask(const Task& task) {
  if (task.title.trimmed().isEmpty()) {
    return QStringLiteral("A task needs a title");
  }
  if (task.title.size() > kMaximumTitleLength) {
    return QStringLiteral("The task title is too long");
  }
  if (task.notes.size() > kMaximumNotesLength) {
    return QStringLiteral("The task notes are too long");
  }
  if (task.priority < 0 || task.priority > 9) {
    return QStringLiteral("Priority must be between 0 and 9");
  }
  if (task.dueUtc.isValid() && !task.dueDate.isValid()) {
    return QStringLiteral("A due time needs a due day");
  }
  return {};
}

}  // namespace omacalendar
