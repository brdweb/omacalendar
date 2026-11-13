// events.* and stats.* IPC handlers.

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QString>
#include <QTimeZone>
#include <utility>

#include "core/domain.h"
#include "core/recurrenceexpander.h"
#include "daemon.h"
#include "daemon/daemon_p.h"
#include "providers/caldav/icalcodec.h"
#include "providers/google/googlemapper.h"

namespace {

constexpr int kDefaultEventPageLimit = 500;
constexpr int kMaxEventPageLimit = 5000;

QString eventReferenceId(const QJsonObject& params) {
  const QString direct = params.value(QStringLiteral("eventId")).toString();
  return direct.isEmpty() ? params.value(QStringLiteral("eventRef"))
                                .toObject()
                                .value(QStringLiteral("eventId"))
                                .toString()
                          : direct;
}

QString recurrenceReference(const QJsonObject& params) {
  const QJsonObject reference = params.value(QStringLiteral("eventRef")).toObject();
  return reference.value(QStringLiteral("recurrenceId"))
      .toString(reference.value(QStringLiteral("occurrenceId")).toString());
}

// Attachments are read from the provider payload on demand rather than
// stored, so events.list stays lean and nothing new is persisted.
QJsonArray eventAttachments(const omacalendar::Event& source,
                            const QString& recurrenceId) {
  if (source.rawPayload.isEmpty()) {
    return {};
  }
  if (source.rawFormat == QStringLiteral("google-json")) {
    return omacalendar::google::attachmentsFromGoogleJson(
        QJsonDocument::fromJson(source.rawPayload.toUtf8()).object());
  }
  if (source.rawFormat == QStringLiteral("text/calendar")) {
    return omacalendar::caldav::ICalendarCodec::attachments(source.rawPayload.toUtf8(),
                                                            source.uid, recurrenceId);
  }
  return {};
}

// Google calendars list the conference types they accept; only those that
// accept Google Meet can have one created.
bool calendarAcceptsGoogleMeet(const omacalendar::Calendar& calendar) {
  return calendar.capabilities.value(QStringLiteral("provider")).toString() ==
             QStringLiteral("google") &&
         calendar.capabilities.value(QStringLiteral("conferenceProperties"))
             .toObject()
             .value(QStringLiteral("allowedConferenceSolutionTypes"))
             .toArray()
             .contains(QStringLiteral("hangoutsMeet"));
}

// Turns a client's addConference flag into a provider conference request.
// Clients can never supply the request id themselves.
bool applyConferenceRequest(const QJsonObject& payload,
                            const omacalendar::Calendar& calendar,
                            omacalendar::Event* event, omacalendar::ipc::Error* error) {
  event->conferenceRequestId.clear();
  if (!payload.value(QStringLiteral("addConference")).toBool(false) ||
      !event->conferenceUrl.isEmpty()) {
    return true;
  }
  if (!calendarAcceptsGoogleMeet(calendar)) {
    if (error != nullptr) {
      *error = {QStringLiteral("conference_unsupported"),
                QStringLiteral("This calendar cannot create Google Meet links"), false};
    }
    return false;
  }
  event->conferenceRequestId = omacalendar::newUuid();
  return true;
}

QString normalizedRecurrenceScope(QString scope) {
  if (scope == QStringLiteral("this_occurrence")) {
    scope = QStringLiteral("occurrence");
  } else if (scope == QStringLiteral("this_and_future")) {
    scope = QStringLiteral("future");
  } else if (scope == QStringLiteral("entire_series") || scope.isEmpty()) {
    scope = QStringLiteral("series");
  }
  return scope;
}

bool occurrenceReferenceMatches(const QString& reference,
                                const omacalendar::Event& occurrence) {
  if (!occurrence.allDay && occurrence.timeKind == omacalendar::TimeKind::Floating) {
    // The client may still hold the generated UTC reference after CalDAV
    // acknowledges the same occurrence using its floating wall-time identity.
    omacalendar::Event requested = occurrence;
    requested.recurrenceId = reference;
    return omacalendar::caldav::ICalendarCodec::sameRecurrenceIdentity(requested,
                                                                       occurrence);
  }
  return omacalendar::recurrenceIdentityEqual(reference, occurrence.recurrenceId,
                                              occurrence.allDay, occurrence.timeKind,
                                              occurrence.startTimeZone);
}

QString resourceRemoteId(const QString& remoteId) {
  const qsizetype separator = remoteId.indexOf(QLatin1Char('#'));
  return separator < 0 ? remoteId : remoteId.left(separator);
}

QString recurrencePayload(QString recurrenceId) {
  recurrenceId = recurrenceId.trimmed();
  if ((recurrenceId.startsWith(QStringLiteral("TZID="), Qt::CaseInsensitive) ||
       recurrenceId.startsWith(QStringLiteral("RANGE="), Qt::CaseInsensitive))) {
    const qsizetype separator = recurrenceId.indexOf(QLatin1Char(':'));
    if (separator >= 0) {
      recurrenceId = recurrenceId.sliced(separator + 1);
    }
  }
  return recurrenceId;
}

QDate recurrenceDate(QString recurrenceId) {
  recurrenceId = recurrencePayload(std::move(recurrenceId));
  QDate date = QDate::fromString(recurrenceId, Qt::ISODate);
  if (!date.isValid() && recurrenceId.size() == 8) {
    date = QDate::fromString(recurrenceId, QStringLiteral("yyyyMMdd"));
  }
  return date;
}

QDateTime recurrenceDateTime(QString recurrenceId, const QString& timeZone) {
  recurrenceId = recurrencePayload(std::move(recurrenceId));
  QDateTime result = omacalendar::dateTimeFromIso(recurrenceId);
  if (result.isValid()) {
    return result;
  }
  const bool utc = recurrenceId.endsWith(QLatin1Char('Z'), Qt::CaseInsensitive);
  result =
      QDateTime::fromString(recurrenceId, utc ? QStringLiteral("yyyyMMdd'T'HHmmss'Z'")
                                              : QStringLiteral("yyyyMMdd'T'HHmmss"));
  if (!result.isValid()) {
    return {};
  }
  const QTimeZone zone = utc ? QTimeZone(QTimeZone::UTC) : QTimeZone(timeZone.toUtf8());
  return QDateTime(result.date(), result.time(),
                   zone.isValid() ? zone : QTimeZone(QTimeZone::LocalTime))
      .toUTC();
}

bool rebaseOccurrenceDraftOntoMaster(omacalendar::Event* draft,
                                     const omacalendar::Event& master,
                                     QString* errorMessage) {
  if (draft == nullptr || draft->recurrenceId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("The selected occurrence has no identity");
    }
    return false;
  }
  if (draft->allDay != master.allDay) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "Changing a whole series between timed and all-day from "
          "one occurrence is not supported");
    }
    return false;
  }
  if (master.allDay) {
    const QDate original = recurrenceDate(draft->recurrenceId);
    if (!original.isValid() || !draft->startDate.isValid()) {
      if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("The occurrence date is invalid");
      }
      return false;
    }
    const qint64 delta = original.daysTo(draft->startDate);
    const qint64 duration =
        draft->endDate.isValid()
            ? qMax<qint64>(1, draft->startDate.daysTo(draft->endDate))
            : qMax<qint64>(1, master.startDate.daysTo(master.endDate));
    draft->startDate = master.startDate.addDays(delta);
    draft->endDate = draft->startDate.addDays(duration);
  } else {
    const QDateTime original =
        recurrenceDateTime(draft->recurrenceId, master.startTimeZone);
    if (!original.isValid() || !draft->startUtc.isValid()) {
      if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("The occurrence time is invalid");
      }
      return false;
    }
    const qint64 delta = original.msecsTo(draft->startUtc);
    const qint64 duration = draft->endUtc.isValid()
                                ? draft->startUtc.msecsTo(draft->endUtc)
                                : master.startUtc.msecsTo(master.endUtc);
    draft->startUtc = master.startUtc.addMSecs(delta);
    draft->endUtc = draft->startUtc.addMSecs(qMax<qint64>(1, duration));
  }
  draft->id = master.id;
  draft->calendarId = master.calendarId;
  draft->remoteId = master.remoteId;
  draft->uid = master.uid;
  draft->etag = master.etag;
  draft->recurrenceId.clear();
  draft->recurrenceRule = master.recurrenceRule;
  draft->rawPayload = master.rawPayload;
  draft->rawFormat = master.rawFormat;
  draft->createdAt = master.createdAt;
  draft->localRevision = master.localRevision;
  return true;
}

