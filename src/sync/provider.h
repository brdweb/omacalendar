#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
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

// Network-backed provider contract shared by the Google and CalDAV adapters.
//
// Every accepted operation is asynchronous: it returns before invoking its
// callback, never runs a nested event loop, and invokes the callback exactly
// once in this object's thread. Cancellation also completes the callback once,
// with ProviderErrorKind::Cancelled. A callback is never invoked after this
// provider has been destroyed.
class Provider : public QObject {
  Q_OBJECT

 public:
  explicit Provider(QString providerId, QObject* parent = nullptr)
      : QObject(parent), m_providerId(std::move(providerId)) {}
  ~Provider() override = default;

  Provider(const Provider&) = delete;
  Provider& operator=(const Provider&) = delete;

  [[nodiscard]] const QString& id() const noexcept { return m_providerId; }
  [[nodiscard]] virtual ProviderKind kind() const noexcept = 0;
  [[nodiscard]] virtual ProviderCapabilities capabilities() const = 0;

  [[nodiscard]] virtual ProviderOperationId discoverAccount(
      const AccountDiscoveryRequest& request, AccountDiscoveryCallback callback) = 0;
  [[nodiscard]] virtual ProviderOperationId discoverCalendars(
      const CalendarDiscoveryRequest& request, CalendarDiscoveryCallback callback) = 0;
  [[nodiscard]] virtual ProviderOperationId pullChanges(
      const PullChangesRequest& request, PullChangesCallback callback) = 0;
  [[nodiscard]] virtual ProviderOperationId fetchEvent(const FetchEventRequest& request,
                                                       FetchEventCallback callback) = 0;
  [[nodiscard]] virtual ProviderOperationId createEvent(
      const CreateEventRequest& request, MutationCallback callback) = 0;
  [[nodiscard]] virtual ProviderOperationId updateEvent(
      const UpdateEventRequest& request, MutationCallback callback) = 0;
  [[nodiscard]] virtual ProviderOperationId removeEvent(
      const RemoveEventRequest& request, RemoveEventCallback callback) = 0;

  // cancel() is idempotent. Unknown and already-completed ids are ignored.
  virtual void cancel(ProviderOperationId operationId) = 0;
  virtual void cancelAll() = 0;

 private:
  QString m_providerId;
};

}  // namespace omacalendar
