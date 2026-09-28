#include "core/freebusy.h"

#include <QJsonObject>
#include <algorithm>

namespace omacalendar {

namespace {

bool declinedBySelf(const Event& event) {
  for (const QJsonValue& value : event.attendees) {
    const QJsonObject attendee = value.toObject();
    if (!attendee.value(QStringLiteral("self")).toBool()) {
      continue;
    }
    const QString response =
        attendee.value(QStringLiteral("responseStatus"))
            .toString(attendee.value(QStringLiteral("partstat")).toString())
            .toLower();
    return response == QStringLiteral("declined");
  }
  return false;
}

}  // namespace

QList<BusyInterval> mergeBusyIntervals(QList<BusyInterval> intervals) {
  std::sort(intervals.begin(), intervals.end(),
            [](const BusyInterval& left, const BusyInterval& right) {
              return left.start < right.start;
            });
  QList<BusyInterval> merged;
  for (const BusyInterval& interval : std::as_const(intervals)) {
    if (!interval.start.isValid() || !interval.end.isValid() ||
        interval.end <= interval.start) {
      continue;
    }
    if (!merged.isEmpty() && interval.start <= merged.last().end) {
      merged.last().end = std::max(merged.last().end, interval.end);
    } else {
      merged.append(interval);
    }
  }
  return merged;
}

QList<BusyInterval> busyIntervalsFromEvents(const QList<Event>& events,
                                            const QDateTime& rangeStart,
                                            const QDateTime& rangeEnd,
                                            const QTimeZone& localZone) {
  QList<BusyInterval> intervals;
  for (const Event& event : events) {
    if (event.deleted ||
        event.status.compare(QStringLiteral("cancelled"), Qt::CaseInsensitive) == 0 ||
        event.transparency.compare(QStringLiteral("transparent"),
                                   Qt::CaseInsensitive) == 0 ||
        declinedBySelf(event)) {
      continue;
    }
    BusyInterval interval;
    if (event.allDay) {
      interval.start = QDateTime(event.startDate, QTime(0, 0), localZone).toUTC();
      interval.end = QDateTime(event.endDate, QTime(0, 0), localZone).toUTC();
    } else if (event.timeKind == TimeKind::Floating) {
      // Floating times are wall-clock times wherever the user is.
      interval.start =
          QDateTime(event.startUtc.date(), event.startUtc.time(), localZone).toUTC();
      interval.end =
          QDateTime(event.endUtc.date(), event.endUtc.time(), localZone).toUTC();
    } else {
      interval.start = event.startUtc.toUTC();
      interval.end = event.endUtc.toUTC();
    }
    interval.start = std::max(interval.start, rangeStart.toUTC());
    interval.end = std::min(interval.end, rangeEnd.toUTC());
    intervals.append(interval);
  }
  return mergeBusyIntervals(intervals);
}

QDateTime nextFreeSlot(const QList<BusyInterval>& busy, const QDateTime& earliest,
                       const int durationMinutes, const int workDayStartHour,
                       const int workDayEndHour, const QTimeZone& zone,
                       const QDateTime& horizon) {
  if (durationMinutes <= 0 || workDayEndHour <= workDayStartHour ||
      !earliest.isValid() || !horizon.isValid()) {
    return {};
  }
  const QList<BusyInterval> merged = mergeBusyIntervals(busy);
  const qint64 duration = qint64(durationMinutes) * 60;
  // Round up to the next quarter hour in the zone.
  QDateTime candidate = earliest.toTimeZone(zone);
  const int minute = candidate.time().minute();
  if (minute % 15 != 0 || candidate.time().second() != 0) {
    candidate = candidate.addSecs(-candidate.time().second())
                    .addSecs(qint64(15 - minute % 15) * 60);
  }
  while (candidate.addSecs(duration) <= horizon) {
    const QDateTime dayStart(candidate.date(), QTime(workDayStartHour, 0), zone);
    const QDateTime dayEnd =
        workDayEndHour >= 24
            ? QDateTime(candidate.date().addDays(1), QTime(0, 0), zone)
            : QDateTime(candidate.date(), QTime(workDayEndHour, 0), zone);
    if (candidate < dayStart) {
      candidate = dayStart;
      continue;
    }
    const QDateTime end = candidate.addSecs(duration);
    if (end > dayEnd) {
      candidate =
          QDateTime(candidate.date().addDays(1), QTime(workDayStartHour, 0), zone);
      continue;
    }
    const auto clash =
        std::find_if(merged.cbegin(), merged.cend(),
                     [&candidate, &end](const BusyInterval& interval) {
                       return interval.start < end && interval.end > candidate;
                     });
    if (clash == merged.cend()) {
      return candidate.toUTC();
    }
    // Jump past the clash, back onto the quarter-hour grid.
    candidate = clash->end.toTimeZone(zone);
    const int clashMinute = candidate.time().minute();
    if (clashMinute % 15 != 0 || candidate.time().second() != 0) {
      candidate = candidate.addSecs(-candidate.time().second())
                      .addSecs(qint64(15 - clashMinute % 15) * 60);
    }
  }
  return {};
}

}  // namespace omacalendar
