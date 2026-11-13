// Database: reminder jobs, notification deliveries and invitation baselines.
// Split out of database.cpp; shared helpers live in database_p.h.

#include <QHash>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <optional>

#include "core/database.h"
#include "core/database_p.h"

namespace omacalendar {

using namespace detail;

namespace {

struct DesiredReminderJob {
  QString occurrenceId;
  qsizetype index = 0;
  QDateTime fireAt;
  QString fingerprint;
};

// The (occurrence, alarm index) slot a stored job belongs to. Fingerprints are
// "<event id>:<occurrence id>:<alarm index>:<fire time>"; the occurrence id is
// stored separately, so the index is the segment right after that prefix.
QString reminderSlotKey(const QString& eventId, const QString& occurrenceId,
                        const QString& fingerprint) {
  const QString prefix = eventId + QLatin1Char(':') + occurrenceId + QLatin1Char(':');
  if (!fingerprint.startsWith(prefix)) {
    return {};
  }
  return occurrenceId + QLatin1Char('\x1f') +
         fingerprint.mid(prefix.size()).section(QLatin1Char(':'), 0, 0);
}

}  // namespace

bool Database::rebuildReminderJobs(const Event& event, QString* errorMessage,
                                   const QDateTime& referenceNow,
                                   const ReminderRebuildMode mode, bool* changed) {
  if (changed != nullptr) {
    *changed = false;
  }
  const QDateTime now = referenceNow.isValid() ? referenceNow.toUTC() : nowUtc();
  QList<Event> occurrences{event};
  if (!event.deleted && !event.reminders.isEmpty() && !event.recurrenceRule.isEmpty() &&
      event.recurrenceId.isEmpty()) {
    const RecurrenceExpansionResult expansion =
        RecurrenceExpander::expand({event}, now.addDays(-2), now.addYears(2), 2000);
    if (expansion.truncated) {
      if (errorMessage != nullptr) {
        *errorMessage =
            QStringLiteral("Reminder expansion exceeded its safe work limit");
      }
      return false;
    }
    occurrences = expansion.occurrences;
  }

  // Alarms that would fire at the same instant collapse into one, and only the
  // first kMaximumRemindersPerEvent distinct triggers are scheduled. Each kept
  // alarm retains its original index so existing fingerprints stay stable.
  QList<qsizetype> alarmIndexes;
  {
    QSet<QString> seenTriggers;
    for (qsizetype index = 0; index < event.reminders.size() &&
                              alarmIndexes.size() < kMaximumRemindersPerEvent;
         ++index) {
      const QString key = reminderTriggerKey(event.reminders.at(index));
      if (key.isEmpty() || seenTriggers.contains(key)) {
        continue;
      }
      seenTriggers.insert(key);
      alarmIndexes.append(index);
    }
  }

  QList<DesiredReminderJob> desired;
  if (!event.deleted) {
    for (const Event& occurrence : std::as_const(occurrences)) {
      const QDateTime eventStart =
          occurrence.allDay
              ? QDateTime(occurrence.startDate, QTime(0, 0), QTimeZone::LocalTime)
                    .toUTC()
              : occurrence.startUtc;
      if (!eventStart.isValid()) {
        continue;
      }
      const QString occurrenceId =
          occurrence.recurrenceId.isEmpty()
              ? (occurrence.allDay ? occurrence.startDate.toString(Qt::ISODate)
                                   : isoUtc(occurrence.startUtc))
              : occurrence.recurrenceId;
      for (const qsizetype index : std::as_const(alarmIndexes)) {
        const QJsonValue value = event.reminders.at(index);
        QDateTime fireAt;
        if (const std::optional<qint64> minutesBefore = reminderOffsetMinutes(value);
            minutesBefore.has_value()) {
          // 64-bit arithmetic on an already clamped offset cannot overflow.
          fireAt = eventStart.addSecs(-*minutesBefore * 60);
        } else if (value.isObject()) {
          fireAt =
              dateTimeFromIso(value.toObject().value(QStringLiteral("at")).toString());
        }
        if (!fireAt.isValid()) {
          continue;
        }
        const QString fingerprint = QStringLiteral("%1:%2:%3:%4")
                                        .arg(event.id, occurrenceId)
                                        .arg(index)
                                        .arg(isoUtc(fireAt));
        desired.append({occurrenceId, index, fireAt, fingerprint});
      }
    }
  }

  // A reconcile never re-creates a job for an (occurrence, alarm) slot that is
  // already settled — delivered, dismissed, claimed or snoozed — even when the
  // recomputed fire time, and so the fingerprint, differs.
  QSet<QString> settledSlots;
  QHash<QString, qint64> pendingByFingerprint;
  if (mode == ReminderRebuildMode::Reconcile) {
    QSqlQuery existing(m_database);
    existing.prepare(
        QStringLiteral("SELECT id,fingerprint,occurrence_id,state FROM reminder_jobs "
                       "WHERE event_id=?"));
    existing.addBindValue(event.id);
    if (!existing.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(existing, QStringLiteral("read event reminders"));
      }
      return false;
    }
    while (existing.next()) {
      const QString fingerprint = existing.value(1).toString();
      if (existing.value(3).toString() == QStringLiteral("pending")) {
        pendingByFingerprint.insert(fingerprint, existing.value(0).toLongLong());
      } else {
        settledSlots.insert(
            reminderSlotKey(event.id, existing.value(2).toString(), fingerprint));
      }
    }
  } else {
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral(R"SQL(
      DELETE FROM reminder_jobs
      WHERE event_id=? AND state IN ('pending','snoozed')
    )SQL"));
    remove.addBindValue(event.id);
    if (!remove.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(remove, QStringLiteral("replace event reminders"));
      }
      return false;
    }
    if (changed != nullptr && remove.numRowsAffected() > 0) {
      *changed = true;
    }
  }

  // A calendar's pending work is bounded: once other events hold the budget,
  // this event schedules only what still fits, nearest occurrences first.
  if (!desired.isEmpty()) {
    QSqlQuery budget(m_database);
    budget.prepare(QStringLiteral(R"SQL(
      SELECT COUNT(*) FROM reminder_jobs
      WHERE state IN ('pending','snoozed') AND event_id<>?
        AND event_id IN (SELECT id FROM events WHERE calendar_id=?)
    )SQL"));
    budget.addBindValue(event.id);
    budget.addBindValue(event.calendarId);
    if (!budget.exec() || !budget.next()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(budget, QStringLiteral("count calendar reminders"));
      }
      return false;
    }
    const qint64 available = std::max<qint64>(
        0, kMaximumPendingReminderJobsPerCalendar - budget.value(0).toLongLong());
    if (desired.size() > available) {
      qWarning(
          "reminder_job_budget_exceeded: calendar %s keeps %lld of %lld "
          "reminders for event %s",
          qPrintable(event.calendarId), static_cast<long long>(available),
          static_cast<long long>(desired.size()), qPrintable(event.id));
      desired.resize(available);
    }
  }

  QSet<QString> keptFingerprints;
  for (const DesiredReminderJob& job : std::as_const(desired)) {
    if (mode == ReminderRebuildMode::Reconcile) {
      if (pendingByFingerprint.contains(job.fingerprint)) {
        keptFingerprints.insert(job.fingerprint);
        continue;
      }
      if (settledSlots.contains(
              reminderSlotKey(event.id, job.occurrenceId, job.fingerprint))) {
        continue;
      }
    }
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(R"SQL(
      INSERT INTO reminder_jobs
        (event_id,occurrence_id,fingerprint,fire_at,state,event_revision)
      VALUES (?, ?, ?, ?, 'pending', ?)
      ON CONFLICT(fingerprint) DO UPDATE SET
        event_revision=excluded.event_revision,
        fire_at=CASE WHEN reminder_jobs.state IN ('pending','snoozed')
                     THEN excluded.fire_at ELSE reminder_jobs.fire_at END
    )SQL"));
    insert.addBindValue(event.id);
    insert.addBindValue(nonNull(job.occurrenceId));
    insert.addBindValue(job.fingerprint);
    insert.addBindValue(isoUtc(job.fireAt));
    insert.addBindValue(event.localRevision);
    if (!insert.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(insert, QStringLiteral("schedule event reminder"));
      }
      return false;
    }
    if (changed != nullptr) {
      *changed = true;
    }
  }

  if (mode == ReminderRebuildMode::Reconcile) {
    for (auto iterator = pendingByFingerprint.cbegin();
         iterator != pendingByFingerprint.cend(); ++iterator) {
      if (keptFingerprints.contains(iterator.key())) {
        continue;
      }
      QSqlQuery remove(m_database);
      remove.prepare(
          QStringLiteral("DELETE FROM reminder_jobs WHERE id=? AND state='pending'"));
      remove.addBindValue(iterator.value());
      if (!remove.exec()) {
        if (errorMessage != nullptr) {
          *errorMessage = sqlError(remove, QStringLiteral("drop stale reminder"));
        }
        return false;
      }
      if (changed != nullptr && remove.numRowsAffected() > 0) {
        *changed = true;
      }
    }
  }
  return true;
}

