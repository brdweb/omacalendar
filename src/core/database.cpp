#include "core/database.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

#include "core/recurrenceexpander.h"

namespace omacalendar {
namespace {

constexpr int kCurrentSchemaVersion = 1;

QString compactJson(const QJsonValue& value) {
  if (value.isArray()) {
    return QString::fromUtf8(
        QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
  }
  return QString::fromUtf8(
      QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
}

QJsonObject parseObject(const QString& value) {
  if (value.isEmpty()) {
    return {};
  }
  return QJsonDocument::fromJson(value.toUtf8()).object();
}

QJsonArray parseArray(const QString& value) {
  if (value.isEmpty()) {
    return {};
  }
  return QJsonDocument::fromJson(value.toUtf8()).array();
}

QString serializeValue(const QJsonValue& value) {
  QJsonArray wrapper;
  wrapper.append(value);
  QByteArray serialized = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
  return QString::fromUtf8(serialized.mid(1, serialized.size() - 2));
}

QJsonValue parseValue(const QString& value, const QJsonValue& fallback,
                      bool* ok = nullptr) {
  QJsonParseError parseError;
  const QJsonDocument wrapper = QJsonDocument::fromJson(
      QByteArrayLiteral("[") + value.toUtf8() + ']', &parseError);
  const bool valid =
      parseError.error == QJsonParseError::NoError && !wrapper.array().isEmpty();
  if (ok != nullptr) {
    *ok = valid;
  }
  return valid ? wrapper.array().first() : fallback;
}

QString sqlError(const QSqlQuery& query, const QString& context) {
  return QStringLiteral("%1: %2").arg(context, query.lastError().text());
}

QDateTime nowUtc() { return QDateTime::currentDateTimeUtc(); }

QString nonNull(const QString& value) {
  return value.isNull() ? QStringLiteral("") : value;
}

}  // namespace

Database::Database()
    : m_connectionName(
          QStringLiteral("omacalendar-%1").arg(QUuid::createUuid().toString())) {}

Database::~Database() { close(); }

bool Database::open(const QString& path, QString* errorMessage) {
  close();
  m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
  m_database.setDatabaseName(path);
  if (!m_database.open()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }

  if (!execute(QStringLiteral("PRAGMA foreign_keys = ON"), errorMessage) ||
      !execute(QStringLiteral("PRAGMA busy_timeout = 5000"), errorMessage)) {
    return false;
  }
  if (path != QStringLiteral(":memory:") &&
      !execute(QStringLiteral("PRAGMA journal_mode = WAL"), errorMessage)) {
    return false;
  }
  return migrate(errorMessage);
}

void Database::close() {
  if (!m_database.isValid()) {
    return;
  }
  m_database.close();
  m_database = QSqlDatabase();
  QSqlDatabase::removeDatabase(m_connectionName);
}

bool Database::isOpen() const { return m_database.isOpen(); }

int Database::schemaVersion() const {
  if (!isOpen()) {
    return 0;
  }
  QSqlQuery query(m_database);
  if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
    return 0;
  }
  return query.value(0).toInt();
}

bool Database::execute(const QString& sql, QString* errorMessage) const {
  QSqlQuery query(m_database);
  if (query.exec(sql)) {
    return true;
  }
  if (errorMessage != nullptr) {
    *errorMessage = sqlError(query, sql.left(80));
  }
  return false;
}

bool Database::migrate(QString* errorMessage) {
  if (schemaVersion() > kCurrentSchemaVersion) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Database schema is newer than this build");
    }
    return false;
  }
  if (schemaVersion() == kCurrentSchemaVersion) {
    return true;
  }
  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }

  const QStringList statements = {
      QStringLiteral(R"SQL(
        CREATE TABLE accounts (
          id TEXT PRIMARY KEY,
          provider TEXT NOT NULL CHECK(provider IN ('caldav', 'google')),
          display_name TEXT NOT NULL,
          principal TEXT NOT NULL DEFAULT '',
          endpoint TEXT NOT NULL DEFAULT '',
          enabled INTEGER NOT NULL DEFAULT 1,
          auth_status TEXT NOT NULL DEFAULT 'not_configured',
          created_at TEXT NOT NULL,
          updated_at TEXT NOT NULL
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE calendars (
          id TEXT PRIMARY KEY,
          account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
          remote_id TEXT NOT NULL DEFAULT '',
          href TEXT NOT NULL DEFAULT '',
          name TEXT NOT NULL,
          description TEXT NOT NULL DEFAULT '',
          color TEXT NOT NULL DEFAULT '#7aa2f7',
          timezone TEXT NOT NULL DEFAULT '',
          read_only INTEGER NOT NULL DEFAULT 0,
          enabled INTEGER NOT NULL DEFAULT 1,
          etag TEXT NOT NULL DEFAULT '',
          sync_token TEXT NOT NULL DEFAULT '',
          capabilities_json TEXT NOT NULL DEFAULT '{}',
          last_sync_at TEXT NOT NULL DEFAULT '',
          CHECK(remote_id <> '' OR href <> '' OR name <> '')
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE UNIQUE INDEX calendars_remote_id_unique
        ON calendars(account_id, remote_id) WHERE remote_id <> ''
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE UNIQUE INDEX calendars_href_unique
        ON calendars(account_id, href) WHERE href <> ''
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE events (
          id TEXT PRIMARY KEY,
          calendar_id TEXT NOT NULL REFERENCES calendars(id) ON DELETE CASCADE,
          remote_id TEXT NOT NULL DEFAULT '',
          uid TEXT NOT NULL,
          etag TEXT NOT NULL DEFAULT '',
          summary TEXT NOT NULL DEFAULT '',
          description TEXT NOT NULL DEFAULT '',
          location TEXT NOT NULL DEFAULT '',
          start_utc TEXT NOT NULL DEFAULT '',
          end_utc TEXT NOT NULL DEFAULT '',
          start_date TEXT NOT NULL DEFAULT '',
          end_date TEXT NOT NULL DEFAULT '',
          start_timezone TEXT NOT NULL DEFAULT '',
          end_timezone TEXT NOT NULL DEFAULT '',
          all_day INTEGER NOT NULL DEFAULT 0,
          status TEXT NOT NULL DEFAULT 'confirmed',
          transparency TEXT NOT NULL DEFAULT 'opaque',
          recurrence_rule TEXT NOT NULL DEFAULT '',
          recurrence_id TEXT NOT NULL DEFAULT '',
          sequence INTEGER NOT NULL DEFAULT 0,
          organizer_json TEXT NOT NULL DEFAULT '{}',
          attendees_json TEXT NOT NULL DEFAULT '[]',
          reminders_json TEXT NOT NULL DEFAULT '[]',
          raw_payload TEXT NOT NULL DEFAULT '',
          raw_format TEXT NOT NULL DEFAULT '',
          dirty INTEGER NOT NULL DEFAULT 0,
          deleted INTEGER NOT NULL DEFAULT 0,
          created_at TEXT NOT NULL,
          updated_at TEXT NOT NULL,
          UNIQUE(calendar_id, uid, recurrence_id)
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE UNIQUE INDEX events_remote_id_unique
        ON events(calendar_id, remote_id) WHERE remote_id <> ''
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX events_time_index
        ON events(calendar_id, start_utc, end_utc)
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX events_date_index
        ON events(calendar_id, start_date, end_date)
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX events_uid_index ON events(uid)
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE event_instances (
          event_id TEXT NOT NULL REFERENCES events(id) ON DELETE CASCADE,
          recurrence_id TEXT NOT NULL DEFAULT '',
          start_utc TEXT NOT NULL DEFAULT '',
          end_utc TEXT NOT NULL DEFAULT '',
          start_date TEXT NOT NULL DEFAULT '',
          end_date TEXT NOT NULL DEFAULT '',
          all_day INTEGER NOT NULL DEFAULT 0,
          PRIMARY KEY(event_id, recurrence_id)
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX event_instances_time_index
        ON event_instances(start_utc, end_utc)
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE outbox (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
          calendar_id TEXT NOT NULL REFERENCES calendars(id) ON DELETE CASCADE,
          event_id TEXT NOT NULL REFERENCES events(id) ON DELETE CASCADE,
          operation TEXT NOT NULL CHECK(operation IN ('create','update','remove')),
          state TEXT NOT NULL DEFAULT 'pending'
            CHECK(state IN ('pending','sending','retry_wait','blocked','done')),
          idempotency_key TEXT NOT NULL UNIQUE,
          expected_revision TEXT NOT NULL DEFAULT '',
          payload_json TEXT NOT NULL DEFAULT '{}',
          attempts INTEGER NOT NULL DEFAULT 0,
          next_attempt_at TEXT NOT NULL DEFAULT '',
          error_code TEXT NOT NULL DEFAULT '',
          error_message TEXT NOT NULL DEFAULT '',
          created_at TEXT NOT NULL,
          updated_at TEXT NOT NULL
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX outbox_ready_index
        ON outbox(state, next_attempt_at, id)
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE conflicts (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          event_id TEXT NOT NULL REFERENCES events(id) ON DELETE CASCADE,
          local_revision TEXT NOT NULL DEFAULT '',
          remote_revision TEXT NOT NULL DEFAULT '',
          remote_payload TEXT NOT NULL DEFAULT '',
          state TEXT NOT NULL DEFAULT 'unresolved',
          created_at TEXT NOT NULL,
          resolved_at TEXT NOT NULL DEFAULT ''
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE calendar_sets (
          id TEXT PRIMARY KEY,
          name TEXT NOT NULL,
          is_default INTEGER NOT NULL DEFAULT 0,
          default_calendar_id TEXT REFERENCES calendars(id) ON DELETE SET NULL,
          created_at TEXT NOT NULL,
          updated_at TEXT NOT NULL
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE calendar_set_members (
          set_id TEXT NOT NULL REFERENCES calendar_sets(id) ON DELETE CASCADE,
          calendar_id TEXT NOT NULL REFERENCES calendars(id) ON DELETE CASCADE,
          position INTEGER NOT NULL DEFAULT 0,
          PRIMARY KEY(set_id, calendar_id)
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE settings (
          key TEXT PRIMARY KEY,
          value_json TEXT NOT NULL,
          updated_at TEXT NOT NULL
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE provider_state (
          account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
          calendar_id TEXT NOT NULL DEFAULT '',
          key TEXT NOT NULL,
          value_json TEXT NOT NULL,
          updated_at TEXT NOT NULL,
          PRIMARY KEY(account_id, calendar_id, key)
        )
      )SQL"),
      QStringLiteral("PRAGMA user_version = 1"),
  };

  for (const QString& statement : statements) {
    if (!execute(statement, errorMessage)) {
      m_database.rollback();
      return false;
    }
  }
  if (!m_database.commit()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  return true;
}

bool Database::upsertAccount(Account account, QString* errorMessage) {
  const QDateTime now = nowUtc();
  if (account.id.isEmpty()) {
    account.id = newUuid();
  }
  if (!account.createdAt.isValid()) {
    account.createdAt = now;
  }
  account.updatedAt = now;

  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO accounts
      (id, provider, display_name, principal, endpoint, enabled, auth_status,
       created_at, updated_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(id) DO UPDATE SET
      provider=excluded.provider,
      display_name=excluded.display_name,
      principal=excluded.principal,
      endpoint=excluded.endpoint,
      enabled=excluded.enabled,
      auth_status=excluded.auth_status,
      updated_at=excluded.updated_at
  )SQL"));
  query.addBindValue(account.id);
  query.addBindValue(providerKindToString(account.provider));
  query.addBindValue(nonNull(account.displayName));
  query.addBindValue(nonNull(account.principal));
  query.addBindValue(nonNull(account.endpoint));
  query.addBindValue(account.enabled);
  query.addBindValue(nonNull(account.authStatus));
  query.addBindValue(isoUtc(account.createdAt));
  query.addBindValue(isoUtc(account.updatedAt));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("upsert account"));
    }
    return false;
  }
  return true;
}

QList<Account> Database::accounts(QString* errorMessage) const {
  QList<Account> result;
  QSqlQuery query(m_database);
  if (!query.exec(QStringLiteral(R"SQL(
    SELECT id, provider, display_name, principal, endpoint, enabled,
           auth_status, created_at, updated_at
    FROM accounts ORDER BY display_name COLLATE NOCASE, id
  )SQL"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list accounts"));
    }
    return result;
  }
  while (query.next()) {
    Account account;
    account.id = query.value(0).toString();
    account.provider = providerKindFromString(query.value(1).toString());
    account.displayName = query.value(2).toString();
    account.principal = query.value(3).toString();
    account.endpoint = query.value(4).toString();
    account.enabled = query.value(5).toBool();
    account.authStatus = query.value(6).toString();
    account.createdAt = dateTimeFromIso(query.value(7).toString());
    account.updatedAt = dateTimeFromIso(query.value(8).toString());
    result.append(account);
  }
  return result;
}

Account Database::account(const QString& accountId, QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT id, provider, display_name, principal, endpoint, enabled,
           auth_status, created_at, updated_at
    FROM accounts WHERE id = ?
  )SQL"));
  query.addBindValue(accountId);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("get account"));
    }
    return {};
  }
  if (!query.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Account not found");
    }
    return {};
  }
  Account result;
  result.id = query.value(0).toString();
  result.provider = providerKindFromString(query.value(1).toString());
  result.displayName = query.value(2).toString();
  result.principal = query.value(3).toString();
  result.endpoint = query.value(4).toString();
  result.enabled = query.value(5).toBool();
  result.authStatus = query.value(6).toString();
  result.createdAt = dateTimeFromIso(query.value(7).toString());
  result.updatedAt = dateTimeFromIso(query.value(8).toString());
  return result;
}

