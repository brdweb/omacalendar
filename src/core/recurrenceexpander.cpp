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

QString recurrenceKey(const Event& exception, const Event& master) {
  const TimeKind timeKind = master.allDay ? TimeKind::AllDay : master.timeKind;
  const QString canonical = canonicalRecurrenceIdentity(
      exception.recurrenceId, master.allDay, timeKind, master.startTimeZone);
  if (canonical.startsWith(QStringLiteral("D:"))) {
    return canonical;
  }
  if (canonical.startsWith(QStringLiteral("Z:"))) {
    const QDateTime dateTime = dateTimeFromIso(canonical.sliced(2));
    return dateTime.isValid()
               ? QStringLiteral("T:") + QString::number(dateTime.toMSecsSinceEpoch())
               : QString();
  }
  if (!canonical.startsWith(QStringLiteral("F:")) ||
      canonical.startsWith(QStringLiteral("F:offset:"))) {
    return {};
  }
  const QDateTime wall = QDateTime::fromString(canonical.sliced(2), Qt::ISODateWithMs);
  if (!wall.isValid()) {
    return {};
  }
  const QDateTime local(wall.date(), wall.time(), QTimeZone::systemTimeZone());
  return QStringLiteral("T:") + QString::number(local.toMSecsSinceEpoch());
}

bool isThisAndFuture(const Event& exception) {
  const QString parameters = exception.recurrenceId.section(QLatin1Char(':'), 0, 0);
  return parameters.contains(QStringLiteral("RANGE=THISANDFUTURE"),
                             Qt::CaseInsensitive);
}

Event applyRangeException(const Event& range, const QString& anchorKey,
                          const Event& occurrence) {
  Event replacement = range;
  replacement.recurrenceId = occurrence.recurrenceId;
  if (range.allDay && occurrence.allDay && anchorKey.startsWith(QStringLiteral("D:"))) {
    const QDate anchor = QDate::fromString(anchorKey.sliced(2), Qt::ISODate);
    if (anchor.isValid() && range.startDate.isValid()) {
      const qint64 offset = anchor.daysTo(occurrence.startDate);
      const qint64 duration =
          range.endDate.isValid()
              ? qMax<qint64>(1, range.startDate.daysTo(range.endDate))
              : 1;
      replacement.startDate = range.startDate.addDays(offset);
      replacement.endDate = replacement.startDate.addDays(duration);
    }
    return replacement;
  }
  if (!range.allDay && !occurrence.allDay &&
      anchorKey.startsWith(QStringLiteral("T:"))) {
    bool ok = false;
    const qint64 anchorMilliseconds = anchorKey.sliced(2).toLongLong(&ok);
    if (ok && range.startUtc.isValid()) {
      const QDateTime anchor =
          QDateTime::fromMSecsSinceEpoch(anchorMilliseconds, QTimeZone::UTC);
      const qint64 offset = anchor.msecsTo(occurrence.startUtc);
      const qint64 duration =
          range.endUtc.isValid() ? range.startUtc.msecsTo(range.endUtc) : 0;
      replacement.startUtc = range.startUtc.addMSecs(offset);
      replacement.endUtc = replacement.startUtc.addMSecs(qMax<qint64>(0, duration));
    }
  }
  return replacement;
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
    QList<QPair<QString, qsizetype>> rangeExceptions;
    for (const qsizetype index : matchingExceptions) {
      const QString key = recurrenceKey(events.at(index), master);
      if (key.isEmpty()) {
        result.warnings.append(
            QStringLiteral("recurrence_id_invalid:%1").arg(events.at(index).id));
        continue;
      }
      if (isThisAndFuture(events.at(index))) {
        rangeExceptions.append({key, index});
        consumedExceptions.insert(index);
        continue;
      }
      const auto current = exceptionsByKey.constFind(key);
      if (current == exceptionsByKey.cend() ||
          exceptionWins(events.at(index), events.at(current.value()))) {
        exceptionsByKey.insert(key, index);
      }
    }
    std::sort(
        rangeExceptions.begin(), rangeExceptions.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; });

    for (Event& occurrence : generated) {
      const QString key = occurrenceKey(occurrence);
      const auto exception = exceptionsByKey.constFind(key);
      if (exception == exceptionsByKey.cend()) {
        qsizetype rangeIndex = -1;
        QString rangeAnchor;
        for (const auto& candidate : rangeExceptions) {
          if (candidate.first > key) {
            break;
          }
          rangeAnchor = candidate.first;
          rangeIndex = candidate.second;
        }
        if (rangeIndex >= 0) {
          const Event replacement =
              applyRangeException(events.at(rangeIndex), rangeAnchor, occurrence);
          if (!isCancelled(replacement) && overlaps(replacement, startUtc, endUtc)) {
            if (result.occurrences.size() < limit) {
              result.occurrences.append(replacement);
            } else {
              result.truncated = true;
            }
          }
          continue;
        }
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