QString Database::metadataValue(const QString& key, QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("SELECT value FROM metadata WHERE key=?"));
  query.addBindValue(key);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("read metadata"));
    }
    return {};
  }
  return query.next() ? query.value(0).toString() : QString();
}

bool Database::setMetadataValue(const QString& key, const QString& value,
                                QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO metadata(key,value) VALUES (?,?)
    ON CONFLICT(key) DO UPDATE SET value=excluded.value
  )SQL"));
  query.addBindValue(key);
  query.addBindValue(nonNull(value));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("write metadata"));
    }
    return false;
  }
  return true;
}

bool Database::reconcileReminderJobs(QList<Event> events, const QDateTime& now,
                                     const bool rebuildInstances, bool* changedOut,
                                     QString* errorMessage) {
  if (changedOut != nullptr) {
    *changedOut = false;
  }
  if (events.isEmpty()) {
    return true;
  }
  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  bool changed = false;
  for (Event& event : events) {
    bool eventChanged = false;
    if (!hydrateProviderResource(&event, errorMessage) ||
        (rebuildInstances && !rebuildEventInstances(event, errorMessage, now)) ||
        !rebuildReminderJobs(event, errorMessage, now, ReminderRebuildMode::Reconcile,
                             &eventChanged)) {
      m_database.rollback();
      return false;
    }
    changed = changed || eventChanged;
  }
  if (changed && !bumpChangeRevision(errorMessage)) {
    m_database.rollback();
    return false;
  }
  if (!m_database.commit()) {
    const QString commitError = m_database.lastError().text();
    m_database.rollback();
    if (errorMessage != nullptr) {
      *errorMessage = commitError;
    }
    return false;
  }
  if (changedOut != nullptr) {
    *changedOut = changed;
  }
  return true;
}

