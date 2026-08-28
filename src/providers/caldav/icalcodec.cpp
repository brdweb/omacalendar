#include "providers/caldav/icalcodec.h"

#include <libical/ical.h>

#include <QDateTime>
#include <QSet>
#include <QTimeZone>
#include <memory>

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
  return zone.isEmpty() ? valueText : QStringLiteral("TZID=%1:%2").arg(zone, valueText);
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

}  // namespace

ICalendarParseResult ICalendarCodec::parse(const QByteArray& payload) {
  ICalendarParseResult result;
  if (payload.isEmpty()) {
    result.error = {QStringLiteral("empty_payload"),
                    QStringLiteral("The iCalendar resource was empty")};
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
    QString recurrence = event.recurrenceId.trimmed();
    QString line;
    if (recurrence.startsWith(QStringLiteral("RECURRENCE-ID"), Qt::CaseInsensitive)) {
      line = recurrence;
    } else if (recurrence.startsWith(QStringLiteral("TZID="), Qt::CaseInsensitive)) {
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

}  // namespace omacalendar::caldav
