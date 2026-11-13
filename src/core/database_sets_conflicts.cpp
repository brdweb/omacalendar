// Database: calendar sets and sync conflicts.
// Split out of database.cpp; shared helpers live in database_p.h.

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "core/database.h"
#include "core/database_p.h"

namespace omacalendar {

using namespace detail;

QList<CalendarSet> Database::calendarSets(QString* errorMessage) const {
  QList<CalendarSet> result;
  QSqlQuery query(m_database);
  if (!query.exec(QStringLiteral(R"SQL(
    SELECT id,name,is_default,default_calendar_id,created_at,updated_at
    FROM calendar_sets ORDER BY is_default DESC, name COLLATE NOCASE, id
  )SQL"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list calendar sets"));
    }
    return result;
  }
  while (query.next()) {
    CalendarSet set;
    set.id = query.value(0).toString();
    set.name = query.value(1).toString();
    set.isDefault = query.value(2).toBool();
    set.defaultCalendarId = query.value(3).toString();
    set.createdAt = dateTimeFromIso(query.value(4).toString());
    set.updatedAt = dateTimeFromIso(query.value(5).toString());
    QSqlQuery members(m_database);
    members.prepare(
        QStringLiteral("SELECT calendar_id FROM calendar_set_members WHERE set_id=? "
                       "ORDER BY position, calendar_id"));
    members.addBindValue(set.id);
    if (!members.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(members, QStringLiteral("list calendar set members"));
      }
      return {};
    }
    while (members.next()) {
      set.calendarIds.append(members.value(0).toString());
    }
    result.append(set);
  }
  return result;
}

bool Database::upsertCalendarSet(CalendarSet* set, QString* errorMessage) {
  if (set == nullptr || set->name.trimmed().isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Calendar set name is required");
    }
    return false;
  }
  const QDateTime now = nowUtc();
  if (set->id.isEmpty()) {
    set->id = newUuid();
  }
  if (!set->createdAt.isValid()) {
    set->createdAt = now;
  }
  set->updatedAt = now;
  set->calendarIds.removeDuplicates();
  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO calendar_sets
      (id,name,is_default,default_calendar_id,created_at,updated_at)
    VALUES (?,?,?,?,?,?)
    ON CONFLICT(id) DO UPDATE SET name=excluded.name,
      default_calendar_id=excluded.default_calendar_id,
      updated_at=excluded.updated_at
  )SQL"));
  query.addBindValue(set->id);
  query.addBindValue(set->name.trimmed());
  query.addBindValue(set->isDefault);
  query.addBindValue(
      set->defaultCalendarId.isEmpty() ? QVariant() : QVariant(set->defaultCalendarId));
  query.addBindValue(isoUtc(set->createdAt));
  query.addBindValue(isoUtc(set->updatedAt));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("save calendar set"));
    }
    m_database.rollback();
    return false;
  }
  QSqlQuery clear(m_database);
  clear.prepare(QStringLiteral("DELETE FROM calendar_set_members WHERE set_id=?"));
  clear.addBindValue(set->id);
  if (!clear.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(clear, QStringLiteral("replace calendar set members"));
    }
    m_database.rollback();
    return false;
  }
  for (qsizetype i = 0; i < set->calendarIds.size(); ++i) {
    QSqlQuery member(m_database);
    member.prepare(QStringLiteral(
        "INSERT INTO calendar_set_members(set_id,calendar_id,position) VALUES(?,?,?)"));
    member.addBindValue(set->id);
    member.addBindValue(set->calendarIds.at(i));
    member.addBindValue(i);
    if (!member.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(member, QStringLiteral("save calendar set member"));
      }
      m_database.rollback();
      return false;
    }
  }
  if (!bumpChangeRevision(errorMessage) || !m_database.commit()) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = m_database.lastError().text();
    }
    m_database.rollback();
    return false;
  }
  return true;
}