bool Database::rebaseAllDayReminderJobs(const QDateTime& now, bool* changed,
                                        QString* errorMessage) {
  // All-day fire times are local midnight converted to UTC when a job is
  // built, so they go stale when the system zone changes. Only pending jobs
  // move; snoozed, claimed, delivered and dismissed slots stay settled.
  QList<Event> events;
  {
    QSqlQuery rows(m_database);
    rows.prepare(QStringLiteral(R"SQL(
      SELECT e.* FROM events e
      WHERE e.deleted=0 AND e.all_day=1 AND e.reminders_json NOT IN ('','[]')
        AND EXISTS (SELECT 1 FROM reminder_jobs j
                    WHERE j.event_id=e.id AND j.state='pending')
      ORDER BY e.id
    )SQL"));
    if (!rows.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(rows, QStringLiteral("read all-day reminder events"));
      }
      return false;
    }
    while (rows.next()) {
      events.append(eventFromQuery(rows));
    }
  }
  return reconcileReminderJobs(std::move(events), now, false, changed, errorMessage);
}

qsizetype Database::extendReminderHorizons(const QDateTime& now,
                                           const QString& afterEventId, const int limit,
                                           QString* lastEventId,
                                           QString* errorMessage) {
  // Instances and reminder jobs are expanded over a window around the time a
  // series was last written. Re-expanding open-ended (or still running) series
  // with reminders keeps an unchanged series reminding indefinitely.
  QList<Event> events;
  {
    QSqlQuery rows(m_database);
    rows.prepare(QStringLiteral(R"SQL(
      SELECT * FROM events
      WHERE deleted=0 AND recurrence_rule<>'' AND recurrence_id=''
        AND reminders_json NOT IN ('','[]') AND id>?
        AND (
          (all_day=0 AND time_kind<>'floating'
           AND COALESCE(series_until_utc,'~')>?)
          OR ((all_day=1 OR time_kind='floating')
              AND COALESCE(series_until_date,'~')>=?)
        )
      ORDER BY id LIMIT ?
    )SQL"));
    rows.addBindValue(nonNull(afterEventId));
    rows.addBindValue(isoUtc(now));
    rows.addBindValue(now.toUTC().date().addDays(-1).toString(Qt::ISODate));
    rows.addBindValue(qBound(1, limit, 1000));
    if (!rows.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(rows, QStringLiteral("read reminder series"));
      }
      return -1;
    }
    while (rows.next()) {
      events.append(eventFromQuery(rows));
    }
  }
  const qsizetype processed = events.size();
  const QString last = events.isEmpty() ? QString() : events.constLast().id;
  if (!reconcileReminderJobs(std::move(events), now, true, nullptr, errorMessage)) {
    return -1;
  }
  if (lastEventId != nullptr) {
    *lastEventId = last;
  }
  return processed;
}

