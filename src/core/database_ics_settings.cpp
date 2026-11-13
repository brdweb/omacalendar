// Database: provider state, ICS subscriptions, sync coverage and settings.
// Split out of database.cpp; shared helpers live in database_p.h.

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "core/database.h"
#include "core/database_p.h"

namespace omacalendar {

using namespace detail;

QJsonValue Database::providerState(const QString& accountId, const QString& calendarId,
                                   const QString& key, const QJsonValue& fallback,
                                   QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT value_json FROM provider_state
    WHERE account_id=? AND calendar_id=? AND key=?
  )SQL"));
  query.addBindValue(accountId);
  query.addBindValue(nonNull(calendarId));
  query.addBindValue(key);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("get provider state"));
    }
    return fallback;
  }
  if (!query.next()) {
    return fallback;
  }
  bool ok = false;
  const QJsonValue result = parseValue(query.value(0).toString(), fallback, &ok);
  if (!ok && errorMessage != nullptr) {
    *errorMessage = QStringLiteral("Stored provider state is invalid JSON");
  }
  return result;
}

bool Database::setProviderState(const QString& accountId, const QString& calendarId,
                                const QString& key, const QJsonValue& value,
                                QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO provider_state
      (account_id, calendar_id, key, value_json, updated_at)
    VALUES (?, ?, ?, ?, ?)
    ON CONFLICT(account_id, calendar_id, key) DO UPDATE SET
      value_json=excluded.value_json, updated_at=excluded.updated_at
  )SQL"));
  query.addBindValue(accountId);
  query.addBindValue(nonNull(calendarId));
  query.addBindValue(key);
  query.addBindValue(serializeValue(value));
  query.addBindValue(isoUtc(nowUtc()));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("set provider state"));
    }
    return false;
  }
  return true;
}

bool Database::clearProviderState(const QString& accountId, const QString& calendarId,
                                  QString* errorMessage) {
  QSqlQuery query(m_database);
  if (calendarId.isEmpty()) {
    query.prepare(QStringLiteral("DELETE FROM provider_state WHERE account_id=?"));
    query.addBindValue(accountId);
  } else {
    query.prepare(QStringLiteral(
        "DELETE FROM provider_state WHERE account_id=? AND calendar_id=?"));
    query.addBindValue(accountId);
    query.addBindValue(calendarId);
  }
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("clear provider state"));
    }
    return false;
  }
  return true;
}

bool Database::upsertIcsSubscription(const IcsSubscription& subscription,
                                     QString* errorMessage) {
  if (subscription.accountId.isEmpty() || subscription.url.isEmpty() ||
      subscription.refreshSeconds < 60) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "A subscription account, URL, and refresh interval are required");
    }
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO ics_subscriptions
      (account_id,url,etag,last_modified,refresh_seconds,last_success_at,
       last_error_code,last_error_message)
    VALUES (?,?,?,?,?,?,?,?)
    ON CONFLICT(account_id) DO UPDATE SET
      url=excluded.url,
      etag=excluded.etag,
      last_modified=excluded.last_modified,
      refresh_seconds=excluded.refresh_seconds,
      last_success_at=excluded.last_success_at,
      last_error_code=excluded.last_error_code,
      last_error_message=excluded.last_error_message
  )SQL"));
  query.addBindValue(subscription.accountId);
  query.addBindValue(nonNull(subscription.url));
  query.addBindValue(nonNull(subscription.etag));
  query.addBindValue(nonNull(subscription.lastModified));
  query.addBindValue(subscription.refreshSeconds);
  query.addBindValue(isoUtc(subscription.lastSuccessAt));
  query.addBindValue(nonNull(subscription.lastErrorCode));
  query.addBindValue(nonNull(subscription.lastErrorMessage));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("save ICS subscription"));
    }
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

IcsSubscription Database::icsSubscription(const QString& accountId,
                                          QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT account_id,url,etag,last_modified,refresh_seconds,last_success_at,
           last_error_code,last_error_message
    FROM ics_subscriptions WHERE account_id=?
  )SQL"));
  query.addBindValue(accountId);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("get ICS subscription"));
    }
    return {};
  }
  if (!query.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("ICS subscription not found");
    }
    return {};
  }
  IcsSubscription result;
  result.accountId = query.value(0).toString();
  result.url = query.value(1).toString();
  result.etag = query.value(2).toString();
  result.lastModified = query.value(3).toString();
  result.refreshSeconds = query.value(4).toInt();
  result.lastSuccessAt = dateTimeFromIso(query.value(5).toString());
  result.lastErrorCode = query.value(6).toString();
  result.lastErrorMessage = query.value(7).toString();
  return result;
}

