#pragma once

#include <QDateTime>
#include <QList>
#include <QTimeZone>

#include "core/domain.h"

namespace omacalendar {

// A half-open [start, end) span of busy time in UTC.
struct BusyInterval {
  QDateTime start;
  QDateTime end;

  friend bool operator==(const BusyInterval&, const BusyInterval&) = default;
};

// Busy time the events occupy within [rangeStart, rangeEnd), merged and
// clipped. Cancelled and deleted events, events marked free (transparent),
// and events the user declined (an attendee marked self with a declined
// response) do not count. All-day events cover their dates in localZone.
[[nodiscard]] QList<BusyInterval> busyIntervalsFromEvents(const QList<Event>& events,
                                                          const QDateTime& rangeStart,
                                                          const QDateTime& rangeEnd,
                                                          const QTimeZone& localZone);

// Sorts and merges overlapping or touching intervals.
[[nodiscard]] QList<BusyInterval> mergeBusyIntervals(QList<BusyInterval> intervals);

// The first start at or after earliest, on a 15-minute step, where a span of
// durationMinutes avoids every busy interval and lies within the working
// hours [workDayStartHour, workDayEndHour) of its day in zone. Searches until
// horizon; returns an invalid QDateTime when nothing fits.
[[nodiscard]] QDateTime nextFreeSlot(const QList<BusyInterval>& busy,
                                     const QDateTime& earliest, int durationMinutes,
                                     int workDayStartHour, int workDayEndHour,
                                     const QTimeZone& zone, const QDateTime& horizon);

}  // namespace omacalendar