namespace {

ReminderJob reminderFromQuery(const QSqlQuery& query) {
  ReminderJob reminder;
  reminder.id = query.value(QStringLiteral("id")).toLongLong();
  reminder.eventId = query.value(QStringLiteral("event_id")).toString();
  reminder.occurrenceId = query.value(QStringLiteral("occurrence_id")).toString();
  reminder.fingerprint = query.value(QStringLiteral("fingerprint")).toString();
  reminder.fireAt = dateTimeFromIso(query.value(QStringLiteral("fire_at")).toString());
  reminder.snoozedUntil =
      dateTimeFromIso(query.value(QStringLiteral("snoozed_until")).toString());
  reminder.state = query.value(QStringLiteral("state")).toString();
  reminder.eventRevision = query.value(QStringLiteral("event_revision")).toLongLong();
  reminder.claimedAt =
      dateTimeFromIso(query.value(QStringLiteral("claimed_at")).toString());
  reminder.leaseExpiresAt =
      dateTimeFromIso(query.value(QStringLiteral("lease_expires_at")).toString());
  reminder.deliveredAt =
      dateTimeFromIso(query.value(QStringLiteral("delivered_at")).toString());
  return reminder;
}

}  // namespace

QList<ReminderJob> Database::reminders(const int limit, QString* errorMessage) const {
  QList<ReminderJob> result;
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT * FROM reminder_jobs
    WHERE state IN ('pending','snoozed','claimed','delivered')
    ORDER BY COALESCE(NULLIF(snoozed_until,''),fire_at),id LIMIT ?
  )SQL"));
  query.addBindValue(qBound(1, limit, 500));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list reminders"));
    }
    return result;
  }
  while (query.next()) {
    result.append(reminderFromQuery(query));
  }
  return result;
}

QList<ReminderJob> Database::dueReminders(const QDateTime& now, const int limit,
                                          QString* errorMessage) const {
  QList<ReminderJob> result;
  // Pending jobs deliver only when they fired within the grace window or their
  // occurrence is still upcoming. The occurrence identity is matched against
  // event_instances, whose start_utc/start_date columns are canonical UTC
  // values — provider recurrence ids on the job may use non-ISO spellings, so
  // they are never compared lexically. Date-only all-day instances have an
  // empty start_utc and compare via start_date. Snoozed jobs keep the previous
  // behavior — the snooze is explicit user intent.
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT j.* FROM reminder_jobs j
    JOIN events e ON e.id=j.event_id
    JOIN calendars c ON c.id=e.calendar_id
    WHERE c.ignore_alerts=0 AND (
      (
        j.state='pending' AND j.fire_at<=? AND (
          j.fire_at>=?
          OR EXISTS (
            SELECT 1 FROM event_instances ei
            WHERE ei.event_id=j.event_id AND ei.recurrence_id=j.occurrence_id
              AND (ei.start_utc>?
                   OR (ei.start_utc='' AND ei.start_date<>'' AND ei.start_date>?))
          )
        )
      ) OR (j.state='snoozed' AND j.snoozed_until<=?)
    )
    ORDER BY
      CASE WHEN j.state='snoozed' THEN 0 ELSE 1 END,
      CASE WHEN j.state='pending' AND j.fire_at>=? THEN 0 ELSE 1 END,
      COALESCE(NULLIF(j.snoozed_until,''),j.fire_at),j.id LIMIT ?
  )SQL"));
  const QString nowIso = isoUtc(now);
  const QString todayIso = now.toLocalTime().date().toString(Qt::ISODate);
  query.addBindValue(nowIso);
  query.addBindValue(isoUtc(now.addSecs(-kStaleReminderGraceSeconds)));
  query.addBindValue(nowIso);
  query.addBindValue(todayIso);
  query.addBindValue(nowIso);
  query.addBindValue(isoUtc(now.addSecs(-kStaleReminderGraceSeconds)));
  query.addBindValue(qBound(1, limit, 500));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list due reminders"));
    }
    return result;
  }
  while (query.next()) {
    result.append(reminderFromQuery(query));
  }
  return result;
}

