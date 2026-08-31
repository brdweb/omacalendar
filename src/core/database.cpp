#include "core/database.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <algorithm>

#include "core/recurrenceexpander.h"

namespace omacalendar {
namespace {

constexpr int kCurrentSchemaVersion = 2;

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

const QStringList& databaseFileSuffixes() {
  static const QStringList suffixes = {QString(), QStringLiteral("-wal"),
                                       QStringLiteral("-shm"),
                                       QStringLiteral("-journal")};
  return suffixes;
}

QString transitionStagingPath(const QString& path) {
  return path + QStringLiteral(".schema2-transition");
}

bool setOwnerOnlyPermissions(const QString& path, const QString& description,
                             QString* errorMessage) {
  if (QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
    return true;
  }
  if (errorMessage != nullptr) {
    *errorMessage =
        QStringLiteral("Unable to restrict %1 permissions: %2").arg(description, path);
  }
  return false;
}

bool removeDatabaseFiles(const QString& path, QString* errorMessage = nullptr) {
  for (const QString& suffix : databaseFileSuffixes()) {
    const QString filePath = path + suffix;
    if (QFileInfo::exists(filePath) && !QFile::remove(filePath)) {
      if (errorMessage != nullptr) {
        *errorMessage =
            QStringLiteral("Unable to remove stale schema transition file: %1")
                .arg(filePath);
      }
      return false;
    }
  }
  return true;
}

bool moveDatabaseFiles(const QString& source, const QString& destination,
                       QString* errorMessage) {
  QStringList movedSuffixes;
  for (const QString& suffix : databaseFileSuffixes()) {
    const QString sourcePath = source + suffix;
    if (!QFileInfo::exists(sourcePath)) {
      continue;
    }
    if (QFile::rename(sourcePath, destination + suffix)) {
      movedSuffixes.append(suffix);
      continue;
    }

    for (auto it = movedSuffixes.crbegin(); it != movedSuffixes.crend(); ++it) {
      QFile::rename(destination + *it, source + *it);
    }
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Unable to move database file %1 to %2")
                          .arg(sourcePath, destination + suffix);
    }
    return false;
  }
  return true;
}

QString unusedLegacyBackupPath(const QString& path) {
  const QString stamp =
      QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz'Z'"));
  const QString prefix = path + QStringLiteral(".pre-v2-") + stamp;
  QString candidate = prefix + QStringLiteral(".backup");
  int discriminator = 1;
  const auto candidateExists = [](const QString& backupPath) {
    return std::any_of(databaseFileSuffixes().cbegin(), databaseFileSuffixes().cend(),
                       [&backupPath](const QString& suffix) {
                         return QFileInfo::exists(backupPath + suffix);
                       });
  };
  while (candidateExists(candidate)) {
    candidate = prefix + QStringLiteral("-%1.backup").arg(discriminator++);
  }
  return candidate;
}

}  // namespace

Database::Database()
    : m_connectionName(
          QStringLiteral("omacalendar-%1").arg(QUuid::createUuid().toString())) {}

Database::~Database() { close(); }

bool Database::open(const QString& path, QString* errorMessage) {
  close();
  const auto openConnection = [this, errorMessage](const QString& databasePath) {
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(databasePath);
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
    return true;
  };
  if (!openConnection(path)) {
    close();
    return false;
  }

  if (schemaVersion() == 1 && path != QStringLiteral(":memory:")) {
    // Archive first, but keep the active schema-1 file in place until a clean
    // schema-2 database has initialized successfully at the staging path.
    if (!archiveLegacyDatabase(path, errorMessage)) {
      close();
      return false;
    }
    close();

    const QString stagingPath = transitionStagingPath(path);
    if (!removeDatabaseFiles(stagingPath, errorMessage)) {
      return false;
    }
    QFile stagingFile(stagingPath);
    if (!stagingFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
      if (errorMessage != nullptr) {
        *errorMessage =
            QStringLiteral("Unable to create schema-2 staging database %1: %2")
                .arg(stagingPath, stagingFile.errorString());
      }
      removeDatabaseFiles(stagingPath);
      return false;
    }
    if (!setOwnerOnlyPermissions(
            stagingPath, QStringLiteral("schema-2 staging database"), errorMessage)) {
      stagingFile.close();
      removeDatabaseFiles(stagingPath);
      return false;
    }
    stagingFile.close();

    if (!openConnection(stagingPath) || !migrate(errorMessage) ||
        !setOwnerOnlyPermissions(
            stagingPath, QStringLiteral("schema-2 staging database"), errorMessage)) {
      close();
      removeDatabaseFiles(stagingPath);
      return false;
    }
    close();

    const QString rollbackPath = path + QStringLiteral(".schema1-rollback-") +
                                 QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!moveDatabaseFiles(path, rollbackPath, errorMessage)) {
      removeDatabaseFiles(stagingPath);
      return false;
    }
    if (!moveDatabaseFiles(stagingPath, path, errorMessage)) {
      QString restoreError;
      if (!moveDatabaseFiles(rollbackPath, path, &restoreError) &&
          errorMessage != nullptr) {
        *errorMessage +=
            QStringLiteral("; legacy restore also failed: %1").arg(restoreError);
      }
      removeDatabaseFiles(stagingPath);
      return false;
    }
    // The timestamped archive is the durable legacy copy. This same-directory
    // rollback exists only across the two renames that activate schema 2.
    removeDatabaseFiles(rollbackPath);

    if (!openConnection(path)) {
      close();
      return false;
    }
  }
  if (!migrate(errorMessage)) {
    close();
    return false;
  }
  if (path != QStringLiteral(":memory:") &&
      !execute(QStringLiteral("PRAGMA journal_mode = WAL"), errorMessage)) {
    close();
    return false;
  }
  if (path != QStringLiteral(":memory:")) {
    for (const QString& databaseFile :
         {path, path + QStringLiteral("-wal"), path + QStringLiteral("-shm")}) {
      if (QFileInfo::exists(databaseFile) &&
          !setOwnerOnlyPermissions(databaseFile, QStringLiteral("database file"),
                                   errorMessage)) {
        close();
        return false;
      }
    }
  }
  return true;
}

bool Database::archiveLegacyDatabase(const QString& path, QString* errorMessage) {
  QSqlQuery checkpoint(m_database);
  if (!checkpoint.exec(QStringLiteral("PRAGMA wal_checkpoint(FULL)")) ||
      !checkpoint.next()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(checkpoint, QStringLiteral("checkpoint schema-1 database"));
    }
    return false;
  }
  if (checkpoint.value(0).toInt() != 0) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "Unable to checkpoint schema-1 database because another writer is active");
    }
    return false;
  }
  checkpoint.finish();
  if (!execute(QStringLiteral("BEGIN IMMEDIATE"), errorMessage)) {
    return false;
  }

  const QString backup = unusedLegacyBackupPath(path);
  QStringList copiedFiles;
  for (const QString& suffix :
       {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
    const QString source = path + suffix;
    if (!QFileInfo::exists(source)) {
      continue;
    }
    const QString destination = backup + suffix;
    if (!QFile::copy(source, destination)) {
      if (errorMessage != nullptr) {
        *errorMessage =
            QStringLiteral("Unable to archive schema-1 database file: %1").arg(source);
      }
      execute(QStringLiteral("ROLLBACK"), nullptr);
      for (const QString& copiedFile : copiedFiles) {
        QFile::remove(copiedFile);
      }
      QFile::remove(destination);
      return false;
    }
    if (!setOwnerOnlyPermissions(destination, QStringLiteral("schema-1 archive"),
                                 errorMessage)) {
      execute(QStringLiteral("ROLLBACK"), nullptr);
      for (const QString& copiedFile : copiedFiles) {
        QFile::remove(copiedFile);
      }
      QFile::remove(destination);
      return false;
    }
    copiedFiles.append(destination);
  }
  if (copiedFiles.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("Schema-1 database file disappeared during archival");
    }
    execute(QStringLiteral("ROLLBACK"), nullptr);
    return false;
  }
  if (!execute(QStringLiteral("COMMIT"), errorMessage)) {
    execute(QStringLiteral("ROLLBACK"), nullptr);
    for (const QString& copiedFile : copiedFiles) {
      QFile::remove(copiedFile);
    }
    return false;
  }
  return true;
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

qint64 Database::changeRevision() const {
  if (!isOpen()) {
    return 0;
  }
  QSqlQuery query(m_database);
  if (!query.exec(
          QStringLiteral("SELECT value FROM metadata WHERE key='change_revision'")) ||
      !query.next()) {
    return 0;
  }
  return query.value(0).toLongLong();
}

bool Database::bumpChangeRevision(QString* errorMessage) const {
  QSqlQuery query(m_database);
  if (!query.exec(QStringLiteral(R"SQL(
        INSERT INTO metadata(key, value) VALUES ('change_revision', '1')
        ON CONFLICT(key) DO UPDATE SET value=CAST(value AS INTEGER)+1
      )SQL"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("advance change revision"));
    }
    return false;
  }
  return true;
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
    return ensureOutboxMoveSchema(errorMessage) &&
           ensureConflictUniquenessSchema(errorMessage) &&
           ensureReminderDeliverySchema(errorMessage) &&
           ensureSyncCoverageSchema(errorMessage) &&
           ensureReadPerformanceIndexes(errorMessage);
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
      QStringLiteral("PRAGMA user_version = 2"),
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
         ensureReadPerformanceIndexes(errorMessage);
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

bool Database::ensureReadPerformanceIndexes(QString* errorMessage) {
  // Schema 2 remains the development schema, so install these idempotently for
  // both new and existing databases. The leading range columns support views
  // that do not filter calendars; the original calendar-leading indexes remain
  // useful for a small calendar subset.
  const QStringList statements = {
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
  };
  for (const QString& statement : statements) {
    if (!execute(statement, errorMessage)) {
      return false;
    }
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
  return bumpChangeRevision(errorMessage);
}

bool Database::rebuildEventInstances(const Event& event, QString* errorMessage) {
  QSqlQuery remove(m_database);
  remove.prepare(QStringLiteral("DELETE FROM event_instances WHERE event_id=?"));
  remove.addBindValue(event.id);
  if (!remove.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(remove, QStringLiteral("replace event instances"));
    }
    return false;
  }
  if (event.deleted) {
    return true;
  }

  QList<Event> occurrences{event};
  if (!event.recurrenceRule.isEmpty() && event.recurrenceId.isEmpty()) {
    const QDateTime now = nowUtc();
    occurrences =
        RecurrenceExpander::expand({event}, now.addYears(-2), now.addYears(5), 10000)
            .occurrences;
  }
  for (const Event& occurrence : occurrences) {
    QString recurrenceId = occurrence.recurrenceId;
    if (recurrenceId.isEmpty()) {
      recurrenceId = occurrence.allDay ? occurrence.startDate.toString(Qt::ISODate)
                                       : isoUtc(occurrence.startUtc);
    }
    if (recurrenceId.isEmpty()) {
      continue;
    }
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(R"SQL(
      INSERT INTO event_instances
        (event_id,recurrence_id,start_utc,end_utc,start_date,end_date,all_day)
      VALUES (?,?,?,?,?,?,?)
      ON CONFLICT(event_id,recurrence_id) DO UPDATE SET
        start_utc=excluded.start_utc,end_utc=excluded.end_utc,
        start_date=excluded.start_date,end_date=excluded.end_date,
        all_day=excluded.all_day
    )SQL"));
    insert.addBindValue(event.id);
    insert.addBindValue(recurrenceId);
    insert.addBindValue(nonNull(isoUtc(occurrence.startUtc)));
    insert.addBindValue(nonNull(isoUtc(occurrence.endUtc)));
    insert.addBindValue(nonNull(occurrence.startDate.toString(Qt::ISODate)));
    insert.addBindValue(nonNull(occurrence.endDate.toString(Qt::ISODate)));
    insert.addBindValue(occurrence.allDay);
    if (!insert.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(insert, QStringLiteral("persist event instance"));
      }
      return false;
    }
  }
  return true;
}

