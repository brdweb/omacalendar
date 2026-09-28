#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <type_traits>

#include "core/database.h"
#include "providers/caldav/caldavsync.h"
#include "providers/google/googlesync.h"
#include "providers/ics/icsservice.h"
#include "sync/chunkedsyncapply.h"
#include "sync/provider.h"
#include "sync/synccoordinator.h"

using namespace omacalendar;

static_assert(std::is_base_of_v<Provider, google::GoogleSync>);
static_assert(std::is_base_of_v<Provider, caldav::CalDavSync>);
static_assert(std::is_base_of_v<Provider, ics::IcsService>);
static_assert(std::is_base_of_v<Provider, LocalProvider>);

namespace {

Event remoteEvent(const QString& calendarId, const int index) {
  Event event;
  event.calendarId = calendarId;
  event.remoteId = QStringLiteral("remote-%1").arg(index);
  event.uid = QStringLiteral("uid-%1@example.test").arg(index);
  event.etag = QStringLiteral("new-etag");
  event.summary = QStringLiteral("Remote event %1").arg(index);
  event.startUtc = QDateTime(QDate(2026, 9, 1), QTime(8, 0), QTimeZone::UTC)
                       .addSecs(index * 3600);
  event.endUtc = event.startUtc.addSecs(1800);
  event.startTimeZone = QStringLiteral("UTC");
  event.endTimeZone = QStringLiteral("UTC");
  event.timeKind = TimeKind::Zoned;
  return event;
}

Calendar remoteCalendar(Database* database, const QString& id) {
  Account account;
  account.id = id + QStringLiteral("-account");
  account.provider = ProviderKind::Google;
  account.displayName = QStringLiteral("Fixture account");
  if (!database->upsertAccount(account)) {
    return {};
  }
  Calendar calendar;
  calendar.id = id;
  calendar.accountId = account.id;
  calendar.remoteId = id;
  calendar.name = QStringLiteral("Fixture calendar");
  calendar.syncToken = QStringLiteral("old-token");
  if (!database->upsertCalendar(calendar)) {
    return {};
  }
  return calendar;
}

}  // namespace

class SyncCoordinatorTest final : public QObject {
  Q_OBJECT

 private slots:
  void allProviderKindsShareOneCoordinator() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(
        database.open(directory.filePath(QStringLiteral("calendar.sqlite3")), &error),
        qPrintable(error));

    google::GoogleSync google(&database);
    caldav::CalDavSync caldav(&database);
    ics::IcsService ics(&database);
    SyncCoordinator coordinator(&database, &google, &caldav, &ics);

    const QJsonObject aggregate = coordinator.status();
    QStringList providerIds = aggregate.keys();
    providerIds.sort();
    QCOMPARE(providerIds,
             QStringList({QStringLiteral("caldav"), QStringLiteral("google"),
                          QStringLiteral("ics"), QStringLiteral("local")}));
    QCOMPARE(coordinator.status(QStringLiteral("local-account"))
                 .value(QStringLiteral("provider"))
                 .toString(),
             QStringLiteral("local"));

