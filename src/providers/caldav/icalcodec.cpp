#include "providers/caldav/icalcodec.h"

#include <libical/ical.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QTimeZone>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace omacalendar::caldav {
namespace {

struct ComponentDeleter {
  void operator()(icalcomponent* component) const {
    if (component != nullptr) {
      icalcomponent_free(component);
    }
  }
};

using ComponentPtr = std::unique_ptr<icalcomponent, ComponentDeleter>;
constexpr qsizetype kMaximumCalendarBytes = 16 * 1024 * 1024;

ICalendarError serializationError(const QString& code, const QString& message);

QString fromIcal(const char* value) {
  return value == nullptr ? QString() : QString::fromUtf8(value);
}

QString propertyText(icalcomponent* component, icalproperty_kind kind) {
  icalproperty* property = icalcomponent_get_first_property(component, kind);
  if (property == nullptr) {
    return {};
  }
  return fromIcal(icalproperty_get_value_as_string(property));
}

QString textProperty(icalcomponent* component, icalproperty_kind kind) {
  icalproperty* property = icalcomponent_get_first_property(component, kind);
  if (property == nullptr) {
    return {};
  }
  switch (kind) {
    case ICAL_UID_PROPERTY:
      return fromIcal(icalproperty_get_uid(property));
    case ICAL_SUMMARY_PROPERTY:
      return fromIcal(icalproperty_get_summary(property));
    case ICAL_DESCRIPTION_PROPERTY:
      return fromIcal(icalproperty_get_description(property));
    case ICAL_LOCATION_PROPERTY:
      return fromIcal(icalproperty_get_location(property));
    default:
      return propertyText(component, kind);
  }
}

QString timeZoneId(icalproperty* property) {
  if (property == nullptr) {
    return {};
  }
  icalparameter* parameter =
      icalproperty_get_first_parameter(property, ICAL_TZID_PARAMETER);
  return parameter == nullptr ? QString() : fromIcal(icalparameter_get_tzid(parameter));
}

bool isUtcZone(const QString& timeZone) {
  return timeZone.compare(QStringLiteral("UTC"), Qt::CaseInsensitive) == 0 ||
         timeZone.compare(QStringLiteral("Etc/UTC"), Qt::CaseInsensitive) == 0 ||
         timeZone.compare(QStringLiteral("GMT"), Qt::CaseInsensitive) == 0 ||
         timeZone == QStringLiteral("Z");
}

QString normalizedRecurrenceId(const Event& event) {
  QString recurrence = event.recurrenceId.trimmed();
  if (recurrence.isEmpty()) {
    return {};
  }

  QStringList parameters;
  QString value = recurrence;
  if (recurrence.startsWith(QStringLiteral("RECURRENCE-ID"), Qt::CaseInsensitive)) {
    const qsizetype separator = recurrence.indexOf(QLatin1Char(':'));
    if (separator < 0) {
      return recurrence;
    }
    const QString property = recurrence.left(separator);
    parameters = property.section(QLatin1Char(';'), 1)
                     .split(QLatin1Char(';'), Qt::SkipEmptyParts);
    value = recurrence.sliced(separator + 1);
  } else if (recurrence.startsWith(QStringLiteral("TZID="), Qt::CaseInsensitive) ||
             recurrence.startsWith(QStringLiteral("RANGE="), Qt::CaseInsensitive)) {
    const qsizetype separator = recurrence.indexOf(QLatin1Char(':'));
    if (separator < 0) {
      return recurrence;
    }
    parameters = recurrence.left(separator).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    value = recurrence.sliced(separator + 1);
  }

  // Presentation occurrence references use ISO 8601 while iCalendar requires
  // the basic RFC 5545 representation. Existing provider-native values pass
  // through unchanged.
  if (value.contains(QLatin1Char('-'))) {
    if (event.allDay) {
      const QDate date = QDate::fromString(value, Qt::ISODate);
      if (date.isValid()) {
        value = date.toString(QStringLiteral("yyyyMMdd"));
      }
    } else {
      QDateTime instant = QDateTime::fromString(value, Qt::ISODateWithMs);
      if (!instant.isValid()) {
        instant = QDateTime::fromString(value, Qt::ISODate);
      }
      if (instant.isValid()) {
        QString recurrenceZone;
        for (const QString& parameter : std::as_const(parameters)) {
          if (parameter.startsWith(QStringLiteral("TZID="), Qt::CaseInsensitive)) {
            recurrenceZone = parameter.sliced(5);
            break;
          }
        }
        if (recurrenceZone.isEmpty() && event.timeKind == TimeKind::Zoned &&
            !event.startTimeZone.isEmpty() && !isUtcZone(event.startTimeZone)) {
          recurrenceZone = event.startTimeZone;
          parameters.prepend(QStringLiteral("TZID=") + recurrenceZone);
        }
        if (!recurrenceZone.isEmpty()) {
          const QTimeZone zone(recurrenceZone.toUtf8());
          if (zone.isValid()) {
            instant = instant.toTimeZone(zone);
          }
          value = instant.toString(QStringLiteral("yyyyMMdd'T'HHmmss"));
        } else if (event.timeKind == TimeKind::Floating) {
          value = instant.time().isValid()
                      ? instant.toString(QStringLiteral("yyyyMMdd'T'HHmmss"))
                      : value;
        } else {
          value = instant.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
        }
      }
    }
  }
  return parameters.isEmpty()
             ? value
             : parameters.join(QLatin1Char(';')) + QLatin1Char(':') + value;
}

struct ParsedTime {
  bool valid = false;
  bool dateOnly = false;
  QDate date;
  QDateTime utc;
  QString timeZone;
};

ParsedTime parseTime(icalproperty* property, icalcomponent* calendar) {
  ParsedTime result;
  if (property == nullptr) {
    return result;
  }

  const icalproperty_kind kind = icalproperty_isa(property);
  icaltimetype value = icaltime_null_time();
  if (kind == ICAL_DTSTART_PROPERTY) {
    value = icalproperty_get_dtstart(property);
  } else if (kind == ICAL_DTEND_PROPERTY) {
    value = icalproperty_get_dtend(property);
  } else if (kind == ICAL_RECURRENCEID_PROPERTY) {
    value = icalproperty_get_recurrenceid(property);
  } else {
    return result;
  }
  if (icaltime_is_null_time(value) || !icaltime_is_valid_time(value)) {
    return result;
  }

  result.date = QDate(value.year, value.month, value.day);
  if (!result.date.isValid()) {
    return result;
  }
  result.valid = true;
  result.dateOnly = icaltime_is_date(value);
  result.timeZone = timeZoneId(property);
  if (result.dateOnly) {
    return result;
  }

  const QTime time(value.hour, value.minute, value.second);
  if (!time.isValid()) {
    result.valid = false;
    return result;
  }

  if (icaltime_is_utc(value) || isUtcZone(result.timeZone)) {
    result.utc = QDateTime(result.date, time, QTimeZone::UTC);
    if (result.timeZone.isEmpty()) {
      result.timeZone = QStringLiteral("UTC");
    }
    return result;
  }

  if (!result.timeZone.isEmpty()) {
    const QTimeZone qtZone(result.timeZone.toUtf8());
    if (qtZone.isValid()) {
      result.utc = QDateTime(result.date, time, qtZone).toUTC();
      return result;
    }

    const QByteArray zoneName = result.timeZone.toUtf8();
    icaltimezone* icalZone = icalcomponent_get_timezone(calendar, zoneName.constData());
    if (icalZone == nullptr) {
      icalZone = icaltimezone_get_builtin_timezone(zoneName.constData());
    }
    if (icalZone == nullptr) {
      icalZone = icaltimezone_get_builtin_timezone_from_tzid(zoneName.constData());
    }
    if (icalZone != nullptr) {
      const icaltime_t epoch = icaltime_as_timet_with_zone(value, icalZone);
      result.utc =
          QDateTime::fromSecsSinceEpoch(static_cast<qint64>(epoch), QTimeZone::UTC);
      return result;
    }
  }

  // RFC 5545 floating times have no absolute instant. The desktop's current
  // zone is the least surprising interpretation for the canonical UTC cache;
  // the empty source TZID is retained so the distinction is not lost.
  result.utc = QDateTime(result.date, time, QTimeZone::systemTimeZone()).toUTC();
  return result;
}

qint64 durationSeconds(const icaldurationtype& duration) {
  qint64 seconds = static_cast<qint64>(duration.weeks) * 7 * 24 * 60 * 60;
  seconds += static_cast<qint64>(duration.days) * 24 * 60 * 60;
  seconds += static_cast<qint64>(duration.hours) * 60 * 60;
  seconds += static_cast<qint64>(duration.minutes) * 60;
  seconds += duration.seconds;
  return duration.is_neg ? -seconds : seconds;
}

struct StructuredReminder {
  QString method;
  std::optional<int> minutesBefore;
  QDateTime absoluteUtc;
};

std::optional<int> reminderMinutes(const QJsonValue& value) {
  if (!value.isDouble()) {
    return std::nullopt;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::trunc(number) != number ||
      number < static_cast<double>(std::numeric_limits<int>::min()) ||
      number > static_cast<double>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  const qint64 minutes = static_cast<qint64>(number);
  if (minutes < std::numeric_limits<int>::min() / 60 ||
      minutes > std::numeric_limits<int>::max() / 60) {
    return std::nullopt;
  }
  return static_cast<int>(minutes);
}

QString normalizedReminderMethod(const QString& method) {
  const QString normalized = method.trimmed().toLower();
  if (normalized.isEmpty() || normalized == QStringLiteral("popup") ||
      normalized == QStringLiteral("display")) {
    return QStringLiteral("popup");
  }
  if (normalized == QStringLiteral("email") || normalized == QStringLiteral("audio")) {
    return normalized;
  }
  return {};
}

std::optional<StructuredReminder> structuredReminder(const QJsonValue& value,
                                                     QString* errorMessage) {
  StructuredReminder result;
  if (value.isDouble()) {
    result.method = QStringLiteral("popup");
    result.minutesBefore = reminderMinutes(value);
    if (!result.minutesBefore.has_value() && errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("A reminder offset must be a whole number of minutes");
    }
    return result.minutesBefore.has_value() ? std::optional<StructuredReminder>(result)
                                            : std::nullopt;
  }
  if (!value.isObject()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("A reminder must be a minute offset or an object");
    }
    return std::nullopt;
  }

  const QJsonObject object = value.toObject();
  result.method =
      normalizedReminderMethod(object.value(QStringLiteral("method")).toString());
  if (result.method.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("A CalDAV reminder method must be popup, email, or audio");
    }
    return std::nullopt;
  }

