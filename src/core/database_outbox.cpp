// Database: outbox queue of pending provider writes and their completion.
// Split out of database.cpp; shared helpers live in database_p.h.

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "core/database.h"
#include "core/database_p.h"

namespace omacalendar {

using namespace detail;

OutboxItem Database::outboxFromQuery(const QSqlQuery& query) const {
  OutboxItem item;
  item.id = query.value(QStringLiteral("id")).toLongLong();
  item.accountId = query.value(QStringLiteral("account_id")).toString();
  item.calendarId = query.value(QStringLiteral("calendar_id")).toString();
  item.eventId = query.value(QStringLiteral("event_id")).toString();
  item.operation =
      outboxOperationFromString(query.value(QStringLiteral("operation")).toString());
  item.state = outboxStateFromString(query.value(QStringLiteral("state")).toString());
  item.idempotencyKey = query.value(QStringLiteral("idempotency_key")).toString();
  item.dependencyId = query.value(QStringLiteral("dependency_id")).toString();
  item.expectedRevision = query.value(QStringLiteral("expected_revision")).toString();
  item.recurrenceScope = query.value(QStringLiteral("recurrence_scope")).toString();
  item.sendUpdates = query.value(QStringLiteral("send_updates")).toString();
  item.payload = parseObject(query.value(QStringLiteral("payload_json")).toString());
  item.attempts = query.value(QStringLiteral("attempts")).toInt();
  item.nextAttemptAt =
      dateTimeFromIso(query.value(QStringLiteral("next_attempt_at")).toString());
  item.notBefore =
      dateTimeFromIso(query.value(QStringLiteral("not_before")).toString());
  item.leaseUntil =
      dateTimeFromIso(query.value(QStringLiteral("lease_until")).toString());
  item.errorCode = query.value(QStringLiteral("error_code")).toString();
  item.errorMessage = query.value(QStringLiteral("error_message")).toString();
  item.createdAt =
      dateTimeFromIso(query.value(QStringLiteral("created_at")).toString());
  item.updatedAt =
      dateTimeFromIso(query.value(QStringLiteral("updated_at")).toString());
  return item;
}

QList<OutboxItem> Database::readyOutbox(const int limit, QString* errorMessage) const {
  QList<OutboxItem> result;
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT * FROM outbox
    WHERE state IN ('pending', 'retry_wait')
      AND (next_attempt_at = '' OR next_attempt_at <= ?)
      AND (not_before = '' OR not_before <= ?)
      AND (dependency_id = '' OR NOT EXISTS (
        SELECT 1 FROM outbox dependency
        WHERE CAST(dependency.id AS TEXT)=outbox.dependency_id
          AND dependency.state <> 'done'
      ))
    ORDER BY id LIMIT ?
  )SQL"));
  query.addBindValue(isoUtc(nowUtc()));
  query.addBindValue(isoUtc(nowUtc()));
  query.addBindValue(qBound(1, limit, 1000));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list outbox"));
    }
    return result;
  }
  while (query.next()) {
    result.append(outboxFromQuery(query));
  }
  return result;
}

QList<OutboxItem> Database::outboxItems(const int limit, QString* errorMessage) const {
  QList<OutboxItem> result;
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("SELECT * FROM outbox ORDER BY id DESC LIMIT ?"));
  query.addBindValue(qBound(1, limit, 1000));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list operations"));
    }
    return result;
  }
  while (query.next()) {
    result.append(outboxFromQuery(query));
  }
  return result;
}

bool Database::recoverExpiredOutbox(QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE outbox SET state='pending', lease_until='', next_attempt_at=?,
      error_code='recovered_after_restart',
      error_message='Recovered an interrupted send lease', updated_at=?
    WHERE state='sending'
  )SQL"));
  const QString now = isoUtc(nowUtc());
  query.addBindValue(now);
  query.addBindValue(now);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("recover mutation leases"));
    }
    return false;
  }
  return query.numRowsAffected() == 0 || bumpChangeRevision(errorMessage);
}