bool Database::removeAccount(const QString& accountId, QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("DELETE FROM accounts WHERE id = ?"));
  query.addBindValue(accountId);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("remove account"));
    }
    return false;
  }
  return query.numRowsAffected() > 0;
}

bool Database::upsertCalendar(Calendar calendar, QString* errorMessage) {
  if (calendar.id.isEmpty()) {
    calendar.id = newUuid();
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO calendars
      (id, account_id, remote_id, href, name, description, color, timezone,
       read_only, enabled, etag, sync_token, capabilities_json, last_sync_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(id) DO UPDATE SET
      account_id=excluded.account_id,
      remote_id=excluded.remote_id,
      href=excluded.href,
      name=excluded.name,
      description=excluded.description,
      color=excluded.color,
      timezone=excluded.timezone,
      read_only=excluded.read_only,
      enabled=excluded.enabled,
      etag=excluded.etag,
      sync_token=excluded.sync_token,
      capabilities_json=excluded.capabilities_json,
      last_sync_at=excluded.last_sync_at
  )SQL"));
  query.addBindValue(calendar.id);
  query.addBindValue(calendar.accountId);
  query.addBindValue(nonNull(calendar.remoteId));
  query.addBindValue(nonNull(calendar.href));
  query.addBindValue(nonNull(calendar.name));
  query.addBindValue(nonNull(calendar.description));
  query.addBindValue(nonNull(calendar.color));
  query.addBindValue(nonNull(calendar.timeZone));
  query.addBindValue(calendar.readOnly);
  query.addBindValue(calendar.enabled);
  query.addBindValue(nonNull(calendar.etag));
  query.addBindValue(nonNull(calendar.syncToken));
  query.addBindValue(compactJson(calendar.capabilities));
  query.addBindValue(isoUtc(calendar.lastSyncAt));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("upsert calendar"));
    }
    return false;
  }
  return true;
}

