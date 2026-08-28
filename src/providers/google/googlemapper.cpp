#include "providers/google/googlemapper.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QTimeZone>

namespace omacalendar::google {
namespace {

QString stringValue(const QJsonObject& object, const char* key) {
  return object.value(QLatin1StringView(key)).toString();
}

bool hasExplicitOffset(const QString& value) {
  static const QRegularExpression offsetExpression(
      QStringLiteral("(?:Z|[+-]\\d{2}:\\d{2})$"),
      QRegularExpression::CaseInsensitiveOption);
  return offsetExpression.match(value).hasMatch();
}

QDateTime parseRfc3339(const QString& value, const QString& timeZone) {
  if (value.isEmpty()) {
    return {};
  }

  QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
  if (!parsed.isValid()) {
    parsed = QDateTime::fromString(value, Qt::ISODate);
  }
  if (!parsed.isValid()) {
    return {};
  }

  if (!hasExplicitOffset(value)) {
    const QTimeZone zone(timeZone.toUtf8());
    parsed = QDateTime(parsed.date(), parsed.time(),
                       zone.isValid() ? zone : QTimeZone(QTimeZone::UTC));
  }
  return parsed.toUTC();
}

QJsonObject parsedRawPayload(const Event& event) {
  if (event.rawFormat != QStringLiteral("google-json") || event.rawPayload.isEmpty()) {
    return {};
  }
  const QJsonDocument document = QJsonDocument::fromJson(event.rawPayload.toUtf8());
  return document.isObject() ? document.object() : QJsonObject{};
}

QJsonObject sanitizedAttendee(const QJsonObject& attendee) {
  // These are the writable EventAttendee fields. In particular, do not send
  // response-only fields such as self and organizer back to Google.
  static constexpr const char* writableKeys[] = {
      "email",          "displayName", "optional",         "resource",
      "responseStatus", "comment",     "additionalGuests",
  };

  QJsonObject result;
  for (const char* key : writableKeys) {
    const QString name = QString::fromLatin1(key);
    if (attendee.contains(name) && !attendee.value(name).isNull() &&
        !attendee.value(name).isUndefined()) {
      result.insert(name, attendee.value(name));
    }
  }
  return result;
}

QJsonArray sanitizedAttendees(const QJsonArray& attendees) {
  QJsonArray result;
  for (const QJsonValue& value : attendees) {
    if (!value.isObject()) {
      continue;
    }
    const QJsonObject attendee = sanitizedAttendee(value.toObject());
    if (!stringValue(attendee, "email").isEmpty()) {
      result.append(attendee);
    }
  }
  return result;
}

QJsonArray sanitizedReminderOverrides(const QJsonArray& reminders) {
  QJsonArray result;
  for (const QJsonValue& value : reminders) {
    if (!value.isObject() || result.size() >= 5) {
      continue;
    }
    const QJsonObject reminder = value.toObject();
    const QString method = stringValue(reminder, "method");
    const int minutes = reminder.value(QStringLiteral("minutes")).toInt(-1);
    if ((method != QStringLiteral("popup") && method != QStringLiteral("email")) ||
        minutes < 0 || minutes > 40320) {
      continue;
    }
    result.append(QJsonObject{{QStringLiteral("method"), method},
                              {QStringLiteral("minutes"), minutes}});
  }
  return result;
}

QJsonArray recurrenceLines(const QString& recurrenceRule) {
  QJsonArray result;
  const QStringList lines = recurrenceRule.split(
      QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
  for (QString line : lines) {
    line = line.trimmed();
    if (!line.contains(QLatin1Char(':')) &&
        line.startsWith(QStringLiteral("FREQ="), Qt::CaseInsensitive)) {
      line.prepend(QStringLiteral("RRULE:"));
    }
    const QString upper = line.left(line.indexOf(QLatin1Char(':')) + 1).toUpper();
    if (upper == QStringLiteral("RRULE:") || upper == QStringLiteral("RDATE:") ||
        upper == QStringLiteral("EXRULE:") || upper == QStringLiteral("EXDATE:")) {
      result.append(upper + line.sliced(upper.size()));
    }
  }
  return result;
}

QJsonObject dateTimeBody(const QDateTime& utc, const QString& timeZone) {
  if (!utc.isValid()) {
    return {};
  }
  QJsonObject result{{QStringLiteral("dateTime"), rfc3339(utc, timeZone)}};
  if (!timeZone.isEmpty()) {
    const QTimeZone zone(timeZone.toUtf8());
    if (zone.isValid()) {
      result.insert(QStringLiteral("timeZone"), timeZone);
    }
  }
  return result;
}

void copyProviderWriteFields(const QJsonObject& raw, QJsonObject* result) {
  // Preserve optional writable Google-only data that has no canonical field.
  // The whitelist prevents response metadata (id, etag, htmlLink, timestamps,
  // organizer, recurringEventId, etc.) from leaking into write requests.
  static constexpr const char* keys[] = {
      "anyoneCanAddSelf",
      "attachments",
      "attendeesOmitted",
      "birthdayProperties",
      "colorId",
      "conferenceData",
      "eventLabelId",
      "eventType",
      "extendedProperties",
      "focusTimeProperties",
      "guestsCanInviteOthers",
      "guestsCanModify",
      "guestsCanSeeOtherGuests",
      "outOfOfficeProperties",
      "source",
      "visibility",
      "workingLocationProperties",
  };
  for (const char* key : keys) {
    const QString name = QString::fromLatin1(key);
    const QJsonValue value = raw.value(name);
    if (!value.isUndefined() && !value.isNull()) {
      result->insert(name, value);
    }
  }
}

QString joinedRecurrence(const QJsonValue& value) {
  if (!value.isArray()) {
    return {};
  }
  QStringList result;
  for (const QJsonValue& line : value.toArray()) {
    if (line.isString() && !line.toString().isEmpty()) {
      result.append(line.toString());
    }
  }
  return result.join(QLatin1Char('\n'));
}

}  // namespace

QString rfc3339(const QDateTime& dateTime, const QString& timeZone) {
  if (!dateTime.isValid()) {
    return {};
  }

  QDateTime value = dateTime.toUTC();
  if (!timeZone.isEmpty()) {
    const QTimeZone zone(timeZone.toUtf8());
    if (zone.isValid()) {
      value = value.toTimeZone(zone);
    }
  }

  // ISODateWithMs always emits seconds, a three-digit fractional part and an
  // explicit Z/offset for these aware QDateTime values, making retries and
  // payload comparisons deterministic.
  return value.toString(Qt::ISODateWithMs);
}

QString recurrenceInstanceId(const QJsonObject& originalStartTime) {
  const QString date = stringValue(originalStartTime, "date");
  if (!date.isEmpty()) {
    const QDate parsed = QDate::fromString(date, Qt::ISODate);
    return parsed.isValid() ? parsed.toString(Qt::ISODate) : date;
  }

  const QString dateTime = stringValue(originalStartTime, "dateTime");
  const QString timeZone = stringValue(originalStartTime, "timeZone");
  const QDateTime parsed = parseRfc3339(dateTime, timeZone);
  if (parsed.isValid()) {
    return rfc3339(parsed);
  }
  return dateTime;
}

Calendar calendarFromGoogleJson(const QJsonObject& resource, const QString& accountId) {
  Calendar calendar;
  calendar.accountId = accountId;
  calendar.remoteId = stringValue(resource, "id");
  calendar.etag = stringValue(resource, "etag");
  calendar.description = stringValue(resource, "description");
  calendar.timeZone = stringValue(resource, "timeZone");

  const QString summaryOverride = stringValue(resource, "summaryOverride");
  calendar.name =
      summaryOverride.isEmpty() ? stringValue(resource, "summary") : summaryOverride;
  const QString backgroundColor = stringValue(resource, "backgroundColor");
  if (!backgroundColor.isEmpty()) {
    calendar.color = backgroundColor;
  }

  const QString accessRole = stringValue(resource, "accessRole");
  calendar.readOnly = accessRole != QStringLiteral("owner") &&
                      accessRole != QStringLiteral("writer") &&
                      accessRole != QStringLiteral("writerWithoutPrivateAccess");
  const bool deleted = resource.value(QStringLiteral("deleted")).toBool(false);
  const bool hidden = resource.value(QStringLiteral("hidden")).toBool(false);
  // CalendarList defines an omitted `selected` field as false.
  const bool selected = resource.value(QStringLiteral("selected")).toBool(false);
  calendar.enabled = !deleted && !hidden && selected;

  calendar.capabilities = {
      {QStringLiteral("provider"), QStringLiteral("google")},
      {QStringLiteral("accessRole"), accessRole},
      {QStringLiteral("primary"),
       resource.value(QStringLiteral("primary")).toBool(false)},
      {QStringLiteral("selected"), selected},
      {QStringLiteral("hidden"), hidden},
      {QStringLiteral("deleted"), deleted},
      {QStringLiteral("foregroundColor"), stringValue(resource, "foregroundColor")},
      {QStringLiteral("colorId"), stringValue(resource, "colorId")},
      {QStringLiteral("defaultReminders"),
       resource.value(QStringLiteral("defaultReminders"))},
      {QStringLiteral("conferenceProperties"),
       resource.value(QStringLiteral("conferenceProperties"))},
      {QStringLiteral("notificationSettings"),
       resource.value(QStringLiteral("notificationSettings"))},
      {QStringLiteral("autoAcceptInvitations"),
       resource.value(QStringLiteral("autoAcceptInvitations"))},
      {QStringLiteral("dataOwner"), stringValue(resource, "dataOwner")},
      {QStringLiteral("location"), stringValue(resource, "location")},
  };
  return calendar;
}

Event eventFromGoogleJson(const QJsonObject& resource, const QString& calendarId) {
  Event event;
  event.calendarId = calendarId;
  event.remoteId = stringValue(resource, "id");
  event.uid = stringValue(resource, "iCalUID");
  event.etag = stringValue(resource, "etag");
  event.summary = stringValue(resource, "summary");
  event.description = stringValue(resource, "description");
  event.location = stringValue(resource, "location");

  const QString recurringEventId = stringValue(resource, "recurringEventId");
  if (event.uid.isEmpty()) {
    // Cancelled recurring exceptions may contain only the Google series id and
    // original start. Keeping that id as the UID preserves their identity.
    event.uid = recurringEventId.isEmpty() ? event.remoteId : recurringEventId;
  }

  const QJsonObject start = resource.value(QStringLiteral("start")).toObject();
  const QJsonObject end = resource.value(QStringLiteral("end")).toObject();
  event.allDay = start.contains(QStringLiteral("date"));
  if (event.allDay) {
    event.startDate = QDate::fromString(stringValue(start, "date"), Qt::ISODate);
    event.endDate = QDate::fromString(stringValue(end, "date"), Qt::ISODate);
  } else {
    event.startTimeZone = stringValue(start, "timeZone");
    event.endTimeZone = stringValue(end, "timeZone");
    if (event.endTimeZone.isEmpty()) {
      event.endTimeZone = event.startTimeZone;
    }
    event.startUtc = parseRfc3339(stringValue(start, "dateTime"), event.startTimeZone);
    event.endUtc = parseRfc3339(stringValue(end, "dateTime"), event.endTimeZone);
  }

  event.status = stringValue(resource, "status");
  if (event.status.isEmpty()) {
    event.status = QStringLiteral("confirmed");
  }
  event.transparency = stringValue(resource, "transparency");
  if (event.transparency.isEmpty()) {
    event.transparency = QStringLiteral("opaque");
  }
  event.deleted = event.status == QStringLiteral("cancelled");
  event.recurrenceRule = joinedRecurrence(resource.value(QStringLiteral("recurrence")));
  event.recurrenceId = recurrenceInstanceId(
      resource.value(QStringLiteral("originalStartTime")).toObject());
  event.sequence = resource.value(QStringLiteral("sequence")).toInt(0);
  event.organizer = resource.value(QStringLiteral("organizer")).toObject();
  event.attendees = resource.value(QStringLiteral("attendees")).toArray();

  const QJsonObject reminders = resource.value(QStringLiteral("reminders")).toObject();
  event.reminders = reminders.value(QStringLiteral("overrides")).toArray();
  event.rawPayload =
      QString::fromUtf8(QJsonDocument(resource).toJson(QJsonDocument::Compact));
  event.rawFormat = QStringLiteral("google-json");
  event.createdAt = parseRfc3339(stringValue(resource, "created"), {});
  event.updatedAt = parseRfc3339(stringValue(resource, "updated"), {});
  return event;
}

QJsonObject eventToGoogleJson(const Event& event) {
  const QJsonObject raw = parsedRawPayload(event);
  QJsonObject result;
  // A new event must never inherit conference entry points from a source
  // event: Google explicitly warns that reusing them can expose meeting
  // details. Existing remote events retain their provider-only writable data.
  if (!event.remoteId.isEmpty()) {
    copyProviderWriteFields(raw, &result);
  }

  // Insert the canonical editable fields even when empty: when this body is
  // used for an update, an intentionally cleared value must reach Google.
  result.insert(QStringLiteral("summary"), event.summary);
  result.insert(QStringLiteral("description"), event.description);
  result.insert(QStringLiteral("location"), event.location);

  if (event.allDay) {
    if (event.startDate.isValid()) {
      result.insert(
          QStringLiteral("start"),
          QJsonObject{{QStringLiteral("date"), event.startDate.toString(Qt::ISODate)}});
      const QDate endDate =
          event.endDate.isValid() ? event.endDate : event.startDate.addDays(1);
      result.insert(
          QStringLiteral("end"),
          QJsonObject{{QStringLiteral("date"), endDate.toString(Qt::ISODate)}});
    }
  } else if (event.startUtc.isValid() && event.endUtc.isValid()) {
    result.insert(QStringLiteral("start"),
                  dateTimeBody(event.startUtc, event.startTimeZone));
    const QString endTimeZone =
        event.endTimeZone.isEmpty() ? event.startTimeZone : event.endTimeZone;
    result.insert(QStringLiteral("end"), dateTimeBody(event.endUtc, endTimeZone));
  }

  const QString status = event.deleted ? QStringLiteral("cancelled") : event.status;
  if (status == QStringLiteral("confirmed") || status == QStringLiteral("tentative") ||
      status == QStringLiteral("cancelled")) {
    result.insert(QStringLiteral("status"), status);
  }
  if (event.transparency == QStringLiteral("opaque") ||
      event.transparency == QStringLiteral("transparent")) {
    result.insert(QStringLiteral("transparency"), event.transparency);
  }

  const QJsonArray recurrence = recurrenceLines(event.recurrenceRule);
  if (!recurrence.isEmpty()) {
    result.insert(QStringLiteral("recurrence"), recurrence);
  }
  if (event.sequence > 0) {
    result.insert(QStringLiteral("sequence"), event.sequence);
  }

  result.insert(QStringLiteral("attendees"), sanitizedAttendees(event.attendees));

  const QJsonArray reminders = sanitizedReminderOverrides(event.reminders);
  QJsonObject reminderBody;
  if (!reminders.isEmpty()) {
    reminderBody.insert(QStringLiteral("useDefault"), false);
    reminderBody.insert(QStringLiteral("overrides"), reminders);
  } else {
    const QJsonObject rawReminders = raw.value(QStringLiteral("reminders")).toObject();
    const bool useDefault =
        rawReminders.value(QStringLiteral("useDefault")).toBool(true);
    reminderBody.insert(QStringLiteral("useDefault"), useDefault);
    if (!useDefault) {
      reminderBody.insert(QStringLiteral("overrides"), QJsonArray{});
    }
  }
  result.insert(QStringLiteral("reminders"), reminderBody);

  return result;
}

}  // namespace omacalendar::google
