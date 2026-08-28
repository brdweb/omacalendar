#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include "core/domain.h"

namespace omacalendar::caldav {

struct ICalendarError {
  QString code;
  QString message;
  int eventIndex = -1;

  [[nodiscard]] bool isEmpty() const { return code.isEmpty(); }
};

struct ICalendarParseResult {
  QList<Event> events;
  ICalendarError error;

  [[nodiscard]] bool ok() const { return error.isEmpty(); }
};

struct ICalendarSerializeResult {
  QByteArray payload;
  ICalendarError error;

  [[nodiscard]] bool ok() const { return error.isEmpty(); }
};

// Converts between the provider-neutral Event representation and RFC 5545
// calendar resources. The raw source resource is retained on every parsed
// event so a caller can persist or inspect the exact server representation.
class ICalendarCodec final {
 public:
  [[nodiscard]] static ICalendarParseResult parse(const QByteArray& payload);
  [[nodiscard]] static ICalendarSerializeResult serialize(
      const Event& event,
      const QString& productId = QStringLiteral("-//OmaCalendar//OmaCalendar 0.1//EN"));
};

}  // namespace omacalendar::caldav
