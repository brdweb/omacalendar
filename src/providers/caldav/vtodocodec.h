#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

#include "core/tasks.h"

namespace omacalendar::caldav {

// Converts between Task and RFC 5545 VTODO resources.
class VTodoCodec final {
 public:
  VTodoCodec() = delete;

  // Reads the first non-exception VTODO of a calendar resource: UID, SUMMARY,
  // DESCRIPTION, DUE, completion (STATUS, COMPLETED, PERCENT-COMPLETE),
  // PRIORITY and a parent RELATED-TO. Returns nothing, with errorMessage set,
  // when there is no usable VTODO.
  [[nodiscard]] static std::optional<Task> parse(const QByteArray& payload,
                                                 QString* errorMessage = nullptr);

  // Produces the resource to upload. With a retained provider payload only
  // the properties whose value changed are replaced, so categories, alarms,
  // recurrence and X- properties survive; without one a new VCALENDAR is
  // built. Returns an empty array, with errorMessage set, on failure.
  [[nodiscard]] static QByteArray serialize(const Task& task,
                                            const QByteArray& retainedPayload = {},
                                            QString* errorMessage = nullptr);
};

}  // namespace omacalendar::caldav