bool Database::rebuildReminderJobs(const Event& event, QString* errorMessage) {
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
  if (event.deleted || event.reminders.isEmpty()) {
    return true;
  }
  QList<Event> occurrences{event};
  if (!event.recurrenceRule.isEmpty() && event.recurrenceId.isEmpty()) {
    const QDateTime now = nowUtc();
    occurrences =
        RecurrenceExpander::expand({event}, now.addDays(-2), now.addYears(2), 2000)
            .occurrences;
  }
  for (const Event& occurrence : occurrences) {
    const QDateTime eventStart =
        occurrence.allDay
            ? QDateTime(occurrence.startDate, QTime(0, 0), QTimeZone::systemTimeZone())
                  .toUTC()
            : occurrence.startUtc;
    if (!eventStart.isValid()) {
      continue;
    }
    for (qsizetype index = 0; index < event.reminders.size(); ++index) {
      const QJsonValue value = event.reminders.at(index);
      QDateTime fireAt;
      int minutesBefore = 0;
      if (value.isDouble()) {
        minutesBefore = value.toInt();
        fireAt = eventStart.addSecs(-minutesBefore * 60);
      } else if (value.isObject()) {
        const QJsonObject reminder = value.toObject();
        fireAt = dateTimeFromIso(reminder.value(QStringLiteral("at")).toString());
        if (!fireAt.isValid()) {
          minutesBefore =
              reminder.value(QStringLiteral("minutesBefore"))
                  .toInt(reminder.value(QStringLiteral("offsetMinutes"))
                             .toInt(reminder.value(QStringLiteral("minutes")).toInt()));
          fireAt = eventStart.addSecs(-minutesBefore * 60);
        }
      }
      if (!fireAt.isValid()) {
        continue;
      }
      const QString occurrenceId =
          occurrence.recurrenceId.isEmpty()
              ? (occurrence.allDay ? occurrence.startDate.toString(Qt::ISODate)
                                   : isoUtc(occurrence.startUtc))
              : occurrence.recurrenceId;
      const QString fingerprint = QStringLiteral("%1:%2:%3:%4")
                                      .arg(event.id, occurrenceId)
                                      .arg(index)
                                      .arg(isoUtc(fireAt));
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
      insert.addBindValue(nonNull(occurrenceId));
      insert.addBindValue(fingerprint);
      insert.addBindValue(isoUtc(fireAt));
      insert.addBindValue(event.localRevision);
      if (!insert.exec()) {
        if (errorMessage != nullptr) {
          *errorMessage = sqlError(insert, QStringLiteral("schedule event reminder"));
        }
        return false;
      }
    }
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
  if (accountId == QStringLiteral("local-account")) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("The device-only account cannot be removed");
    }
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("DELETE FROM accounts WHERE id = ?"));
  query.addBindValue(accountId);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("remove account"));
    }
    return false;
  }
  return query.numRowsAffected() > 0 && bumpChangeRevision(errorMessage);
}

bool Database::upsertCalendar(Calendar calendar, QString* errorMessage) {
  if (calendar.id.isEmpty()) {
    calendar.id = newUuid();
  }
  QSqlQuery existsQuery(m_database);
  existsQuery.prepare(QStringLiteral("SELECT 1 FROM calendars WHERE id=?"));
  existsQuery.addBindValue(calendar.id);
  if (!existsQuery.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(existsQuery, QStringLiteral("check calendar"));
    }
    return false;
  }
  const bool existed = existsQuery.next();
  QSqlQuery calendarSavepoint(m_database);
  if (!calendarSavepoint.exec(QStringLiteral("SAVEPOINT upsert_calendar"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(calendarSavepoint, QStringLiteral("start calendar upsert"));
    }
    return false;
  }
  const auto rollbackCalendar = [this]() {
    QSqlQuery rollbackQuery(m_database);
    rollbackQuery.exec(QStringLiteral("ROLLBACK TO upsert_calendar"));
    rollbackQuery.exec(QStringLiteral("RELEASE upsert_calendar"));
  };
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO calendars
      (id, account_id, remote_id, href, name, description, color, timezone,
       read_only, enabled, color_override, position, ignore_alerts, etag,
       sync_token, capabilities_json, last_sync_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
      color_override=excluded.color_override,
      position=excluded.position,
      ignore_alerts=excluded.ignore_alerts,
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
  query.addBindValue(nonNull(calendar.colorOverride));
  query.addBindValue(calendar.position);
  query.addBindValue(calendar.ignoreAlerts);
  query.addBindValue(nonNull(calendar.etag));
  query.addBindValue(nonNull(calendar.syncToken));
  query.addBindValue(compactJson(calendar.capabilities));
  query.addBindValue(isoUtc(calendar.lastSyncAt));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("upsert calendar"));
    }
    rollbackCalendar();
    return false;
  }
  if (!existed) {
    QSqlQuery membership(m_database);
    membership.prepare(QStringLiteral(R"SQL(
      INSERT OR IGNORE INTO calendar_set_members(set_id,calendar_id,position)
      SELECT 'all-calendars',?,COALESCE(MAX(position)+1,0)
      FROM calendar_set_members WHERE set_id='all-calendars'
    )SQL"));
    membership.addBindValue(calendar.id);
    if (!membership.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage =
            sqlError(membership, QStringLiteral("add calendar to default set"));
      }
      rollbackCalendar();
      return false;
    }
  }
  QSqlQuery release(m_database);
  if (!bumpChangeRevision(errorMessage) ||
      !release.exec(QStringLiteral("RELEASE upsert_calendar"))) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = m_database.lastError().text();
    }
    rollbackCalendar();
    return false;
  }
  return true;
}

bool Database::removeLocalCalendar(const QString& calendarId, QString* errorMessage) {
  if (calendarId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Calendar ID is required");
    }
    return false;
  }
  if (calendarId == QStringLiteral("local-default")) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("The default device calendar cannot be removed");
    }
    return false;
  }

  QSqlQuery ownerQuery(m_database);
  ownerQuery.prepare(QStringLiteral(R"SQL(
    SELECT a.provider
    FROM calendars c JOIN accounts a ON a.id=c.account_id
    WHERE c.id=?
  )SQL"));
  ownerQuery.addBindValue(calendarId);
  if (!ownerQuery.exec() || !ownerQuery.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = ownerQuery.lastError().isValid()
                          ? sqlError(ownerQuery, QStringLiteral("load calendar owner"))
                          : QStringLiteral("Calendar not found");
    }
    return false;
  }
  if (providerKindFromString(ownerQuery.value(0).toString()) != ProviderKind::Local) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Only device-only calendars can be removed");
    }
    return false;
  }

  return removeCalendarCache(calendarId, errorMessage);
}

bool Database::removeCalendarCache(const QString& calendarId, QString* errorMessage) {
  if (calendarId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Calendar ID is required");
    }
    return false;
  }
  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  QSqlQuery removeQuery(m_database);
  removeQuery.prepare(QStringLiteral("DELETE FROM calendars WHERE id=?"));
  removeQuery.addBindValue(calendarId);
  if (!removeQuery.exec() || removeQuery.numRowsAffected() != 1 ||
      !bumpChangeRevision(errorMessage) || !m_database.commit()) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = removeQuery.lastError().isValid()
                          ? sqlError(removeQuery, QStringLiteral("remove calendar"))
                          : m_database.lastError().text();
    }
    m_database.rollback();
    return false;
  }
  return true;
}

bool Database::updateCalendarPreferences(const QString& calendarId, const bool enabled,
                                         const QString& colorOverride,
                                         const int position, const bool ignoreAlerts,
                                         QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    UPDATE calendars SET enabled=?, color_override=?, position=?, ignore_alerts=?
    WHERE id=?
  )SQL"));
  query.addBindValue(enabled);
  query.addBindValue(nonNull(colorOverride));
  query.addBindValue(position);
  query.addBindValue(ignoreAlerts);
  query.addBindValue(calendarId);
  if (!query.exec() || query.numRowsAffected() == 0) {
    if (errorMessage != nullptr) {
      *errorMessage =
          query.lastError().isValid()
              ? sqlError(query, QStringLiteral("update calendar preferences"))
              : QStringLiteral("Calendar not found");
    }
    return false;
  }
  return bumpChangeRevision(errorMessage);
}