  const QString absolute = object.value(QStringLiteral("at")).toString().trimmed();
  if (!absolute.isEmpty()) {
    result.absoluteUtc = dateTimeFromIso(absolute);
    if (!result.absoluteUtc.isValid()) {
      if (errorMessage != nullptr) {
        *errorMessage =
            QStringLiteral("An absolute reminder needs a valid ISO 8601 timestamp");
      }
      return std::nullopt;
    }
    result.absoluteUtc = result.absoluteUtc.toUTC();
    return result;
  }

  constexpr const char* kMinuteKeys[] = {"minutesBefore", "offsetMinutes", "minutes",
                                         "min"};
  for (const char* key : kMinuteKeys) {
    const QString name = QString::fromLatin1(key);
    if (!object.contains(name)) {
      continue;
    }
    result.minutesBefore = reminderMinutes(object.value(name));
    break;
  }
  if (!result.minutesBefore.has_value()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "A reminder needs a whole-number minute offset or an absolute timestamp");
    }
    return std::nullopt;
  }
  return result;
}

QString reminderMethod(icalcomponent* alarm) {
  icalproperty* action = icalcomponent_get_first_property(alarm, ICAL_ACTION_PROPERTY);
  if (action == nullptr) {
    return {};
  }
  switch (icalproperty_get_action(action)) {
    case ICAL_ACTION_DISPLAY:
      return QStringLiteral("popup");
    case ICAL_ACTION_EMAIL:
      return QStringLiteral("email");
    case ICAL_ACTION_AUDIO:
      return QStringLiteral("audio");
    default:
      return {};
  }
}