omacalendar::Event detachedOccurrence(const omacalendar::Event& master,
                                      QString recurrenceId) {
  omacalendar::Event event = master;
  event.id = omacalendar::newUuid();
  event.remoteId = resourceRemoteId(master.remoteId);
  event.recurrenceId = std::move(recurrenceId);
  if (!event.remoteId.isEmpty()) {
    event.remoteId += QLatin1Char('#') + event.recurrenceId;
  }
  event.recurrenceRule.clear();
  event.createdAt = {};
  event.updatedAt = {};
  event.localRevision = 0;
  event.dirty = false;
  event.deleted = false;
  if (event.allDay) {
    const QDate occurrence = recurrenceDate(event.recurrenceId);
    const qint64 duration = qMax<qint64>(1, master.startDate.daysTo(master.endDate));
    event.startDate = occurrence;
    event.endDate = occurrence.addDays(duration);
  } else {
    const QDateTime occurrence =
        recurrenceDateTime(event.recurrenceId, master.startTimeZone);
    const qint64 duration = master.startUtc.msecsTo(master.endUtc);
    event.startUtc = occurrence;
    event.endUtc = occurrence.addMSecs(qMax<qint64>(1, duration));
  }
  return event;
}

}  // namespace

namespace omacalendar {

using namespace detail;

namespace {

// Records a non-retryable IPC error; returns false so callers can bail out.
bool setError(ipc::Error* error, const QString& code, const QString& message) {
  if (error != nullptr) {
    *error = {code, message, false};
  }
  return false;
}

// Reads params.recurrenceScope, accepting the long aliases.
bool parseRecurrenceScope(const QJsonObject& params, QString* recurrenceScope,
                          ipc::Error* error) {
  *recurrenceScope = normalizedRecurrenceScope(
      params.value(QStringLiteral("recurrenceScope")).toString());
  if (!QStringList{QStringLiteral("series"), QStringLiteral("occurrence"),
                   QStringLiteral("future")}
           .contains(*recurrenceScope)) {
    return setError(error, QStringLiteral("invalid_recurrence_scope"),
                    QStringLiteral("Unknown recurrence scope"));
  }
  return true;
}

// A change that affects guests needs an explicit notification choice; anything
// else defaults to not notifying them.
bool resolveGuestPolicy(const QJsonObject& params, bool guestsAffected,
                        QString* guestPolicy, ipc::Error* error) {
  *guestPolicy = params.value(QStringLiteral("guestNotificationPolicy")).toString();
  if (guestsAffected && guestPolicy->isEmpty()) {
    return setError(error, QStringLiteral("guest_notification_policy_required"),
                    QStringLiteral("Choose whether guests should be notified"));
  }
  if (guestPolicy->isEmpty()) {
    *guestPolicy = QStringLiteral("none");
  }
  return true;
}

// A recurrence identity is part of the event reference, never an editable
// draft field.  In particular, an untrusted draft must not be able to turn a
// non-recurring event into an occurrence mutation.  Persisted detached
// exceptions retain their own identity, while generated master occurrences
// require it on eventRef.
bool bindUpdateRecurrenceIdentity(const QJsonObject& params,
                                  const QString& recurrenceScope,
                                  const Event& currentEvent, Event* event,
                                  ipc::Error* error) {
  const QString referencedRecurrenceId = recurrenceReference(params).trimmed();
  const bool storedDetachedException = !currentEvent.recurrenceId.isEmpty();
  const bool storedRecurringMaster =
      !currentEvent.recurrenceRule.isEmpty() && !storedDetachedException;
  if (recurrenceScope == QStringLiteral("series")) {
    event->recurrenceId =
        storedDetachedException ? currentEvent.recurrenceId : QString();
    return true;
  }
  if (!storedDetachedException && !storedRecurringMaster) {
    return setError(error, QStringLiteral("recurrence_scope_requires_recurring_event"),
                    QStringLiteral("This recurrence scope requires a recurring event"));
  }
  if (storedDetachedException) {
    if (!referencedRecurrenceId.isEmpty() &&
        !occurrenceReferenceMatches(referencedRecurrenceId, currentEvent)) {
      return setError(
          error, QStringLiteral("occurrence_identity_mismatch"),
          QStringLiteral("eventRef does not match this detached occurrence"));
    }
    event->recurrenceId = currentEvent.recurrenceId;
    return true;
  }
  if (referencedRecurrenceId.isEmpty()) {
    return setError(error, QStringLiteral("occurrence_identity_required"),
                    QStringLiteral("This recurrence scope requires an occurrence ID"));
  }
  event->recurrenceId = referencedRecurrenceId;
  return true;
}

// The stored rows an events.update applies against.
struct UpdateTarget final {
  // The row whose daemon-owned fields the update keeps.
  Event current;
  // The row whose guests and local revision the client edited.
  Event providerSource;
  // The update creates the first detached exception of a generated occurrence.
  bool newDetachedException = false;
};

// Points an update at the row it really changes: a series-scope edit made from
// an occurrence is rebased onto the series master, and the first edit of a
// generated occurrence becomes a new detached exception.
bool retargetUpdate(const Database& database, const QString& recurrenceScope,
                    Event* event, UpdateTarget* target, ipc::Error* error) {
  const Event& currentEvent = target->current;
  const bool generatedOccurrence = !currentEvent.recurrenceRule.isEmpty() &&
                                   currentEvent.recurrenceId.isEmpty() &&
                                   !event->recurrenceId.isEmpty();
  if ((generatedOccurrence || !currentEvent.recurrenceId.isEmpty()) &&
      recurrenceScope == QStringLiteral("series")) {
    const Event master =
        currentEvent.recurrenceId.isEmpty()
            ? currentEvent
            : database.eventByUid(currentEvent.calendarId, currentEvent.uid);
    QString rebaseError;
    if (master.id.isEmpty() ||
        !rebaseOccurrenceDraftOntoMaster(event, master, &rebaseError)) {
      return setError(error, QStringLiteral("series_rebase_unsupported"),
                      rebaseError.isEmpty()
                          ? QStringLiteral("The recurring master could not be loaded")
                          : rebaseError);
    }
    target->current = master;
    target->providerSource = master;
    return true;
  }
  if (generatedOccurrence) {
    const QString recurrenceId = event->recurrenceId;
    event->id = newUuid();
    event->calendarId = currentEvent.calendarId;
    event->remoteId = resourceRemoteId(currentEvent.remoteId);
    if (!event->remoteId.isEmpty()) {
      event->remoteId += QLatin1Char('#') + recurrenceId;
    }
    event->uid = currentEvent.uid;
    event->etag = currentEvent.etag;
    event->recurrenceId = recurrenceId;
    event->recurrenceRule.clear();
    event->rawPayload = currentEvent.rawPayload;
    event->rawFormat = currentEvent.rawFormat;
    event->createdAt = {};
    event->localRevision = 0;
    target->providerSource = currentEvent;
    target->newDetachedException = true;
    return true;
  }
  if (recurrenceScope != QStringLiteral("series") && event->recurrenceId.isEmpty()) {
    return setError(error, QStringLiteral("occurrence_identity_required"),
                    QStringLiteral("This recurrence scope requires an occurrence ID"));
  }
  return true;
}

// Provider identity and retained lossless payload are daemon-owned. Clients
// can edit canonical fields but cannot redirect an update to a different
// remote object or erase metadata required for a safe round trip.
bool keepDaemonOwnedFields(const UpdateTarget& target, Event* event,
                           ipc::Error* error) {
  const Event& currentEvent = target.current;
  if (event->calendarId.trimmed().isEmpty()) {
    event->calendarId = currentEvent.calendarId;
  } else if (event->calendarId != currentEvent.calendarId) {
    return setError(
        error, QStringLiteral("calendar_move_not_supported"),
        QStringLiteral(
            "Moving an existing event between calendars is not available yet"));
  }
  if (!target.newDetachedException) {
    event->remoteId = currentEvent.remoteId;
    event->uid = currentEvent.uid;
    event->etag = currentEvent.etag;
    event->rawPayload = currentEvent.rawPayload;
    event->rawFormat = currentEvent.rawFormat;
    event->createdAt = currentEvent.createdAt;
  }
  return true;
}

bool calendarAcceptsUpdate(const Calendar& targetCalendar,
                           const QString& recurrenceScope, ipc::Error* error) {
  if (targetCalendar.id.isEmpty()) {
    return setError(error, QStringLiteral("not_found"),
                    QStringLiteral("Calendar not found"));
  }
  if (recurrenceScope == QStringLiteral("future") &&
      !targetCalendar.capabilities.value(QStringLiteral("thisAndFuture")).toBool()) {
    return setError(error, QStringLiteral("recurrence_scope_unsupported"),
                    QStringLiteral("This calendar cannot safely apply this-and-future "
                                   "changes"));
  }
  if (targetCalendar.readOnly) {
    return setError(error, QStringLiteral("calendar_read_only"),
                    QStringLiteral("This calendar is read-only"));
  }
  return true;
}

// The client's expected revision, defaulting to the stored one. A new detached
// exception is saved as a fresh row (revision 0), but only while the series is
// still at the revision the client opened.
bool updateExpectedRevision(const QJsonObject& params, const UpdateTarget& target,
                            qint64* expectedLocalRevision, ipc::Error* error) {
  const qint64 requestedLocalRevision =
      params.value(QStringLiteral("expectedLocalRevision"))
          .toInteger(target.current.localRevision);
  if (target.newDetachedException &&
      requestedLocalRevision != target.providerSource.localRevision) {
    return setError(error, QStringLiteral("revision_mismatch"),
                    QStringLiteral("The recurring series changed since it was opened"));
  }
  *expectedLocalRevision = target.newDetachedException ? 0 : requestedLocalRevision;
  return true;
}

// Resolves the stored row a move takes: the series master for a series move
// from a detached occurrence, or for an occurrence move the existing exception
// or a detached copy of the generated occurrence. `revisionSource` is the row
// whose local revision the client must have seen.
bool resolveMoveSource(const Database& database, const QString& recurrenceScope,
                       const QString& requestedRecurrenceId, Event* source,
                       Event* revisionSource, QString* dbError, ipc::Error* error) {
  if (source->id.isEmpty()) {
    return true;
  }
  if (recurrenceScope == QStringLiteral("series") && !source->recurrenceId.isEmpty()) {
    const Event master =
        database.eventByUid(source->calendarId, source->uid, {}, dbError);
    if (master.id.isEmpty()) {
      return setError(error, QStringLiteral("series_master_not_found"),
                      QStringLiteral("The recurring master could not be loaded"));
    }
    *source = master;
    *revisionSource = master;
    return true;
  }
  if (recurrenceScope != QStringLiteral("occurrence")) {
    return true;
  }
  if (!source->recurrenceId.isEmpty()) {
    if (!requestedRecurrenceId.isEmpty() &&
        !occurrenceReferenceMatches(requestedRecurrenceId, *source)) {
      return setError(
          error, QStringLiteral("occurrence_identity_mismatch"),
          QStringLiteral("The event and occurrence reference do not match"));
    }
    return true;
  }
  if (source->recurrenceRule.isEmpty() || requestedRecurrenceId.isEmpty()) {
    return setError(error, QStringLiteral("occurrence_identity_required"),
                    QStringLiteral("Moving one recurrence requires an occurrence ID"));
  }
  const Event existing = database.eventByUid(source->calendarId, source->uid,
                                             requestedRecurrenceId, dbError);
  if (!existing.id.isEmpty()) {
    *source = existing;
    *revisionSource = existing;
    return true;
  }
  *revisionSource = *source;
  *source = detachedOccurrence(*revisionSource, requestedRecurrenceId);
  const QString occurrenceTimeError = validateEventTimes(*source);
  if (!occurrenceTimeError.isEmpty()) {
    return setError(error, QStringLiteral("invalid_occurrence"), occurrenceTimeError);
  }
  return true;
}

// Checks the client's revision, that both calendars are writable and that a
// cross-account move was confirmed.
bool checkMoveAllowed(const QJsonObject& params, const Event& revisionSource,
                      const Calendar& sourceCalendar, const Calendar& targetCalendar,
                      qint64* expectedLocalRevision, ipc::Error* error) {
  *expectedLocalRevision = params.value(QStringLiteral("expectedLocalRevision"))
                               .toInteger(revisionSource.localRevision);
  if (*expectedLocalRevision >= 0 &&
      *expectedLocalRevision != revisionSource.localRevision) {
    return setError(error, QStringLiteral("stale_local_revision"),
                    QStringLiteral("The event changed after this editor was opened; "
                                   "reload it before moving"));
  }
  if (sourceCalendar.readOnly || targetCalendar.readOnly) {
    return setError(error, QStringLiteral("calendar_read_only"),
                    QStringLiteral("Source and target calendars must be writable"));
  }
  if (sourceCalendar.accountId != targetCalendar.accountId &&
      !params.value(QStringLiteral("confirmedCrossProvider")).toBool()) {
    return setError(error, QStringLiteral("confirmation_required"),
                    QStringLiteral("Confirm the cross-account calendar move"));
  }
  return true;
}

// The moved row: the source with the client's draft applied, in the target
// calendar. Moving one occurrence creates a standalone event there.
Event moveDestination(const QJsonObject& params, const Event& source,
                      const QString& targetCalendarId, const QString& recurrenceScope) {
  Event destination = source;
  if (params.value(QStringLiteral("draft")).isObject()) {
    QJsonObject merged = toJson(source);
    const QJsonObject draft =
        normalizedEventDraft(params.value(QStringLiteral("draft")).toObject());
    for (auto iterator = draft.constBegin(); iterator != draft.constEnd(); ++iterator) {
      merged.insert(iterator.key(), iterator.value());
    }
    destination = eventFromJson(merged);
    destination.uid = source.uid;
  }
  destination.calendarId = targetCalendarId;
  if (recurrenceScope == QStringLiteral("occurrence")) {
    destination.id.clear();
    destination.remoteId.clear();
    destination.uid.clear();
    destination.etag.clear();
    destination.recurrenceId.clear();
    destination.recurrenceRule.clear();
    destination.rawPayload.clear();
    destination.rawFormat.clear();
    destination.createdAt = {};
    destination.localRevision = 0;
  }
  return destination;
}

// Maps a database move failure to its IPC error code. Only a cross-account
// move can be blocked by the source's pending changes.
QString moveFailureCode(const QString& dbError, bool acrossAccounts) {
  if (dbError.contains(QStringLiteral("changed since"))) {
    return QStringLiteral("stale_local_revision");
  }
  if (acrossAccounts && dbError.contains(QStringLiteral("pending event changes"))) {
    return QStringLiteral("pending_operations");
  }
  return QStringLiteral("move_failed");
}

QJsonObject moveResult(const Event& source, const Event& destination,
                       const QString& mutationId, bool complete, qint64 revision) {
  return QJsonObject{
      {QStringLiteral("sourceEventId"), source.id},
      {QStringLiteral("event"), toJson(destination)},
      {QStringLiteral("clientMutationId"), mutationId},
      {QStringLiteral("state"),
       complete ? QStringLiteral("complete") : QStringLiteral("pending")},
      {QStringLiteral("revision"), revision},
  };
}

}  // namespace

QJsonValue Daemon::onEventsList(const QJsonObject& params, ipc::Error* error) {
  const QString startText = params.value(QStringLiteral("start")).toString();
  const QString endText = params.value(QStringLiteral("end")).toString();
  const QDateTime startUtc = dateTimeFromIso(startText);
  const QDateTime endUtc = dateTimeFromIso(endText);
  if (!startUtc.isValid() || !endUtc.isValid() || startUtc >= endUtc) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("start and end must be bounded ISO-8601 datetimes"),
                false};
    }
    return {};
  }
  QStringList calendarIds =
      parseCalendarIds(params.value(QStringLiteral("calendarIds")), error);
  if (error != nullptr && !error->code.isEmpty()) {
    return {};
  }
  QString dbError;
  const QJsonObject coverage =
      m_sync.ensureRangeHydrated(startUtc, endUtc, calendarIds, &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  const QList<Event> events =
      m_database.eventsBetween(startUtc, endUtc, calendarIds, &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }

  int offset = qMax(0, params.value(QStringLiteral("offset")).toInt());
  int limit = params.value(QStringLiteral("limit")).toInt(kDefaultEventPageLimit);
  if (limit <= 0) {
    limit = kDefaultEventPageLimit;
  }
  if (limit > kMaxEventPageLimit) {
    limit = kMaxEventPageLimit;
  }
  const int total = static_cast<int>(events.size());
  const int safeOffset = qMin(offset, total);
  const int safeCount = (safeOffset >= total) ? 0 : qMin(limit, total - safeOffset);

  QJsonArray encodedEvents;
  for (int i = 0; i < safeCount; ++i) {
    encodedEvents.append(toJson(events[safeOffset + i]));
  }
  return QJsonObject{
      {QStringLiteral("events"), encodedEvents},
      {QStringLiteral("offset"), safeOffset},
      {QStringLiteral("limit"), limit},
      {QStringLiteral("total"), total},
      {QStringLiteral("hasMore"), safeOffset + safeCount < total},
      {QStringLiteral("nextOffset"), safeOffset + safeCount < total
                                         ? (safeOffset + safeCount)
                                         : safeOffset + safeCount},
      {QStringLiteral("coverage"), coverage},
  };
}

