#pragma once

// Private helpers shared by the database*.cpp translation units. Not part of
// the public Database interface.

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QTimeZone>
#include <QVariant>

#include "core/domain.h"
#include "core/recurrenceexpander.h"

namespace omacalendar::detail {

inline QString compactJson(const QJsonValue& value) {
  if (value.isArray()) {
    return QString::fromUtf8(
        QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
  }
  return QString::fromUtf8(
      QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
}

inline QJsonObject parseObject(const QString& value) {
  if (value.isEmpty()) {
    return {};
  }
  return QJsonDocument::fromJson(value.toUtf8()).object();
}

inline QJsonArray parseArray(const QString& value) {
  if (value.isEmpty()) {
    return {};
  }
  return QJsonDocument::fromJson(value.toUtf8()).array();
}

inline QString serializeValue(const QJsonValue& value) {
  QJsonArray wrapper;
  wrapper.append(value);
  QByteArray serialized = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
  return QString::fromUtf8(serialized.mid(1, serialized.size() - 2));
}

inline QJsonValue parseValue(const QString& value, const QJsonValue& fallback,
                             bool* ok = nullptr) {
  QJsonParseError parseError;
  const QJsonDocument wrapper = QJsonDocument::fromJson(
      QByteArrayLiteral("[") + value.toUtf8() + ']', &parseError);
  const bool valid =
      parseError.error == QJsonParseError::NoError && !wrapper.array().isEmpty();
  if (ok != nullptr) {
    *ok = valid;
  }
  return valid ? wrapper.array().first() : fallback;
}

inline QString sqlError(const QSqlQuery& query, const QString& context) {
  return QStringLiteral("%1: %2").arg(context, query.lastError().text());
}

inline QDateTime nowUtc() { return QDateTime::currentDateTimeUtc(); }

inline QString nonNull(const QString& value) {
  return value.isNull() ? QStringLiteral("") : value;
}

struct RecurrenceStoredBounds {
  QVariant untilUtc;
  QVariant untilDate;
  QVariant originalUtc;
  QVariant originalDate;
};

inline RecurrenceStoredBounds recurrenceBounds(const Event& event) {
  RecurrenceStoredBounds bounds;
  if (!event.recurrenceId.isEmpty()) {
    const QString canonical = canonicalRecurrenceIdentity(
        event.recurrenceId, event.allDay, event.timeKind, event.startTimeZone);
    if (canonical.startsWith(QStringLiteral("D:"))) {
      const QDate date = QDate::fromString(canonical.sliced(2), Qt::ISODate);
      if (date.isValid()) {
        bounds.originalDate = date.toString(Qt::ISODate);
      }
    } else if (canonical.startsWith(QStringLiteral("Z:"))) {
      const QDateTime instant = dateTimeFromIso(canonical.sliced(2));
      if (instant.isValid()) {
        bounds.originalUtc = isoUtc(instant);
      }
    } else if (canonical.startsWith(QStringLiteral("F:")) &&
               !canonical.startsWith(QStringLiteral("F:offset:"))) {
      const QDateTime wall =
          QDateTime::fromString(canonical.sliced(2), Qt::ISODateWithMs);
      if (wall.isValid()) {
        const QDateTime local(wall.date(), wall.time(), QTimeZone::LocalTime);
        bounds.originalUtc = isoUtc(local);
      }
    }
    return bounds;
  }
  if (event.recurrenceRule.isEmpty()) {
    return bounds;
  }
  const auto end = RecurrenceExpander::finiteEnd(event);
  if (end.has_value()) {
    if (event.allDay || event.timeKind == TimeKind::Floating) {
      if (end->date.isValid()) {
        bounds.untilDate = end->date.toString(Qt::ISODate);
      }
    } else if (end->utc.isValid()) {
      bounds.untilUtc = isoUtc(end->utc);
    }
  }
  return bounds;
}

}  // namespace omacalendar::detail
