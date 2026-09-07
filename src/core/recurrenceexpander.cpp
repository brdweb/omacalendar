#include "core/recurrenceexpander.h"

#include <libical/ical.h>

#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <QTime>
#include <QTimeZone>
#include <algorithm>
#include <memory>
#include <optional>

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

struct RecurrenceIteratorDeleter {
  void operator()(icalrecur_iterator* iterator) const {
    if (iterator != nullptr) {
      icalrecur_iterator_free(iterator);
    }
  }
};

using RecurrenceIteratorPtr =
    std::unique_ptr<icalrecur_iterator, RecurrenceIteratorDeleter>;

struct WorkBudget {
  qsizetype remaining = 0;
  bool exhausted = false;

  bool spend() {
    if (remaining <= 0) {
      exhausted = true;
      return false;
    }
    --remaining;
    return true;
  }
};

constexpr qsizetype kMaximumComponentTraversal = 4096;
constexpr int kMaximumComponentDepth = 64;

void sortOccurrences(QList<Event>* events);

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

QString parentKey(const QString& kind, const QString& calendarId,
                  const QString& identity) {
  return identity.isEmpty()
             ? QString()
             : kind + QLatin1Char('\n') + calendarId + QLatin1Char('\n') + identity;
}

icalcomponent* findMasterComponent(icalcomponent* root, const QByteArray& uid,
                                   bool* traversalExceeded) {
  if (root == nullptr) {
    return nullptr;
  }
  QList<QPair<icalcomponent*, int>> pending{{root, 0}};
  qsizetype visited = 0;
  while (!pending.isEmpty()) {
    const auto [component, depth] = pending.takeLast();
    if (++visited > kMaximumComponentTraversal || depth > kMaximumComponentDepth) {
      *traversalExceeded = true;
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
      continue;
    }

    for (icalcomponent* child =
             icalcomponent_get_first_component(component, ICAL_ANY_COMPONENT);
         child != nullptr;
         child = icalcomponent_get_next_component(component, ICAL_ANY_COMPONENT)) {
      if (pending.size() >= kMaximumComponentTraversal) {
        *traversalExceeded = true;
        return nullptr;
      }
      pending.append({child, depth + 1});
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
    bool traversalExceeded = false;
    result.event = findMasterComponent(result.owner.get(), master.uid.toUtf8(),
                                       &traversalExceeded);
    if (traversalExceeded) {
      warnings->append(
          QStringLiteral("recurrence_component_limit_exceeded:%1").arg(master.id));
      result.owner.reset();
      result.event = nullptr;
      return result;
    }
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

void applyPropertyTimeZone(icalproperty* property, icaltimetype* value) {
  const char* zoneName = icalproperty_get_parameter_as_string(property, "TZID");
  if (zoneName == nullptr || *zoneName == '\0' || icaltime_is_utc(*value)) {
    return;
  }
  if (icaltimezone* zone = zoneFor(QString::fromUtf8(zoneName))) {
    icaltime_set_timezone(value, zone);
  }
}

QDateTime occurrenceUtc(const icaltimetype& value, const Event& master) {
  if (icaltime_is_null_time(value) || !icaltime_is_valid_time(value)) {
    return {};
  }
  const icaltimezone* valueZone = icaltime_get_timezone(value);
  icaltimezone* fallback = nullptr;
  if (valueZone == nullptr) {
    const QString zoneName = master.startTimeZone.isEmpty()
                                 ? QString::fromUtf8(QTimeZone::systemTimeZoneId())
                                 : master.startTimeZone;
    fallback = zoneFor(zoneName);
  }
  if (valueZone == nullptr && fallback == nullptr) {
    fallback = icaltimezone_get_utc_timezone();
  }
  const icaltime_t seconds = icaltime_as_timet_with_zone(
      value, const_cast<icaltimezone*>(valueZone == nullptr ? fallback : valueZone));
  return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(seconds), QTimeZone::UTC);
}

Event occurrenceFrom(const Event& master, const icaltimetype& start,
                     const std::optional<icaltimetype>& explicitEnd = std::nullopt,
                     const std::optional<qint64>& explicitDuration = std::nullopt) {
  Event occurrence = master;
  if (master.allDay || icaltime_is_date(start)) {
    occurrence.allDay = true;
    occurrence.startDate = QDate(start.year, start.month, start.day);
    qint64 durationDays =
        master.endDate.isValid()
            ? std::max<qint64>(1, master.startDate.daysTo(master.endDate))
            : INT64_C(1);
    if (explicitEnd.has_value() && icaltime_is_date(*explicitEnd)) {
      const QDate end(explicitEnd->year, explicitEnd->month, explicitEnd->day);
      durationDays = std::max<qint64>(1, occurrence.startDate.daysTo(end));
    } else if (explicitDuration.has_value()) {
      durationDays = std::max<qint64>(1, *explicitDuration / (24 * 60 * 60));
    }
    occurrence.endDate = occurrence.startDate.addDays(durationDays);
  } else {
    occurrence.allDay = false;
    occurrence.startUtc = occurrenceUtc(start, master);
    const qint64 masterDuration =
        master.endUtc.isValid()
            ? std::max<qint64>(0, master.startUtc.secsTo(master.endUtc))
            : 0;
    if (explicitEnd.has_value()) {
      occurrence.endUtc = occurrenceUtc(*explicitEnd, master);
    } else {
      occurrence.endUtc =
          occurrence.startUtc.addSecs(explicitDuration.value_or(masterDuration));
    }
  }
  occurrence.recurrenceId =
      occurrence.allDay ? occurrence.startDate.toString(Qt::ISODate)
                        : occurrence.startUtc.toUTC().toString(Qt::ISODateWithMs);
  return occurrence;
}

bool enumerateRule(icalproperty* property, const icaltimetype& dtstart,
                   const icaltimetype& from, const icaltimetype& until,
                   const Event& master, WorkBudget* budget, QList<Event>* output) {
  if (!budget->spend()) {
    return false;
  }
  icalrecurrencetype* rule = icalproperty_isa(property) == ICAL_EXRULE_PROPERTY
                                 ? icalproperty_get_exrule(property)
                                 : icalproperty_get_rrule(property);
  RecurrenceIteratorPtr iterator(
      rule == nullptr ? nullptr : icalrecur_iterator_new(rule, dtstart));
  if (!iterator) {
    return true;
  }
  icalrecur_iterator_set_end(iterator.get(), until);
  if (rule->count == 0 && icaltime_compare(from, dtstart) > 0) {
    icalrecur_iterator_set_start(iterator.get(), from);
  }
  while (true) {
    if (!budget->spend()) {
      return false;
    }
    const icaltimetype next = icalrecur_iterator_next(iterator.get());
    if (icaltime_is_null_time(next)) {
      return true;
    }
    if (icaltime_compare(next, from) >= 0 && icaltime_compare(next, until) < 0) {
      output->append(occurrenceFrom(master, next));
    }
  }
}

bool enumerateDateProperties(icalcomponent* component, const icalproperty_kind kind,
                             const Event& master, WorkBudget* budget,
                             QList<Event>* output) {
  for (icalproperty* property = icalcomponent_get_first_property(component, kind);
       property != nullptr;
       property = icalcomponent_get_next_property(component, kind)) {
    if (!budget->spend()) {
      return false;
    }
    const char* encoded = icalproperty_get_value_as_string(property);
    const QList<QByteArray> values =
        encoded == nullptr ? QList<QByteArray>{} : QByteArray(encoded).split(',');
    for (const QByteArray& rawValue : values) {
      if (!budget->spend()) {
        return false;
      }
      const QByteArray value = rawValue.trimmed();
      if (value.isEmpty()) {
        continue;
      }
      if (kind == ICAL_RDATE_PROPERTY && value.contains('/')) {
        icalperiodtype period = icalperiodtype_from_string(value.constData());
        if (!icalperiodtype_is_valid_period(period)) {
          continue;
        }
        applyPropertyTimeZone(property, &period.start);
        if (!icaltime_is_null_time(period.end)) {
          applyPropertyTimeZone(property, &period.end);
          output->append(occurrenceFrom(master, period.start, period.end));
        } else {
          output->append(occurrenceFrom(
              master, period.start, std::nullopt,
              static_cast<qint64>(icaldurationtype_as_seconds(period.duration))));
        }
        continue;
      }
      icaltimetype time = icaltime_from_string(value.constData());
      if (icaltime_is_null_time(time) || !icaltime_is_valid_time(time)) {
        continue;
      }
      applyPropertyTimeZone(property, &time);
      output->append(occurrenceFrom(master, time));
    }
  }
  return true;
}

QList<Event> expandMaster(const Event& master, const QDateTime& startUtc,
                          const QDateTime& endUtc, qsizetype limit, WorkBudget* budget,
                          bool* truncated, QStringList* warnings) {
  if (master.recurrenceRule.isEmpty() &&
      (master.rawFormat != QStringLiteral("text/calendar") ||
       master.rawPayload.isEmpty())) {
    return overlaps(master, startUtc, endUtc) ? QList<Event>{master} : QList<Event>{};
  }
  RecurrenceComponent component = componentFor(master, warnings);
  if (component.event == nullptr) {
    *truncated = true;
    return {};
  }
  if (!component.hasRecurrence) {
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
  const icaltimetype dtstart = icalcomponent_get_dtstart(component.event);

  QList<Event> excluded;
  for (icalproperty* property =
           icalcomponent_get_first_property(component.event, ICAL_EXRULE_PROPERTY);
       property != nullptr; property = icalcomponent_get_next_property(
                                component.event, ICAL_EXRULE_PROPERTY)) {
    if (!enumerateRule(property, dtstart, from, until, master, budget, &excluded)) {
      *truncated = true;
      return {};
    }
  }
  if (!enumerateDateProperties(component.event, ICAL_EXDATE_PROPERTY, master, budget,
                               &excluded)) {
    *truncated = true;
    return {};
  }
  QSet<QString> excludedKeys;
  for (const Event& event : std::as_const(excluded)) {
    excludedKeys.insert(occurrenceKey(event));
  }

  QList<Event> candidates;
  if (!budget->spend()) {
    *truncated = true;
    return {};
  }
  candidates.append(occurrenceFrom(master, dtstart));
  for (icalproperty* property =
           icalcomponent_get_first_property(component.event, ICAL_RRULE_PROPERTY);
       property != nullptr; property = icalcomponent_get_next_property(
                                component.event, ICAL_RRULE_PROPERTY)) {
    if (!enumerateRule(property, dtstart, from, until, master, budget, &candidates)) {
      *truncated = true;
      return {};
    }
  }
  if (!enumerateDateProperties(component.event, ICAL_RDATE_PROPERTY, master, budget,
                               &candidates)) {
    *truncated = true;
    return {};
  }

  sortOccurrences(&candidates);
  QList<Event> occurrences;
  QSet<QString> seenStarts;
  for (Event& occurrence : candidates) {
    const QString key = occurrenceKey(occurrence);
    if (key.isEmpty() || seenStarts.contains(key) || excludedKeys.contains(key)) {
      continue;
    }
    seenStarts.insert(key);
    if (!overlaps(occurrence, startUtc, endUtc)) {
      continue;
    }
    if (occurrences.size() >= std::max<qsizetype>(1, limit)) {
      *truncated = true;
      continue;
    }
    occurrences.append(std::move(occurrence));
  }
  return occurrences;
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
    const qsizetype maximumOccurrences, const qsizetype maximumExpansionSteps) {
  RecurrenceExpansionResult result;
  if (!startUtc.isValid() || !endUtc.isValid() || startUtc >= endUtc) {
    result.warnings.append(QStringLiteral("invalid_expansion_range"));
    return result;
  }
  const qsizetype limit = std::max<qsizetype>(1, maximumOccurrences);
  WorkBudget budget{std::max<qsizetype>(1, maximumExpansionSteps)};
  const auto markWorkLimit = [&result]() {
    result.truncated = true;
    if (!result.warnings.contains(QStringLiteral("recurrence_work_limit_exceeded"))) {
      result.warnings.append(QStringLiteral("recurrence_work_limit_exceeded"));
    }
  };

  QList<qsizetype> masters;
  QList<qsizetype> exceptions;
  QHash<QString, QList<qsizetype>> exceptionsByParent;
  for (qsizetype index = 0; index < events.size(); ++index) {
    if (!budget.spend()) {
      markWorkLimit();
      result.occurrences.clear();
      return result;
    }
    const Event& event = events.at(index);
    if (!event.recurrenceId.isEmpty()) {
      exceptions.append(index);
      const QString googleParent = googleRecurringParentId(event);
      const QString key =
          googleParent.isEmpty()
              ? parentKey(QStringLiteral("uid"), event.calendarId, event.uid)
              : parentKey(QStringLiteral("google"), event.calendarId, googleParent);
      if (!key.isEmpty()) {
        exceptionsByParent[key].append(index);
      }
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
    if (!budget.spend()) {
      markWorkLimit();
      break;
    }
    if (result.occurrences.size() >= limit) {
      result.truncated = true;
      break;
    }
    const Event& master = events.at(masterIndex);
    QList<qsizetype> matchingExceptions;
    const QList<QString> keys{
        parentKey(QStringLiteral("google"), master.calendarId, master.remoteId),
        parentKey(QStringLiteral("uid"), master.calendarId, master.uid)};
    for (const QString& key : keys) {
      if (key.isEmpty()) {
        continue;
      }
      for (const qsizetype exceptionIndex : exceptionsByParent.value(key)) {
        if (!budget.spend()) {
          markWorkLimit();
          break;
        }
        if (!consumedExceptions.contains(exceptionIndex)) {
          matchingExceptions.append(exceptionIndex);
        }
      }
      if (budget.exhausted) {
        break;
      }
    }
    if (budget.exhausted) {
      break;
    }

    if (isCancelled(master)) {
      for (const qsizetype index : matchingExceptions) {
        if (!budget.spend()) {
          markWorkLimit();
          break;
        }
        if (!consumedExceptions.contains(index)) {
          consumedExceptions.insert(index);
        }
      }
      if (budget.exhausted) {
        break;
      }
      continue;
    }

    bool masterTruncated = false;
    QList<Event> generated =
        expandMaster(master, startUtc, endUtc,
                     std::max<qsizetype>(1, limit - result.occurrences.size()), &budget,
                     &masterTruncated, &result.warnings);
    result.truncated = result.truncated || masterTruncated;
    if (budget.exhausted) {
      markWorkLimit();
      break;
    }

    QHash<QString, qsizetype> exceptionsByKey;
    QHash<QString, qsizetype> rangeByKey;
    for (const qsizetype index : matchingExceptions) {
      if (!budget.spend()) {
        markWorkLimit();
        break;
      }
      const QString key = recurrenceKey(events.at(index), master);
      if (key.isEmpty()) {
        result.warnings.append(
            QStringLiteral("recurrence_id_invalid:%1").arg(events.at(index).id));
        continue;
      }
      QHash<QString, qsizetype>* target =
          isThisAndFuture(events.at(index)) ? &rangeByKey : &exceptionsByKey;
      const auto current = target->constFind(key);
      if (current == target->cend() ||
          exceptionWins(events.at(index), events.at(current.value()))) {
        if (current != target->cend()) {
          consumedExceptions.insert(current.value());
        }
        target->insert(key, index);
      } else {
        consumedExceptions.insert(index);
      }
      if (target == &rangeByKey) {
        consumedExceptions.insert(index);
      }
    }
    if (budget.exhausted) {
      break;
    }
    QList<QPair<QString, qsizetype>> rangeExceptions;
    rangeExceptions.reserve(rangeByKey.size());
    for (auto iterator = rangeByKey.cbegin(); iterator != rangeByKey.cend();
         ++iterator) {
      rangeExceptions.append({iterator.key(), iterator.value()});
    }
    std::sort(
        rangeExceptions.begin(), rangeExceptions.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; });

    for (Event& occurrence : generated) {
      if (!budget.spend()) {
        markWorkLimit();
        break;
      }
      const QString key = occurrenceKey(occurrence);
      const auto exception = exceptionsByKey.constFind(key);
      if (exception == exceptionsByKey.cend()) {
        const auto upper = std::upper_bound(
            rangeExceptions.cbegin(), rangeExceptions.cend(), key,
            [](const QString& value, const QPair<QString, qsizetype>& candidate) {
              return value < candidate.first;
            });
        if (upper != rangeExceptions.cbegin()) {
          const auto selected = std::prev(upper);
          const Event replacement = applyRangeException(events.at(selected->second),
                                                        selected->first, occurrence);
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
    if (budget.exhausted) {
      break;
    }

    // A moved exception can be inside this query even when its original start
    // was outside it and therefore no generated occurrence was visited.
    for (const qsizetype index : matchingExceptions) {
      if (!budget.spend()) {
        markWorkLimit();
        break;
      }
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
    if (budget.exhausted) {
      break;
    }
  }

  if (!budget.exhausted) {
    for (const qsizetype exceptionIndex : exceptions) {
      if (!budget.spend()) {
        markWorkLimit();
        break;
      }
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
  }

  sortOccurrences(&result.occurrences);
  return result;
}

}  // namespace omacalendar