QJsonValue Daemon::onStatsDailyCounts(const QJsonObject& params, ipc::Error* error) {
  const QString startText = params.value(QStringLiteral("start")).toString();
  const QString endText = params.value(QStringLiteral("end")).toString();
  const QDate start = QDate::fromString(startText, Qt::ISODate);
  const QDate end = QDate::fromString(endText, Qt::ISODate);
  if (!start.isValid() || !end.isValid() || start.toString(Qt::ISODate) != startText ||
      end.toString(Qt::ISODate) != endText || start >= end || start.daysTo(end) > 366) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("start and end must be ISO dates within 366 days"),
                false};
    }
    return {};
  }
  const QStringList calendarIds =
      parseCalendarIds(params.value(QStringLiteral("calendarIds")), error);
  if (error != nullptr && !error->code.isEmpty()) {
    return {};
  }
  QString zoneId;
  if (params.contains(QStringLiteral("timeZone"))) {
    if (!params.value(QStringLiteral("timeZone")).isString()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_params"),
                  QStringLiteral("timeZone must be an IANA time zone"), false};
      }
      return {};
    }
    zoneId = params.value(QStringLiteral("timeZone")).toString();
  } else {
    QString dbError;
    zoneId =
        m_database.setting(QStringLiteral("displayTimeZone"), {}, &dbError).toString();
    if (!dbError.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("database_error"), dbError, false};
      }
      return {};
    }
  }
  const QTimeZone zone =
      zoneId.isEmpty() ? QTimeZone(QTimeZone::LocalTime) : QTimeZone(zoneId.toUtf8());
  if (!zone.isValid()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("timeZone must be an IANA time zone"), false};
    }
    return {};
  }
  const QDateTime startUtc = QDateTime(start, QTime(0, 0), zone).toUTC();
  const QDateTime endUtc = QDateTime(end, QTime(0, 0), zone).toUTC();
  QString dbError;
  const QJsonObject coverage =
      m_sync.ensureRangeHydrated(startUtc, endUtc, calendarIds, &dbError);
  if (dbError.isEmpty()) {
    // The range query filters all-day dates using UTC dates. Padding makes
    // local dates at either edge available even in UTC+14 / UTC-12.
    const QList<Event> events = m_database.dailyCountEventsBetween(
        startUtc.addDays(-2), endUtc.addDays(2), calendarIds, &dbError);
    if (dbError.isEmpty()) {
      QJsonObject counts;
      for (const Event& event : events) {
        const QDate first =
            event.allDay ? event.startDate : event.startUtc.toTimeZone(zone).date();
        const QDate last = event.allDay
                               ? event.endDate.addDays(-1)
                               : event.endUtc.addMSecs(-1).toTimeZone(zone).date();
        for (QDate day = qMax(first, start); day <= qMin(last, end.addDays(-1));
             day = day.addDays(1)) {
          const QString key = day.toString(Qt::ISODate);
          counts.insert(key, counts.value(key).toInt() + 1);
        }
      }
      return QJsonObject{{QStringLiteral("counts"), counts},
                         {QStringLiteral("coverage"), coverage}};
    }
  }
  if (error != nullptr) {
    *error = {QStringLiteral("database_error"), dbError, false};
  }
  return {};
}