QList<IcsSubscription> Database::icsSubscriptions(QString* errorMessage) const {
  QList<IcsSubscription> result;
  QSqlQuery query(m_database);
  if (!query.exec(QStringLiteral(R"SQL(
    SELECT account_id,url,etag,last_modified,refresh_seconds,last_success_at,
           last_error_code,last_error_message
    FROM ics_subscriptions ORDER BY account_id
  )SQL"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list ICS subscriptions"));
    }
    return result;
  }
  while (query.next()) {
    IcsSubscription subscription;
    subscription.accountId = query.value(0).toString();
    subscription.url = query.value(1).toString();
    subscription.etag = query.value(2).toString();
    subscription.lastModified = query.value(3).toString();
    subscription.refreshSeconds = query.value(4).toInt();
    subscription.lastSuccessAt = dateTimeFromIso(query.value(5).toString());
    subscription.lastErrorCode = query.value(6).toString();
    subscription.lastErrorMessage = query.value(7).toString();
    result.append(subscription);
  }
  return result;
}

bool Database::updateIcsSubscriptionResult(
    const QString& accountId, const QString& etag, const QString& lastModified,
    const QDateTime& successAt, const QString& errorCode, const QString& errorText,
    QString* errorMessage) {
  QSqlQuery query(m_database);
  if (successAt.isValid()) {
    query.prepare(QStringLiteral(R"SQL(
      UPDATE ics_subscriptions SET
        etag=CASE WHEN ?='' THEN etag ELSE ? END,
        last_modified=CASE WHEN ?='' THEN last_modified ELSE ? END,
        last_success_at=?,last_error_code='',last_error_message=''
      WHERE account_id=?
    )SQL"));
    query.addBindValue(nonNull(etag));
    query.addBindValue(nonNull(etag));
    query.addBindValue(nonNull(lastModified));
    query.addBindValue(nonNull(lastModified));
    query.addBindValue(isoUtc(successAt));
    query.addBindValue(accountId);
  } else {
    query.prepare(QStringLiteral(R"SQL(
      UPDATE ics_subscriptions
      SET last_error_code=?,last_error_message=? WHERE account_id=?
    )SQL"));
    query.addBindValue(nonNull(errorCode));
    query.addBindValue(nonNull(errorText).left(300));
    query.addBindValue(accountId);
  }
  if (!query.exec() || query.numRowsAffected() == 0) {
    if (errorMessage != nullptr) {
      *errorMessage = query.lastError().isValid()
                          ? sqlError(query, QStringLiteral("update ICS status"))
                          : QStringLiteral("ICS subscription not found");
    }
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

QList<SyncCoverage> Database::syncCoverage(const QString& calendarId,
                                           QString* errorMessage) const {
  QList<SyncCoverage> result;
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT calendar_id,start_utc,end_utc,complete,updated_at
    FROM sync_coverage WHERE calendar_id=? AND complete=1
    ORDER BY start_utc,end_utc
  )SQL"));
  query.addBindValue(calendarId);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list sync coverage"));
    }
    return result;
  }
  while (query.next()) {
    SyncCoverage coverage;
    coverage.calendarId = query.value(0).toString();
    coverage.startUtc = dateTimeFromIso(query.value(1).toString());
    coverage.endUtc = dateTimeFromIso(query.value(2).toString());
    coverage.complete = query.value(3).toBool();
    coverage.updatedAt = dateTimeFromIso(query.value(4).toString());
    result.append(coverage);
  }
  return result;
}

bool Database::isSyncRangeCovered(const QString& calendarId, const QDateTime& startUtc,
                                  const QDateTime& endUtc,
                                  QString* errorMessage) const {
  if (calendarId.isEmpty() || !startUtc.isValid() || !endUtc.isValid() ||
      startUtc >= endUtc) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Sync coverage needs a calendar and valid bounds");
    }
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT 1 FROM sync_coverage
    WHERE calendar_id=? AND complete=1 AND start_utc<=? AND end_utc>=?
    LIMIT 1
  )SQL"));
  query.addBindValue(calendarId);
  query.addBindValue(isoUtc(startUtc.toUTC()));
  query.addBindValue(isoUtc(endUtc.toUTC()));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("check sync coverage"));
    }
    return false;
  }
  return query.next();
}