QList<Calendar> Database::calendars(const QString& accountId,
                                    QString* errorMessage) const {
  QList<Calendar> result;
  QSqlQuery query(m_database);
  QString sql = QStringLiteral(R"SQL(
    SELECT id, account_id, remote_id, href, name, description, color, timezone,
           read_only, enabled, etag, sync_token, capabilities_json, last_sync_at
    FROM calendars
  )SQL");
  if (!accountId.isEmpty()) {
    sql += QStringLiteral(" WHERE account_id = ?");
  }
  sql += QStringLiteral(" ORDER BY name COLLATE NOCASE, id");
  query.prepare(sql);
  if (!accountId.isEmpty()) {
    query.addBindValue(accountId);
  }
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list calendars"));
    }
    return result;
  }
  while (query.next()) {
    Calendar calendar;
    calendar.id = query.value(0).toString();
    calendar.accountId = query.value(1).toString();
    calendar.remoteId = query.value(2).toString();
    calendar.href = query.value(3).toString();
    calendar.name = query.value(4).toString();
    calendar.description = query.value(5).toString();
    calendar.color = query.value(6).toString();
    calendar.timeZone = query.value(7).toString();
    calendar.readOnly = query.value(8).toBool();
    calendar.enabled = query.value(9).toBool();
    calendar.etag = query.value(10).toString();
    calendar.syncToken = query.value(11).toString();
    calendar.capabilities = parseObject(query.value(12).toString());
    calendar.lastSyncAt = dateTimeFromIso(query.value(13).toString());
    result.append(calendar);
  }
  return result;
}

