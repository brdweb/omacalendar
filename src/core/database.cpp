#include "core/database.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <QVariant>
#include <algorithm>

#include "core/database_p.h"
#include "core/recurrenceexpander.h"

namespace omacalendar {

using namespace detail;

namespace {

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
    // Temporary b-trees (sorting, UNION ALL range queries, FTS merges) stay in
    // memory, and a 16 MiB page cache keeps a warm working set of recent
    // ranges instead of SQLite's 2 MiB default.
    if (!execute(QStringLiteral("PRAGMA foreign_keys = ON"), errorMessage) ||
        !execute(QStringLiteral("PRAGMA busy_timeout = 5000"), errorMessage) ||
        !execute(QStringLiteral("PRAGMA temp_store = MEMORY"), errorMessage) ||
        !execute(QStringLiteral("PRAGMA cache_size = -16384"), errorMessage)) {
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
  // synchronous stays at SQLite's FULL default: with WAL, NORMAL can lose the
  // last commits on power loss, including a local edit and its outbox entry,
  // which no provider resync can restore. Reads are served from a 64 MiB
  // memory map.
  if (path != QStringLiteral(":memory:") &&
      (!execute(QStringLiteral("PRAGMA journal_mode = WAL"), errorMessage) ||
       !execute(QStringLiteral("PRAGMA mmap_size = 67108864"), errorMessage))) {
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
  m_expansionCache.invalidateIfChanged(-1);
  if (!m_database.isValid()) {
    return;
  }
  if (m_database.isOpen()) {
    // Refresh query-planner statistics for tables whose shape changed during
    // this connection. It is best effort and bounded by SQLite's analysis
    // limit, so a failure here never blocks shutdown.
    QSqlQuery optimize(m_database);
    optimize.exec(QStringLiteral("PRAGMA optimize"));
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

bool Database::rebuildEventInstances(const Event& event, QString* errorMessage,
                                     const QDateTime& referenceNow) {
  QList<Event> occurrences{event};
  if (!event.deleted && !event.recurrenceRule.isEmpty() &&
      event.recurrenceId.isEmpty()) {
    const QDateTime now = referenceNow.isValid() ? referenceNow.toUTC() : nowUtc();
    const RecurrenceExpansionResult expansion =
        RecurrenceExpander::expand({event}, now.addYears(-2), now.addYears(5), 10000);
    if (expansion.truncated) {
      if (errorMessage != nullptr) {
        *errorMessage =
            QStringLiteral("Event instance expansion exceeded its safe work limit");
      }
      return false;
    }
    occurrences = expansion.occurrences;
  }

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
  QSqlQuery savepoint(m_database);
  if (!savepoint.exec(QStringLiteral("SAVEPOINT update_calendar_preferences"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(savepoint, QStringLiteral("start calendar preference update"));
    }
    return false;
  }
  const auto rollback = [this]() {
    QSqlQuery query(m_database);
    query.exec(QStringLiteral("ROLLBACK TO update_calendar_preferences"));
    query.exec(QStringLiteral("RELEASE update_calendar_preferences"));
  };
  if (!ignoreAlerts) {
    QSqlQuery dismiss(m_database);
    dismiss.prepare(QStringLiteral(R"SQL(
      UPDATE reminder_jobs
      SET state='dismissed',claimed_at='',claim_token='',lease_expires_at=''
      WHERE state='pending' AND fire_at<=? AND EXISTS (
        SELECT 1 FROM events e
        JOIN calendars c ON c.id=e.calendar_id
        WHERE e.id=reminder_jobs.event_id AND c.id=? AND c.ignore_alerts=1
      )
    )SQL"));
    dismiss.addBindValue(isoUtc(nowUtc()));
    dismiss.addBindValue(calendarId);
    if (!dismiss.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage =
            sqlError(dismiss, QStringLiteral("dismiss muted calendar reminders"));
      }
      rollback();
      return false;
    }
  }
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
    rollback();
    return false;
  }
  if (!bumpChangeRevision(errorMessage)) {
    rollback();
    return false;
  }
  QSqlQuery release(m_database);
  if (!release.exec(QStringLiteral("RELEASE update_calendar_preferences"))) {
    if (errorMessage != nullptr) {
      *errorMessage =
          sqlError(release, QStringLiteral("commit calendar preference update"));
    }
    rollback();
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

QString Database::providerResourceKey(const QString& remoteId) {
  const qsizetype fragment = remoteId.indexOf(QLatin1Char('#'));
  return fragment < 0 ? remoteId : remoteId.left(fragment);
}

bool Database::upsertProviderResource(const ProviderResource& resource,
                                      QString* errorMessage) {
  if (resource.calendarId.isEmpty() || resource.canonicalKey.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("A provider resource needs a calendar and canonical key");
    }
    return false;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO provider_resources
      (calendar_id,canonical_key,remote_revision,format,acknowledged_json,
       raw_payload,sync_generation)
    VALUES (?,?,?,?,'{}',?,0)
    ON CONFLICT(calendar_id,canonical_key) DO UPDATE SET
      remote_revision=excluded.remote_revision,
      format=excluded.format,
      raw_payload=excluded.raw_payload
  )SQL"));
  query.addBindValue(resource.calendarId);
  query.addBindValue(resource.canonicalKey);
  query.addBindValue(nonNull(resource.remoteRevision));
  query.addBindValue(nonNull(resource.format));
  query.addBindValue(nonNull(resource.rawPayload));
  if (query.exec()) {
    return true;
  }
  if (errorMessage != nullptr) {
    *errorMessage = sqlError(query, QStringLiteral("upsert provider resource"));
  }
  return false;
}

bool Database::removeOrphanedProviderResources(const QString& calendarId,
                                               QString* errorMessage) {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    DELETE FROM provider_resources
    WHERE calendar_id=? AND NOT EXISTS (
      SELECT 1 FROM events e
      WHERE e.calendar_id=provider_resources.calendar_id
        AND e.remote_id<>'' AND e.remote_id=provider_resources.canonical_key
    ) AND NOT EXISTS (
      SELECT 1 FROM events e
      WHERE e.calendar_id=provider_resources.calendar_id
        AND e.remote_id<>''
        AND e.remote_id>=provider_resources.canonical_key || '#'
        AND e.remote_id<provider_resources.canonical_key || '$'
    )
  )SQL"));
  query.addBindValue(calendarId);
  if (query.exec()) {
    return true;
  }
  if (errorMessage != nullptr) {
    *errorMessage =
        sqlError(query, QStringLiteral("remove orphaned provider resources"));
  }
  return false;
}

bool Database::hydrateProviderResource(Event* event, QString* errorMessage) const {
  if (event == nullptr || !event->rawPayload.isEmpty() || event->calendarId.isEmpty() ||
      event->remoteId.isEmpty()) {
    return true;
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    SELECT raw_payload,format FROM provider_resources
    WHERE calendar_id=? AND canonical_key=? LIMIT 1
  )SQL"));
  query.addBindValue(event->calendarId);
  query.addBindValue(providerResourceKey(event->remoteId));
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("load provider resource"));
    }
    return false;
  }
  if (query.next()) {
    event->rawPayload = query.value(0).toString();
    event->rawFormat = query.value(1).toString();
  }
  return true;
}

bool Database::hydrateProviderResources(QList<Event>* events,
                                        QString* errorMessage) const {
  if (events == nullptr || events->isEmpty()) {
    return true;
  }
  QList<QPair<QString, QString>> wanted;
  QSet<QString> observed;
  for (const Event& event : std::as_const(*events)) {
    if (!event.rawPayload.isEmpty() || event.calendarId.isEmpty() ||
        event.remoteId.isEmpty()) {
      continue;
    }
    const QString resourceKey = providerResourceKey(event.remoteId);
    const QString identity = event.calendarId + QLatin1Char('\n') + resourceKey;
    if (!observed.contains(identity)) {
      observed.insert(identity);
      wanted.append({event.calendarId, resourceKey});
    }
  }
  if (wanted.isEmpty()) {
    return true;
  }
  QHash<QString, QPair<QString, QString>> payloads;
  constexpr qsizetype kLookupBatchSize = 200;
  for (qsizetype offset = 0; offset < wanted.size(); offset += kLookupBatchSize) {
    const qsizetype count = qMin(kLookupBatchSize, wanted.size() - offset);
    QStringList predicates;
    predicates.fill(QStringLiteral("(calendar_id=? AND canonical_key=?)"), count);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(R"SQL(
      SELECT calendar_id,canonical_key,raw_payload,format
      FROM provider_resources WHERE %1
    )SQL")
                      .arg(predicates.join(QStringLiteral(" OR "))));
    for (qsizetype index = 0; index < count; ++index) {
      const auto& identity = wanted.at(offset + index);
      query.addBindValue(identity.first);
      query.addBindValue(identity.second);
    }
    if (!query.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(query, QStringLiteral("load provider resources"));
      }
      return false;
    }
    while (query.next()) {
      const QString identity =
          query.value(0).toString() + QLatin1Char('\n') + query.value(1).toString();
      payloads.insert(identity, {query.value(2).toString(), query.value(3).toString()});
    }
  }
  for (Event& event : *events) {
    if (!event.rawPayload.isEmpty() || event.calendarId.isEmpty() ||
        event.remoteId.isEmpty()) {
      continue;
    }
    const QString identity =
        event.calendarId + QLatin1Char('\n') + providerResourceKey(event.remoteId);
    const auto payload = payloads.constFind(identity);
    if (payload != payloads.constEnd()) {
      event.rawPayload = payload->first;
      event.rawFormat = payload->second;
    }
  }
  return true;
}