QJsonValue Daemon::onEventsGet(const QJsonObject& params, ipc::Error* error) {
  QString eventId;
  if (!validateRequiredString(params, QStringLiteral("eventId"), &eventId, error)) {
    return {};
  }
  QString dbError;
  Event event = m_database.event(eventId, &dbError);
  if (event.id.isEmpty()) {
    if (error != nullptr) {
      const bool notFound =
          dbError.isEmpty() || dbError == QStringLiteral("Event not found");
      *error = {
          notFound ? QStringLiteral("not_found") : QStringLiteral("database_error"),
          notFound ? QStringLiteral("Event not found") : dbError, false};
    }
    return {};
  }
  const auto detailed = [&event](const Event& value) {
    QJsonObject result = toJson(value);
    const Event& source = value.rawPayload.isEmpty() ? event : value;
    result.insert(QStringLiteral("attachments"),
                  eventAttachments(source, value.recurrenceId));
    return result;
  };
  QString requestedRecurrenceId =
      params.value(QStringLiteral("recurrenceId")).toString().trimmed();
  if (requestedRecurrenceId.isEmpty()) {
    requestedRecurrenceId = recurrenceReference(params).trimmed();
  }
  if (!requestedRecurrenceId.isEmpty()) {
    QString seriesError;
    const QList<Event> series =
        m_database.eventsByUid(event.calendarId, event.uid, &seriesError);
    if (!seriesError.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("database_error"), seriesError, false};
      }
      return {};
    }
    Event master;
    for (const Event& candidate : series) {
      if (candidate.recurrenceId.isEmpty() && !candidate.recurrenceRule.isEmpty()) {
        master = candidate;
        break;
      }
    }
    if (master.id.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("not_found"),
                  QStringLiteral("Recurring event series not found"), false};
      }
      return {};
    }
    const auto matchesReference = [&requestedRecurrenceId,
                                   &master](const Event& candidate) {
      Event reference = master;
      reference.recurrenceId = candidate.recurrenceId;
      return occurrenceReferenceMatches(requestedRecurrenceId, reference);
    };
    for (const Event& candidate : series) {
      if (!candidate.recurrenceId.isEmpty() && matchesReference(candidate)) {
        if (candidate.deleted) {
          if (error != nullptr) {
            *error = {QStringLiteral("not_found"),
                      QStringLiteral("The requested occurrence was cancelled"), false};
          }
          return {};
        }
        return detailed(candidate);
      }
    }

    QDateTime anchor;
    if (master.allDay) {
      const QDate date = recurrenceDate(requestedRecurrenceId);
      if (date.isValid()) {
        anchor = QDateTime(date, QTime(0, 0), QTimeZone::UTC);
      }
    } else {
      anchor = recurrenceDateTime(requestedRecurrenceId, master.startTimeZone);
    }
    if (!anchor.isValid()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_params"),
                  QStringLiteral("recurrenceId is not a valid occurrence reference"),
                  false};
      }
      return {};
    }
    const RecurrenceExpansionResult expanded =
        RecurrenceExpander::expand(series, anchor.addDays(-2), anchor.addDays(2), 1000);
    for (const Event& occurrence : expanded.occurrences) {
      if (matchesReference(occurrence)) {
        return detailed(occurrence);
      }
    }
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"),
                QStringLiteral("The requested occurrence does not exist"), false};
    }
    return {};
  }
  return detailed(event);
}