QList<Calendar> Database::calendars(const QString& accountId,
                                    QString* errorMessage) const {
  QList<Calendar> result;
  QSqlQuery query(m_database);
  QString sql = QStringLiteral(R"SQL(
    SELECT id, account_id, remote_id, href, name, description, color, timezone,
           read_only, enabled, color_override, position, ignore_alerts, etag,
           sync_token, capabilities_json, last_sync_at
    FROM calendars
  )SQL");
  if (!accountId.isEmpty()) {
    sql += QStringLiteral(" WHERE account_id = ?");
  }
  sql += QStringLiteral(" ORDER BY position, name COLLATE NOCASE, id");
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
    calendar.colorOverride = query.value(10).toString();
    calendar.position = query.value(11).toInt();
    calendar.ignoreAlerts = query.value(12).toBool();
    calendar.etag = query.value(13).toString();
    calendar.syncToken = query.value(14).toString();
    calendar.capabilities = parseObject(query.value(15).toString());
    calendar.lastSyncAt = dateTimeFromIso(query.value(16).toString());
    result.append(calendar);
  }
  return result;
}

Calendar Database::calendar(const QString& calendarId, QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT id, account_id, remote_id, href, name, description, color, timezone,
           read_only, enabled, color_override, position, ignore_alerts, etag,
           sync_token, capabilities_json, last_sync_at
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
  result.colorOverride = query.value(10).toString();
  result.position = query.value(11).toInt();
  result.ignoreAlerts = query.value(12).toBool();
  result.etag = query.value(13).toString();
  result.syncToken = query.value(14).toString();
  result.capabilities = parseObject(query.value(15).toString());
  result.lastSyncAt = dateTimeFromIso(query.value(16).toString());
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
       url, conference_url, start_utc, end_utc, start_date, end_date,
       start_timezone, end_timezone, all_day, time_kind, status, transparency,
       visibility, recurrence_rule, recurrence_id, sequence, organizer_json,
       attendees_json, reminders_json, raw_payload, raw_format, dirty, deleted,
       local_revision, sync_state, created_at, updated_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?)
    ON CONFLICT(id) DO UPDATE SET
      calendar_id=excluded.calendar_id,
      remote_id=excluded.remote_id,
      uid=excluded.uid,
      etag=excluded.etag,
      summary=excluded.summary,
      description=excluded.description,
      location=excluded.location,
      url=excluded.url,
      conference_url=excluded.conference_url,
      start_utc=excluded.start_utc,
      end_utc=excluded.end_utc,
      start_date=excluded.start_date,
      end_date=excluded.end_date,
      start_timezone=excluded.start_timezone,
      end_timezone=excluded.end_timezone,
      all_day=excluded.all_day,
      time_kind=excluded.time_kind,
      status=excluded.status,
      transparency=excluded.transparency,
      visibility=excluded.visibility,
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
      local_revision=excluded.local_revision,
      sync_state=excluded.sync_state,
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
  query.addBindValue(nonNull(event.url));
  query.addBindValue(nonNull(event.conferenceUrl));
  query.addBindValue(isoUtc(event.startUtc));
  query.addBindValue(isoUtc(event.endUtc));
  query.addBindValue(nonNull(event.startDate.toString(Qt::ISODate)));
  query.addBindValue(nonNull(event.endDate.toString(Qt::ISODate)));
  query.addBindValue(nonNull(event.startTimeZone));
  query.addBindValue(nonNull(event.endTimeZone));
  query.addBindValue(event.allDay);
  query.addBindValue(
      timeKindToString(event.allDay ? TimeKind::AllDay : event.timeKind));
  query.addBindValue(nonNull(event.status));
  query.addBindValue(nonNull(event.transparency));
  query.addBindValue(nonNull(event.visibility));
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
  query.addBindValue(event.localRevision);
  query.addBindValue(nonNull(event.syncState));
  query.addBindValue(isoUtc(event.createdAt));
  query.addBindValue(isoUtc(event.updatedAt));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("upsert event"));
    }
    return false;
  }
  return rebuildEventInstances(event, errorMessage) &&
         rebuildReminderJobs(event, errorMessage);
}

bool Database::activeMoveReservesRemote(const QString& calendarId,
                                        const QString& remoteId, bool* reserved,
                                        QString* errorMessage) const {
  if (reserved != nullptr) {
    *reserved = false;
  }
  if (calendarId.isEmpty() || remoteId.isEmpty()) {
    return true;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT payload_json FROM outbox
    WHERE operation='move'
      AND state IN ('pending','sending','retry_wait','blocked')
  )SQL"));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("inspect pending moves"));
    }
    return false;
  }
  while (query.next()) {
    const QJsonObject payload = parseObject(query.value(0).toString());
    const Event target = eventFromJson(payload);
    const QJsonObject metadata = payload.value(QStringLiteral("_move")).toObject();
    const Event source =
        eventFromJson(metadata.value(QStringLiteral("sourceEvent")).toObject());
    const QString targetRemoteId =
        metadata.value(QStringLiteral("targetRemoteId")).toString(target.remoteId);
    if ((source.calendarId == calendarId && source.remoteId == remoteId) ||
        (target.calendarId == calendarId && targetRemoteId == remoteId)) {
      if (reserved != nullptr) {
        *reserved = true;
      }
      return true;
    }
  }
  return true;
}

bool Database::upsertUnresolvedConflict(
    const QString& eventId, const qint64 mutationId, const QString& kind,
    const QString& localRevision, const QString& remoteRevision,
    const QJsonObject& localSnapshot, const QJsonObject& remoteSnapshot,
    const QString& remotePayload, const QDateTime& createdAt, QString* errorMessage) {
  QSqlQuery conflict(m_database);
  conflict.prepare(QStringLiteral(R"SQL(
    INSERT INTO conflicts
      (event_id,mutation_id,kind,local_revision,remote_revision,
       local_snapshot_json,remote_snapshot_json,remote_payload,state,created_at)
    VALUES (?,?,?,?,?,?,?,?,'unresolved',?)
    ON CONFLICT(event_id) WHERE state='unresolved' DO UPDATE SET
      mutation_id=COALESCE(excluded.mutation_id,conflicts.mutation_id),
      kind=excluded.kind,
      local_revision=excluded.local_revision,
      remote_revision=excluded.remote_revision,
      local_snapshot_json=excluded.local_snapshot_json,
      remote_snapshot_json=excluded.remote_snapshot_json,
      remote_payload=excluded.remote_payload,
      created_at=excluded.created_at
  )SQL"));
  conflict.addBindValue(eventId);
  if (mutationId > 0) {
    conflict.addBindValue(mutationId);
  } else {
    conflict.addBindValue(QVariant{});
  }
  conflict.addBindValue(kind);
  conflict.addBindValue(nonNull(localRevision));
  conflict.addBindValue(nonNull(remoteRevision));
  conflict.addBindValue(compactJson(localSnapshot));
  conflict.addBindValue(compactJson(remoteSnapshot));
  conflict.addBindValue(nonNull(remotePayload));
  conflict.addBindValue(isoUtc(createdAt));
  if (conflict.exec()) {
    return true;
  }
  if (errorMessage != nullptr) {
    *errorMessage = sqlError(conflict, QStringLiteral("record unresolved conflict"));
  }
  return false;
}

bool Database::applyRemoteEvent(Event event, QString* errorMessage, bool* conflicted) {
  if (conflicted != nullptr) {
    *conflicted = false;
  }
  bool reservedByMove = false;
  if (!activeMoveReservesRemote(event.calendarId, event.remoteId, &reservedByMove,
                                errorMessage)) {
    return false;
  }
  // Sync pulls run before mutation drain. Do not recreate the source row (or
  // manufacture a target conflict after an ambiguous accepted move) while the
  // durable Move operation owns this remote identity.
  if (reservedByMove) {
    return true;
  }
  const QDateTime now = nowUtc();
  qint64 priorLocalRevision = 0;
  const bool queryByProviderIdentity = !event.id.isEmpty() || !event.remoteId.isEmpty();
  QSqlQuery existingQuery(m_database);
  if (!event.id.isEmpty()) {
    existingQuery.prepare(QStringLiteral("SELECT * FROM events WHERE id=? LIMIT 1"));
    existingQuery.addBindValue(event.id);
  } else if (!event.remoteId.isEmpty()) {
    existingQuery.prepare(QStringLiteral(
        "SELECT * FROM events WHERE calendar_id=? AND remote_id=? LIMIT 1"));
    existingQuery.addBindValue(event.calendarId);
    existingQuery.addBindValue(event.remoteId);
  } else {
    existingQuery.prepare(QStringLiteral(
        "SELECT * FROM events WHERE calendar_id=? AND uid=? ORDER BY id"));
    existingQuery.addBindValue(event.calendarId);
    existingQuery.addBindValue(event.uid);
  }
  if (!existingQuery.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(existingQuery, QStringLiteral("match remote event"));
    }
    return false;
  }
  Event existing;
  while (existingQuery.next()) {
    const Event candidate = eventFromQuery(existingQuery);
    const bool sameIdentity =
        queryByProviderIdentity ||
        (event.recurrenceId.isEmpty()
             ? candidate.recurrenceId.isEmpty()
             : recurrenceIdentityEqual(event.recurrenceId, candidate.recurrenceId,
                                       candidate.allDay, candidate.timeKind,
                                       candidate.startTimeZone));
    if (sameIdentity) {
      existing = candidate;
      break;
    }
  }
  if (!existing.id.isEmpty()) {
    priorLocalRevision = existing.localRevision;
    event.id = existing.id;
    event.createdAt = existing.createdAt;
    if (existing.dirty) {
      if (!event.etag.isEmpty() && event.etag != existing.etag) {
        if (!upsertUnresolvedConflict(existing.id, 0, QStringLiteral("remote_changed"),
                                      existing.etag, event.etag,
                                      toStorageJson(existing), toStorageJson(event),
                                      event.rawPayload, now, errorMessage)) {
          return false;
        }
        if (!bumpChangeRevision(errorMessage)) {
          return false;
        }
        if (conflicted != nullptr) {
          *conflicted = true;
        }
      }
      return true;
    }
  } else if (event.id.isEmpty()) {
    event.id = newUuid();
  }
  if (event.uid.isEmpty()) {
    event.uid = newUuid();
  }
  if (!event.createdAt.isValid()) {
    event.createdAt = now;
  }
  event.updatedAt = now;
  event.dirty = false;
  event.localRevision = qMax(event.localRevision, priorLocalRevision + 1);
  event.syncState = QStringLiteral("clean");
  return upsertEventRecord(event, errorMessage) && bumpChangeRevision(errorMessage);
}

