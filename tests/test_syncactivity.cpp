#include <QSignalSpy>
#include <QTest>

#include "sync/syncactivity.h"

using namespace omacalendar;

class SyncActivityTest final : public QObject {
  Q_OBJECT

 private slots:
  void pollIntervalFollowsUseAndPower() {
    QDateTime current(QDate(2026, 9, 28), QTime(9, 0), QTimeZone::UTC);
    SyncActivity activity([&current]() { return current; });
    QSignalSpy changed(&activity, &SyncActivity::pollIntervalChanged);
    QCOMPARE(activity.pollIntervalMs(), SyncActivity::kDefaultIntervalMs);

    activity.setOnBattery(true);
    QCOMPARE(activity.pollIntervalMs(), SyncActivity::kBatteryIntervalMs);
    // In use wins over battery: the user is looking at the calendar.
    activity.setInteractive(true);
    QVERIFY(activity.interactive());
    QCOMPARE(activity.pollIntervalMs(), SyncActivity::kInteractiveIntervalMs);
    activity.setInteractive(false);
    QCOMPARE(activity.pollIntervalMs(), SyncActivity::kBatteryIntervalMs);
    activity.setOnBattery(false);
    QCOMPARE(activity.pollIntervalMs(), SyncActivity::kDefaultIntervalMs);
    QCOMPARE(changed.count(), 4);
    // An unchanged interval is not announced again.
    activity.setOnBattery(false);
    QCOMPARE(changed.count(), 4);

    // A client that stops renewing loses "in use" when the lease runs out.
    activity.setInteractive(true);
    current = current.addSecs(SyncActivity::kInteractiveLeaseSeconds + 1);
    QVERIFY(!activity.interactive());
    activity.setOnBattery(false);
    QCOMPARE(activity.pollIntervalMs(), SyncActivity::kDefaultIntervalMs);
  }

  void resumeAndConnectivityRequestRateLimitedSyncs() {
    QDateTime current(QDate(2026, 9, 28), QTime(9, 0), QTimeZone::UTC);
    SyncActivity activity([&current]() { return current; });
    QSignalSpy requested(&activity, &SyncActivity::syncRequested);

    activity.networkStateChanged(SyncActivity::kNetworkConnectedGlobal);
    QCOMPARE(requested.count(), 1);
    // Still connected is not a change.
    activity.networkStateChanged(SyncActivity::kNetworkConnectedGlobal);
    QCOMPARE(requested.count(), 1);

    // Dropping and regaining the network within a minute does not sync again.
    current = current.addSecs(20);
    activity.networkStateChanged(20);
    activity.networkStateChanged(SyncActivity::kNetworkConnectedGlobal);
    QCOMPARE(requested.count(), 1);
    activity.resumed();
    QCOMPARE(requested.count(), 1);

    current = current.addSecs(SyncActivity::kMinimumTriggerGapSeconds);
    activity.resumed();
    QCOMPARE(requested.count(), 2);
  }
};

QTEST_GUILESS_MAIN(SyncActivityTest)
#include "test_syncactivity.moc"