std::optional<QJsonObject> reminderFromAlarm(icalcomponent* alarm) {
  const QString method = reminderMethod(alarm);
  icalproperty* trigger =
      icalcomponent_get_first_property(alarm, ICAL_TRIGGER_PROPERTY);
  if (method.isEmpty() || trigger == nullptr) {
    return std::nullopt;
  }

  QJsonObject result{{QStringLiteral("method"), method}};
  const QString encoded = propertyText(alarm, ICAL_TRIGGER_PROPERTY).trimmed();
  const QString upper = encoded.toUpper();
  if (upper.startsWith(QStringLiteral("P")) || upper.startsWith(QStringLiteral("+P")) ||
      upper.startsWith(QStringLiteral("-P"))) {
    if (icalparameter* related =
            icalproperty_get_first_parameter(trigger, ICAL_RELATED_PARAMETER);
        related != nullptr &&
        icalparameter_get_related(related) != ICAL_RELATED_START) {
      // The canonical scheduler currently measures relative alarms from the
      // event start. Keep end-relative/provider-specific alarms only in the
      // retained resource rather than scheduling them at the wrong instant.
      return std::nullopt;
    }
    const icaldurationtype duration =
        icaldurationtype_from_string(encoded.toLatin1().constData());
    if (icaldurationtype_is_bad_duration(duration)) {
      return std::nullopt;
    }
    const qint64 seconds = durationSeconds(duration);
    if (seconds % 60 != 0) {
      return std::nullopt;
    }
    const qint64 minutes = -seconds / 60;
    if (minutes < std::numeric_limits<int>::min() ||
        minutes > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    result.insert(QStringLiteral("minutes"), static_cast<int>(minutes));
    return result;
  }

  const icaltriggertype value = icalproperty_get_trigger(trigger);
  if (icaltime_is_null_time(value.time) || !icaltime_is_valid_time(value.time) ||
      !icaltime_is_utc(value.time)) {
    // RFC 5545 absolute triggers are UTC. Do not guess a zone for malformed
    // or provider-specific values.
    return std::nullopt;
  }
  const icaltime_t epoch =
      icaltime_as_timet_with_zone(value.time, icaltimezone_get_utc_timezone());
  const QDateTime absoluteUtc =
      QDateTime::fromSecsSinceEpoch(static_cast<qint64>(epoch), QTimeZone::UTC);
  if (!absoluteUtc.isValid()) {
    return std::nullopt;
  }
  result.insert(QStringLiteral("at"), isoUtc(absoluteUtc));
  return result;
}

QString recurrenceId(icalcomponent* component) {
  icalproperty* property =
      icalcomponent_get_first_property(component, ICAL_RECURRENCEID_PROPERTY);
  if (property == nullptr) {
    return {};
  }
  const icaltimetype value = icalproperty_get_recurrenceid(property);
  if (icaltime_is_null_time(value) || !icaltime_is_valid_time(value)) {
    return propertyText(component, ICAL_RECURRENCEID_PROPERTY);
  }
  char* encoded = icaltime_as_ical_string_r(value);
  if (encoded == nullptr) {
    return {};
  }
  const QString valueText = QString::fromUtf8(encoded);
  icalmemory_free_buffer(encoded);
  const QString zone = timeZoneId(property);
  QStringList parameters;
  if (!zone.isEmpty()) {
    parameters.append(QStringLiteral("TZID=") + zone);
  }
  if (icalparameter* range =
          icalproperty_get_first_parameter(property, ICAL_RANGE_PARAMETER);
      range != nullptr && icalparameter_get_range(range) == ICAL_RANGE_THISANDFUTURE) {
    parameters.append(QStringLiteral("RANGE=THISANDFUTURE"));
  }
  return parameters.isEmpty()
             ? valueText
             : parameters.join(QLatin1Char(';')) + QLatin1Char(':') + valueText;
}

QString calendarAddressEmail(const QString& address) {
  const QString trimmed = address.trimmed();
  return trimmed.startsWith(QStringLiteral("mailto:"), Qt::CaseInsensitive)
             ? trimmed.mid(7)
             : QString();
}

QString attendeeIdentity(const QJsonObject& attendee) {
  QString address = attendee.value(QStringLiteral("uri")).toString().trimmed();
  if (address.isEmpty()) {
    address = attendee.value(QStringLiteral("email")).toString().trimmed();
    if (!address.isEmpty()) {
      address.prepend(QStringLiteral("mailto:"));
    }
  }
  if (address.startsWith(QStringLiteral("mailto:"), Qt::CaseInsensitive)) {
    return QStringLiteral("mailto:") + address.mid(7).toCaseFolded();
  }
  return address.toCaseFolded();
}

QString parameterValue(icalproperty* property, const char* name) {
  return fromIcal(icalproperty_get_parameter_as_string(property, name));
}

QString responseStatus(const QString& partstat) {
  const QString normalized = partstat.trimmed().toUpper();
  if (normalized == QStringLiteral("NEEDS-ACTION")) {
    return QStringLiteral("needsAction");
  }
  if (normalized == QStringLiteral("ACCEPTED")) {
    return QStringLiteral("accepted");
  }
  if (normalized == QStringLiteral("TENTATIVE")) {
    return QStringLiteral("tentative");
  }
  if (normalized == QStringLiteral("DECLINED")) {
    return QStringLiteral("declined");
  }
  if (normalized == QStringLiteral("DELEGATED")) {
    return QStringLiteral("delegated");
  }
  return normalized.toLower();
}

QJsonObject calendarUser(icalproperty* property, const bool organizer) {
  const QString address = organizer ? fromIcal(icalproperty_get_organizer(property))
                                    : fromIcal(icalproperty_get_attendee(property));
  QJsonObject result{{QStringLiteral("uri"), address}};
  const QString email = calendarAddressEmail(address);
  if (!email.isEmpty()) {
    result.insert(QStringLiteral("email"), email);
  }
  const QString name = parameterValue(property, "CN");
  if (!name.isEmpty()) {
    result.insert(QStringLiteral("displayName"), name);
  }
  if (organizer) {
    return result;
  }
  const QString partstat = parameterValue(property, "PARTSTAT").toUpper();
  if (!partstat.isEmpty()) {
    result.insert(QStringLiteral("partstat"), partstat);
    result.insert(QStringLiteral("responseStatus"), responseStatus(partstat));
  }
  const QString role = parameterValue(property, "ROLE").toUpper();
  if (!role.isEmpty()) {
    result.insert(QStringLiteral("role"), role);
  }
  const QString cutype = parameterValue(property, "CUTYPE").toUpper();
  if (!cutype.isEmpty()) {
    result.insert(QStringLiteral("cutype"), cutype);
  }
  const QString rsvp = parameterValue(property, "RSVP").toUpper();
  if (!rsvp.isEmpty()) {
    result.insert(QStringLiteral("rsvp"), rsvp == QStringLiteral("TRUE"));
  }
  return result;
}

void setParameter(icalproperty* property, const char* name, const QString& value) {
  icalproperty_remove_parameter_by_name(property, name);
  if (!value.isEmpty()) {
    const QByteArray encoded = value.toUtf8();
    icalproperty_set_parameter_from_string(property, name, encoded.constData());
  }
}

QString attendeeAddress(const QJsonObject& attendee) {
  QString address = attendee.value(QStringLiteral("uri")).toString().trimmed();
  if (address.isEmpty()) {
    const QString email = attendee.value(QStringLiteral("email")).toString().trimmed();
    if (!email.isEmpty()) {
      address = QStringLiteral("mailto:") + email;
    }
  }
  return address;
}

QString attendeePartstat(const QJsonObject& attendee) {
  QString partstat = attendee.value(QStringLiteral("partstat")).toString().trimmed();
  if (partstat.isEmpty()) {
    partstat = attendee.value(QStringLiteral("responseStatus")).toString().trimmed();
  }
  partstat = partstat.toUpper();
  if (partstat == QStringLiteral("NEEDSACTION")) {
    return QStringLiteral("NEEDS-ACTION");
  }
  return partstat;
}

bool updateAttendeeProperty(icalproperty* property, const QJsonObject& attendee) {
  const QString address = attendeeAddress(attendee);
  if (address.isEmpty()) {
    return false;
  }
  icalproperty_set_attendee(property, address.toUtf8().constData());
  setParameter(property, "CN",
               attendee.value(QStringLiteral("displayName")).toString());
  setParameter(property, "PARTSTAT", attendeePartstat(attendee));
  setParameter(property, "ROLE",
               attendee.value(QStringLiteral("role")).toString().toUpper());
  setParameter(property, "CUTYPE",
               attendee.value(QStringLiteral("cutype")).toString().toUpper());
  setParameter(
      property, "RSVP",
      attendee.contains(QStringLiteral("rsvp"))
          ? (attendee.value(QStringLiteral("rsvp")).toBool() ? QStringLiteral("TRUE")
                                                             : QStringLiteral("FALSE"))
          : QString());
  return true;
}

bool mergeAttendees(icalcomponent* target, const QJsonArray& desired,
                    ICalendarError* error) {
  QHash<QString, icalproperty*> existing;
  QList<icalproperty*> existingProperties;
  for (icalproperty* property =
           icalcomponent_get_first_property(target, ICAL_ATTENDEE_PROPERTY);
       property != nullptr;
       property = icalcomponent_get_next_property(target, ICAL_ATTENDEE_PROPERTY)) {
    existingProperties.append(property);
    const QString identity = attendeeIdentity(calendarUser(property, false));
    if (identity.isEmpty() || existing.contains(identity)) {
      *error = serializationError(
          QStringLiteral("ambiguous_attendee"),
          QStringLiteral("The retained event contains an attendee without a unique "
                         "calendar address"));
      return false;
    }
    existing.insert(identity, property);
  }

  QSet<icalproperty*> retained;
  QSet<QString> desiredIdentities;
  for (const QJsonValue& value : desired) {
    if (!value.isObject()) {
      *error = serializationError(
          QStringLiteral("invalid_attendee"),
          QStringLiteral("Every attendee must be an object with an email or URI"));
      return false;
    }
    const QJsonObject attendee = value.toObject();
    const QString identity = attendeeIdentity(attendee);
    if (identity.isEmpty() || desiredIdentities.contains(identity)) {
      *error = serializationError(
          QStringLiteral("invalid_attendee"),
          QStringLiteral("Every attendee needs a unique email or URI"));
      return false;
    }
    desiredIdentities.insert(identity);
    icalproperty* property = existing.value(identity, nullptr);
    if (property == nullptr) {
      const QString address = attendeeAddress(attendee);
      property = icalproperty_new_attendee(address.toUtf8().constData());
      if (property == nullptr) {
        *error = serializationError(
            QStringLiteral("allocation_failed"),
            QStringLiteral("Unable to add an attendee to the event"));
        return false;
      }
      icalcomponent_add_property(target, property);
    }
    if (!updateAttendeeProperty(property, attendee)) {
      *error =
          serializationError(QStringLiteral("invalid_attendee"),
                             QStringLiteral("Every attendee needs an email or URI"));
      return false;
    }
    retained.insert(property);
  }
  for (icalproperty* property : existingProperties) {
    if (retained.contains(property)) {
      continue;
    }
    icalcomponent_remove_property(target, property);
    icalproperty_free(property);
  }
  return true;
}

QString componentText(const icalcomponent* component) {
  char* encoded = icalcomponent_as_ical_string_r(component);
  if (encoded == nullptr) {
    return {};
  }
  const QString result = QString::fromUtf8(encoded);
  icalmemory_free_buffer(encoded);
  return result;
}

void collectEvents(icalcomponent* component, QList<icalcomponent*>* events) {
  if (icalcomponent_isa(component) == ICAL_VEVENT_COMPONENT) {
    events->append(component);
    return;
  }
  for (icalcomponent* child =
           icalcomponent_get_first_component(component, ICAL_ANY_COMPONENT);
       child != nullptr;
       child = icalcomponent_get_next_component(component, ICAL_ANY_COMPONENT)) {
    collectEvents(child, events);
  }
}

void removeProperties(icalcomponent* component, const icalproperty_kind kind) {
  for (icalproperty* property = icalcomponent_get_first_property(component, kind);
       property != nullptr;) {
    icalproperty* next = icalcomponent_get_next_property(component, kind);
    icalcomponent_remove_property(component, property);
    icalproperty_free(property);
    property = next;
  }
}

bool replaceProperties(icalcomponent* target, icalcomponent* source,
                       const icalproperty_kind kind) {
  removeProperties(target, kind);
  for (icalproperty* property = icalcomponent_get_first_property(source, kind);
       property != nullptr; property = icalcomponent_get_next_property(source, kind)) {
    icalproperty* clone = icalproperty_clone(property);
    if (clone == nullptr) {
      return false;
    }
    icalcomponent_add_property(target, clone);
  }
  return true;
}

QString componentTimeZoneId(icalcomponent* component) {
  return propertyText(component, ICAL_TZID_PROPERTY);
}

bool containsTimeZone(icalcomponent* calendar, const QString& timeZoneId) {
  for (icalcomponent* component =
           icalcomponent_get_first_component(calendar, ICAL_VTIMEZONE_COMPONENT);
       component != nullptr; component = icalcomponent_get_next_component(
                                 calendar, ICAL_VTIMEZONE_COMPONENT)) {
    if (componentTimeZoneId(component) == timeZoneId) {
      return true;
    }
  }
  return false;
}

bool copyMissingTimeZones(icalcomponent* target, icalcomponent* source) {
  for (icalcomponent* component =
           icalcomponent_get_first_component(source, ICAL_VTIMEZONE_COMPONENT);
       component != nullptr;
       component = icalcomponent_get_next_component(source, ICAL_VTIMEZONE_COMPONENT)) {
    const QString timeZoneId = componentTimeZoneId(component);
    if (timeZoneId.isEmpty() || containsTimeZone(target, timeZoneId)) {
      continue;
    }
    icalcomponent* clone = icalcomponent_clone(component);
    if (clone == nullptr) {
      return false;
    }
    icalcomponent_add_component(target, clone);
  }
  return true;
}

bool addProperty(icalcomponent* component, icalproperty* property) {
  if (property == nullptr) {
    return false;
  }
  icalcomponent_add_property(component, property);
  return true;
}

icaltimetype dateValue(const QDate& date) {
  const QByteArray encoded = date.toString(QStringLiteral("yyyyMMdd")).toLatin1();
  return icaltime_from_string(encoded.constData());
}

icaltimetype utcValue(const QDateTime& dateTime) {
  return icaltime_from_timet_with_zone(
      static_cast<icaltime_t>(dateTime.toUTC().toSecsSinceEpoch()), false,
      icaltimezone_get_utc_timezone());
}

bool addReminderAlarm(icalcomponent* event, const QJsonValue& value,
                      const QString& eventSummary, ICalendarError* error) {
  QString validationError;
  const std::optional<StructuredReminder> reminder =
      structuredReminder(value, &validationError);
  if (!reminder.has_value()) {
    *error = serializationError(QStringLiteral("invalid_reminder"), validationError);
    return false;
  }

  ComponentPtr alarm(icalcomponent_new_valarm());
  if (!alarm) {
    *error = serializationError(QStringLiteral("allocation_failed"),
                                QStringLiteral("Unable to create a reminder alarm"));
    return false;
  }

  icalproperty_action action = ICAL_ACTION_NONE;
  if (reminder->method == QStringLiteral("popup")) {
    action = ICAL_ACTION_DISPLAY;
  } else if (reminder->method == QStringLiteral("email")) {
    action = ICAL_ACTION_EMAIL;
  } else if (reminder->method == QStringLiteral("audio")) {
    action = ICAL_ACTION_AUDIO;
  }

  icaltriggertype trigger = icaltriggertype_from_seconds(0);
  if (reminder->absoluteUtc.isValid()) {
    trigger.time = utcValue(reminder->absoluteUtc);
    trigger.duration = icaldurationtype_null_duration();
  } else {
    const qint64 seconds = -static_cast<qint64>(reminder->minutesBefore.value()) * 60;
    trigger = icaltriggertype_from_seconds(static_cast<int>(seconds));
  }
  if (!addProperty(alarm.get(), icalproperty_new_action(action)) ||
      !addProperty(alarm.get(), icalproperty_new_trigger(trigger))) {
    *error = serializationError(QStringLiteral("allocation_failed"),
                                QStringLiteral("Unable to serialize a reminder alarm"));
    return false;
  }

  const QString description =
      eventSummary.isEmpty() ? QStringLiteral("Calendar reminder") : eventSummary;
  if ((action == ICAL_ACTION_DISPLAY || action == ICAL_ACTION_EMAIL) &&
      !addProperty(alarm.get(),
                   icalproperty_new_description(description.toUtf8().constData()))) {
    *error = serializationError(
        QStringLiteral("allocation_failed"),
        QStringLiteral("Unable to serialize a reminder description"));
    return false;
  }
  if (action == ICAL_ACTION_EMAIL &&
      !addProperty(alarm.get(),
                   icalproperty_new_summary(description.toUtf8().constData()))) {
    *error = serializationError(
        QStringLiteral("allocation_failed"),
        QStringLiteral("Unable to serialize an email reminder summary"));
    return false;
  }

  icalcomponent_add_component(event, alarm.release());
  return true;
}

QList<icalcomponent*> alarmComponents(icalcomponent* event) {
  QList<icalcomponent*> result;
  for (icalcomponent* alarm =
           icalcomponent_get_first_component(event, ICAL_VALARM_COMPONENT);
       alarm != nullptr;
       alarm = icalcomponent_get_next_component(event, ICAL_VALARM_COMPONENT)) {
    result.append(alarm);
  }
  return result;
}

bool mergeReminderAlarms(icalcomponent* target, icalcomponent* draft,
                         ICalendarError* error) {
  struct ExistingAlarm {
    icalcomponent* component = nullptr;
    QJsonObject reminder;
    bool used = false;
  };
  QList<ExistingAlarm> editableExisting;
  for (icalcomponent* alarm : alarmComponents(target)) {
    const std::optional<QJsonObject> reminder = reminderFromAlarm(alarm);
    if (reminder.has_value()) {
      editableExisting.append({alarm, *reminder, false});
    }
  }
  const QList<icalcomponent*> desired = alarmComponents(draft);
  QList<std::optional<qsizetype>> matches(desired.size());

  // First preserve alarms whose structured values did not change. This keeps
  // alarm-specific provider metadata attached to the right alarm when another
  // reminder is removed or the UI changes their order.
  for (qsizetype desiredIndex = 0; desiredIndex < desired.size(); ++desiredIndex) {
    const std::optional<QJsonObject> desiredReminder =
        reminderFromAlarm(desired.at(desiredIndex));
    if (!desiredReminder.has_value()) {
      *error =
          serializationError(QStringLiteral("serialization_failed"),
                             QStringLiteral("A reminder draft could not be decoded"));
      return false;
    }
    for (qsizetype existingIndex = 0; existingIndex < editableExisting.size();
         ++existingIndex) {
      ExistingAlarm& existing = editableExisting[existingIndex];
      if (!existing.used && existing.reminder == *desiredReminder) {
        existing.used = true;
        matches[desiredIndex] = existingIndex;
        break;
      }
    }
  }

  // Pair edited reminders with the remaining retained alarms in stable order;
  // any excess desired reminder becomes a fresh VALARM.
  for (qsizetype desiredIndex = 0; desiredIndex < desired.size(); ++desiredIndex) {
    if (matches.at(desiredIndex).has_value()) {
      continue;
    }
    for (qsizetype existingIndex = 0; existingIndex < editableExisting.size();
         ++existingIndex) {
      ExistingAlarm& existing = editableExisting[existingIndex];
      if (!existing.used) {
        existing.used = true;
        matches[desiredIndex] = existingIndex;
        break;
      }
    }
  }

  for (qsizetype desiredIndex = 0; desiredIndex < desired.size(); ++desiredIndex) {
    if (!matches.at(desiredIndex).has_value()) {
      icalcomponent* clone = icalcomponent_clone(desired.at(desiredIndex));
      if (clone == nullptr) {
        *error = serializationError(QStringLiteral("allocation_failed"),
                                    QStringLiteral("Unable to add a reminder alarm"));
        return false;
      }
      icalcomponent_add_component(target, clone);
      continue;
    }
    icalcomponent* existing =
        editableExisting.at(matches.at(desiredIndex).value()).component;
    // Only ACTION and TRIGGER are represented by the structured reminder
    // editor. Everything else, including DESCRIPTION, ATTENDEE, REPEAT,
    // DURATION, ATTACH, and X-* provider data, stays byte-semantically intact.
    if (!replaceProperties(existing, desired.at(desiredIndex), ICAL_ACTION_PROPERTY) ||
        !replaceProperties(existing, desired.at(desiredIndex), ICAL_TRIGGER_PROPERTY)) {
      *error = serializationError(
          QStringLiteral("allocation_failed"),
          QStringLiteral("Unable to patch retained reminder alarms"));
      return false;
    }
  }

  for (const ExistingAlarm& existing : std::as_const(editableExisting)) {
    if (!existing.used) {
      icalcomponent_remove_component(target, existing.component);
      icalcomponent_free(existing.component);
    }
  }
  return true;
}

icaltimetype zonedValue(const QDateTime& dateTime, const QString& timeZone) {
  if (timeZone.isEmpty() || isUtcZone(timeZone)) {
    return utcValue(dateTime);
  }

  const QByteArray zoneName = timeZone.toUtf8();
  icaltimezone* zone = icaltimezone_get_builtin_timezone(zoneName.constData());
  if (zone == nullptr) {
    zone = icaltimezone_get_builtin_timezone_from_tzid(zoneName.constData());
  }
  if (zone != nullptr) {
    return icaltime_from_timet_with_zone(
        static_cast<icaltime_t>(dateTime.toUTC().toSecsSinceEpoch()), false, zone);
  }

  const QTimeZone qtZone(zoneName);
  const QDateTime local =
      qtZone.isValid() ? dateTime.toUTC().toTimeZone(qtZone) : dateTime;
  const QByteArray encoded =
      local.toString(QStringLiteral("yyyyMMdd'T'HHmmss")).toLatin1();
  return icaltime_from_string(encoded.constData());
}

icalproperty* dateTimeProperty(icalproperty_kind kind, const QDateTime& dateTime,
                               const QString& timeZone) {
  const icaltimetype value = zonedValue(dateTime, timeZone);
  icalproperty* property = nullptr;
  if (kind == ICAL_DTSTART_PROPERTY) {
    property = icalproperty_new_dtstart(value);
  } else if (kind == ICAL_DTEND_PROPERTY) {
    property = icalproperty_new_dtend(value);
  }
  if (property != nullptr && !timeZone.isEmpty() && !isUtcZone(timeZone)) {
    const QByteArray zoneName = timeZone.toUtf8();
    icalproperty_add_parameter(property, icalparameter_new_tzid(zoneName.constData()));
  }
  return property;
}

void addTimeZone(icalcomponent* calendar, const QString& timeZone,
                 QSet<QString>* addedZones) {
  if (timeZone.isEmpty() || isUtcZone(timeZone) || addedZones->contains(timeZone)) {
    return;
  }
  addedZones->insert(timeZone);

  const QByteArray zoneName = timeZone.toUtf8();
  icaltimezone* zone = icaltimezone_get_builtin_timezone(zoneName.constData());
  if (zone == nullptr) {
    zone = icaltimezone_get_builtin_timezone_from_tzid(zoneName.constData());
  }
  icalcomponent* source = zone == nullptr ? nullptr : icaltimezone_get_component(zone);
  if (source == nullptr) {
    return;
  }
  icalcomponent* clone = icalcomponent_clone(source);
  if (clone == nullptr) {
    return;
  }
  icalproperty* tzid = icalcomponent_get_first_property(clone, ICAL_TZID_PROPERTY);
  if (tzid != nullptr) {
    icalproperty_set_tzid(tzid, zoneName.constData());
  }
  icalcomponent_add_component(calendar, clone);
}

ICalendarError serializationError(const QString& code, const QString& message) {
  return {code, message, 0};
}

QByteArray mutationFingerprint(const QString& clientMutationId) {
  return QCryptographicHash::hash(clientMutationId.toUtf8(), QCryptographicHash::Sha256)
      .toHex();
}

}  // namespace

