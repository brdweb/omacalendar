// Copyright (c) 2026

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest/QtTest>
#include <algorithm>

#include "core/database.h"

using namespace omacalendar;

namespace {

Account makeAccount(const QString& id, const QString& name) {
  Account account;
  account.id = id;
  account.provider = ProviderKind::Google;
  account.displayName = name;
  account.principal = "user@example.com";
  account.endpoint = "https://example.local/";
  account.enabled = true;
  account.authStatus = QStringLiteral("connected");
  account.createdAt = QDateTime(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::utc());
  account.updatedAt = account.createdAt;
  return account;
}

Calendar makeCalendar(const QString& id, const QString& accountId) {
  Calendar calendar;
  calendar.id = id;
  calendar.accountId = accountId;
  calendar.remoteId = QStringLiteral("remote-") + id;
  calendar.href = QStringLiteral("/cal/") + id;
  calendar.name = QStringLiteral("Calendar ") + id;
  calendar.description = QStringLiteral("Test calendar");
  calendar.color = QStringLiteral("#7aa2f7");
  calendar.timeZone = "UTC";
  calendar.readOnly = false;
  calendar.enabled = true;
  calendar.etag = QStringLiteral("etag-") + id;
  calendar.syncToken = QStringLiteral("sync-") + id;
  calendar.capabilities = QJsonObject{{"read", true}};
  calendar.lastSyncAt = QDateTime(QDate(2026, 1, 2), QTime(0, 0), QTimeZone::utc());
  return calendar;
}

Event makeRemoteEvent(const QString& calendarId, const QString& id, int offsetHours) {
  Event event;
  event.id = id;
  event.calendarId = calendarId;
  event.remoteId = QStringLiteral("remote-") + id;
  event.uid = QStringLiteral("uid-") + id;
  event.summary = QStringLiteral("Remote ");
  event.summary.append(id);
  event.description = QStringLiteral("");
  event.location = QStringLiteral("");
  event.etag = QStringLiteral("etag-") + id;
  event.recurrenceRule = QStringLiteral("");
  event.recurrenceId = QStringLiteral("");
  event.rawPayload = QStringLiteral("");
  event.rawFormat = QStringLiteral("");
  event.startUtc =
      QDateTime(QDate(2026, 2, 1), QTime(8 + offsetHours, 0), QTimeZone::utc());
  event.endUtc = event.startUtc.addSecs(3600);
  event.startDate = event.startUtc.date();
  event.endDate = event.endUtc.date();
  event.startTimeZone = QStringLiteral("UTC");
  event.endTimeZone = QStringLiteral("UTC");
  event.status = QStringLiteral("confirmed");
  event.transparency = QStringLiteral("opaque");
  event.allDay = false;
  event.createdAt = QDateTime(QDate(2026, 1, 3), QTime(9, 0), QTimeZone::utc());
  event.updatedAt = event.createdAt;
  return event;
}

Event makeLocalEvent(const QString& calendarId, const QString& id) {
  Event event;
  event.id = id;
  event.calendarId = calendarId;
  event.remoteId = QStringLiteral("");
  event.uid = id.isEmpty() ? QStringLiteral("") : QStringLiteral("local-uid-") + id;
  event.summary = QStringLiteral("Local ") + id;
  event.description = QStringLiteral("");
  event.location = QStringLiteral("");
  event.etag = QStringLiteral("etag-") + id;
  event.recurrenceRule = QStringLiteral("");
  event.recurrenceId = QStringLiteral("");
  event.rawPayload = QStringLiteral("");
  event.rawFormat = QStringLiteral("");
  event.startUtc = QDateTime(QDate(2026, 3, 1), QTime(10, 0), QTimeZone::utc());
  event.endUtc = event.startUtc.addSecs(7200);
  event.startDate = event.startUtc.date();
  event.endDate = event.endUtc.date();
  event.startTimeZone = QStringLiteral("UTC");
  event.endTimeZone = QStringLiteral("UTC");
  event.status = QStringLiteral("confirmed");
  event.transparency = QStringLiteral("opaque");
  event.allDay = false;
  return event;
}

void assertOwnerOnlyPermissions(const QString& path) {
  const QFileInfo info(path);
  if (!info.exists()) {
    return;
  }
  QVERIFY(info.permission(QFileDevice::ReadOwner));
  QVERIFY(info.permission(QFileDevice::WriteOwner));
  const QFileDevice::Permissions exposed =
      info.permissions() &
      (QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::WriteGroup |
       QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther |
       QFileDevice::ExeOther);
  QCOMPARE(exposed, QFileDevice::Permissions{});
}

}  // namespace

class DatabaseTest final : public QObject {
  Q_OBJECT

 private slots:
  void schemaAndSettings() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QCOMPARE(db.isOpen(), false);