bool Database::updateOutboxState(qint64 id, OutboxState state, int attempts,
                                 const QDateTime& nextAttemptAt,
                                 const QString& errorCode, const QString& errorMessage,
                                 QString* databaseError) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE outbox SET state=?, attempts=?, next_attempt_at=?, lease_until=?,
      error_code=?, error_message=?, updated_at=? WHERE id=?
  )SQL"));
  query.addBindValue(outboxStateToString(state));
  query.addBindValue(attempts);
  query.addBindValue(isoUtc(nextAttemptAt));
  query.addBindValue(state == OutboxState::Sending ? isoUtc(nowUtc().addSecs(120))
                                                   : QStringLiteral(""));
  query.addBindValue(errorCode.isNull() ? QStringLiteral("") : errorCode);
  query.addBindValue(errorMessage.isNull() ? QStringLiteral("")
                                           : errorMessage.left(512));
  query.addBindValue(isoUtc(nowUtc()));
  query.addBindValue(id);
  if (!query.exec()) {
    if (databaseError != nullptr) {
      *databaseError = sqlError(query, QStringLiteral("update outbox"));
    }
    return false;
  }
  return query.numRowsAffected() > 0 && bumpChangeRevision(databaseError);
}

bool Database::completeOutbox(const qint64 id, const Event* remoteEvent,
                              QString* errorMessage) {
  return completeOutboxInternal(id, remoteEvent, true, errorMessage);
}

bool Database::completeOutboxWithRemoteSyncBatch(
    const qint64 id, const Event* remoteEvent, const Calendar& calendar,
    const QList<Event>& events, const QStringList& deletedRemoteIds,
    const QStringList& prunedRemoteIds, QString* errorMessage,
    const QList<ProviderResource>& providerResources) {
  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  if (!completeOutboxInternal(id, remoteEvent, false, errorMessage) ||
      !applyRemoteSyncBatch(calendar, events, deletedRemoteIds, prunedRemoteIds,
                            errorMessage, providerResources)) {
    m_database.rollback();
    return false;
  }
  if (!m_database.commit()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    m_database.rollback();
    return false;
  }
  return true;
}