ICalendarParseResult ICalendarCodec::parse(const QByteArray& payload) {
  ICalendarParseResult result;
  if (payload.isEmpty()) {
    result.error = {QStringLiteral("empty_payload"),
                    QStringLiteral("The iCalendar resource was empty")};
    return result;
  }
  if (payload.size() > kMaximumCalendarBytes) {
    result.error = {QStringLiteral("payload_too_large"),
                    QStringLiteral("The iCalendar resource exceeds 16 MiB")};
    return result;
  }
  if (payload.contains('\0')) {
    result.error = {QStringLiteral("embedded_nul"),
                    QStringLiteral("The iCalendar resource contains a NUL byte")};
    return result;
  }

  icalerror_clear_errno();
  ComponentPtr calendar(icalcomponent_new_from_string(payload.constData()));
  if (!calendar) {
    result.error = {QStringLiteral("parse_error"),
                    QStringLiteral("libical could not parse the calendar resource: %1")
                        .arg(QString::fromLatin1(icalerror_strerror(icalerrno)))};
    icalerror_clear_errno();
    return result;
  }

  QList<icalcomponent*> components;
  collectEvents(calendar.get(), &components);
  if (components.isEmpty()) {
    result.error = {QStringLiteral("no_events"),
                    QStringLiteral("The calendar resource contains no VEVENT")};
    return result;
  }

  const QString rawPayload = QString::fromUtf8(payload);
  for (qsizetype index = 0; index < components.size(); ++index) {
    icalcomponent* component = components.at(index);
    Event event;
    event.uid = textProperty(component, ICAL_UID_PROPERTY);
    if (event.uid.isEmpty()) {
      result.events.clear();
      result.error = {QStringLiteral("missing_uid"),
                      QStringLiteral("VEVENT is missing its UID"),
                      static_cast<int>(index)};
      return result;
    }

    icalproperty* startProperty =
        icalcomponent_get_first_property(component, ICAL_DTSTART_PROPERTY);
    const ParsedTime start = parseTime(startProperty, calendar.get());
    if (!start.valid) {
      result.events.clear();
      result.error = {QStringLiteral("invalid_start"),
                      QStringLiteral("VEVENT has no valid DTSTART"),
                      static_cast<int>(index)};
      return result;
    }

    icalproperty* endProperty =
        icalcomponent_get_first_property(component, ICAL_DTEND_PROPERTY);
    ParsedTime end = parseTime(endProperty, calendar.get());
    if (end.valid && end.dateOnly != start.dateOnly) {
      result.events.clear();
      result.error = {QStringLiteral("mixed_date_types"),
                      QStringLiteral("DTSTART and DTEND use different value types"),
                      static_cast<int>(index)};
      return result;
    }

    qint64 duration = 0;
    if (!end.valid) {
      icalproperty* durationProperty =
          icalcomponent_get_first_property(component, ICAL_DURATION_PROPERTY);
      if (durationProperty != nullptr) {
        duration = durationSeconds(icalproperty_get_duration(durationProperty));
      }
    }

    event.allDay = start.dateOnly;
    if (event.allDay) {
      event.startDate = start.date;
      event.endDate =
          end.valid ? end.date
                    : start.date.addDays(duration > 0 ? duration / (24 * 60 * 60) : 1);
      if (event.endDate < event.startDate) {
        result.events.clear();
        result.error = {QStringLiteral("invalid_range"),
                        QStringLiteral("VEVENT ends before it starts"),
                        static_cast<int>(index)};
        return result;
      }
    } else {
      event.startUtc = start.utc;
      event.endUtc = end.valid ? end.utc : start.utc.addSecs(duration);
      event.startTimeZone = start.timeZone;
      event.endTimeZone = end.valid ? end.timeZone : start.timeZone;
      if (event.endUtc < event.startUtc) {
        result.events.clear();
        result.error = {QStringLiteral("invalid_range"),
                        QStringLiteral("VEVENT ends before it starts"),
                        static_cast<int>(index)};
        return result;
      }
    }

    event.summary = textProperty(component, ICAL_SUMMARY_PROPERTY);
    event.description = textProperty(component, ICAL_DESCRIPTION_PROPERTY);
    event.location = textProperty(component, ICAL_LOCATION_PROPERTY);
    event.url = textProperty(component, ICAL_URL_PROPERTY);
    if (icalproperty* organizer =
            icalcomponent_get_first_property(component, ICAL_ORGANIZER_PROPERTY)) {
      event.organizer = calendarUser(organizer, true);
    }
    for (icalproperty* attendee =
             icalcomponent_get_first_property(component, ICAL_ATTENDEE_PROPERTY);
         attendee != nullptr; attendee = icalcomponent_get_next_property(
                                  component, ICAL_ATTENDEE_PROPERTY)) {
      event.attendees.append(calendarUser(attendee, false));
    }

    if (icalproperty* property =
            icalcomponent_get_first_property(component, ICAL_CLASS_PROPERTY)) {
      const char* visibility =
          icalproperty_class_to_string(icalproperty_get_class(property));
      if (visibility != nullptr && *visibility != '\0') {
        event.visibility = QString::fromLatin1(visibility).toLower();
      }
    }

    if (icalproperty* property =
            icalcomponent_get_first_property(component, ICAL_STATUS_PROPERTY)) {
      const char* status =
          icalproperty_status_to_string(icalproperty_get_status(property));
      if (status != nullptr && *status != '\0') {
        event.status = QString::fromLatin1(status).toLower();
      }
    }
    if (icalproperty* property =
            icalcomponent_get_first_property(component, ICAL_TRANSP_PROPERTY)) {
      const char* transparency =
          icalproperty_transp_to_string(icalproperty_get_transp(property));
      if (transparency != nullptr && *transparency != '\0') {
        event.transparency = QString::fromLatin1(transparency).toLower();
      }
    }
    if (icalproperty* property =
            icalcomponent_get_first_property(component, ICAL_SEQUENCE_PROPERTY)) {
      event.sequence = icalproperty_get_sequence(property);
    }

    event.recurrenceRule = propertyText(component, ICAL_RRULE_PROPERTY);
    event.recurrenceId = recurrenceId(component);
    for (icalcomponent* alarm : alarmComponents(component)) {
      const std::optional<QJsonObject> reminder = reminderFromAlarm(alarm);
      if (reminder.has_value()) {
        event.reminders.append(*reminder);
      }
    }
    event.rawPayload = rawPayload;
    event.rawFormat = QStringLiteral("text/calendar");
    result.events.append(event);
  }

  icalerror_clear_errno();
  return result;
}

