#include "providers/caldav/vtodocodec.h"

#include <libical/ical.h>

#include <QTimeZone>
#include <algorithm>
#include <memory>

namespace omacalendar::caldav {
namespace {

constexpr qsizetype kMaximumTaskBytes = 4 * 1024 * 1024;

struct ComponentDeleter {
  void operator()(icalcomponent* component) const {
    if (component != nullptr) {
      icalcomponent_free(component);
    }
  }
};
using ComponentPtr = std::unique_ptr<icalcomponent, ComponentDeleter>;

void setError(QString* errorMessage, const QString& message) {
  if (errorMessage != nullptr) {
    *errorMessage = message;
  }
}

QString fromIcal(const char* value) {
  return value == nullptr ? QString() : QString::fromUtf8(value);
}

QByteArray toIcal(const QString& value) { return value.toUtf8(); }

icaltimetype dateValue(const QDate& date) {
  const QByteArray encoded = date.toString(QStringLiteral("yyyyMMdd")).toLatin1();
  return icaltime_from_string(encoded.constData());
}

icaltimetype utcValue(const QDateTime& dateTime) {
  return icaltime_from_timet_with_zone(
      static_cast<icaltime_t>(dateTime.toUTC().toSecsSinceEpoch()), false,
      icaltimezone_get_utc_timezone());
}

QDateTime utcFrom(icaltimetype value, const QString& timeZoneId) {
  const QDate date(value.year, value.month, value.day);
  const QTime time(value.hour, value.minute, value.second);
  if (!date.isValid() || !time.isValid()) {
    return {};
  }
  if (icaltime_is_utc(value)) {
    return QDateTime(date, time, QTimeZone::UTC);
  }
  const QTimeZone zone(timeZoneId.toUtf8());
  // Floating times, and zones Qt does not know, are read in the local zone.
  return QDateTime(date, time, zone.isValid() ? zone : QTimeZone(QTimeZone::LocalTime))
      .toUTC();
}

QString timeZoneId(icalproperty* property) {
  icalparameter* value =
      icalproperty_get_first_parameter(property, ICAL_TZID_PARAMETER);
  return value == nullptr ? QString() : fromIcal(icalparameter_get_tzid(value));
}

icalcomponent* firstTask(icalcomponent* calendar) {
  if (icalcomponent_isa(calendar) == ICAL_VTODO_COMPONENT) {
    return calendar;
  }
  for (icalcomponent* todo =
           icalcomponent_get_first_component(calendar, ICAL_VTODO_COMPONENT);
       todo != nullptr;
       todo = icalcomponent_get_next_component(calendar, ICAL_VTODO_COMPONENT)) {
    if (icalcomponent_get_first_property(todo, ICAL_RECURRENCEID_PROPERTY) == nullptr) {
      return todo;
    }
  }
  return nullptr;
}

void removeAll(icalcomponent* component, icalproperty_kind kind) {
  while (icalproperty* property = icalcomponent_get_first_property(component, kind)) {
    icalcomponent_remove_property(component, property);
    icalproperty_free(property);
  }
}

Task parseTodo(icalcomponent* todo) {
  Task task;
  task.uid = fromIcal(icalcomponent_get_uid(todo));
  task.title = fromIcal(icalcomponent_get_summary(todo));
  task.notes = fromIcal(icalcomponent_get_description(todo));

  if (icalproperty* due = icalcomponent_get_first_property(todo, ICAL_DUE_PROPERTY)) {
    const icaltimetype value = icalproperty_get_due(due);
    if (!icaltime_is_null_time(value) && icaltime_is_valid_time(value)) {
      if (icaltime_is_date(value)) {
        task.dueDate = QDate(value.year, value.month, value.day);
      } else {
        task.dueUtc = utcFrom(value, timeZoneId(due));
        // The day as written; a UTC time is shown on its local day.
        task.dueDate = icaltime_is_utc(value)
                           ? task.dueUtc.toLocalTime().date()
                           : QDate(value.year, value.month, value.day);
      }
    }
  }

  const icalproperty_status status = icalcomponent_get_status(todo);
  icalproperty* completed =
      icalcomponent_get_first_property(todo, ICAL_COMPLETED_PROPERTY);
  icalproperty* percent =
      icalcomponent_get_first_property(todo, ICAL_PERCENTCOMPLETE_PROPERTY);
  task.completed =
      status == ICAL_STATUS_COMPLETED || completed != nullptr ||
      (percent != nullptr && icalproperty_get_percentcomplete(percent) >= 100);
  if (completed != nullptr) {
    task.completedAt = utcFrom(icalproperty_get_completed(completed), {});
  }
  if (icalproperty* priority =
          icalcomponent_get_first_property(todo, ICAL_PRIORITY_PROPERTY)) {
    task.priority = std::clamp(icalproperty_get_priority(priority), 0, 9);
  }
  for (icalproperty* related =
           icalcomponent_get_first_property(todo, ICAL_RELATEDTO_PROPERTY);
       related != nullptr;
       related = icalcomponent_get_next_property(todo, ICAL_RELATEDTO_PROPERTY)) {
    icalparameter* type =
        icalproperty_get_first_parameter(related, ICAL_RELTYPE_PARAMETER);
    if (type == nullptr || icalparameter_get_reltype(type) == ICAL_RELTYPE_PARENT) {
      task.parentId = fromIcal(icalproperty_get_relatedto(related));
      break;
    }
  }
  return task;
}

bool sameDue(const Task& left, const Task& right) {
  return left.dueDate == right.dueDate && left.dueUtc == right.dueUtc;
}

void writeDue(icalcomponent* todo, const Task& task) {
  removeAll(todo, ICAL_DUE_PROPERTY);
  if (!task.dueDate.isValid()) {
    return;
  }
  icalproperty* due = icalproperty_new_due(
      task.dueUtc.isValid() ? utcValue(task.dueUtc) : dateValue(task.dueDate));
  if (!task.dueUtc.isValid()) {
    icalproperty_add_parameter(due, icalparameter_new_value(ICAL_VALUE_DATE));
  }
  icalcomponent_add_property(todo, due);
}

void writeCompletion(icalcomponent* todo, const Task& task) {
  removeAll(todo, ICAL_COMPLETED_PROPERTY);
  removeAll(todo, ICAL_PERCENTCOMPLETE_PROPERTY);
  if (task.completed) {
    icalcomponent_set_status(todo, ICAL_STATUS_COMPLETED);
    icalcomponent_add_property(
        todo, icalproperty_new_completed(utcValue(
                  task.completedAt.isValid() ? task.completedAt
                                             : QDateTime::currentDateTimeUtc())));
    icalcomponent_add_property(todo, icalproperty_new_percentcomplete(100));
  } else {
    icalcomponent_set_status(todo, ICAL_STATUS_NEEDSACTION);
  }
}

void writeText(icalcomponent* todo, icalproperty_kind kind, const QString& value) {
  removeAll(todo, kind);
  if (value.isEmpty()) {
    return;
  }
  const QByteArray encoded = toIcal(value);
  icalcomponent_add_property(todo,
                             kind == ICAL_SUMMARY_PROPERTY
                                 ? icalproperty_new_summary(encoded.constData())
                                 : icalproperty_new_description(encoded.constData()));
}

void stamp(icalcomponent* todo) {
  const icaltimetype now = utcValue(QDateTime::currentDateTimeUtc());
  removeAll(todo, ICAL_DTSTAMP_PROPERTY);
  removeAll(todo, ICAL_LASTMODIFIED_PROPERTY);
  icalcomponent_add_property(todo, icalproperty_new_dtstamp(now));
  icalcomponent_add_property(todo, icalproperty_new_lastmodified(now));
}

QByteArray render(icalcomponent* calendar) {
  char* text = icalcomponent_as_ical_string_r(calendar);
  if (text == nullptr) {
    return {};
  }
  QByteArray result(text);
  icalmemory_free_buffer(text);
  return result;
}

}  // namespace

std::optional<Task> VTodoCodec::parse(const QByteArray& payload,
                                      QString* errorMessage) {
  if (payload.isEmpty() || payload.size() > kMaximumTaskBytes ||
      payload.contains('\0')) {
    setError(errorMessage, QStringLiteral("The task resource is empty or too large"));
    return std::nullopt;
  }
  icalerror_clear_errno();
  const ComponentPtr calendar(icalcomponent_new_from_string(payload.constData()));
  icalerror_clear_errno();
  if (!calendar) {
    setError(errorMessage, QStringLiteral("The task resource could not be parsed"));
    return std::nullopt;
  }
  icalcomponent* todo = firstTask(calendar.get());
  if (todo == nullptr) {
    setError(errorMessage, QStringLiteral("The resource contains no VTODO"));
    return std::nullopt;
  }
  Task task = parseTodo(todo);
  if (task.uid.isEmpty()) {
    setError(errorMessage, QStringLiteral("The VTODO has no UID"));
    return std::nullopt;
  }
  return task;
}

QByteArray VTodoCodec::serialize(const Task& task, const QByteArray& retainedPayload,
                                 QString* errorMessage) {
  if (task.uid.isEmpty()) {
    setError(errorMessage, QStringLiteral("A task needs a UID to be uploaded"));
    return {};
  }
  ComponentPtr calendar;
  icalcomponent* todo = nullptr;
  if (!retainedPayload.isEmpty()) {
    icalerror_clear_errno();
    calendar.reset(icalcomponent_new_from_string(retainedPayload.constData()));
    icalerror_clear_errno();
    todo = calendar ? firstTask(calendar.get()) : nullptr;
  }
  if (todo == nullptr) {
    calendar.reset(icalcomponent_new_vcalendar());
    icalcomponent_add_property(calendar.get(), icalproperty_new_version("2.0"));
    icalcomponent_add_property(
        calendar.get(),
        icalproperty_new_prodid("-//OmaCalendar//OmaCalendar Tasks//EN"));
    todo = icalcomponent_new_vtodo();
    const QByteArray uid = toIcal(task.uid);
    icalcomponent_set_uid(todo, uid.constData());
    icalcomponent_add_property(
        todo, icalproperty_new_created(utcValue(QDateTime::currentDateTimeUtc())));
    icalcomponent_add_component(calendar.get(), todo);
    writeText(todo, ICAL_SUMMARY_PROPERTY, task.title);
    writeText(todo, ICAL_DESCRIPTION_PROPERTY, task.notes);
    writeDue(todo, task);
    writeCompletion(todo, task);
    if (task.priority > 0) {
      icalcomponent_add_property(todo, icalproperty_new_priority(task.priority));
    }
  } else {
    // Only touch what changed, so provider-owned details survive.
    const Task before = parseTodo(todo);
    if (before.title != task.title) {
      writeText(todo, ICAL_SUMMARY_PROPERTY, task.title);
    }
    if (before.notes != task.notes) {
      writeText(todo, ICAL_DESCRIPTION_PROPERTY, task.notes);
    }
    if (!sameDue(before, task)) {
      writeDue(todo, task);
    }
    if (before.completed != task.completed) {
      writeCompletion(todo, task);
    }
    if (before.priority != task.priority) {
      removeAll(todo, ICAL_PRIORITY_PROPERTY);
      if (task.priority > 0) {
        icalcomponent_add_property(todo, icalproperty_new_priority(task.priority));
      }
    }
  }
  stamp(todo);
  const QByteArray result = render(calendar.get());
  if (result.isEmpty()) {
    setError(errorMessage,
             QStringLiteral("The task could not be written as iCalendar"));
  }
  return result;
}

}  // namespace omacalendar::caldav