    QString error;
    QVERIFY(db.open(directory.filePath("store.sqlite"), &error));
    QVERIFY(db.isOpen());
    QCOMPARE(error, QString());
    QCOMPARE(db.schemaVersion(), 2);
    const qint64 initialRevision = db.changeRevision();
    QVERIFY(db.setSetting("timezone", QStringLiteral("UTC"), &error));
    QVERIFY(db.changeRevision() > initialRevision);
    QCOMPARE(db.setting("timezone", QStringLiteral("auto"), &error).toString(),
             QStringLiteral("UTC"));
    QCOMPARE(db.setting("missing", 123, &error).toInt(), 123);
  }

  void schema2OpenIsRepeatableAndClean() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("store.sqlite"));

    Database db;
    QString error;
    QVERIFY2(db.open(path, &error), qPrintable(error));
    QCOMPARE(db.schemaVersion(), 2);
    const QList<Account> firstAccounts = db.accounts(&error);
    QCOMPARE(firstAccounts.size(), 1);
    QCOMPARE(firstAccounts.first().id, QStringLiteral("local-account"));

    // Reopening the same database object must remain a clean schema-2 path.
    QVERIFY2(db.open(path, &error), qPrintable(error));
    const QList<Account> secondAccounts = db.accounts(&error);
    QCOMPARE(secondAccounts.size(), 1);
    QCOMPARE(secondAccounts.first().id, QStringLiteral("local-account"));

    // Simulate daemon restart with a fresh process handle.
    Database reopened;
    QVERIFY2(reopened.open(path, &error), qPrintable(error));
    QCOMPARE(reopened.schemaVersion(), 2);
    const QList<Account> thirdAccounts = reopened.accounts(&error);
    QCOMPARE(thirdAccounts.size(), 1);
    QCOMPARE(thirdAccounts.first().provider, ProviderKind::Local);

    QCOMPARE(QDir(directory.path())
                 .entryList({QStringLiteral("*.pre-v2-*.backup")}, QDir::Files)
                 .size(),
             0);
  }

  void schema2RepairsDuplicateUnresolvedConflicts() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("store.sqlite"));

    Database db;
    QString error;
    QVERIFY2(db.open(path, &error), qPrintable(error));
    Account account =
        makeAccount(QStringLiteral("repair-account"), QStringLiteral("Repair"));
    Calendar calendar = makeCalendar(QStringLiteral("repair-calendar"), account.id);
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    QVERIFY2(db.upsertCalendar(calendar, &error), qPrintable(error));

    Event initial = makeRemoteEvent(calendar.id, QStringLiteral("repair-event"), 0);
    QVERIFY2(db.applyRemoteEvent(initial, &error), qPrintable(error));
    Event local = db.event(initial.id, &error);
    local.summary = QStringLiteral("Local repair edit");
    QVERIFY2(
        db.saveLocalEvent(&local, OutboxOperation::Update, &error,
                          QStringLiteral("repair-mutation"), QStringLiteral("series"),
                          QStringLiteral("none"), local.localRevision),
        qPrintable(error));
    const QList<OutboxItem> operations = db.outboxItems(20, &error);
    const auto mutation = std::find_if(
        operations.cbegin(), operations.cend(), [&local](const auto& item) {
          return item.eventId == local.id && item.state != OutboxState::Done;
        });
    QVERIFY(mutation != operations.cend());

    Event remote = initial;
    remote.id.clear();
    remote.etag = QStringLiteral("repair-v2");
    remote.summary = QStringLiteral("Remote repair edit");
    bool conflicted = false;
    QVERIFY2(db.applyRemoteEvent(remote, &error, &conflicted), qPrintable(error));
    QVERIFY(conflicted);
    QCOMPARE(db.conflicts(true, &error).size(), 1);
    db.close();

    const QString connectionName =
        QStringLiteral("duplicate-conflicts-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
      QSqlDatabase raw =
          QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
      raw.setDatabaseName(path);
      QVERIFY(raw.open());
      QSqlQuery query(raw);
      QVERIFY(
          query.exec(QStringLiteral("DROP INDEX conflicts_unresolved_event_index")));
      query.prepare(QStringLiteral(R"SQL(
        INSERT INTO conflicts
          (event_id,mutation_id,kind,local_revision,remote_revision,
           local_snapshot_json,remote_snapshot_json,remote_payload,state,
           resolution_revision,created_at,resolved_at)
        SELECT event_id,?,kind,local_revision,remote_revision,
               local_snapshot_json,remote_snapshot_json,remote_payload,
               'unresolved',0,?,''
        FROM conflicts WHERE event_id=? ORDER BY id LIMIT 1
      )SQL"));
      query.addBindValue(mutation->id);
      query.addBindValue(QStringLiteral("2030-01-01T00:00:00.000Z"));
      query.addBindValue(local.id);
      QVERIFY2(query.exec(), qPrintable(query.lastError().text()));

      // This is newer, but the repair must still prefer the durable
      // mutation-linked row over a pull-only row.
      query.prepare(QStringLiteral(R"SQL(
        INSERT INTO conflicts
          (event_id,mutation_id,kind,local_revision,remote_revision,
           local_snapshot_json,remote_snapshot_json,remote_payload,state,
           resolution_revision,created_at,resolved_at)
        SELECT event_id,NULL,kind,local_revision,remote_revision,
               local_snapshot_json,remote_snapshot_json,remote_payload,
               'unresolved',0,?,''
        FROM conflicts WHERE event_id=? ORDER BY id LIMIT 1
      )SQL"));
      query.addBindValue(QStringLiteral("2031-01-01T00:00:00.000Z"));
      query.addBindValue(local.id);
      QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
      raw.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    QVERIFY2(db.open(path, &error), qPrintable(error));
    const QList<Conflict> unresolved = db.conflicts(true, &error);
    QCOMPARE(unresolved.size(), 1);
    QCOMPARE(unresolved.first().eventId, local.id);
    QCOMPARE(unresolved.first().mutationId, mutation->id);

    const QList<Conflict> history = db.conflicts(false, &error);
    QCOMPARE(history.size(), 3);
    QCOMPARE(std::count_if(history.cbegin(), history.cend(),
                           [](const Conflict& item) {
                             return item.state == QStringLiteral("superseded");
                           }),
             2);
    for (const Conflict& item : history) {
      if (item.state == QStringLiteral("superseded")) {
        QVERIFY(item.resolvedAt.isValid());
        QVERIFY(item.resolutionRevision > 0);
      }
    }

    const QString verifyConnectionName =
        QStringLiteral("verify-conflict-index-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
      QSqlDatabase verify =
          QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), verifyConnectionName);
      verify.setDatabaseName(path);
      QVERIFY(verify.open());
      QSqlQuery query(verify);
      QVERIFY(
          query.exec(QStringLiteral("SELECT sql FROM sqlite_master WHERE type='index' "
                                    "AND name='conflicts_unresolved_event_index'")));
      QVERIFY(query.next());
      QVERIFY(query.value(0).toString().contains(QStringLiteral("state='unresolved'")));
      verify.close();
    }
    QSqlDatabase::removeDatabase(verifyConnectionName);

    // The repair is safe to run on every schema-2 startup.
    QVERIFY2(db.open(path, &error), qPrintable(error));
    QCOMPARE(db.conflicts(true, &error).size(), 1);
    QCOMPARE(db.conflicts(false, &error).size(), 3);
  }

  void schema2RepairsOrphanCoveredByNewerResolutionOnly() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("store.sqlite"));

    Database db;
    QString error;
    QVERIFY2(db.open(path, &error), qPrintable(error));
    Account account = makeAccount(QStringLiteral("orphan-repair-account"),
                                  QStringLiteral("Orphan repair"));
    Calendar calendar =
        makeCalendar(QStringLiteral("orphan-repair-calendar"), account.id);
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    QVERIFY2(db.upsertCalendar(calendar, &error), qPrintable(error));

    const auto createPullConflict = [&db, &calendar, &error](
                                        const QString& id, const QString& mutationId) {
      Event initial = makeRemoteEvent(calendar.id, id, 0);
      if (!db.applyRemoteEvent(initial, &error)) {
        return Event{};
      }
      Event local = db.event(initial.id, &error);
      local.summary = QStringLiteral("Local ") + id;
      if (!db.saveLocalEvent(&local, OutboxOperation::Update, &error, mutationId,
                             QStringLiteral("series"), QStringLiteral("none"),
                             local.localRevision)) {
        return Event{};
      }
      Event remote = initial;
      remote.id.clear();
      remote.etag = QStringLiteral("remote-v2-") + id;
      remote.summary = QStringLiteral("Remote ") + id;
      bool conflicted = false;
      if (!db.applyRemoteEvent(remote, &error, &conflicted) || !conflicted) {
        return Event{};
      }
      return local;
    };

    const Event coveredOrphan = createPullConflict(QStringLiteral("covered-orphan"),
                                                   QStringLiteral("covered-mutation"));
    QVERIFY2(!coveredOrphan.id.isEmpty(), qPrintable(error));
    const Event genuineNewer = createPullConflict(QStringLiteral("genuine-newer"),
                                                  QStringLiteral("newer-mutation"));
    QVERIFY2(!genuineNewer.id.isEmpty(), qPrintable(error));

    const QList<OutboxItem> operations = db.outboxItems(20, &error);
    const auto mutationFor = [&operations](const QString& eventId) {
      const auto mutation = std::find_if(
          operations.cbegin(), operations.cend(), [&eventId](const OutboxItem& item) {
            return item.eventId == eventId && item.state != OutboxState::Done;
          });
      return mutation == operations.cend() ? qint64{0} : mutation->id;
    };
    const qint64 coveredMutationId = mutationFor(coveredOrphan.id);
    const qint64 newerMutationId = mutationFor(genuineNewer.id);
    QVERIFY(coveredMutationId > 0);
    QVERIFY(newerMutationId > 0);
    db.close();

    const QString connectionName =
        QStringLiteral("resolved-orphan-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
      QSqlDatabase raw =
          QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
      raw.setDatabaseName(path);
      QVERIFY(raw.open());
      QSqlQuery query(raw);
      QVERIFY(
          query.exec(QStringLiteral("DROP INDEX conflicts_unresolved_event_index")));

      // Exact legacy ordering: an unlinked pull conflict remains unresolved,
      // then a newer mutation-linked duplicate is resolved by the user.
      query.prepare(QStringLiteral(R"SQL(
        INSERT INTO conflicts
          (event_id,mutation_id,kind,local_revision,remote_revision,
           local_snapshot_json,remote_snapshot_json,remote_payload,state,
           resolution_revision,created_at,resolved_at)
        SELECT event_id,?,kind,local_revision,remote_revision,
               local_snapshot_json,remote_snapshot_json,remote_payload,
               'keep_remote',77,?,?
        FROM conflicts WHERE event_id=? AND state='unresolved' LIMIT 1
      )SQL"));
      query.addBindValue(coveredMutationId);
      query.addBindValue(QStringLiteral("2030-01-01T00:00:00.000Z"));
      query.addBindValue(QStringLiteral("2030-01-01T00:01:00.000Z"));
      query.addBindValue(coveredOrphan.id);
      QVERIFY2(query.exec(), qPrintable(query.lastError().text()));

      // A historical resolution must not hide a genuinely newer conflict for
      // the same event. Its created_at precedes the unresolved row even though
      // its auto-increment id is larger.
      query.prepare(QStringLiteral(R"SQL(
        INSERT INTO conflicts
          (event_id,mutation_id,kind,local_revision,remote_revision,
           local_snapshot_json,remote_snapshot_json,remote_payload,state,
           resolution_revision,created_at,resolved_at)
        SELECT event_id,?,kind,local_revision,remote_revision,
               local_snapshot_json,remote_snapshot_json,remote_payload,
               'keep_local',55,?,?
        FROM conflicts WHERE event_id=? AND state='unresolved' LIMIT 1
      )SQL"));
      query.addBindValue(newerMutationId);
      query.addBindValue(QStringLiteral("2020-01-01T00:00:00.000Z"));
      query.addBindValue(QStringLiteral("2020-01-01T00:01:00.000Z"));
      query.addBindValue(genuineNewer.id);
      QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
      raw.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    QVERIFY2(db.open(path, &error), qPrintable(error));
    const QList<Conflict> unresolved = db.conflicts(true, &error);
    QCOMPARE(unresolved.size(), 1);
    QCOMPARE(unresolved.first().eventId, genuineNewer.id);

    const QList<Conflict> history = db.conflicts(false, &error);
    const auto covered = std::find_if(
        history.cbegin(), history.cend(), [&coveredOrphan](const auto& item) {
          return item.eventId == coveredOrphan.id && item.mutationId == 0;
        });
    QVERIFY(covered != history.cend());
    QCOMPARE(covered->state, QStringLiteral("superseded"));
    QVERIFY(covered->resolvedAt.isValid());
    QVERIFY(std::any_of(history.cbegin(), history.cend(),
                        [&coveredOrphan](const Conflict& item) {
                          return item.eventId == coveredOrphan.id &&
                                 item.state == QStringLiteral("keep_remote");
                        }));
    QVERIFY(std::any_of(history.cbegin(), history.cend(),
                        [&genuineNewer](const Conflict& item) {
                          return item.eventId == genuineNewer.id &&
                                 item.state == QStringLiteral("keep_local");
                        }));

    // Reopening is idempotent and leaves the genuine newer conflict visible.
    QVERIFY2(db.open(path, &error), qPrintable(error));
    QCOMPARE(db.conflicts(true, &error).size(), 1);
    QCOMPARE(db.conflicts(true, &error).first().eventId, genuineNewer.id);
  }

  void archivesSchemaOneBeforeCleanStart() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("legacy.sqlite"));
    const QString savedWal = path + QStringLiteral(".test-wal-snapshot");
    const QString savedShm = path + QStringLiteral(".test-shm-snapshot");
    const QString connection =
        QStringLiteral("legacy-%1").arg(QUuid::createUuid().toString());
    {
      QSqlDatabase legacy =
          QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
      legacy.setDatabaseName(path);
      QVERIFY(legacy.open());
      {
        QSqlQuery query(legacy);
        QVERIFY(query.exec(QStringLiteral("PRAGMA journal_mode=WAL")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString().toLower(), QStringLiteral("wal"));
        QVERIFY(query.exec(QStringLiteral("PRAGMA wal_autocheckpoint=0")));
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE legacy_events(id TEXT PRIMARY KEY, title TEXT)")));
        QVERIFY(query.exec(
            QStringLiteral("INSERT INTO legacy_events VALUES('one','Preserve me')")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version=1")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA wal_checkpoint(FULL)")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 0);
        QVERIFY(QFileInfo::exists(path + QStringLiteral("-wal")));
        QVERIFY(QFileInfo::exists(path + QStringLiteral("-shm")));
        QVERIFY(QFile::copy(path + QStringLiteral("-wal"), savedWal));
        QVERIFY(QFile::copy(path + QStringLiteral("-shm"), savedShm));
      }
      legacy.close();
    }
    QSqlDatabase::removeDatabase(connection);
    // SQLite normally removes clean WAL state with the last connection. Restore
    // valid checkpointed sidecars so the transition has a complete file set to
    // archive, as it would after an interrupted daemon shutdown.
    QFile::remove(path + QStringLiteral("-wal"));
    QFile::remove(path + QStringLiteral("-shm"));
    QVERIFY(QFile::rename(savedWal, path + QStringLiteral("-wal")));
    QVERIFY(QFile::rename(savedShm, path + QStringLiteral("-shm")));

    Database db;
    QString error;
    QVERIFY2(db.open(path, &error), qPrintable(error));
    QCOMPARE(db.schemaVersion(), 2);
    const QList<Account> accounts = db.accounts(&error);
    QCOMPARE(accounts.size(), 1);
    QCOMPARE(accounts.first().provider, ProviderKind::Local);
    const QStringList backups =
        QDir(directory.path())
            .entryList({QStringLiteral("legacy.sqlite.pre-v2-*.backup")}, QDir::Files);
    QCOMPARE(backups.size(), 1);
    const QFileInfo backup(directory.filePath(backups.first()));
    QVERIFY(QFileInfo(path).exists());
    QVERIFY(QFileInfo::exists(backup.filePath() + QStringLiteral("-wal")));
    QVERIFY(QFileInfo::exists(backup.filePath() + QStringLiteral("-shm")));
    assertOwnerOnlyPermissions(backup.filePath());
    assertOwnerOnlyPermissions(backup.filePath() + QStringLiteral("-wal"));
    assertOwnerOnlyPermissions(backup.filePath() + QStringLiteral("-shm"));
    const QString verifyConnection =
        QStringLiteral("legacy-verify-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
      QSqlDatabase verify =
          QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), verifyConnection);
      verify.setDatabaseName(backup.filePath());
      QVERIFY(verify.open());
      QSqlQuery verifyQuery(verify);
      QVERIFY(verifyQuery.exec(QStringLiteral("PRAGMA user_version")));
      QVERIFY(verifyQuery.next());
      QCOMPARE(verifyQuery.value(0).toInt(), 1);
      QVERIFY(verifyQuery.exec(
          QStringLiteral("SELECT title FROM legacy_events WHERE id='one'")));
      QVERIFY(verifyQuery.next());
      QCOMPARE(verifyQuery.value(0).toString(), QStringLiteral("Preserve me"));
      verify.close();
    }
    QSqlDatabase::removeDatabase(verifyConnection);
    const QList<Account> cleanAccounts = db.accounts(&error);
    QCOMPARE(cleanAccounts.size(), 1);
    QCOMPARE(cleanAccounts.first().provider, ProviderKind::Local);
    QVERIFY2(db.open(path, &error), qPrintable(error));
    QCOMPARE(
        QDir(directory.path())
            .entryList({QStringLiteral("legacy.sqlite.pre-v2-*.backup")}, QDir::Files)
            .size(),
        1);
    QVERIFY(!QFileInfo::exists(path + QStringLiteral(".schema2-transition")));
    assertOwnerOnlyPermissions(path);
    assertOwnerOnlyPermissions(path + QStringLiteral("-wal"));
    assertOwnerOnlyPermissions(path + QStringLiteral("-shm"));
  }

  void permissionsAreSetForSchema2Databases() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("store.sqlite"));
    Database db;
    QString error;
    QVERIFY2(db.open(path, &error), qPrintable(error));
    const Account account = makeAccount("perm-account", "Perms");
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    Event local = makeLocalEvent(QStringLiteral("local-default"),
                                 QStringLiteral("permission-test"));
    QVERIFY2(db.saveLocalEvent(&local, OutboxOperation::Create, &error),
             qPrintable(error));
    db.close();
    assertOwnerOnlyPermissions(path);
    assertOwnerOnlyPermissions(path + QStringLiteral("-wal"));
    assertOwnerOnlyPermissions(path + QStringLiteral("-shm"));
  }

  void blockedSchema2InitializationKeepsLegacyDatabaseRecoverable() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("legacy.sqlite"));
    const QString stagingPath = path + QStringLiteral(".schema2-transition");
    const QString connection =
        QStringLiteral("legacy-failure-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
      QSqlDatabase legacy =
          QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
      legacy.setDatabaseName(path);
      QVERIFY(legacy.open());
      {
        QSqlQuery query(legacy);
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE legacy_events(id TEXT PRIMARY KEY, title TEXT)")));
        QVERIFY(query.exec(
            QStringLiteral("INSERT INTO legacy_events VALUES('one','Keep me')")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = 1")));
      }
      legacy.close();
    }
    QSqlDatabase::removeDatabase(connection);
    QVERIFY(QDir().mkpath(stagingPath));

    Database db;
    QString error;
    QVERIFY(!db.open(path, &error));
    QVERIFY(!db.isOpen());
    QVERIFY(!error.isEmpty());
    QCOMPARE(
        QDir(directory.path())
            .entryList({QStringLiteral("legacy.sqlite.pre-v2-*.backup")}, QDir::Files)
            .size(),
        1);
    QVERIFY(QFileInfo::exists(path));
    const QString verifyConnection =
        QStringLiteral("legacy-failure-check-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
      QSqlDatabase verify =
          QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), verifyConnection);
      verify.setDatabaseName(path);
      QVERIFY(verify.open());
      QSqlQuery verifyQuery(verify);
      QVERIFY(verifyQuery.exec(QStringLiteral("PRAGMA user_version")));
      QVERIFY(verifyQuery.next());
      QCOMPARE(verifyQuery.value(0).toInt(), 1);
      QVERIFY(verifyQuery.exec(
          QStringLiteral("SELECT title FROM legacy_events WHERE id='one'")));
      QVERIFY(verifyQuery.next());
      QCOMPARE(verifyQuery.value(0).toString(), QStringLiteral("Keep me"));
      verify.close();
    }
    QSqlDatabase::removeDatabase(verifyConnection);

    // Removing the external obstruction makes the same preserved schema-1
    // database retryable; the failed attempt's timestamped archive remains.
    QVERIFY(QDir().rmdir(stagingPath));
    error.clear();
    QVERIFY2(db.open(path, &error), qPrintable(error));
    QCOMPARE(db.schemaVersion(), 2);
    QCOMPARE(
        QDir(directory.path())
            .entryList({QStringLiteral("legacy.sqlite.pre-v2-*.backup")}, QDir::Files)
            .size(),
        2);
  }

  void newerSchemaIsRejectedWithoutArchival() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("future.sqlite"));
    const QString connection =
        QStringLiteral("future-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
      QSqlDatabase future =
          QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
      future.setDatabaseName(path);
      QVERIFY(future.open());
      {
        QSqlQuery query(future);
        QVERIFY(query.exec(QStringLiteral(
            "CREATE TABLE legacy_events(id TEXT PRIMARY KEY, title TEXT)")));
        QVERIFY(query.exec(
            QStringLiteral("INSERT INTO legacy_events VALUES('one','Keep me')")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = 3")));
      }
      future.close();
    }
    QSqlDatabase::removeDatabase(connection);

    Database db;
    QString error;
    QVERIFY(!db.open(path, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(
        QDir(directory.path())
            .entryList({QStringLiteral("future.sqlite.pre-v2-*.backup")}, QDir::Files)
            .size(),
        0);
    const QString verifyConnection =
        QStringLiteral("future-check-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
      QSqlDatabase verify =
          QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), verifyConnection);
      verify.setDatabaseName(path);
      QVERIFY(verify.open());
      QSqlQuery verifyQuery(verify);
      QVERIFY(verifyQuery.exec(QStringLiteral("PRAGMA user_version")));
      QVERIFY(verifyQuery.next());
      QCOMPARE(verifyQuery.value(0).toInt(), 3);
      QVERIFY(verifyQuery.exec(
          QStringLiteral("SELECT title FROM legacy_events WHERE id='one'")));
      QVERIFY(verifyQuery.next());
      QCOMPARE(verifyQuery.value(0).toString(), QStringLiteral("Keep me"));
      verify.close();
    }
    QSqlDatabase::removeDatabase(verifyConnection);
  }

  void accountAndCalendarCrud() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QVERIFY(db.open(directory.filePath("store.sqlite")));

    Account google = makeAccount("acc-google", "Google");
    Account caldav = makeAccount("acc-caldav", "Caldav");
    QString error;
    QVERIFY(db.upsertAccount(google, &error));
    QVERIFY(db.upsertAccount(caldav, &error));
    const QList<Account> accounts = db.accounts(&error);
    QCOMPARE(accounts.size(), 3);
    QVERIFY(std::any_of(accounts.cbegin(), accounts.cend(), [](const Account& account) {
      return account.provider == ProviderKind::Local &&
             account.id == QStringLiteral("local-account");
    }));

    Calendar gcal = makeCalendar("cal-google", google.id);
    Calendar ccal = makeCalendar("cal-caldav", caldav.id);
    QVERIFY(db.upsertCalendar(gcal, &error));
    QVERIFY(db.upsertCalendar(ccal, &error));

    const QList<Calendar> gcalendars = db.calendars(google.id, &error);
    const QList<Calendar> ccalendars = db.calendars(caldav.id, &error);
    QCOMPARE(gcalendars.size(), 1);
    QCOMPARE(ccalendars.size(), 1);
    QCOMPARE(gcalendars.first().accountId, google.id);
    QCOMPARE(ccalendars.first().id, ccal.id);

    const QList<CalendarSet> sets = db.calendarSets(&error);
    QCOMPARE(sets.size(), 1);
    QVERIFY(sets.first().calendarIds.contains(QStringLiteral("local-default")));
    QVERIFY(sets.first().calendarIds.contains(gcal.id));
    QVERIFY(sets.first().calendarIds.contains(ccal.id));

    QCOMPARE(db.removeAccount(google.id, &error), true);
    const QList<Calendar> nowForGoogle = db.calendars(google.id, &error);
    QVERIFY(nowForGoogle.isEmpty());
    const QList<Calendar> all = db.calendars({}, &error);
    // The seeded device-only calendar remains alongside the CalDAV fixture.
    QCOMPARE(all.size(), 2);
  }

  void localCalendarRemovalCascadesTransactionally() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QString error;
    QVERIFY2(db.open(directory.filePath("store.sqlite"), &error), qPrintable(error));

    Calendar local = makeCalendar("local-removable", "local-account");
    local.remoteId.clear();
    local.href.clear();
    local.etag.clear();
    local.syncToken.clear();
    QVERIFY2(db.upsertCalendar(local, &error), qPrintable(error));

    Event retained = makeLocalEvent(local.id, "retained");
    QVERIFY2(db.saveLocalEvent(&retained, OutboxOperation::Create, &error),
             qPrintable(error));
    Event pendingRemoval = makeLocalEvent(local.id, "pending-remove");
    QVERIFY2(db.saveLocalEvent(&pendingRemoval, OutboxOperation::Create, &error),
             qPrintable(error));
    const qint64 expectedRevision = pendingRemoval.localRevision;
    pendingRemoval.deleted = true;
    QVERIFY2(db.saveLocalEvent(&pendingRemoval, OutboxOperation::Remove, &error,
                               QStringLiteral("local-delete"), QStringLiteral("series"),
                               QStringLiteral("none"), expectedRevision),
             qPrintable(error));
    const QList<OutboxItem> queuedOperations = db.outboxItems();
    QVERIFY(std::any_of(
        queuedOperations.cbegin(), queuedOperations.cend(),
        [&local](const OutboxItem& item) { return item.calendarId == local.id; }));

    const qint64 beforeRejectedRemoval = db.changeRevision();
    QVERIFY(!db.removeLocalCalendar(QStringLiteral("local-default"), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(db.changeRevision(), beforeRejectedRemoval);

    Account remote = makeAccount("remote-owner", "Remote");
    QVERIFY2(db.upsertAccount(remote, &error), qPrintable(error));
    Calendar remoteCalendar = makeCalendar("remote-calendar", remote.id);
    QVERIFY2(db.upsertCalendar(remoteCalendar, &error), qPrintable(error));
    const qint64 beforeRemoteRejection = db.changeRevision();
    QVERIFY(!db.removeLocalCalendar(remoteCalendar.id, &error));
    QCOMPARE(db.changeRevision(), beforeRemoteRejection);

    const qint64 beforeRemoval = db.changeRevision();
    QVERIFY2(db.removeLocalCalendar(local.id, &error), qPrintable(error));
    QVERIFY(db.changeRevision() > beforeRemoval);
    QVERIFY(db.calendar(local.id).id.isEmpty());
    QVERIFY(db.event(retained.id).id.isEmpty());
    QVERIFY(db.event(pendingRemoval.id).id.isEmpty());
    const QList<OutboxItem> remainingOperations = db.outboxItems();
    QVERIFY(std::none_of(
        remainingOperations.cbegin(), remainingOperations.cend(),
        [&local](const OutboxItem& item) { return item.calendarId == local.id; }));
    const QList<CalendarSet> remainingSets = db.calendarSets(&error);
    QVERIFY(std::none_of(remainingSets.cbegin(), remainingSets.cend(),
                         [&local](const CalendarSet& set) {
                           return set.calendarIds.contains(local.id);
                         }));
  }

  void eventsOutboxFlow() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QVERIFY(db.open(directory.filePath("store.sqlite")));
    QString error;

    Account account = makeAccount("acc-events", "Events");
    QVERIFY(db.upsertAccount(account, &error));
    Calendar cal = makeCalendar("cal-events", account.id);
    QVERIFY(db.upsertCalendar(cal, &error));

    Event remote1 = makeRemoteEvent(cal.id, "remote-1", 0);
    Event remote2 = makeRemoteEvent(cal.id, "remote-2", 3);
    QVERIFY2(db.applyRemoteEvent(remote1, &error),
             qPrintable(error.isEmpty() ? QStringLiteral("missing error") : error));
    QVERIFY2(db.applyRemoteEvent(remote2, &error),
             qPrintable(error.isEmpty() ? QStringLiteral("missing error") : error));

    const QDateTime queryStart(QDate(2026, 2, 1), QTime(7, 30), QTimeZone::utc());
    const QDateTime queryEnd(QDate(2026, 2, 1), QTime(12, 0), QTimeZone::utc());
    const QList<Event> overlap = db.eventsBetween(queryStart, queryEnd, {}, &error);
    QCOMPARE(overlap.size(), 2);

    Event allDay = makeRemoteEvent(cal.id, "all-day", 0);
    allDay.summary = QStringLiteral("All Day");
    allDay.allDay = true;
    allDay.startUtc = QDateTime(QDate(2026, 2, 1), QTime(0, 0), QTimeZone::utc());
    allDay.endUtc = QDateTime(QDate(2026, 2, 2), QTime(0, 0), QTimeZone::utc());
    allDay.startDate = QDate(2026, 2, 1);
    allDay.endDate = QDate(2026, 2, 2);
    QVERIFY2(db.applyRemoteEvent(allDay, &error),
             qPrintable(error.isEmpty() ? QStringLiteral("missing error") : error));

    const QList<Event> allDayOverlap = db.eventsBetween(
        QDateTime(QDate(2026, 2, 1), QTime(0, 0), QTimeZone::utc()),
        QDateTime(QDate(2026, 2, 3), QTime(0, 0), QTimeZone::utc()), {cal.id}, &error);
    QCOMPARE(allDayOverlap.size(), 3);

    QList<Event> outOfRange = db.eventsBetween(queryEnd, queryStart, {}, &error);
    QVERIFY(outOfRange.isEmpty());
    QVERIFY(!error.isEmpty());

    Event localOne = makeLocalEvent(cal.id, QString());
    QVERIFY2(db.saveLocalEvent(&localOne, OutboxOperation::Create, &error),
             qPrintable(error.isEmpty() ? QStringLiteral("missing error") : error));

    Event localTwo = makeLocalEvent(cal.id, QString());
    localTwo.summary = QStringLiteral("Local Two");
    QVERIFY2(db.saveLocalEvent(&localTwo, OutboxOperation::Create, &error),
             qPrintable(error.isEmpty() ? QStringLiteral("missing error") : error));

    QList<OutboxItem> readyAll = db.readyOutbox(100, &error);
    QCOMPARE(readyAll.size(), 2);
    const OutboxItem first = readyAll.first();
    QCOMPARE(first.operation, OutboxOperation::Create);
    QCOMPARE(first.state, OutboxState::Pending);
    QVERIFY2(db.updateOutboxState(first.id, OutboxState::Sending, 1,
                                  QDateTime::currentDateTimeUtc(), QStringLiteral(""),
                                  QStringLiteral(""), &error),
             qPrintable(error.isEmpty() ? QStringLiteral("missing error") : error));

    QList<OutboxItem> readyLimit = db.readyOutbox(1, &error);
    QCOMPARE(readyLimit.size(), 1);
    QCOMPARE(readyLimit.first().id, readyAll[1].id);

    Event removed = localTwo;
    QVERIFY(db.markLocalEventRemoved(removed.id, &error));
    QString lookupError;
    const Event reread = db.event(removed.id, &lookupError);
    QVERIFY(reread.id.isEmpty());
    QVERIFY(!lookupError.isEmpty());

    // A create that has never left the pending queue can be cancelled
    // locally. Neither a tombstone nor a remote Remove operation is needed.
    const QList<OutboxItem> readyAfterCancellation = db.readyOutbox(20, &error);
    QVERIFY(readyAfterCancellation.isEmpty());
  }

  void eventByUidMatchesCanonicalRecurrenceForms() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QString error;
    QVERIFY2(db.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));
    Account account =
        makeAccount(QStringLiteral("acc-identity"), QStringLiteral("Identity"));
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    Calendar calendar = makeCalendar(QStringLiteral("cal-identity"), account.id);
    QVERIFY2(db.upsertCalendar(calendar, &error), qPrintable(error));

    Event master = makeRemoteEvent(calendar.id, QStringLiteral("identity-master"), 0);
    master.uid = QStringLiteral("identity-series");
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=3");
    master.startTimeZone = QStringLiteral("America/New_York");
    QVERIFY2(db.applyRemoteEvent(master, &error), qPrintable(error));

    Event exception = master;
    exception.id = QStringLiteral("identity-exception");
    exception.remoteId = QStringLiteral("remote-identity-exception");
    exception.recurrenceRule.clear();
    exception.recurrenceId = QStringLiteral("TZID=America/New_York:20260904T090000");
    exception.startUtc = QDateTime(QDate(2026, 9, 4), QTime(13, 0), QTimeZone::UTC);
    exception.endUtc = exception.startUtc.addSecs(3600);
    QVERIFY2(db.applyRemoteEvent(exception, &error), qPrintable(error));

    const Event byBasic = db.eventByUid(calendar.id, master.uid,
                                        QStringLiteral("20260904T090000"), &error);
    QCOMPARE(byBasic.id, exception.id);
    const Event byRange = db.eventByUid(
        calendar.id, master.uid,
        QStringLiteral("RANGE=THISANDFUTURE;TZID=America/New_York:2026-09-04T09:00:00"),
        &error);
    QCOMPARE(byRange.id, exception.id);

    Event pulled = exception;
    pulled.id.clear();
    pulled.remoteId.clear();
    pulled.recurrenceId = QStringLiteral("20260904T130000Z");
    pulled.summary = QStringLiteral("Canonical pull refresh");
    QVERIFY2(db.applyRemoteEvent(pulled, &error), qPrintable(error));
    QCOMPARE(db.eventsByUid(calendar.id, master.uid, &error).size(), 2);
    QCOMPARE(db.event(exception.id, &error).summary,
             QStringLiteral("Canonical pull refresh"));
    QVERIFY(db.eventByUid(calendar.id, master.uid,
                          QStringLiteral("TZID=America/New_York:20260905T090000"),
                          &error)
                .id.isEmpty());
  }

  void moveRejectsCanonicalRecurrenceCollision() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QString error;
    QVERIFY2(db.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));

    Account account = makeAccount(QStringLiteral("acc-move-identity"),
                                  QStringLiteral("Move identity"));
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    Calendar sourceCalendar =
        makeCalendar(QStringLiteral("cal-move-identity-source"), account.id);
    Calendar targetCalendar =
        makeCalendar(QStringLiteral("cal-move-identity-target"), account.id);
    QVERIFY2(db.upsertCalendar(sourceCalendar, &error), qPrintable(error));
    QVERIFY2(db.upsertCalendar(targetCalendar, &error), qPrintable(error));

    Event source =
        makeRemoteEvent(sourceCalendar.id, QStringLiteral("move-identity-source"), 0);
    source.uid = QStringLiteral("move-identity-series");
    source.recurrenceId = QStringLiteral("TZID=America/New_York:20260904T090000");
    source.startTimeZone = QStringLiteral("America/New_York");
    source.endTimeZone = QStringLiteral("America/New_York");
    source.startUtc = QDateTime(QDate(2026, 9, 4), QTime(13, 0), QTimeZone::UTC);
    source.endUtc = source.startUtc.addSecs(3600);
    QVERIFY2(db.applyRemoteEvent(source, &error), qPrintable(error));
    source = db.event(source.id, &error);

    Event target = source;
    target.id = QStringLiteral("move-identity-target-event");
    target.remoteId = QStringLiteral("remote-move-identity-target-event");
    target.calendarId = targetCalendar.id;
    target.recurrenceId = QStringLiteral("20260904T130000Z");
    target.etag = QStringLiteral("etag-move-identity-target-event");
    QVERIFY2(db.applyRemoteEvent(target, &error), qPrintable(error));

    Event requestedMove = source;
    requestedMove.calendarId = targetCalendar.id;
    QVERIFY(!db.moveLocalEvent(
        &requestedMove, &error, QStringLiteral("move-identity-collision"),
        QStringLiteral("series"), QStringLiteral("none"), source.localRevision));
    QVERIFY2(!error.isEmpty(), qPrintable(error));
    QCOMPARE(db.event(source.id, &error).calendarId, sourceCalendar.id);
    QVERIFY(db.outboxItems(20, &error).isEmpty());
  }

  void explicitRemoteIdentityKeepsLocalRevisionMonotonic() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QString error;
    QVERIFY2(db.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));

    Account account =
        makeAccount(QStringLiteral("acc-revisions"), QStringLiteral("Revisions"));
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    Calendar calendar = makeCalendar(QStringLiteral("cal-revisions"), account.id);
    QVERIFY2(db.upsertCalendar(calendar, &error), qPrintable(error));

    Event remote = makeRemoteEvent(calendar.id, QStringLiteral("stable-row"), 0);
    remote.localRevision = 0;
    QVERIFY2(db.applyRemoteEvent(remote, &error), qPrintable(error));
    const Event first = db.event(remote.id, &error);
    QVERIFY2(first.localRevision > 0, qPrintable(error));

    Event refreshed = remote;
    refreshed.summary = QStringLiteral("Refreshed remotely");
    refreshed.etag = QStringLiteral("etag-stable-row-v2");
    refreshed.localRevision = 0;
    QVERIFY2(db.applyRemoteEvent(refreshed, &error), qPrintable(error));
    const Event second = db.event(remote.id, &error);
    QVERIFY(second.localRevision > first.localRevision);

    Event local = second;
    local.summary = QStringLiteral("Edited locally");
    QVERIFY2(db.saveLocalEvent(&local, OutboxOperation::Update, &error,
                               QStringLiteral("revision-ack"), QStringLiteral("series"),
                               QStringLiteral("none"), second.localRevision),
             qPrintable(error));
    const qint64 optimisticRevision = local.localRevision;
    QVERIFY(optimisticRevision > second.localRevision);
    const QList<OutboxItem> ready = db.readyOutbox(10, &error);
    QCOMPARE(ready.size(), 1);

    Event acknowledged = local;
    acknowledged.etag = QStringLiteral("etag-stable-row-v3");
    QVERIFY2(db.completeOutbox(ready.first().id, &acknowledged, &error),
             qPrintable(error));
    const Event afterAcknowledgement = db.event(remote.id, &error);
    QVERIFY(afterAcknowledgement.localRevision >= optimisticRevision);

    // CalDAV follows a successful write by parsing the acknowledged VCALENDAR
    // and applying that canonical remote snapshot with the known local row ID.
    // A parser-produced revision of zero must never reset the optimistic row.
    Event parsedAcknowledgement = refreshed;
    parsedAcknowledgement.id = remote.id;
    parsedAcknowledgement.summary = local.summary;
    parsedAcknowledgement.etag = acknowledged.etag;
    parsedAcknowledgement.localRevision = 0;
    QVERIFY2(db.applyRemoteEvent(parsedAcknowledgement, &error), qPrintable(error));
    const Event final = db.event(remote.id, &error);
    QVERIFY(final.localRevision > afterAcknowledgement.localRevision);
    QCOMPARE(final.summary, local.summary);
    QVERIFY(!final.dirty);
  }

  void mutationRecoveryUndoConflictAndReminders() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QString error;
    QVERIFY2(db.open(directory.filePath("store.sqlite"), &error), qPrintable(error));

    Account account = makeAccount("acc-durable", "Durable");
    QVERIFY(db.upsertAccount(account, &error));
    Calendar calendar = makeCalendar("cal-durable", account.id);
    QVERIFY(db.upsertCalendar(calendar, &error));

    Event pending = makeLocalEvent(calendar.id, QString());
    pending.summary = QStringLiteral("Crash recovery");
    QVERIFY(db.saveLocalEvent(&pending, OutboxOperation::Create, &error,
                              QStringLiteral("mutation-crash")));
    QList<OutboxItem> ready = db.readyOutbox(10, &error);
    QCOMPARE(ready.size(), 1);
    QVERIFY(db.updateOutboxState(ready.first().id, OutboxState::Sending, 1, QDateTime(),
                                 {}, {}, &error));
    QVERIFY(db.readyOutbox(10, &error).isEmpty());
    QVERIFY(db.recoverExpiredOutbox(&error));
    ready = db.readyOutbox(10, &error);
    QCOMPARE(ready.size(), 1);
    QCOMPARE(ready.first().state, OutboxState::Pending);
    QCOMPARE(ready.first().errorCode, QStringLiteral("recovered_after_restart"));

    Event acknowledged = pending;
    acknowledged.remoteId = QStringLiteral("remote-pending");
    acknowledged.etag = QStringLiteral("v1");
    QVERIFY(db.completeOutbox(ready.first().id, &acknowledged, &error));

    Event removeCandidate = makeRemoteEvent(calendar.id, "remove-me", 1);
    QVERIFY(db.applyRemoteEvent(removeCandidate, &error));
    QVERIFY(db.markLocalEventRemoved(removeCandidate.id, &error));
    const QList<OutboxItem> operations = db.outboxItems(20, &error);
    const auto removeIt =
        std::find_if(operations.cbegin(), operations.cend(),
                     [&removeCandidate](const OutboxItem& item) {
                       return item.eventId == removeCandidate.id &&
                              item.operation == OutboxOperation::Remove &&
                              item.state != OutboxState::Done;
                     });
    QVERIFY(removeIt != operations.cend());
    QVERIFY(removeIt->notBefore > QDateTime::currentDateTimeUtc());
    error.clear();
    const QList<OutboxItem> delayedReady = db.readyOutbox(20, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(delayedReady.isEmpty());
    QVERIFY(db.discardOutbox(removeIt->id, &error));
    const Event restored = db.event(removeCandidate.id, &error);
    QVERIFY(!restored.deleted);
    QCOMPARE(restored.syncState, QStringLiteral("clean"));

    Event conflicted = makeRemoteEvent(calendar.id, "conflicted", 2);
    QVERIFY(db.applyRemoteEvent(conflicted, &error));
    conflicted = db.event(conflicted.id, &error);
    conflicted.summary = QStringLiteral("Local edit");
    QVERIFY(db.saveLocalEvent(&conflicted, OutboxOperation::Update, &error,
                              QStringLiteral("mutation-conflict"),
                              QStringLiteral("series"), QStringLiteral("none"),
                              conflicted.localRevision));
    Event remoteChanged = conflicted;
    remoteChanged.id.clear();
    remoteChanged.summary = QStringLiteral("Remote edit");
    remoteChanged.etag = QStringLiteral("v2");
    remoteChanged.dirty = false;
    bool didConflict = false;
    QVERIFY(db.applyRemoteEvent(remoteChanged, &error, &didConflict));
    QVERIFY(didConflict);
    const QList<Conflict> conflicts = db.conflicts(true, &error);
    QCOMPARE(conflicts.size(), 1);
    QVERIFY(db.resolveConflict(conflicts.first().id, QStringLiteral("keep_remote"), {},
                               &error));
    QCOMPARE(db.event(conflicted.id, &error).summary, QStringLiteral("Remote edit"));

    Event reminder = makeLocalEvent(QStringLiteral("local-default"), QString());
    reminder.summary = QStringLiteral("Recurring reminder");
    reminder.startUtc = QDateTime::currentDateTimeUtc().addSecs(3600);
    reminder.endUtc = reminder.startUtc.addSecs(1800);
    reminder.startDate = reminder.startUtc.date();
    reminder.endDate = reminder.endUtc.date();
    reminder.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=3");
    reminder.reminders =
        QJsonArray{QJsonObject{{QStringLiteral("method"), QStringLiteral("popup")},
                               {QStringLiteral("minutes"), 15}}};
    QVERIFY(db.saveLocalEvent(&reminder, OutboxOperation::Create, &error));
    const QList<ReminderJob> reminders = db.reminders(20, &error);
    QCOMPARE(reminders.size(), 3);
    for (const ReminderJob& job : reminders) {
      QVERIFY(!job.occurrenceId.isEmpty());
      const QDateTime occurrence = dateTimeFromIso(job.occurrenceId);
      QVERIFY(occurrence.isValid());
      QCOMPARE(job.fireAt, occurrence.addSecs(-15 * 60));
    }
  }

  void pullAndProviderConflictsConvergePerEvent() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QString error;
    QVERIFY2(db.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));

    Account account = makeAccount(QStringLiteral("converge-account"),
                                  QStringLiteral("Conflict convergence"));
    Calendar calendar = makeCalendar(QStringLiteral("converge-calendar"), account.id);
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    QVERIFY2(db.upsertCalendar(calendar, &error), qPrintable(error));

    Event initial = makeRemoteEvent(calendar.id, QStringLiteral("pull-then-412"), 0);
    initial.rawPayload = QStringLiteral("remote-v1");
    QVERIFY2(db.applyRemoteEvent(initial, &error), qPrintable(error));
    Event local = db.event(initial.id, &error);
    local.summary = QStringLiteral("Local pending edit");
    QVERIFY2(db.saveLocalEvent(&local, OutboxOperation::Update, &error,
                               QStringLiteral("pull-then-412-mutation"),
                               QStringLiteral("series"), QStringLiteral("none"),
                               local.localRevision),
             qPrintable(error));
    const QList<OutboxItem> operations = db.outboxItems(20, &error);
    const auto mutation = std::find_if(
        operations.cbegin(), operations.cend(), [&local](const auto& item) {
          return item.eventId == local.id && item.state != OutboxState::Done;
        });
    QVERIFY(mutation != operations.cend());

    Event remote = initial;
    remote.id.clear();
    remote.etag = QStringLiteral("remote-v2");
    remote.summary = QStringLiteral("Remote concurrent edit");
    remote.rawPayload = QStringLiteral("remote-v2-payload");
    for (int repeat = 0; repeat < 2; ++repeat) {
      bool conflicted = false;
      QVERIFY2(db.applyRemoteEvent(remote, &error, &conflicted), qPrintable(error));
      QVERIFY(conflicted);
      const QList<Conflict> pullConflicts = db.conflicts(true, &error);
      QCOMPARE(pullConflicts.size(), 1);
      QCOMPARE(pullConflicts.first().eventId, local.id);
      QCOMPARE(pullConflicts.first().mutationId, 0);
    }

    // The provider's stale precondition result upgrades the pull-discovered
    // row instead of creating a second user-visible conflict.
    QVERIFY2(db.recordProviderConflict(mutation->id, &remote, &error),
             qPrintable(error));
    QList<Conflict> unresolved = db.conflicts(true, &error);
    QCOMPARE(unresolved.size(), 1);
    QCOMPARE(unresolved.first().eventId, local.id);
    QCOMPARE(unresolved.first().mutationId, mutation->id);
    QCOMPARE(
        unresolved.first().remoteSnapshot.value(QStringLiteral("summary")).toString(),
        remote.summary);

    // A later pull cannot erase the mutation association, and a repeated 412
    // remains idempotent.
    bool conflicted = false;
    QVERIFY2(db.applyRemoteEvent(remote, &error, &conflicted), qPrintable(error));
    QVERIFY(conflicted);
    QVERIFY2(db.recordProviderConflict(mutation->id, &remote, &error),
             qPrintable(error));
    unresolved = db.conflicts(true, &error);
    QCOMPARE(unresolved.size(), 1);
    QCOMPARE(unresolved.first().mutationId, mutation->id);

    Event deleted = makeRemoteEvent(calendar.id, QStringLiteral("repeated-delete"), 1);
    QVERIFY2(db.applyRemoteEvent(deleted, &error), qPrintable(error));
    Event dirtyDeleted = db.event(deleted.id, &error);
    dirtyDeleted.summary = QStringLiteral("Local edit before remote delete");
    QVERIFY2(db.saveLocalEvent(&dirtyDeleted, OutboxOperation::Update, &error,
                               QStringLiteral("repeated-delete-mutation"),
                               QStringLiteral("series"), QStringLiteral("none"),
                               dirtyDeleted.localRevision),
             qPrintable(error));
    for (int repeat = 0; repeat < 2; ++repeat) {
      bool deleteConflict = false;
      QVERIFY2(db.removeRemoteEvent(calendar.id, deleted.remoteId,
                                    QStringLiteral("remote-delete"), &error,
                                    &deleteConflict),
               qPrintable(error));
      QVERIFY(deleteConflict);
    }
    unresolved = db.conflicts(true, &error);
    QCOMPARE(std::count_if(unresolved.cbegin(), unresolved.cend(),
                           [&dirtyDeleted](const Conflict& item) {
                             return item.eventId == dirtyDeleted.id;
                           }),
             1);
  }

  void newestConflictResolutionUsesProviderAndLocalTimestamps() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QString error;
    QVERIFY2(db.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));
    Account account =
        makeAccount(QStringLiteral("newest-account"), QStringLiteral("Newest update"));
    Calendar calendar = makeCalendar(QStringLiteral("newest-calendar"), account.id);
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    QVERIFY2(db.upsertCalendar(calendar, &error), qPrintable(error));

    Event remoteWins = makeRemoteEvent(calendar.id, QStringLiteral("remote-wins"), 0);
    QVERIFY2(db.applyRemoteEvent(remoteWins, &error), qPrintable(error));
    Event local = db.event(remoteWins.id, &error);
    local.summary = QStringLiteral("Older local edit");
    QVERIFY2(db.saveLocalEvent(&local, OutboxOperation::Update, &error,
                               QStringLiteral("remote-wins-mutation"),
                               QStringLiteral("series"), QStringLiteral("none"),
                               local.localRevision),
             qPrintable(error));
    local = db.event(remoteWins.id, &error);
    Event newerRemote = remoteWins;
    newerRemote.id.clear();
    newerRemote.etag = QStringLiteral("remote-wins-v2");
    newerRemote.summary = QStringLiteral("Newer remote edit");
    newerRemote.updatedAt = local.updatedAt.addSecs(60);
    bool conflicted = false;
    QVERIFY2(db.applyRemoteEvent(newerRemote, &error, &conflicted), qPrintable(error));
    QVERIFY(conflicted);
    bool queuedLocalWrites = false;
    QCOMPARE(db.resolveNewestConflicts(&queuedLocalWrites, &error), 1);
    QVERIFY(!queuedLocalWrites);
    QCOMPARE(db.event(remoteWins.id, &error).summary,
             QStringLiteral("Newer remote edit"));
    QVERIFY(db.conflicts(true, &error).isEmpty());

    Event localWins = makeRemoteEvent(calendar.id, QStringLiteral("local-wins"), 1);
    QVERIFY2(db.applyRemoteEvent(localWins, &error), qPrintable(error));
    local = db.event(localWins.id, &error);
    local.summary = QStringLiteral("Newer local edit");
    QVERIFY2(db.saveLocalEvent(&local, OutboxOperation::Update, &error,
                               QStringLiteral("local-wins-mutation"),
                               QStringLiteral("series"), QStringLiteral("none"),
                               local.localRevision),
             qPrintable(error));
    local = db.event(localWins.id, &error);
    Event olderRemote = localWins;
    olderRemote.id.clear();
    olderRemote.etag = QStringLiteral("local-wins-v2");
    olderRemote.summary = QStringLiteral("Older remote edit");
    olderRemote.updatedAt = local.updatedAt.addSecs(-60);
    conflicted = false;
    QVERIFY2(db.applyRemoteEvent(olderRemote, &error, &conflicted), qPrintable(error));
    QVERIFY(conflicted);
    queuedLocalWrites = false;
    QCOMPARE(db.resolveNewestConflicts(&queuedLocalWrites, &error), 1);
    QVERIFY(queuedLocalWrites);
    QCOMPARE(db.event(localWins.id, &error).summary,
             QStringLiteral("Newer local edit"));
    QVERIFY(db.conflicts(true, &error).isEmpty());
  }

  void providerConflictsResolveAtomically() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("store.sqlite"));
    Database db;
    QString error;
    QVERIFY2(db.open(databasePath, &error), qPrintable(error));

    Account account = makeAccount(QStringLiteral("acc-provider-conflicts"),
                                  QStringLiteral("Provider conflicts"));
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    Calendar calendar =
        makeCalendar(QStringLiteral("cal-provider-conflicts"), account.id);
    QVERIFY2(db.upsertCalendar(calendar, &error), qPrintable(error));

    const auto executeExternally = [&databasePath](const QString& statement) {
      const QString connectionName =
          QStringLiteral("conflict-trigger-%1")
              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
      bool succeeded = false;
      {
        QSqlDatabase connection =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        connection.setDatabaseName(databasePath);
        if (connection.open()) {
          QSqlQuery query(connection);
          succeeded = query.exec(statement);
          connection.close();
        }
      }
      QSqlDatabase::removeDatabase(connectionName);
      return succeeded;
    };

    struct ResolutionCase {
      QString name;
      QString strategy;
      bool remoteDeleted = false;
    };
    const QList<ResolutionCase> cases = {
        {QStringLiteral("keep-remote"), QStringLiteral("keep_remote"), false},
        {QStringLiteral("keep-local"), QStringLiteral("keep_local"), false},
        {QStringLiteral("merge"), QStringLiteral("merge"), false},
        {QStringLiteral("recreate-deleted"), QStringLiteral("keep_local"), true},
    };

    for (qsizetype index = 0; index < cases.size(); ++index) {
      const ResolutionCase& resolution = cases.at(index);
      Event initial = makeRemoteEvent(
          calendar.id, QStringLiteral("atomic-%1").arg(index), static_cast<int>(index));
      initial.rawPayload = QStringLiteral("BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n");
      QVERIFY2(db.applyRemoteEvent(initial, &error), qPrintable(error));

      Event local = db.event(initial.id, &error);
      local.summary = QStringLiteral("Local %1").arg(resolution.name);
      const qint64 expectedRevision = local.localRevision;
      QVERIFY2(db.saveLocalEvent(&local, OutboxOperation::Update, &error,
                                 QStringLiteral("atomic-mutation-%1").arg(index),
                                 QStringLiteral("series"), QStringLiteral("none"),
                                 expectedRevision),
               qPrintable(error));
      const QList<OutboxItem> queued = db.outboxItems(100, &error);
      const auto queuedMutation = std::find_if(
          queued.cbegin(), queued.cend(), [&local](const OutboxItem& item) {
            return item.eventId == local.id && item.state != OutboxState::Done;
          });
      QVERIFY(queuedMutation != queued.cend());

      Event remote = initial;
      remote.id = local.id;
      remote.summary = QStringLiteral("Remote %1").arg(resolution.name);
      remote.etag = QStringLiteral("remote-v2-%1").arg(index);
      remote.rawPayload =
          QStringLiteral("BEGIN:VCALENDAR\r\nX-REMOTE:%1\r\nEND:VCALENDAR\r\n")
              .arg(index);
      error.clear();
      QVERIFY2(
          db.recordProviderConflict(
              queuedMutation->id, resolution.remoteDeleted ? nullptr : &remote, &error),
          qPrintable(error));

      const QList<Conflict> unresolved = db.conflicts(true, &error);
      const auto conflict = std::find_if(
          unresolved.cbegin(), unresolved.cend(),
          [&local](const Conflict& item) { return item.eventId == local.id; });
      QVERIFY(conflict != unresolved.cend());
      QCOMPARE(conflict->mutationId, queuedMutation->id);
      QCOMPARE(conflict->localSnapshot.value(QStringLiteral("summary")).toString(),
               local.summary);
      if (resolution.remoteDeleted) {
        QCOMPARE(conflict->kind, QStringLiteral("remote_deleted"));
        QVERIFY(conflict->remoteSnapshot.isEmpty());
      } else {
        QCOMPARE(conflict->kind, QStringLiteral("remote_changed"));
        QCOMPARE(conflict->remoteSnapshot.value(QStringLiteral("summary")).toString(),
                 remote.summary);
      }
      const QList<OutboxItem> blocked = db.outboxItems(100, &error);
      const auto blockedMutation = std::find_if(
          blocked.cbegin(), blocked.cend(), [queuedMutation](const OutboxItem& item) {
            return item.id == queuedMutation->id;
          });
      QVERIFY(blockedMutation != blocked.cend());
      QCOMPARE(blockedMutation->state, OutboxState::Blocked);
      QCOMPARE(db.event(local.id, &error).syncState, QStringLiteral("conflict"));

      // Simulate an already-open legacy schema-2 database that still contains
      // the historical pull/provider duplicate. Resolution must be defensive
      // even before the next startup repair runs.
      QVERIFY(executeExternally(
          QStringLiteral("DROP INDEX conflicts_unresolved_event_index")));
      QVERIFY(executeExternally(QStringLiteral(R"SQL(
        INSERT INTO conflicts
          (event_id,mutation_id,kind,local_revision,remote_revision,
           local_snapshot_json,remote_snapshot_json,remote_payload,state,
           resolution_revision,created_at,resolved_at)
        SELECT event_id,NULL,kind,local_revision,remote_revision,
               local_snapshot_json,remote_snapshot_json,remote_payload,
               'unresolved',0,created_at,'' FROM conflicts WHERE id=%1
      )SQL")
                                    .arg(conflict->id)));
      const QList<Conflict> duplicates = db.conflicts(true, &error);
      QCOMPARE(std::count_if(
                   duplicates.cbegin(), duplicates.cend(),
                   [&local](const Conflict& item) { return item.eventId == local.id; }),
               2);

      const QString triggerName = QStringLiteral("reject_resolution_%1").arg(index);
      QVERIFY(executeExternally(
          QStringLiteral("CREATE TRIGGER %1 BEFORE UPDATE ON events WHEN OLD.id='%2' "
                         "BEGIN SELECT RAISE(ABORT,'forced resolution failure'); END")
              .arg(triggerName, local.id)));
      QJsonObject mergeDraft = conflict->localSnapshot;
      mergeDraft.insert(QStringLiteral("summary"),
                        QStringLiteral("Merged %1").arg(resolution.name));
      error.clear();
      QVERIFY(
          !db.resolveConflict(conflict->id, resolution.strategy, mergeDraft, &error));
      QVERIFY(!error.isEmpty());

      const QList<Conflict> afterRollback = db.conflicts(true, &error);
      QCOMPARE(std::count_if(
                   afterRollback.cbegin(), afterRollback.cend(),
                   [&local](const Conflict& item) { return item.eventId == local.id; }),
               2);
      const QList<OutboxItem> afterRollbackOutbox = db.outboxItems(100, &error);
      const auto preservedMutation =
          std::find_if(afterRollbackOutbox.cbegin(), afterRollbackOutbox.cend(),
                       [queuedMutation](const OutboxItem& item) {
                         return item.id == queuedMutation->id;
                       });
      QVERIFY(preservedMutation != afterRollbackOutbox.cend());
      QCOMPARE(preservedMutation->state, OutboxState::Blocked);
      QCOMPARE(db.event(local.id, &error).summary, local.summary);

      QVERIFY(executeExternally(QStringLiteral("DROP TRIGGER %1").arg(triggerName)));
      error.clear();
      QVERIFY2(
          db.resolveConflict(conflict->id, resolution.strategy, mergeDraft, &error),
          qPrintable(error));
      const QList<Conflict> resolvedConflicts = db.conflicts(true, &error);
      QVERIFY(std::none_of(
          resolvedConflicts.cbegin(), resolvedConflicts.cend(),
          [&local](const Conflict& item) { return item.eventId == local.id; }));
      QVERIFY(executeExternally(QStringLiteral(R"SQL(
        CREATE UNIQUE INDEX conflicts_unresolved_event_index
        ON conflicts(event_id) WHERE state='unresolved'
      )SQL")));

      const Event resolved = db.event(local.id, &error);
      if (resolution.strategy == QStringLiteral("keep_remote")) {
        QCOMPARE(resolved.summary, remote.summary);
        QCOMPARE(resolved.syncState, QStringLiteral("clean"));
      } else if (resolution.strategy == QStringLiteral("merge")) {
        QCOMPARE(resolved.summary,
                 mergeDraft.value(QStringLiteral("summary")).toString());
      } else {
        QCOMPARE(resolved.summary, local.summary);
      }
      if (resolution.remoteDeleted) {
        QVERIFY(resolved.remoteId.isEmpty());
        const QList<OutboxItem> recreated = db.outboxItems(100, &error);
        QVERIFY(std::any_of(recreated.cbegin(), recreated.cend(),
                            [&local](const OutboxItem& item) {
                              return item.eventId == local.id &&
                                     item.operation == OutboxOperation::Create &&
                                     item.state == OutboxState::Pending;
                            }));
      }
    }
  }

  void crossAccountMoveQueueIsAtomicAndDependencyOrdered() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("store.sqlite"));
    Database db;
    QString error;
    QVERIFY2(db.open(path, &error), qPrintable(error));

    Account targetAccount =
        makeAccount(QStringLiteral("cross-target-account"), QStringLiteral("Target"));
    QVERIFY2(db.upsertAccount(targetAccount, &error), qPrintable(error));
    Calendar targetCalendar =
        makeCalendar(QStringLiteral("cross-target-calendar"), targetAccount.id);
    QVERIFY2(db.upsertCalendar(targetCalendar, &error), qPrintable(error));

    Event source =
        makeLocalEvent(QStringLiteral("local-default"), QStringLiteral("cross-source"));
    QVERIFY2(db.saveLocalEvent(&source, OutboxOperation::Create, &error),
             qPrintable(error));
    Event destination = source;
    destination.calendarId = targetCalendar.id;
    QVERIFY2(db.moveEventAcrossAccounts(source, &destination, &error,
                                        QStringLiteral("cross-move"),
                                        QStringLiteral("series"),
                                        QStringLiteral("none"), source.localRevision),
             qPrintable(error));
    QVERIFY(destination.id != source.id);
    const QList<OutboxItem> operations = db.outboxItems(20, &error);
    const auto destinationOperation = std::find_if(
        operations.cbegin(), operations.cend(), [](const OutboxItem& item) {
          return item.idempotencyKey == QStringLiteral("cross-move:destination");
        });
    const auto sourceOperation = std::find_if(
        operations.cbegin(), operations.cend(), [](const OutboxItem& item) {
          return item.idempotencyKey == QStringLiteral("cross-move:source");
        });
    QVERIFY(destinationOperation != operations.cend());
    QVERIFY(sourceOperation != operations.cend());
    QCOMPARE(sourceOperation->dependencyId, QString::number(destinationOperation->id));
    QVERIFY(db.event(source.id, &error).deleted);

    // A retried IPC request reuses the durable destination rather than
    // creating another copy.
    Event retried = source;
    retried.calendarId = targetCalendar.id;
    QVERIFY2(db.moveEventAcrossAccounts(source, &retried, &error,
                                        QStringLiteral("cross-move"),
                                        QStringLiteral("series"),
                                        QStringLiteral("none"), source.localRevision),
             qPrintable(error));
    QCOMPARE(retried.id, destination.id);

    // Force the second half of a different move to fail. The outer savepoint
    // must roll back both the destination row and its create operation.
    Event rollbackSource = makeLocalEvent(QStringLiteral("local-default"),
                                          QStringLiteral("cross-rollback-source"));
    QVERIFY2(db.saveLocalEvent(&rollbackSource, OutboxOperation::Create, &error),
             qPrintable(error));
    const QString connectionName =
        QStringLiteral("cross-move-trigger-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
      QSqlDatabase raw =
          QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
      raw.setDatabaseName(path);
      QVERIFY(raw.open());
      QSqlQuery trigger(raw);
      QVERIFY2(trigger.exec(QStringLiteral(R"SQL(
        CREATE TRIGGER fail_cross_move_source BEFORE INSERT ON outbox
        WHEN new.idempotency_key='cross-rollback:source'
        BEGIN SELECT RAISE(ABORT, 'forced cross move failure'); END
      )SQL")),
               qPrintable(trigger.lastError().text()));
      raw.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    Event rolledBackDestination = rollbackSource;
    rolledBackDestination.calendarId = targetCalendar.id;
    rolledBackDestination.id.clear();
    QVERIFY(!db.moveEventAcrossAccounts(
        rollbackSource, &rolledBackDestination, &error,
        QStringLiteral("cross-rollback"), QStringLiteral("series"),
        QStringLiteral("none"), rollbackSource.localRevision));
    QVERIFY(db.event(rolledBackDestination.id).id.isEmpty());
    const QList<OutboxItem> afterRollback = db.outboxItems(50, &error);
    QVERIFY(std::none_of(
        afterRollback.cbegin(), afterRollback.cend(), [](const OutboxItem& item) {
          return item.idempotencyKey.startsWith(QStringLiteral("cross-rollback:"));
        }));
    QVERIFY(!db.event(rollbackSource.id, &error).deleted);
  }

  void filteredSearchPaginatesAfterAllFilters() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QString error;
    QVERIFY2(db.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
             qPrintable(error));

    Account account =
        makeAccount(QStringLiteral("search-account"), QStringLiteral("Search"));
    Account otherAccount = makeAccount(QStringLiteral("search-other-account"),
                                       QStringLiteral("Search other"));
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    QVERIFY2(db.upsertAccount(otherAccount, &error), qPrintable(error));
    Calendar calendar = makeCalendar(QStringLiteral("search-calendar"), account.id);
    Calendar otherCalendar =
        makeCalendar(QStringLiteral("search-other-calendar"), otherAccount.id);
    QVERIFY2(db.upsertCalendar(calendar, &error), qPrintable(error));
    QVERIFY2(db.upsertCalendar(otherCalendar, &error), qPrintable(error));

    const auto saveMatch = [&db, &error](const QString& calendarId, const QString& id,
                                         const QDateTime& start,
                                         const QString& responseStatus,
                                         const QString& partstat = {}) {
      Event event = makeRemoteEvent(calendarId, id, 0);
      event.summary = QStringLiteral("Filtered needle ") + id;
      event.startUtc = start;
      event.endUtc = start.addSecs(3600);
      event.startDate = start.date();
      event.endDate = event.endUtc.date();
      QJsonObject attendee{
          {QStringLiteral("email"), QStringLiteral("user@example.com")}};
      if (!responseStatus.isEmpty()) {
        attendee.insert(QStringLiteral("responseStatus"), responseStatus);
      }
      if (!partstat.isEmpty()) {
        attendee.insert(QStringLiteral("partstat"), partstat);
      }
      event.attendees = QJsonArray{attendee};
      return db.applyRemoteEvent(event, &error);
    };
    QVERIFY2(saveMatch(calendar.id, QStringLiteral("search-out-of-range"),
                       QDateTime(QDate(2026, 9, 10), QTime(12, 0), QTimeZone::UTC),
                       QStringLiteral("accepted")),
             qPrintable(error));
    QVERIFY2(saveMatch(calendar.id, QStringLiteral("search-declined"),
                       QDateTime(QDate(2026, 9, 8), QTime(16, 0), QTimeZone::UTC),
                       QStringLiteral("declined")),
             qPrintable(error));
    QVERIFY2(saveMatch(calendar.id, QStringLiteral("search-newer-valid"),
                       QDateTime(QDate(2026, 9, 8), QTime(12, 0), QTimeZone::UTC),
                       QStringLiteral("accepted")),
             qPrintable(error));
    QVERIFY2(saveMatch(calendar.id, QStringLiteral("search-older-valid"),
                       QDateTime(QDate(2026, 9, 7), QTime(12, 0), QTimeZone::UTC), {},
                       QStringLiteral("ACCEPTED")),
             qPrintable(error));
    QVERIFY2(saveMatch(otherCalendar.id, QStringLiteral("search-other-account-match"),
                       QDateTime(QDate(2026, 9, 8), QTime(18, 0), QTimeZone::UTC),
                       QStringLiteral("accepted")),
             qPrintable(error));

    EventSearchQuery search;
    search.text = QStringLiteral("Filtered needle");
    search.calendarIds = {calendar.id, otherCalendar.id};
    search.accountId = account.id;
    search.startUtc = QDateTime(QDate(2026, 9, 1), QTime(0, 0), QTimeZone::UTC);
    search.endUtc = QDateTime(QDate(2026, 9, 9), QTime(0, 0), QTimeZone::UTC);
    search.invitationState = QStringLiteral("accepted");
    search.limit = 1;

    EventSearchPage first = db.searchEvents(search, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(first.total, 2);
    QCOMPARE(first.events.size(), 1);
    QCOMPARE(first.events.front().id, QStringLiteral("search-newer-valid"));

    search.offset = 1;
    EventSearchPage second = db.searchEvents(search, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(second.total, 2);
    QCOMPARE(second.events.size(), 1);
    QCOMPARE(second.events.front().id, QStringLiteral("search-older-valid"));
  }

  void sameAccountMoveIsAtomicDurableAndRecoverable() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("store.sqlite"));
    Database db;
    QString error;
    QVERIFY2(db.open(path, &error), qPrintable(error));

    Account account =
        makeAccount(QStringLiteral("move-account"), QStringLiteral("Move account"));
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    Calendar sourceCalendar = makeCalendar(QStringLiteral("move-source"), account.id);
    Calendar targetCalendar = makeCalendar(QStringLiteral("move-target"), account.id);
    QVERIFY2(db.upsertCalendar(sourceCalendar, &error), qPrintable(error));
    QVERIFY2(db.upsertCalendar(targetCalendar, &error), qPrintable(error));

    Event source = makeRemoteEvent(sourceCalendar.id, QStringLiteral("move-event"), 0);
    source.rawFormat = QStringLiteral("google-json");
    source.rawPayload = QStringLiteral("{\"id\":\"remote-move-event\"}");
    QVERIFY2(db.applyRemoteEvent(source, &error), qPrintable(error));
    source = db.event(source.id, &error);
    const qint64 beforeMoveRevision = db.changeRevision();

    Event moved = source;
    moved.calendarId = targetCalendar.id;
    QVERIFY2(db.moveLocalEvent(&moved, &error, QStringLiteral("move-mutation"),
                               QStringLiteral("series"), QStringLiteral("none"),
                               source.localRevision),
             qPrintable(error));
    QVERIFY(db.changeRevision() > beforeMoveRevision);
    QCOMPARE(moved.id, source.id);
    QCOMPARE(moved.calendarId, targetCalendar.id);
    QCOMPARE(moved.remoteId, source.remoteId);
    QVERIFY(moved.dirty);
    QCOMPARE(moved.syncState, QStringLiteral("pending"));

    QList<OutboxItem> operations = db.outboxItems(20, &error);
    const auto moveIt = std::find_if(
        operations.cbegin(), operations.cend(), [](const OutboxItem& item) {
          return item.idempotencyKey == QStringLiteral("move-mutation");
        });
    QVERIFY(moveIt != operations.cend());
    QCOMPARE(moveIt->operation, OutboxOperation::Move);
    QCOMPARE(moveIt->accountId, account.id);
    QCOMPARE(moveIt->calendarId, targetCalendar.id);
    QCOMPARE(moveIt->expectedRevision, source.etag);
    QCOMPARE(moveIt->payload.value(QStringLiteral("_move"))
                 .toObject()
                 .value(QStringLiteral("sourceCalendarId"))
                 .toString(),
             sourceCalendar.id);
    QVERIFY(!toJson(*moveIt).contains(QStringLiteral("payload")));
    QVERIFY(!toJson(*moveIt).contains(QStringLiteral("expectedRevision")));

    // A provider pull that runs before outbox drain must not recreate the
    // source row while the optimistic move owns its remote identity.
    Event pulledSource = source;
    pulledSource.id.clear();
    QVERIFY2(db.applyRemoteEvent(pulledSource, &error), qPrintable(error));
    QVERIFY(db.eventsForCalendars({sourceCalendar.id}, &error).isEmpty());
    QCOMPARE(db.event(source.id, &error).calendarId, targetCalendar.id);

    QVERIFY2(
        db.updateOutboxState(moveIt->id, OutboxState::Sending, 1, {}, {}, {}, &error),
        qPrintable(error));
    db.close();
    QVERIFY2(db.open(path, &error), qPrintable(error));
    QVERIFY2(db.recoverExpiredOutbox(&error), qPrintable(error));
    const QList<OutboxItem> recovered = db.readyOutbox(20, &error);
    const auto recoveredMove =
        std::find_if(recovered.cbegin(), recovered.cend(), [](const OutboxItem& item) {
          return item.idempotencyKey == QStringLiteral("move-mutation");
        });
    QVERIFY(recoveredMove != recovered.cend());
    QCOMPARE(recoveredMove->state, OutboxState::Pending);
    QCOMPARE(recoveredMove->errorCode, QStringLiteral("recovered_after_restart"));

    // A definitive provider failure can be discarded without losing either
    // the event or its source provider identity.
    QVERIFY2(db.discardOutbox(recoveredMove->id, &error), qPrintable(error));
    Event restored = db.event(source.id, &error);
    QCOMPARE(restored.calendarId, sourceCalendar.id);
    QCOMPARE(restored.remoteId, source.remoteId);
    QCOMPARE(restored.rawPayload, source.rawPayload);
    QVERIFY(!restored.dirty);
    QCOMPARE(restored.syncState, QStringLiteral("clean"));

    Event acknowledgedMove = restored;
    acknowledgedMove.calendarId = targetCalendar.id;
    QVERIFY2(
        db.moveLocalEvent(&acknowledgedMove, &error,
                          QStringLiteral("move-mutation-ack"), QStringLiteral("series"),
                          QStringLiteral("none"), restored.localRevision),
        qPrintable(error));
    operations = db.outboxItems(20, &error);
    const auto ackIt = std::find_if(
        operations.cbegin(), operations.cend(), [](const OutboxItem& item) {
          return item.idempotencyKey == QStringLiteral("move-mutation-ack");
        });
    QVERIFY(ackIt != operations.cend());
    Event remoteAck = acknowledgedMove;
    remoteAck.etag = QStringLiteral("move-etag-v2");
    remoteAck.rawPayload = QStringLiteral("{\"id\":\"remote-move-event\"}");
    QVERIFY2(db.completeOutbox(ackIt->id, &remoteAck, &error), qPrintable(error));
    const Event acknowledged = db.event(source.id, &error);
    QCOMPARE(acknowledged.calendarId, targetCalendar.id);
    QCOMPARE(acknowledged.etag, QStringLiteral("move-etag-v2"));
    QVERIFY(!acknowledged.dirty);

    Calendar localTarget = makeCalendar(QStringLiteral("local-move-target"),
                                        QStringLiteral("local-account"));
    localTarget.remoteId.clear();
    localTarget.href.clear();
    QVERIFY2(db.upsertCalendar(localTarget, &error), qPrintable(error));
    Event local =
        makeLocalEvent(QStringLiteral("local-default"), QStringLiteral("local-move"));
    QVERIFY2(db.saveLocalEvent(&local, OutboxOperation::Create, &error),
             qPrintable(error));
    local.calendarId = localTarget.id;
    QVERIFY2(db.moveLocalEvent(&local, &error, QStringLiteral("local-move-id"),
                               QStringLiteral("series"), QStringLiteral("none"),
                               local.localRevision),
             qPrintable(error));
    QCOMPARE(db.event(local.id, &error).calendarId, localTarget.id);
    QVERIFY(!db.event(local.id, &error).dirty);
    const QList<OutboxItem> afterLocalMove = db.outboxItems(100, &error);
    QVERIFY(std::none_of(
        afterLocalMove.cbegin(), afterLocalMove.cend(), [](const OutboxItem& item) {
          return item.idempotencyKey == QStringLiteral("local-move-id");
        }));
  }

  void remoteSyncBatchIsAtomic() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QString error;
    QVERIFY(db.open(directory.filePath(QStringLiteral("store.sqlite")), &error));
    Account account = makeAccount(QStringLiteral("acc-batch"), QStringLiteral("Batch"));
    QVERIFY(db.upsertAccount(account, &error));
    Calendar calendar = makeCalendar(QStringLiteral("cal-batch"), account.id);
    calendar.syncToken = QStringLiteral("old-token");
    QVERIFY(db.upsertCalendar(calendar, &error));
    Event retained = makeRemoteEvent(calendar.id, QStringLiteral("retained"), 0);
    Event pruned = makeRemoteEvent(calendar.id, QStringLiteral("pruned"), 2);
    QVERIFY(db.applyRemoteEvent(retained, &error));
    QVERIFY(db.applyRemoteEvent(pruned, &error));

    Calendar replacement = calendar;
    replacement.syncToken = QStringLiteral("new-token");
    Event staged = makeRemoteEvent(calendar.id, QStringLiteral("staged"), 4);
    Event invalid =
        makeRemoteEvent(QStringLiteral("other-calendar"), QStringLiteral("invalid"), 5);
    error.clear();
    QVERIFY(!db.applyRemoteSyncBatch(replacement, {staged, invalid}, {},
                                     {pruned.remoteId}, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(db.calendar(calendar.id).syncToken, QStringLiteral("old-token"));
    QVERIFY(db.eventByRemoteId(calendar.id, staged.remoteId).id.isEmpty());
    QVERIFY(!db.eventByRemoteId(calendar.id, pruned.remoteId).id.isEmpty());

    error.clear();
    QVERIFY2(
        db.applyRemoteSyncBatch(replacement, {staged}, {}, {pruned.remoteId}, &error),
        qPrintable(error));
    QCOMPARE(db.calendar(calendar.id).syncToken, QStringLiteral("new-token"));
    QVERIFY(!db.eventByRemoteId(calendar.id, staged.remoteId).id.isEmpty());
    QVERIFY(db.eventByRemoteId(calendar.id, pruned.remoteId).id.isEmpty());
  }

  void scopedAcknowledgementAndCanonicalRangeAreAtomic() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("range-ack.sqlite"));
    Database db;
    QString error;
    QVERIFY2(db.open(path, &error), qPrintable(error));

    Account account =
        makeAccount(QStringLiteral("acc-range-ack"), QStringLiteral("Range ack"));
    account.provider = ProviderKind::CalDav;
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    Calendar calendar = makeCalendar(QStringLiteral("cal-range-ack"), account.id);
    calendar.capabilities.insert(QStringLiteral("thisAndFuture"), true);
    calendar.capabilities.insert(QStringLiteral("thisAndFutureProven"), true);
    QVERIFY2(db.upsertCalendar(calendar, &error), qPrintable(error));

    Event master = makeRemoteEvent(calendar.id, QStringLiteral("range-ack-master"), 0);
    master.uid = QStringLiteral("range-ack-series");
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=5");
    master.rawPayload = QStringLiteral("original-resource");
    master.rawFormat = QStringLiteral("text/calendar");
    QVERIFY2(db.applyRemoteEvent(master, &error), qPrintable(error));

    Event future = master;
    future.id = QStringLiteral("range-ack-exception");
    future.remoteId = master.remoteId + QStringLiteral("#20260203T080000Z");
    future.recurrenceRule.clear();
    future.recurrenceId = QStringLiteral("20260203T080000Z");
    future.summary = QStringLiteral("Changed from here");
    future.startUtc = QDateTime(QDate(2026, 2, 3), QTime(10, 0), QTimeZone::UTC);
    future.endUtc = future.startUtc.addSecs(3600);
    future.createdAt = {};
    future.localRevision = 0;
    QVERIFY2(db.saveLocalEvent(&future, OutboxOperation::Update, &error,
                               QStringLiteral("range-ack-mutation"),
                               QStringLiteral("future"), QStringLiteral("none"), 0),
             qPrintable(error));
    const QList<OutboxItem> ready = db.readyOutbox(10, &error);
    QCOMPARE(ready.size(), 1);
    QVERIFY2(db.updateOutboxState(ready.first().id, OutboxState::Sending, 1, {}, {}, {},
                                  &error),
             qPrintable(error));

    Event canonicalMaster = master;
    canonicalMaster.etag = QStringLiteral("range-etag-v2");
    canonicalMaster.rawPayload = QStringLiteral("canonical-range-resource");
    Event canonicalRange = future;
    canonicalRange.recurrenceId =
        QStringLiteral("RANGE=THISANDFUTURE:20260203T080000Z");
    canonicalRange.remoteId =
        master.remoteId + QLatin1Char('#') + canonicalRange.recurrenceId;
    canonicalRange.etag = canonicalMaster.etag;
    canonicalRange.rawPayload = canonicalMaster.rawPayload;
    canonicalRange.rawFormat = QStringLiteral("text/calendar");
    canonicalRange.dirty = false;
    canonicalRange.syncState = QStringLiteral("clean");

    // This invalid staged sibling represents a failure at the old crash
    // boundary: completion has run inside the transaction, but canonical
    // resource staging cannot finish. Both pieces must roll back together.
    Event invalid = canonicalMaster;
    invalid.calendarId = QStringLiteral("wrong-calendar");
    error.clear();
    QVERIFY(!db.completeOutboxWithRemoteSyncBatch(
        ready.first().id, &canonicalRange, calendar,
        {canonicalMaster, canonicalRange, invalid}, {}, {}, &error));
    QVERIFY(!error.isEmpty());
    const auto pendingAfterFailure = db.outboxItems(10, &error);
    QCOMPARE(pendingAfterFailure.size(), 1);
    QCOMPARE(pendingAfterFailure.first().state, OutboxState::Sending);
    QCOMPARE(db.event(future.id, &error).recurrenceId,
             QStringLiteral("20260203T080000Z"));

    error.clear();
    QVERIFY2(db.completeOutboxWithRemoteSyncBatch(
                 ready.first().id, &canonicalRange, calendar,
                 {canonicalMaster, canonicalRange}, {}, {}, &error),
             qPrintable(error));

    Event futureRemoval = db.event(future.id, &error);
    futureRemoval.deleted = true;
    QVERIFY2(db.saveLocalEvent(&futureRemoval, OutboxOperation::Remove, &error,
                               QStringLiteral("range-remove-mutation"),
                               QStringLiteral("future"), QStringLiteral("none"),
                               futureRemoval.localRevision),
             qPrintable(error));
    const QList<OutboxItem> removalOperations = db.outboxItems(10, &error);
    const auto removalIt = std::find_if(
        removalOperations.cbegin(), removalOperations.cend(),
        [](const OutboxItem& item) {
          return item.idempotencyKey == QStringLiteral("range-remove-mutation");
        });
    QVERIFY(removalIt != removalOperations.cend());
    QVERIFY2(db.updateOutboxState(removalIt->id, OutboxState::Sending, 1, {}, {}, {},
                                  &error),
             qPrintable(error));
    Event canonicalCancellation = canonicalRange;
    canonicalCancellation.status = QStringLiteral("cancelled");
    canonicalCancellation.deleted = false;
    canonicalCancellation.rawPayload = QStringLiteral("canonical-range-cancellation");
    QVERIFY2(db.completeOutboxWithRemoteSyncBatch(
                 removalIt->id, nullptr, calendar,
                 {canonicalMaster, canonicalCancellation}, {}, {}, &error),
             qPrintable(error));
    db.close();

    Database reopened;
    QVERIFY2(reopened.open(path, &error), qPrintable(error));
    const Event persisted = reopened.event(future.id, &error);
    QCOMPARE(persisted.recurrenceId,
             QStringLiteral("RANGE=THISANDFUTURE:20260203T080000Z"));
    QCOMPARE(persisted.rawPayload, QStringLiteral("canonical-range-cancellation"));
    QCOMPARE(persisted.status, QStringLiteral("cancelled"));
    QVERIFY(!persisted.dirty);
    const QList<OutboxItem> completed = reopened.outboxItems(10, &error);
    QCOMPARE(completed.size(), 2);
    QVERIFY(std::all_of(
        completed.cbegin(), completed.cend(),
        [](const OutboxItem& item) { return item.state == OutboxState::Done; }));
    QVERIFY(reopened.calendar(calendar.id, &error)
                .capabilities.value(QStringLiteral("thisAndFutureProven"))
                .toBool());
  }

  void syncCoverageMergesPersistsAndRollsBackWithRangeBatch() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("coverage.sqlite"));
    Database db;
    QString error;
    QVERIFY2(db.open(path, &error), qPrintable(error));
    Account account =
        makeAccount(QStringLiteral("coverage-account"), QStringLiteral("Coverage"));
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    Calendar calendar = makeCalendar(QStringLiteral("coverage-calendar"), account.id);
    QVERIFY2(db.upsertCalendar(calendar, &error), qPrintable(error));

    const QDateTime firstStart(QDate(2020, 1, 1), QTime(0, 0), QTimeZone::UTC);
    const QDateTime boundary(QDate(2021, 1, 1), QTime(0, 0), QTimeZone::UTC);
    const QDateTime secondEnd(QDate(2022, 1, 1), QTime(0, 0), QTimeZone::UTC);
    QVERIFY2(db.recordSyncCoverage(calendar.id, firstStart, boundary, &error),
             qPrintable(error));
    QVERIFY2(db.recordSyncCoverage(calendar.id, boundary, secondEnd, &error),
             qPrintable(error));
    const QList<SyncCoverage> merged = db.syncCoverage(calendar.id, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(merged.size(), 1);
    QCOMPARE(merged.first().startUtc, firstStart);
    QCOMPARE(merged.first().endUtc, secondEnd);
    QVERIFY(db.isSyncRangeCovered(calendar.id, firstStart.addDays(1),
                                  secondEnd.addDays(-1), &error));

    Event previous = makeRemoteEvent(calendar.id, QStringLiteral("coverage-old"), 0);
    QVERIFY2(db.applyRemoteEvent(previous, &error), qPrintable(error));
    Event replacement =
        makeRemoteEvent(calendar.id, QStringLiteral("coverage-replacement"), 1);
    Event invalid = replacement;
    invalid.calendarId = QStringLiteral("wrong-calendar");
    const QDateTime failedStart(QDate(2010, 1, 1), QTime(0, 0), QTimeZone::UTC);
    const QDateTime failedEnd(QDate(2011, 1, 1), QTime(0, 0), QTimeZone::UTC);
    error.clear();
    QVERIFY(!db.applyRemoteRangeSyncBatch(calendar, {replacement, invalid}, {},
                                          {previous.remoteId}, failedStart, failedEnd,
                                          &error, true));
    QVERIFY(!error.isEmpty());
    error.clear();
    QVERIFY(!db.isSyncRangeCovered(calendar.id, failedStart, failedEnd, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(db.isSyncRangeCovered(calendar.id, firstStart, secondEnd, &error));
    QVERIFY(!db.eventByRemoteId(calendar.id, previous.remoteId).id.isEmpty());
    QVERIFY(db.eventByRemoteId(calendar.id, replacement.remoteId).id.isEmpty());

    error.clear();
    QVERIFY2(
        db.applyRemoteRangeSyncBatch(calendar, {replacement}, {}, {previous.remoteId},
                                     failedStart, failedEnd, &error),
        qPrintable(error));
    QVERIFY(db.isSyncRangeCovered(calendar.id, failedStart, failedEnd, &error));
    QVERIFY(db.eventByRemoteId(calendar.id, previous.remoteId).id.isEmpty());
    QVERIFY(!db.eventByRemoteId(calendar.id, replacement.remoteId).id.isEmpty());

    const QDateTime resetStart(QDate(2025, 1, 1), QTime(0, 0), QTimeZone::UTC);
    const QDateTime resetEnd(QDate(2031, 1, 1), QTime(0, 0), QTimeZone::UTC);
    error.clear();
    QVERIFY2(db.applyRemoteRangeSyncBatch(calendar, {}, {}, {}, resetStart, resetEnd,
                                          &error, true),
             qPrintable(error));
    QVERIFY(db.isSyncRangeCovered(calendar.id, resetStart, resetEnd, &error));
    QVERIFY(!db.isSyncRangeCovered(calendar.id, firstStart, secondEnd, &error));
    QVERIFY(!db.isSyncRangeCovered(calendar.id, failedStart, failedEnd, &error));

    db.close();
    Database reopened;
    QVERIFY2(reopened.open(path, &error), qPrintable(error));
    QVERIFY(reopened.isSyncRangeCovered(calendar.id, resetStart, resetEnd, &error));
    QVERIFY(!reopened.isSyncRangeCovered(calendar.id, firstStart, secondEnd, &error));
  }

  void icsFeedReplacementIsAtomic() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("store.sqlite"));
    Database db;
    QString error;
    QVERIFY2(db.open(databasePath, &error), qPrintable(error));

    Account account =
        makeAccount(QStringLiteral("acc-ics-batch"), QStringLiteral("ICS batch"));
    account.provider = ProviderKind::Ics;
    QVERIFY2(db.upsertAccount(account, &error), qPrintable(error));
    Calendar calendar = makeCalendar(QStringLiteral("cal-ics-batch"), account.id);
    calendar.readOnly = true;
    calendar.etag = QStringLiteral("old-calendar-etag");
    QVERIFY2(db.upsertCalendar(calendar, &error), qPrintable(error));

    IcsSubscription subscription;
    subscription.accountId = account.id;
    subscription.url = QStringLiteral("https://calendar.example.test/feed.ics");
    subscription.etag = QStringLiteral("old-feed-etag");
    subscription.lastModified = QStringLiteral("Wed, 26 Aug 2026 12:00:00 GMT");
    subscription.lastSuccessAt =
        QDateTime(QDate(2026, 8, 26), QTime(12, 0), QTimeZone::utc());
    subscription.lastErrorCode = QStringLiteral("old_error");
    subscription.lastErrorMessage = QStringLiteral("Previous refresh failed");
    QVERIFY2(db.upsertIcsSubscription(subscription, &error), qPrintable(error));

    Event retained = makeRemoteEvent(calendar.id, QStringLiteral("ics-retained"), 0);
    retained.summary = QStringLiteral("Cached retained event");
    Event stale = makeRemoteEvent(calendar.id, QStringLiteral("ics-stale"), 2);
    QVERIFY2(db.applyRemoteEvent(retained, &error), qPrintable(error));
    QVERIFY2(db.applyRemoteEvent(stale, &error), qPrintable(error));
    const qint64 previousRevision = db.changeRevision();

    const auto executeExternally = [&databasePath](const QString& statement) {
      const QString connectionName =
          QStringLiteral("ics-batch-trigger-%1")
              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
      bool succeeded = false;
      {
        QSqlDatabase connection =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        connection.setDatabaseName(databasePath);
        if (connection.open()) {
          QSqlQuery query(connection);
          succeeded = query.exec(statement);
          connection.close();
        }
      }
      QSqlDatabase::removeDatabase(connectionName);
      return succeeded;
    };
    QVERIFY(executeExternally(QStringLiteral(R"SQL(
      CREATE TRIGGER fail_ics_feed_status
      BEFORE UPDATE OF etag,last_modified,last_success_at ON ics_subscriptions
      WHEN NEW.account_id='acc-ics-batch'
      BEGIN
        SELECT RAISE(ABORT, 'injected ICS metadata failure');
      END
    )SQL")));

    Calendar replacement = calendar;
    replacement.etag = QStringLiteral("new-calendar-etag");
    replacement.lastSyncAt =
        QDateTime(QDate(2026, 8, 29), QTime(13, 0), QTimeZone::utc());
    Event updated = retained;
    updated.id.clear();
    updated.summary = QStringLiteral("Replacement retained event");
    updated.etag = QStringLiteral("new-feed-etag");
    Event added = makeRemoteEvent(calendar.id, QStringLiteral("ics-added"), 4);
    added.id.clear();

    error.clear();
    QVERIFY(!db.applyIcsFeedReplacement(replacement, {updated, added}, {stale.remoteId},
                                        QStringLiteral("new-feed-etag"),
                                        QStringLiteral("Sat, 29 Aug 2026 13:00:00 GMT"),
                                        replacement.lastSyncAt, &error));
    QVERIFY(error.contains(QStringLiteral("injected ICS metadata failure")));

    // The failure occurs after the event/calendar batch has run. The outer
    // savepoint must still restore every prior row and the global revision.
    QCOMPARE(db.changeRevision(), previousRevision);
    const Calendar rolledBackCalendar = db.calendar(calendar.id, &error);
    QCOMPARE(rolledBackCalendar.etag, calendar.etag);
    QCOMPARE(rolledBackCalendar.lastSyncAt, calendar.lastSyncAt);
    const IcsSubscription rolledBackSubscription =
        db.icsSubscription(account.id, &error);
    QCOMPARE(rolledBackSubscription.etag, subscription.etag);
    QCOMPARE(rolledBackSubscription.lastModified, subscription.lastModified);
    QCOMPARE(rolledBackSubscription.lastSuccessAt, subscription.lastSuccessAt);
    QCOMPARE(rolledBackSubscription.lastErrorCode, subscription.lastErrorCode);
    QCOMPARE(db.eventByRemoteId(calendar.id, retained.remoteId, &error).summary,
             retained.summary);
    QVERIFY(!db.eventByRemoteId(calendar.id, stale.remoteId, &error).id.isEmpty());
    QVERIFY(db.eventByRemoteId(calendar.id, added.remoteId, &error).id.isEmpty());

    QVERIFY(executeExternally(QStringLiteral("DROP TRIGGER fail_ics_feed_status")));
    error.clear();
    QVERIFY2(db.applyIcsFeedReplacement(replacement, {updated, added}, {stale.remoteId},
                                        QStringLiteral("new-feed-etag"),
                                        QStringLiteral("Sat, 29 Aug 2026 13:00:00 GMT"),
                                        replacement.lastSyncAt, &error),
             qPrintable(error));

    const Calendar committedCalendar = db.calendar(calendar.id, &error);
    QCOMPARE(committedCalendar.etag, replacement.etag);
    QCOMPARE(committedCalendar.lastSyncAt, replacement.lastSyncAt);
    const IcsSubscription committedSubscription =
        db.icsSubscription(account.id, &error);
    QCOMPARE(committedSubscription.etag, QStringLiteral("new-feed-etag"));
    QCOMPARE(committedSubscription.lastModified,
             QStringLiteral("Sat, 29 Aug 2026 13:00:00 GMT"));
    QCOMPARE(committedSubscription.lastSuccessAt, replacement.lastSyncAt);
    QVERIFY(committedSubscription.lastErrorCode.isEmpty());
    QVERIFY(committedSubscription.lastErrorMessage.isEmpty());
    QCOMPARE(db.eventByRemoteId(calendar.id, retained.remoteId, &error).summary,
             updated.summary);
    QVERIFY(db.eventByRemoteId(calendar.id, stale.remoteId, &error).id.isEmpty());
    QVERIFY(!db.eventByRemoteId(calendar.id, added.remoteId, &error).id.isEmpty());
    QVERIFY(db.changeRevision() > previousRevision);
  }

  void timedAllDayAndRangeInstancesPersist() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database db;
    QString error;
    QVERIFY(db.open(directory.filePath(QStringLiteral("store.sqlite")), &error));

    Event timed = makeLocalEvent(QStringLiteral("local-default"), QString());
    timed.summary = QStringLiteral("Timed instance");
    QVERIFY2(db.saveLocalEvent(&timed, OutboxOperation::Create, &error),
             qPrintable(error));
    Event allDay = makeLocalEvent(QStringLiteral("local-default"), QString());
    allDay.summary = QStringLiteral("All-day instance");
    allDay.allDay = true;
    allDay.startUtc = {};
    allDay.endUtc = {};
    allDay.startDate = QDate(2026, 3, 2);
    allDay.endDate = QDate(2026, 3, 3);
    QVERIFY2(db.saveLocalEvent(&allDay, OutboxOperation::Create, &error),
             qPrintable(error));

    Event master = makeRemoteEvent(QStringLiteral("local-default"),
                                   QStringLiteral("range-master"), 0);
    master.remoteId.clear();
    master.uid = QStringLiteral("range-series");
    master.startUtc = QDateTime(QDate(2026, 3, 3), QTime(9, 0), QTimeZone::UTC);
    master.endUtc = master.startUtc.addSecs(3600);
    master.recurrenceRule = QStringLiteral("FREQ=DAILY;COUNT=4");
    QVERIFY(db.applyRemoteEvent(master, &error));
    Event range = master;
    range.id.clear();
    range.recurrenceRule.clear();
    range.recurrenceId = QStringLiteral("RANGE=THISANDFUTURE:20260304T090000Z");
    range.startUtc = QDateTime(QDate(2026, 3, 4), QTime(11, 0), QTimeZone::UTC);
    range.endUtc = range.startUtc.addSecs(5400);
    QVERIFY(db.applyRemoteEvent(range, &error));

    const QList<Event> events =
        db.eventsBetween(QDateTime(QDate(2026, 3, 1), QTime(0, 0), QTimeZone::UTC),
                         QDateTime(QDate(2026, 3, 8), QTime(0, 0), QTimeZone::UTC),
                         {QStringLiteral("local-default")}, &error);
    QVERIFY(std::any_of(events.cbegin(), events.cend(), [](const Event& event) {
      return event.summary == QStringLiteral("Timed instance");
    }));
    QVERIFY(std::any_of(events.cbegin(), events.cend(), [](const Event& event) {
      return event.summary == QStringLiteral("All-day instance");
    }));
    QList<Event> ranged;
    for (const Event& event : events) {
      if (event.uid == QStringLiteral("range-series")) {
        ranged.append(event);
      }
    }
    QCOMPARE(ranged.size(), 4);
    QCOMPARE(ranged.at(0).startUtc.time(), QTime(9, 0));
    QCOMPARE(ranged.at(1).startUtc.time(), QTime(11, 0));
    QCOMPARE(ranged.at(2).startUtc.time(), QTime(11, 0));
    QCOMPARE(ranged.at(3).startUtc.time(), QTime(11, 0));
    QCOMPARE(ranged.at(3).startUtc.secsTo(ranged.at(3).endUtc), 5400);
  }
};

QTEST_MAIN(DatabaseTest)
#include "test_database.moc"
