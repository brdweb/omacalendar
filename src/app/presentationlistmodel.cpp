#include "presentationlistmodel.h"

#include <QByteArrayView>

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
  beginResetModel();
  m_rows = std::move(nextRows);
  endResetModel();
  if (previousCount != m_rows.size()) {
    emit countChanged();
  }
}

void PresentationListModel::clear() { replace({}); }

}  // namespace omacalendar