QJsonValue Daemon::onEventsCreate(const QJsonObject& params, ipc::Error* error) {
  const QJsonObject eventPayload =
      normalizedEventDraft(params.value(QStringLiteral("event")).isObject()
                               ? params.value(QStringLiteral("event")).toObject()
                               : params.value(QStringLiteral("draft")).toObject());
  if (eventPayload.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("events.create requires {event:{...}} payload"), false};
    }
    return {};
  }
  Event event = eventFromJson(eventPayload);
  if (event.calendarId.trimmed().isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("calendarId is required"), false};
    }
    return {};
  }
  if (event.id.isEmpty()) {
    event.id = newUuid();
  }
  QString dbError;
  const Calendar targetCalendar = m_database.calendar(event.calendarId, &dbError);
  if (targetCalendar.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Calendar not found"),
                false};
    }
    return {};
  }
  if (targetCalendar.readOnly) {
    if (error != nullptr) {
      *error = {QStringLiteral("calendar_read_only"),
                QStringLiteral("This calendar is read-only"), false};
    }
    return {};
  }
  if (!applyConferenceRequest(eventPayload, targetCalendar, &event, error)) {
    return {};
  }
  const QString timeError = validateEventTimes(event);
  if (!timeError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_event_time"), timeError, false};
    }
    return {};
  }
  const QString recurrenceScope = normalizedRecurrenceScope(
      params.value(QStringLiteral("recurrenceScope")).toString());
  if (recurrenceScope != QStringLiteral("series")) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_recurrence_scope"),
                QStringLiteral("New events must use the entire-series recurrence "
                               "scope"),
                false};
    }
    return {};
  }
  // Exception rows are provider-owned representations of a series occurrence.
  // Creating one from a client draft would let a caller invent an occurrence
  // identity that cannot be reconciled safely with the provider resource.
  if (!event.recurrenceId.trimmed().isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_recurrence_creation"),
                QStringLiteral("New events cannot include a recurrence occurrence "
                               "identity"),
                false};
    }
    return {};
  }
  QString guestPolicy =
      params.value(QStringLiteral("guestNotificationPolicy")).toString();
  if (!event.attendees.isEmpty() && guestPolicy.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("guest_notification_policy_required"),
                QStringLiteral("Choose whether guests should be notified"), false};
    }
    return {};
  }
  if (guestPolicy.isEmpty()) {
    guestPolicy = QStringLiteral("none");
  }
  const QString clientMutationId =
      params.value(QStringLiteral("clientMutationId")).toString(newUuid());
  if (!m_database.saveLocalEvent(&event, OutboxOperation::Create, &dbError,
                                 clientMutationId, recurrenceScope, guestPolicy, 0)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"),
                dbError.isEmpty() ? QStringLiteral("Unable to save event") : dbError,
                false};
    }
    return {};
  }
  emitEventsChanged({event.calendarId});
  const Calendar calendar = m_database.calendar(event.calendarId);
  m_sync.syncAccount(calendar.accountId);
  QJsonObject result = toJson(event);
  result.insert(QStringLiteral("clientMutationId"), clientMutationId);
  result.insert(QStringLiteral("revision"), m_database.changeRevision());
  return result;
}

bool Daemon::resolveUpdatePayload(const QJsonObject& params, QJsonObject* eventPayload,
                                  ipc::Error* error) {
  *eventPayload =
      normalizedEventDraft(params.value(QStringLiteral("event")).isObject()
                               ? params.value(QStringLiteral("event")).toObject()
                               : params.value(QStringLiteral("patch")).toObject());
  const QString referencedId = eventReferenceId(params);
  if (!referencedId.isEmpty() && !eventPayload->contains(QStringLiteral("id"))) {
    eventPayload->insert(QStringLiteral("id"), referencedId);
  }
  if (eventPayload->isEmpty()) {
    return setError(error, QStringLiteral("invalid_params"),
                    QStringLiteral("events.update requires {event:{...}} payload"));
  }
  const QString requestedId = eventPayload->value(QStringLiteral("id")).toString();
  const Event requestedCurrent = m_database.event(requestedId);
  if (!params.value(QStringLiteral("patch")).isObject() ||
      requestedCurrent.id.isEmpty()) {
    return true;
  }
  QJsonObject merged = toJson(requestedCurrent);
  const QString patchScope = normalizedRecurrenceScope(
      params.value(QStringLiteral("recurrenceScope")).toString());
  const QString occurrenceId = recurrenceReference(params).trimmed();
  if (!requestedCurrent.recurrenceRule.isEmpty() &&
      requestedCurrent.recurrenceId.isEmpty() && !occurrenceId.isEmpty() &&
      (patchScope == QStringLiteral("occurrence") ||
       patchScope == QStringLiteral("future"))) {
    // A partial edit applies to the selected occurrence, whose dates and
    // duration can differ from the master (including across DST). Resolve
    // through the same canonical expansion/exception lookup as events.get
    // before merging fields, so an existing detached edit is preserved too.
    const QJsonValue occurrence =
        onEventsGet({{QStringLiteral("eventId"), requestedId},
                     {QStringLiteral("recurrenceId"), occurrenceId}},
                    error);
    if (!occurrence.isObject()) {
      return false;
    }
    merged = occurrence.toObject();
  }
  const QString patchedId = merged.value(QStringLiteral("id")).toString();
  const QJsonObject patch =
      normalizedEventDraft(params.value(QStringLiteral("patch")).toObject());
  for (auto iterator = patch.constBegin(); iterator != patch.constEnd(); ++iterator) {
    merged.insert(iterator.key(), iterator.value());
  }
  merged.insert(QStringLiteral("id"), patchedId);
  *eventPayload = merged;
  return true;
}