Calendar Database::calendar(const QString& calendarId, QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT id, account_id, remote_id, href, name, description, color, timezone,
           read_only, enabled, etag, sync_token, capabilities_json, last_sync_at
    FROM calendars WHERE id = ?
  )SQL"));
  query.addBindValue(calendarId);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("get calendar"));
    }
    return {};
  }
  if (!query.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Calendar not found");
    }
    return {};
  }
  Calendar result;
  result.id = query.value(0).toString();
  result.accountId = query.value(1).toString();
  result.remoteId = query.value(2).toString();
  result.href = query.value(3).toString();
  result.name = query.value(4).toString();
  result.description = query.value(5).toString();
  result.color = query.value(6).toString();
  result.timeZone = query.value(7).toString();
  result.readOnly = query.value(8).toBool();
  result.enabled = query.value(9).toBool();
  result.etag = query.value(10).toString();
  result.syncToken = query.value(11).toString();
  result.capabilities = parseObject(query.value(12).toString());
  result.lastSyncAt = dateTimeFromIso(query.value(13).toString());
  return result;
}

Calendar Database::calendarByRemoteId(const QString& accountId, const QString& remoteId,
                                      QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT id FROM calendars
    WHERE account_id = ? AND (remote_id = ? OR href = ?)
    LIMIT 1
  )SQL"));
  query.addBindValue(accountId);
  query.addBindValue(remoteId);
  query.addBindValue(remoteId);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("find remote calendar"));
    }
    return {};
  }
  if (!query.next()) {
    return {};
  }
  return calendar(query.value(0).toString(), errorMessage);
}

bool Database::upsertEventRecord(const Event& event, QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO events
      (id, calendar_id, remote_id, uid, etag, summary, description, location,
       start_utc, end_utc, start_date, end_date, start_timezone, end_timezone,
       all_day, status, transparency, recurrence_rule, recurrence_id, sequence,
       organizer_json, attendees_json, reminders_json, raw_payload, raw_format,
       dirty, deleted, created_at, updated_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(id) DO UPDATE SET
      calendar_id=excluded.calendar_id,
      remote_id=excluded.remote_id,
      uid=excluded.uid,
      etag=excluded.etag,
      summary=excluded.summary,
      description=excluded.description,
      location=excluded.location,
      start_utc=excluded.start_utc,
      end_utc=excluded.end_utc,
      start_date=excluded.start_date,
      end_date=excluded.end_date,
      start_timezone=excluded.start_timezone,
      end_timezone=excluded.end_timezone,
      all_day=excluded.all_day,
      status=excluded.status,
      transparency=excluded.transparency,
      recurrence_rule=excluded.recurrence_rule,
      recurrence_id=excluded.recurrence_id,
      sequence=excluded.sequence,
      organizer_json=excluded.organizer_json,
      attendees_json=excluded.attendees_json,
      reminders_json=excluded.reminders_json,
      raw_payload=excluded.raw_payload,
      raw_format=excluded.raw_format,
      dirty=excluded.dirty,
      deleted=excluded.deleted,
      updated_at=excluded.updated_at
  )SQL"));
  query.addBindValue(event.id);
  query.addBindValue(event.calendarId);
  query.addBindValue(nonNull(event.remoteId));
  query.addBindValue(nonNull(event.uid));
  query.addBindValue(nonNull(event.etag));
  query.addBindValue(nonNull(event.summary));
  query.addBindValue(nonNull(event.description));
  query.addBindValue(nonNull(event.location));
  query.addBindValue(isoUtc(event.startUtc));
  query.addBindValue(isoUtc(event.endUtc));
  query.addBindValue(nonNull(event.startDate.toString(Qt::ISODate)));
  query.addBindValue(nonNull(event.endDate.toString(Qt::ISODate)));
  query.addBindValue(nonNull(event.startTimeZone));
  query.addBindValue(nonNull(event.endTimeZone));
  query.addBindValue(event.allDay);
  query.addBindValue(nonNull(event.status));
  query.addBindValue(nonNull(event.transparency));
  query.addBindValue(nonNull(event.recurrenceRule));
  query.addBindValue(nonNull(event.recurrenceId));
  query.addBindValue(event.sequence);
  query.addBindValue(compactJson(event.organizer));
  query.addBindValue(compactJson(event.attendees));
  query.addBindValue(compactJson(event.reminders));
  query.addBindValue(nonNull(event.rawPayload));
  query.addBindValue(nonNull(event.rawFormat));
  query.addBindValue(event.dirty);
  query.addBindValue(event.deleted);
  query.addBindValue(isoUtc(event.createdAt));
  query.addBindValue(isoUtc(event.updatedAt));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("upsert event"));
    }
    return false;
  }
  return true;
}

