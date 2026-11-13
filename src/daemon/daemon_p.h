#pragma once

// Private helpers shared by the daemon*.cpp translation units. Not part of
// the public Daemon interface.

#include <QJsonObject>
#include <QString>

#include "core/domain.h"

namespace omacalendar::detail {

inline QString validateEventTimes(const omacalendar::Event& event) {
  if (event.allDay) {
    if (!event.startDate.isValid() || !event.endDate.isValid() ||
        event.endDate <= event.startDate) {
      return QStringLiteral(
          "All-day events require valid start and exclusive end dates");
    }
    return {};
  }
  if (!event.startUtc.isValid() || !event.endUtc.isValid() ||
      event.endUtc <= event.startUtc) {
    return QStringLiteral("Timed events require a valid end later than their start");
  }
  return {};
}

inline QJsonObject normalizedEventDraft(QJsonObject object) {
  const auto copyAlias = [&object](const QString& canonical, const QString& alias) {
    if (!object.contains(canonical) && object.contains(alias)) {
      object.insert(canonical, object.value(alias));
    }
  };
  copyAlias(QStringLiteral("summary"), QStringLiteral("title"));
  copyAlias(QStringLiteral("description"), QStringLiteral("notes"));
  copyAlias(QStringLiteral("startUtc"), QStringLiteral("start"));
  copyAlias(QStringLiteral("endUtc"), QStringLiteral("end"));
  copyAlias(QStringLiteral("timeKind"), QStringLiteral("timeMode"));
  return object;
}

}  // namespace omacalendar::detail