QJsonValue Daemon::onEventsUpdate(const QJsonObject& params, ipc::Error* error) {
  QJsonObject eventPayload;
  if (!resolveUpdatePayload(params, &eventPayload, error)) {
    return {};
  }
  Event event = eventFromJson(eventPayload);
  if (event.id.trimmed().isEmpty()) {
    setError(error, QStringLiteral("invalid_params"),
             QStringLiteral("event.id is required"));
    return {};
  }
  UpdateTarget target;
  target.current = m_database.event(event.id);
  if (target.current.id.isEmpty()) {
    setError(error, QStringLiteral("not_found"), QStringLiteral("Event not found"));
    return {};
  }
  target.providerSource = target.current;
  QString recurrenceScope;
  if (!parseRecurrenceScope(params, &recurrenceScope, error) ||
      !bindUpdateRecurrenceIdentity(params, recurrenceScope, target.current, &event,
                                    error) ||
      !retargetUpdate(m_database, recurrenceScope, &event, &target, error) ||
      !keepDaemonOwnedFields(target, &event, error)) {
    return {};
  }
  const Calendar targetCalendar = m_database.calendar(event.calendarId);
  if (!calendarAcceptsUpdate(targetCalendar, recurrenceScope, error) ||
      !applyConferenceRequest(eventPayload, targetCalendar, &event, error)) {
    return {};
  }
  const QString timeError = validateEventTimes(event);
  if (!timeError.isEmpty()) {
    setError(error, QStringLiteral("invalid_event_time"), timeError);
    return {};
  }
  QString guestPolicy;
  if (!resolveGuestPolicy(params, event.attendees != target.providerSource.attendees,
                          &guestPolicy, error)) {
    return {};
  }
  const QString clientMutationId =
      params.value(QStringLiteral("clientMutationId")).toString(newUuid());
  qint64 expectedLocalRevision = 0;
  if (!updateExpectedRevision(params, target, &expectedLocalRevision, error)) {
    return {};
  }
  QString dbError;
  if (!m_database.saveLocalEvent(&event, OutboxOperation::Update, &dbError,
                                 clientMutationId, recurrenceScope, guestPolicy,
                                 expectedLocalRevision)) {
    setError(error, QStringLiteral("database_error"),
             dbError.isEmpty() ? QStringLiteral("Unable to update event") : dbError);
    return {};
  }
  emitEventsChanged({event.calendarId});
  const Calendar calendar = m_database.calendar(event.calendarId);
  m_sync.syncAccount(calendar.accountId);
  QJsonObject result = toJson(event);
  result.insert(QStringLiteral("clientMutationId"), clientMutationId);
  result.insert(QStringLiteral("revision"), m_database.changeRevision());
  return result;
}

QJsonValue Daemon::onEventsRemove(const QJsonObject& params, ipc::Error* error) {
  const QString eventId = eventReferenceId(params);
  if (eventId.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("eventRef.eventId is required"), false};
    }
    return {};
  }
  QString dbError;
  Event event = m_database.event(eventId, &dbError);
  if (!dbError.isEmpty() && event.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  if (event.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Event not found"), false};
    }
    return {};
  }
  const QString recurrenceScope = normalizedRecurrenceScope(
      params.value(QStringLiteral("recurrenceScope")).toString());
  if (!QStringList{QStringLiteral("series"), QStringLiteral("occurrence"),
                   QStringLiteral("future")}
           .contains(recurrenceScope)) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_recurrence_scope"),
                QStringLiteral("Unknown recurrence scope"), false};
    }
    return {};
  }
  bool newDetachedException = false;
  Event providerSource = event;
  if (recurrenceScope == QStringLiteral("series") && !event.recurrenceId.isEmpty()) {
    const Event master =
        m_database.eventByUid(event.calendarId, event.uid, {}, &dbError);
    if (master.id.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("series_master_not_found"),
                  QStringLiteral("The recurring master could not be loaded"), false};
      }
      return {};
    }
    event = master;
    providerSource = master;
  } else if (recurrenceScope != QStringLiteral("series") &&
             event.recurrenceId.isEmpty()) {
    const QString recurrenceId = recurrenceReference(params);
    if (event.recurrenceRule.isEmpty() || recurrenceId.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("occurrence_identity_required"),
                  QStringLiteral("Deleting one recurrence requires an occurrence ID"),
                  false};
      }
      return {};
    }
    providerSource = event;
    event = detachedOccurrence(providerSource, recurrenceId);
    newDetachedException = true;
    const QString occurrenceTimeError = validateEventTimes(event);
    if (!occurrenceTimeError.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_occurrence"), occurrenceTimeError, false};
      }
      return {};
    }
  }
  const Calendar eventCalendar = m_database.calendar(event.calendarId);
  if (eventCalendar.readOnly) {
    if (error != nullptr) {
      *error = {QStringLiteral("calendar_read_only"),
                QStringLiteral("This calendar is read-only"), false};
    }
    return {};
  }
  if (recurrenceScope == QStringLiteral("future") &&
      !eventCalendar.capabilities.value(QStringLiteral("thisAndFuture")).toBool()) {
    if (error != nullptr) {
      *error = {QStringLiteral("recurrence_scope_unsupported"),
                QStringLiteral("This calendar cannot safely delete this and future "
                               "occurrences"),
                false};
    }
    return {};
  }
  Event removal = event;
  removal.deleted = true;
  QString guestPolicy =
      params.value(QStringLiteral("guestNotificationPolicy")).toString();
  if (!providerSource.attendees.isEmpty() && guestPolicy.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("guest_notification_policy_required"),
                QStringLiteral("Choose whether guests should be notified"), false};
    }
    return {};
  }
  if (guestPolicy.isEmpty()) {
    guestPolicy = QStringLiteral("none");
  }
  const QString clientMutationId =
      params.value(QStringLiteral("clientMutationId")).toString(newUuid());
  const qint64 requestedLocalRevision =
      params.value(QStringLiteral("expectedLocalRevision"))
          .toInteger(providerSource.localRevision);
  if (newDetachedException && requestedLocalRevision != providerSource.localRevision) {
    if (error != nullptr) {
      *error = {QStringLiteral("revision_mismatch"),
                QStringLiteral("The recurring series changed since it was opened"),
                false};
    }
    return {};
  }
  const qint64 expectedLocalRevision =
      newDetachedException ? 0 : requestedLocalRevision;
  QString removeError;
  if (!m_database.saveLocalEvent(&removal, OutboxOperation::Remove, &removeError,
                                 clientMutationId, recurrenceScope, guestPolicy,
                                 expectedLocalRevision)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"),
                removeError.isEmpty() ? QStringLiteral("Unable to remove event")
                                      : removeError,
                false};
    }
    return {};
  }
  const Event updated = m_database.event(removal.id, &dbError);
  emitEventsChanged({removal.calendarId});
  const Calendar calendar = m_database.calendar(removal.calendarId);
  m_sync.syncAccount(calendar.accountId);
  if (!updated.id.isEmpty()) {
    QJsonObject result = toJson(updated);
    result.insert(QStringLiteral("clientMutationId"), clientMutationId);
    const QString undoToken = clientMutationId;
    result.insert(QStringLiteral("undoUntil"),
                  isoUtc(QDateTime::currentDateTimeUtc().addSecs(10)));
    result.insert(QStringLiteral("undoToken"), undoToken);
    result.insert(QStringLiteral("undoLabel"), QStringLiteral("Undo delete"));
    result.insert(QStringLiteral("revision"), m_database.changeRevision());
    return result;
  }
  QJsonObject result = toJson(event);
  result.insert(QStringLiteral("clientMutationId"), clientMutationId);
  result.insert(QStringLiteral("revision"), m_database.changeRevision());
  return result;
}

