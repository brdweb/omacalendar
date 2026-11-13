// Database: schema creation and migrations.
// Split out of database.cpp; shared helpers live in database_p.h.

#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "core/database.h"
#include "core/database_p.h"

namespace omacalendar {

using namespace detail;

bool Database::migrate(QString* errorMessage) {
  if (schemaVersion() > kDatabaseSchemaVersion) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Database schema is newer than this build");
    }
    return false;
  }
  if (schemaVersion() == 2) {
    return ensureOutboxMoveSchema(errorMessage) &&
           ensureConflictUniquenessSchema(errorMessage) &&
           ensureReminderDeliverySchema(errorMessage) &&
           ensureSyncCoverageSchema(errorMessage) &&
           ensureProviderResourcesSchema(errorMessage) &&
           repairInclusiveAllDayEndDates(errorMessage) &&
           migrateSeriesBounds(errorMessage) && ensureTaskSchema(errorMessage);
  }
  if (schemaVersion() == kDatabaseSchemaVersion) {
    return ensureOutboxMoveSchema(errorMessage) &&
           ensureConflictUniquenessSchema(errorMessage) &&
           ensureReminderDeliverySchema(errorMessage) &&
           ensureSyncCoverageSchema(errorMessage) &&
           ensureProviderResourcesSchema(errorMessage) &&
           ensureReadPerformanceIndexes(errorMessage) &&
           repairInclusiveAllDayEndDates(errorMessage) &&
           ensureTaskSchema(errorMessage);
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
          provider TEXT NOT NULL CHECK(provider IN ('local','caldav','google','ics')),
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
          color_override TEXT NOT NULL DEFAULT '',
          position INTEGER NOT NULL DEFAULT 0,
          ignore_alerts INTEGER NOT NULL DEFAULT 0,
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
          url TEXT NOT NULL DEFAULT '',
          conference_url TEXT NOT NULL DEFAULT '',
          start_utc TEXT NOT NULL DEFAULT '',
          end_utc TEXT NOT NULL DEFAULT '',
          start_date TEXT NOT NULL DEFAULT '',
          end_date TEXT NOT NULL DEFAULT '',
          start_timezone TEXT NOT NULL DEFAULT '',
          end_timezone TEXT NOT NULL DEFAULT '',
          all_day INTEGER NOT NULL DEFAULT 0,
          time_kind TEXT NOT NULL DEFAULT 'zoned'
            CHECK(time_kind IN ('zoned','floating','all_day')),
          status TEXT NOT NULL DEFAULT 'confirmed',
          transparency TEXT NOT NULL DEFAULT 'opaque',
          visibility TEXT NOT NULL DEFAULT 'default',
          recurrence_rule TEXT NOT NULL DEFAULT '',
          recurrence_id TEXT NOT NULL DEFAULT '',
          series_until_utc TEXT,
          series_until_date TEXT,
          original_start_utc TEXT,
          original_start_date TEXT,
          sequence INTEGER NOT NULL DEFAULT 0,
          organizer_json TEXT NOT NULL DEFAULT '{}',
          attendees_json TEXT NOT NULL DEFAULT '[]',
          reminders_json TEXT NOT NULL DEFAULT '[]',
          raw_payload TEXT NOT NULL DEFAULT '',
          raw_format TEXT NOT NULL DEFAULT '',
          dirty INTEGER NOT NULL DEFAULT 0,
          deleted INTEGER NOT NULL DEFAULT 0,
          local_revision INTEGER NOT NULL DEFAULT 0,
          sync_state TEXT NOT NULL DEFAULT 'clean',
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
          operation TEXT NOT NULL
            CHECK(operation IN ('create','update','move','remove')),
          state TEXT NOT NULL DEFAULT 'pending'
            CHECK(state IN ('pending','sending','retry_wait','blocked','done')),
          idempotency_key TEXT NOT NULL UNIQUE,
          dependency_id TEXT NOT NULL DEFAULT '',
          expected_revision TEXT NOT NULL DEFAULT '',
          recurrence_scope TEXT NOT NULL DEFAULT 'series',
          send_updates TEXT NOT NULL DEFAULT 'none',
          payload_json TEXT NOT NULL DEFAULT '{}',
          attempts INTEGER NOT NULL DEFAULT 0,
          next_attempt_at TEXT NOT NULL DEFAULT '',
          not_before TEXT NOT NULL DEFAULT '',
          lease_until TEXT NOT NULL DEFAULT '',
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
          mutation_id INTEGER REFERENCES outbox(id) ON DELETE SET NULL,
          kind TEXT NOT NULL DEFAULT 'remote_changed',
          local_revision TEXT NOT NULL DEFAULT '',
          remote_revision TEXT NOT NULL DEFAULT '',
          local_snapshot_json TEXT NOT NULL DEFAULT '{}',
          remote_snapshot_json TEXT NOT NULL DEFAULT '{}',
          remote_payload TEXT NOT NULL DEFAULT '',
          state TEXT NOT NULL DEFAULT 'unresolved',
          resolution_revision INTEGER NOT NULL DEFAULT 0,
          created_at TEXT NOT NULL,
          resolved_at TEXT NOT NULL DEFAULT ''
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE UNIQUE INDEX conflicts_unresolved_mutation_index
        ON conflicts(mutation_id)
        WHERE mutation_id IS NOT NULL AND state='unresolved'
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE UNIQUE INDEX conflicts_unresolved_event_index
        ON conflicts(event_id)
        WHERE state='unresolved'
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
      QStringLiteral(R"SQL(
        CREATE TABLE provider_resources (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          calendar_id TEXT NOT NULL REFERENCES calendars(id) ON DELETE CASCADE,
          canonical_key TEXT NOT NULL,
          remote_revision TEXT NOT NULL DEFAULT '',
          format TEXT NOT NULL DEFAULT '',
          acknowledged_json TEXT NOT NULL DEFAULT '{}',
          raw_payload TEXT NOT NULL DEFAULT '',
          sync_generation INTEGER NOT NULL DEFAULT 0,
          UNIQUE(calendar_id, canonical_key)
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE reminder_jobs (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          event_id TEXT NOT NULL REFERENCES events(id) ON DELETE CASCADE,
          occurrence_id TEXT NOT NULL DEFAULT '',
          fingerprint TEXT NOT NULL UNIQUE,
          fire_at TEXT NOT NULL,
          snoozed_until TEXT NOT NULL DEFAULT '',
          state TEXT NOT NULL DEFAULT 'pending',
          event_revision INTEGER NOT NULL DEFAULT 0,
          claimed_at TEXT NOT NULL DEFAULT '',
          claim_token TEXT NOT NULL DEFAULT '',
          lease_expires_at TEXT NOT NULL DEFAULT '',
          delivered_at TEXT NOT NULL DEFAULT ''
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX reminder_jobs_ready_index
        ON reminder_jobs(state, snoozed_until, fire_at)
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX reminder_jobs_claim_lease_index
        ON reminder_jobs(state, lease_expires_at)
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE notification_deliveries (
          fingerprint TEXT PRIMARY KEY,
          kind TEXT NOT NULL,
          event_id TEXT NOT NULL REFERENCES events(id) ON DELETE CASCADE,
          event_revision INTEGER NOT NULL DEFAULT 0,
          state TEXT NOT NULL DEFAULT 'claimed',
          claimed_at TEXT NOT NULL,
          delivered_at TEXT NOT NULL DEFAULT ''
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX notification_deliveries_event_index
        ON notification_deliveries(event_id, kind, state)
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE ics_subscriptions (
          account_id TEXT PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
          url TEXT NOT NULL,
          etag TEXT NOT NULL DEFAULT '',
          last_modified TEXT NOT NULL DEFAULT '',
          refresh_seconds INTEGER NOT NULL DEFAULT 3600,
          last_success_at TEXT NOT NULL DEFAULT '',
          last_error_code TEXT NOT NULL DEFAULT '',
          last_error_message TEXT NOT NULL DEFAULT ''
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE sync_coverage (
          calendar_id TEXT NOT NULL REFERENCES calendars(id) ON DELETE CASCADE,
          start_utc TEXT NOT NULL,
          end_utc TEXT NOT NULL,
          complete INTEGER NOT NULL DEFAULT 0,
          updated_at TEXT NOT NULL,
          PRIMARY KEY(calendar_id, start_utc, end_utc)
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE sync_runs (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
          calendar_id TEXT NOT NULL DEFAULT '',
          phase TEXT NOT NULL,
          state TEXT NOT NULL,
          progress REAL NOT NULL DEFAULT 0,
          error_code TEXT NOT NULL DEFAULT '',
          error_message TEXT NOT NULL DEFAULT '',
          started_at TEXT NOT NULL,
          finished_at TEXT NOT NULL DEFAULT ''
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TABLE metadata (
          key TEXT PRIMARY KEY,
          value TEXT NOT NULL
        )
      )SQL"),
      QStringLiteral("INSERT INTO metadata(key,value) VALUES ('change_revision','0')"),
      QStringLiteral(R"SQL(
        CREATE VIRTUAL TABLE events_fts USING fts5(
          event_id UNINDEXED, summary, location, description, attendees,
          tokenize='unicode61 remove_diacritics 2'
        )
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TRIGGER events_fts_insert AFTER INSERT ON events BEGIN
          INSERT INTO events_fts(event_id,summary,location,description,attendees)
          VALUES (new.id,new.summary,new.location,new.description,new.attendees_json);
        END
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TRIGGER events_fts_update AFTER UPDATE ON events BEGIN
          DELETE FROM events_fts WHERE event_id=old.id;
          INSERT INTO events_fts(event_id,summary,location,description,attendees)
          VALUES (new.id,new.summary,new.location,new.description,new.attendees_json);
        END
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE TRIGGER events_fts_delete AFTER DELETE ON events BEGIN
          DELETE FROM events_fts WHERE event_id=old.id;
        END
      )SQL"),
      QStringLiteral(R"SQL(
        INSERT INTO accounts
          (id,provider,display_name,principal,endpoint,enabled,auth_status,
           created_at,updated_at)
        VALUES ('local-account','local','On this device','','',1,'connected',
                strftime('%Y-%m-%dT%H:%M:%fZ','now'),
                strftime('%Y-%m-%dT%H:%M:%fZ','now'))
      )SQL"),
      QStringLiteral(R"SQL(
        INSERT INTO calendars
          (id,account_id,name,color,timezone,read_only,enabled,capabilities_json)
        VALUES ('local-default','local-account','Personal','#7aa2f7','',0,1,
                '{"provider":"local","createEvent":true,"updateEvent":true,"removeEvent":true}')
      )SQL"),
      QStringLiteral(R"SQL(
        INSERT INTO calendar_sets
          (id,name,is_default,default_calendar_id,created_at,updated_at)
        VALUES ('all-calendars','All Calendars',1,'local-default',
                strftime('%Y-%m-%dT%H:%M:%fZ','now'),
                strftime('%Y-%m-%dT%H:%M:%fZ','now'))
      )SQL"),
      QStringLiteral(R"SQL(
        INSERT INTO calendar_set_members(set_id,calendar_id,position)
        VALUES ('all-calendars','local-default',0)
      )SQL"),
      QStringLiteral("PRAGMA user_version = %1").arg(kDatabaseSchemaVersion),
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
  return ensureOutboxMoveSchema(errorMessage) &&
         ensureConflictUniquenessSchema(errorMessage) &&
         ensureReminderDeliverySchema(errorMessage) &&
         ensureSyncCoverageSchema(errorMessage) &&
         ensureProviderResourcesSchema(errorMessage) &&
         ensureReadPerformanceIndexes(errorMessage) &&
         repairInclusiveAllDayEndDates(errorMessage) && ensureTaskSchema(errorMessage);
}

bool Database::ensureOutboxMoveSchema(QString* errorMessage) {
  QSqlQuery schemaQuery(m_database);
  schemaQuery.prepare(QStringLiteral(
      "SELECT sql FROM sqlite_master WHERE type='table' AND name='outbox'"));
  if (!schemaQuery.exec() || !schemaQuery.next()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          schemaQuery.lastError().isValid()
              ? sqlError(schemaQuery, QStringLiteral("inspect outbox schema"))
              : QStringLiteral("Outbox table is missing");
    }
    return false;
  }
  if (schemaQuery.value(0).toString().contains(QStringLiteral("'move'"))) {
    return true;
  }

  // Schema 2 was intentionally kept as the development schema. Rebuild only
  // the outbox table so existing schema-2 databases gain the first-class move
  // operation without discarding durable work or changing the public schema
  // version. Foreign-key validation is restored before returning.
  if (!execute(QStringLiteral("PRAGMA foreign_keys = OFF"), errorMessage)) {
    return false;
  }
  const auto restoreForeignKeys = [this, errorMessage]() {
    return execute(QStringLiteral("PRAGMA foreign_keys = ON"), errorMessage);
  };
  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    restoreForeignKeys();
    return false;
  }
  const QStringList statements = {
      QStringLiteral(R"SQL(
        CREATE TABLE outbox_with_move (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
          calendar_id TEXT NOT NULL REFERENCES calendars(id) ON DELETE CASCADE,
          event_id TEXT NOT NULL REFERENCES events(id) ON DELETE CASCADE,
          operation TEXT NOT NULL
            CHECK(operation IN ('create','update','move','remove')),
          state TEXT NOT NULL DEFAULT 'pending'
            CHECK(state IN ('pending','sending','retry_wait','blocked','done')),
          idempotency_key TEXT NOT NULL UNIQUE,
          dependency_id TEXT NOT NULL DEFAULT '',
          expected_revision TEXT NOT NULL DEFAULT '',
          recurrence_scope TEXT NOT NULL DEFAULT 'series',
          send_updates TEXT NOT NULL DEFAULT 'none',
          payload_json TEXT NOT NULL DEFAULT '{}',
          attempts INTEGER NOT NULL DEFAULT 0,
          next_attempt_at TEXT NOT NULL DEFAULT '',
          not_before TEXT NOT NULL DEFAULT '',
          lease_until TEXT NOT NULL DEFAULT '',
          error_code TEXT NOT NULL DEFAULT '',
          error_message TEXT NOT NULL DEFAULT '',
          created_at TEXT NOT NULL,
          updated_at TEXT NOT NULL
        )
      )SQL"),
      QStringLiteral(R"SQL(
        INSERT INTO outbox_with_move
          (id, account_id, calendar_id, event_id, operation, state,
           idempotency_key, dependency_id, expected_revision, recurrence_scope,
           send_updates, payload_json, attempts, next_attempt_at, not_before,
           lease_until, error_code, error_message, created_at, updated_at)
        SELECT id, account_id, calendar_id, event_id, operation, state,
               idempotency_key, dependency_id, expected_revision, recurrence_scope,
               send_updates, payload_json, attempts, next_attempt_at, not_before,
               lease_until, error_code, error_message, created_at, updated_at
        FROM outbox
      )SQL"),
      QStringLiteral("DROP TABLE outbox"),
      QStringLiteral("ALTER TABLE outbox_with_move RENAME TO outbox"),
      QStringLiteral(R"SQL(
        CREATE INDEX outbox_ready_index
        ON outbox(state, next_attempt_at, id)
      )SQL"),
  };
  for (const QString& statement : statements) {
    if (!execute(statement, errorMessage)) {
      m_database.rollback();
      restoreForeignKeys();
      return false;
    }
  }
  if (!m_database.commit()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    m_database.rollback();
    restoreForeignKeys();
    return false;
  }
  if (!restoreForeignKeys()) {
    return false;
  }
  QSqlQuery foreignKeyCheck(m_database);
  if (!foreignKeyCheck.exec(QStringLiteral("PRAGMA foreign_key_check"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(foreignKeyCheck, QStringLiteral("validate outbox repair"));
    }
    return false;
  }
  if (foreignKeyCheck.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "The outbox schema repair exposed an invalid foreign-key reference");
    }
    return false;
  }
  return true;
}

bool Database::ensureConflictUniquenessSchema(QString* errorMessage) {
  QSqlQuery savepoint(m_database);
  if (!savepoint.exec(QStringLiteral("SAVEPOINT conflict_uniqueness_schema"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(savepoint, QStringLiteral("start conflict schema repair"));
    }
    return false;
  }
  const auto fail = [this]() {
    QSqlQuery rollback(m_database);
    rollback.exec(QStringLiteral("ROLLBACK TO conflict_uniqueness_schema"));
    rollback.exec(QStringLiteral("RELEASE conflict_uniqueness_schema"));
    return false;
  };

  // Development schema-2 databases could contain both a pull-discovered row
  // (without a mutation) and a later provider-precondition row for the same
  // event. Keep the mutation-linked row when one exists, otherwise the newest
  // row, and retain every loser as resolved audit history. An older unresolved
  // row can also survive when the user resolved only its newer duplicate; that
  // orphan is already covered by the later resolution and is superseded too.
  QSqlQuery supersede(m_database);
  supersede.prepare(QStringLiteral(R"SQL(
    UPDATE conflicts AS loser
    SET state='superseded',
        resolved_at=CASE WHEN resolved_at='' THEN ? ELSE resolved_at END,
        resolution_revision=CASE
          WHEN resolution_revision=0 THEN ? ELSE resolution_revision END
    WHERE loser.state='unresolved' AND (
      EXISTS (
        SELECT 1 FROM conflicts AS winner
        WHERE winner.event_id=loser.event_id AND winner.state='unresolved' AND (
          (winner.mutation_id IS NOT NULL AND loser.mutation_id IS NULL) OR
          ((winner.mutation_id IS NULL)=(loser.mutation_id IS NULL) AND (
            winner.created_at>loser.created_at OR
            (winner.created_at=loser.created_at AND winner.id>loser.id)
          ))
        )
      ) OR EXISTS (
        SELECT 1 FROM conflicts AS resolved
        WHERE resolved.event_id=loser.event_id
          AND resolved.state IN ('keep_remote','keep_local','merge')
          AND resolved.resolved_at<>'' AND (
            resolved.created_at>loser.created_at OR
            (resolved.created_at=loser.created_at AND resolved.id>loser.id)
          )
      )
    )
  )SQL"));
  supersede.addBindValue(isoUtc(nowUtc()));
  supersede.addBindValue(qMax<qint64>(1, changeRevision() + 1));
  if (!supersede.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(supersede, QStringLiteral("repair duplicate conflicts"));
    }
    return fail();
  }
  const bool changed = supersede.numRowsAffected() > 0;
  if (!execute(QStringLiteral(R"SQL(
        CREATE UNIQUE INDEX IF NOT EXISTS conflicts_unresolved_event_index
        ON conflicts(event_id)
        WHERE state='unresolved'
      )SQL"),
               errorMessage) ||
      (changed && !bumpChangeRevision(errorMessage))) {
    return fail();
  }

  QSqlQuery release(m_database);
  if (!release.exec(QStringLiteral("RELEASE conflict_uniqueness_schema"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(release, QStringLiteral("commit conflict schema repair"));
    }
    return fail();
  }
  return true;
}

bool Database::ensureReminderDeliverySchema(QString* errorMessage) {
  QSqlQuery columns(m_database);
  if (!columns.exec(QStringLiteral("PRAGMA table_info(reminder_jobs)"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(columns, QStringLiteral("inspect reminder delivery schema"));
    }
    return false;
  }
  bool foundReminderTable = false;
  bool hasClaimedAt = false;
  bool hasClaimToken = false;
  bool hasLeaseExpiresAt = false;
  while (columns.next()) {
    foundReminderTable = true;
    const QString name = columns.value(QStringLiteral("name")).toString();
    hasClaimedAt = hasClaimedAt || name == QStringLiteral("claimed_at");
    hasClaimToken = hasClaimToken || name == QStringLiteral("claim_token");
    hasLeaseExpiresAt = hasLeaseExpiresAt || name == QStringLiteral("lease_expires_at");
  }
  if (!foundReminderTable) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Reminder jobs table is missing");
    }
    return false;
  }

  QSqlQuery savepoint(m_database);
  if (!savepoint.exec(QStringLiteral("SAVEPOINT reminder_delivery_schema"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(savepoint, QStringLiteral("start reminder schema repair"));
    }
    return false;
  }
  const auto fail = [this]() {
    QSqlQuery rollback(m_database);
    rollback.exec(QStringLiteral("ROLLBACK TO reminder_delivery_schema"));
    rollback.exec(QStringLiteral("RELEASE reminder_delivery_schema"));
    return false;
  };
  if (!hasClaimedAt &&
      !execute(QStringLiteral(
                   "ALTER TABLE reminder_jobs ADD COLUMN claimed_at TEXT NOT NULL "
                   "DEFAULT ''"),
               errorMessage)) {
    return fail();
  }
  if (!hasClaimToken &&
      !execute(QStringLiteral(
                   "ALTER TABLE reminder_jobs ADD COLUMN claim_token TEXT NOT NULL "
                   "DEFAULT ''"),
               errorMessage)) {
    return fail();
  }
  if (!hasLeaseExpiresAt &&
      !execute(QStringLiteral(
                   "ALTER TABLE reminder_jobs ADD COLUMN lease_expires_at TEXT NOT "
                   "NULL DEFAULT ''"),
               errorMessage)) {
    return fail();
  }
  if (!execute(QStringLiteral(R"SQL(
        CREATE TABLE IF NOT EXISTS notification_deliveries (
          fingerprint TEXT PRIMARY KEY,
          kind TEXT NOT NULL,
          event_id TEXT NOT NULL REFERENCES events(id) ON DELETE CASCADE,
          event_revision INTEGER NOT NULL DEFAULT 0,
          state TEXT NOT NULL DEFAULT 'claimed',
          claimed_at TEXT NOT NULL,
          delivered_at TEXT NOT NULL DEFAULT ''
        )
      )SQL"),
               errorMessage) ||
      !execute(QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS notification_deliveries_event_index
        ON notification_deliveries(event_id, kind, state)
      )SQL"),
               errorMessage) ||
      !execute(QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS reminder_jobs_claim_lease_index
        ON reminder_jobs(state, lease_expires_at)
      )SQL"),
               errorMessage)) {
    return fail();
  }
  QSqlQuery release(m_database);
  if (!release.exec(QStringLiteral("RELEASE reminder_delivery_schema"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(release, QStringLiteral("commit reminder schema repair"));
    }
    return fail();
  }
  return true;
}

bool Database::ensureSyncCoverageSchema(QString* errorMessage) {
  // Schema 2 is still the development schema. Keep this repair idempotent so
  // databases created by earlier development builds gain durable coverage
  // without being reset.
  return execute(QStringLiteral(R"SQL(
           CREATE TABLE IF NOT EXISTS sync_coverage (
             calendar_id TEXT NOT NULL REFERENCES calendars(id) ON DELETE CASCADE,
             start_utc TEXT NOT NULL,
             end_utc TEXT NOT NULL,
             complete INTEGER NOT NULL DEFAULT 0,
             updated_at TEXT NOT NULL,
             PRIMARY KEY(calendar_id, start_utc, end_utc)
           )
         )SQL"),
                 errorMessage) &&
         execute(QStringLiteral(R"SQL(
           CREATE INDEX IF NOT EXISTS sync_coverage_lookup_index
           ON sync_coverage(calendar_id, complete, start_utc, end_utc)
         )SQL"),
                 errorMessage);
}

bool Database::ensureProviderResourcesSchema(QString* errorMessage) {
  QSqlQuery savepoint(m_database);
  if (!savepoint.exec(QStringLiteral("SAVEPOINT provider_resources_schema"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(savepoint, QStringLiteral("start provider resource schema repair"));
    }
    return false;
  }
  const auto fail = [this, errorMessage](const QString& fallback) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = fallback;
    }
    QSqlQuery rollback(m_database);
    rollback.exec(QStringLiteral("ROLLBACK TO provider_resources_schema"));
    rollback.exec(QStringLiteral("RELEASE provider_resources_schema"));
    return false;
  };

  if (!execute(QStringLiteral(R"SQL(
        CREATE TABLE IF NOT EXISTS provider_resources (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          calendar_id TEXT NOT NULL REFERENCES calendars(id) ON DELETE CASCADE,
          canonical_key TEXT NOT NULL,
          remote_revision TEXT NOT NULL DEFAULT '',
          format TEXT NOT NULL DEFAULT '',
          acknowledged_json TEXT NOT NULL DEFAULT '{}',
          raw_payload TEXT NOT NULL DEFAULT '',
          sync_generation INTEGER NOT NULL DEFAULT 0,
          UNIQUE(calendar_id, canonical_key)
        )
      )SQL"),
               errorMessage) ||
      !execute(QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS provider_resources_calendar_index
        ON provider_resources(calendar_id, canonical_key)
      )SQL"),
               errorMessage)) {
    return fail(QStringLiteral("Unable to repair provider resource storage"));
  }

  // Early schema-2 builds stored a full CalDAV VCALENDAR in every normalized
  // event row. Preserve the newest copy per canonical resource before clearing
  // those duplicates. Google JSON and every non-CalDAV payload remain per-event.
  QSqlQuery legacy(m_database);
  if (!legacy.exec(QStringLiteral(R"SQL(
        SELECT e.calendar_id,
          CASE WHEN instr(e.remote_id,'#')>0
               THEN substr(e.remote_id,1,instr(e.remote_id,'#')-1)
               ELSE e.remote_id END AS resource_key,
          e.etag,e.raw_format,e.raw_payload
        FROM events e
        JOIN calendars c ON c.id=e.calendar_id
        JOIN accounts a ON a.id=c.account_id
        WHERE a.provider='caldav' AND e.remote_id<>''
          AND e.raw_format='text/calendar' AND e.raw_payload<>''
          AND NOT EXISTS (
            SELECT 1 FROM provider_resources p WHERE p.calendar_id=e.calendar_id
              AND p.canonical_key=CASE WHEN instr(e.remote_id,'#')>0
                  THEN substr(e.remote_id,1,instr(e.remote_id,'#')-1)
                  ELSE e.remote_id END
          )
        ORDER BY e.updated_at DESC,e.id DESC
      )SQL"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(legacy, QStringLiteral("load legacy CalDAV payloads"));
    }
    return fail(QStringLiteral("Unable to read legacy CalDAV payloads"));
  }
  QList<ProviderResource> resources;
  QSet<QString> observed;
  while (legacy.next()) {
    ProviderResource resource;
    resource.calendarId = legacy.value(0).toString();
    resource.canonicalKey = legacy.value(1).toString();
    resource.remoteRevision = legacy.value(2).toString();
    resource.format = legacy.value(3).toString();
    resource.rawPayload = legacy.value(4).toString();
    const QString identity =
        resource.calendarId + QLatin1Char('\n') + resource.canonicalKey;
    if (resource.canonicalKey.isEmpty() || observed.contains(identity)) {
      continue;
    }
    observed.insert(identity);
    resources.append(std::move(resource));
  }
  legacy.finish();
  for (const ProviderResource& resource : std::as_const(resources)) {
    if (!upsertProviderResource(resource, errorMessage)) {
      return fail(QStringLiteral("Unable to migrate a CalDAV provider resource"));
    }
  }

  if (!execute(QStringLiteral(R"SQL(
        UPDATE events SET raw_payload='',raw_format=''
        WHERE raw_format='text/calendar' AND raw_payload<>'' AND dirty=0
          AND EXISTS (
            SELECT 1 FROM calendars c JOIN accounts a ON a.id=c.account_id
            WHERE c.id=events.calendar_id AND a.provider='caldav'
          )
          AND EXISTS (
            SELECT 1 FROM provider_resources pr
            WHERE pr.calendar_id=events.calendar_id
              AND pr.canonical_key=(
                CASE WHEN instr(events.remote_id,'#')>0
                     THEN substr(events.remote_id,1,instr(events.remote_id,'#')-1)
                     ELSE events.remote_id END
              )
          )
      )SQL"),
               errorMessage)) {
    return fail(QStringLiteral("Unable to normalize legacy CalDAV payloads"));
  }

  QSqlQuery release(m_database);
  if (!release.exec(QStringLiteral("RELEASE provider_resources_schema"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(release, QStringLiteral("commit provider resource schema repair"));
    }
    return fail(QStringLiteral("Unable to commit provider resource schema repair"));
  }
  return true;
}

bool Database::repairInclusiveAllDayEndDates(QString* errorMessage) {
  // Provider read paths once accepted an inclusive all-day end date, so an
  // existing cache can hold a same-day span. Range queries compare the stored
  // end_date in SQL, which discards such a row before any read-time repair can
  // see it, so the stored rows themselves have to be corrected. Keep this
  // idempotent: it is a no-op once every span is exclusive.
  const QStringList statements = {
      QStringLiteral(R"SQL(
        UPDATE events SET end_date=date(start_date,'+1 day')
        WHERE all_day=1 AND start_date<>''
          AND (end_date='' OR end_date<=start_date)
          AND date(start_date,'+1 day') IS NOT NULL
      )SQL"),
      QStringLiteral(R"SQL(
        UPDATE event_instances SET end_date=date(start_date,'+1 day')
        WHERE all_day=1 AND start_date<>''
          AND (end_date='' OR end_date<=start_date)
          AND date(start_date,'+1 day') IS NOT NULL
      )SQL"),
  };
  for (const QString& statement : statements) {
    if (!execute(statement, errorMessage)) {
      return false;
    }
  }
  return true;
}

bool Database::ensureReadPerformanceIndexes(QString* errorMessage) {
  // Both the ordinary event ranges and the finite-series upper bounds use
  // partial indexes; NULL bounds are deliberately retained for infinite or
  // unparseable recurrences.
  // Coalesce NULL (infinite) to a high sort key in the index and predicate.
  // A plain "IS NULL OR > start" prevents SQLite from using the bound as a
  // range constraint, even when INDEXED BY forces the right partial index.
  const QStringList statements = {
      QStringLiteral("DROP INDEX IF EXISTS events_series_utc_index"),
      QStringLiteral("DROP INDEX IF EXISTS events_series_date_index"),
      QStringLiteral("DROP INDEX IF EXISTS events_series_floating_index"),
      QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS events_timed_range_active_index
        ON events(start_utc, end_utc, calendar_id)
        WHERE deleted=0 AND all_day=0
          AND recurrence_rule='' AND recurrence_id=''
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS events_all_day_range_active_index
        ON events(start_date, end_date, calendar_id)
        WHERE deleted=0 AND all_day=1
          AND recurrence_rule='' AND recurrence_id=''
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS events_recurrence_active_index
        ON events(calendar_id, uid, recurrence_id)
        WHERE deleted=0 AND (recurrence_rule<>'' OR recurrence_id<>'')
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS events_series_utc_bound_index
        ON events(COALESCE(series_until_utc,'~'), start_utc, calendar_id)
        WHERE deleted=0 AND recurrence_rule<>'' AND recurrence_id=''
          AND all_day=0 AND time_kind<>'floating'
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS events_series_date_bound_index
        ON events(COALESCE(series_until_date,'~'), start_date, calendar_id)
        WHERE deleted=0 AND recurrence_rule<>'' AND recurrence_id=''
          AND all_day=1
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS events_series_floating_bound_index
        ON events(COALESCE(series_until_date,'~'), start_utc, calendar_id)
        WHERE deleted=0 AND recurrence_rule<>'' AND recurrence_id=''
          AND all_day=0 AND time_kind='floating'
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS events_exception_utc_index
        ON events(original_start_utc, start_utc, end_utc)
        WHERE recurrence_id<>''
      )SQL"),
      QStringLiteral(R"SQL(
        CREATE INDEX IF NOT EXISTS events_exception_date_index
        ON events(original_start_date, start_date, end_date)
        WHERE recurrence_id<>''
      )SQL"),
  };
  for (const QString& statement : statements) {
    if (!execute(statement, errorMessage)) {
      return false;
    }
  }
  return true;
}

bool Database::repairSeriesBounds(QString* errorMessage) {
  QSqlQuery rows(m_database);
  if (!rows.exec(QStringLiteral(R"SQL(
        SELECT * FROM events WHERE recurrence_rule<>'' OR recurrence_id<>''
      )SQL"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(rows, QStringLiteral("read recurring rows for upgrade"));
    }
    return false;
  }
  QSqlQuery update(m_database);
  update.prepare(QStringLiteral(R"SQL(
    UPDATE events SET series_until_utc=?,series_until_date=?,
      original_start_utc=?,original_start_date=? WHERE id=?
  )SQL"));
  while (rows.next()) {
    Event event = eventFromQuery(rows);
    if (!hydrateProviderResource(&event, errorMessage)) {
      return false;
    }
    const RecurrenceStoredBounds bounds = recurrenceBounds(event);
    update.bindValue(0, bounds.untilUtc);
    update.bindValue(1, bounds.untilDate);
    update.bindValue(2, bounds.originalUtc);
    update.bindValue(3, bounds.originalDate);
    update.bindValue(4, event.id);
    if (!update.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(update, QStringLiteral("backfill series bounds"));
      }
      return false;
    }
  }
  return true;
}

bool Database::migrateSeriesBounds(QString* errorMessage) {
  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  for (const QString& sql :
       {QStringLiteral("ALTER TABLE events ADD COLUMN series_until_utc TEXT"),
        QStringLiteral("ALTER TABLE events ADD COLUMN series_until_date TEXT"),
        QStringLiteral("ALTER TABLE events ADD COLUMN original_start_utc TEXT"),
        QStringLiteral("ALTER TABLE events ADD COLUMN original_start_date TEXT")}) {
    if (!execute(sql, errorMessage)) {
      m_database.rollback();
      return false;
    }
  }
  // This step takes schema 2 to exactly 3. A later schema adds its own step
  // after this one and updates migrate() to chain them.
  static_assert(kDatabaseSchemaVersion == 3,
                "chain a migration step from schema 3 to the new version");
  if (!repairSeriesBounds(errorMessage) ||
      !ensureReadPerformanceIndexes(errorMessage) ||
      !execute(QStringLiteral("PRAGMA user_version = 3"), errorMessage)) {
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

}  // namespace omacalendar