    QVERIFY(google.capabilities().createEvent);
    QVERIFY(caldav.capabilities().incrementalSync);
    QVERIFY(!ics.capabilities().createEvent);
  }

  void localMutationsDrainThroughProviderContract() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(
        database.open(directory.filePath(QStringLiteral("calendar.sqlite3")), &error),
        qPrintable(error));

    google::GoogleSync google(&database);
    caldav::CalDavSync caldav(&database);
    ics::IcsService ics(&database);
    SyncCoordinator coordinator(&database, &google, &caldav, &ics);
    QSignalSpy eventsChanged(&coordinator, &SyncCoordinator::eventsChanged);
    QSignalSpy operationsChanged(&coordinator, &SyncCoordinator::operationStateChanged);

    Account remoteAccount;
    remoteAccount.id = QStringLiteral("dependency-account");
    remoteAccount.provider = ProviderKind::CalDav;
    remoteAccount.displayName = QStringLiteral("Dependency account");
    remoteAccount.authStatus = QStringLiteral("connected");
    QVERIFY2(database.upsertAccount(remoteAccount, &error), qPrintable(error));
    Calendar remoteCalendar;
    remoteCalendar.id = QStringLiteral("dependency-calendar");
    remoteCalendar.accountId = remoteAccount.id;
    remoteCalendar.remoteId = QStringLiteral("dependency-calendar");
    remoteCalendar.name = QStringLiteral("Dependency calendar");
    QVERIFY2(database.upsertCalendar(remoteCalendar, &error), qPrintable(error));
    Event prerequisite;
    prerequisite.calendarId = remoteCalendar.id;
    prerequisite.uid = QStringLiteral("prerequisite@example.test");
    prerequisite.summary = QStringLiteral("Acknowledged prerequisite");
    prerequisite.startUtc = QDateTime(QDate(2026, 8, 31), QTime(9, 0), QTimeZone::UTC);
    prerequisite.endUtc = prerequisite.startUtc.addSecs(1800);
    prerequisite.startTimeZone = QStringLiteral("UTC");
    prerequisite.endTimeZone = QStringLiteral("UTC");
    QVERIFY2(database.saveLocalEvent(&prerequisite, OutboxOperation::Create, &error,
                                     QStringLiteral("provider-prerequisite")),
             qPrintable(error));
    QList<OutboxItem> operations = database.outboxItems(10, &error);
    QCOMPARE(operations.size(), 1);
    const qint64 prerequisiteId = operations.first().id;
    QVERIFY2(database.completeOutbox(prerequisiteId, nullptr, &error),
             qPrintable(error));

    Event event;
    event.calendarId = QStringLiteral("local-default");
    event.uid = QStringLiteral("local-provider-contract@example.test");
    event.summary = QStringLiteral("Provider contract");
    event.startUtc = QDateTime(QDate(2026, 9, 1), QTime(9, 0), QTimeZone::UTC);
    event.endUtc = event.startUtc.addSecs(1800);
    event.startTimeZone = QStringLiteral("UTC");
    event.endTimeZone = QStringLiteral("UTC");
    event.timeKind = TimeKind::Zoned;
    event.status = QStringLiteral("confirmed");
    event.transparency = QStringLiteral("opaque");
    QVERIFY2(database.saveLocalEvent(&event, OutboxOperation::Create, &error,
                                     QStringLiteral("local-provider-mutation"),
                                     QStringLiteral("series"), QStringLiteral("none"),
                                     -1, QString::number(prerequisiteId)),
             qPrintable(error));
    QVERIFY(database.event(event.id).dirty);

    QVERIFY2(coordinator.syncAccount(QStringLiteral("local-account"), &error),
             qPrintable(error));

    QCOMPARE(operationsChanged.count(), 1);
    QCOMPARE(eventsChanged.count(), 1);
    QCOMPARE(eventsChanged.takeFirst().at(0).toStringList(),
             QStringList{QStringLiteral("local-default")});
    operations = database.outboxItems(10, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 2);
    const auto localOperation = std::find_if(
        operations.cbegin(), operations.cend(),
        [&event](const OutboxItem& item) { return item.eventId == event.id; });
    QVERIFY(localOperation != operations.cend());
    QCOMPARE(localOperation->state, OutboxState::Done);
    QVERIFY(!database.event(event.id).dirty);
  }

  void uncoveredRemoteRangeIsQueuedWithoutBlockingReads() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(
        database.open(directory.filePath(QStringLiteral("calendar.sqlite3")), &error),
        qPrintable(error));

    Account account;
    account.id = QStringLiteral("coverage-caldav-account");
    account.provider = ProviderKind::CalDav;
    account.displayName = QStringLiteral("Coverage CalDAV");
    account.endpoint = QStringLiteral("https://calendar.example.test/dav/");
    account.authStatus = QStringLiteral("connected");
    QVERIFY2(database.upsertAccount(account, &error), qPrintable(error));
    Calendar calendar;
    calendar.id = QStringLiteral("coverage-caldav-calendar");
    calendar.accountId = account.id;
    calendar.remoteId = QStringLiteral("remote-coverage-calendar");
    calendar.href = QStringLiteral("https://calendar.example.test/calendars/main/");
    calendar.name = QStringLiteral("Coverage");
    QVERIFY2(database.upsertCalendar(calendar, &error), qPrintable(error));

    google::GoogleSync google(&database);
    caldav::CalDavSync caldav(&database);
    ics::IcsService ics(&database);
    SyncCoordinator coordinator(&database, &google, &caldav, &ics);
    QSignalSpy scheduled(&coordinator, &SyncCoordinator::rangeHydrationScheduled);
    const QDateTime start(QDate(2012, 1, 1), QTime(0, 0), QTimeZone::UTC);
    const QDateTime end(QDate(2013, 1, 1), QTime(0, 0), QTimeZone::UTC);
    const QJsonObject first =
        coordinator.ensureRangeHydrated(start, end, {calendar.id}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!first.value(QStringLiteral("complete")).toBool());
    QVERIFY(first.value(QStringLiteral("hydrationScheduled")).toBool());
    QCOMPARE(scheduled.count(), 1);
    QVERIFY(!database.isSyncRangeCovered(calendar.id, start, end, &error));

    QVERIFY2(database.recordSyncCoverage(calendar.id, start, end, &error),
             qPrintable(error));
    const QJsonObject completed =
        coordinator.ensureRangeHydrated(start, end, {calendar.id}, &error);
    QVERIFY(completed.value(QStringLiteral("complete")).toBool());
    QVERIFY(!completed.value(QStringLiteral("hydrationScheduled")).toBool());
    QCOMPARE(scheduled.count(), 1);
  }

  void chunkedApplyDefersTokenAndPreservesConcurrentLocalEdit() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("calendar.sqlite3")),
                           &error), qPrintable(error));
    const Calendar calendar = remoteCalendar(&database, QStringLiteral("large-sync"));
    QVERIFY(!calendar.id.isEmpty());
    Event prior = remoteEvent(calendar.id, 40);
    prior.etag = QStringLiteral("old-etag");
    QVERIFY2(database.applyRemoteEvent(prior, &error), qPrintable(error));

    ChunkedSyncApply apply(&database);
    ChunkedSyncApply::Request batch;
    batch.calendar = calendar;
    batch.calendar.syncToken = QStringLiteral("new-token");
    for (int index = 0; index < 70; ++index) {
      batch.events.append(remoteEvent(calendar.id, index));
    }
    QStringList tokensAtCommit;
    bool editSucceeded = false;
    connect(&apply, &ChunkedSyncApply::chunkCommitted, this,
            [&](const QString&) {
              tokensAtCommit.append(database.calendar(calendar.id).syncToken);
              if (tokensAtCommit.size() == 1) {
                Calendar preferences = database.calendar(calendar.id);
                preferences.enabled = false;
                Event local = database.eventByRemoteId(calendar.id,
                                                        QStringLiteral("remote-40"));
                local.summary = QStringLiteral("Edited while sync applies");
                editSucceeded =
                    database.upsertCalendar(preferences, &error) &&
                    database.saveLocalEvent(
                        &local, OutboxOperation::Update, &error,
                        QStringLiteral("chunked-local-edit"), QStringLiteral("series"),
                        QStringLiteral("none"), local.localRevision);
              }
            });
    bool completed = false;
    bool succeeded = false;
    apply.start(std::move(batch), [&](bool ok, const QString& message) {
      completed = true;
      succeeded = ok;
      error = message;
    });
    QTRY_VERIFY_WITH_TIMEOUT(completed, 10000);
    QVERIFY2(succeeded, qPrintable(error));
    QVERIFY2(editSucceeded, qPrintable(error));
    QCOMPARE(tokensAtCommit, (QStringList{QStringLiteral("old-token"),
                                          QStringLiteral("old-token"),
                                          QStringLiteral("new-token")}));
    const Calendar after = database.calendar(calendar.id);
    QVERIFY(!after.enabled);
    QCOMPARE(after.syncToken, QStringLiteral("new-token"));
    QCOMPARE(database.eventsForCalendars({calendar.id}, &error).size(), 70);
    const Event local =
        database.eventByRemoteId(calendar.id, QStringLiteral("remote-40"));
    QVERIFY(local.dirty);
    QCOMPARE(local.summary, QStringLiteral("Edited while sync applies"));
    QVERIFY(!database.outboxItems(10, &error).isEmpty());
  }

  void failedChunkKeepsCursorAndReplayConverges() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("calendar.sqlite3")),
                           &error), qPrintable(error));
    const Calendar calendar = remoteCalendar(&database, QStringLiteral("retry-sync"));
    QVERIFY(!calendar.id.isEmpty());
    ChunkedSyncApply::Request batch;
    batch.calendar = calendar;
    batch.calendar.syncToken = QStringLiteral("final-token");
    for (int index = 0; index < 75; ++index) {
      batch.events.append(remoteEvent(calendar.id, index));
    }
    ChunkedSyncApply::Request retry = batch;
    batch.events[45].calendarId = QStringLiteral("another-calendar");
    ChunkedSyncApply failed(&database);
    int committed = 0;
    connect(&failed, &ChunkedSyncApply::chunkCommitted, this,
            [&](const QString&) { ++committed; });
    bool completed = false;
    bool succeeded = true;
    failed.start(std::move(batch), [&](bool ok, const QString&) {
      succeeded = ok;
      completed = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(completed, 10000);
    QVERIFY(!succeeded);
    QCOMPARE(committed, 1);
    QCOMPARE(database.calendar(calendar.id).syncToken, QStringLiteral("old-token"));
    QCOMPARE(database.eventsForCalendars({calendar.id}, &error).size(), 32);
    const QString firstId =
        database.eventByRemoteId(calendar.id, QStringLiteral("remote-0")).id;
    QVERIFY(!firstId.isEmpty());

    ChunkedSyncApply replay(&database);
    completed = false;
    replay.start(std::move(retry), [&](bool ok, const QString& message) {
      succeeded = ok;
      error = message;
      completed = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(completed, 10000);
    QVERIFY2(succeeded, qPrintable(error));
    QCOMPARE(database.calendar(calendar.id).syncToken, QStringLiteral("final-token"));
    QCOMPARE(database.eventsForCalendars({calendar.id}, &error).size(), 75);
    QCOMPARE(database.eventByRemoteId(calendar.id, QStringLiteral("remote-0")).id,
             firstId);
  }

  void completedRangeCoverageWaitsForLastChunk() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("calendar.sqlite3")),
                           &error), qPrintable(error));
    const Calendar calendar = remoteCalendar(&database, QStringLiteral("range-sync"));
    QVERIFY(!calendar.id.isEmpty());
    const QDateTime start(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::UTC);
    const QDateTime end(QDate(2027, 1, 1), QTime(0, 0), QTimeZone::UTC);
    ChunkedSyncApply::Request batch;
    batch.kind = ChunkedSyncApply::Request::Kind::Range;
    batch.calendar = calendar;
    batch.calendar.syncToken = QStringLiteral("range-token");
    batch.coverageStartUtc = start;
    batch.coverageEndUtc = end;
    for (int index = 0; index < 65; ++index) {
      batch.events.append(remoteEvent(calendar.id, index));
    }
    ChunkedSyncApply apply(&database);
    QList<bool> coverageAtCommit;
    connect(&apply, &ChunkedSyncApply::chunkCommitted, this,
            [&](const QString&) {
              coverageAtCommit.append(
                  database.isSyncRangeCovered(calendar.id, start, end));
            });
    bool completed = false;
    bool succeeded = false;
    apply.start(std::move(batch), [&](bool ok, const QString& message) {
      completed = true;
      succeeded = ok;
      error = message;
    });
    QTRY_VERIFY_WITH_TIMEOUT(completed, 10000);
    QVERIFY2(succeeded, qPrintable(error));
    QCOMPARE(coverageAtCommit, (QList<bool>{false, false, true}));
  }

  void icsValidatorsAdvanceWithLastEventChunk() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Database database;
    QString error;
    QVERIFY2(database.open(directory.filePath(QStringLiteral("calendar.sqlite3")),
                           &error), qPrintable(error));
    Account account;
    account.id = QStringLiteral("ics-sync-account");
    account.provider = ProviderKind::Ics;
    QVERIFY2(database.upsertAccount(account, &error), qPrintable(error));
    Calendar calendar;
    calendar.id = QStringLiteral("ics-sync-calendar");
    calendar.accountId = account.id;
    calendar.remoteId = QStringLiteral("subscription");
    calendar.name = QStringLiteral("ICS fixture");
    calendar.etag = QStringLiteral("old-etag");
    QVERIFY2(database.upsertCalendar(calendar, &error), qPrintable(error));
    IcsSubscription subscription;
    subscription.accountId = account.id;
    subscription.url = QStringLiteral("https://calendar.example.test/fixture.ics");
    subscription.etag = QStringLiteral("old-etag");
    QVERIFY2(database.upsertIcsSubscription(subscription, &error), qPrintable(error));

    ChunkedSyncApply::Request batch;
    batch.kind = ChunkedSyncApply::Request::Kind::IcsFeed;
    batch.calendar = calendar;
    batch.calendar.etag = QStringLiteral("new-etag");
    batch.etag = QStringLiteral("new-etag");
    batch.lastModified = QStringLiteral("Mon, 28 Sep 2026 10:00:00 GMT");
    batch.successAt = QDateTime(QDate(2026, 9, 28), QTime(10, 0), QTimeZone::UTC);
    batch.calendar.lastSyncAt = batch.successAt;
    for (int index = 0; index < 40; ++index) {
      batch.events.append(remoteEvent(calendar.id, index));
    }
    ChunkedSyncApply apply(&database);
    QStringList validators;
    connect(&apply, &ChunkedSyncApply::chunkCommitted, this,
            [&](const QString&) {
              validators.append(database.icsSubscription(account.id).etag);
            });
    bool completed = false;
    bool succeeded = false;
    apply.start(std::move(batch), [&](bool ok, const QString& message) {
      completed = true;
      succeeded = ok;
      error = message;
    });
    QTRY_VERIFY_WITH_TIMEOUT(completed, 10000);
    QVERIFY2(succeeded, qPrintable(error));
    QCOMPARE(validators, (QStringList{QStringLiteral("old-etag"),
                                      QStringLiteral("new-etag")}));
    QCOMPARE(database.calendar(calendar.id).etag, QStringLiteral("new-etag"));
    QCOMPARE(database.eventsForCalendars({calendar.id}, &error).size(), 40);
  }
};

QTEST_MAIN(SyncCoordinatorTest)
#include "test_synccoordinator.moc"