bool Database::removeRemoteEvent(const QString& calendarId, const QString& remoteId,
                                 const QString& remotePayload, QString* errorMessage,
                                 bool* conflicted) {
  return removeRemoteEventInternal(calendarId, remoteId, remotePayload, errorMessage,
                                   conflicted, true);
}

bool Database::removeRemoteEventInternal(const QString& calendarId,
                                         const QString& remoteId,
                                         const QString& remotePayload,
                                         QString* errorMessage, bool* conflicted,
                                         const bool manageTransaction) {
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
  if (manageTransaction && !m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }

  bool foundConflict = false;
  for (const Event& existing : existingEvents) {
    if (existing.dirty) {
      if (!upsertUnresolvedConflict(existing.id, 0, QStringLiteral("remote_deleted"),
                                    existing.etag, QStringLiteral("deleted"),
                                    toStorageJson(existing), {}, remotePayload,
                                    nowUtc(), errorMessage)) {
        if (manageTransaction) {
          m_database.rollback();
        }
        return false;
      }
      foundConflict = true;
      continue;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM events WHERE id=?"));
    query.addBindValue(existing.id);
    if (!query.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(query, QStringLiteral("remove remote event"));
      }
      if (manageTransaction) {
        m_database.rollback();
      }
      return false;
    }
  }
  if (!bumpChangeRevision(errorMessage)) {
    if (manageTransaction) {
      m_database.rollback();
    }
    return false;
  }
  if (manageTransaction && !m_database.commit()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    m_database.rollback();
    return false;
  }
  if (conflicted != nullptr) {
    *conflicted = foundConflict;
  }
  return true;
}

bool Database::applyRemoteSyncBatch(const Calendar& calendar,
                                    const QList<Event>& events,
                                    const QStringList& deletedRemoteIds,
                                    const QStringList& prunedRemoteIds,
                                    QString* errorMessage) {
  if (calendar.id.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("A remote sync batch needs a calendar");
    }
    return false;
  }
  QSqlQuery savepoint(m_database);
  if (!savepoint.exec(QStringLiteral("SAVEPOINT remote_sync_batch"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(savepoint, QStringLiteral("start remote sync batch"));
    }
    return false;
  }
  const auto fail = [this, errorMessage](const QString& fallback) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = fallback;
    }
    QSqlQuery rollback(m_database);
    rollback.exec(QStringLiteral("ROLLBACK TO remote_sync_batch"));
    rollback.exec(QStringLiteral("RELEASE remote_sync_batch"));
    return false;
  };

  for (const Event& event : events) {
    if (event.calendarId != calendar.id) {
      return fail(QStringLiteral("A staged event belongs to another calendar"));
    }
    bool conflicted = false;
    if (!applyRemoteEvent(event, errorMessage, &conflicted)) {
      return fail(QStringLiteral("Unable to apply a staged remote event"));
    }
  }

  QSet<QString> removals(deletedRemoteIds.cbegin(), deletedRemoteIds.cend());
  for (const QString& remoteId : prunedRemoteIds) {
    removals.insert(remoteId);
  }
  for (const QString& remoteId : std::as_const(removals)) {
    if (remoteId.isEmpty()) {
      continue;
    }
    bool conflicted = false;
    if (!removeRemoteEventInternal(calendar.id, remoteId, {}, errorMessage, &conflicted,
                                   false)) {
      return fail(QStringLiteral("Unable to apply a staged remote deletion"));
    }
  }
  if (!upsertCalendar(calendar, errorMessage)) {
    return fail(QStringLiteral("Unable to commit the staged calendar state"));
  }
  QSqlQuery release(m_database);
  if (!release.exec(QStringLiteral("RELEASE remote_sync_batch"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(release, QStringLiteral("commit remote sync batch"));
    }
    return fail(QStringLiteral("Unable to commit the remote sync batch"));
  }
  return true;
}

bool Database::applyRemoteRangeSyncBatch(
    const Calendar& calendar, const QList<Event>& events,
    const QStringList& deletedRemoteIds, const QStringList& prunedRemoteIds,
    const QDateTime& coverageStartUtc, const QDateTime& coverageEndUtc,
    QString* errorMessage, const bool replaceExistingCoverage) {
  if (!coverageStartUtc.isValid() || !coverageEndUtc.isValid() ||
      coverageStartUtc >= coverageEndUtc) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("A remote range sync needs valid UTC bounds");
    }
    return false;
  }
  QSqlQuery savepoint(m_database);
  if (!savepoint.exec(QStringLiteral("SAVEPOINT remote_range_sync_batch"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(savepoint, QStringLiteral("start remote range sync batch"));
    }
    return false;
  }
  const auto fail = [this, errorMessage](const QString& fallback) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = fallback;
    }
    QSqlQuery rollback(m_database);
    rollback.exec(QStringLiteral("ROLLBACK TO remote_range_sync_batch"));
    rollback.exec(QStringLiteral("RELEASE remote_range_sync_batch"));
    return false;
  };

  if (!applyRemoteSyncBatch(calendar, events, deletedRemoteIds, prunedRemoteIds,
                            errorMessage)) {
    return fail(QStringLiteral("Unable to apply the bounded remote sync batch"));
  }
  if (replaceExistingCoverage) {
    QSqlQuery clearCoverage(m_database);
    clearCoverage.prepare(
        QStringLiteral("DELETE FROM sync_coverage WHERE calendar_id=?"));
    clearCoverage.addBindValue(calendar.id);
    if (!clearCoverage.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage =
            sqlError(clearCoverage, QStringLiteral("replace stale sync coverage"));
      }
      return fail(QStringLiteral("Unable to replace stale sync coverage"));
    }
  }
  if (!mergeSyncCoverageInternal(calendar.id, coverageStartUtc, coverageEndUtc,
                                 errorMessage)) {
    return fail(QStringLiteral("Unable to record the completed sync range"));
  }
  QSqlQuery release(m_database);
  if (!release.exec(QStringLiteral("RELEASE remote_range_sync_batch"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(release, QStringLiteral("commit remote range sync batch"));
    }
    return fail(QStringLiteral("Unable to commit the remote range sync batch"));
  }
  return true;
}

bool Database::applyIcsFeedReplacement(const Calendar& calendar,
                                       const QList<Event>& replacementEvents,
                                       const QStringList& staleRemoteIds,
                                       const QString& etag, const QString& lastModified,
                                       const QDateTime& successAt,
                                       QString* errorMessage) {
  if (calendar.id.isEmpty() || calendar.accountId.isEmpty() || !successAt.isValid()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "An ICS feed replacement needs a calendar, account, and success time");
    }
    return false;
  }

  QSqlQuery savepoint(m_database);
  if (!savepoint.exec(QStringLiteral("SAVEPOINT ics_feed_replacement"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(savepoint, QStringLiteral("start ICS feed replacement"));
    }
    return false;
  }
  const auto fail = [this, errorMessage](const QString& fallback) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = fallback;
    }
    QSqlQuery rollback(m_database);
    rollback.exec(QStringLiteral("ROLLBACK TO ics_feed_replacement"));
    rollback.exec(QStringLiteral("RELEASE ics_feed_replacement"));
    return false;
  };

  if (!applyRemoteSyncBatch(calendar, replacementEvents, {}, staleRemoteIds,
                            errorMessage)) {
    return fail(QStringLiteral("Unable to stage the ICS feed cache"));
  }

  // A successful 200 response replaces its validators. Unlike a 304 status
  // refresh, an omitted validator must clear the old value rather than send a
  // stale conditional header on the next request.
  QSqlQuery subscription(m_database);
  subscription.prepare(QStringLiteral(R"SQL(
    UPDATE ics_subscriptions SET
      etag=?,last_modified=?,last_success_at=?,
      last_error_code='',last_error_message=''
    WHERE account_id=?
  )SQL"));
  subscription.addBindValue(nonNull(etag));
  subscription.addBindValue(nonNull(lastModified));
  subscription.addBindValue(isoUtc(successAt));
  subscription.addBindValue(calendar.accountId);
  if (!subscription.exec() || subscription.numRowsAffected() != 1) {
    if (errorMessage != nullptr) {
      *errorMessage =
          subscription.lastError().isValid()
              ? sqlError(subscription, QStringLiteral("update ICS feed status"))
              : QStringLiteral("ICS subscription not found");
    }
    return fail(QStringLiteral("Unable to stage the ICS subscription status"));
  }
  if (!bumpChangeRevision(errorMessage)) {
    return fail(QStringLiteral("Unable to publish the ICS feed revision"));
  }

  QSqlQuery release(m_database);
  if (!release.exec(QStringLiteral("RELEASE ics_feed_replacement"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(release, QStringLiteral("commit ICS feed replacement"));
    }
    return fail(QStringLiteral("Unable to commit the ICS feed replacement"));
  }
  return true;
}

bool Database::recordProviderConflict(const qint64 mutationId, const Event* remoteEvent,
                                      QString* errorMessage) {
  QSqlQuery mutation(m_database);
  mutation.prepare(QStringLiteral(R"SQL(
    SELECT event_id FROM outbox WHERE id=? AND state<>'done'
  )SQL"));
  mutation.addBindValue(mutationId);
  if (!mutation.exec() || !mutation.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = mutation.lastError().isValid()
                          ? sqlError(mutation, QStringLiteral("load conflict mutation"))
                          : QStringLiteral("Active mutation not found");
    }
    return false;
  }
  const QString eventId = mutation.value(0).toString();
  const Event local = event(eventId, errorMessage);
  if (local.id.isEmpty()) {
    return false;
  }
  Event remote;
  const bool remoteDeleted = remoteEvent == nullptr;
  if (!remoteDeleted) {
    remote = *remoteEvent;
    remote.id = eventId;
    if (remote.calendarId.isEmpty()) {
      remote.calendarId = local.calendarId;
    }
    if (remote.uid.isEmpty()) {
      remote.uid = local.uid;
    }
  }

  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  const QDateTime now = nowUtc();
  if (!upsertUnresolvedConflict(
          local.id, mutationId,
          remoteDeleted ? QStringLiteral("remote_deleted")
                        : QStringLiteral("remote_changed"),
          local.etag, remoteDeleted ? QStringLiteral("deleted") : remote.etag,
          toStorageJson(local), remoteDeleted ? QJsonObject{} : toStorageJson(remote),
          remoteDeleted ? QString{} : remote.rawPayload, now, errorMessage)) {
    m_database.rollback();
    return false;
  }
  QSqlQuery block(m_database);
  block.prepare(QStringLiteral(R"SQL(
    UPDATE outbox SET state='blocked',lease_until='',next_attempt_at='',
      error_code='provider_conflict',
      error_message='The remote event changed while this mutation was pending',
      updated_at=? WHERE id=? AND state<>'done'
  )SQL"));
  block.addBindValue(isoUtc(now));
  block.addBindValue(mutationId);
  QSqlQuery mark(m_database);
  mark.prepare(QStringLiteral(
      "UPDATE events SET sync_state='conflict',dirty=1,updated_at=? WHERE id=?"));
  mark.addBindValue(isoUtc(now));
  mark.addBindValue(eventId);
  if (!block.exec() || block.numRowsAffected() == 0 || !mark.exec() ||
      !bumpChangeRevision(errorMessage) || !m_database.commit()) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = block.lastError().isValid()
                          ? sqlError(block, QStringLiteral("block conflict mutation"))
                      : mark.lastError().isValid()
                          ? sqlError(mark, QStringLiteral("mark conflicted event"))
                          : m_database.lastError().text();
    }
    m_database.rollback();
    return false;
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
  return bumpChangeRevision(errorMessage);
}

bool Database::removeCleanEventsExcept(const QString& calendarId,
                                       const QStringList& retainedRemoteIds,
                                       QString* errorMessage) {
  QString sql = QStringLiteral("DELETE FROM events WHERE calendar_id=? AND dirty=0");
  if (!retainedRemoteIds.isEmpty()) {
    QStringList placeholders;
    placeholders.fill(QStringLiteral("?"), retainedRemoteIds.size());
    sql += QStringLiteral(" AND remote_id NOT IN (%1)")
               .arg(placeholders.join(QLatin1Char(',')));
  }
  QSqlQuery query(m_database);
  query.prepare(sql);
  query.addBindValue(calendarId);
  for (const QString& remoteId : retainedRemoteIds) {
    query.addBindValue(remoteId);
  }
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("prune staged full sync"));
    }
    return false;
  }
  return query.numRowsAffected() == 0 || bumpChangeRevision(errorMessage);
}

