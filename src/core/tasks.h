#pragma once

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace omacalendar {

// The built-in list every installation has, so tasks work without an account.
inline constexpr auto kLocalTaskListId = "local-tasks";

// A to-do list: a CalDAV collection that holds VTODOs, a Google Tasks list,
// or the device-only list.
struct TaskList {
  QString id;
  QString accountId;
  // Provider identity: the collection href for CalDAV, the list id for Google.
  QString remoteId;
  QString name;
  QString color = QStringLiteral("#9ece6a");
  bool readOnly = false;
  bool enabled = true;
  int position = 0;
  QString syncToken;
  QJsonObject capabilities;
  QDateTime lastSyncAt;
};

struct Task {
  QString id;
  QString listId;
  QString remoteId;
  QString uid;
  QString etag;
  QString title;
  QString notes;
  // A due day, and optionally a due time. Google Tasks keeps only the day;
  // CalDAV DUE may carry a time.
  QDate dueDate;
  QDateTime dueUtc;
  bool completed = false;
  QDateTime completedAt;
  // RFC 5545 PRIORITY: 0 is undefined, 1 highest, 9 lowest.
  int priority = 0;
  // Google's parent task, for subtasks; kept so edits do not flatten them.
  QString parentId;
  QString position;
  QString rawPayload;
  QString rawFormat;
  bool dirty = false;
  bool deleted = false;
  // The provider write still owed for this task: "", "create", "update" or
  // "remove".
  QString pendingOperation;
  qint64 localRevision = 0;
  QDateTime createdAt;
  QDateTime updatedAt;
};

QJsonObject toJson(const TaskList& list);
QJsonObject toJson(const Task& task);
// Storage payloads include provider-owned metadata and are never returned
// over IPC.
QJsonObject toStorageJson(const Task& task);
TaskList taskListFromJson(const QJsonObject& object);
Task taskFromJson(const QJsonObject& object);

// Checks the fields a client may set. Returns an empty string when the task
// can be saved, otherwise a message for the user.
QString validateTask(const Task& task);

}  // namespace omacalendar