bool Database::applyRemoteEvent(Event event, QString* errorMessage, bool* conflicted) {
  if (conflicted != nullptr) {
    *conflicted = false;
  }
  const QDateTime now = nowUtc();
  if (event.id.isEmpty()) {
    QSqlQuery existingQuery(m_database);
    if (!event.remoteId.isEmpty()) {
      existingQuery.prepare(QStringLiteral(
          "SELECT * FROM events WHERE calendar_id=? AND remote_id=? LIMIT 1"));
      existingQuery.addBindValue(event.calendarId);
      existingQuery.addBindValue(event.remoteId);
    } else {
      existingQuery.prepare(
          QStringLiteral("SELECT * FROM events WHERE calendar_id=? AND uid=? AND "
                         "recurrence_id=? LIMIT 1"));
      existingQuery.addBindValue(event.calendarId);
      existingQuery.addBindValue(event.uid);
      existingQuery.addBindValue(nonNull(event.recurrenceId));
    }
    if (!existingQuery.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(existingQuery, QStringLiteral("match remote event"));
      }
      return false;
    }
    if (existingQuery.next()) {
      const Event existing = eventFromQuery(existingQuery);
      event.id = existing.id;
      event.createdAt = existing.createdAt;
      if (existing.dirty) {
        if (!event.etag.isEmpty() && event.etag != existing.etag) {
          QSqlQuery conflictQuery(m_database);
          conflictQuery.prepare(QStringLiteral(R"SQL(
            INSERT INTO conflicts
              (event_id, local_revision, remote_revision, remote_payload,
               state, created_at)
            VALUES (?, ?, ?, ?, 'unresolved', ?)
          )SQL"));
          conflictQuery.addBindValue(existing.id);
          conflictQuery.addBindValue(nonNull(existing.etag));
          conflictQuery.addBindValue(nonNull(event.etag));
          conflictQuery.addBindValue(nonNull(event.rawPayload));
          conflictQuery.addBindValue(isoUtc(now));
          if (!conflictQuery.exec()) {
            if (errorMessage != nullptr) {
              *errorMessage =
                  sqlError(conflictQuery, QStringLiteral("record conflict"));
            }
            return false;
          }
          if (conflicted != nullptr) {
            *conflicted = true;
          }
        }
        return true;
      }
    } else {
      event.id = newUuid();
    }
  }
  if (event.uid.isEmpty()) {
    event.uid = newUuid();
  }
  if (!event.createdAt.isValid()) {
    event.createdAt = now;
  }
  event.updatedAt = now;
  event.dirty = false;
  return upsertEventRecord(event, errorMessage);
}

bool Database::removeRemoteEvent(const QString& calendarId, const QString& remoteId,
                                 const QString& remotePayload, QString* errorMessage,
                                 bool* conflicted) {
  if (conflicted != nullptr) {
    *conflicted = false;
  }
  QSqlQuery exactQuery(m_database);
  exactQuery.prepare(QStringLiteral(
      "SELECT * FROM events WHERE calendar_id=? AND remote_id=? LIMIT 1"));
  exactQuery.addBindValue(calendarId);
  exactQuery.addBindValue(remoteId);
  if (!exactQuery.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(exactQuery, QStringLiteral("find remote event"));
    }
    return false;
  }

  QList<Event> existingEvents;
  Event exactEvent;
  if (exactQuery.next()) {
    exactEvent = eventFromQuery(exactQuery);
    existingEvents.append(exactEvent);
  }

  const bool exactIsMaster = !exactEvent.id.isEmpty() &&
                             !exactEvent.recurrenceRule.isEmpty() &&
                             exactEvent.recurrenceId.isEmpty();
  if (exactIsMaster) {
    QSqlQuery seriesQuery(m_database);
    seriesQuery.prepare(QStringLiteral(R"SQL(
      SELECT * FROM events
      WHERE calendar_id=? AND id<>? AND (
        (uid=? AND recurrence_id<>'') OR
        substr(remote_id, 1, length(?) + 1) = ? || '#'
      )
    )SQL"));
    seriesQuery.addBindValue(calendarId);
    seriesQuery.addBindValue(exactEvent.id);
    seriesQuery.addBindValue(exactEvent.uid);
    seriesQuery.addBindValue(remoteId);
    seriesQuery.addBindValue(remoteId);
    if (!seriesQuery.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage =
            sqlError(seriesQuery, QStringLiteral("find remote event series"));
      }
      return false;
    }
    while (seriesQuery.next()) {
      existingEvents.append(eventFromQuery(seriesQuery));
    }
  } else if (exactEvent.id.isEmpty() && !remoteId.contains(QLatin1Char('#'))) {
    // A CalDAV sync-token deletion can arrive after the clean master row was
    // already removed while its dirty detached instance remains.
    QSqlQuery detachedQuery(m_database);
    detachedQuery.prepare(QStringLiteral(R"SQL(
      SELECT * FROM events
      WHERE calendar_id=? AND
        substr(remote_id, 1, length(?) + 1) = ? || '#'
    )SQL"));
    detachedQuery.addBindValue(calendarId);
    detachedQuery.addBindValue(remoteId);
    detachedQuery.addBindValue(remoteId);
    if (!detachedQuery.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage =
            sqlError(detachedQuery, QStringLiteral("find detached remote events"));
      }
      return false;
    }
    while (detachedQuery.next()) {
      existingEvents.append(eventFromQuery(detachedQuery));
    }
  }
  if (existingEvents.isEmpty()) {
    return true;
  }
  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }

  bool foundConflict = false;
  for (const Event& existing : existingEvents) {
    QSqlQuery query(m_database);
    if (existing.dirty) {
      query.prepare(QStringLiteral(R"SQL(
        INSERT INTO conflicts
          (event_id, local_revision, remote_revision, remote_payload, state,
           created_at)
        VALUES (?, ?, 'deleted', ?, 'unresolved', ?)
      )SQL"));
      query.addBindValue(existing.id);
      query.addBindValue(nonNull(existing.etag));
      query.addBindValue(nonNull(remotePayload));
      query.addBindValue(isoUtc(nowUtc()));
      foundConflict = true;
    } else {
      query.prepare(QStringLiteral("DELETE FROM events WHERE id=?"));
      query.addBindValue(existing.id);
    }
    if (!query.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage =
            sqlError(query, existing.dirty ? QStringLiteral("record delete conflict")
                                           : QStringLiteral("remove remote event"));
      }
      m_database.rollback();
      return false;
    }
  }
  if (!m_database.commit()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  if (conflicted != nullptr) {
    *conflicted = foundConflict;
  }
  return true;
}