bool Database::hasPendingSeriesRemoval(const QString& calendarId, const QString& uid,
                                       QString* errorMessage) const {
  if (calendarId.isEmpty() || uid.isEmpty()) {
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT 1
    FROM outbox o
    JOIN events master ON master.id=o.event_id
    WHERE o.operation='remove'
      AND o.recurrence_scope='series'
      AND o.state IN ('pending','sending','retry_wait','blocked')
      AND master.calendar_id=?
      AND master.uid=?
      AND master.recurrence_id=''
    LIMIT 1
  )SQL"));
  query.addBindValue(calendarId);
  query.addBindValue(uid);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("inspect pending series removal"));
    }
    return false;
  }
  return query.next();
}

bool Database::claimReminderDelivery(const qint64 id, const QString& claimToken,
                                     const QDateTime& claimedAt,
                                     const QDateTime& leaseExpiresAt, bool* claimed,
                                     QString* errorMessage) {
  if (claimed != nullptr) {
    *claimed = false;
  }
  if (claimToken.isEmpty() || !claimedAt.isValid() || !leaseExpiresAt.isValid() ||
      leaseExpiresAt <= claimedAt) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("A valid reminder claim token and lease are required");
    }
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE reminder_jobs
    SET state='claimed',claimed_at=?,claim_token=?,lease_expires_at=?
    WHERE id=? AND (
      (state='pending' AND fire_at<=?) OR
      (state='snoozed' AND snoozed_until<=?)
    )
  )SQL"));
  const QString timestamp = isoUtc(claimedAt);
  query.addBindValue(timestamp);
  query.addBindValue(claimToken);
  query.addBindValue(isoUtc(leaseExpiresAt));
  query.addBindValue(id);
  query.addBindValue(timestamp);
  query.addBindValue(timestamp);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("claim reminder delivery"));
    }
    return false;
  }
  const bool changed = query.numRowsAffected() > 0;
  if (claimed != nullptr) {
    *claimed = changed;
  }
  return !changed || bumpChangeRevision(errorMessage);
}

bool Database::finishReminderDelivery(const qint64 id, const QString& claimToken,
                                      const QDateTime& deliveredAt,
                                      QString* errorMessage) {
  if (claimToken.isEmpty() || !deliveredAt.isValid()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "A valid reminder delivery claim and completion time are required");
    }
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE reminder_jobs
    SET state='delivered',delivered_at=?,snoozed_until='',claimed_at='',
        claim_token='',lease_expires_at=''
    WHERE id=? AND state='claimed' AND claim_token=?
  )SQL"));
  query.addBindValue(isoUtc(deliveredAt));
  query.addBindValue(id);
  query.addBindValue(claimToken);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("finish reminder delivery"));
    }
    return false;
  }
  if (query.numRowsAffected() == 0) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Reminder delivery claim is no longer active");
    }
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

bool Database::releaseReminderDelivery(const qint64 id, const QString& claimToken,
                                       QString* errorMessage) {
  if (claimToken.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("A reminder delivery claim token is required");
    }
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE reminder_jobs
    SET state=CASE WHEN snoozed_until<>'' THEN 'snoozed' ELSE 'pending' END,
        claimed_at='',claim_token='',lease_expires_at=''
    WHERE id=? AND state='claimed' AND claim_token=?
  )SQL"));
  query.addBindValue(id);
  query.addBindValue(claimToken);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("release reminder delivery"));
    }
    return false;
  }
  return query.numRowsAffected() == 0 || bumpChangeRevision(errorMessage);
}