bool Database::mergeSyncCoverageInternal(const QString& calendarId,
                                         const QDateTime& startUtc,
                                         const QDateTime& endUtc,
                                         QString* errorMessage) {
  if (calendarId.isEmpty() || !startUtc.isValid() || !endUtc.isValid() ||
      startUtc >= endUtc) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Sync coverage needs a calendar and valid bounds");
    }
    return false;
  }
  QDateTime mergedStart = startUtc.toUTC();
  QDateTime mergedEnd = endUtc.toUTC();
  QSqlQuery overlaps(m_database);
  overlaps.prepare(QStringLiteral(R"SQL(
    SELECT start_utc,end_utc FROM sync_coverage
    WHERE calendar_id=? AND complete=1 AND end_utc>=? AND start_utc<=?
    ORDER BY start_utc,end_utc
  )SQL"));
  overlaps.addBindValue(calendarId);
  overlaps.addBindValue(isoUtc(mergedStart));
  overlaps.addBindValue(isoUtc(mergedEnd));
  if (!overlaps.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(overlaps, QStringLiteral("load overlapping coverage"));
    }
    return false;
  }
  while (overlaps.next()) {
    const QDateTime existingStart = dateTimeFromIso(overlaps.value(0).toString());
    const QDateTime existingEnd = dateTimeFromIso(overlaps.value(1).toString());
    if (existingStart.isValid() && existingStart < mergedStart) {
      mergedStart = existingStart;
    }
    if (existingEnd.isValid() && existingEnd > mergedEnd) {
      mergedEnd = existingEnd;
    }
  }

  QSqlQuery remove(m_database);
  remove.prepare(QStringLiteral(R"SQL(
    DELETE FROM sync_coverage
    WHERE calendar_id=? AND end_utc>=? AND start_utc<=?
  )SQL"));
  remove.addBindValue(calendarId);
  remove.addBindValue(isoUtc(mergedStart));
  remove.addBindValue(isoUtc(mergedEnd));
  if (!remove.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(remove, QStringLiteral("merge overlapping coverage"));
    }
    return false;
  }

  QSqlQuery insert(m_database);
  insert.prepare(QStringLiteral(R"SQL(
    INSERT INTO sync_coverage(calendar_id,start_utc,end_utc,complete,updated_at)
    VALUES(?,?,?,?,?)
  )SQL"));
  insert.addBindValue(calendarId);
  insert.addBindValue(isoUtc(mergedStart));
  insert.addBindValue(isoUtc(mergedEnd));
  insert.addBindValue(true);
  insert.addBindValue(isoUtc(nowUtc()));
  if (!insert.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(insert, QStringLiteral("record sync coverage"));
    }
    return false;
  }
  return true;
}

bool Database::recordSyncCoverage(const QString& calendarId, const QDateTime& startUtc,
                                  const QDateTime& endUtc, QString* errorMessage) {
  QSqlQuery savepoint(m_database);
  if (!savepoint.exec(QStringLiteral("SAVEPOINT record_sync_coverage"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(savepoint, QStringLiteral("start coverage update"));
    }
    return false;
  }
  const auto fail = [this]() {
    QSqlQuery rollback(m_database);
    rollback.exec(QStringLiteral("ROLLBACK TO record_sync_coverage"));
    rollback.exec(QStringLiteral("RELEASE record_sync_coverage"));
    return false;
  };
  if (!mergeSyncCoverageInternal(calendarId, startUtc, endUtc, errorMessage) ||
      !bumpChangeRevision(errorMessage)) {
    return fail();
  }
  QSqlQuery release(m_database);
  if (!release.exec(QStringLiteral("RELEASE record_sync_coverage"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(release, QStringLiteral("commit coverage update"));
    }
    return fail();
  }
  return true;
}

QJsonValue Database::setting(const QString& key, const QJsonValue& fallback,
                             QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("SELECT value_json FROM settings WHERE key=?"));
  query.addBindValue(key);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("get setting"));
    }
    return fallback;
  }
  if (!query.next()) {
    return fallback;
  }
  bool ok = false;
  const QJsonValue result = parseValue(query.value(0).toString(), fallback, &ok);
  if (!ok) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Stored setting is invalid JSON");
    }
    return fallback;
  }
  return result;
}

bool Database::setSetting(const QString& key, const QJsonValue& value,
                          QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO settings(key, value_json, updated_at) VALUES (?, ?, ?)
    ON CONFLICT(key) DO UPDATE SET
      value_json=excluded.value_json, updated_at=excluded.updated_at
  )SQL"));
  query.addBindValue(key);
  query.addBindValue(serializeValue(value));
  query.addBindValue(isoUtc(nowUtc()));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("set setting"));
    }
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

}  // namespace omacalendar
