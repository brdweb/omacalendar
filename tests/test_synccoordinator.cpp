#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <type_traits>

#include "core/database.h"
#include "providers/caldav/caldavsync.h"
#include "providers/google/googlesync.h"
#include "providers/ics/icsservice.h"
#include "sync/provider.h"
#include "sync/synccoordinator.h"

using namespace omacalendar;

static_assert(std::is_base_of_v<Provider, google::GoogleSync>);
static_assert(std::is_base_of_v<Provider, caldav::CalDavSync>);
static_assert(std::is_base_of_v<Provider, ics::IcsService>);
static_assert(std::is_base_of_v<Provider, LocalProvider>);

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
};

QTEST_MAIN(SyncCoordinatorTest)
#include "test_synccoordinator.moc"