bool Database::clearCleanRemoteEvents(const QString& calendarId,
                                      QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("DELETE FROM events WHERE calendar_id=? AND dirty=0"));
  query.addBindValue(calendarId);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("clear remote events"));
    }
    return false;
  }
  return true;
}

bool Database::saveLocalEvent(Event* event, const OutboxOperation operation,
                              QString* errorMessage) {
  if (event == nullptr || event->calendarId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Event and calendarId are required");
    }
    return false;
  }
  const QDateTime now = nowUtc();
  if (event->id.isEmpty()) {
    event->id = newUuid();
  }
  if (event->uid.isEmpty()) {
    event->uid = newUuid();
  }
  if (!event->createdAt.isValid()) {
    event->createdAt = now;
  }
  event->updatedAt = now;
  event->dirty = true;

  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  if (!upsertEventRecord(*event, errorMessage)) {
    m_database.rollback();
    return false;
  }

  QSqlQuery accountQuery(m_database);
  accountQuery.prepare(QStringLiteral("SELECT account_id FROM calendars WHERE id = ?"));
  accountQuery.addBindValue(event->calendarId);
  if (!accountQuery.exec() || !accountQuery.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = accountQuery.lastError().isValid()
                          ? accountQuery.lastError().text()
                          : QStringLiteral("Calendar does not exist");
    }
    m_database.rollback();
    return false;
  }
  const QString accountId = accountQuery.value(0).toString();

  QSqlQuery pendingQuery(m_database);
  pendingQuery.prepare(QStringLiteral(R"SQL(
    SELECT id, operation FROM outbox
    WHERE event_id=? AND state IN ('pending','retry_wait')
    ORDER BY id DESC LIMIT 1
  )SQL"));
  pendingQuery.addBindValue(event->id);
  if (!pendingQuery.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(pendingQuery, QStringLiteral("find queued event"));
    }
    m_database.rollback();
    return false;
  }
  if (pendingQuery.next()) {
    const qint64 pendingId = pendingQuery.value(0).toLongLong();
    const OutboxOperation previous =
        outboxOperationFromString(pendingQuery.value(1).toString());
    if (previous == OutboxOperation::Create && operation == OutboxOperation::Remove) {
      QSqlQuery deleteOutbox(m_database);
      deleteOutbox.prepare(QStringLiteral("DELETE FROM outbox WHERE id=?"));
      deleteOutbox.addBindValue(pendingId);
      QSqlQuery deleteEvent(m_database);
      deleteEvent.prepare(QStringLiteral("DELETE FROM events WHERE id=?"));
      deleteEvent.addBindValue(event->id);
      if (!deleteOutbox.exec() || !deleteEvent.exec() || !m_database.commit()) {
        if (errorMessage != nullptr) {
          *errorMessage = QStringLiteral("Could not cancel unsynced event");
        }
        m_database.rollback();
        return false;
      }
      return true;
    }
    const OutboxOperation effectiveOperation =
        previous == OutboxOperation::Create ? OutboxOperation::Create : operation;
    QSqlQuery updateQuery(m_database);
    updateQuery.prepare(QStringLiteral(R"SQL(
      UPDATE outbox SET operation=?, state='pending', expected_revision=?,
        payload_json=?, attempts=0, next_attempt_at=?, error_code='',
        error_message='', updated_at=? WHERE id=?
    )SQL"));
    updateQuery.addBindValue(outboxOperationToString(effectiveOperation));
    updateQuery.addBindValue(nonNull(event->etag));
    updateQuery.addBindValue(compactJson(toJson(*event)));
    updateQuery.addBindValue(isoUtc(now));
    updateQuery.addBindValue(isoUtc(now));
    updateQuery.addBindValue(pendingId);
    if (!updateQuery.exec() || !m_database.commit()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(updateQuery, QStringLiteral("coalesce event"));
      }
      m_database.rollback();
      return false;
    }
    return true;
  }

  const QString idempotencyKey = newUuid();

  QSqlQuery outboxQuery(m_database);
  outboxQuery.prepare(QStringLiteral(R"SQL(
    INSERT INTO outbox
      (account_id, calendar_id, event_id, operation, state, idempotency_key,
       expected_revision, payload_json, attempts, next_attempt_at, created_at,
       updated_at)
    VALUES (?, ?, ?, ?, 'pending', ?, ?, ?, 0, ?, ?, ?)
  )SQL"));
  outboxQuery.addBindValue(accountId);
  outboxQuery.addBindValue(event->calendarId);
  outboxQuery.addBindValue(event->id);
  outboxQuery.addBindValue(outboxOperationToString(operation));
  outboxQuery.addBindValue(idempotencyKey);
  outboxQuery.addBindValue(nonNull(event->etag));
  outboxQuery.addBindValue(compactJson(toJson(*event)));
  outboxQuery.addBindValue(isoUtc(now));
  outboxQuery.addBindValue(isoUtc(now));
  outboxQuery.addBindValue(isoUtc(now));
  if (!outboxQuery.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(outboxQuery, QStringLiteral("queue event"));
    }
    m_database.rollback();
    return false;
  }
  if (!m_database.commit()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  return true;
}

