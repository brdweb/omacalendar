#pragma once

#include <QList>
#include <QStringList>

#include "core/domain.h"

namespace omacalendar {

struct RecurrenceExpansionResult {
  QList<Event> occurrences;
  QStringList warnings;
  bool truncated = false;
};

// Expands a mixed collection of ordinary events, recurring masters, and
// detached exception rows into concrete occurrences for a bounded window.
// The class is stateless and safe to use without an event loop.
class RecurrenceExpander final {
 public:
  // The output and work limits are independent: excluded and duplicate
  // recurrence candidates still consume expansion steps.
  [[nodiscard]] static RecurrenceExpansionResult expand(
      const QList<Event>& events, const QDateTime& startUtc, const QDateTime& endUtc,
      qsizetype maximumOccurrences = 10000, qsizetype maximumExpansionSteps = 100000);
};

}  // namespace omacalendar
