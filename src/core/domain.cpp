#include "core/domain.h"

#include <QUuid>

namespace omacalendar {
namespace {

QString jsonString(const QJsonObject& object, const char* key) {
  return object.value(QLatin1StringView(key)).toString();
}

QDate jsonDate(const QJsonObject& object, const char* key) {
  return QDate::fromString(jsonString(object, key), Qt::ISODate);
}

}  // namespace

QString providerKindToString(const ProviderKind kind) {
  switch (kind) {
    case ProviderKind::CalDav:
      return QStringLiteral("caldav");
    case ProviderKind::Google:
      return QStringLiteral("google");
    case ProviderKind::Unknown:
      return QStringLiteral("unknown");
  }
  return QStringLiteral("unknown");
}

ProviderKind providerKindFromString(const QString& value) {
  if (value.compare(QStringLiteral("caldav"), Qt::CaseInsensitive) == 0) {
    return ProviderKind::CalDav;
  }
  if (value.compare(QStringLiteral("google"), Qt::CaseInsensitive) == 0) {
    return ProviderKind::Google;
  }
  return ProviderKind::Unknown;
}

QString outboxOperationToString(const OutboxOperation operation) {
  switch (operation) {
    case OutboxOperation::Create:
      return QStringLiteral("create");
    case OutboxOperation::Update:
      return QStringLiteral("update");
    case OutboxOperation::Remove:
      return QStringLiteral("remove");
  }
  return QStringLiteral("create");
}

OutboxOperation outboxOperationFromString(const QString& value) {
  if (value == QStringLiteral("update")) {
    return OutboxOperation::Update;
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

QJsonObject toJson(const Account& account) {
  return {
      {QStringLiteral("id"), account.id},
      {QStringLiteral("provider"), providerKindToString(account.provider)},
      {QStringLiteral("displayName"), account.displayName},
      {QStringLiteral("principal"), account.principal},
      {QStringLiteral("endpoint"), account.endpoint},
      {QStringLiteral("enabled"), account.enabled},
      {QStringLiteral("authStatus"), account.authStatus},
      {QStringLiteral("createdAt"), isoUtc(account.createdAt)},
      {QStringLiteral("updatedAt"), isoUtc(account.updatedAt)},
  };
}

QJsonObject toJson(const Calendar& calendar) {
  return {
      {QStringLiteral("id"), calendar.id},
      {QStringLiteral("accountId"), calendar.accountId},
      {QStringLiteral("remoteId"), calendar.remoteId},
      {QStringLiteral("href"), calendar.href},
      {QStringLiteral("name"), calendar.name},
      {QStringLiteral("description"), calendar.description},
      {QStringLiteral("color"), calendar.color},
      {QStringLiteral("timeZone"), calendar.timeZone},
      {QStringLiteral("readOnly"), calendar.readOnly},
      {QStringLiteral("enabled"), calendar.enabled},
      {QStringLiteral("etag"), calendar.etag},
      {QStringLiteral("syncToken"), calendar.syncToken},
      {QStringLiteral("capabilities"), calendar.capabilities},
      {QStringLiteral("lastSyncAt"), isoUtc(calendar.lastSyncAt)},
  };
}

QJsonObject toJson(const Event& event) {
  return {
      {QStringLiteral("id"), event.id},
      {QStringLiteral("calendarId"), event.calendarId},
      {QStringLiteral("remoteId"), event.remoteId},
      {QStringLiteral("uid"), event.uid},
      {QStringLiteral("etag"), event.etag},
      {QStringLiteral("summary"), event.summary},
      {QStringLiteral("description"), event.description},
      {QStringLiteral("location"), event.location},
      {QStringLiteral("startUtc"), isoUtc(event.startUtc)},
      {QStringLiteral("endUtc"), isoUtc(event.endUtc)},
      {QStringLiteral("startDate"), event.startDate.toString(Qt::ISODate)},
      {QStringLiteral("endDate"), event.endDate.toString(Qt::ISODate)},
      {QStringLiteral("startTimeZone"), event.startTimeZone},
      {QStringLiteral("endTimeZone"), event.endTimeZone},
      {QStringLiteral("allDay"), event.allDay},
      {QStringLiteral("status"), event.status},
      {QStringLiteral("transparency"), event.transparency},
      {QStringLiteral("recurrenceRule"), event.recurrenceRule},
      {QStringLiteral("recurrenceId"), event.recurrenceId},
      {QStringLiteral("sequence"), event.sequence},
      {QStringLiteral("organizer"), event.organizer},
      {QStringLiteral("attendees"), event.attendees},
      {QStringLiteral("reminders"), event.reminders},
      {QStringLiteral("rawPayload"), event.rawPayload},
      {QStringLiteral("rawFormat"), event.rawFormat},
      {QStringLiteral("dirty"), event.dirty},
      {QStringLiteral("deleted"), event.deleted},
      {QStringLiteral("createdAt"), isoUtc(event.createdAt)},
      {QStringLiteral("updatedAt"), isoUtc(event.updatedAt)},
  };
}

QJsonObject toJson(const OutboxItem& item) {
  return {
      {QStringLiteral("id"), item.id},
      {QStringLiteral("accountId"), item.accountId},
      {QStringLiteral("calendarId"), item.calendarId},
      {QStringLiteral("eventId"), item.eventId},
      {QStringLiteral("operation"), outboxOperationToString(item.operation)},
      {QStringLiteral("state"), outboxStateToString(item.state)},
      {QStringLiteral("idempotencyKey"), item.idempotencyKey},
      {QStringLiteral("expectedRevision"), item.expectedRevision},
      {QStringLiteral("attempts"), item.attempts},
      {QStringLiteral("nextAttemptAt"), isoUtc(item.nextAttemptAt)},
      {QStringLiteral("errorCode"), item.errorCode},
      {QStringLiteral("errorMessage"), item.errorMessage},
      {QStringLiteral("createdAt"), isoUtc(item.createdAt)},
      {QStringLiteral("updatedAt"), isoUtc(item.updatedAt)},
  };
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
  event.startUtc = dateTimeFromIso(jsonString(object, "startUtc"));
  event.endUtc = dateTimeFromIso(jsonString(object, "endUtc"));
  event.startDate = jsonDate(object, "startDate");
  event.endDate = jsonDate(object, "endDate");
  event.startTimeZone = jsonString(object, "startTimeZone");
  event.endTimeZone = jsonString(object, "endTimeZone");
  event.allDay = object.value(QStringLiteral("allDay")).toBool();
  const QString status = jsonString(object, "status");
  if (!status.isEmpty()) {
    event.status = status;
  }
  const QString transparency = jsonString(object, "transparency");
  if (!transparency.isEmpty()) {
    event.transparency = transparency;
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
  event.createdAt = dateTimeFromIso(jsonString(object, "createdAt"));
  event.updatedAt = dateTimeFromIso(jsonString(object, "updatedAt"));
  return event;
}

}  // namespace omacalendar