ICalendarSerializeResult ICalendarCodec::serialize(const Event& event,
                                                   const QString& productId) {
  ICalendarSerializeResult result;
  const QString uid = event.uid.isEmpty() ? event.id : event.uid;
  if (uid.isEmpty()) {
    result.error = serializationError(
        QStringLiteral("missing_uid"),
        QStringLiteral("An event needs a UID before it can be serialized"));
    return result;
  }
  if (event.allDay && !event.startDate.isValid()) {
    result.error =
        serializationError(QStringLiteral("invalid_start"),
                           QStringLiteral("An all-day event needs a valid start date"));
    return result;
  }
  if (!event.allDay && !event.startUtc.isValid()) {
    result.error =
        serializationError(QStringLiteral("invalid_start"),
                           QStringLiteral("A timed event needs a valid start time"));
    return result;
  }
  if (event.allDay && event.endDate.isValid() && event.endDate <= event.startDate) {
    result.error =
        serializationError(QStringLiteral("invalid_range"),
                           QStringLiteral("The event ends before it starts"));
    return result;
  }
  if (!event.allDay && event.endUtc.isValid() && event.endUtc < event.startUtc) {
    result.error =
        serializationError(QStringLiteral("invalid_range"),
                           QStringLiteral("The event ends before it starts"));
    return result;
  }

  ComponentPtr calendar(icalcomponent_new_vcalendar());
  if (!calendar) {
    result.error =
        serializationError(QStringLiteral("allocation_failed"),
                           QStringLiteral("Unable to create an iCalendar document"));
    return result;
  }
  if (!addProperty(calendar.get(), icalproperty_new_version("2.0")) ||
      !addProperty(calendar.get(),
                   icalproperty_new_prodid(productId.toUtf8().constData())) ||
      !addProperty(calendar.get(), icalproperty_new_calscale("GREGORIAN"))) {
    result.error =
        serializationError(QStringLiteral("allocation_failed"),
                           QStringLiteral("Unable to create calendar metadata"));
    return result;
  }

  QSet<QString> addedZones;
  if (!event.allDay) {
    addTimeZone(calendar.get(), event.startTimeZone, &addedZones);
    addTimeZone(calendar.get(), event.endTimeZone, &addedZones);
  }

  icalcomponent* component = icalcomponent_new_vevent();
  if (component == nullptr) {
    result.error = serializationError(QStringLiteral("allocation_failed"),
                                      QStringLiteral("Unable to create a VEVENT"));
    return result;
  }
  // Ownership transfers to the VCALENDAR immediately, so every later early
  // return remains leak-free through the calendar's RAII owner.
  icalcomponent_add_component(calendar.get(), component);

  const QDateTime stamp =
      event.updatedAt.isValid()
          ? event.updatedAt
          : (event.createdAt.isValid()
                 ? event.createdAt
                 : (event.allDay
                        ? QDateTime(event.startDate, QTime(0, 0), QTimeZone::UTC)
                        : event.startUtc));
  if (!addProperty(component, icalproperty_new_uid(uid.toUtf8().constData())) ||
      !addProperty(component, icalproperty_new_dtstamp(utcValue(stamp)))) {
    result.error = serializationError(
        QStringLiteral("allocation_failed"),
        QStringLiteral("Unable to serialize required event metadata"));
    return result;
  }

  if (event.allDay) {
    const QDate endDate =
        event.endDate.isValid() ? event.endDate : event.startDate.addDays(1);
    if (!addProperty(component, icalproperty_new_dtstart(dateValue(event.startDate))) ||
        !addProperty(component, icalproperty_new_dtend(dateValue(endDate)))) {
      result.error =
          serializationError(QStringLiteral("allocation_failed"),
                             QStringLiteral("Unable to serialize all-day event dates"));
      return result;
    }
  } else {
    const QString endZone =
        event.endTimeZone.isEmpty() ? event.startTimeZone : event.endTimeZone;
    if (!addProperty(component, dateTimeProperty(ICAL_DTSTART_PROPERTY, event.startUtc,
                                                 event.startTimeZone))) {
      result.error =
          serializationError(QStringLiteral("allocation_failed"),
                             QStringLiteral("Unable to serialize event times"));
      return result;
    }
    // RFC 5545 represents a zero-duration timed event by omitting DTEND. A
    // present DTEND must be later than DTSTART.
    if (event.endUtc.isValid() && event.endUtc > event.startUtc &&
        !addProperty(component,
                     dateTimeProperty(ICAL_DTEND_PROPERTY, event.endUtc, endZone))) {
      result.error =
          serializationError(QStringLiteral("allocation_failed"),
                             QStringLiteral("Unable to serialize event times"));
      return result;
    }
  }

  if (!event.recurrenceId.isEmpty()) {
    const QString recurrence = normalizedRecurrenceId(event);
    QString line;
    if (recurrence.startsWith(QStringLiteral("RECURRENCE-ID"), Qt::CaseInsensitive)) {
      line = recurrence;
    } else if (recurrence.startsWith(QStringLiteral("TZID="), Qt::CaseInsensitive) ||
               recurrence.startsWith(QStringLiteral("RANGE="), Qt::CaseInsensitive)) {
      line = QStringLiteral("RECURRENCE-ID;") + recurrence;
    } else {
      line = QStringLiteral("RECURRENCE-ID:") + recurrence;
    }
    icalproperty* property = icalproperty_new_from_string(line.toUtf8().constData());
    if (!addProperty(component, property)) {
      result.error = serializationError(
          QStringLiteral("invalid_recurrence_id"),
          QStringLiteral("RECURRENCE-ID is not valid RFC 5545 syntax"));
      return result;
    }
  }

  if (!event.recurrenceRule.isEmpty()) {
    QString rule = event.recurrenceRule.trimmed();
    if (rule.startsWith(QStringLiteral("RRULE:"), Qt::CaseInsensitive)) {
      rule.remove(0, 6);
    }
    icalproperty* property = icalproperty_new_from_string(
        (QStringLiteral("RRULE:") + rule).toUtf8().constData());
    if (!addProperty(component, property)) {
      result.error =
          serializationError(QStringLiteral("invalid_recurrence_rule"),
                             QStringLiteral("RRULE is not valid RFC 5545 syntax"));
      return result;
    }
  }

  if (!addProperty(component,
                   icalproperty_new_summary(event.summary.toUtf8().constData()))) {
    result.error =
        serializationError(QStringLiteral("allocation_failed"),
                           QStringLiteral("Unable to serialize the event summary"));
    return result;
  }
  if (!event.description.isEmpty() &&
      !addProperty(component, icalproperty_new_description(
                                  event.description.toUtf8().constData()))) {
    result.error =
        serializationError(QStringLiteral("allocation_failed"),
                           QStringLiteral("Unable to serialize the event description"));
    return result;
  }
  if (!event.location.isEmpty() &&
      !addProperty(component,
                   icalproperty_new_location(event.location.toUtf8().constData()))) {
    result.error =
        serializationError(QStringLiteral("allocation_failed"),
                           QStringLiteral("Unable to serialize the event location"));
    return result;
  }
  if (!event.url.isEmpty() &&
      !addProperty(component, icalproperty_new_url(event.url.toUtf8().constData()))) {
    result.error = serializationError(QStringLiteral("allocation_failed"),
                                      QStringLiteral("Unable to serialize event URL"));
    return result;
  }

  if (!event.organizer.isEmpty()) {
    const QString address = attendeeAddress(event.organizer);
    if (address.isEmpty()) {
      result.error = serializationError(
          QStringLiteral("invalid_organizer"),
          QStringLiteral("An organizer needs an email or calendar address"));
      return result;
    }
    icalproperty* organizer = icalproperty_new_organizer(address.toUtf8().constData());
    if (!addProperty(component, organizer)) {
      result.error =
          serializationError(QStringLiteral("allocation_failed"),
                             QStringLiteral("Unable to serialize the event organizer"));
      return result;
    }
    setParameter(organizer, "CN",
                 event.organizer.value(QStringLiteral("displayName")).toString());
  }
  ICalendarError attendeeError;
  if (!mergeAttendees(component, event.attendees, &attendeeError)) {
    result.error = attendeeError;
    return result;
  }

  for (const QJsonValue& reminder : event.reminders) {
    if (!addReminderAlarm(component, reminder, event.summary, &result.error)) {
      return result;
    }
  }

  const QByteArray visibility = event.visibility.trimmed().toUpper().toLatin1();
  const icalproperty_class visibilityValue =
      icalproperty_string_to_class(visibility.constData());
  if (!visibility.isEmpty() && visibility != QByteArrayLiteral("DEFAULT") &&
      visibilityValue == ICAL_CLASS_NONE) {
    result.error = serializationError(
        QStringLiteral("invalid_visibility"),
        QStringLiteral("The event visibility is not valid RFC 5545 syntax"));
    return result;
  }
  if (visibilityValue != ICAL_CLASS_NONE &&
      !addProperty(component, icalproperty_new_class(visibilityValue))) {
    result.error =
        serializationError(QStringLiteral("allocation_failed"),
                           QStringLiteral("Unable to serialize event visibility"));
    return result;
  }

  const QByteArray status = event.status.trimmed().toUpper().toLatin1();
  const icalproperty_status statusValue =
      icalproperty_string_to_status(status.constData());
  if (!status.isEmpty() && statusValue == ICAL_STATUS_NONE) {
    result.error = serializationError(
        QStringLiteral("invalid_status"),
        QStringLiteral("The event status is not an RFC 5545 status"));
    return result;
  }
  if (statusValue != ICAL_STATUS_NONE &&
      !addProperty(component, icalproperty_new_status(statusValue))) {
    result.error =
        serializationError(QStringLiteral("allocation_failed"),
                           QStringLiteral("Unable to serialize the event status"));
    return result;
  }

  const QByteArray transparency = event.transparency.trimmed().toUpper().toLatin1();
  const icalproperty_transp transparencyValue =
      icalproperty_string_to_transp(transparency.constData());
  if (!transparency.isEmpty() && transparencyValue == ICAL_TRANSP_NONE) {
    result.error = serializationError(
        QStringLiteral("invalid_transparency"),
        QStringLiteral("The event transparency is not valid RFC 5545 syntax"));
    return result;
  }
  if (transparencyValue != ICAL_TRANSP_NONE &&
      !addProperty(component, icalproperty_new_transp(transparencyValue))) {
    result.error =
        serializationError(QStringLiteral("allocation_failed"),
                           QStringLiteral("Unable to serialize event transparency"));
    return result;
  }

  if (!addProperty(component, icalproperty_new_sequence(event.sequence))) {
    result.error =
        serializationError(QStringLiteral("allocation_failed"),
                           QStringLiteral("Unable to serialize the event sequence"));
    return result;
  }

  icalcomponent_normalize(calendar.get());
  const QString encoded = componentText(calendar.get());
  if (encoded.isEmpty()) {
    result.error = serializationError(
        QStringLiteral("serialization_failed"),
        QStringLiteral("libical could not encode the calendar resource"));
    return result;
  }
  result.payload = encoded.toUtf8();
  return result;
}

