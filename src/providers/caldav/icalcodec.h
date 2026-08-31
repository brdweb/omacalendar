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
  // Updates only fields represented by Event inside the matching VEVENT. All
  // sibling events/components and provider-owned properties remain intact.
  [[nodiscard]] static ICalendarSerializeResult patch(
      const Event& event, const QByteArray& retainedPayload,
      const QString& productId = QStringLiteral("-//OmaCalendar//OmaCalendar 0.1//EN"));
  // Applies a recurrence-scoped mutation without replacing the VCALENDAR
  // resource. Occurrence/future mutations create a detached VEVENT when the
  // server resource does not already contain one; cancellations are encoded
  // as STATUS:CANCELLED so the master and sibling exceptions remain intact.
  [[nodiscard]] static ICalendarSerializeResult patchScoped(
      const Event& event, const QByteArray& retainedPayload,
      const QString& recurrenceScope, bool cancelled = false,
      const QString& productId = QStringLiteral("-//OmaCalendar//OmaCalendar 0.1//EN"));
  [[nodiscard]] static ICalendarSerializeResult stampClientMutationId(
      const QByteArray& payload, const QString& clientMutationId);
  [[nodiscard]] static bool hasClientMutationId(const QByteArray& payload,
                                                const QString& clientMutationId);
};

}  // namespace omacalendar::caldav
