#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

#include "core/freebusy.h"

using namespace omacalendar;

namespace {

QDateTime utc(const int day, const int hour, const int minute = 0) {
  return {QDate(2026, 9, day), QTime(hour, minute), QTimeZone::UTC};
}

Event timed(const QDateTime& start, const QDateTime& end) {
  Event event;
  event.startUtc = start;
  event.endUtc = end;
  return event;
}

}  // namespace

class FreeBusyTest final : public QObject {
  Q_OBJECT

 private slots:
  void busyIntervalsSkipFreeCancelledAndDeclined() {
    QList<Event> events;
    events.append(timed(utc(28, 9), utc(28, 10)));
    events.append(timed(utc(28, 9, 30), utc(28, 11)));  // overlaps: merged
    Event free = timed(utc(28, 12), utc(28, 13));
    free.transparency = QStringLiteral("transparent");
    events.append(free);
    Event cancelled = timed(utc(28, 13), utc(28, 14));
    cancelled.status = QStringLiteral("cancelled");
    events.append(cancelled);
    Event declined = timed(utc(28, 14), utc(28, 15));
    declined.attendees = QJsonArray{
        QJsonObject{{QStringLiteral("email"), QStringLiteral("me@example.com")},
                    {QStringLiteral("self"), true},
                    {QStringLiteral("responseStatus"), QStringLiteral("declined")}}};
    events.append(declined);
    events.append(timed(utc(28, 22), utc(29, 2)));  // clipped to the range end

    const QList<BusyInterval> busy =
        busyIntervalsFromEvents(events, utc(28, 0), utc(29, 0), QTimeZone::UTC);
    QCOMPARE(busy.size(), 2);
    QCOMPARE(busy.at(0), (BusyInterval{utc(28, 9), utc(28, 11)}));
    QCOMPARE(busy.at(1), (BusyInterval{utc(28, 22), utc(29, 0)}));
  }

  void allDayAndFloatingEventsUseTheLocalZone() {
    const QTimeZone berlin("Europe/Berlin");
    Event allDay;
    allDay.allDay = true;
    allDay.startDate = QDate(2026, 9, 28);
    allDay.endDate = QDate(2026, 9, 29);
    Event floating = timed(utc(28, 9), utc(28, 10));
    floating.timeKind = TimeKind::Floating;
    const QList<BusyInterval> busy =
        busyIntervalsFromEvents({floating, allDay}, utc(27, 0), utc(30, 0), berlin);
    QCOMPARE(busy.size(), 1);
    // Berlin is UTC+2 in September: the day runs 22:00 to 22:00 UTC.
    QCOMPARE(busy.first(), (BusyInterval{utc(27, 22), utc(28, 22)}));

    const QList<BusyInterval> floatingOnly =
        busyIntervalsFromEvents({floating}, utc(27, 0), utc(30, 0), berlin);
    QCOMPARE(floatingOnly.first(), (BusyInterval{utc(28, 7), utc(28, 8)}));
  }

  void nextFreeSlotRespectsBusyTimeAndWorkHours() {
    const QList<BusyInterval> busy{{utc(28, 9), utc(28, 10, 10)},
                                   {utc(28, 10, 30), utc(28, 17)}};
    // 10:10 rounds up to 10:15, and a 15-minute meeting fits before 10:30.
    QCOMPARE(nextFreeSlot(busy, utc(28, 9, 5), 15, 8, 18, QTimeZone::UTC, utc(30, 0)),
             utc(28, 10, 15));
    // 30 minutes clashes at 10:30, so it waits for 17:00.
    QCOMPARE(nextFreeSlot(busy, utc(28, 9, 5), 30, 8, 18, QTimeZone::UTC, utc(30, 0)),
             utc(28, 17));
    // An hour just fits before 18:00.
    QCOMPARE(nextFreeSlot(busy, utc(28, 9, 5), 60, 8, 18, QTimeZone::UTC, utc(30, 0)),
             utc(28, 17));
    // Ninety minutes would run past 18:00, so it moves to the next morning.
    QCOMPARE(nextFreeSlot(busy, utc(28, 9, 5), 90, 8, 18, QTimeZone::UTC, utc(30, 0)),
             utc(29, 8));
    // Nothing before the horizon, and no zero-length meetings.
    QVERIFY(!nextFreeSlot(busy, utc(28, 9, 5), 90, 8, 18, QTimeZone::UTC, utc(28, 20))
                 .isValid());
    QVERIFY(!nextFreeSlot(busy, utc(28, 9), 0, 8, 18, QTimeZone::UTC, utc(30, 0))
                 .isValid());
  }
};

QTEST_GUILESS_MAIN(FreeBusyTest)
#include "test_freebusy.moc"
