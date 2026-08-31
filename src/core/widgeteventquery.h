#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include "core/domain.h"

namespace omacalendar {

class Database;

// The popup can browse or search dates unrelated to today. Keep the compact
// bar's current/up-next selection on its own bounded, index-backed time window
// so changing the popup range cannot change what the bar reports for now.
struct WidgetEventQueryResult {
  QList<Event> events;
  Event currentEvent;
  Event upNext;
};

[[nodiscard]] WidgetEventQueryResult queryWidgetEvents(
    const Database& database, const QDateTime& rangeStartUtc,
    const QDateTime& rangeEndUtc, const QDateTime& nowUtc,
    const QStringList& calendarIds = {}, const QString& searchQuery = {},
    QString* errorMessage = nullptr);

}  // namespace omacalendar