bool Database::removeCalendarSet(const QString& setId, QString* errorMessage) {
  if (setId == QStringLiteral("all-calendars")) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("The All Calendars set cannot be removed");
    }
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("DELETE FROM calendar_sets WHERE id=?"));
  query.addBindValue(setId);
  if (!query.exec() || query.numRowsAffected() == 0) {
    if (errorMessage != nullptr) {
      *errorMessage = query.lastError().isValid()
                          ? sqlError(query, QStringLiteral("remove calendar set"))
                          : QStringLiteral("Calendar set not found");
    }
    return false;
  }
  const QJsonValue active = setting(QStringLiteral("active_calendar_set"));
  if (active.toString() == setId &&
      !setSetting(QStringLiteral("active_calendar_set"),
                  QStringLiteral("all-calendars"), errorMessage)) {
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

bool Database::activateCalendarSet(const QString& setId, QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("SELECT 1 FROM calendar_sets WHERE id=?"));
  query.addBindValue(setId);
  if (!query.exec() || !query.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = query.lastError().isValid()
                          ? sqlError(query, QStringLiteral("activate calendar set"))
                          : QStringLiteral("Calendar set not found");
    }
    return false;
  }
  return setSetting(QStringLiteral("active_calendar_set"), setId, errorMessage) &&
         bumpChangeRevision(errorMessage);
}

QList<Conflict> Database::conflicts(const bool unresolvedOnly,
                                    QString* errorMessage) const {
  QList<Conflict> result;
  QSqlQuery query(m_database);
  QString sql = QStringLiteral(R"SQL(
    SELECT id,event_id,mutation_id,kind,local_revision,remote_revision,
      local_snapshot_json,remote_snapshot_json,state,resolution_revision,
      created_at,resolved_at FROM conflicts
  )SQL");
  if (unresolvedOnly) {
    sql += QStringLiteral(" WHERE state='unresolved'");
  }
  sql += QStringLiteral(" ORDER BY created_at DESC,id DESC");
  if (!query.exec(sql)) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list conflicts"));
    }
    return result;
  }
  while (query.next()) {
    Conflict conflict;
    conflict.id = query.value(0).toLongLong();
    conflict.eventId = query.value(1).toString();
    conflict.mutationId = query.value(2).toLongLong();
    conflict.kind = query.value(3).toString();
    conflict.localRevision = query.value(4).toString();
    conflict.remoteRevision = query.value(5).toString();
    conflict.localSnapshot = parseObject(query.value(6).toString());
    conflict.remoteSnapshot = parseObject(query.value(7).toString());
    conflict.state = query.value(8).toString();
    conflict.resolutionRevision = query.value(9).toLongLong();
    conflict.createdAt = dateTimeFromIso(query.value(10).toString());
    conflict.resolvedAt = dateTimeFromIso(query.value(11).toString());
    result.append(conflict);
  }
  return result;
}

