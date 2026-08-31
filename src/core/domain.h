#pragma once

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace omacalendar {

inline constexpr int kIpcProtocolMajor = 2;
inline constexpr int kIpcProtocolMinor = 0;

enum class ProviderKind {
  Local,
  CalDav,
  Google,
  Ics,
  Unknown,
};

enum class TimeKind {
  Zoned,
  Floating,
  AllDay,
};

enum class OutboxOperation {
  Create,
  Update,
  Move,
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
  QString colorOverride;
  int position = 0;
  bool ignoreAlerts = false;
  QString etag;
  QString syncToken;
  QJsonObject capabilities;
  QDateTime lastSyncAt;
};

[[nodiscard]] bool canDeleteCalendar(const Calendar& calendar);

struct Event {
  QString id;
  QString calendarId;
  QString remoteId;
  QString uid;
  QString etag;
  QString summary;
  QString description;
  QString location;
  QString url;
  QString conferenceUrl;
  QDateTime startUtc;
  QDateTime endUtc;
  QDate startDate;
  QDate endDate;
  QString startTimeZone;
  QString endTimeZone;
  bool allDay = false;
  TimeKind timeKind = TimeKind::Zoned;
  QString status = QStringLiteral("confirmed");
  QString transparency = QStringLiteral("opaque");
  QString visibility = QStringLiteral("default");
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
  qint64 localRevision = 0;
  QString syncState = QStringLiteral("clean");
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
  QString dependencyId;
  QString expectedRevision;
  QString recurrenceScope = QStringLiteral("series");
  QString sendUpdates = QStringLiteral("none");
  QJsonObject payload;
  int attempts = 0;
  QDateTime nextAttemptAt;
  QDateTime notBefore;
  QDateTime leaseUntil;
  QString errorCode;
  QString errorMessage;
  QDateTime createdAt;
  QDateTime updatedAt;
};

struct CalendarSet {
  QString id;
  QString name;
  bool isDefault = false;
  QString defaultCalendarId;
  QStringList calendarIds;
  QDateTime createdAt;
  QDateTime updatedAt;
};

struct Conflict {
  qint64 id = 0;
  QString eventId;
  qint64 mutationId = 0;
  QString kind = QStringLiteral("remote_changed");
  QString localRevision;
  QString remoteRevision;
  QJsonObject localSnapshot;
  QJsonObject remoteSnapshot;
  QString state = QStringLiteral("unresolved");
  qint64 resolutionRevision = 0;
  QDateTime createdAt;
  QDateTime resolvedAt;
};

struct ReminderJob {
  qint64 id = 0;
  QString eventId;
  QString occurrenceId;
  QString fingerprint;
  QDateTime fireAt;
  QDateTime snoozedUntil;
  QString state = QStringLiteral("pending");
  qint64 eventRevision = 0;
  QDateTime claimedAt;
  QDateTime leaseExpiresAt;
  QDateTime deliveredAt;
};

QString providerKindToString(ProviderKind kind);
ProviderKind providerKindFromString(const QString& value);
QString timeKindToString(TimeKind kind);
TimeKind timeKindFromString(const QString& value);
QString outboxOperationToString(OutboxOperation operation);
OutboxOperation outboxOperationFromString(const QString& value);
QString outboxStateToString(OutboxState state);
OutboxState outboxStateFromString(const QString& value);

QJsonObject toJson(const Account& account);
QJsonObject toJson(const Calendar& calendar);
QJsonObject toJson(const Event& event);
QJsonObject toJson(const OutboxItem& item);
QJsonObject toJson(const CalendarSet& set);
QJsonObject toJson(const Conflict& conflict);
QJsonObject toJson(const ReminderJob& reminder);

// Storage payloads include provider-owned metadata and are never returned over IPC.
QJsonObject toStorageJson(const Event& event);

Account accountFromJson(const QJsonObject& object);
Calendar calendarFromJson(const QJsonObject& object);
Event eventFromJson(const QJsonObject& object);

QString newUuid();
QString isoUtc(const QDateTime& value);
QDateTime dateTimeFromIso(const QString& value);

// RFC 5545 recurrence IDs are commonly represented in either iCalendar basic
// form (20260904T090000), ISO form, or with TZID/RANGE parameters.  Providers
// are allowed to choose any of those equivalent spellings.  These helpers
// produce a provider-neutral identity while retaining all-day and floating
// time semantics.  For zoned events an explicit TZID wins; otherwise the
// supplied event timezone is used for local-wall-time IDs.
QString canonicalRecurrenceIdentity(const QString& value, bool allDay,
                                    TimeKind timeKind = TimeKind::Zoned,
                                    const QString& timeZone = {});
bool recurrenceIdentityEqual(const QString& left, const QString& right, bool allDay,
                             TimeKind timeKind = TimeKind::Zoned,
                             const QString& timeZone = {});
bool recurrenceIdentityEqual(const Event& left, const Event& right);

}  // namespace omacalendar

Q_DECLARE_METATYPE(omacalendar::Account)
Q_DECLARE_METATYPE(omacalendar::Calendar)
Q_DECLARE_METATYPE(omacalendar::Event)
Q_DECLARE_METATYPE(omacalendar::OutboxItem)
