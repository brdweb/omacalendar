#pragma once

#include <QCache>
#include <QList>
#include <QStringList>
#include <optional>

#include "core/domain.h"

namespace omacalendar {

struct RecurrenceExpansionResult {
  QList<Event> occurrences;
  QStringList warnings;
  bool truncated = false;
};

// Exclusive end of the last possible generated occurrence. An absent bound
// means that the rule is infinite or could not be proven finite safely.
struct RecurrenceSeriesEnd {
  QDateTime utc;
  QDate date;
};

struct CachedSeriesExpansion {
  QList<Event> occurrences;
  qsizetype workSteps = 0;
};

// Owned by one database connection. Any durable revision change (including a
// detached exception update) discards its bounded collection of series buckets.
class RecurrenceExpansionCache final {
 public:
  void invalidateIfChanged(qint64 revision);

 private:
  friend class RecurrenceExpander;
  QCache<QString, CachedSeriesExpansion> m_entries{5000};
  qint64 m_revision = -1;
};

// Expands a mixed collection of ordinary events, recurring masters, and
// detached exception rows into concrete occurrences for a bounded window.
// The expansion engine remains stateless; caching is opt-in per DB connection.
class RecurrenceExpander final {
 public:
  // The output and work limits are independent: excluded and duplicate
  // recurrence candidates still consume expansion steps.
  [[nodiscard]] static RecurrenceExpansionResult expand(
      const QList<Event>& events, const QDateTime& startUtc, const QDateTime& endUtc,
      qsizetype maximumOccurrences = 10000, qsizetype maximumExpansionSteps = 100000,
      RecurrenceExpansionCache* cache = nullptr);
  [[nodiscard]] static std::optional<RecurrenceSeriesEnd> finiteEnd(
      const Event& master);
};

}  // namespace omacalendar
