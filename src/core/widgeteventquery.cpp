#include "core/widgeteventquery.h"

#include <QTime>
#include <QTimeZone>

#include "core/database.h"

namespace omacalendar {
namespace {

constexpr int kUpNextLookaheadDays = 45;

QDateTime eventStart(const Event& event) {
  return event.allDay ? QDateTime(event.startDate, QTime(0, 0), QTimeZone::UTC)
                      : event.startUtc;
}

QDateTime eventEnd(const Event& event) {
  return event.allDay ? QDateTime(event.endDate, QTime(0, 0), QTimeZone::UTC)
                      : event.endUtc;
}

bool isEarlierCurrentEvent(const Event& candidate, const Event& selected) {
  if (selected.id.isEmpty()) {
    return true;
  }
  if (candidate.endUtc != selected.endUtc) {
    return candidate.endUtc < selected.endUtc;
  }
  if (candidate.startUtc != selected.startUtc) {
    return candidate.startUtc < selected.startUtc;
  }
  if (candidate.id != selected.id) {
    return candidate.id < selected.id;
  }
  return candidate.recurrenceId < selected.recurrenceId;
}

bool isEarlierUpNextEvent(const Event& candidate, const Event& selected) {
  if (selected.id.isEmpty()) {
    return true;
  }
  const QDateTime candidateStart = eventStart(candidate);
  const QDateTime selectedStart = eventStart(selected);
  if (candidateStart != selectedStart) {
    return candidateStart < selectedStart;
  }
  const QDateTime candidateEnd = eventEnd(candidate);
  const QDateTime selectedEnd = eventEnd(selected);
  if (candidateEnd != selectedEnd) {
    return candidateEnd < selectedEnd;
  }
  if (candidate.id != selected.id) {
    return candidate.id < selected.id;
  }
  return candidate.recurrenceId < selected.recurrenceId;
}

}  // namespace

WidgetEventQueryResult queryWidgetEvents(
    const Database& database, const QDateTime& rangeStartUtc,
    const QDateTime& rangeEndUtc, const QDateTime& nowUtc,
    const QStringList& calendarIds, const QString& searchQuery, QString* errorMessage) {
  WidgetEventQueryResult result;
  QString localError;
  QString* queryError = errorMessage != nullptr ? errorMessage : &localError;
  queryError->clear();
  if (!rangeStartUtc.isValid() || !rangeEndUtc.isValid() ||
      rangeStartUtc >= rangeEndUtc || !nowUtc.isValid()) {
    *queryError = QStringLiteral("Valid widget range and current time required");
    return result;
  }

  if (searchQuery.trimmed().isEmpty()) {
    result.events =
        database.eventsBetween(rangeStartUtc, rangeEndUtc, calendarIds, queryError);
  } else {
    result.events =
        database.searchEvents(searchQuery.trimmed(), calendarIds, 500, 0, queryError);
  }
  if (!queryError->isEmpty()) {
    return result;
  }

  // Starting the overlap window at now still includes ongoing timed and
  // multi-day/all-day events because eventsBetween uses end > rangeStart.
  // The fixed lookahead prevents a distant popup range from widening this
  // query or turning it into an unbounded scan.
  const QList<Event> nowEvents = database.eventsBetween(
      nowUtc, nowUtc.addDays(kUpNextLookaheadDays), calendarIds, queryError);
  if (!queryError->isEmpty()) {
    return result;
  }

  for (const Event& event : nowEvents) {
    const QDateTime start = eventStart(event);
    const QDateTime end = eventEnd(event);
    if (!start.isValid() || !end.isValid() || end <= nowUtc) {
      continue;
    }
    // Match the widget's established "current event" semantics: all-day
    // events remain eligible for Up Next but not for the NOW row.
    if (!event.allDay && start <= nowUtc &&
        isEarlierCurrentEvent(event, result.currentEvent)) {
      result.currentEvent = event;
    }
    // Preserve the existing Up Next contract, where an ongoing event is
    // eligible and the widget renders it with a "Now" label.
    if (isEarlierUpNextEvent(event, result.upNext)) {
      result.upNext = event;
    }
  }
  return result;
}

}  // namespace omacalendar