bool Database::saveLocalEvent(Event* event, const OutboxOperation operation,
                              QString* errorMessage, const QString& clientMutationId,
                              const QString& recurrenceScope,
                              const QString& sendUpdates,
                              const qint64 expectedLocalRevision,
                              const QString& dependencyMutationId) {
  if (operation == OutboxOperation::Move) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("Calendar moves must use the atomic moveLocalEvent path");
    }
    return false;
  }
  if (event == nullptr || event->calendarId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Event and calendarId are required");
    }
    return false;
  }
  const QStringList validScopes = {QStringLiteral("occurrence"),
                                   QStringLiteral("future"), QStringLiteral("series")};
  const QStringList validNotificationPolicies = {
      QStringLiteral("none"), QStringLiteral("all"), QStringLiteral("externalOnly")};
  if (!validScopes.contains(recurrenceScope) ||
      !validNotificationPolicies.contains(sendUpdates)) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "A valid recurrence scope and guest notification policy are required");
    }
    return false;
  }

  QSqlQuery calendarQuery(m_database);
  calendarQuery.prepare(QStringLiteral(R"SQL(
    SELECT c.account_id, c.read_only, a.provider
    FROM calendars c JOIN accounts a ON a.id=c.account_id WHERE c.id=?
  )SQL"));
  calendarQuery.addBindValue(event->calendarId);
  if (!calendarQuery.exec() || !calendarQuery.next()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          calendarQuery.lastError().isValid()
              ? sqlError(calendarQuery, QStringLiteral("get event calendar"))
              : QStringLiteral("Calendar does not exist");
    }
    return false;
  }
  const QString accountId = calendarQuery.value(0).toString();
  const bool readOnly = calendarQuery.value(1).toBool();
  const ProviderKind provider =
      providerKindFromString(calendarQuery.value(2).toString());
  if (readOnly || provider == ProviderKind::Ics) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("This calendar is read-only");
    }
    return false;
  }

  const QDateTime now = nowUtc();
  Event previousEvent;
  if (!event->id.isEmpty()) {
    previousEvent = this->event(event->id);
  }
  const qint64 currentLocalRevision =
      previousEvent.id.isEmpty() ? 0 : previousEvent.localRevision;
  if (expectedLocalRevision >= 0 && expectedLocalRevision != currentLocalRevision) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Event changed since it was opened");
    }
    return false;
  }
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
  event->localRevision = currentLocalRevision + 1;
  event->timeKind = event->allDay ? TimeKind::AllDay : event->timeKind;
  event->dirty = provider != ProviderKind::Local || !dependencyMutationId.isEmpty();
  event->syncState = event->dirty ? QStringLiteral("pending") : QStringLiteral("clean");

  QSqlQuery savepoint(m_database);
  if (!savepoint.exec(QStringLiteral("SAVEPOINT save_local_event"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(savepoint, QStringLiteral("start event savepoint"));
    }
    return false;
  }
  const auto rollbackSavepoint = [this]() {
    QSqlQuery rollback(m_database);
    rollback.exec(QStringLiteral("ROLLBACK TO save_local_event"));
    rollback.exec(QStringLiteral("RELEASE save_local_event"));
  };
  const auto releaseSavepoint = [this, errorMessage]() {
    QSqlQuery release(m_database);
    if (release.exec(QStringLiteral("RELEASE save_local_event"))) {
      return true;
    }
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(release, QStringLiteral("commit event savepoint"));
    }
    return false;
  };
  if (!upsertEventRecord(*event, errorMessage)) {
    rollbackSavepoint();
    return false;
  }
  if (provider == ProviderKind::Local && dependencyMutationId.isEmpty() &&
      operation != OutboxOperation::Remove) {
    if (!bumpChangeRevision(errorMessage) || !releaseSavepoint()) {
      if (errorMessage != nullptr && errorMessage->isEmpty()) {
        *errorMessage = m_database.lastError().text();
      }
      rollbackSavepoint();
      return false;
    }
    return true;
  }

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
    rollbackSavepoint();
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
      if (!deleteOutbox.exec() || !deleteEvent.exec() ||
          !bumpChangeRevision(errorMessage) || !releaseSavepoint()) {
        if (errorMessage != nullptr) {
          *errorMessage = QStringLiteral("Could not cancel unsynced event");
        }
        rollbackSavepoint();
        return false;
      }
      return true;
    }
    const OutboxOperation effectiveOperation =
        previous == OutboxOperation::Create ? OutboxOperation::Create : operation;
    QSqlQuery updateQuery(m_database);
    updateQuery.prepare(QStringLiteral(R"SQL(
      UPDATE outbox SET operation=?, state='pending', expected_revision=?,
        recurrence_scope=?, send_updates=?, payload_json=?, attempts=0,
        next_attempt_at=?, not_before=?, lease_until='', error_code='',
        error_message='', updated_at=? WHERE id=?
    )SQL"));
    updateQuery.addBindValue(outboxOperationToString(effectiveOperation));
    updateQuery.addBindValue(nonNull(event->etag));
    updateQuery.addBindValue(recurrenceScope);
    updateQuery.addBindValue(sendUpdates);
    updateQuery.addBindValue(compactJson(toStorageJson(*event)));
    updateQuery.addBindValue(isoUtc(now));
    updateQuery.addBindValue(
        isoUtc(operation == OutboxOperation::Remove && dependencyMutationId.isEmpty()
                   ? now.addSecs(10)
                   : now));
    updateQuery.addBindValue(isoUtc(now));
    updateQuery.addBindValue(pendingId);
    if (!updateQuery.exec() || !bumpChangeRevision(errorMessage) ||
        !releaseSavepoint()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(updateQuery, QStringLiteral("coalesce event"));
      }
      rollbackSavepoint();
      return false;
    }
    return true;
  }

  QString dependencyId = dependencyMutationId;
  if (dependencyId.isEmpty()) {
    QSqlQuery dependencyQuery(m_database);
    dependencyQuery.prepare(QStringLiteral(R"SQL(
      SELECT id FROM outbox WHERE event_id=?
        AND state IN ('pending','sending','retry_wait','blocked')
      ORDER BY id DESC LIMIT 1
    )SQL"));
    dependencyQuery.addBindValue(event->id);
    if (!dependencyQuery.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage =
            sqlError(dependencyQuery, QStringLiteral("find mutation dependency"));
      }
      rollbackSavepoint();
      return false;
    }
    if (dependencyQuery.next()) {
      dependencyId = dependencyQuery.value(0).toString();
    }
  }

  const QString idempotencyKey =
      clientMutationId.isEmpty() ? newUuid() : clientMutationId;
  const QDateTime notBefore =
      operation == OutboxOperation::Remove && dependencyMutationId.isEmpty()
          ? now.addSecs(10)
          : now;

  QSqlQuery outboxQuery(m_database);
  outboxQuery.prepare(QStringLiteral(R"SQL(
    INSERT INTO outbox
      (account_id, calendar_id, event_id, operation, state, idempotency_key,
       dependency_id, expected_revision, recurrence_scope, send_updates,
       payload_json, attempts, next_attempt_at, not_before, created_at, updated_at)
    VALUES (?, ?, ?, ?, 'pending', ?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?)
  )SQL"));
  outboxQuery.addBindValue(accountId);
  outboxQuery.addBindValue(event->calendarId);
  outboxQuery.addBindValue(event->id);
  outboxQuery.addBindValue(outboxOperationToString(operation));
  outboxQuery.addBindValue(idempotencyKey);
  outboxQuery.addBindValue(nonNull(dependencyId));
  outboxQuery.addBindValue(nonNull(event->etag));
  outboxQuery.addBindValue(recurrenceScope);
  outboxQuery.addBindValue(sendUpdates);
  outboxQuery.addBindValue(compactJson(toStorageJson(*event)));
  outboxQuery.addBindValue(isoUtc(now));
  outboxQuery.addBindValue(isoUtc(notBefore));
  outboxQuery.addBindValue(isoUtc(now));
  outboxQuery.addBindValue(isoUtc(now));
  if (!outboxQuery.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(outboxQuery, QStringLiteral("queue event"));
    }
    rollbackSavepoint();
    return false;
  }
  if (!bumpChangeRevision(errorMessage) || !releaseSavepoint()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    rollbackSavepoint();
    return false;
  }
  return true;
}