bool Database::completeOutboxInternal(const qint64 id, const Event* remoteEvent,
                                      const bool manageTransaction,
                                      QString* errorMessage) {
  if (manageTransaction && !m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  const auto rollback = [this, manageTransaction]() {
    if (manageTransaction) {
      m_database.rollback();
    }
  };
  QSqlQuery itemQuery(m_database);
  itemQuery.prepare(QStringLiteral(R"SQL(
    SELECT o.event_id,o.operation,e.calendar_id,e.uid,e.recurrence_id,e.remote_id
    FROM outbox o LEFT JOIN events e ON e.id=o.event_id WHERE o.id=?
  )SQL"));
  itemQuery.addBindValue(id);
  if (!itemQuery.exec() || !itemQuery.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = itemQuery.lastError().isValid()
                          ? itemQuery.lastError().text()
                          : QStringLiteral("Outbox item not found");
    }
    rollback();
    return false;
  }
  const QString eventId = itemQuery.value(0).toString();
  const OutboxOperation operation =
      outboxOperationFromString(itemQuery.value(1).toString());
  const QString eventCalendarId = itemQuery.value(2).toString();
  const QString eventUid = itemQuery.value(3).toString();
  const QString eventRecurrenceId = itemQuery.value(4).toString();
  const QString eventRemoteId = itemQuery.value(5).toString();

  QSqlQuery finishQuery(m_database);
  finishQuery.prepare(
      QStringLiteral("UPDATE outbox SET state='done', lease_until='', error_code='', "
                     "error_message='', updated_at=? WHERE id=?"));
  finishQuery.addBindValue(isoUtc(nowUtc()));
  finishQuery.addBindValue(id);
  if (!finishQuery.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(finishQuery, QStringLiteral("finish outbox"));
    }
    rollback();
    return false;
  }

  QSqlQuery laterQuery(m_database);
  laterQuery.prepare(QStringLiteral(R"SQL(
    SELECT COUNT(*) FROM outbox WHERE event_id=? AND id<>?
      AND state IN ('pending','sending','retry_wait','blocked')
  )SQL"));
  laterQuery.addBindValue(eventId);
  laterQuery.addBindValue(id);
  if (!laterQuery.exec() || !laterQuery.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(laterQuery, QStringLiteral("check later outbox"));
    }
    rollback();
    return false;
  }
  const bool hasLaterMutation = laterQuery.value(0).toInt() > 0;

  if (operation == OutboxOperation::Remove && !hasLaterMutation) {
    QSqlQuery removalQuery(m_database);
    if (!eventRecurrenceId.isEmpty()) {
      // A recurrence cancellation is part of the canonical series state. Keep
      // the clean tombstone so later expansion continues to suppress the
      // occurrence after provider/local acknowledgement and daemon restart.
      removalQuery.prepare(QStringLiteral(R"SQL(
        UPDATE events SET dirty=0,deleted=1,sync_state='clean',updated_at=?
        WHERE id=?
      )SQL"));
      removalQuery.addBindValue(isoUtc(nowUtc()));
      removalQuery.addBindValue(eventId);
    } else if (!eventCalendarId.isEmpty() && !eventUid.isEmpty()) {
      // Removing a master removes its retained exceptions as one logical
      // series, preventing non-deleted exceptions from resurfacing as orphans.
      removalQuery.prepare(
          QStringLiteral("DELETE FROM events WHERE calendar_id=? AND uid=?"));
      removalQuery.addBindValue(eventCalendarId);
      removalQuery.addBindValue(eventUid);
    } else {
      removalQuery.prepare(QStringLiteral("DELETE FROM events WHERE id=?"));
      removalQuery.addBindValue(eventId);
    }
    if (!removalQuery.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(removalQuery, QStringLiteral("finish removal"));
      }
      rollback();
      return false;
    }
    if (!eventCalendarId.isEmpty() && !eventRemoteId.isEmpty() &&
        !removeOrphanedProviderResources(eventCalendarId, errorMessage)) {
      rollback();
      return false;
    }
  } else if (remoteEvent != nullptr) {
    if (hasLaterMutation) {
      Event current = event(eventId, errorMessage);
      if (current.id.isEmpty()) {
        rollback();
        return false;
      }
      current.recurrenceId = remoteEvent->recurrenceId;
      const RecurrenceStoredBounds bounds = recurrenceBounds(current);
      QSqlQuery revisionQuery(m_database);
      revisionQuery.prepare(QStringLiteral(R"SQL(
        UPDATE events SET remote_id=?, etag=?, recurrence_id=?,
          series_until_utc=?, series_until_date=?,
          original_start_utc=?, original_start_date=?,
          raw_payload=?, raw_format=?, dirty=1
        WHERE id=?
      )SQL"));
      revisionQuery.addBindValue(nonNull(remoteEvent->remoteId));
      revisionQuery.addBindValue(nonNull(remoteEvent->etag));
      revisionQuery.addBindValue(nonNull(remoteEvent->recurrenceId));
      revisionQuery.addBindValue(bounds.untilUtc);
      revisionQuery.addBindValue(bounds.untilDate);
      revisionQuery.addBindValue(bounds.originalUtc);
      revisionQuery.addBindValue(bounds.originalDate);
      revisionQuery.addBindValue(nonNull(remoteEvent->rawPayload));
      revisionQuery.addBindValue(nonNull(remoteEvent->rawFormat));
      // Advancing an earlier acknowledgement must not redate the newer local
      // edit that remains queued; its content has not changed here.
      revisionQuery.addBindValue(eventId);
      if (!revisionQuery.exec()) {
        if (errorMessage != nullptr) {
          *errorMessage =
              sqlError(revisionQuery, QStringLiteral("update remote revision"));
        }
        rollback();
        return false;
      }
      QSqlQuery expectedQuery(m_database);
      expectedQuery.prepare(QStringLiteral(R"SQL(
        UPDATE outbox SET expected_revision=? WHERE event_id=? AND id<>?
          AND state IN ('pending','retry_wait')
      )SQL"));
      expectedQuery.addBindValue(nonNull(remoteEvent->etag));
      expectedQuery.addBindValue(eventId);
      expectedQuery.addBindValue(id);
      if (!expectedQuery.exec()) {
        if (errorMessage != nullptr) {
          *errorMessage =
              sqlError(expectedQuery, QStringLiteral("advance queued revision"));
        }
        rollback();
        return false;
      }
    } else {
      Event acknowledged = *remoteEvent;
      acknowledged.id = eventId;
      acknowledged.dirty = false;
      acknowledged.deleted = false;
      if (!acknowledged.createdAt.isValid()) {
        const Event existing = event(eventId);
        acknowledged.createdAt = existing.createdAt;
      }
      acknowledged.updatedAt = nowUtc();
      if (!upsertEventRecord(acknowledged, errorMessage)) {
        rollback();
        return false;
      }
    }
  } else if (!hasLaterMutation) {
    QSqlQuery cleanQuery(m_database);
    cleanQuery.prepare(QStringLiteral("UPDATE events SET dirty=0 WHERE id=?"));
    cleanQuery.addBindValue(eventId);
    if (!cleanQuery.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(cleanQuery, QStringLiteral("clean event"));
      }
      rollback();
      return false;
    }
  }
  if (!bumpChangeRevision(errorMessage)) {
    rollback();
    return false;
  }
  if (manageTransaction && !m_database.commit()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    m_database.rollback();
    return false;
  }
  return true;
}

bool Database::discardOutbox(const qint64 id, QString* errorMessage) {
  QSqlQuery item(m_database);
  item.prepare(QStringLiteral(
      "SELECT event_id,operation,state,payload_json FROM outbox WHERE id=?"));
  item.addBindValue(id);
  if (!item.exec() || !item.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = item.lastError().isValid()
                          ? sqlError(item, QStringLiteral("load operation"))
                          : QStringLiteral("Operation not found");
    }
    return false;
  }
  if (item.value(2).toString() == QStringLiteral("sending")) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("An operation already being sent cannot be discarded");
    }
    return false;
  }
  const QString eventId = item.value(0).toString();
  const OutboxOperation operation = outboxOperationFromString(item.value(1).toString());
  const QJsonObject payload = parseObject(item.value(3).toString());
  QSqlQuery savepoint(m_database);
  if (!savepoint.exec(QStringLiteral("SAVEPOINT discard_outbox"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(savepoint, QStringLiteral("start discard"));
    }
    return false;
  }
  const auto rollback = [this]() {
    QSqlQuery query(m_database);
    query.exec(QStringLiteral("ROLLBACK TO discard_outbox"));
    query.exec(QStringLiteral("RELEASE discard_outbox"));
  };
  QSqlQuery discard(m_database);
  discard.prepare(QStringLiteral(R"SQL(
    UPDATE outbox SET state='done',error_code='discarded',error_message='',
      lease_until='',updated_at=? WHERE id=?
  )SQL"));
  discard.addBindValue(isoUtc(nowUtc()));
  discard.addBindValue(id);
  if (!discard.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(discard, QStringLiteral("discard operation"));
    }
    rollback();
    return false;
  }
  QSqlQuery dependents(m_database);
  dependents.prepare(QStringLiteral(R"SQL(
    UPDATE outbox SET state='blocked', lease_until='', next_attempt_at='',
      error_code='dependency_discarded',
      error_message='A prerequisite operation was discarded', updated_at=?
    WHERE dependency_id=?
      AND state IN ('pending','retry_wait','blocked')
  )SQL"));
  dependents.addBindValue(isoUtc(nowUtc()));
  dependents.addBindValue(QString::number(id));
  if (!dependents.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(dependents, QStringLiteral("block discarded dependents"));
    }
    rollback();
    return false;
  }
  if (operation == OutboxOperation::Remove) {
    QSqlQuery restore(m_database);
    restore.prepare(QStringLiteral(
        "UPDATE events SET deleted=0,dirty=0,sync_state='clean',updated_at=? "
        "WHERE id=?"));
    restore.addBindValue(isoUtc(nowUtc()));
    restore.addBindValue(eventId);
    if (!restore.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(restore, QStringLiteral("undo event deletion"));
      }
      rollback();
      return false;
    }
  } else if (operation == OutboxOperation::Move) {
    const QJsonObject metadata = payload.value(QStringLiteral("_move")).toObject();
    Event source =
        eventFromJson(metadata.value(QStringLiteral("sourceEvent")).toObject());
    if (source.id.isEmpty() || source.id != eventId || source.calendarId.isEmpty()) {
      if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("The move source snapshot is unavailable");
      }
      rollback();
      return false;
    }
    const Event current = event(eventId);
    source.dirty = false;
    source.deleted = false;
    source.syncState = QStringLiteral("clean");
    source.localRevision = qMax(source.localRevision, current.localRevision) + 1;
    source.updatedAt = nowUtc();
    if (!upsertEventRecord(source, errorMessage)) {
      rollback();
      return false;
    }
  }
  if (!bumpChangeRevision(errorMessage)) {
    rollback();
    return false;
  }
  QSqlQuery release(m_database);
  if (!release.exec(QStringLiteral("RELEASE discard_outbox"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(release, QStringLiteral("commit discard"));
    }
    rollback();
    return false;
  }
  return true;
}

}  // namespace omacalendar
