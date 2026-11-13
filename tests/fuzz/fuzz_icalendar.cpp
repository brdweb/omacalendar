// Parses an untrusted iCalendar resource, then expands what it produced over
// a bounded window, as a CalDAV sync or ICS subscription would.
#include <QByteArray>
#include <QDateTime>
#include <QTimeZone>
#include <cstddef>
#include <cstdint>

#include "core/recurrenceexpander.h"
#include "providers/caldav/icalcodec.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const QByteArray payload(reinterpret_cast<const char*>(data),
                           static_cast<qsizetype>(size));
  omacalendar::caldav::ICalendarParseResult parsed =
      omacalendar::caldav::ICalendarCodec::parse(payload);
  if (!parsed.ok() || parsed.events.isEmpty()) {
    return 0;
  }
  for (omacalendar::Event& event : parsed.events) {
    event.rawPayload = QString::fromUtf8(payload);
  }
  const QDateTime start(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::UTC);
  const QDateTime end = start.addDays(400);
  // Small limits keep each run fast; the expander must honour them.
  const auto expanded =
      omacalendar::RecurrenceExpander::expand(parsed.events, start, end, 500, 5000);
  (void)expanded;
  for (const omacalendar::Event& event : parsed.events) {
    (void)omacalendar::RecurrenceExpander::finiteEnd(event);
  }
  return 0;
}