QJsonValue Daemon::onEventsUndo(const QJsonObject& params, ipc::Error* error) {
  const QString mutationId =
      params.value(QStringLiteral("undoToken"))
          .toString(params.value(QStringLiteral("clientMutationId")).toString());
  const QString eventId = params.value(QStringLiteral("eventId")).toString();
  if (mutationId.isEmpty() && eventId.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("undoToken, clientMutationId, or eventId is required"),
                false};
    }
    return {};
  }
  const QList<OutboxItem> operations = m_database.outboxItems(500);
  for (const OutboxItem& operation : operations) {
    if (operation.operation != OutboxOperation::Remove ||
        operation.state == OutboxState::Done ||
        (!mutationId.isEmpty() && operation.idempotencyKey != mutationId &&
         !operation.idempotencyKey.startsWith(mutationId + QLatin1Char(':'))) ||
        (!eventId.isEmpty() && operation.eventId != eventId)) {
      continue;
    }
    QString dbError;
    if (!m_database.discardOutbox(operation.id, &dbError)) {
      if (error != nullptr) {
        *error = {QStringLiteral("undo_failed"), dbError, false};
      }
      return {};
    }
    const Event restored = m_database.event(operation.eventId);
    emitEventsChanged({operation.calendarId});
    return QJsonObject{{QStringLiteral("undone"), true},
                       {QStringLiteral("event"), toJson(restored)},
                       {QStringLiteral("revision"), m_database.changeRevision()}};
  }
  if (error != nullptr) {
    *error = {QStringLiteral("undo_expired"),
              QStringLiteral("The delete can no longer be undone"), false};
  }
  return {};
}

