#include "core/recurrenceexpander.h"

#include <libical/ical.h>

#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <QTime>
#include <QTimeZone>
#include <algorithm>
#include <memory>

namespace omacalendar {
namespace {

struct ComponentDeleter {
  void operator()(icalcomponent* component) const {
    if (component != nullptr) {
      icalcomponent_free(component);
    }
  }
};

using ComponentPtr = std::unique_ptr<icalcomponent, ComponentDeleter>;

struct RecurrenceComponent {
  ComponentPtr owner;
  icalcomponent* event = nullptr;
  bool hasRecurrence = false;
};

struct Collector {
  const Event* master = nullptr;
  const QDateTime* startUtc = nullptr;
  const QDateTime* endUtc = nullptr;
  qsizetype limit = 0;
  QList<Event> occurrences;
  QSet<QString> seenStarts;
  bool truncated = false;
};

bool isCancelled(const Event& event) {
  return event.deleted ||
         event.status.compare(QStringLiteral("cancelled"), Qt::CaseInsensitive) == 0;
}

QDate exclusiveEndDate(const QDateTime& endUtc) {
  if (endUtc.time() == QTime(0, 0)) {
    return endUtc.date();
  }
  return endUtc.date().addDays(1);
}

bool overlaps(const Event& event, const QDateTime& startUtc, const QDateTime& endUtc) {
  if (event.allDay) {
    if (!event.startDate.isValid()) {
      return false;
    }
    const QDate eventEnd =
        event.endDate.isValid() ? event.endDate : event.startDate.addDays(1);
    return eventEnd > startUtc.date() && event.startDate < exclusiveEndDate(endUtc);
  }

  if (!event.startUtc.isValid()) {
    return false;
  }
  const QDateTime eventEnd = event.endUtc.isValid() ? event.endUtc : event.startUtc;
  if (eventEnd == event.startUtc) {
    return event.startUtc >= startUtc && event.startUtc < endUtc;
  }
  return eventEnd > startUtc && event.startUtc < endUtc;
}

QString occurrenceKey(const Event& event) {
  if (event.allDay) {
    return event.startDate.isValid()
               ? QStringLiteral("D:") + event.startDate.toString(Qt::ISODate)
               : QString();
  }
  return event.startUtc.isValid()
             ? QStringLiteral("T:") +
                   QString::number(event.startUtc.toUTC().toMSecsSinceEpoch())
             : QString();
}

QDate basicDate(const QString& value) {
  QDate result = QDate::fromString(value, Qt::ISODate);
  if (!result.isValid() && value.size() == 8) {
    result = QDate::fromString(value, QStringLiteral("yyyyMMdd"));
  }
  return result;
}

QDateTime basicDateTime(const QString& value, const QString& timeZone) {
  QDateTime result = QDateTime::fromString(value, Qt::ISODateWithMs);
  if (!result.isValid()) {
    result = QDateTime::fromString(value, Qt::ISODate);
  }
  if (result.isValid()) {
    if (result.timeSpec() == Qt::LocalTime && !timeZone.isEmpty()) {
      const QTimeZone zone(timeZone.toUtf8());
      if (zone.isValid()) {
        result = QDateTime(result.date(), result.time(), zone);
      }
    }
    return result.toUTC();
  }

  const bool utc = value.endsWith(QLatin1Char('Z'), Qt::CaseInsensitive);
  const QString format = utc ? QStringLiteral("yyyyMMdd'T'HHmmss'Z'")
                             : QStringLiteral("yyyyMMdd'T'HHmmss");
  result = QDateTime::fromString(value, format);
  if (!result.isValid()) {
    return {};
  }
  if (utc) {
    result = QDateTime(result.date(), result.time(), QTimeZone::UTC);
  } else {
    const QTimeZone zone(timeZone.toUtf8());
    result = QDateTime(result.date(), result.time(),
                       zone.isValid() ? zone : QTimeZone::systemTimeZone());
  }
  return result.toUTC();
}

QString recurrenceKey(const Event& exception, const Event& master) {
  QString value = exception.recurrenceId.trimmed();
  QString timeZone = master.startTimeZone;
  if (value.startsWith(QStringLiteral("RECURRENCE-ID"), Qt::CaseInsensitive)) {
    const qsizetype separator = value.indexOf(QLatin1Char(':'));
    if (separator < 0) {
      return {};
    }
    const QString parameters = value.left(separator);
    const qsizetype zoneStart =
        parameters.indexOf(QStringLiteral("TZID="), 0, Qt::CaseInsensitive);
    if (zoneStart >= 0) {
      timeZone = parameters.sliced(zoneStart + 5).section(QLatin1Char(';'), 0, 0);
    }
    value = value.sliced(separator + 1);
  } else if (value.startsWith(QStringLiteral("TZID="), Qt::CaseInsensitive)) {
    const qsizetype separator = value.indexOf(QLatin1Char(':'));
    if (separator < 0) {
      return {};
    }
    timeZone = value.sliced(5, separator - 5);
    value = value.sliced(separator + 1);
  }

  const QDate date = basicDate(value);
  if (date.isValid() && (master.allDay || value.size() <= 10)) {
    return QStringLiteral("D:") + date.toString(Qt::ISODate);
  }
  const QDateTime dateTime = basicDateTime(value, timeZone);
  return dateTime.isValid()
             ? QStringLiteral("T:") + QString::number(dateTime.toMSecsSinceEpoch())
             : QString();
}

QString googleRecurringParentId(const Event& event) {
  if (event.rawFormat != QStringLiteral("google-json") || event.rawPayload.isEmpty()) {
    return {};
  }
  const QJsonDocument document = QJsonDocument::fromJson(event.rawPayload.toUtf8());
  return document.isObject()
             ? document.object().value(QStringLiteral("recurringEventId")).toString()
             : QString();
}

bool belongsToMaster(const Event& exception, const Event& master) {
  if (exception.calendarId != master.calendarId) {
    return false;
  }
  const QString parentId = googleRecurringParentId(exception);
  if (!parentId.isEmpty()) {
    return parentId == master.remoteId;
  }
  return !exception.uid.isEmpty() && exception.uid == master.uid;
}

icalcomponent* findMasterComponent(icalcomponent* component, const QByteArray& uid) {
  if (component == nullptr) {
    return nullptr;
  }
  if (icalcomponent_isa(component) == ICAL_VEVENT_COMPONENT) {
    icalproperty* uidProperty =
        icalcomponent_get_first_property(component, ICAL_UID_PROPERTY);
    icalproperty* recurrenceId =
        icalcomponent_get_first_property(component, ICAL_RECURRENCEID_PROPERTY);
    const char* componentUid =
        uidProperty == nullptr ? nullptr : icalproperty_get_uid(uidProperty);
    if (recurrenceId == nullptr && componentUid != nullptr && uid == componentUid) {
      return component;
    }
    return nullptr;
  }

  for (icalcomponent* child =
           icalcomponent_get_first_component(component, ICAL_ANY_COMPONENT);
       child != nullptr;
       child = icalcomponent_get_next_component(component, ICAL_ANY_COMPONENT)) {
    if (icalcomponent* match = findMasterComponent(child, uid)) {
      return match;
    }
  }
  return nullptr;
}

bool hasRecurrenceProperties(icalcomponent* event) {
  return event != nullptr &&
         (icalcomponent_get_first_property(event, ICAL_RRULE_PROPERTY) != nullptr ||
          icalcomponent_get_first_property(event, ICAL_RDATE_PROPERTY) != nullptr);
}

icaltimezone* zoneFor(const QString& timeZone) {
  if (timeZone.isEmpty()) {
    return nullptr;
  }
  const QByteArray name = timeZone.toUtf8();
  icaltimezone* zone = icaltimezone_get_builtin_timezone(name.constData());
  if (zone == nullptr) {
    zone = icaltimezone_get_builtin_timezone_from_tzid(name.constData());
  }
  return zone;
}

icaltimetype dateValue(const QDate& date) {
  const QByteArray value = date.toString(QStringLiteral("yyyyMMdd")).toLatin1();
  return icaltime_from_string(value.constData());
}

icaltimetype dateTimeValue(const QDateTime& utc, const QString& timeZone) {
  if (icaltimezone* zone = zoneFor(timeZone)) {
    return icaltime_from_timet_with_zone(
        static_cast<icaltime_t>(utc.toUTC().toSecsSinceEpoch()), false, zone);
  }
  if (timeZone.isEmpty()) {
    // Floating RFC 5545 values follow the desktop's current zone. Keep the
    // canonical field empty, but give libical the system zone while iterating
    // so local wall time remains stable across DST boundaries.
    const QString systemZone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    if (icaltimezone* zone = zoneFor(systemZone)) {
      return icaltime_from_timet_with_zone(
          static_cast<icaltime_t>(utc.toUTC().toSecsSinceEpoch()), false, zone);
    }
  }
  return icaltime_from_timet_with_zone(
      static_cast<icaltime_t>(utc.toUTC().toSecsSinceEpoch()), false,
      icaltimezone_get_utc_timezone());
}

icalproperty* dateTimeProperty(const icalproperty_kind kind, const QDateTime& utc,
                               const QString& timeZone) {
  const icaltimetype value = dateTimeValue(utc, timeZone);
  icalproperty* property = kind == ICAL_DTSTART_PROPERTY
                               ? icalproperty_new_dtstart(value)
                               : icalproperty_new_dtend(value);
  if (property != nullptr && !timeZone.isEmpty() && zoneFor(timeZone)) {
    const QByteArray name = timeZone.toUtf8();
    icalproperty_add_parameter(property, icalparameter_new_tzid(name.constData()));
  }
  return property;
}

bool addProperty(icalcomponent* component, icalproperty* property) {
  if (property == nullptr) {
    return false;
  }
  icalcomponent_add_property(component, property);
  return true;
}

RecurrenceComponent componentFor(const Event& master, QStringList* warnings) {
  RecurrenceComponent result;
  if (!master.dirty && master.rawFormat == QStringLiteral("text/calendar") &&
      !master.rawPayload.isEmpty()) {
    const QByteArray payload = master.rawPayload.toUtf8();
    result.owner.reset(icalcomponent_new_from_string(payload.constData()));
    result.event = findMasterComponent(result.owner.get(), master.uid.toUtf8());
    result.hasRecurrence = hasRecurrenceProperties(result.event);
    if (result.event != nullptr && result.hasRecurrence) {
      return result;
    }
    result.owner.reset();
    result.event = nullptr;
  }

  result.owner.reset(icalcomponent_new_vevent());
  result.event = result.owner.get();
  if (result.event == nullptr) {
    warnings->append(QStringLiteral("recurrence_component_allocation_failed"));
    return result;
  }

  if (master.allDay) {
    const QDate endDate =
        master.endDate.isValid() ? master.endDate : master.startDate.addDays(1);
    if (!addProperty(result.event,
                     icalproperty_new_dtstart(dateValue(master.startDate))) ||
        !addProperty(result.event, icalproperty_new_dtend(dateValue(endDate)))) {
      warnings->append(QStringLiteral("recurrence_time_invalid:%1").arg(master.id));
      return result;
    }
  } else {
    const QDateTime end = master.endUtc.isValid() ? master.endUtc : master.startUtc;
    const QString endZone =
        master.endTimeZone.isEmpty() ? master.startTimeZone : master.endTimeZone;
    if (!addProperty(result.event,
                     dateTimeProperty(ICAL_DTSTART_PROPERTY, master.startUtc,
                                      master.startTimeZone)) ||
        !addProperty(result.event,
                     dateTimeProperty(ICAL_DTEND_PROPERTY, end, endZone))) {
      warnings->append(QStringLiteral("recurrence_time_invalid:%1").arg(master.id));
      return result;
    }
  }

  QString normalizedRules = master.recurrenceRule;
  normalizedRules.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  normalizedRules.replace(QLatin1Char('\r'), QLatin1Char('\n'));
  const QStringList lines =
      normalizedRules.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  for (QString line : lines) {
    line = line.trimmed();
    if (line.startsWith(QStringLiteral("FREQ="), Qt::CaseInsensitive)) {
      line.prepend(QStringLiteral("RRULE:"));
    }
    const qsizetype colon = line.indexOf(QLatin1Char(':'));
    const QString name =
        (colon < 0 ? line : line.left(colon)).section(QLatin1Char(';'), 0, 0).toUpper();
    if (name != QStringLiteral("RRULE") && name != QStringLiteral("RDATE") &&
        name != QStringLiteral("EXRULE") && name != QStringLiteral("EXDATE")) {
      continue;
    }
    icalproperty* property = icalproperty_new_from_string(line.toUtf8().constData());
    if (property == nullptr) {
      warnings->append(QStringLiteral("recurrence_rule_invalid:%1").arg(master.id));
      continue;
    }
    icalcomponent_add_property(result.event, property);
  }
  result.hasRecurrence = hasRecurrenceProperties(result.event);
  if (!result.hasRecurrence) {
    warnings->append(QStringLiteral("recurrence_rule_invalid:%1").arg(master.id));
  }
  return result;
}

void collectOccurrence(icalcomponent*, const icaltime_span* span, void* data) {
  auto* collector = static_cast<Collector*>(data);
  if (span == nullptr || collector == nullptr || collector->master == nullptr) {
    return;
  }
  if (collector->occurrences.size() >= collector->limit) {
    collector->truncated = true;
    return;
  }

  Event occurrence = *collector->master;
  if (occurrence.allDay) {
    const QDate start =
        QDateTime::fromSecsSinceEpoch(static_cast<qint64>(span->start), QTimeZone::UTC)
            .date();
    const qint64 durationDays =
        occurrence.endDate.isValid()
            ? std::max<qint64>(1, occurrence.startDate.daysTo(occurrence.endDate))
            : INT64_C(1);
    occurrence.startDate = start;
    occurrence.endDate = start.addDays(durationDays);
  } else {
    occurrence.startUtc =
        QDateTime::fromSecsSinceEpoch(static_cast<qint64>(span->start), QTimeZone::UTC);
    occurrence.endUtc =
        QDateTime::fromSecsSinceEpoch(static_cast<qint64>(span->end), QTimeZone::UTC);
  }
  occurrence.recurrenceId =
      occurrence.allDay ? occurrence.startDate.toString(Qt::ISODate)
                        : occurrence.startUtc.toUTC().toString(Qt::ISODateWithMs);
  const QString key = occurrenceKey(occurrence);
  if (key.isEmpty() || collector->seenStarts.contains(key)) {
    return;
  }
  collector->seenStarts.insert(key);
  if (overlaps(occurrence, *collector->startUtc, *collector->endUtc)) {
    collector->occurrences.append(std::move(occurrence));
  }
}

QList<Event> expandMaster(const Event& master, const QDateTime& startUtc,
                          const QDateTime& endUtc, qsizetype limit, bool* truncated,
                          QStringList* warnings) {
  RecurrenceComponent component = componentFor(master, warnings);
  if (component.event == nullptr || !component.hasRecurrence) {
    return overlaps(master, startUtc, endUtc) ? QList<Event>{master} : QList<Event>{};
  }

  qint64 duration = 0;
  if (master.allDay && master.startDate.isValid() && master.endDate.isValid()) {
    duration =
        static_cast<qint64>(master.startDate.daysTo(master.endDate)) * 24 * 60 * 60;
  } else if (master.startUtc.isValid() && master.endUtc.isValid()) {
    duration = std::max<qint64>(0, master.startUtc.secsTo(master.endUtc));
  }
  const QDateTime scanStart = startUtc.addSecs(-duration);
  const icaltimetype from = icaltime_from_timet_with_zone(
      static_cast<icaltime_t>(scanStart.toSecsSinceEpoch()), false,
      icaltimezone_get_utc_timezone());
  const icaltimetype until =
      icaltime_from_timet_with_zone(static_cast<icaltime_t>(endUtc.toSecsSinceEpoch()),
                                    false, icaltimezone_get_utc_timezone());

  Collector collector;
  collector.master = &master;
  collector.startUtc = &startUtc;
  collector.endUtc = &endUtc;
  collector.limit = std::max<qsizetype>(1, limit);
  icalcomponent_foreach_recurrence(component.event, from, until, collectOccurrence,
                                   &collector);
  if (collector.truncated) {
    *truncated = true;
  }
  return collector.occurrences;
}

bool exceptionWins(const Event& candidate, const Event& current) {
  if (candidate.sequence != current.sequence) {
    return candidate.sequence > current.sequence;
  }
  return candidate.updatedAt > current.updatedAt;
}

void sortOccurrences(QList<Event>* events) {
  std::sort(events->begin(), events->end(), [](const Event& left, const Event& right) {
    if (left.allDay != right.allDay) {
      return left.allDay;
    }
    if (left.allDay && left.startDate != right.startDate) {
      return left.startDate < right.startDate;
    }
    if (!left.allDay && left.startUtc != right.startUtc) {
      return left.startUtc < right.startUtc;
    }
    if (left.id != right.id) {
      return left.id < right.id;
    }
    return left.recurrenceId < right.recurrenceId;
  });
}

}  // namespace

RecurrenceExpansionResult RecurrenceExpander::expand(
    const QList<Event>& events, const QDateTime& startUtc, const QDateTime& endUtc,
    const qsizetype maximumOccurrences) {
  RecurrenceExpansionResult result;
  if (!startUtc.isValid() || !endUtc.isValid() || startUtc >= endUtc) {
    result.warnings.append(QStringLiteral("invalid_expansion_range"));
    return result;
  }
  const qsizetype limit = std::max<qsizetype>(1, maximumOccurrences);

  QList<qsizetype> masters;
  QList<qsizetype> exceptions;
  for (qsizetype index = 0; index < events.size(); ++index) {
    const Event& event = events.at(index);
    if (!event.recurrenceId.isEmpty()) {
      exceptions.append(index);
    } else if (!event.recurrenceRule.isEmpty()) {
      masters.append(index);
    } else if (!isCancelled(event) && overlaps(event, startUtc, endUtc)) {
      if (result.occurrences.size() < limit) {
        result.occurrences.append(event);
      } else {
        result.truncated = true;
      }
    }
  }

  QSet<qsizetype> consumedExceptions;
  for (const qsizetype masterIndex : masters) {
    const Event& master = events.at(masterIndex);
    QList<qsizetype> matchingExceptions;
    for (const qsizetype exceptionIndex : exceptions) {
      if (!consumedExceptions.contains(exceptionIndex) &&
          belongsToMaster(events.at(exceptionIndex), master)) {
        matchingExceptions.append(exceptionIndex);
      }
    }

    if (isCancelled(master)) {
      for (const qsizetype index : matchingExceptions) {
        consumedExceptions.insert(index);
      }
      continue;
    }

    bool masterTruncated = false;
    QList<Event> generated =
        expandMaster(master, startUtc, endUtc,
                     std::max<qsizetype>(1, limit - result.occurrences.size()),
                     &masterTruncated, &result.warnings);
    result.truncated = result.truncated || masterTruncated;

    QHash<QString, qsizetype> exceptionsByKey;
    for (const qsizetype index : matchingExceptions) {
      const QString key = recurrenceKey(events.at(index), master);
      if (key.isEmpty()) {
        result.warnings.append(
            QStringLiteral("recurrence_id_invalid:%1").arg(events.at(index).id));
        continue;
      }
      const auto current = exceptionsByKey.constFind(key);
      if (current == exceptionsByKey.cend() ||
          exceptionWins(events.at(index), events.at(current.value()))) {
        exceptionsByKey.insert(key, index);
      }
    }

    for (Event& occurrence : generated) {
      const QString key = occurrenceKey(occurrence);
      const auto exception = exceptionsByKey.constFind(key);
      if (exception == exceptionsByKey.cend()) {
        if (result.occurrences.size() < limit) {
          result.occurrences.append(std::move(occurrence));
        } else {
          result.truncated = true;
        }
        continue;
      }
      const qsizetype exceptionIndex = exception.value();
      consumedExceptions.insert(exceptionIndex);
      const Event& replacement = events.at(exceptionIndex);
      if (!isCancelled(replacement) && overlaps(replacement, startUtc, endUtc)) {
        if (result.occurrences.size() < limit) {
          result.occurrences.append(replacement);
        } else {
          result.truncated = true;
        }
      }
    }

    // A moved exception can be inside this query even when its original start
    // was outside it and therefore no generated occurrence was visited.
    for (const qsizetype index : matchingExceptions) {
      if (consumedExceptions.contains(index)) {
        continue;
      }
      consumedExceptions.insert(index);
      const Event& exception = events.at(index);
      if (!isCancelled(exception) && overlaps(exception, startUtc, endUtc)) {
        if (result.occurrences.size() < limit) {
          result.occurrences.append(exception);
        } else {
          result.truncated = true;
        }
      }
    }
  }

  for (const qsizetype exceptionIndex : exceptions) {
    if (consumedExceptions.contains(exceptionIndex)) {
      continue;
    }
    const Event& exception = events.at(exceptionIndex);
    if (!isCancelled(exception) && overlaps(exception, startUtc, endUtc)) {
      if (result.occurrences.size() < limit) {
        result.occurrences.append(exception);
      } else {
        result.truncated = true;
      }
    }
  }

  sortOccurrences(&result.occurrences);
  return result;
}

}  // namespace omacalendar