bool Database::markLocalEventRemoved(const QString& eventId, QString* errorMessage) {
  Event existing = event(eventId, errorMessage);
  if (existing.id.isEmpty()) {
    return false;
  }
  existing.deleted = true;
  return saveLocalEvent(&existing, OutboxOperation::Remove, errorMessage);
}

Event Database::eventFromQuery(const QSqlQuery& query) const {
  Event result;
  result.id = query.value(QStringLiteral("id")).toString();
  result.calendarId = query.value(QStringLiteral("calendar_id")).toString();
  result.remoteId = query.value(QStringLiteral("remote_id")).toString();
  result.uid = query.value(QStringLiteral("uid")).toString();
  result.etag = query.value(QStringLiteral("etag")).toString();
  result.summary = query.value(QStringLiteral("summary")).toString();
  result.description = query.value(QStringLiteral("description")).toString();
  result.location = query.value(QStringLiteral("location")).toString();
  result.startUtc =
      dateTimeFromIso(query.value(QStringLiteral("start_utc")).toString());
  result.endUtc = dateTimeFromIso(query.value(QStringLiteral("end_utc")).toString());
  result.startDate = QDate::fromString(
      query.value(QStringLiteral("start_date")).toString(), Qt::ISODate);
  result.endDate = QDate::fromString(query.value(QStringLiteral("end_date")).toString(),
                                     Qt::ISODate);
  result.startTimeZone = query.value(QStringLiteral("start_timezone")).toString();
  result.endTimeZone = query.value(QStringLiteral("end_timezone")).toString();
  result.allDay = query.value(QStringLiteral("all_day")).toBool();
  result.status = query.value(QStringLiteral("status")).toString();
  result.transparency = query.value(QStringLiteral("transparency")).toString();
  result.recurrenceRule = query.value(QStringLiteral("recurrence_rule")).toString();
  result.recurrenceId = query.value(QStringLiteral("recurrence_id")).toString();
  result.sequence = query.value(QStringLiteral("sequence")).toInt();
  result.organizer =
      parseObject(query.value(QStringLiteral("organizer_json")).toString());
  result.attendees =
      parseArray(query.value(QStringLiteral("attendees_json")).toString());
  result.reminders =
      parseArray(query.value(QStringLiteral("reminders_json")).toString());
  result.rawPayload = query.value(QStringLiteral("raw_payload")).toString();
  result.rawFormat = query.value(QStringLiteral("raw_format")).toString();
  result.dirty = query.value(QStringLiteral("dirty")).toBool();
  result.deleted = query.value(QStringLiteral("deleted")).toBool();
  result.createdAt =
      dateTimeFromIso(query.value(QStringLiteral("created_at")).toString());
  result.updatedAt =
      dateTimeFromIso(query.value(QStringLiteral("updated_at")).toString());
  return result;
}

Event Database::event(const QString& eventId, QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("SELECT * FROM events WHERE id = ?"));
  query.addBindValue(eventId);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("get event"));
    }
    return {};
  }
  if (!query.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Event not found");
    }
    return {};
  }
  return eventFromQuery(query);
}

Event Database::eventByRemoteId(const QString& calendarId, const QString& remoteId,
                                QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(
      "SELECT * FROM events WHERE calendar_id=? AND remote_id=? LIMIT 1"));
  query.addBindValue(calendarId);
  query.addBindValue(remoteId);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("get remote event"));
    }
    return {};
  }
  if (!query.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Event not found");
    }
    return {};
  }
  return eventFromQuery(query);
}