QJsonValue Daemon::onEventsSearch(const QJsonObject& params, ipc::Error* error) {
  const QString query = params.value(QStringLiteral("query")).toString().trimmed();
  const QString attendee =
      params.value(QStringLiteral("attendee")).toString().trimmed();
  if (attendee.size() > 200) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("attendee must be at most 200 characters"), false};
    }
    return {};
  }
  if (query.isEmpty() && attendee.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("A non-empty search query or attendee is required"),
                false};
    }
    return {};
  }
  const QStringList calendarIds =
      parseCalendarIds(params.value(QStringLiteral("calendarIds")), error);
  if (error != nullptr && !error->code.isEmpty()) {
    return {};
  }
  const int limit =
      qBound(1, params.value(QStringLiteral("limit")).toInt(kDefaultEventPageLimit),
             kMaxEventPageLimit);
  const int offset = qMax(0, params.value(QStringLiteral("offset")).toInt());
  const QString startText = params.value(QStringLiteral("start")).toString().trimmed();
  const QString endText = params.value(QStringLiteral("end")).toString().trimmed();
  const QDateTime start = dateTimeFromIso(startText);
  const QDateTime end = dateTimeFromIso(endText);
  if ((!startText.isEmpty() && !start.isValid()) ||
      (!endText.isEmpty() && !end.isValid()) ||
      (start.isValid() && end.isValid() && start >= end)) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("Search date filters must be valid ISO-8601 bounds"),
                false};
    }
    return {};
  }
  EventSearchQuery search;
  search.text = query;
  search.calendarIds = calendarIds;
  search.startUtc = start;
  search.endUtc = end;
  search.accountId = params.value(QStringLiteral("accountId")).toString().trimmed();
  search.invitationState =
      params.value(QStringLiteral("invitationState")).toString().trimmed();
  search.attendee = attendee;
  search.limit = limit;
  search.offset = offset;
  QString dbError;
  const EventSearchPage page = m_database.searchEvents(search, &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  QJsonArray matches;
  for (const Event& event : page.events) {
    matches.append(toJson(event));
  }
  return QJsonObject{{QStringLiteral("events"), matches},
                     {QStringLiteral("total"), page.total},
                     {QStringLiteral("offset"), offset},
                     {QStringLiteral("limit"), limit},
                     {QStringLiteral("hasMore"), offset + matches.size() < page.total},
                     {QStringLiteral("nextOffset"), offset + matches.size()},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onEventsMove(const QJsonObject& params, ipc::Error* error) {
  QString eventId = eventReferenceId(params);
  QString targetCalendarId;
  if (eventId.isEmpty() ||
      !validateRequiredString(params, QStringLiteral("targetCalendarId"),
                              &targetCalendarId, error)) {
    if (eventId.isEmpty()) {
      setError(error, QStringLiteral("invalid_params"),
               QStringLiteral("eventRef.eventId is required"));
    }
    return {};
  }
  QString dbError;
  Event source = m_database.event(eventId, &dbError);
  Event revisionSource = source;
  QString recurrenceScope;
  if (!parseRecurrenceScope(params, &recurrenceScope, error)) {
    return {};
  }
  if (recurrenceScope == QStringLiteral("future")) {
    setError(error, QStringLiteral("recurrence_scope_unsupported"),
             QStringLiteral("Moving this and future occurrences cannot be represented "
                            "safely by the selected providers"));
    return {};
  }
  if (!resolveMoveSource(m_database, recurrenceScope,
                         recurrenceReference(params).trimmed(), &source,
                         &revisionSource, &dbError, error)) {
    return {};
  }
  const Calendar sourceCalendar = m_database.calendar(source.calendarId);
  const Calendar targetCalendar = m_database.calendar(targetCalendarId);
  if (source.id.isEmpty() || targetCalendar.id.isEmpty()) {
    setError(error, QStringLiteral("not_found"),
             source.id.isEmpty() ? QStringLiteral("Event not found")
                                 : QStringLiteral("Target calendar not found"));
    return {};
  }
  if (source.calendarId == targetCalendarId) {
    return toJson(source);
  }
  qint64 expectedLocalRevision = 0;
  if (!checkMoveAllowed(params, revisionSource, sourceCalendar, targetCalendar,
                        &expectedLocalRevision, error)) {
    return {};
  }
  Event destination =
      moveDestination(params, source, targetCalendarId, recurrenceScope);
  const QString timeError = validateEventTimes(destination);
  if (!timeError.isEmpty()) {
    setError(error, QStringLiteral("invalid_event_time"), timeError);
    return {};
  }
  QString guestPolicy;
  if (!resolveGuestPolicy(params, !destination.attendees.isEmpty(), &guestPolicy,
                          error)) {
    return {};
  }
  const QString mutationId =
      params.value(QStringLiteral("clientMutationId")).toString(newUuid());

  if (sourceCalendar.accountId == targetCalendar.accountId &&
      recurrenceScope == QStringLiteral("series")) {
    if (!m_database.moveLocalEvent(&destination, &dbError, mutationId, recurrenceScope,
                                   guestPolicy, expectedLocalRevision)) {
      setError(error, moveFailureCode(dbError, false), dbError);
      return {};
    }
    emitEventsChanged({source.calendarId, destination.calendarId});
    if (m_database.account(sourceCalendar.accountId).provider != ProviderKind::Local) {
      m_sync.syncAccount(sourceCalendar.accountId);
    }
    return moveResult(source, destination, mutationId, !destination.dirty,
                      m_database.changeRevision());
  }

  if (!m_database.moveEventAcrossAccounts(source, &destination, &dbError, mutationId,
                                          recurrenceScope, guestPolicy,
                                          expectedLocalRevision)) {
    setError(error, moveFailureCode(dbError, true), dbError);
    return {};
  }
  emitEventsChanged({source.calendarId, destination.calendarId});
  // The source removal depends on destination acknowledgement. Starting the
  // destination first lets an immediately acknowledged local destination
  // release the source operation before its provider drain begins.
  m_sync.syncAccount(targetCalendar.accountId);
  if (sourceCalendar.accountId != targetCalendar.accountId) {
    m_sync.syncAccount(sourceCalendar.accountId);
  } else if (recurrenceScope == QStringLiteral("occurrence") &&
             m_database.account(targetCalendar.accountId).provider ==
                 ProviderKind::Local) {
    // The detached cancellation depends on the destination create. Local
    // acknowledgement is synchronous, so a second bounded drain completes the
    // newly released dependency before returning to the client.
    m_sync.syncAccount(targetCalendar.accountId);
  }
  const bool complete =
      targetCalendar.accountId == sourceCalendar.accountId &&
      m_database.account(targetCalendar.accountId).provider == ProviderKind::Local;
  return moveResult(source, destination, mutationId, complete,
                    m_database.changeRevision());
}

QJsonValue Daemon::onEventsRespond(const QJsonObject& params, ipc::Error* error) {
  QString eventId = eventReferenceId(params);
  QString response;
  if (eventId.isEmpty() ||
      !validateRequiredString(params, QStringLiteral("response"), &response, error)) {
    if (eventId.isEmpty() && error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("eventRef.eventId is required"), false};
    }
    return {};
  }
  response = response.toLower();
  if (!QStringList{QStringLiteral("accepted"), QStringLiteral("tentative"),
                   QStringLiteral("declined")}
           .contains(response)) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("response must be accepted, tentative, or declined"),
                false};
    }
    return {};
  }
  QString dbError;
  Event event = m_database.event(eventId, &dbError);
  if (event.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Event not found"), false};
    }
    return {};
  }
  const QString recurrenceScope = normalizedRecurrenceScope(
      params.value(QStringLiteral("recurrenceScope")).toString());
  if (!QStringList{QStringLiteral("series"), QStringLiteral("occurrence"),
                   QStringLiteral("future")}
           .contains(recurrenceScope)) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_recurrence_scope"),
                QStringLiteral("Unknown recurrence scope"), false};
    }
    return {};
  }

  Event providerSource = event;
  bool newDetachedException = false;
  const QString requestedRecurrenceId = recurrenceReference(params).trimmed();
  if (recurrenceScope == QStringLiteral("series") && !event.recurrenceId.isEmpty()) {
    const Event master =
        m_database.eventByUid(event.calendarId, event.uid, {}, &dbError);
    if (master.id.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("series_master_not_found"),
                  QStringLiteral("The recurring master could not be loaded"), false};
      }
      return {};
    }
    event = master;
    providerSource = master;
  } else if (recurrenceScope != QStringLiteral("series") &&
             event.recurrenceId.isEmpty()) {
    if (event.recurrenceRule.isEmpty() || requestedRecurrenceId.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("occurrence_identity_required"),
                  QStringLiteral("This RSVP requires an occurrence ID"), false};
      }
      return {};
    }
    providerSource = event;
    event = detachedOccurrence(providerSource, requestedRecurrenceId);
    newDetachedException = true;
    const QString occurrenceTimeError = validateEventTimes(event);
    if (!occurrenceTimeError.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_occurrence"), occurrenceTimeError, false};
      }
      return {};
    }
  } else if (recurrenceScope != QStringLiteral("series") &&
             !requestedRecurrenceId.isEmpty() &&
             !occurrenceReferenceMatches(requestedRecurrenceId, event)) {
    if (error != nullptr) {
      *error = {QStringLiteral("occurrence_identity_mismatch"),
                QStringLiteral("The event and occurrence reference do not match"),
                false};
    }
    return {};
  }

  const Calendar calendar = m_database.calendar(event.calendarId);
  const Account account = m_database.account(calendar.accountId);
  if (calendar.id.isEmpty() || calendar.readOnly) {
    if (error != nullptr) {
      *error = {calendar.id.isEmpty() ? QStringLiteral("not_found")
                                      : QStringLiteral("calendar_read_only"),
                calendar.id.isEmpty() ? QStringLiteral("Calendar not found")
                                      : QStringLiteral("This calendar is read-only"),
                false};
    }
    return {};
  }
  const QString futureResponseCapability = account.provider == ProviderKind::CalDav
                                               ? QStringLiteral("rsvpThisAndFuture")
                                               : QStringLiteral("thisAndFuture");
  if (recurrenceScope == QStringLiteral("future") &&
      !calendar.capabilities.value(futureResponseCapability).toBool()) {
    if (error != nullptr) {
      *error = {
          QStringLiteral("recurrence_scope_unsupported"),
          QStringLiteral("This calendar cannot safely send this-and-future "
                         "responses. Reply to this occurrence or the entire series"),
          false};
    }
    return {};
  }
  if (account.provider == ProviderKind::CalDav &&
      !calendar.capabilities.value(QStringLiteral("serverScheduling")).toBool()) {
    if (error != nullptr) {
      *error = {QStringLiteral("unsupported"),
                QStringLiteral("This CalDAV server did not advertise scheduling"),
                false};
    }
    return {};
  }
  QJsonArray attendees = event.attendees;
  int attendeeIndex = -1;
  for (qsizetype i = 0; i < attendees.size(); ++i) {
    const QJsonObject attendee = attendees.at(i).toObject();
    const QString email = attendee.value(QStringLiteral("email")).toString();
    if (attendee.value(QStringLiteral("self")).toBool() ||
        (!account.principal.isEmpty() &&
         email.compare(account.principal, Qt::CaseInsensitive) == 0)) {
      attendeeIndex = static_cast<int>(i);
      break;
    }
  }
  if (attendeeIndex < 0) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_an_invitation"),
                QStringLiteral("No attendee identity for this account was found"),
                false};
    }
    return {};
  }
  QJsonObject attendee = attendees.at(attendeeIndex).toObject();
  attendee.insert(QStringLiteral("responseStatus"), response);
  attendee.insert(QStringLiteral("partstat"), response.toUpper());
  attendees.replace(attendeeIndex, attendee);
  event.attendees = attendees;
  const QString mutationId =
      params.value(QStringLiteral("clientMutationId")).toString(newUuid());
  QString responseNotificationPolicy =
      params.value(QStringLiteral("guestNotificationPolicy")).toString();
  if (responseNotificationPolicy.isEmpty()) {
    responseNotificationPolicy = account.provider == ProviderKind::CalDav
                                     ? QStringLiteral("all")
                                     : QStringLiteral("none");
  }
  const qint64 requestedLocalRevision =
      params.value(QStringLiteral("expectedLocalRevision"))
          .toInteger(providerSource.localRevision);
  if (newDetachedException && requestedLocalRevision != providerSource.localRevision) {
    if (error != nullptr) {
      *error = {QStringLiteral("revision_mismatch"),
                QStringLiteral("The recurring series changed since it was opened"),
                false};
    }
    return {};
  }
  const qint64 expectedLocalRevision =
      newDetachedException ? 0 : requestedLocalRevision;
  if (!m_database.saveLocalEvent(&event, OutboxOperation::Update, &dbError, mutationId,
                                 recurrenceScope, responseNotificationPolicy,
                                 expectedLocalRevision)) {
    if (error != nullptr) {
      *error = {QStringLiteral("response_failed"), dbError, false};
    }
    return {};
  }
  emitEventsChanged({event.calendarId});
  m_sync.syncAccount(account.id);
  return QJsonObject{{QStringLiteral("event"), toJson(event)},
                     {QStringLiteral("response"), response},
                     {QStringLiteral("clientMutationId"), mutationId},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

}  // namespace omacalendar