bool Database::resolveConflict(const qint64 id, const QString& strategy,
                               const QJsonObject& mergedEvent, QString* errorMessage) {
  if (strategy != QStringLiteral("keep_remote") &&
      strategy != QStringLiteral("keep_local") && strategy != QStringLiteral("merge")) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Unknown conflict resolution strategy");
    }
    return false;
  }
  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  const auto fail = [this]() {
    m_database.rollback();
    return false;
  };
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT event_id,remote_revision,local_snapshot_json,remote_snapshot_json
    FROM conflicts WHERE id=? AND state='unresolved'
  )SQL"));
  query.addBindValue(id);
  if (!query.exec() || !query.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = query.lastError().isValid()
                          ? sqlError(query, QStringLiteral("load conflict"))
                          : QStringLiteral("Unresolved conflict not found");
    }
    return fail();
  }
  const QString eventId = query.value(0).toString();
  const QString remoteRevision = query.value(1).toString();
  const QJsonObject localSnapshot = parseObject(query.value(2).toString());
  const QJsonObject remoteSnapshot = parseObject(query.value(3).toString());

  if (strategy == QStringLiteral("keep_remote")) {
    QSqlQuery discard(m_database);
    discard.prepare(QStringLiteral(R"SQL(
      UPDATE outbox SET state='done',error_code='conflict_keep_remote',
        error_message='',lease_until='',updated_at=?
      WHERE event_id=? AND state<>'done'
    )SQL"));
    discard.addBindValue(isoUtc(nowUtc()));
    discard.addBindValue(eventId);
    if (!discard.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(discard, QStringLiteral("cancel local mutations"));
      }
      return fail();
    }
    if (remoteRevision == QStringLiteral("deleted") || remoteSnapshot.isEmpty()) {
      // Keep a clean tombstone so the resolved conflict remains auditable;
      // normal event queries exclude deleted rows.
      QSqlQuery remove(m_database);
      remove.prepare(QStringLiteral(R"SQL(
        UPDATE events SET deleted=1,dirty=0,sync_state='clean',updated_at=?
        WHERE id=?
      )SQL"));
      remove.addBindValue(isoUtc(nowUtc()));
      remove.addBindValue(eventId);
      if (!remove.exec()) {
        if (errorMessage != nullptr) {
          *errorMessage = sqlError(remove, QStringLiteral("restore remote deletion"));
        }
        return fail();
      }
    } else {
      Event remote = eventFromJson(remoteSnapshot);
      remote.id = eventId;
      remote.dirty = false;
      remote.syncState = QStringLiteral("clean");
      remote.localRevision = this->event(eventId).localRevision + 1;
      remote.updatedAt = nowUtc();
      if (!upsertEventRecord(remote, errorMessage)) {
        return fail();
      }
    }
  } else {
    Event local = eventFromJson(strategy == QStringLiteral("merge") ? mergedEvent
                                                                    : localSnapshot);
    const Event stored = this->event(eventId);
    local.id = eventId;
    local.calendarId =
        local.calendarId.isEmpty() ? stored.calendarId : local.calendarId;
    local.uid = local.uid.isEmpty() ? stored.uid : local.uid;
    local.remoteId = remoteRevision == QStringLiteral("deleted") ? QStringLiteral("")
                                                                 : stored.remoteId;
    local.etag = remoteRevision == QStringLiteral("deleted") ? QStringLiteral("")
                                                             : remoteRevision;
    local.createdAt = stored.createdAt;
    QSqlQuery discard(m_database);
    discard.prepare(QStringLiteral(R"SQL(
      UPDATE outbox SET state='done',error_code='conflict_resolved',
        error_message='',updated_at=? WHERE event_id=? AND state<>'done'
    )SQL"));
    discard.addBindValue(isoUtc(nowUtc()));
    discard.addBindValue(eventId);
    if (!discard.exec() || !saveLocalEvent(&local,
                                           remoteRevision == QStringLiteral("deleted")
                                               ? OutboxOperation::Create
                                               : OutboxOperation::Update,
                                           errorMessage)) {
      return fail();
    }
  }
  QSqlQuery resolved(m_database);
  resolved.prepare(QStringLiteral(R"SQL(
    UPDATE conflicts SET state=?,resolved_at=?,resolution_revision=?
    WHERE event_id=? AND state='unresolved'
  )SQL"));
  resolved.addBindValue(strategy);
  resolved.addBindValue(isoUtc(nowUtc()));
  resolved.addBindValue(changeRevision() + 1);
  resolved.addBindValue(eventId);
  if (!resolved.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(resolved, QStringLiteral("resolve conflict"));
    }
    return fail();
  }
  if (!bumpChangeRevision(errorMessage) || !m_database.commit()) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = m_database.lastError().text();
    }
    return fail();
  }
  return true;
}

int Database::resolveNewestConflicts(bool* queuedLocalWrites, QString* errorMessage) {
  if (queuedLocalWrites != nullptr) {
    *queuedLocalWrites = false;
  }
  QString queryError;
  const QList<Conflict> unresolved = conflicts(true, &queryError);
  if (!queryError.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = queryError;
    }
    return -1;
  }
  int resolvedCount = 0;
  for (const Conflict& conflict : unresolved) {
    const Event local = eventFromJson(conflict.localSnapshot);
    const Event remote = eventFromJson(conflict.remoteSnapshot);
    const QDateTime localUpdated = local.updatedAt;
    const QDateTime remoteUpdated = remote.updatedAt;
    // Observing a conflict says nothing about when the provider was edited.
    // Missing timestamps (including remote deletion) must retain both choices.
    // iCalendar timestamps have second precision, so subsecond local precision
    // cannot establish an ordering within that same provider timestamp second.
    if (!localUpdated.isValid() || !remoteUpdated.isValid() ||
        localUpdated.toSecsSinceEpoch() == remoteUpdated.toSecsSinceEpoch()) {
      continue;
    }
    const bool keepLocal = localUpdated > remoteUpdated;
    const QString strategy =
        keepLocal ? QStringLiteral("keep_local") : QStringLiteral("keep_remote");
    if (!resolveConflict(conflict.id, strategy, {}, errorMessage)) {
      return -1;
    }
    if (queuedLocalWrites != nullptr && keepLocal) {
      *queuedLocalWrites = true;
    }
    ++resolvedCount;
  }
  return resolvedCount;
}

}  // namespace omacalendar
