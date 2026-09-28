#include "presentationlistmodel.h"

#include <QByteArrayView>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <utility>

namespace omacalendar {
namespace {

constexpr int kModelDataRole = Qt::UserRole + 1;
constexpr int kFirstFieldRole = Qt::UserRole + 2;

// Append new fields to this table. Reordering or removing an entry would change
// an existing role number and is therefore an IPC-to-presentation compatibility
// break. The table covers all current account, calendar, event, calendar-set,
// invitation, conflict, operation, and search-result DTO fields.
constexpr QByteArrayView kFieldNames[] = {"id",
                                          "accountId",
                                          "calendarId",
                                          "eventId",
                                          "occurrenceId",
                                          "mutationId",
                                          "clientMutationId",
                                          "dependencyId",
                                          "provider",
                                          "displayName",
                                          "principal",
                                          "enabled",
                                          "authStatus",
                                          "name",
                                          "description",
                                          "color",
                                          "timeZone",
                                          "readOnly",
                                          "colorOverride",
                                          "position",
                                          "ignoreAlerts",
                                          "capabilities",
                                          "lastSyncAt",
                                          "summary",
                                          "location",
                                          "url",
                                          "conferenceUrl",
                                          "startUtc",
                                          "endUtc",
                                          "displayStart",
                                          "displayEnd",
                                          "startDate",
                                          "endDate",
                                          "startTimeZone",
                                          "endTimeZone",
                                          "allDay",
                                          "timeKind",
                                          "status",
                                          "transparency",
                                          "visibility",
                                          "recurrenceRule",
                                          "recurrenceId",
                                          "sequence",
                                          "organizer",
                                          "attendees",
                                          "reminders",
                                          "dirty",
                                          "deleted",
                                          "localRevision",
                                          "syncState",
                                          "isDefault",
                                          "defaultCalendarId",
                                          "calendarIds",
                                          "operation",
                                          "state",
                                          "recurrenceScope",
                                          "sendUpdates",
                                          "attempts",
                                          "nextAttemptAt",
                                          "notBefore",
                                          "leaseUntil",
                                          "errorCode",
                                          "errorMessage",
                                          "kind",
                                          "localSnapshot",
                                          "remoteSnapshot",
                                          "resolutionRevision",
                                          "resolvedAt",
                                          "createdAt",
                                          "updatedAt",
                                          "eventSummary",
                                          "message",
                                          "invitationState",
                                          "responseStatus",
                                          "meetingLink",
                                          "title",
                                          "notes",
                                          "displayStartLocal",
                                          "displayEndLocal",
                                          "eventStartLocal",
                                          "eventEndLocal"};

QByteArray fieldNameForRole(const int role) {
  const int offset = role - kFirstFieldRole;
  if (offset < 0 || offset >= static_cast<int>(std::size(kFieldNames))) {
    return {};
  }
  return kFieldNames[offset].toByteArray();
}

}  // namespace

PresentationListModel::PresentationListModel(QObject* parent)
    : QAbstractListModel(parent) {}

int PresentationListModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant PresentationListModel::data(const QModelIndex& index, const int role) const {
  if (!index.isValid() || index.parent().isValid() || index.column() != 0 ||
      index.row() < 0 || index.row() >= m_rows.size()) {
    return {};
  }

  const QVariantMap& row = m_rows.at(index.row());
  if (role == kModelDataRole || role == Qt::DisplayRole) {
    return row;
  }
  const QByteArray field = fieldNameForRole(role);
  return field.isEmpty() ? QVariant() : row.value(QString::fromUtf8(field));
}

QHash<int, QByteArray> PresentationListModel::roleNames() const {
  QHash<int, QByteArray> roles;
  roles.reserve(static_cast<qsizetype>(std::size(kFieldNames)) + 1);
  roles.insert(kModelDataRole, QByteArrayLiteral("modelData"));
  for (std::size_t index = 0; index < std::size(kFieldNames); ++index) {
    roles.insert(kFirstFieldRole + static_cast<int>(index),
                 kFieldNames[index].toByteArray());
  }
  return roles;
}

QVariantMap PresentationListModel::get(const int row) const {
  if (row < 0 || row >= m_rows.size()) {
    return {};
  }
  return m_rows.at(row);
}

QVariantList PresentationListModel::toList() const {
  QVariantList result;
  result.reserve(m_rows.size());
  for (const QVariantMap& row : m_rows) {
    result.append(row);
  }
  return result;
}

QString PresentationListModel::rowKey(const QVariantMap& row) {
  const QString id = row.value(QStringLiteral("id")).toString();
  if (id.isEmpty()) {
    return {};
  }
  // Occurrences of one series share the series id; the recurrence id tells
  // them apart.
  return id + QLatin1Char('\n') + row.value(QStringLiteral("recurrenceId")).toString();
}

void PresentationListModel::replace(const QVariantList& rows) {
  QList<QVariantMap> nextRows;
  nextRows.reserve(rows.size());
  for (const QVariant& row : rows) {
    if (row.canConvert<QVariantMap>()) {
      nextRows.append(row.toMap());
    }
  }
  if (nextRows == m_rows) {
    return;
  }

  const qsizetype previousCount = m_rows.size();
  if (!applyIncrementally(nextRows)) {
    resetTo(std::move(nextRows));
  }
  if (previousCount != m_rows.size()) {
    emit countChanged();
  }
}

bool PresentationListModel::applyIncrementally(QList<QVariantMap>& nextRows) {
  QStringList nextKeys;
  nextKeys.reserve(nextRows.size());
  QSet<QString> nextKeySet;
  nextKeySet.reserve(nextRows.size());
  for (const QVariantMap& row : nextRows) {
    const QString key = rowKey(row);
    if (key.isEmpty() || nextKeySet.contains(key)) {
      return false;
    }
    nextKeys.append(key);
    nextKeySet.insert(key);
  }
  QStringList keys;
  keys.reserve(m_rows.size());
  QSet<QString> keySet;
  keySet.reserve(m_rows.size());
  for (const QVariantMap& row : m_rows) {
    const QString key = rowKey(row);
    if (key.isEmpty() || keySet.contains(key)) {
      return false;
    }
    keys.append(key);
    keySet.insert(key);
  }

  // Rows that stay keep their relative order unless something moved. When
  // most of them moved, individual move signals cost more than a reset.
  qsizetype retained = 0;
  qsizetype outOfOrder = 0;
  qsizetype lastNextIndex = -1;
  QHash<QString, qsizetype> nextIndex;
  nextIndex.reserve(nextKeys.size());
  for (qsizetype index = 0; index < nextKeys.size(); ++index) {
    nextIndex.insert(nextKeys.at(index), index);
  }
  for (const QString& key : std::as_const(keys)) {
    const auto found = nextIndex.constFind(key);
    if (found == nextIndex.constEnd()) {
      continue;
    }
    ++retained;
    if (found.value() < lastNextIndex) {
      ++outOfOrder;
    } else {
      lastNextIndex = found.value();
    }
  }
  if (retained > 0 && outOfOrder * 2 > retained) {
    return false;
  }

  // Remove rows that are gone, from the end so earlier indexes stay valid.
  for (qsizetype index = keys.size() - 1; index >= 0; --index) {
    if (!nextKeySet.contains(keys.at(index))) {
      const int row = static_cast<int>(index);
      beginRemoveRows({}, row, row);
      m_rows.removeAt(index);
      keys.removeAt(index);
      endRemoveRows();
    }
  }

  // Walk the target order, moving, inserting and updating rows in place.
  for (qsizetype target = 0; target < nextKeys.size(); ++target) {
    const QString& key = nextKeys.at(target);
    if (target >= keys.size() || keys.at(target) != key) {
      qsizetype source = -1;
      if (keySet.contains(key)) {
        source = keys.indexOf(key, target + 1);
      }
      if (source >= 0) {
        const int from = static_cast<int>(source);
        const int to = static_cast<int>(target);
        beginMoveRows({}, from, from, {}, to);
        m_rows.move(source, target);
        keys.move(source, target);
        endMoveRows();
      } else {
        const int row = static_cast<int>(target);
        beginInsertRows({}, row, row);
        m_rows.insert(target, std::move(nextRows[target]));
        keys.insert(target, key);
        endInsertRows();
        continue;
      }
    }
    if (m_rows.at(target) != nextRows.at(target)) {
      m_rows[target] = std::move(nextRows[target]);
      const QModelIndex changed = index(static_cast<int>(target));
      emit dataChanged(changed, changed);
    }
  }
  return true;
}

void PresentationListModel::resetTo(QList<QVariantMap>&& nextRows) {
  beginResetModel();
  m_rows = std::move(nextRows);
  endResetModel();
}

void PresentationListModel::clear() { replace({}); }

}  // namespace omacalendar