bool Database::moveLocalEvent(Event* event, QString* errorMessage,
                              const QString& clientMutationId,
                              const QString& recurrenceScope,
                              const QString& sendUpdates,
                              const qint64 expectedLocalRevision) {
  if (event == nullptr || event->id.isEmpty() || event->calendarId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "An existing event and target calendar are required for a move");
    }
    return false;
  }
  const QStringList validScopes = {QStringLiteral("occurrence"),
                                   QStringLiteral("future"), QStringLiteral("series")};
  const QStringList validNotificationPolicies = {
      QStringLiteral("none"), QStringLiteral("all"), QStringLiteral("externalOnly")};
  if (!validScopes.contains(recurrenceScope) ||
      !validNotificationPolicies.contains(sendUpdates)) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "A valid recurrence scope and guest notification policy are required");
    }
    return false;
  }

  if (!clientMutationId.isEmpty()) {
    QSqlQuery duplicate(m_database);
    duplicate.prepare(QStringLiteral(R"SQL(
      SELECT event_id, operation FROM outbox WHERE idempotency_key=? LIMIT 1
    )SQL"));
    duplicate.addBindValue(clientMutationId);
    if (!duplicate.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(duplicate, QStringLiteral("check move identity"));
      }
      return false;
    }
    if (duplicate.next()) {
      if (duplicate.value(0).toString() != event->id ||
          outboxOperationFromString(duplicate.value(1).toString()) !=
              OutboxOperation::Move) {
        if (errorMessage != nullptr) {
          *errorMessage =
              QStringLiteral("The client mutation identifier is already in use");
        }
        return false;
      }
      const Event existing = this->event(event->id, errorMessage);
      if (existing.id.isEmpty()) {
        return false;
      }
      *event = existing;
      return true;
    }
  }

  const Event source = this->event(event->id, errorMessage);
  if (source.id.isEmpty()) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = QStringLiteral("Event does not exist");
    }
    return false;
  }
  const Calendar sourceCalendar = calendar(source.calendarId, errorMessage);
  const Calendar targetCalendar = calendar(event->calendarId, errorMessage);
  if (sourceCalendar.id.isEmpty() || targetCalendar.id.isEmpty()) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = QStringLiteral("Source or target calendar does not exist");
    }
    return false;
  }
  if (sourceCalendar.accountId != targetCalendar.accountId) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("A first-class move requires calendars on the same account");
    }
    return false;
  }
  if (sourceCalendar.readOnly || targetCalendar.readOnly) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Source and target calendars must be writable");
    }
    return false;
  }
  const Account owner = account(sourceCalendar.accountId, errorMessage);
  if (owner.id.isEmpty() || owner.provider == ProviderKind::Ics) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = QStringLiteral("The calendar provider cannot move events");
    }
    return false;
  }
  if (source.calendarId == targetCalendar.id) {
    *event = source;
    return true;
  }
  if (expectedLocalRevision >= 0 && expectedLocalRevision != source.localRevision) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Event changed since it was opened");
    }
    return false;
  }

  QSqlQuery collision(m_database);
  collision.prepare(QStringLiteral(R"SQL(
    SELECT * FROM events
    WHERE calendar_id=? AND uid=? AND id<>?
  )SQL"));
  collision.addBindValue(targetCalendar.id);
  collision.addBindValue(source.uid);
  collision.addBindValue(source.id);
  bool hasCollision = false;
  if (!collision.exec()) {
    hasCollision = true;
  } else {
    while (collision.next()) {
      const Event candidate = eventFromQuery(collision);
      if (source.recurrenceId.isEmpty()
              ? candidate.recurrenceId.isEmpty()
              : recurrenceIdentityEqual(source.recurrenceId, candidate.recurrenceId,
                                        candidate.allDay, candidate.timeKind,
                                        candidate.startTimeZone)) {
        hasCollision = true;
        break;
      }
    }
  }
  if (hasCollision) {
    if (errorMessage != nullptr) {
      *errorMessage =
          collision.lastError().isValid()
              ? sqlError(collision, QStringLiteral("check move target"))
              : QStringLiteral("The target calendar already contains this event");
    }
    return false;
  }

  const QDateTime now = nowUtc();
  event->id = source.id;
  event->uid = source.uid;
  event->calendarId = targetCalendar.id;
  event->remoteId = source.remoteId;
  event->etag = source.etag;
  event->rawPayload = source.rawPayload;
  event->rawFormat = source.rawFormat;
  event->createdAt = source.createdAt;
  event->updatedAt = now;
  event->localRevision = source.localRevision + 1;
  event->timeKind = event->allDay ? TimeKind::AllDay : event->timeKind;
  event->deleted = false;
  event->dirty = owner.provider != ProviderKind::Local;
  event->syncState = event->dirty ? QStringLiteral("pending") : QStringLiteral("clean");

  QSqlQuery savepoint(m_database);
  if (!savepoint.exec(QStringLiteral("SAVEPOINT move_local_event"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(savepoint, QStringLiteral("start event move"));
    }
    return false;
  }
  const auto rollback = [this]() {
    QSqlQuery query(m_database);
    query.exec(QStringLiteral("ROLLBACK TO move_local_event"));
    query.exec(QStringLiteral("RELEASE move_local_event"));
  };
  const auto release = [this, errorMessage]() {
    QSqlQuery query(m_database);
    if (query.exec(QStringLiteral("RELEASE move_local_event"))) {
      return true;
    }
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("commit event move"));
    }
    return false;
  };
  if (!upsertEventRecord(*event, errorMessage)) {
    rollback();
    return false;
  }

  if (owner.provider != ProviderKind::Local) {
    QString dependencyId;
    QSqlQuery dependency(m_database);
    dependency.prepare(QStringLiteral(R"SQL(
      SELECT id FROM outbox WHERE event_id=?
        AND state IN ('pending','sending','retry_wait','blocked')
      ORDER BY id DESC LIMIT 1
    )SQL"));
    dependency.addBindValue(source.id);
    if (!dependency.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(dependency, QStringLiteral("find move dependency"));
      }
      rollback();
      return false;
    }
    if (dependency.next()) {
      dependencyId = dependency.value(0).toString();
    }

    QJsonObject payload = toStorageJson(*event);
    QJsonObject moveMetadata{
        {QStringLiteral("sourceEvent"), toStorageJson(source)},
        {QStringLiteral("sourceCalendarId"), source.calendarId},
        {QStringLiteral("targetCalendarId"), targetCalendar.id},
    };
    if (owner.provider == ProviderKind::Google && !source.remoteId.isEmpty()) {
      // Google preserves the event id across its native move endpoint.
      moveMetadata.insert(QStringLiteral("targetRemoteId"), source.remoteId);
    }
    payload.insert(QStringLiteral("_move"), moveMetadata);

    QSqlQuery outbox(m_database);
    outbox.prepare(QStringLiteral(R"SQL(
      INSERT INTO outbox
        (account_id, calendar_id, event_id, operation, state, idempotency_key,
         dependency_id, expected_revision, recurrence_scope, send_updates,
         payload_json, attempts, next_attempt_at, not_before, created_at, updated_at)
      VALUES (?, ?, ?, 'move', 'pending', ?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?)
    )SQL"));
    outbox.addBindValue(owner.id);
    outbox.addBindValue(targetCalendar.id);
    outbox.addBindValue(source.id);
    outbox.addBindValue(clientMutationId.isEmpty() ? newUuid() : clientMutationId);
    outbox.addBindValue(nonNull(dependencyId));
    outbox.addBindValue(nonNull(source.etag));
    outbox.addBindValue(recurrenceScope);
    outbox.addBindValue(sendUpdates);
    outbox.addBindValue(compactJson(payload));
    outbox.addBindValue(isoUtc(now));
    outbox.addBindValue(isoUtc(now));
    outbox.addBindValue(isoUtc(now));
    outbox.addBindValue(isoUtc(now));
    if (!outbox.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(outbox, QStringLiteral("queue event move"));
      }
      rollback();
      return false;
    }
  }

  if (!bumpChangeRevision(errorMessage) || !release()) {
    rollback();
    return false;
  }
  return true;
}