bool Database::upsertEventRecord(const Event& event, QString* errorMessage,
                                 const bool omitProviderPayload) {
  bool normalizedProviderPayload = omitProviderPayload;
  if (!normalizedProviderPayload && !event.dirty && !event.remoteId.isEmpty() &&
      event.rawFormat == QStringLiteral("text/calendar")) {
    QSqlQuery resource(m_database);
    resource.prepare(QStringLiteral(R"SQL(
      SELECT 1 FROM provider_resources
      WHERE calendar_id=? AND canonical_key=? LIMIT 1
    )SQL"));
    resource.addBindValue(event.calendarId);
    resource.addBindValue(providerResourceKey(event.remoteId));
    if (!resource.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage =
            sqlError(resource, QStringLiteral("check normalized provider resource"));
      }
      return false;
    }
    normalizedProviderPayload = resource.next();
  }
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    INSERT INTO events
      (id, calendar_id, remote_id, uid, etag, summary, description, location,
       url, conference_url, start_utc, end_utc, start_date, end_date,
       start_timezone, end_timezone, all_day, time_kind, status, transparency,
       visibility, recurrence_rule, recurrence_id, series_until_utc,
       series_until_date, original_start_utc, original_start_date, sequence,
       organizer_json, attendees_json, reminders_json, raw_payload, raw_format,
       dirty, deleted, local_revision, sync_state, created_at, updated_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
      series_until_utc=excluded.series_until_utc,
      series_until_date=excluded.series_until_date,
      original_start_utc=excluded.original_start_utc,
      original_start_date=excluded.original_start_date,
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
  // Range queries compare the stored end date in SQL, so an inclusive all-day
  // span must never reach storage regardless of which caller supplied it.
  query.addBindValue(
      nonNull((event.allDay ? exclusiveAllDayEnd(event.startDate, event.endDate)
                            : event.endDate)
                  .toString(Qt::ISODate)));
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
  const RecurrenceStoredBounds bounds = recurrenceBounds(event);
  query.addBindValue(bounds.untilUtc);
  query.addBindValue(bounds.untilDate);
  query.addBindValue(bounds.originalUtc);
  query.addBindValue(bounds.originalDate);
  query.addBindValue(event.sequence);
  query.addBindValue(compactJson(event.organizer));
  query.addBindValue(compactJson(event.attendees));
  query.addBindValue(compactJson(event.reminders));
  query.addBindValue(normalizedProviderPayload ? QStringLiteral("")
                                               : nonNull(event.rawPayload));
  query.addBindValue(normalizedProviderPayload ? QStringLiteral("")
                                               : nonNull(event.rawFormat));
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
  return applyRemoteEventInternal(std::move(event), errorMessage, conflicted, false);
}

bool Database::applyRemoteEventInternal(Event event, QString* errorMessage,
                                        bool* conflicted,
                                        const bool omitProviderPayload) {
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
    Event candidate = eventFromQuery(existingQuery);
    if (!hydrateProviderResource(&candidate, errorMessage)) {
      return false;
    }
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
  return upsertEventRecord(event, errorMessage, omitProviderPayload) &&
         bumpChangeRevision(errorMessage);
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
  if (!hydrateProviderResources(&existingEvents, errorMessage)) {
    return false;
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
                                    QString* errorMessage,
                                    const QList<ProviderResource>& providerResources,
                                    const bool finalizeCalendar) {
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

  QHash<QString, ProviderResource> stagedResources;
  for (const ProviderResource& resource : providerResources) {
    if (resource.calendarId != calendar.id || resource.canonicalKey.isEmpty()) {
      return fail(
          QStringLiteral("A staged provider resource belongs to another calendar"));
    }
    stagedResources.insert(resource.canonicalKey, resource);
  }
  for (const Event& event : events) {
    if (event.calendarId != calendar.id) {
      return fail(QStringLiteral("A staged event belongs to another calendar"));
    }
    Event stagedEvent = event;
    const QString resourceKey = providerResourceKey(stagedEvent.remoteId);
    const auto resource = stagedResources.constFind(resourceKey);
    const bool hasProviderResource = resource != stagedResources.constEnd();
    if (hasProviderResource) {
      stagedEvent.rawPayload = resource->rawPayload;
      stagedEvent.rawFormat = resource->format;
    }
    bool conflicted = false;
    if (!applyRemoteEventInternal(std::move(stagedEvent), errorMessage, &conflicted,
                                  hasProviderResource)) {
      return fail(QStringLiteral("Unable to apply a staged remote event"));
    }
    if (conflicted && hasProviderResource) {
      // A locally edited exception must keep its own acknowledged editing base,
      // while clean siblings advance to the incoming resource revision. Only
      // user-created dirty state needs this snapshot; remote rows share one blob.
      const Event existing = eventByRemoteId(calendar.id, event.remoteId, errorMessage);
      if (existing.id.isEmpty()) {
        return fail(QStringLiteral("Unable to preserve a conflicted event base"));
      }
      QSqlQuery preserve(m_database);
      preserve.prepare(QStringLiteral(
          "UPDATE events SET raw_payload=?,raw_format=? WHERE id=? AND dirty=1"));
      preserve.addBindValue(existing.rawPayload);
      preserve.addBindValue(existing.rawFormat);
      preserve.addBindValue(existing.id);
      if (!preserve.exec()) {
        return fail(
            sqlError(preserve, QStringLiteral("preserve conflicted event base")));
      }
    }
  }
  for (auto resource = stagedResources.constBegin();
       resource != stagedResources.constEnd(); ++resource) {
    if (!upsertProviderResource(resource.value(), errorMessage)) {
      return fail(QStringLiteral("Unable to stage a provider resource"));
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
  if (finalizeCalendar &&
      (!removeOrphanedProviderResources(calendar.id, errorMessage) ||
       !upsertCalendar(calendar, errorMessage))) {
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
    QString* errorMessage, const bool replaceExistingCoverage,
    const QList<ProviderResource>& providerResources) {
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
                            errorMessage, providerResources)) {
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
  // Sync bookkeeping is not an edit. Repeated conflict observations must keep
  // the original content timestamp used to compare the local and remote edits.
  mark.prepare(
      QStringLiteral("UPDATE events SET sync_state='conflict',dirty=1 WHERE id=?"));
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
    SELECT id, operation, payload_json FROM outbox
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
    if (event->conferenceRequestId.isEmpty()) {
      // A conference asked for by a still-queued write must survive later
      // edits that are folded into the same outbox item.
      event->conferenceRequestId =
          QJsonDocument::fromJson(pendingQuery.value(2).toString().toUtf8())
              .object()
              .value(QStringLiteral("conferenceRequestId"))
              .toString();
    }
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
  Event result = eventFromQuery(query);
  return hydrateProviderResource(&result, errorMessage) ? result : Event{};
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
  Event result = eventFromQuery(query);
  return hydrateProviderResource(&result, errorMessage) ? result : Event{};
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
    Event candidate = eventFromQuery(query);
    if (recurrenceId.isEmpty()) {
      if (candidate.recurrenceId.isEmpty()) {
        return hydrateProviderResource(&candidate, errorMessage) ? candidate : Event{};
      }
      continue;
    }
    if (recurrenceIdentityEqual(recurrenceId, candidate.recurrenceId, candidate.allDay,
                                candidate.timeKind, candidate.startTimeZone)) {
      return hydrateProviderResource(&candidate, errorMessage) ? candidate : Event{};
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
  return hydrateProviderResources(&result, errorMessage) ? result : QList<Event>{};
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
  return hydrateProviderResources(&result, errorMessage) ? result : QList<Event>{};
}

QList<Event> Database::eventsBetween(const QDateTime& startUtc, const QDateTime& endUtc,
                                     const QStringList& calendarIds,
                                     QString* errorMessage) const {
  return eventsBetweenInternal(startUtc, endUtc, calendarIds, false, 10000, 100000,
                               errorMessage);
}

QList<Event> Database::dailyCountEventsBetween(const QDateTime& startUtc,
                                               const QDateTime& endUtc,
                                               const QStringList& calendarIds,
                                               QString* errorMessage) const {
  return eventsBetweenInternal(startUtc, endUtc, calendarIds, false, 50000, 500000,
                               errorMessage);
}

QList<Event> Database::invitationEventsBetween(const QDateTime& startUtc,
                                               const QDateTime& endUtc,
                                               QString* errorMessage) const {
  return eventsBetweenInternal(startUtc, endUtc, {}, true, 10000, 100000, errorMessage);
}

QList<Event> Database::eventsBetweenInternal(
    const QDateTime& startUtc, const QDateTime& endUtc, const QStringList& calendarIds,
    const bool invitationsOnly, const qsizetype maximumOccurrences,
    const qsizetype maximumExpansionSteps, QString* errorMessage) const {
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
    for (qsizetype index = 0; index < calendarIds.size(); ++index) {
      placeholders.append(QStringLiteral(":calendar%1").arg(index));
    }
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

  // Fetch only the fields consumed by Event and recurrence expansion, not the
  // schema-3 bound columns (or future storage-only metadata).
  const QString columns = QStringLiteral(
      "e.id,e.calendar_id,e.remote_id,e.uid,e.etag,e.summary,e.description,"
      "e.location,e.url,e.conference_url,e.start_utc,e.end_utc,e.start_date,"
      "e.end_date,e.start_timezone,e.end_timezone,e.all_day,e.time_kind,e.status,"
      "e.transparency,e.visibility,e.recurrence_rule,e.recurrence_id,e.sequence,"
      "e.organizer_json,e.attendees_json,e.reminders_json,e.raw_payload,e.raw_format,"
      "e.dirty,e.deleted,e.local_revision,e.sync_state,e.created_at,e.updated_at");
  const QString sql =
      QStringLiteral(R"SQL(
    SELECT * FROM (
      SELECT %4 FROM events AS e
      WHERE e.deleted=0 AND e.all_day=0
        AND e.recurrence_rule='' AND e.recurrence_id=''
        AND e.end_utc>:startUtc AND e.start_utc<:endUtc%1%2
      UNION ALL
      SELECT %4 FROM events AS e
      WHERE e.deleted=0 AND e.all_day=1
        AND e.recurrence_rule='' AND e.recurrence_id=''
        AND e.end_date>:startDate AND e.start_date<:endDate%1%2
      UNION ALL
      SELECT %4 FROM events AS e INDEXED BY events_series_utc_bound_index
      WHERE e.deleted=0 AND e.recurrence_rule<>'' AND e.recurrence_id=''
        AND e.all_day=0 AND e.time_kind<>'floating'
        AND (e.start_utc='' OR e.start_utc<:endUtc
             OR e.recurrence_rule LIKE '%RDATE%'
             OR e.raw_format='text/calendar'
             OR EXISTS (
               SELECT 1 FROM provider_resources AS resource
               WHERE resource.calendar_id=e.calendar_id
                 AND resource.canonical_key=
                   substr(e.remote_id,1,
                     CASE WHEN instr(e.remote_id,'#')=0 THEN length(e.remote_id)
                          ELSE instr(e.remote_id,'#')-1 END)
                 AND resource.raw_payload LIKE '%RDATE%'))
        AND COALESCE(e.series_until_utc,'~')>=:startUtc%3%2
      UNION ALL
      SELECT %4 FROM events AS e INDEXED BY events_series_floating_bound_index
      WHERE e.deleted=0 AND e.recurrence_rule<>'' AND e.recurrence_id=''
        AND e.all_day=0 AND e.time_kind='floating'
        AND (e.start_utc='' OR e.start_utc<:endUtc
             OR e.recurrence_rule LIKE '%RDATE%'
             OR e.raw_format='text/calendar'
             OR EXISTS (
               SELECT 1 FROM provider_resources AS resource
               WHERE resource.calendar_id=e.calendar_id
                 AND resource.canonical_key=
                   substr(e.remote_id,1,
                     CASE WHEN instr(e.remote_id,'#')=0 THEN length(e.remote_id)
                          ELSE instr(e.remote_id,'#')-1 END)
                 AND resource.raw_payload LIKE '%RDATE%'))
        AND COALESCE(e.series_until_date,'~')>=:startDate%3%2
      UNION ALL
      SELECT %4 FROM events AS e INDEXED BY events_series_date_bound_index
      WHERE e.deleted=0 AND e.recurrence_rule<>'' AND e.recurrence_id=''
        AND e.all_day=1
        AND (e.start_date='' OR e.start_date<:endDate
             OR e.recurrence_rule LIKE '%RDATE%'
             OR e.raw_format='text/calendar'
             OR EXISTS (
               SELECT 1 FROM provider_resources AS resource
               WHERE resource.calendar_id=e.calendar_id
                 AND resource.canonical_key=
                   substr(e.remote_id,1,
                     CASE WHEN instr(e.remote_id,'#')=0 THEN length(e.remote_id)
                          ELSE instr(e.remote_id,'#')-1 END)
                 AND resource.raw_payload LIKE '%RDATE%'))
        AND COALESCE(e.series_until_date,'~')>=:startDate%3%2
      UNION ALL
      SELECT %4 FROM events AS e INDEXED BY events_exception_utc_index
      WHERE e.recurrence_id<>'' AND (
        (e.all_day=0 AND e.start_utc<:endUtc AND
          (e.end_utc>:startUtc OR
           (e.end_utc=e.start_utc AND e.start_utc>=:startUtc)))
        OR (e.all_day=1 AND e.start_date<:endDate AND
            e.end_date>:startDate)
        OR (e.original_start_utc>=:startUtc AND e.original_start_utc<:endUtc)
        OR (e.original_start_date>=:startDate AND
            e.original_start_date<:endDate)
        OR (e.recurrence_id LIKE '%RANGE=THISANDFUTURE%' AND
            (e.original_start_utc<:endUtc OR e.original_start_date<:endDate))
        OR (e.original_start_utc IS NULL AND e.original_start_date IS NULL)
      )%3%2
    ) AS bounded_events
    ORDER BY all_day DESC, COALESCE(NULLIF(start_utc,''), start_date), id
  )SQL")
          .arg(invitedOnly, calendarClause, recurringInvitationFilter, columns);

  QSqlQuery query(m_database);
  query.prepare(sql);
  query.bindValue(QStringLiteral(":startUtc"), isoUtc(startUtc));
  query.bindValue(QStringLiteral(":endUtc"), isoUtc(endUtc));
  query.bindValue(QStringLiteral(":startDate"), startUtc.date().toString(Qt::ISODate));
  const QDate allDayEnd =
      endUtc.time() == QTime(0, 0) ? endUtc.date() : endUtc.date().addDays(1);
  query.bindValue(QStringLiteral(":endDate"), allDayEnd.toString(Qt::ISODate));
  for (qsizetype index = 0; index < calendarIds.size(); ++index) {
    query.bindValue(QStringLiteral(":calendar%1").arg(index), calendarIds.at(index));
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
  if (!hydrateProviderResources(&candidates, errorMessage)) {
    return {};
  }
  // A detached occurrence can move into this window from an expired series.
  // Keep its master when present: cancelled masters and RANGE exceptions
  // consume their children even if the master's final generated end is old.
  // Look up only parents of selected exceptions, never every historical series.
  const auto parentIdentity = [](const QString& kind, const QString& calendar,
                                 const QString& value) {
    return kind + QLatin1Char('\n') + calendar + QLatin1Char('\n') + value;
  };
  QSet<QString> knownMasters;
  for (const Event& candidate : std::as_const(candidates)) {
    if (candidate.recurrenceId.isEmpty() && !candidate.recurrenceRule.isEmpty()) {
      knownMasters.insert(
          parentIdentity(QStringLiteral("uid"), candidate.calendarId, candidate.uid));
      if (!candidate.remoteId.isEmpty()) {
        knownMasters.insert(parentIdentity(QStringLiteral("google"),
                                           candidate.calendarId, candidate.remoteId));
      }
    }
  }
  QSet<QString> checkedParents;
  const qsizetype selectedCount = candidates.size();
  for (qsizetype index = 0; index < selectedCount; ++index) {
    const Event& exception = candidates.at(index);
    if (exception.recurrenceId.isEmpty()) {
      continue;
    }
    QString googleParent;
    if (exception.rawFormat == QStringLiteral("google-json") &&
        !exception.rawPayload.isEmpty()) {
      googleParent = QJsonDocument::fromJson(exception.rawPayload.toUtf8())
                         .object()
                         .value(QStringLiteral("recurringEventId"))
                         .toString();
    }
    const QString identity = parentIdentity(
        googleParent.isEmpty() ? QStringLiteral("uid") : QStringLiteral("google"),
        exception.calendarId, googleParent.isEmpty() ? exception.uid : googleParent);
    if (knownMasters.contains(identity) || checkedParents.contains(identity)) {
      continue;
    }
    checkedParents.insert(identity);
    QSqlQuery parent(m_database);
    parent.prepare(QStringLiteral("SELECT %1 FROM events AS e WHERE e.calendar_id=? "
                                  "AND e.recurrence_rule<>'' AND e.recurrence_id='' "
                                  "AND e.deleted=0 AND %2=? LIMIT 1")
                       .arg(columns, googleParent.isEmpty()
                                         ? QStringLiteral("e.uid")
                                         : QStringLiteral("e.remote_id")));
    parent.addBindValue(exception.calendarId);
    parent.addBindValue(googleParent.isEmpty() ? exception.uid : googleParent);
    if (!parent.exec()) {
      if (errorMessage != nullptr) {
        *errorMessage = sqlError(parent, QStringLiteral("load exception parent"));
      }
      return {};
    }
    if (parent.next()) {
      Event master = eventFromQuery(parent);
      if (!hydrateProviderResource(&master, errorMessage)) {
        return {};
      }
      knownMasters.insert(
          parentIdentity(QStringLiteral("uid"), master.calendarId, master.uid));
      if (!master.remoteId.isEmpty()) {
        knownMasters.insert(parentIdentity(QStringLiteral("google"), master.calendarId,
                                           master.remoteId));
      }
      candidates.append(std::move(master));
    }
  }
  m_expansionCache.invalidateIfChanged(changeRevision());
  const RecurrenceExpansionResult expansion =
      RecurrenceExpander::expand(candidates, startUtc, endUtc, maximumOccurrences,
                                 maximumExpansionSteps, &m_expansionCache);
  if (expansion.truncated) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "Calendar recurrence expansion exceeded its safe work or occurrence limit");
    }
    return {};
  }
  const QList<Event>& expanded = expansion.occurrences;
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

QJsonArray Database::contactSuggestions(const QString& prefix, const int limit,
                                        QString* errorMessage) const {
  QString needle = prefix.trimmed().toLower();
  needle.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
  needle.replace(QLatin1Char('%'), QStringLiteral("\\%"));
  needle.replace(QLatin1Char('_'), QStringLiteral("\\_"));
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(R"SQL(
    WITH people AS (
      SELECT lower(trim(json_extract(attendee.value, '$.email'))) AS email,
             COALESCE(json_extract(attendee.value, '$.displayName'), '') AS name
      FROM events AS e, json_each(e.attendees_json) AS attendee
      WHERE e.deleted=0 AND json_valid(e.attendees_json)
        AND json_type(attendee.value) = 'object'
      UNION ALL
      SELECT lower(trim(json_extract(e.organizer_json, '$.email'))),
             COALESCE(json_extract(e.organizer_json, '$.displayName'), '')
      FROM events AS e
      WHERE e.deleted=0 AND json_valid(e.organizer_json)
        AND json_type(e.organizer_json) = 'object'
    )
    SELECT email, MAX(name) AS name, COUNT(*) AS uses FROM people
    WHERE email IS NOT NULL AND email LIKE '%_@_%'
      AND (email LIKE ? || '%' ESCAPE '\' OR lower(name) LIKE '%' || ? || '%' ESCAPE '\')
    GROUP BY email
    ORDER BY uses DESC, email
    LIMIT ?
  )SQL"));
  query.addBindValue(needle);
  query.addBindValue(needle);
  query.addBindValue(limit);
  QJsonArray result;
  if (!query.exec()) {
    if (errorMessage != nullptr) {
      *errorMessage = sqlError(query, QStringLiteral("suggest contacts"));
    }
    return result;
  }
  while (query.next()) {
    result.append(QJsonObject{
        {QStringLiteral("email"), query.value(0).toString()},
        {QStringLiteral("displayName"), query.value(1).toString()},
    });
  }
  return result;
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
  const QString attendee = search.attendee.trimmed();
  if (trimmed.isEmpty() && attendee.isEmpty()) {
    return result;
  }
  QString filteredRows;
  QVariantList bindings;
  if (trimmed.isEmpty()) {
    filteredRows = QStringLiteral(R"SQL(
      FROM events e
      JOIN calendars c ON c.id=e.calendar_id
      WHERE e.deleted=0
    )SQL");
  } else {
    QString match = trimmed;
    match.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    match = QStringLiteral("\"") + match + QStringLiteral("\"");
    filteredRows = QStringLiteral(R"SQL(
      FROM events e
      JOIN events_fts ON events_fts.event_id=e.id
      JOIN calendars c ON c.id=e.calendar_id
      WHERE e.deleted=0 AND events_fts MATCH ?
    )SQL");
    bindings.append(match);
  }
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
  if (!attendee.isEmpty()) {
    QString pattern = attendee;
    pattern.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    pattern.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    pattern.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    pattern = QLatin1Char('%') + pattern + QLatin1Char('%');
    filteredRows += QStringLiteral(R"SQL(
      AND (EXISTS (
             SELECT 1
             FROM json_each(CASE WHEN json_valid(e.attendees_json)
                                 THEN e.attendees_json ELSE '[]' END) guest
             WHERE COALESCE(json_extract(guest.value,'$.email'),'') LIKE ? ESCAPE '\'
                OR COALESCE(json_extract(guest.value,'$.displayName'),'') LIKE ? ESCAPE '\')
           OR (json_valid(e.organizer_json) AND (
             COALESCE(json_extract(e.organizer_json,'$.email'),'') LIKE ? ESCAPE '\'
             OR COALESCE(json_extract(e.organizer_json,'$.displayName'),'')
                LIKE ? ESCAPE '\')))
    )SQL");
    for (int index = 0; index < 4; ++index) {
      bindings.append(pattern);
    }
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

bool Database::refreshCalDavTimeKinds(const QString& accountId,
                                      const CalDavTimeKindLookupBuilder& buildLookup,
                                      QString* errorMessage) {
  const QString key = QStringLiteral("caldav_time_kind_version");
  QString error;
  if (providerState(accountId, {}, key, 0, &error).toInt() >= 1) {
    return true;
  }
  if (!error.isEmpty() || account(accountId, &error).provider != ProviderKind::CalDav) {
    if (errorMessage != nullptr) {
      *errorMessage =
          error.isEmpty() ? QStringLiteral("CalDAV account required") : error;
    }
    return false;
  }
  if (!execute(QStringLiteral("SAVEPOINT refresh_caldav_time_kinds"), errorMessage)) {
    return false;
  }
  const auto rollback = [this]() {
    execute(QStringLiteral("ROLLBACK TO refresh_caldav_time_kinds"), nullptr);
    execute(QStringLiteral("RELEASE refresh_caldav_time_kinds"), nullptr);
    return false;
  };
  struct CachedLookup final {
    QByteArray payload;
    CalDavTimeKindLookup lookup;
  };
  QHash<QByteArray, QList<CachedLookup>> payloadLookups;
  QHash<QString, CalDavTimeKindLookup> resourceLookups;
  const auto lookupForPayload = [&buildLookup, &payloadLookups, errorMessage](
                                    const QByteArray& payload,
                                    CalDavTimeKindLookup* lookup) {
    const QByteArray digest =
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
    const auto cached = payloadLookups.constFind(digest);
    if (cached != payloadLookups.constEnd()) {
      for (const CachedLookup& candidate : *cached) {
        if (candidate.payload == payload) {
          *lookup = candidate.lookup;
          return true;
        }
      }
    }
    CalDavTimeKindLookup built;
    if (!buildLookup(payload, &built, errorMessage)) {
      return false;
    }
    if (!built) {
      if (errorMessage != nullptr) {
        *errorMessage =
            QStringLiteral("CalDAV time metadata lookup builder returned no lookup");
      }
      return false;
    }
    payloadLookups[digest].append({payload, built});
    *lookup = std::move(built);
    return true;
  };
  const auto lookupFor = [this, &lookupForPayload, &resourceLookups, errorMessage](
                             const Event& value, CalDavTimeKindLookup* lookup) {
    if (!value.rawPayload.isEmpty()) {
      return lookupForPayload(value.rawPayload.toUtf8(), lookup);
    }
    const QString resourceIdentity =
        value.calendarId + QLatin1Char('\n') + providerResourceKey(value.remoteId);
    const auto cached = resourceLookups.constFind(resourceIdentity);
    if (cached != resourceLookups.constEnd()) {
      *lookup = *cached;
      return true;
    }
    Event hydrated = value;
    if (!hydrateProviderResource(&hydrated, errorMessage)) {
      return false;
    }
    CalDavTimeKindLookup built;
    if (!lookupForPayload(hydrated.rawPayload.toUtf8(), &built)) {
      return false;
    }
    resourceLookups.insert(resourceIdentity, built);
    *lookup = std::move(built);
    return true;
  };
  const auto normalize = [&lookupFor, errorMessage](Event* value) {
    // A nonempty timezone may be an intentional local conversion. New local
    // events without provider source are also left as explicitly authored.
    if (value->timeKind != TimeKind::Zoned || !value->startTimeZone.isEmpty() ||
        !value->endTimeZone.isEmpty()) {
      return true;
    }
    if (value->allDay) {
      value->timeKind = TimeKind::AllDay;
      return true;
    }
    if (value->rawPayload.isEmpty() && value->remoteId.isEmpty()) {
      return true;
    }
    CalDavTimeKindLookup lookup;
    return lookupFor(*value, &lookup) && lookup(*value, &value->timeKind, errorMessage);
  };
  QSqlQuery events(m_database);
  events.prepare(
      QStringLiteral("SELECT e.* FROM events e JOIN calendars c ON c.id=e.calendar_id "
                     "WHERE c.account_id=? ORDER BY e.id"));
  events.addBindValue(accountId);
  if (!events.exec()) {
    if (errorMessage != nullptr)
      *errorMessage = sqlError(events, QStringLiteral("read legacy events"));
    return rollback();
  }
  bool changed = false;
  while (events.next()) {
    Event value = eventFromQuery(events);
    const TimeKind previous = value.timeKind;
    if (!normalize(&value)) return rollback();
    if (value.timeKind == previous) continue;
    if (!hydrateProviderResource(&value, errorMessage)) return rollback();
    const RecurrenceStoredBounds bounds = recurrenceBounds(value);
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE events SET time_kind=?,series_until_utc=?,series_until_date=?,"
        "original_start_utc=?,original_start_date=? WHERE id=?"));
    update.addBindValue(timeKindToString(value.timeKind));
    update.addBindValue(bounds.untilUtc);
    update.addBindValue(bounds.untilDate);
    update.addBindValue(bounds.originalUtc);
    update.addBindValue(bounds.originalDate);
    update.addBindValue(value.id);
    if (!update.exec()) {
      if (errorMessage != nullptr)
        *errorMessage = sqlError(update, QStringLiteral("refresh event time kind"));
      return rollback();
    }
    if (!rebuildEventInstances(value, errorMessage) ||
        !rebuildReminderJobs(value, errorMessage))
      return rollback();
    changed = true;
  }
  // Patch just the metadata key, preserving unknown mutation fields and every
  // operation identifier/revision, timestamp, lease and retry state verbatim.
  const auto normalizeSnapshot = [this, &normalize, &accountId](QJsonObject* object) {
    if (object->isEmpty()) return true;
    Event value = eventFromJson(*object);
    if (calendar(value.calendarId).accountId != accountId) return true;
    const TimeKind previous = value.timeKind;
    if (!normalize(&value)) return false;
    if (value.timeKind != previous) {
      object->insert(QStringLiteral("timeKind"), timeKindToString(value.timeKind));
    }
    return true;
  };
  QSqlQuery mutations(m_database);
  mutations.prepare(
      QStringLiteral("SELECT id,payload_json FROM outbox WHERE account_id=? "
                     "AND state IN ('pending','sending','retry_wait','blocked')"));
  mutations.addBindValue(accountId);
  if (!mutations.exec()) {
    if (errorMessage != nullptr)
      *errorMessage = sqlError(mutations, QStringLiteral("read legacy mutations"));
    return rollback();
  }
  while (mutations.next()) {
    QJsonObject payload = parseObject(mutations.value(1).toString());
    const QJsonObject original = payload;
    if (!normalizeSnapshot(&payload)) return rollback();
    if (payload.contains(QStringLiteral("_move"))) {
      QJsonObject move = payload.value(QStringLiteral("_move")).toObject();
      QJsonObject source = move.value(QStringLiteral("sourceEvent")).toObject();
      const QJsonObject oldSource = source;
      if (!normalizeSnapshot(&source)) return rollback();
      if (source != oldSource) {
        move.insert(QStringLiteral("sourceEvent"), source);
        payload.insert(QStringLiteral("_move"), move);
      }
    }
    if (payload == original) continue;
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE outbox SET payload_json=? WHERE id=?"));
    update.addBindValue(compactJson(payload));
    update.addBindValue(mutations.value(0));
    if (!update.exec()) {
      if (errorMessage != nullptr)
        *errorMessage = sqlError(update, QStringLiteral("refresh mutation time kind"));
      return rollback();
    }
    changed = true;
  }
  QSqlQuery conflicts(m_database);
  conflicts.prepare(QStringLiteral(
      "SELECT f.id,f.local_snapshot_json,f.remote_snapshot_json FROM conflicts f "
      "JOIN events e ON e.id=f.event_id JOIN calendars c ON c.id=e.calendar_id "
      "WHERE c.account_id=? AND f.state='unresolved'"));
  conflicts.addBindValue(accountId);
  if (!conflicts.exec()) {
    if (errorMessage != nullptr)
      *errorMessage = sqlError(conflicts, QStringLiteral("read legacy conflicts"));
    return rollback();
  }
  while (conflicts.next()) {
    QJsonObject local = parseObject(conflicts.value(1).toString());
    QJsonObject remote = parseObject(conflicts.value(2).toString());
    const QJsonObject oldLocal = local;
    const QJsonObject oldRemote = remote;
    if (!normalizeSnapshot(&local) || !normalizeSnapshot(&remote)) return rollback();
    if (local == oldLocal && remote == oldRemote) continue;
    QSqlQuery update(m_database);
    update.prepare(
        QStringLiteral("UPDATE conflicts SET "
                       "local_snapshot_json=?,remote_snapshot_json=? WHERE id=?"));
    update.addBindValue(compactJson(local));
    update.addBindValue(compactJson(remote));
    update.addBindValue(conflicts.value(0));
    if (!update.exec()) {
      if (errorMessage != nullptr)
        *errorMessage = sqlError(update, QStringLiteral("refresh conflict time kind"));
      return rollback();
    }
    changed = true;
  }
  if (!setProviderState(accountId, {}, key, 1, errorMessage) ||
      (changed && !bumpChangeRevision(errorMessage)) ||
      !execute(QStringLiteral("RELEASE refresh_caldav_time_kinds"), errorMessage)) {
    return rollback();
  }
  return true;
}

}  // namespace omacalendar