ICalendarSerializeResult ICalendarCodec::patch(const Event& event,
                                               const QByteArray& retainedPayload,
                                               const QString& productId) {
  ICalendarSerializeResult result;
  if (retainedPayload.isEmpty()) {
    result.error = serializationError(
        QStringLiteral("missing_retained_payload"),
        QStringLiteral("A retained CalDAV resource is required for a safe update"));
    return result;
  }
  if (retainedPayload.size() > kMaximumCalendarBytes ||
      retainedPayload.contains('\0')) {
    result.error = serializationError(
        QStringLiteral("invalid_retained_payload"),
        QStringLiteral("The retained CalDAV resource is unsafe to parse"));
    return result;
  }

  const ICalendarSerializeResult draftResult = serialize(event, productId);
  if (!draftResult.ok()) {
    return draftResult;
  }

  icalerror_clear_errno();
  ComponentPtr retained(icalcomponent_new_from_string(retainedPayload.constData()));
  ComponentPtr draft(icalcomponent_new_from_string(draftResult.payload.constData()));
  if (!retained || !draft ||
      icalcomponent_isa(retained.get()) != ICAL_VCALENDAR_COMPONENT) {
    result.error = serializationError(
        QStringLiteral("invalid_retained_payload"),
        QStringLiteral("The retained resource is not a valid VCALENDAR"));
    icalerror_clear_errno();
    return result;
  }

  QList<icalcomponent*> draftEvents;
  collectEvents(draft.get(), &draftEvents);
  if (draftEvents.size() != 1) {
    result.error = serializationError(
        QStringLiteral("serialization_failed"),
        QStringLiteral("The update draft did not contain exactly one VEVENT"));
    return result;
  }
  icalcomponent* draftEvent = draftEvents.first();
  const QString uid = textProperty(draftEvent, ICAL_UID_PROPERTY);
  const QString targetRecurrenceId = recurrenceId(draftEvent);

  QList<icalcomponent*> retainedEvents;
  collectEvents(retained.get(), &retainedEvents);
  icalcomponent* targetEvent = nullptr;
  for (icalcomponent* candidate : retainedEvents) {
    if (textProperty(candidate, ICAL_UID_PROPERTY) == uid &&
        recurrenceIdentityEqual(recurrenceId(candidate), targetRecurrenceId,
                                event.allDay, event.timeKind, event.startTimeZone)) {
      if (targetEvent != nullptr) {
        result.error = serializationError(
            QStringLiteral("ambiguous_event"),
            QStringLiteral(
                "The retained resource contains duplicate event identities"));
        return result;
      }
      targetEvent = candidate;
    }
  }
  if (targetEvent == nullptr) {
    result.error = serializationError(
        QStringLiteral("event_not_found"),
        QStringLiteral("The event to update is absent from the retained resource"));
    return result;
  }

  constexpr icalproperty_kind editableProperties[] = {
      ICAL_UID_PROPERTY,      ICAL_DTSTAMP_PROPERTY,  ICAL_DTSTART_PROPERTY,
      ICAL_DTEND_PROPERTY,    ICAL_DURATION_PROPERTY, ICAL_RECURRENCEID_PROPERTY,
      ICAL_RRULE_PROPERTY,    ICAL_SUMMARY_PROPERTY,  ICAL_DESCRIPTION_PROPERTY,
      ICAL_LOCATION_PROPERTY, ICAL_URL_PROPERTY,      ICAL_CLASS_PROPERTY,
      ICAL_STATUS_PROPERTY,   ICAL_TRANSP_PROPERTY,   ICAL_SEQUENCE_PROPERTY,
  };
  for (const icalproperty_kind kind : editableProperties) {
    if (!replaceProperties(targetEvent, draftEvent, kind)) {
      result.error = serializationError(
          QStringLiteral("allocation_failed"),
          QStringLiteral("Unable to patch the retained CalDAV resource"));
      return result;
    }
  }
  if (!mergeAttendees(targetEvent, event.attendees, &result.error)) {
    return result;
  }
  if (!mergeReminderAlarms(targetEvent, draftEvent, &result.error)) {
    return result;
  }
  if (!copyMissingTimeZones(retained.get(), draft.get())) {
    result.error = serializationError(
        QStringLiteral("allocation_failed"),
        QStringLiteral("Unable to retain event time-zone definitions"));
    return result;
  }

  const QString encoded = componentText(retained.get());
  if (encoded.isEmpty() || encoded.toUtf8().size() > kMaximumCalendarBytes) {
    result.error = serializationError(
        QStringLiteral("serialization_failed"),
        QStringLiteral("The patched CalDAV resource could not be encoded safely"));
    return result;
  }
  result.payload = encoded.toUtf8();
  icalerror_clear_errno();
  return result;
}

