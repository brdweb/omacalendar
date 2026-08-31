#include "providers/google/googlemapper.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrl>

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
    const QTimeZone zone =
        timeZone.isEmpty() ? QTimeZone(QTimeZone::UTC) : QTimeZone(timeZone.toUtf8());
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
  if (!result.contains(QStringLiteral("responseStatus"))) {
    const QString partstat = stringValue(attendee, "partstat").toLower();
    if (partstat == QStringLiteral("accepted") ||
        partstat == QStringLiteral("tentative") ||
        partstat == QStringLiteral("declined") ||
        partstat == QStringLiteral("needsaction")) {
      result.insert(QStringLiteral("responseStatus"),
                    partstat == QStringLiteral("needsaction")
                        ? QStringLiteral("needsAction")
                        : partstat);
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

QString safeWebUrl(const QString& value, const bool requireHttps = false) {
  const QUrl url(value);
  const QString scheme = url.scheme().toLower();
  const bool permittedScheme = scheme == QStringLiteral("https") ||
                               (!requireHttps && scheme == QStringLiteral("http"));
  return url.isValid() && permittedScheme && !url.host().isEmpty()
             ? url.toString(QUrl::FullyEncoded)
             : QString{};
}

QString detectedMeetingUrl(const QString& text) {
  static const QRegularExpression urlExpression(
      QStringLiteral("https://[^\\s<>\\\"']+"),
      QRegularExpression::CaseInsensitiveOption);
  auto match = urlExpression.globalMatch(text);
  while (match.hasNext()) {
    QString candidate = match.next().captured();
    while (!candidate.isEmpty() &&
           QStringLiteral(".,;:!?)]}").contains(candidate.back())) {
      candidate.chop(1);
    }
    const QUrl url(candidate);
    const QString host = url.host().toLower();
    const bool knownProvider = host == QStringLiteral("meet.google.com") ||
                               host == QStringLiteral("teams.microsoft.com") ||
                               host == QStringLiteral("teams.live.com") ||
                               host == QStringLiteral("meet.jit.si") ||
                               host == QStringLiteral("whereby.com") ||
                               host == QStringLiteral("zoom.us") ||
                               host.endsWith(QStringLiteral(".zoom.us")) ||
                               host == QStringLiteral("webex.com") ||
                               host.endsWith(QStringLiteral(".webex.com"));
    if (knownProvider) {
      return safeWebUrl(candidate, true);
    }
  }
  return {};
}

QString conferenceUrl(const QJsonObject& resource) {
  const QString hangout = safeWebUrl(stringValue(resource, "hangoutLink"), true);
  if (!hangout.isEmpty()) {
    return hangout;
  }
  const QJsonArray entryPoints = resource.value(QStringLiteral("conferenceData"))
                                     .toObject()
                                     .value(QStringLiteral("entryPoints"))
                                     .toArray();
  QString fallback;
  for (const QJsonValue& value : entryPoints) {
    const QJsonObject entry = value.toObject();
    const QString uri = safeWebUrl(stringValue(entry, "uri"), true);
    if (uri.isEmpty()) {
      continue;
    }
    if (stringValue(entry, "entryPointType") == QStringLiteral("video")) {
      return uri;
    }
    if (fallback.isEmpty()) {
      fallback = uri;
    }
  }
  if (!fallback.isEmpty()) {
    return fallback;
  }
  const QString locationMeeting = detectedMeetingUrl(stringValue(resource, "location"));
  return locationMeeting.isEmpty()
             ? detectedMeetingUrl(stringValue(resource, "description"))
             : locationMeeting;
}

QJsonArray normalizedAttendees(const QJsonArray& attendees) {
  QJsonArray result;
  for (const QJsonValue& value : attendees) {
    if (!value.isObject()) {
      continue;
    }
    QJsonObject attendee = value.toObject();
    const QString response = stringValue(attendee, "responseStatus");
    if (!response.isEmpty()) {
      attendee.insert(QStringLiteral("partstat"), response.toUpper());
    }
    result.append(attendee);
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

QJsonObject floatingDateTimeBody(const QDateTime& wallTime) {
  if (!wallTime.isValid()) {
    return {};
  }
  // Floating Google date-times intentionally have neither a UTC offset nor a
  // timeZone field. The mapper stores their wall-clock components in the UTC
  // fields, so format those components directly instead of converting them.
  return {{QStringLiteral("dateTime"),
           wallTime.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz"))}};
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

GoogleOccurrenceMutationTarget googleOccurrenceMutationTarget(const Event& event) {
  if (event.recurrenceId.isEmpty()) {
    return {};
  }

  const QJsonObject raw = parsedRawPayload(event);
  const QString recurringEventId =
      raw.value(QStringLiteral("recurringEventId")).toString();
  const qsizetype syntheticSeparator = event.remoteId.indexOf(QLatin1Char('#'));
  if (syntheticSeparator >= 0) {
    const QString masterId = event.remoteId.left(syntheticSeparator);
    return {GoogleOccurrenceMutationTargetKind::ResolveInstance,
            masterId.isEmpty() ? recurringEventId : masterId};
  }

  if (!recurringEventId.isEmpty()) {
    if (!event.remoteId.isEmpty() && event.remoteId != recurringEventId) {
      return {GoogleOccurrenceMutationTargetKind::DirectInstance, event.remoteId};
    }
    return {GoogleOccurrenceMutationTargetKind::ResolveInstance, recurringEventId};
  }

  if (!event.remoteId.isEmpty()) {
    return {GoogleOccurrenceMutationTargetKind::ResolveInstance, event.remoteId};
  }
  return {};
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
  const bool primary = resource.value(QStringLiteral("primary")).toBool(false);
  calendar.enabled = !deleted && !hidden && selected;

  calendar.capabilities = {
      {QStringLiteral("provider"), QStringLiteral("google")},
      {QStringLiteral("accessRole"), accessRole},
      {QStringLiteral("primary"), primary},
      {QStringLiteral("canDeleteCalendar"),
       accessRole == QStringLiteral("owner") && !primary && !deleted},
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

Event eventFromGoogleJson(const QJsonObject& resource, const QString& calendarId,
                          const QJsonArray& calendarDefaultReminders) {
  Event event;
  event.calendarId = calendarId;
  event.remoteId = stringValue(resource, "id");
  event.uid = stringValue(resource, "iCalUID");
  event.etag = stringValue(resource, "etag");
  event.summary = stringValue(resource, "summary");
  event.description = stringValue(resource, "description");
  event.location = stringValue(resource, "location");
  event.url = safeWebUrl(resource.value(QStringLiteral("source"))
                             .toObject()
                             .value(QStringLiteral("url"))
                             .toString());
  event.conferenceUrl = conferenceUrl(resource);

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
    event.timeKind = TimeKind::AllDay;
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
    const bool floating = event.startTimeZone.isEmpty() &&
                          !hasExplicitOffset(stringValue(start, "dateTime"));
    event.timeKind = floating ? TimeKind::Floating : TimeKind::Zoned;
  }

  event.status = stringValue(resource, "status");
  if (event.status.isEmpty()) {
    event.status = QStringLiteral("confirmed");
  }
  event.transparency = stringValue(resource, "transparency");
  if (event.transparency.isEmpty()) {
    event.transparency = QStringLiteral("opaque");
  }
  event.visibility = stringValue(resource, "visibility");
  if (event.visibility.isEmpty()) {
    event.visibility = QStringLiteral("default");
  }
  event.deleted = event.status == QStringLiteral("cancelled");
  event.recurrenceRule = joinedRecurrence(resource.value(QStringLiteral("recurrence")));
  event.recurrenceId = recurrenceInstanceId(
      resource.value(QStringLiteral("originalStartTime")).toObject());
  event.sequence = resource.value(QStringLiteral("sequence")).toInt(0);
  event.organizer = resource.value(QStringLiteral("organizer")).toObject();
  event.attendees =
      normalizedAttendees(resource.value(QStringLiteral("attendees")).toArray());

  const QJsonObject reminders = resource.value(QStringLiteral("reminders")).toObject();
  if (reminders.value(QStringLiteral("useDefault")).toBool(false)) {
    for (const QJsonValue& value : calendarDefaultReminders) {
      if (!value.isObject()) {
        continue;
      }
      QJsonObject inherited = value.toObject();
      inherited.insert(QStringLiteral("providerDefault"), true);
      event.reminders.append(inherited);
    }
  } else {
    event.reminders = reminders.value(QStringLiteral("overrides")).toArray();
  }
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

  const QJsonObject rawSource = raw.value(QStringLiteral("source")).toObject();
  const QString rawSourceUrl = safeWebUrl(stringValue(rawSource, "url"));
  if (event.remoteId.isEmpty() || event.url != rawSourceUrl) {
    if (event.url.isEmpty()) {
      result.insert(QStringLiteral("source"), QJsonValue::Null);
    } else {
      const QString writableUrl = safeWebUrl(event.url);
      if (!writableUrl.isEmpty()) {
        QJsonObject source{{QStringLiteral("url"), writableUrl}};
        const QString title = stringValue(rawSource, "title");
        source.insert(QStringLiteral("title"), title.isEmpty() ? event.summary : title);
        result.insert(QStringLiteral("source"), source);
      }
    }
  }

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
    if (event.timeKind == TimeKind::Floating) {
      result.insert(QStringLiteral("start"), floatingDateTimeBody(event.startUtc));
      result.insert(QStringLiteral("end"), floatingDateTimeBody(event.endUtc));
    } else {
      result.insert(QStringLiteral("start"),
                    dateTimeBody(event.startUtc, event.startTimeZone));
      const QString endTimeZone =
          event.endTimeZone.isEmpty() ? event.startTimeZone : event.endTimeZone;
      result.insert(QStringLiteral("end"), dateTimeBody(event.endUtc, endTimeZone));
    }
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
  if (event.visibility == QStringLiteral("default") ||
      event.visibility == QStringLiteral("public") ||
      event.visibility == QStringLiteral("private") ||
      event.visibility == QStringLiteral("confidential")) {
    result.insert(QStringLiteral("visibility"), event.visibility);
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
  const QJsonObject rawReminders = raw.value(QStringLiteral("reminders")).toObject();
  bool inheritedProviderDefaults =
      !event.reminders.isEmpty() &&
      rawReminders.value(QStringLiteral("useDefault")).toBool(false);
  for (const QJsonValue& value : event.reminders) {
    if (!value.isObject() ||
        !value.toObject().value(QStringLiteral("providerDefault")).toBool(false)) {
      inheritedProviderDefaults = false;
      break;
    }
  }
  if (inheritedProviderDefaults) {
    reminderBody.insert(QStringLiteral("useDefault"), true);
  } else if (!reminders.isEmpty()) {
    reminderBody.insert(QStringLiteral("useDefault"), false);
    reminderBody.insert(QStringLiteral("overrides"), reminders);
  } else {
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

QString eventIdForClientMutation(const QString& clientMutationId) {
  if (clientMutationId.isEmpty()) {
    return {};
  }
  // Google event ids allow base32hex characters. A lowercase hex digest is a
  // valid subset and avoids leaking the user-provided mutation id itself.
  return QString::fromLatin1(QCryptographicHash::hash(clientMutationId.toUtf8(),
                                                      QCryptographicHash::Sha256)
                                 .toHex())
      .left(32);
}

QJsonObject eventToGoogleCreateJson(const Event& event,
                                    const QString& clientMutationId) {
  QJsonObject payload = eventToGoogleJson(event);
  const QString remoteId = eventIdForClientMutation(clientMutationId);
  if (remoteId.isEmpty()) {
    return payload;
  }
  payload.insert(QStringLiteral("id"), remoteId);
  QJsonObject extended = payload.value(QStringLiteral("extendedProperties")).toObject();
  QJsonObject privateProperties = extended.value(QStringLiteral("private")).toObject();
  privateProperties.insert(QStringLiteral("omacalendarMutationId"), clientMutationId);
  extended.insert(QStringLiteral("private"), privateProperties);
  payload.insert(QStringLiteral("extendedProperties"), extended);
  return payload;
}

bool googleEventHasMutationIdentity(const QJsonObject& resource,
                                    const QString& clientMutationId) {
  return !clientMutationId.isEmpty() &&
         resource.value(QStringLiteral("extendedProperties"))
                 .toObject()
                 .value(QStringLiteral("private"))
                 .toObject()
                 .value(QStringLiteral("omacalendarMutationId"))
                 .toString() == clientMutationId;
}

QJsonObject rsvpPatchForGoogleEvent(const Event& event) {
  const QJsonObject raw = parsedRawPayload(event);
  if (event.remoteId.isEmpty() || raw.isEmpty()) {
    return {};
  }

  Event baseline = eventFromGoogleJson(raw, event.calendarId);
  baseline.id = event.id;
  QJsonObject beforeBody = eventToGoogleJson(baseline);
  QJsonObject afterBody = eventToGoogleJson(event);
  const QJsonArray beforeAttendees =
      beforeBody.take(QStringLiteral("attendees")).toArray();
  const QJsonArray afterAttendees =
      afterBody.take(QStringLiteral("attendees")).toArray();
  if (beforeBody != afterBody || beforeAttendees.size() != afterAttendees.size()) {
    return {};
  }

  QJsonObject changed;
  int changedCount = 0;
  for (const QJsonValue& value : afterAttendees) {
    const QJsonObject candidate = value.toObject();
    const QString email = stringValue(candidate, "email");
    auto matching = beforeAttendees.constBegin();
    for (; matching != beforeAttendees.constEnd(); ++matching) {
      if (stringValue(matching->toObject(), "email")
              .compare(email, Qt::CaseInsensitive) == 0) {
        break;
      }
    }
    if (matching == beforeAttendees.constEnd()) {
      return {};
    }
    const QJsonObject previous = matching->toObject();
    if (previous == candidate) {
      continue;
    }
    QJsonObject previousIdentity = previous;
    QJsonObject candidateIdentity = candidate;
    previousIdentity.remove(QStringLiteral("responseStatus"));
    candidateIdentity.remove(QStringLiteral("responseStatus"));
    if (previousIdentity != candidateIdentity) {
      return {};
    }
    const QString nextResponse = stringValue(candidate, "responseStatus");
    if (nextResponse != QStringLiteral("accepted") &&
        nextResponse != QStringLiteral("tentative") &&
        nextResponse != QStringLiteral("declined")) {
      return {};
    }
    bool self = false;
    for (const QJsonValue& rawValue :
         raw.value(QStringLiteral("attendees")).toArray()) {
      const QJsonObject rawAttendee = rawValue.toObject();
      if (stringValue(rawAttendee, "email").compare(email, Qt::CaseInsensitive) == 0) {
        self = rawAttendee.value(QStringLiteral("self")).toBool(false);
        break;
      }
    }
    if (!self) {
      return {};
    }
    changed = candidate;
    ++changedCount;
  }
  if (changedCount != 1) {
    return {};
  }
  return {{QStringLiteral("attendees"), QJsonArray{changed}},
          {QStringLiteral("attendeesOmitted"), true}};
}

}  // namespace omacalendar::google
