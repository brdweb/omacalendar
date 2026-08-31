#include "core/domain.h"

#include <QTimeZone>
#include <QUuid>

namespace omacalendar {
namespace {

QString jsonString(const QJsonObject& object, const char* key) {
  return object.value(QLatin1StringView(key)).toString();
}

QDate jsonDate(const QJsonObject& object, const char* key) {
  return QDate::fromString(jsonString(object, key), Qt::ISODate);
}

QJsonObject presentationCapabilities(QJsonObject capabilities) {
  // Provider validators drive daemon-side synchronization and are not client
  // capabilities. Keep them out of presentation DTOs even when a provider
  // stores them beside public booleans in the database.
  capabilities.remove(QStringLiteral("ctag"));
  capabilities.remove(QStringLiteral("syncedCtag"));
  capabilities.remove(QStringLiteral("serverSyncToken"));
  capabilities.remove(QStringLiteral("syncToken"));
  capabilities.remove(QStringLiteral("etag"));
  return capabilities;
}

struct RecurrenceParts {
  QString body;
  QString timeZone;
  bool valueDate = false;
};

RecurrenceParts splitRecurrenceParameters(QString value) {
  RecurrenceParts result;
  value = value.trimmed();
  const qsizetype separator = value.indexOf(QLatin1Char(':'));
  const QString prefix = separator < 0 ? QString() : value.left(separator);
  // Plain ISO timestamps contain colons too.  Only treat a colon as the
  // parameter separator when the preceding text is recognizably an
  // iCalendar property/parameter prefix.
  const bool hasParameters =
      prefix.contains(QLatin1Char('=')) ||
      prefix.startsWith(QStringLiteral("RECURRENCE-ID"), Qt::CaseInsensitive);
  if (!hasParameters || separator < 0) {
    result.body = value;
    return result;
  }

  const QString parameters = value.left(separator);
  for (const QString& parameter :
       parameters.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
    const qsizetype equals = parameter.indexOf(QLatin1Char('='));
    if (equals < 0) {
      continue;
    }
    const QString name = parameter.left(equals).trimmed();
    if (name.compare(QStringLiteral("TZID"), Qt::CaseInsensitive) == 0) {
      result.timeZone = parameter.sliced(equals + 1).trimmed();
    } else if (name.compare(QStringLiteral("VALUE"), Qt::CaseInsensitive) == 0 &&
               parameter.sliced(equals + 1)
                       .trimmed()
                       .compare(QStringLiteral("DATE"), Qt::CaseInsensitive) == 0) {
      result.valueDate = true;
    }
  }
  result.body = value.sliced(separator + 1).trimmed();
  return result;
}

QDate parseRecurrenceDate(const QString& value) {
  QDate date = QDate::fromString(value, Qt::ISODate);
  if (!date.isValid() && value.size() == 8) {
    date = QDate::fromString(value, QStringLiteral("yyyyMMdd"));
  }
  return date;
}

QDateTime parseRecurrenceDateTime(const QString& value) {
  QDateTime result = QDateTime::fromString(value, Qt::ISODateWithMs);
  if (!result.isValid()) {
    result = QDateTime::fromString(value, Qt::ISODate);
  }
  if (!result.isValid()) {
    static const QStringList formats = {
        QStringLiteral("yyyyMMdd'T'HHmmss.zzz'Z'"),
        QStringLiteral("yyyyMMdd'T'HHmmss'Z'"),
        QStringLiteral("yyyyMMdd'T'HHmmss.zzz"),
        QStringLiteral("yyyyMMdd'T'HHmmss"),
    };
    for (const QString& format : formats) {
      result = QDateTime::fromString(value, format);
      if (result.isValid()) {
        break;
      }
    }
  }
  return result;
}

bool hasExplicitOffset(const QString& value) {
  if (value.endsWith(QLatin1Char('Z'), Qt::CaseInsensitive)) {
    return true;
  }
  if (value.size() < 6) {
    return false;
  }
  const qsizetype sign = value.size() - 6;
  return (value.at(sign) == QLatin1Char('+') || value.at(sign) == QLatin1Char('-')) &&
         value.at(sign + 3) == QLatin1Char(':');
}

QString wallTimeKey(const QDateTime& value) {
  return value.date().toString(QStringLiteral("yyyy-MM-dd")) + QLatin1Char('T') +
         value.time().toString(QStringLiteral("HH:mm:ss.zzz"));
}

}  // namespace

QString providerKindToString(const ProviderKind kind) {
  switch (kind) {
    case ProviderKind::Local:
      return QStringLiteral("local");
    case ProviderKind::CalDav:
      return QStringLiteral("caldav");
    case ProviderKind::Google:
      return QStringLiteral("google");
    case ProviderKind::Ics:
      return QStringLiteral("ics");
    case ProviderKind::Unknown:
      return QStringLiteral("unknown");
  }
  return QStringLiteral("unknown");
}

ProviderKind providerKindFromString(const QString& value) {
  if (value.compare(QStringLiteral("local"), Qt::CaseInsensitive) == 0) {
    return ProviderKind::Local;
  }
  if (value.compare(QStringLiteral("caldav"), Qt::CaseInsensitive) == 0) {
    return ProviderKind::CalDav;
  }
  if (value.compare(QStringLiteral("google"), Qt::CaseInsensitive) == 0) {
    return ProviderKind::Google;
  }
  if (value.compare(QStringLiteral("ics"), Qt::CaseInsensitive) == 0) {
    return ProviderKind::Ics;
  }
  return ProviderKind::Unknown;
}

QString timeKindToString(const TimeKind kind) {
  switch (kind) {
    case TimeKind::Zoned:
      return QStringLiteral("zoned");
    case TimeKind::Floating:
      return QStringLiteral("floating");
    case TimeKind::AllDay:
      return QStringLiteral("all_day");
  }
  return QStringLiteral("zoned");
}

TimeKind timeKindFromString(const QString& value) {
  if (value == QStringLiteral("all_day")) {
    return TimeKind::AllDay;
  }
  if (value == QStringLiteral("floating")) {
    return TimeKind::Floating;
  }
  return TimeKind::Zoned;
}

QString outboxOperationToString(const OutboxOperation operation) {
  switch (operation) {
    case OutboxOperation::Create:
      return QStringLiteral("create");
    case OutboxOperation::Update:
      return QStringLiteral("update");
    case OutboxOperation::Move:
      return QStringLiteral("move");
    case OutboxOperation::Remove:
      return QStringLiteral("remove");
  }
  return QStringLiteral("create");
}

OutboxOperation outboxOperationFromString(const QString& value) {
  if (value == QStringLiteral("update")) {
    return OutboxOperation::Update;
  }
  if (value == QStringLiteral("move")) {
    return OutboxOperation::Move;
  }
  if (value == QStringLiteral("remove")) {
    return OutboxOperation::Remove;
  }
  return OutboxOperation::Create;
}

QString outboxStateToString(const OutboxState state) {
  switch (state) {
    case OutboxState::Pending:
      return QStringLiteral("pending");
    case OutboxState::Sending:
      return QStringLiteral("sending");
    case OutboxState::RetryWait:
      return QStringLiteral("retry_wait");
    case OutboxState::Blocked:
      return QStringLiteral("blocked");
    case OutboxState::Done:
      return QStringLiteral("done");
  }
  return QStringLiteral("pending");
}

OutboxState outboxStateFromString(const QString& value) {
  if (value == QStringLiteral("sending")) {
    return OutboxState::Sending;
  }
  if (value == QStringLiteral("retry_wait")) {
    return OutboxState::RetryWait;
  }
  if (value == QStringLiteral("blocked")) {
    return OutboxState::Blocked;
  }
  if (value == QStringLiteral("done")) {
    return OutboxState::Done;
  }
  return OutboxState::Pending;
}

QString newUuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QString isoUtc(const QDateTime& value) {
  if (!value.isValid()) {
    return QStringLiteral("");
  }
  return value.toUTC().toString(Qt::ISODateWithMs);
}

QDateTime dateTimeFromIso(const QString& value) {
  if (value.isEmpty()) {
    return {};
  }
  return QDateTime::fromString(value, Qt::ISODateWithMs).toUTC();
}

QString canonicalRecurrenceIdentity(const QString& value, const bool allDay,
                                    const TimeKind timeKind, const QString& timeZone) {
  const RecurrenceParts parts = splitRecurrenceParameters(value);
  if (parts.body.isEmpty()) {
    return {};
  }
  if (allDay || timeKind == TimeKind::AllDay || parts.valueDate) {
    const QDate date = parseRecurrenceDate(parts.body);
    return date.isValid() ? QStringLiteral("D:") + date.toString(Qt::ISODate)
                          : QStringLiteral("invalid:") + parts.body;
  }

  const QDateTime parsed = parseRecurrenceDateTime(parts.body);
  if (!parsed.isValid()) {
    return QStringLiteral("invalid:") + parts.body;
  }
  const bool floating = timeKind == TimeKind::Floating;
  const bool explicitOffset = hasExplicitOffset(parts.body);
  if (floating) {
    // Floating recurrence IDs are local wall times.  A UTC/offset-bearing
    // spelling is not interchangeable with a floating value, even if the
    // displayed clock fields happen to match.
    return QStringLiteral("F:") +
           (explicitOffset ? QStringLiteral("offset:") : QString()) +
           wallTimeKey(parsed);
  }

  const QString selectedTimeZone = parts.timeZone.isEmpty() ? timeZone : parts.timeZone;
  QDateTime zoned;
  if (explicitOffset) {
    // The basic RFC 5545 parser uses a literal 'Z' format and therefore
    // yields a LocalTime QDateTime.  Reattach UTC explicitly in that case;
    // otherwise a machine's local zone would corrupt the occurrence key.
    zoned = parts.body.endsWith(QLatin1Char('Z'), Qt::CaseInsensitive)
                ? QDateTime(parsed.date(), parsed.time(), QTimeZone::UTC)
                : parsed.toUTC();
  } else {
    if (selectedTimeZone.isEmpty()) {
      return QStringLiteral("F:") + wallTimeKey(parsed);
    }
    const QTimeZone zone(selectedTimeZone.toUtf8());
    if (!zone.isValid()) {
      return QStringLiteral("invalid:") + parts.body;
    }
    zoned = QDateTime(parsed.date(), parsed.time(), zone).toUTC();
  }
  return QStringLiteral("Z:") + zoned.toString(Qt::ISODateWithMs);
}

bool recurrenceIdentityEqual(const QString& left, const QString& right,
                             const bool allDay, const TimeKind timeKind,
                             const QString& timeZone) {
  if (left.trimmed().isEmpty() || right.trimmed().isEmpty()) {
    return left.trimmed().isEmpty() && right.trimmed().isEmpty();
  }
  return canonicalRecurrenceIdentity(left, allDay, timeKind, timeZone) ==
         canonicalRecurrenceIdentity(right, allDay, timeKind, timeZone);
}

bool recurrenceIdentityEqual(const Event& left, const Event& right) {
  if (left.allDay != right.allDay || left.timeKind != right.timeKind) {
    return false;
  }
  return recurrenceIdentityEqual(
      left.recurrenceId, right.recurrenceId, left.allDay,
      left.allDay
          ? TimeKind::AllDay
          : (left.timeKind == TimeKind::Floating || right.timeKind == TimeKind::Floating
                 ? TimeKind::Floating
                 : TimeKind::Zoned),
      left.startTimeZone.isEmpty() ? right.startTimeZone : left.startTimeZone);
}

QJsonObject toJson(const Account& account) {
  return {
      {QStringLiteral("id"), account.id},
      {QStringLiteral("provider"), providerKindToString(account.provider)},
      {QStringLiteral("displayName"), account.displayName},
      {QStringLiteral("principal"), account.principal},
      {QStringLiteral("enabled"), account.enabled},
      {QStringLiteral("authStatus"), account.authStatus},
      {QStringLiteral("createdAt"), isoUtc(account.createdAt)},
      {QStringLiteral("updatedAt"), isoUtc(account.updatedAt)},
  };
}

bool canDeleteCalendar(const Calendar& calendar) {
  if (calendar.capabilities.value(QStringLiteral("canDeleteCalendar")).toBool()) {
    return true;
  }
  return calendar.capabilities.value(QStringLiteral("provider")).toString() ==
             QStringLiteral("google") &&
         calendar.capabilities.value(QStringLiteral("accessRole")).toString() ==
             QStringLiteral("owner") &&
         !calendar.capabilities.value(QStringLiteral("primary")).toBool() &&
         !calendar.capabilities.value(QStringLiteral("deleted")).toBool();
}

QJsonObject toJson(const Calendar& calendar) {
  QJsonObject capabilities = presentationCapabilities(calendar.capabilities);
  capabilities.insert(QStringLiteral("canDeleteCalendar"), canDeleteCalendar(calendar));
  return {
      {QStringLiteral("id"), calendar.id},
      {QStringLiteral("accountId"), calendar.accountId},
      {QStringLiteral("name"), calendar.name},
      {QStringLiteral("description"), calendar.description},
      {QStringLiteral("color"), calendar.color},
      {QStringLiteral("timeZone"), calendar.timeZone},
      {QStringLiteral("readOnly"), calendar.readOnly},
      {QStringLiteral("enabled"), calendar.enabled},
      {QStringLiteral("colorOverride"), calendar.colorOverride},
      {QStringLiteral("position"), calendar.position},
      {QStringLiteral("ignoreAlerts"), calendar.ignoreAlerts},
      {QStringLiteral("capabilities"), capabilities},
      {QStringLiteral("lastSyncAt"), isoUtc(calendar.lastSyncAt)},
  };
}

QJsonObject toJson(const Event& event) {
  return {
      {QStringLiteral("id"), event.id},
      {QStringLiteral("calendarId"), event.calendarId},
      {QStringLiteral("summary"), event.summary},
      {QStringLiteral("description"), event.description},
      {QStringLiteral("location"), event.location},
      {QStringLiteral("url"), event.url},
      {QStringLiteral("conferenceUrl"), event.conferenceUrl},
      {QStringLiteral("startUtc"), isoUtc(event.startUtc)},
      {QStringLiteral("endUtc"), isoUtc(event.endUtc)},
      {QStringLiteral("startDate"), event.startDate.toString(Qt::ISODate)},
      {QStringLiteral("endDate"), event.endDate.toString(Qt::ISODate)},
      {QStringLiteral("startTimeZone"), event.startTimeZone},
      {QStringLiteral("endTimeZone"), event.endTimeZone},
      {QStringLiteral("allDay"), event.allDay},
      {QStringLiteral("timeKind"), timeKindToString(event.timeKind)},
      {QStringLiteral("status"), event.status},
      {QStringLiteral("transparency"), event.transparency},
      {QStringLiteral("visibility"), event.visibility},
      {QStringLiteral("recurrenceRule"), event.recurrenceRule},
      {QStringLiteral("recurrenceId"), event.recurrenceId},
      {QStringLiteral("sequence"), event.sequence},
      {QStringLiteral("organizer"), event.organizer},
      {QStringLiteral("attendees"), event.attendees},
      {QStringLiteral("reminders"), event.reminders},
      {QStringLiteral("dirty"), event.dirty},
      {QStringLiteral("deleted"), event.deleted},
      {QStringLiteral("localRevision"), event.localRevision},
      {QStringLiteral("syncState"), event.syncState},
      {QStringLiteral("createdAt"), isoUtc(event.createdAt)},
      {QStringLiteral("updatedAt"), isoUtc(event.updatedAt)},
  };
}

QJsonObject toStorageJson(const Event& event) {
  QJsonObject result = toJson(event);
  result.insert(QStringLiteral("remoteId"), event.remoteId);
  result.insert(QStringLiteral("uid"), event.uid);
  result.insert(QStringLiteral("etag"), event.etag);
  result.insert(QStringLiteral("rawPayload"), event.rawPayload);
  result.insert(QStringLiteral("rawFormat"), event.rawFormat);
  return result;
}

QJsonObject toJson(const OutboxItem& item) {
  return {
      {QStringLiteral("id"), item.id},
      {QStringLiteral("accountId"), item.accountId},
      {QStringLiteral("calendarId"), item.calendarId},
      {QStringLiteral("eventId"), item.eventId},
      {QStringLiteral("operation"), outboxOperationToString(item.operation)},
      {QStringLiteral("state"), outboxStateToString(item.state)},
      {QStringLiteral("clientMutationId"), item.idempotencyKey},
      {QStringLiteral("dependencyId"), item.dependencyId},
      {QStringLiteral("recurrenceScope"), item.recurrenceScope},
      {QStringLiteral("sendUpdates"), item.sendUpdates},
      {QStringLiteral("attempts"), item.attempts},
      {QStringLiteral("nextAttemptAt"), isoUtc(item.nextAttemptAt)},
      {QStringLiteral("notBefore"), isoUtc(item.notBefore)},
      {QStringLiteral("leaseUntil"), isoUtc(item.leaseUntil)},
      {QStringLiteral("errorCode"), item.errorCode},
      {QStringLiteral("errorMessage"), item.errorMessage},
      {QStringLiteral("createdAt"), isoUtc(item.createdAt)},
      {QStringLiteral("updatedAt"), isoUtc(item.updatedAt)},
  };
}

QJsonObject toJson(const CalendarSet& set) {
  QJsonArray calendarIds;
  for (const QString& id : set.calendarIds) {
    calendarIds.append(id);
  }
  return {{QStringLiteral("id"), set.id},
          {QStringLiteral("name"), set.name},
          {QStringLiteral("isDefault"), set.isDefault},
          {QStringLiteral("defaultCalendarId"), set.defaultCalendarId},
          {QStringLiteral("calendarIds"), calendarIds},
          {QStringLiteral("createdAt"), isoUtc(set.createdAt)},
          {QStringLiteral("updatedAt"), isoUtc(set.updatedAt)}};
}

QJsonObject toJson(const Conflict& conflict) {
  const QJsonObject local = conflict.localSnapshot.isEmpty()
                                ? QJsonObject()
                                : toJson(eventFromJson(conflict.localSnapshot));
  const QJsonObject remote = conflict.remoteSnapshot.isEmpty()
                                 ? QJsonObject()
                                 : toJson(eventFromJson(conflict.remoteSnapshot));
  return {{QStringLiteral("id"), conflict.id},
          {QStringLiteral("eventId"), conflict.eventId},
          {QStringLiteral("mutationId"), conflict.mutationId},
          {QStringLiteral("kind"), conflict.kind},
          {QStringLiteral("localSnapshot"), local},
          {QStringLiteral("remoteSnapshot"), remote},
          {QStringLiteral("state"), conflict.state},
          {QStringLiteral("resolutionRevision"), conflict.resolutionRevision},
          {QStringLiteral("createdAt"), isoUtc(conflict.createdAt)},
          {QStringLiteral("resolvedAt"), isoUtc(conflict.resolvedAt)}};
}

QJsonObject toJson(const ReminderJob& reminder) {
  return {{QStringLiteral("id"), reminder.id},
          {QStringLiteral("eventId"), reminder.eventId},
          {QStringLiteral("occurrenceId"), reminder.occurrenceId},
          {QStringLiteral("fireAt"), isoUtc(reminder.fireAt)},
          {QStringLiteral("snoozedUntil"), isoUtc(reminder.snoozedUntil)},
          {QStringLiteral("state"), reminder.state},
          {QStringLiteral("eventRevision"), reminder.eventRevision},
          {QStringLiteral("claimedAt"), isoUtc(reminder.claimedAt)},
          {QStringLiteral("leaseExpiresAt"), isoUtc(reminder.leaseExpiresAt)},
          {QStringLiteral("deliveredAt"), isoUtc(reminder.deliveredAt)}};
}

Account accountFromJson(const QJsonObject& object) {
  Account account;
  account.id = jsonString(object, "id");
  account.provider = providerKindFromString(jsonString(object, "provider"));
  account.displayName = jsonString(object, "displayName");
  account.principal = jsonString(object, "principal");
  account.endpoint = jsonString(object, "endpoint");
  account.enabled = object.value(QStringLiteral("enabled")).toBool(true);
  const QString authStatus = jsonString(object, "authStatus");
  if (!authStatus.isEmpty()) {
    account.authStatus = authStatus;
  }
  account.createdAt = dateTimeFromIso(jsonString(object, "createdAt"));
  account.updatedAt = dateTimeFromIso(jsonString(object, "updatedAt"));
  return account;
}

Calendar calendarFromJson(const QJsonObject& object) {
  Calendar calendar;
  calendar.id = jsonString(object, "id");
  calendar.accountId = jsonString(object, "accountId");
  calendar.remoteId = jsonString(object, "remoteId");
  calendar.href = jsonString(object, "href");
  calendar.name = jsonString(object, "name");
  calendar.description = jsonString(object, "description");
  const QString color = jsonString(object, "color");
  if (!color.isEmpty()) {
    calendar.color = color;
  }
  calendar.timeZone = jsonString(object, "timeZone");
  calendar.readOnly = object.value(QStringLiteral("readOnly")).toBool();
  calendar.enabled = object.value(QStringLiteral("enabled")).toBool(true);
  calendar.colorOverride = jsonString(object, "colorOverride");
  calendar.position = object.value(QStringLiteral("position")).toInt();
  calendar.ignoreAlerts = object.value(QStringLiteral("ignoreAlerts")).toBool();
  calendar.etag = jsonString(object, "etag");
  calendar.syncToken = jsonString(object, "syncToken");
  calendar.capabilities = object.value(QStringLiteral("capabilities")).toObject();
  calendar.lastSyncAt = dateTimeFromIso(jsonString(object, "lastSyncAt"));
  return calendar;
}

Event eventFromJson(const QJsonObject& object) {
  Event event;
  event.id = jsonString(object, "id");
  event.calendarId = jsonString(object, "calendarId");
  event.remoteId = jsonString(object, "remoteId");
  event.uid = jsonString(object, "uid");
  event.etag = jsonString(object, "etag");
  event.summary = jsonString(object, "summary");
  event.description = jsonString(object, "description");
  event.location = jsonString(object, "location");
  event.url = jsonString(object, "url");
  event.conferenceUrl = jsonString(object, "conferenceUrl");
  event.startUtc = dateTimeFromIso(jsonString(object, "startUtc"));
  event.endUtc = dateTimeFromIso(jsonString(object, "endUtc"));
  event.startDate = jsonDate(object, "startDate");
  event.endDate = jsonDate(object, "endDate");
  event.startTimeZone = jsonString(object, "startTimeZone");
  event.endTimeZone = jsonString(object, "endTimeZone");
  event.allDay = object.value(QStringLiteral("allDay")).toBool();
  event.timeKind = timeKindFromString(jsonString(object, "timeKind"));
  if (event.allDay) {
    event.timeKind = TimeKind::AllDay;
  }
  const QString status = jsonString(object, "status");
  if (!status.isEmpty()) {
    event.status = status;
  }
  const QString transparency = jsonString(object, "transparency");
  if (!transparency.isEmpty()) {
    event.transparency = transparency;
  }
  const QString visibility = jsonString(object, "visibility");
  if (!visibility.isEmpty()) {
    event.visibility = visibility;
  }
  event.recurrenceRule = jsonString(object, "recurrenceRule");
  event.recurrenceId = jsonString(object, "recurrenceId");
  event.sequence = object.value(QStringLiteral("sequence")).toInt();
  event.organizer = object.value(QStringLiteral("organizer")).toObject();
  event.attendees = object.value(QStringLiteral("attendees")).toArray();
  event.reminders = object.value(QStringLiteral("reminders")).toArray();
  event.rawPayload = jsonString(object, "rawPayload");
  event.rawFormat = jsonString(object, "rawFormat");
  event.dirty = object.value(QStringLiteral("dirty")).toBool();
  event.deleted = object.value(QStringLiteral("deleted")).toBool();
  event.localRevision = object.value(QStringLiteral("localRevision")).toInteger();
  const QString syncState = jsonString(object, "syncState");
  if (!syncState.isEmpty()) {
    event.syncState = syncState;
  }
  event.createdAt = dateTimeFromIso(jsonString(object, "createdAt"));
  event.updatedAt = dateTimeFromIso(jsonString(object, "updatedAt"));
  return event;
}

}  // namespace omacalendar