QList<Event> Database::eventsBetween(const QDateTime& startUtc, const QDateTime& endUtc,
                                     const QStringList& calendarIds,
                                     QString* errorMessage) const {
  QList<Event> candidates;
  if (!startUtc.isValid() || !endUtc.isValid() || startUtc >= endUtc) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("A valid bounded time range is required");
    }
    return candidates;
  }
  QString sql = QStringLiteral(R"SQL(
    SELECT * FROM events
    WHERE (
      recurrence_rule <> '' OR recurrence_id <> '' OR
      (deleted = 0 AND (
        (all_day = 0 AND end_utc > ? AND start_utc < ?)
        OR
        (all_day = 1 AND end_date > ? AND start_date < ?)
      ))
    )
  )SQL");
  if (!calendarIds.isEmpty()) {
    QStringList placeholders;
    placeholders.fill(QStringLiteral("?"), calendarIds.size());
    sql += QStringLiteral(" AND calendar_id IN (%1)")
               .arg(placeholders.join(QLatin1Char(',')));
  }
  sql += QStringLiteral(
      " ORDER BY all_day DESC, COALESCE(NULLIF(start_utc,''), start_date), id");

  QSqlQuery query(m_database);
  query.prepare(sql);
  query.addBindValue(isoUtc(startUtc));
  query.addBindValue(isoUtc(endUtc));
  query.addBindValue(startUtc.date().toString(Qt::ISODate));
  const QDate allDayEnd =
      endUtc.time() == QTime(0, 0) ? endUtc.date() : endUtc.date().addDays(1);
  query.addBindValue(allDayEnd.toString(Qt::ISODate));
  for (const QString& calendarId : calendarIds) {
    query.addBindValue(calendarId);
  }
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list events"));
    }
    return candidates;
  }
  while (query.next()) {
    candidates.append(eventFromQuery(query));
  }
  return RecurrenceExpander::expand(candidates, startUtc, endUtc).occurrences;
}

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
  item.expectedRevision = query.value(QStringLiteral("expected_revision")).toString();
  item.payload = parseObject(query.value(QStringLiteral("payload_json")).toString());
  item.attempts = query.value(QStringLiteral("attempts")).toInt();
  item.nextAttemptAt =
      dateTimeFromIso(query.value(QStringLiteral("next_attempt_at")).toString());
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
    ORDER BY id LIMIT ?
  )SQL"));
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

bool Database::updateOutboxState(qint64 id, OutboxState state, int attempts,
                                 const QDateTime& nextAttemptAt,
                                 const QString& errorCode, const QString& errorMessage,
                                 QString* databaseError) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE outbox SET state=?, attempts=?, next_attempt_at=?, error_code=?,
      error_message=?, updated_at=? WHERE id=?
  )SQL"));
  query.addBindValue(outboxStateToString(state));
  query.addBindValue(attempts);
  query.addBindValue(isoUtc(nextAttemptAt));
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
  return query.numRowsAffected() > 0;
}

bool Database::completeOutbox(const qint64 id, const Event* remoteEvent,
                              QString* errorMessage) {
  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  QSqlQuery itemQuery(m_database);
  itemQuery.prepare(
      QStringLiteral("SELECT event_id, operation FROM outbox WHERE id=?"));
  itemQuery.addBindValue(id);
  if (!itemQuery.exec() || !itemQuery.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = itemQuery.lastError().isValid()
                          ? itemQuery.lastError().text()
                          : QStringLiteral("Outbox item not found");
    }
    m_database.rollback();
    return false;
  }
  const QString eventId = itemQuery.value(0).toString();
  const OutboxOperation operation =
      outboxOperationFromString(itemQuery.value(1).toString());

  QSqlQuery finishQuery(m_database);
  finishQuery.prepare(
      QStringLiteral("UPDATE outbox SET state='done', error_code='', error_message='', "
                     "updated_at=? WHERE id=?"));
  finishQuery.addBindValue(isoUtc(nowUtc()));
  finishQuery.addBindValue(id);
  if (!finishQuery.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(finishQuery, QStringLiteral("finish outbox"));
    }
    m_database.rollback();
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
    m_database.rollback();
    return false;
  }
  const bool hasLaterMutation = laterQuery.value(0).toInt() > 0;

  if (operation == OutboxOperation::Remove && !hasLaterMutation) {
    QSqlQuery deleteQuery(m_database);
    deleteQuery.prepare(QStringLiteral("DELETE FROM events WHERE id=?"));
    deleteQuery.addBindValue(eventId);
    if (!deleteQuery.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(deleteQuery, QStringLiteral("finish removal"));
      }
      m_database.rollback();
      return false;
    }
  } else if (remoteEvent != nullptr) {
    if (hasLaterMutation) {
      QSqlQuery revisionQuery(m_database);
      revisionQuery.prepare(QStringLiteral(R"SQL(
        UPDATE events SET remote_id=?, etag=?, dirty=1, updated_at=?
        WHERE id=?
      )SQL"));
      revisionQuery.addBindValue(nonNull(remoteEvent->remoteId));
      revisionQuery.addBindValue(nonNull(remoteEvent->etag));
      revisionQuery.addBindValue(isoUtc(nowUtc()));
      revisionQuery.addBindValue(eventId);
      if (!revisionQuery.exec()) {
        if (errorMessage != nullptr) {
          *errorMessage =
              sqlError(revisionQuery, QStringLiteral("update remote revision"));
        }
        m_database.rollback();
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
        m_database.rollback();
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
        m_database.rollback();
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
      m_database.rollback();
      return false;
    }
  }
  if (!m_database.commit()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  return true;
}

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
  return true;
}

}  // namespace omacalendar