ICalendarSerializeResult ICalendarCodec::patchScoped(const Event& event,
                                                     const QByteArray& retainedPayload,
                                                     const QString& recurrenceScope,
                                                     const bool cancelled,
                                                     const QString& productId) {
  const QString scope = recurrenceScope.trimmed().toLower();
  if (scope != QStringLiteral("series") && scope != QStringLiteral("occurrence") &&
      scope != QStringLiteral("future")) {
    return {
        {},
        serializationError(QStringLiteral("invalid_recurrence_scope"),
                           QStringLiteral("The recurrence scope is not supported"))};
  }
  if (scope == QStringLiteral("series") && !event.recurrenceId.isEmpty()) {
    return {{},
            serializationError(
                QStringLiteral("series_master_required"),
                QStringLiteral("A series mutation must use the master event"))};
  }
  if (scope != QStringLiteral("series") && event.recurrenceId.isEmpty()) {
    return {{},
            serializationError(
                QStringLiteral("occurrence_identity_required"),
                QStringLiteral("An occurrence mutation needs a recurrence ID"))};
  }

  Event draft = event;
  draft.recurrenceId = normalizedRecurrenceId(draft);
  if (scope != QStringLiteral("series")) {
    // Detached exceptions inherit recurrence from the master and must not
    // duplicate its RRULE.
    draft.recurrenceRule.clear();
  }
  if (cancelled) {
    draft.status = QStringLiteral("cancelled");
  }

  ICalendarSerializeResult result = patch(draft, retainedPayload, productId);
  if (!result.ok() && result.error.code == QStringLiteral("event_not_found") &&
      scope != QStringLiteral("series")) {
    const ICalendarSerializeResult encoded = serialize(draft, productId);
    if (!encoded.ok()) {
      return encoded;
    }
    ComponentPtr retained(icalcomponent_new_from_string(retainedPayload.constData()));
    ComponentPtr addition(icalcomponent_new_from_string(encoded.payload.constData()));
    if (!retained || !addition ||
        icalcomponent_isa(retained.get()) != ICAL_VCALENDAR_COMPONENT) {
      return {{},
              serializationError(QStringLiteral("invalid_retained_payload"),
                                 QStringLiteral("The retained resource is not a valid "
                                                "VCALENDAR"))};
    }
    QList<icalcomponent*> additions;
    collectEvents(addition.get(), &additions);
    if (additions.size() != 1) {
      return {{},
              serializationError(
                  QStringLiteral("serialization_failed"),
                  QStringLiteral("The detached exception could not be encoded"))};
    }
    icalcomponent* clone = icalcomponent_clone(additions.first());
    if (clone == nullptr || !copyMissingTimeZones(retained.get(), addition.get())) {
      if (clone != nullptr) {
        icalcomponent_free(clone);
      }
      return {
          {},
          serializationError(QStringLiteral("allocation_failed"),
                             QStringLiteral("Unable to add the detached exception"))};
    }
    icalcomponent_add_component(retained.get(), clone);
    const QString encodedResource = componentText(retained.get());
    if (encodedResource.isEmpty() ||
        encodedResource.toUtf8().size() > kMaximumCalendarBytes) {
      return {
          {},
          serializationError(QStringLiteral("serialization_failed"),
                             QStringLiteral("The patched CalDAV resource could not be "
                                            "encoded safely"))};
    }
    result.payload = encodedResource.toUtf8();
    result.error = {};
  }
  if (!result.ok() || scope != QStringLiteral("future")) {
    return result;
  }

  ComponentPtr calendar(icalcomponent_new_from_string(result.payload.constData()));
  if (!calendar) {
    return {
        {},
        serializationError(QStringLiteral("serialization_failed"),
                           QStringLiteral("The this-and-future exception could not be "
                                          "reopened"))};
  }
  QList<icalcomponent*> events;
  collectEvents(calendar.get(), &events);
  icalcomponent* target = nullptr;
  for (icalcomponent* candidate : events) {
    if (textProperty(candidate, ICAL_UID_PROPERTY) == draft.uid &&
        recurrenceIdentityEqual(recurrenceId(candidate), draft.recurrenceId,
                                draft.allDay, draft.timeKind, draft.startTimeZone)) {
      if (target != nullptr) {
        return {{},
                serializationError(
                    QStringLiteral("ambiguous_event"),
                    QStringLiteral("The retained resource contains duplicate "
                                   "event identities"))};
      }
      target = candidate;
    }
  }
  icalproperty* recurrence =
      target == nullptr
          ? nullptr
          : icalcomponent_get_first_property(target, ICAL_RECURRENCEID_PROPERTY);
  if (recurrence == nullptr) {
    return {{},
            serializationError(QStringLiteral("occurrence_identity_required"),
                               QStringLiteral("The this-and-future exception has no "
                                              "RECURRENCE-ID"))};
  }
  icalproperty_set_parameter(recurrence,
                             icalparameter_new_range(ICAL_RANGE_THISANDFUTURE));
  const QString finalPayload = componentText(calendar.get());
  if (finalPayload.isEmpty() || finalPayload.toUtf8().size() > kMaximumCalendarBytes) {
    return {
        {},
        serializationError(QStringLiteral("serialization_failed"),
                           QStringLiteral("The this-and-future exception could not be "
                                          "encoded safely"))};
  }
  result.payload = finalPayload.toUtf8();
  return result;
}

