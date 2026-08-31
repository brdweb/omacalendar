#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <functional>
#include <utility>

#include "core/domain.h"

namespace omacalendar {

// Zero is a sentinel only. Implementations return a unique, non-zero value for
// every call, including requests that later fail asynchronous validation.
using ProviderOperationId = quint64;
inline constexpr ProviderOperationId kInvalidProviderOperationId = 0;

enum class ProviderErrorKind {
  None,
  Cancelled,
  Network,
  Timeout,
  Authentication,
  Permission,
  Conflict,
  RateLimited,
  NotFound,
  InvalidRequest,
  SyncTokenExpired,
  Unsupported,
  ServiceUnavailable,
  Unknown,
};

// Provider errors are safe to persist and display. Adapters must not put
// access tokens, passwords, authorization headers, or raw credential-bearing
// responses in any field (including details).
struct ProviderError {
  ProviderErrorKind kind = ProviderErrorKind::None;
  QString code;
  QString message;
  int httpStatus = 0;
  int networkError = 0;

  // A non-negative value represents a provider Retry-After hint. A negative
  // value means that the provider did not supply one.
  qint64 retryAfterMs = -1;
  QJsonObject details;

  [[nodiscard]] bool isError() const noexcept {
    return kind != ProviderErrorKind::None;
  }
};

struct ProviderCapabilities {
  bool accountDiscovery = true;
  bool calendarDiscovery = true;
  bool incrementalSync = false;
  bool fetchEvent = true;
  bool createEvent = true;
  bool updateEvent = true;
  bool removeEvent = true;
  bool recurringEvents = true;
  bool recurrenceExceptions = true;
  bool attendees = false;
  bool reminders = false;
  bool conferenceData = false;
  bool serverScheduling = false;
  bool pushNotifications = false;
  QJsonObject extensions;
};

template <typename T>
struct ProviderResult {
  T value{};
  ProviderError error;

  [[nodiscard]] bool succeeded() const noexcept { return !error.isError(); }

  static ProviderResult success(T value) {
    ProviderResult result;
    result.value = std::move(value);
    return result;
  }

  static ProviderResult failure(ProviderError error) {
    ProviderResult result;
    result.error = std::move(error);
    return result;
  }
};

struct AccountDiscoveryRequest {
  // The seed contains the local account id, provider kind, configured endpoint,
  // and any user-entered principal. The adapter returns a normalized Account.
  Account seed;
  QJsonObject options;
};

struct AccountDiscoveryPayload {
  Account account;
  QJsonObject serverProperties;
};

struct CalendarDiscoveryRequest {
  Account account;
  QString pageToken;
  int pageSizeHint = 0;
};

struct CalendarDiscoveryPayload {
  QList<Calendar> calendars;
  QString nextPageToken;
  bool hasMore = false;
};

enum class RemoteChangeKind {
  Upsert,
  Remove,
};

struct RemoteChange {
  RemoteChangeKind kind = RemoteChangeKind::Upsert;
  QString remoteId;
  QString revision;

  // event is populated for Upsert changes. For Remove changes, remoteId and
  // revision are authoritative and event may be empty.
  Event event;
};

struct PullChangesRequest {
  Account account;
  Calendar calendar;

  // An empty sync token starts a full sync. pageToken continues the same pull
  // and must not be persisted as a durable sync token.
  QString syncToken;
  QString pageToken;

  // Optional UTC bounds for an initial/full sync. Invalid values mean that the
  // adapter should use its documented defaults.
  QDateTime windowStartUtc;
  QDateTime windowEndUtc;
  int pageSizeHint = 0;
};

struct PullChangesPayload {
  QList<RemoteChange> changes;
  QString nextPageToken;
  bool hasMore = false;

  // A durable token is only committed after the last page has been applied.
  QString nextSyncToken;
};

struct RangeSyncRequest {
  QString calendarId;
  QDateTime startUtc;
  QDateTime endUtc;
};

struct FetchEventRequest {
  Account account;
  Calendar calendar;
  QString remoteId;
};

struct CreateEventRequest {
  Account account;
  Calendar calendar;
  Event event;
  QString idempotencyKey;
};

struct UpdateEventRequest {
  Account account;
  Calendar calendar;
  Event event;
  QString expectedRevision;
  QString idempotencyKey;
};

struct RemoveEventRequest {
  Account account;
  Calendar calendar;
  QString remoteId;
  QString expectedRevision;
  QString idempotencyKey;
};

struct MutationPayload {
  Event event;
  QString remoteId;
  QString revision;
};

struct RemoveEventPayload {
  QString remoteId;
  QString revision;
};

using AccountDiscoveryCallback =
    std::function<void(ProviderResult<AccountDiscoveryPayload>)>;
using CalendarDiscoveryCallback =
    std::function<void(ProviderResult<CalendarDiscoveryPayload>)>;
using PullChangesCallback = std::function<void(ProviderResult<PullChangesPayload>)>;
using FetchEventCallback = std::function<void(ProviderResult<Event>)>;
using MutationCallback = std::function<void(ProviderResult<MutationPayload>)>;
using RemoveEventCallback = std::function<void(ProviderResult<RemoveEventPayload>)>;

// Daemon-facing contract shared by every calendar source. Remote protocol
// implementations retain the operation DTOs above internally, while the
// SyncCoordinator depends on this lifecycle/synchronization surface. This
// keeps provider selection and status routing out of IPC handlers and gives
// device-only and read-only subscription calendars the same observable
// behavior as writable network providers.
class Provider : public QObject {
  Q_OBJECT

 public:
  Provider(QString providerId, ProviderKind providerKind, QObject* parent = nullptr)
      : QObject(parent),
        m_providerId(std::move(providerId)),
        m_providerKind(providerKind) {}
  ~Provider() override = default;

  Provider(const Provider&) = delete;
  Provider& operator=(const Provider&) = delete;

  [[nodiscard]] const QString& id() const noexcept { return m_providerId; }
  [[nodiscard]] ProviderKind kind() const noexcept { return m_providerKind; }
  [[nodiscard]] virtual ProviderCapabilities capabilities() const = 0;

  virtual void start() {}
  virtual void syncAll() = 0;
  virtual void syncAccount(const QString& accountId) = 0;
  // Queue a bounded historical/future hydration. Implementations must return
  // without waiting for network I/O and mark durable coverage only after the
  // complete provider response has been committed.
  virtual bool syncRange(const RangeSyncRequest& request,
                         QString* errorMessage = nullptr) {
    Q_UNUSED(request)
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("This provider does not support range sync");
    }
    return false;
  }
  [[nodiscard]] virtual QJsonObject status(const QString& accountId = {}) const = 0;

 signals:
  void accountChanged(const QString& accountId);
  void calendarsChanged(const QString& accountId);
  void eventsChanged(const QStringList& calendarIds);
  void syncStatusChanged(const QString& accountId, const QJsonObject& status);
  void operationStateChanged();

 private:
  QString m_providerId;
  ProviderKind m_providerKind = ProviderKind::Unknown;
};

}  // namespace omacalendar
