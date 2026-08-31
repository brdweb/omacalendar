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
[[nodiscard]] Event eventFromGoogleJson(
    const QJsonObject& resource, const QString& calendarId,
    const QJsonArray& calendarDefaultReminders = {});

// Produces an Events insert/update request body. The result contains only
// Google-writable fields; local database fields and Google response metadata
// are never copied into the request.
[[nodiscard]] QJsonObject eventToGoogleJson(const Event& event);

// Builds an idempotent insert body. Google accepts caller-supplied event ids;
// deriving one from the durable client mutation id makes a repeated POST safe
// to reconcile after a lost response.
[[nodiscard]] QString eventIdForClientMutation(const QString& clientMutationId);
[[nodiscard]] QJsonObject eventToGoogleCreateJson(const Event& event,
                                                  const QString& clientMutationId);
[[nodiscard]] bool googleEventHasMutationIdentity(const QJsonObject& resource,
                                                  const QString& clientMutationId);

// Returns a minimal attendeesOmitted patch only when the canonical event is an
// RSVP-only edit of the provider snapshot. An empty result means the normal
// full editable-field patch must be used.
[[nodiscard]] QJsonObject rsvpPatchForGoogleEvent(const Event& event);

// Stable helpers used by the sync layer and mapper tests.
[[nodiscard]] QString rfc3339(const QDateTime& dateTime, const QString& timeZone = {});
[[nodiscard]] QString recurrenceInstanceId(const QJsonObject& originalStartTime);

enum class GoogleOccurrenceMutationTargetKind {
  Invalid,
  DirectInstance,
  ResolveInstance,
};

struct GoogleOccurrenceMutationTarget final {
  GoogleOccurrenceMutationTargetKind kind = GoogleOccurrenceMutationTargetKind::Invalid;
  QString remoteId;
};

// Selects the provider identity used for an occurrence-scoped mutation.
// Materialized Google instances can be addressed directly by their instance
// id. Locally generated occurrences use a synthetic "master#recurrence-id"
// identity and must first be resolved through events.instances using only the
// master id.
[[nodiscard]] GoogleOccurrenceMutationTarget googleOccurrenceMutationTarget(
    const Event& event);

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