bool Database::moveEventAcrossAccounts(const Event& source, Event* destination,
                                       QString* errorMessage,
                                       const QString& clientMutationId,
                                       const QString& recurrenceScope,
                                       const QString& sendUpdates,
                                       const qint64 expectedLocalRevision) {
  if (source.id.isEmpty() || destination == nullptr ||
      destination->calendarId.isEmpty() || clientMutationId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "A source, destination, and client mutation identifier are required");
    }
    return false;
  }

  const QString destinationMutationId =
      clientMutationId + QStringLiteral(":destination");
  const QString sourceMutationId = clientMutationId + QStringLiteral(":source");
  QSqlQuery duplicate(m_database);
  duplicate.prepare(
      QStringLiteral("SELECT event_id FROM outbox WHERE idempotency_key=? LIMIT 1"));
  duplicate.addBindValue(destinationMutationId);
  if (!duplicate.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(duplicate, QStringLiteral("check cross-account move"));
    }
    return false;
  }
  if (duplicate.next()) {
    const Event existing = event(duplicate.value(0).toString(), errorMessage);
    if (existing.id.isEmpty()) {
      if (errorMessage != nullptr && errorMessage->isEmpty()) {
        *errorMessage = QStringLiteral("The existing move destination is unavailable");
      }
      return false;
    }
    *destination = existing;
    return true;
  }

  Event currentSource = event(source.id, errorMessage);
  const bool generatedOccurrence =
      currentSource.id.isEmpty() && !source.recurrenceId.isEmpty();
  Event revisionSource = currentSource;
  if (generatedOccurrence) {
    if (errorMessage != nullptr) {
      errorMessage->clear();
    }
    revisionSource = eventByUid(source.calendarId, source.uid, {}, errorMessage);
    currentSource = source;
  }
  const Calendar sourceCalendar = calendar(currentSource.calendarId, errorMessage);
  const Calendar targetCalendar = calendar(destination->calendarId, errorMessage);
  if (currentSource.id.isEmpty() || revisionSource.id.isEmpty() ||
      sourceCalendar.id.isEmpty() || targetCalendar.id.isEmpty()) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = QStringLiteral("The move source or destination does not exist");
    }
    return false;
  }
  if (sourceCalendar.accountId == targetCalendar.accountId &&
      currentSource.recurrenceId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "A copied same-account move is only valid for a detached occurrence");
    }
    return false;
  }
  if (sourceCalendar.readOnly || targetCalendar.readOnly) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Source and target calendars must be writable");
    }
    return false;
  }
  if (expectedLocalRevision >= 0 &&
      expectedLocalRevision != revisionSource.localRevision) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Event changed since it was opened");
    }
    return false;
  }

  // A pre-existing mutation cannot safely be folded into a two-resource move:
  // it could otherwise bypass the destination-create dependency. Let it drain
  // or be resolved before starting the move.
  QSqlQuery activeSource(m_database);
  activeSource.prepare(QStringLiteral(R"SQL(
    SELECT 1 FROM outbox WHERE event_id=?
      AND state IN ('pending','sending','retry_wait','blocked') LIMIT 1
  )SQL"));
  activeSource.addBindValue(revisionSource.id);
  if (!activeSource.exec() || activeSource.next()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          activeSource.lastError().isValid()
              ? sqlError(activeSource, QStringLiteral("check move source operations"))
              : QStringLiteral("Finish or resolve pending event changes before moving");
    }
    return false;
  }

  QSqlQuery savepoint(m_database);
  if (!savepoint.exec(QStringLiteral("SAVEPOINT move_across_accounts"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(savepoint, QStringLiteral("start cross-account move"));
    }
    return false;
  }
  const auto rollback = [this]() {
    QSqlQuery query(m_database);
    query.exec(QStringLiteral("ROLLBACK TO move_across_accounts"));
    query.exec(QStringLiteral("RELEASE move_across_accounts"));
  };

  Event created = *destination;
  created.id.clear();
  created.remoteId.clear();
  created.etag.clear();
  created.rawPayload.clear();
  created.rawFormat.clear();
  created.createdAt = {};
  created.updatedAt = {};
  created.localRevision = 0;
  created.deleted = false;
  // A local destination is still queued through LocalProvider so the durable
  // destination operation exists for idempotency and dependency ordering.
  const Account targetAccount = account(targetCalendar.accountId, errorMessage);
  const QString destinationDependency =
      targetAccount.provider == ProviderKind::Local ? QStringLiteral("0") : QString();
  if (!saveLocalEvent(&created, OutboxOperation::Create, errorMessage,
                      destinationMutationId, recurrenceScope, sendUpdates, 0,
                      destinationDependency)) {
    rollback();
    return false;
  }

  QSqlQuery destinationOperation(m_database);
  destinationOperation.prepare(
      QStringLiteral("SELECT id FROM outbox WHERE idempotency_key=? LIMIT 1"));
  destinationOperation.addBindValue(destinationMutationId);
  if (!destinationOperation.exec() || !destinationOperation.next()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          destinationOperation.lastError().isValid()
              ? sqlError(destinationOperation,
                         QStringLiteral("load move destination operation"))
              : QStringLiteral("The destination create was not durably queued");
    }
    rollback();
    return false;
  }
  const QString dependencyId = destinationOperation.value(0).toString();

  Event removal = currentSource;
  removal.deleted = true;
  if (!saveLocalEvent(&removal, OutboxOperation::Remove, errorMessage, sourceMutationId,
                      recurrenceScope, sendUpdates,
                      generatedOccurrence ? 0 : currentSource.localRevision,
                      dependencyId)) {
    rollback();
    return false;
  }

  QSqlQuery release(m_database);
  if (!release.exec(QStringLiteral("RELEASE move_across_accounts"))) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(release, QStringLiteral("commit cross-account move"));
    }
    rollback();
    return false;
  }
  *destination = created;
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
  result.url = query.value(QStringLiteral("url")).toString();
  result.conferenceUrl = query.value(QStringLiteral("conference_url")).toString();
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
  result.timeKind =
      timeKindFromString(query.value(QStringLiteral("time_kind")).toString());
  result.status = query.value(QStringLiteral("status")).toString();
  result.transparency = query.value(QStringLiteral("transparency")).toString();
  result.visibility = query.value(QStringLiteral("visibility")).toString();
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
  result.localRevision = query.value(QStringLiteral("local_revision")).toLongLong();
  result.syncState = query.value(QStringLiteral("sync_state")).toString();
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

Event Database::eventByUid(const QString& calendarId, const QString& uid,
                           const QString& recurrenceId, QString* errorMessage) const {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT * FROM events
    WHERE calendar_id=? AND uid=? AND deleted=0
    ORDER BY CASE WHEN recurrence_id='' THEN 0 ELSE 1 END, recurrence_id, id
  )SQL"));
  query.addBindValue(calendarId);
  query.addBindValue(uid);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("get event by UID"));
    }
    return {};
  }
  while (query.next()) {
    const Event candidate = eventFromQuery(query);
    if (recurrenceId.isEmpty()) {
      if (candidate.recurrenceId.isEmpty()) {
        return candidate;
      }
      continue;
    }
    if (recurrenceIdentityEqual(recurrenceId, candidate.recurrenceId, candidate.allDay,
                                candidate.timeKind, candidate.startTimeZone)) {
      return candidate;
    }
  }
  return {};
}

QList<Event> Database::eventsByUid(const QString& calendarId, const QString& uid,
                                   QString* errorMessage) const {
  QList<Event> result;
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT * FROM events
    WHERE calendar_id=? AND uid=? AND (deleted=0 OR recurrence_id<>'')
    ORDER BY CASE WHEN recurrence_id='' THEN 0 ELSE 1 END, recurrence_id, id
  )SQL"));
  query.addBindValue(calendarId);
  query.addBindValue(uid);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list events by UID"));
    }
    return result;
  }
  while (query.next()) {
    result.append(eventFromQuery(query));
  }
  return result;
}

QList<Event> Database::eventsForCalendars(const QStringList& calendarIds,
                                          QString* errorMessage) const {
  QList<Event> result;
  if (calendarIds.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("At least one calendar is required");
    }
    return result;
  }
  QStringList placeholders;
  placeholders.fill(QStringLiteral("?"), calendarIds.size());
  QSqlQuery query(m_database);
  query.prepare(
      QStringLiteral("SELECT * FROM events WHERE deleted=0 AND calendar_id IN (%1) "
                     "ORDER BY all_day DESC, "
                     "COALESCE(NULLIF(start_utc,''),start_date),id")
          .arg(placeholders.join(QLatin1Char(','))));
  for (const QString& calendarId : calendarIds) {
    query.addBindValue(calendarId);
  }
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("list calendar events"));
    }
    return result;
  }
  while (query.next()) {
    result.append(eventFromQuery(query));
  }
  return result;
}

QList<Event> Database::eventsBetween(const QDateTime& startUtc, const QDateTime& endUtc,
                                     const QStringList& calendarIds,
                                     QString* errorMessage) const {
  return eventsBetweenInternal(startUtc, endUtc, calendarIds, false, errorMessage);
}

QList<Event> Database::invitationEventsBetween(const QDateTime& startUtc,
                                               const QDateTime& endUtc,
                                               QString* errorMessage) const {
  return eventsBetweenInternal(startUtc, endUtc, {}, true, errorMessage);
}

QList<Event> Database::eventsBetweenInternal(const QDateTime& startUtc,
                                             const QDateTime& endUtc,
                                             const QStringList& calendarIds,
                                             const bool invitationsOnly,
                                             QString* errorMessage) const {
  QList<Event> candidates;
  if (!startUtc.isValid() || !endUtc.isValid() || startUtc >= endUtc) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("A valid bounded time range is required");
    }
    return candidates;
  }
  QString calendarClause;
  if (!calendarIds.isEmpty()) {
    QStringList placeholders;
    placeholders.fill(QStringLiteral("?"), calendarIds.size());
    calendarClause = QStringLiteral(" AND e.calendar_id IN (%1)")
                         .arg(placeholders.join(QLatin1Char(',')));
  }
  const QString invitationPredicate = QStringLiteral(
      "e.organizer_json NOT IN ('','{}') AND "
      "e.attendees_json NOT IN ('','[]')");
  const QString invitedOnly =
      invitationsOnly ? QStringLiteral(" AND ") + invitationPredicate : QString();
  QString recurringInvitationFilter;
  if (invitationsOnly) {
    recurringInvitationFilter = QStringLiteral(R"SQL(
              AND (e.recurrence_id<>'' OR
              (e.recurrence_rule<>'' AND (
                %1 OR EXISTS (
                  SELECT 1 FROM events AS invite_exception
                  WHERE invite_exception.deleted=0
                    AND invite_exception.calendar_id=e.calendar_id
                    AND invite_exception.uid=e.uid
                    AND invite_exception.recurrence_id<>''
                    AND invite_exception.organizer_json NOT IN ('','{}')
                    AND invite_exception.attendees_json NOT IN ('','[]')
                )
              )))
            )SQL")
                                    .arg(invitationPredicate);
  }

  // Splitting timed, all-day, and recurrence candidates into UNION branches
  // lets SQLite use a bounded partial index for each kind. The prior top-level
  // OR forced a full events-table scan even for a seven-day view.
  const QString sql = QStringLiteral(R"SQL(
    SELECT * FROM (
      SELECT e.* FROM events AS e
      WHERE e.deleted=0 AND e.all_day=0
        AND e.recurrence_rule='' AND e.recurrence_id=''
        AND e.end_utc>? AND e.start_utc<?%1%2
      UNION ALL
      SELECT e.* FROM events AS e
      WHERE e.deleted=0 AND e.all_day=1
        AND e.recurrence_rule='' AND e.recurrence_id=''
        AND e.end_date>? AND e.start_date<?%1%2
      UNION ALL
      SELECT e.* FROM events AS e
      WHERE (e.deleted=0 OR e.recurrence_id<>'')
        AND (e.recurrence_rule<>'' OR e.recurrence_id<>'')%3%2
    ) AS bounded_events
    ORDER BY all_day DESC, COALESCE(NULLIF(start_utc,''), start_date), id
  )SQL")
                          .arg(invitedOnly, calendarClause, recurringInvitationFilter);

  QSqlQuery query(m_database);
  query.prepare(sql);
  query.addBindValue(isoUtc(startUtc));
  query.addBindValue(isoUtc(endUtc));
  for (const QString& calendarId : calendarIds) {
    query.addBindValue(calendarId);
  }
  query.addBindValue(startUtc.date().toString(Qt::ISODate));
  const QDate allDayEnd =
      endUtc.time() == QTime(0, 0) ? endUtc.date() : endUtc.date().addDays(1);
  query.addBindValue(allDayEnd.toString(Qt::ISODate));
  for (const QString& calendarId : calendarIds) {
    query.addBindValue(calendarId);
  }
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
  const QList<Event> expanded =
      RecurrenceExpander::expand(candidates, startUtc, endUtc).occurrences;
  if (!invitationsOnly) {
    return expanded;
  }
  QList<Event> invitations;
  invitations.reserve(expanded.size());
  for (const Event& event : expanded) {
    if (!event.organizer.isEmpty() && !event.attendees.isEmpty()) {
      invitations.append(event);
    }
  }
  return invitations;
}