bool Database::recoverExpiredReminderDeliveries(const QDateTime& recoveredAt,
                                                QString* errorMessage) {
  if (!recoveredAt.isValid()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("A valid reminder recovery time is required");
    }
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE reminder_jobs
    SET state=CASE WHEN snoozed_until<>'' THEN 'snoozed' ELSE 'pending' END,
        claimed_at='',claim_token='',lease_expires_at=''
    WHERE state='claimed'
      AND (lease_expires_at='' OR lease_expires_at<=?)
  )SQL"));
  query.addBindValue(isoUtc(recoveredAt));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("recover reminder claims"));
    }
    return false;
  }
  return query.numRowsAffected() == 0 || bumpChangeRevision(errorMessage);
}

bool Database::recoverClaimedNotificationDeliveries(const QDateTime& recoveredAt,
                                                    QString* errorMessage) {
  if (!recoveredAt.isValid()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("A valid notification recovery time is required");
    }
    return false;
  }
  QSqlQuery savepoint(m_database);
  if (!savepoint.exec(QStringLiteral("SAVEPOINT recover_notification_claims"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(savepoint, QStringLiteral("start notification recovery"));
    }
    return false;
  }
  const auto fail = [this]() {
    QSqlQuery rollback(m_database);
    rollback.exec(QStringLiteral("ROLLBACK TO recover_notification_claims"));
    rollback.exec(QStringLiteral("RELEASE recover_notification_claims"));
    return false;
  };
  QSqlQuery reminders(m_database);
  reminders.prepare(QStringLiteral(R"SQL(
    UPDATE reminder_jobs
    SET state=CASE WHEN snoozed_until<>'' THEN 'snoozed' ELSE 'pending' END,
        claimed_at='',claim_token='',lease_expires_at=''
    WHERE state='claimed'
      AND (lease_expires_at='' OR lease_expires_at<=?)
  )SQL"));
  reminders.addBindValue(isoUtc(recoveredAt));
  QSqlQuery invitations(m_database);
  invitations.prepare(QStringLiteral(R"SQL(
    UPDATE notification_deliveries
    SET state='retry_pending',delivered_at=''
    WHERE state='claimed'
  )SQL"));
  if (!reminders.exec() || !invitations.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          reminders.lastError().isValid()
              ? sqlError(reminders, QStringLiteral("recover reminder claims"))
              : sqlError(invitations,
                         QStringLiteral("release abandoned notification claims"));
    }
    return fail();
  }
  if ((reminders.numRowsAffected() > 0 || invitations.numRowsAffected() > 0) &&
      !bumpChangeRevision(errorMessage)) {
    return fail();
  }
  QSqlQuery release(m_database);
  if (!release.exec(QStringLiteral("RELEASE recover_notification_claims"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(release, QStringLiteral("commit notification recovery"));
    }
    return fail();
  }
  return true;
}

bool Database::snoozeReminder(const qint64 id, const int minutes,
                              QString* errorMessage) {
  return snoozeReminderAt(id, minutes, nowUtc(), errorMessage);
}

bool Database::snoozeReminderAt(const qint64 id, const int minutes,
                                const QDateTime& now, QString* errorMessage) {
  if (!QList<int>{5, 10, 30, 60}.contains(minutes)) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Snooze must be 5, 10, 30, or 60 minutes");
    }
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE reminder_jobs SET state='snoozed',snoozed_until=?,claimed_at='',
      claim_token='',lease_expires_at='',delivered_at=''
    WHERE id=? AND state<>'dismissed'
  )SQL"));
  query.addBindValue(isoUtc(now.addSecs(minutes * 60)));
  query.addBindValue(id);
  if (!query.exec() || query.numRowsAffected() == 0) {
    if (errorMessage != nullptr) {
      *errorMessage = query.lastError().isValid()
                          ? sqlError(query, QStringLiteral("snooze reminder"))
                          : QStringLiteral("Reminder not found");
    }
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

bool Database::snoozeReminderUntil(const qint64 id, const QDateTime& until,
                                   const QDateTime& now, QString* errorMessage) {
  if (!until.isValid() || until <= now || until > now.addDays(1)) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Snooze must end within the next day");
    }
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE reminder_jobs SET state='snoozed',snoozed_until=?,claimed_at='',
      claim_token='',lease_expires_at='',delivered_at=''
    WHERE id=? AND state<>'dismissed'
  )SQL"));
  query.addBindValue(isoUtc(until));
  query.addBindValue(id);
  if (!query.exec() || query.numRowsAffected() == 0) {
    if (errorMessage != nullptr) {
      *errorMessage = query.lastError().isValid()
                          ? sqlError(query, QStringLiteral("snooze reminder"))
                          : QStringLiteral("Reminder not found");
    }
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

bool Database::dismissReminder(const qint64 id, QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE reminder_jobs
    SET state='dismissed',claimed_at='',claim_token='',lease_expires_at=''
    WHERE id=?
  )SQL"));
  query.addBindValue(id);
  if (!query.exec() || query.numRowsAffected() == 0) {
    if (errorMessage != nullptr) {
      *errorMessage = query.lastError().isValid()
                          ? sqlError(query, QStringLiteral("dismiss reminder"))
                          : QStringLiteral("Reminder not found");
    }
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

qint64 Database::dismissStaleReminders(const QDateTime& now, const int graceSeconds,
                                       QString* errorMessage) {
  // A due pending job is consumed while its calendar is muted. Otherwise it is
  // stale only past the grace window and once its occurrence is no longer
  // upcoming. Snoozed jobs preserve the user's explicit delivery intent.
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE reminder_jobs
    SET state='dismissed',claimed_at='',claim_token='',lease_expires_at=''
    WHERE state='pending'
      AND (
        (fire_at<=? AND EXISTS (
          SELECT 1 FROM events e
          JOIN calendars c ON c.id=e.calendar_id
          WHERE e.id=reminder_jobs.event_id AND c.ignore_alerts=1
        )) OR (
          fire_at<? AND NOT EXISTS (
            SELECT 1 FROM event_instances ei
            WHERE ei.event_id=reminder_jobs.event_id
              AND ei.recurrence_id=reminder_jobs.occurrence_id
              AND (ei.start_utc>?
                   OR (ei.start_utc='' AND ei.start_date<>'' AND ei.start_date>?))
          )
        )
      )
  )SQL"));
  query.addBindValue(isoUtc(now));
  query.addBindValue(isoUtc(now.addSecs(-graceSeconds)));
  query.addBindValue(isoUtc(now));
  query.addBindValue(now.toLocalTime().date().toString(Qt::ISODate));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("dismiss stale reminders"));
    }
    return -1;
  }
  const qint64 dismissed = query.numRowsAffected();
  if (dismissed > 0 && !bumpChangeRevision(errorMessage)) {
    return -1;
  }
  return dismissed;
}