ICalendarSerializeResult ICalendarCodec::stampClientMutationId(
    const QByteArray& payload, const QString& clientMutationId) {
  ICalendarSerializeResult result;
  if (clientMutationId.isEmpty()) {
    result.error = serializationError(
        QStringLiteral("missing_client_mutation_id"),
        QStringLiteral("A client mutation ID is required for safe CalDAV creation"));
    return result;
  }
  if (payload.isEmpty() || payload.size() > kMaximumCalendarBytes ||
      payload.contains('\0')) {
    result.error = serializationError(
        QStringLiteral("invalid_payload"),
        QStringLiteral("The CalDAV create payload is unsafe to stamp"));
    return result;
  }
  ComponentPtr calendar(icalcomponent_new_from_string(payload.constData()));
  if (!calendar || icalcomponent_isa(calendar.get()) != ICAL_VCALENDAR_COMPONENT) {
    result.error = serializationError(
        QStringLiteral("invalid_payload"),
        QStringLiteral("The CalDAV create payload is not a valid VCALENDAR"));
    return result;
  }
  QList<icalcomponent*> events;
  collectEvents(calendar.get(), &events);
  if (events.size() != 1) {
    result.error = serializationError(
        QStringLiteral("invalid_payload"),
        QStringLiteral("A CalDAV create payload must contain exactly one VEVENT"));
    return result;
  }

  constexpr auto kPropertyName = "X-OMACALENDAR-CLIENT-MUTATION-ID";
  icalcomponent* event = events.first();
  for (icalproperty* property =
           icalcomponent_get_first_property(event, ICAL_X_PROPERTY);
       property != nullptr;) {
    icalproperty* next = icalcomponent_get_next_property(event, ICAL_X_PROPERTY);
    const char* name = icalproperty_get_x_name(property);
    if (name != nullptr &&
        QString::fromLatin1(name).compare(QString::fromLatin1(kPropertyName),
                                          Qt::CaseInsensitive) == 0) {
      icalcomponent_remove_property(event, property);
      icalproperty_free(property);
    }
    property = next;
  }
  icalproperty* marker =
      icalproperty_new_x(mutationFingerprint(clientMutationId).constData());
  if (marker == nullptr) {
    result.error =
        serializationError(QStringLiteral("allocation_failed"),
                           QStringLiteral("Unable to stamp the CalDAV create payload"));
    return result;
  }
  icalproperty_set_x_name(marker, kPropertyName);
  icalcomponent_add_property(event, marker);

  const QString encoded = componentText(calendar.get());
  if (encoded.isEmpty() || encoded.toUtf8().size() > kMaximumCalendarBytes) {
    result.error = serializationError(
        QStringLiteral("serialization_failed"),
        QStringLiteral("The stamped CalDAV resource could not be encoded safely"));
    return result;
  }
  result.payload = encoded.toUtf8();
  return result;
}

bool ICalendarCodec::hasClientMutationId(const QByteArray& payload,
                                         const QString& clientMutationId) {
  if (payload.isEmpty() || clientMutationId.isEmpty() ||
      payload.size() > kMaximumCalendarBytes || payload.contains('\0')) {
    return false;
  }
  ComponentPtr calendar(icalcomponent_new_from_string(payload.constData()));
  if (!calendar) {
    return false;
  }
  constexpr auto kPropertyName = "X-OMACALENDAR-CLIENT-MUTATION-ID";
  const QByteArray expected = mutationFingerprint(clientMutationId);
  QList<icalcomponent*> events;
  collectEvents(calendar.get(), &events);
  for (icalcomponent* event : events) {
    for (icalproperty* property =
             icalcomponent_get_first_property(event, ICAL_X_PROPERTY);
         property != nullptr;
         property = icalcomponent_get_next_property(event, ICAL_X_PROPERTY)) {
      const char* name = icalproperty_get_x_name(property);
      const char* value = icalproperty_get_x(property);
      if (name != nullptr && value != nullptr &&
          QString::fromLatin1(name).compare(QString::fromLatin1(kPropertyName),
                                            Qt::CaseInsensitive) == 0 &&
          QByteArray(value) == expected) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace omacalendar::caldav