QHash<QString, bool> Database::invitationSeenStates(const QStringList& eventIds,
                                                    QString* errorMessage) const {
  QHash<QString, bool> result;
  QStringList uniqueIds;
  uniqueIds.reserve(eventIds.size());
  QSet<QString> visited;
  for (const QString& eventId : eventIds) {
    if (!eventId.isEmpty() && !visited.contains(eventId)) {
      visited.insert(eventId);
      uniqueIds.append(eventId);
      result.insert(eventId, false);
    }
  }

  constexpr qsizetype kBatchSize = 400;
  const QString prefix = QStringLiteral("invitation_seen_");
  for (qsizetype offset = 0; offset < uniqueIds.size(); offset += kBatchSize) {
    const qsizetype count = qMin(kBatchSize, uniqueIds.size() - offset);
    QStringList placeholders;
    placeholders.fill(QStringLiteral("?"), count);
    QSqlQuery query(m_database);
    query.prepare(
        QStringLiteral("SELECT key,value_json FROM settings WHERE key IN (%1)")
            .arg(placeholders.join(QLatin1Char(','))));
    for (qsizetype index = 0; index < count; ++index) {
      query.addBindValue(prefix + uniqueIds.at(offset + index));
    }
    if (!query.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(query, QStringLiteral("list invitation seen states"));
      }
      return {};
    }
    while (query.next()) {
      const QString key = query.value(0).toString();
      if (!key.startsWith(prefix)) {
        continue;
      }
      bool valid = false;
      const bool seen = parseValue(query.value(1).toString(), false, &valid).toBool();
      if (valid) {
        result.insert(key.sliced(prefix.size()), seen);
      }
    }
  }
  return result;
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
    const QStringList& prunedRemoteIds, QString* errorMessage) {
  if (!m_database.transaction()) {
    if (errorMessage != nullptr) {
      *errorMessage = m_database.lastError().text();
    }
    return false;
  }
  if (!completeOutboxInternal(id, remoteEvent, false, errorMessage) ||
      !applyRemoteSyncBatch(calendar, events, deletedRemoteIds, prunedRemoteIds,
                            errorMessage)) {
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
    SELECT o.event_id,o.operation,e.calendar_id,e.uid,e.recurrence_id
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
  } else if (remoteEvent != nullptr) {
    if (hasLaterMutation) {
      QSqlQuery revisionQuery(m_database);
      revisionQuery.prepare(QStringLiteral(R"SQL(
        UPDATE events SET remote_id=?, etag=?, recurrence_id=?, raw_payload=?,
          raw_format=?, dirty=1, updated_at=?
        WHERE id=?
      )SQL"));
      revisionQuery.addBindValue(nonNull(remoteEvent->remoteId));
      revisionQuery.addBindValue(nonNull(remoteEvent->etag));
      revisionQuery.addBindValue(nonNull(remoteEvent->recurrenceId));
      revisionQuery.addBindValue(nonNull(remoteEvent->rawPayload));
      revisionQuery.addBindValue(nonNull(remoteEvent->rawFormat));
      revisionQuery.addBindValue(isoUtc(nowUtc()));
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
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT * FROM reminder_jobs
    WHERE (state='pending' AND fire_at<=?)
       OR (state='snoozed' AND snoozed_until<=?)
    ORDER BY COALESCE(NULLIF(snoozed_until,''),fire_at),id LIMIT ?
  )SQL"));
  query.addBindValue(isoUtc(now));
  query.addBindValue(isoUtc(now));
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
  const QString timestamp = isoUtc(recoveredAt);
  QSqlQuery reminders(m_database);
  reminders.prepare(QStringLiteral(R"SQL(
    UPDATE reminder_jobs
    SET state=CASE WHEN snoozed_until<>'' THEN 'snoozed' ELSE 'pending' END,
        claimed_at='',claim_token='',lease_expires_at=''
    WHERE state='claimed'
      AND (lease_expires_at='' OR lease_expires_at<=?)
  )SQL"));
  reminders.addBindValue(timestamp);
  QSqlQuery invitations(m_database);
  invitations.prepare(QStringLiteral(R"SQL(
    UPDATE notification_deliveries
    SET state='delivered',delivered_at=COALESCE(NULLIF(claimed_at,''),?)
    WHERE state='claimed'
  )SQL"));
  invitations.addBindValue(timestamp);
  if (!reminders.exec() || !invitations.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          reminders.lastError().isValid()
              ? sqlError(reminders, QStringLiteral("recover reminder claims"))
              : sqlError(invitations, QStringLiteral("recover invitation claims"));
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
    INSERT OR IGNORE INTO notification_deliveries
      (fingerprint,kind,event_id,event_revision,state,claimed_at)
    VALUES (?,?,?,?,'claimed',?)
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
  query.prepare(QStringLiteral(
      "DELETE FROM notification_deliveries WHERE fingerprint=? AND state='claimed'"));
  query.addBindValue(fingerprint);
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("release notification delivery"));
    }
    return false;
  }
  return query.numRowsAffected() == 0 || bumpChangeRevision(errorMessage);
}

bool Database::hasNotificationDeliveryForEvent(const QString& eventId,
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

QList<Event> Database::searchEvents(const QString& text, const QStringList& calendarIds,
                                    const int limit, const int offset,
                                    QString* errorMessage) const {
  EventSearchQuery search;
  search.text = text;
  search.calendarIds = calendarIds;
  search.limit = limit;
  search.offset = offset;
  return searchEvents(search, errorMessage).events;
}

EventSearchPage Database::searchEvents(const EventSearchQuery& search,
                                       QString* errorMessage) const {
  EventSearchPage result;
  const QString trimmed = search.text.trimmed();
  if (trimmed.isEmpty()) {
    return result;
  }
  QString match = trimmed;
  match.replace(QLatin1Char('"'), QStringLiteral("\"\""));
  match = QStringLiteral("\"") + match + QStringLiteral("\"");

  QString filteredRows = QStringLiteral(R"SQL(
    FROM events e
    JOIN events_fts ON events_fts.event_id=e.id
    JOIN calendars c ON c.id=e.calendar_id
    WHERE e.deleted=0 AND events_fts MATCH ?
  )SQL");
  QVariantList bindings{match};
  if (!search.calendarIds.isEmpty()) {
    QStringList placeholders;
    placeholders.fill(QStringLiteral("?"), search.calendarIds.size());
    filteredRows += QStringLiteral(" AND e.calendar_id IN (%1)")
                        .arg(placeholders.join(QLatin1Char(',')));
    for (const QString& calendarId : search.calendarIds) {
      bindings.append(calendarId);
    }
  }
  if (!search.accountId.isEmpty()) {
    filteredRows += QStringLiteral(" AND c.account_id=?");
    bindings.append(search.accountId);
  }
  if (search.startUtc.isValid()) {
    filteredRows += QStringLiteral(R"SQL(
      AND ((e.all_day=0 AND e.end_utc>?) OR
           (e.all_day=1 AND e.end_date>?))
    )SQL");
    bindings.append(isoUtc(search.startUtc));
    bindings.append(search.startUtc.toUTC().date().toString(Qt::ISODate));
  }
  if (search.endUtc.isValid()) {
    filteredRows += QStringLiteral(R"SQL(
      AND ((e.all_day=0 AND e.start_utc<?) OR
           (e.all_day=1 AND e.start_date<?))
    )SQL");
    bindings.append(isoUtc(search.endUtc));
    bindings.append(search.endUtc.toUTC().date().toString(Qt::ISODate));
  }
  if (!search.invitationState.isEmpty()) {
    filteredRows += QStringLiteral(R"SQL(
      AND EXISTS (
        SELECT 1
        FROM json_each(CASE WHEN json_valid(e.attendees_json)
                            THEN e.attendees_json ELSE '[]' END) attendee
        WHERE replace(replace(lower(COALESCE(
          json_extract(attendee.value,'$.responseStatus'),
          json_extract(attendee.value,'$.partstat'), '')), '-', ''), '_', '')=?
      )
    )SQL");
    QString normalizedState = search.invitationState.toLower();
    normalizedState.remove(QLatin1Char('-'));
    normalizedState.remove(QLatin1Char('_'));
    bindings.append(normalizedState);
  }

  const auto bindFilters = [&bindings](QSqlQuery* query) {
    for (const QVariant& binding : std::as_const(bindings)) {
      query->addBindValue(binding);
    }
  };
  QSqlQuery countQuery(m_database);
  countQuery.prepare(QStringLiteral("SELECT COUNT(*) ") + filteredRows);
  bindFilters(&countQuery);
  if (!countQuery.exec() || !countQuery.next()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(countQuery, QStringLiteral("count search events"));
    }
    return result;
  }
  result.total = countQuery.value(0).toInt();

  QString sql = QStringLiteral("SELECT e.* ") + filteredRows;
  sql +=
      QStringLiteral(" ORDER BY e.start_utc DESC, e.start_date DESC LIMIT ? OFFSET ?");
  QSqlQuery query(m_database);
  query.prepare(sql);
  bindFilters(&query);
  query.addBindValue(qBound(1, search.limit, 500));
  query.addBindValue(qMax(0, search.offset));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("search events"));
    }
    return result;
  }
  while (query.next()) {
    result.events.append(eventFromQuery(query));
  }
  return result;
}

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
    const QDateTime remoteUpdated =
        remote.updatedAt.isValid() ? remote.updatedAt : conflict.createdAt;
    const bool keepLocal = localUpdated.isValid() && remoteUpdated.isValid() &&
                           localUpdated > remoteUpdated;
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