bool Database::markReminderDelivered(const qint64 id, QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE reminder_jobs
    SET state='delivered',delivered_at=?,snoozed_until='',claimed_at='',
        claim_token='',lease_expires_at=''
    WHERE id=? AND state IN ('pending','snoozed','claimed')
  )SQL"));
  query.addBindValue(isoUtc(nowUtc()));
  query.addBindValue(id);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("complete reminder delivery"));
    }
    return false;
  }
  return query.numRowsAffected() == 0 || bumpChangeRevision(errorMessage);
}

QList<Event> Database::notificationEventsForCalendars(const QStringList& calendarIds,
                                                      QString* errorMessage) const {
  QList<Event> result;
  if (calendarIds.isEmpty()) {
    return result;
  }
  QStringList placeholders;
  placeholders.fill(QStringLiteral("?"), calendarIds.size());
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT e.* FROM events e
    JOIN calendars c ON c.id=e.calendar_id
    WHERE e.calendar_id IN (%1)
      AND c.enabled=1 AND c.ignore_alerts=0
      AND e.organizer_json<>'{}' AND e.attendees_json<>'[]'
    ORDER BY e.updated_at,e.id
  )SQL")
                    .arg(placeholders.join(QLatin1Char(','))));
  for (const QString& calendarId : calendarIds) {
    query.addBindValue(calendarId);
  }
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list notification invitations"));
    }
    return result;
  }
  while (query.next()) {
    result.append(eventFromQuery(query));
  }
  return result;
}

