#include <QCoreApplication>
#include <QtTest/QtTest>
#include <ctime>

#include "daemon/invitationclassification.h"

using namespace omacalendar;

namespace {

QDateTime utc(const int year, const int month, const int day, const int hour,
              const int minute = 0, const int second = 0) {
  return QDateTime(QDate(year, month, day), QTime(hour, minute, second),
                   QTimeZone::UTC);
}

Event allDayInvitation(const QString& id) {
  Event event;
  event.id = id;
  event.allDay = true;
  event.timeKind = TimeKind::AllDay;
  event.startDate = QDate(2027, 5, 5);
  event.endDate = QDate(2027, 5, 6);
  event.updatedAt = utc(2027, 5, 5, 6, 30);
  return event;
}

Event timedInvitation(const QString& id, const QDateTime& start, const QDateTime& end,
                      const QDateTime& updatedAt) {
  Event event;
  event.id = id;
  event.startUtc = start;
  event.endUtc = end;
  event.updatedAt = updatedAt;
  return event;
}

}  // namespace

class DaemonInvitationTest final : public QObject {
  Q_OBJECT

 private slots:
  void allDayClassificationChangesAtLocalExclusiveEnd() {
    const Event event = allDayInvitation(QStringLiteral("all-day"));
    const QDateTime beforeBoundary = utc(2027, 5, 6, 6, 59, 30);
    const QDateTime boundary = utc(2027, 5, 6, 7);

    QCOMPARE(invitationStartBoundary(event), utc(2027, 5, 5, 7));
    QCOMPARE(invitationEndBoundary(event), boundary);
    QVERIFY(invitationIsUpcoming(event, beforeBoundary));
    QVERIFY(!invitationIsUpcoming(event, boundary));
  }

  void sortingTotalsAndExpiryUseTheSameLocalBoundary() {
    const Event allDay = allDayInvitation(QStringLiteral("all-day"));
    const Event timedUpcoming =
        timedInvitation(QStringLiteral("timed-upcoming"), utc(2027, 5, 6, 7, 15),
                        utc(2027, 5, 6, 7, 30), utc(2027, 5, 5, 6));
    const Event timedPast =
        timedInvitation(QStringLiteral("timed-past"), utc(2027, 5, 6, 6, 30),
                        utc(2027, 5, 6, 6, 59), utc(2027, 5, 5, 5));
    const QDateTime beforeBoundary = utc(2027, 5, 6, 6, 59, 30);
    const QDateTime boundary = utc(2027, 5, 6, 7);
    QList<Event> invitations{timedPast, timedUpcoming, allDay};

    sortInvitations(&invitations, beforeBoundary);
    QCOMPARE(invitations.at(0).id, QStringLiteral("all-day"));
    QCOMPARE(invitations.at(1).id, QStringLiteral("timed-upcoming"));
    QCOMPARE(invitations.at(2).id, QStringLiteral("timed-past"));
    InvitationBucketTotals totals = invitationBucketTotals(invitations, beforeBoundary);
    QCOMPARE(totals.upcoming, 2);
    QCOMPARE(totals.past, 1);
    QCOMPARE(invitationCacheExpiry(invitations, beforeBoundary), boundary);

    sortInvitations(&invitations, boundary);
    QCOMPARE(invitations.at(0).id, QStringLiteral("timed-upcoming"));
    QCOMPARE(invitations.at(1).id, QStringLiteral("all-day"));
    QCOMPARE(invitations.at(2).id, QStringLiteral("timed-past"));
    totals = invitationBucketTotals(invitations, boundary);
    QCOMPARE(totals.upcoming, 1);
    QCOMPARE(totals.past, 2);
    QCOMPARE(invitationCacheExpiry(invitations, boundary), boundary.addSecs(60));
  }

  void timedClassificationRemainsUtc() {
    const Event event = timedInvitation(QStringLiteral("timed"), utc(2027, 5, 6, 6, 30),
                                        utc(2027, 5, 6, 6, 59, 45), utc(2027, 5, 5, 5));

    QCOMPARE(invitationStartBoundary(event), utc(2027, 5, 6, 6, 30));
    QCOMPARE(invitationEndBoundary(event), utc(2027, 5, 6, 6, 59, 45));
    QVERIFY(invitationIsUpcoming(event, utc(2027, 5, 6, 6, 59, 44)));
    QVERIFY(!invitationIsUpcoming(event, utc(2027, 5, 6, 6, 59, 45)));
  }
};

int main(int argc, char** argv) {
  qputenv("TZ", "America/Los_Angeles");
  ::tzset();
  QCoreApplication application(argc, argv);
  DaemonInvitationTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "test_daemon_invitations.moc"
