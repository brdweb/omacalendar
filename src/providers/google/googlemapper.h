#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include "core/domain.h"

namespace omacalendar::google {

// Maps one CalendarList entry. Local ids are deliberately left unset: the
// sync layer owns remote-id to local-id reconciliation.
[[nodiscard]] Calendar calendarFromGoogleJson(const QJsonObject& resource,
                                              const QString& accountId);

// Maps one Events resource. originalStartTime becomes the canonical
// recurrenceId, which keeps recurring exceptions distinct even when Google
// only returns a sparse cancellation tombstone.
[[nodiscard]] Event eventFromGoogleJson(const QJsonObject& resource,
                                        const QString& calendarId);

// Produces an Events insert/update request body. The result contains only
// Google-writable fields; local database fields and Google response metadata
// are never copied into the request.
[[nodiscard]] QJsonObject eventToGoogleJson(const Event& event);

// Stable helpers used by the sync layer and mapper tests.
[[nodiscard]] QString rfc3339(const QDateTime& dateTime, const QString& timeZone = {});
[[nodiscard]] QString recurrenceInstanceId(const QJsonObject& originalStartTime);

class GoogleMapper final {
 public:
  GoogleMapper() = delete;

  [[nodiscard]] static Calendar calendarFromJson(const QJsonObject& resource,
                                                 const QString& accountId) {
    return calendarFromGoogleJson(resource, accountId);
  }

  [[nodiscard]] static Event eventFromJson(const QJsonObject& resource,
                                           const QString& calendarId) {
    return eventFromGoogleJson(resource, calendarId);
  }

  [[nodiscard]] static QJsonObject eventToJson(const Event& event) {
    return eventToGoogleJson(event);
  }
};

}  // namespace omacalendar::google
