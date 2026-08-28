#pragma once

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace omacalendar {

inline constexpr int kIpcProtocolMajor = 1;
inline constexpr int kIpcProtocolMinor = 0;

enum class ProviderKind {
  CalDav,
  Google,
  Unknown,
};

enum class OutboxOperation {
  Create,
  Update,
  Remove,
};

enum class OutboxState {
  Pending,
  Sending,
  RetryWait,
  Blocked,
  Done,
};

struct Account {
  QString id;
  ProviderKind provider = ProviderKind::Unknown;
  QString displayName;
  QString principal;
  QString endpoint;
  bool enabled = true;
  QString authStatus = QStringLiteral("not_configured");
  QDateTime createdAt;
  QDateTime updatedAt;
};

struct Calendar {
  QString id;
  QString accountId;
  QString remoteId;
  QString href;
  QString name;
  QString description;
  QString color = QStringLiteral("#7aa2f7");
  QString timeZone;
  bool readOnly = false;
  bool enabled = true;
  QString etag;
  QString syncToken;
  QJsonObject capabilities;
  QDateTime lastSyncAt;
};

struct Event {
  QString id;
  QString calendarId;
  QString remoteId;
  QString uid;
  QString etag;
  QString summary;
  QString description;
  QString location;
  QDateTime startUtc;
  QDateTime endUtc;
  QDate startDate;
  QDate endDate;
  QString startTimeZone;
  QString endTimeZone;
  bool allDay = false;
  QString status = QStringLiteral("confirmed");
  QString transparency = QStringLiteral("opaque");
  QString recurrenceRule;
  QString recurrenceId;
  int sequence = 0;
  QJsonObject organizer;
  QJsonArray attendees;
  QJsonArray reminders;
  QString rawPayload;
  QString rawFormat;
  bool dirty = false;
  bool deleted = false;
  QDateTime createdAt;
  QDateTime updatedAt;
};

struct OutboxItem {
  qint64 id = 0;
  QString accountId;
  QString calendarId;
  QString eventId;
  OutboxOperation operation = OutboxOperation::Create;
  OutboxState state = OutboxState::Pending;
  QString idempotencyKey;
  QString expectedRevision;
  QJsonObject payload;
  int attempts = 0;
  QDateTime nextAttemptAt;
  QString errorCode;
  QString errorMessage;
  QDateTime createdAt;
  QDateTime updatedAt;
};

QString providerKindToString(ProviderKind kind);
ProviderKind providerKindFromString(const QString& value);
QString outboxOperationToString(OutboxOperation operation);
OutboxOperation outboxOperationFromString(const QString& value);
QString outboxStateToString(OutboxState state);
OutboxState outboxStateFromString(const QString& value);

QJsonObject toJson(const Account& account);
QJsonObject toJson(const Calendar& calendar);
QJsonObject toJson(const Event& event);
QJsonObject toJson(const OutboxItem& item);

Account accountFromJson(const QJsonObject& object);
Calendar calendarFromJson(const QJsonObject& object);
Event eventFromJson(const QJsonObject& object);

QString newUuid();
QString isoUtc(const QDateTime& value);
QDateTime dateTimeFromIso(const QString& value);

}  // namespace omacalendar

Q_DECLARE_METATYPE(omacalendar::Account)
Q_DECLARE_METATYPE(omacalendar::Calendar)
Q_DECLARE_METATYPE(omacalendar::Event)
Q_DECLARE_METATYPE(omacalendar::OutboxItem)