bool Database::claimNotificationDelivery(const QString& fingerprint,
                                         const QString& kind, const QString& eventId,
                                         const qint64 eventRevision,
                                         const QDateTime& claimedAt, bool* claimed,
                                         QString* errorMessage) {
  if (claimed != nullptr) {
    *claimed = false;
  }
  if (fingerprint.isEmpty() || kind.isEmpty() || eventId.isEmpty() ||
      !claimedAt.isValid()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("A complete notification claim is required");
    }
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO notification_deliveries
      (fingerprint,kind,event_id,event_revision,state,claimed_at,delivered_at)
    VALUES (?,?,?,?,'claimed',?,'')
    ON CONFLICT(fingerprint) DO UPDATE SET
      kind=excluded.kind,
      event_id=excluded.event_id,
      event_revision=excluded.event_revision,
      state='claimed',
      claimed_at=excluded.claimed_at,
      delivered_at=''
    WHERE notification_deliveries.state='retry_pending'
  )SQL"));
  query.addBindValue(fingerprint);
  query.addBindValue(kind);
  query.addBindValue(eventId);
  query.addBindValue(eventRevision);
  query.addBindValue(isoUtc(claimedAt));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("claim notification delivery"));
    }
    return false;
  }
  const bool changed = query.numRowsAffected() > 0;
  if (claimed != nullptr) {
    *claimed = changed;
  }
  return !changed || bumpChangeRevision(errorMessage);
}

bool Database::finishNotificationDelivery(const QString& fingerprint,
                                          const QDateTime& deliveredAt,
                                          QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE notification_deliveries SET state='delivered',delivered_at=?
    WHERE fingerprint=? AND state='claimed'
  )SQL"));
  query.addBindValue(isoUtc(deliveredAt));
  query.addBindValue(fingerprint);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("finish notification delivery"));
    }
    return false;
  }
  return query.numRowsAffected() == 0 || bumpChangeRevision(errorMessage);
}

bool Database::releaseNotificationDelivery(const QString& fingerprint,
                                           QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE notification_deliveries
    SET state='retry_pending',delivered_at=''
    WHERE fingerprint=? AND state='claimed'
  )SQL"));
  query.addBindValue(fingerprint);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("release notification delivery"));
    }
    return false;
  }
  return query.numRowsAffected() == 0 || bumpChangeRevision(errorMessage);
}

bool Database::hasAnyNotificationDeliveryForEvent(const QString& eventId,
                                                  QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(
      QStringLiteral("SELECT 1 FROM notification_deliveries WHERE event_id=? LIMIT 1"));
  query.addBindValue(eventId);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("check notification history"));
    }
    return false;
  }
  return query.next();
}

bool Database::hasCompletedNotificationDeliveryForEvent(const QString& eventId,
                                                        QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT 1 FROM notification_deliveries
    WHERE event_id=? AND state='delivered' LIMIT 1
  )SQL"));
  query.addBindValue(eventId);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(query, QStringLiteral("check completed notification history"));
    }
    return false;
  }
  return query.next();
}

bool Database::isNotificationDeliveryCompleted(const QString& fingerprint,
                                               QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT 1 FROM notification_deliveries
    WHERE fingerprint=? AND state='delivered' LIMIT 1
  )SQL"));
  query.addBindValue(fingerprint);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("check notification delivery"));
    }
    return false;
  }
  return query.next();
}

bool Database::persistInvitationBaselines(const QList<Event>& invitations,
                                          QString* errorMessage) {
  if (invitations.isEmpty()) {
    return true;
  }
  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  const QDateTime now = nowUtc();
  bool changed = false;
  for (const Event& event : invitations) {
    QSqlQuery claim(m_database);
    claim.prepare(QStringLiteral(R"SQL(
      INSERT OR IGNORE INTO notification_deliveries
        (fingerprint,kind,event_id,event_revision,state,claimed_at)
      VALUES (?,'invitation_baseline',?,?,'delivered',?)
    )SQL"));
    claim.addBindValue(invitationFingerprint(event));
    claim.addBindValue(event.id);
    claim.addBindValue(event.localRevision);
    claim.addBindValue(isoUtc(now));
    if (!claim.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(claim, QStringLiteral("persist invitation baseline"));
      }
      m_database.rollback();
      return false;
    }
    changed = changed || claim.numRowsAffected() > 0;
  }
  if (!m_database.commit()) {
    // A failed commit can leave the transaction open on SQLite; rolling back
    // unconditionally keeps the connection usable for the next batch.
    const QString commitError = m_database.lastError().text();
    m_database.rollback();
    if (errorMessage != nullptr) {
      *errorMessage = commitError;
    }
    return false;
  }
  return !changed || bumpChangeRevision(errorMessage);
}

}  // namespace omacalendar
