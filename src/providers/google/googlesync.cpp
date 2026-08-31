#include "providers/google/googlesync.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTimeZone>
#include <QUrl>
#include <algorithm>
#include <limits>
#include <utility>

#include "core/domain.h"
#include "providers/google/googlemapper.h"
#include "providers/google/googleoauthconfig.h"

namespace omacalendar::google {
namespace {

constexpr auto kClientIdSetting = "google.oauth.clientId";
constexpr auto kOAuthScopeVersionSetting = "google.oauth.scopeVersion";
constexpr int kOAuthScopeVersion = 2;
constexpr auto kCalendarListToken = "calendarList.syncToken";

quint64 stableJitterKey(const QString& value) {
  const QByteArray digest =
      QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256);
  quint64 result = 0;
  const qsizetype count = qMin<qsizetype>(8, digest.size());
  for (qsizetype index = 0; index < count; ++index) {
    result = (result << 8U) | static_cast<quint8>(digest.at(index));
  }
  return result;
}

QJsonObject rawGoogleEvent(const Event& event) {
  if (event.rawFormat != QStringLiteral("google-json") || event.rawPayload.isEmpty()) {
    return {};
  }
  const QJsonDocument document = QJsonDocument::fromJson(event.rawPayload.toUtf8());
  return document.isObject() ? document.object() : QJsonObject{};
}

QJsonArray calendarDefaultReminders(const Calendar& calendar) {
  return calendar.capabilities.value(QStringLiteral("defaultReminders")).toArray();
}

QString recurringParentRemoteId(const Event& event) {
  return rawGoogleEvent(event).value(QStringLiteral("recurringEventId")).toString();
}

ProviderError providerErrorForResponse(const ApiResponse& response) {
  ProviderError error;
  error.code = response.errorCode;
  error.message = response.errorMessage;
  error.httpStatus = response.httpStatus;
  error.networkError = response.networkError;
  error.retryAfterMs = response.retryAfterMs;
  if (response.authenticationRequired) {
    error.kind = ProviderErrorKind::Authentication;
  } else if (response.httpStatus == 409 || response.httpStatus == 412) {
    error.kind = ProviderErrorKind::Conflict;
  } else if (response.httpStatus == 403 && !response.retryable) {
    error.kind = ProviderErrorKind::Permission;
  } else if (response.httpStatus == 404 || response.httpStatus == 410) {
    error.kind = ProviderErrorKind::NotFound;
  } else if (response.httpStatus == 408) {
    error.kind = ProviderErrorKind::Timeout;
  } else if (response.httpStatus == 425 || response.httpStatus == 429 ||
             (response.httpStatus == 403 && response.retryable)) {
    error.kind = ProviderErrorKind::RateLimited;
  } else if (response.httpStatus >= 500) {
    error.kind = ProviderErrorKind::ServiceUnavailable;
  } else if (response.httpStatus == 0 && response.networkError != 0) {
    error.kind = ProviderErrorKind::Network;
  } else if (response.retryable) {
    error.kind = ProviderErrorKind::ServiceUnavailable;
  } else {
    error.kind = ProviderErrorKind::InvalidRequest;
  }
  return error;
}

QPair<QDateTime, QDateTime> occurrenceLookupBounds(const Event& event) {
  const QDate date = QDate::fromString(event.recurrenceId, Qt::ISODate);
  if (date.isValid() && event.recurrenceId.size() == 10) {
    const QDateTime start(date.addDays(-1), QTime(0, 0), QTimeZone::UTC);
    return {start, start.addDays(3)};
  }
  QDateTime instant = QDateTime::fromString(event.recurrenceId, Qt::ISODateWithMs);
  if (!instant.isValid()) {
    instant = QDateTime::fromString(event.recurrenceId, Qt::ISODate);
  }
  if (!instant.isValid()) {
    return {};
  }
  instant = instant.toUTC();
  return {instant.addDays(-1), instant.addDays(1)};
}

QJsonObject moveMetadata(const OutboxItem& item) {
  return item.payload.value(QStringLiteral("_move")).toObject();
}

Event moveSourceEvent(const OutboxItem& item) {
  return eventFromJson(
      moveMetadata(item).value(QStringLiteral("sourceEvent")).toObject());
}

QJsonObject statusObject(const QString& state, const QString& errorCode = {},
                         const QString& message = {}, const QDateTime& lastSync = {}) {
  return {
      {QStringLiteral("state"), state},
      {QStringLiteral("errorCode"), errorCode},
      {QStringLiteral("message"), message},
      {QStringLiteral("lastSyncAt"), isoUtc(lastSync)},
  };
}

QPair<QDateTime, QDateTime> initialCoverageWindow(const QDateTime& nowUtc) {
  const QDate today = nowUtc.toUTC().date();
  // Keep first authorization lightweight. Views hydrate uncovered ranges on
  // demand and the low-priority backfill timer expands older history later.
  return {QDateTime(today.addMonths(-3), QTime(0, 0), QTimeZone::UTC),
          QDateTime(today.addMonths(15), QTime(0, 0), QTimeZone::UTC)};
}

}  // namespace

QStringList googleFullSyncPruneCandidates(const QList<Event>& cachedEvents,
                                          const QSet<QString>& retainedRemoteIds) {
  QSet<QString> pruned;
  for (const Event& cached : cachedEvents) {
    if (!cached.remoteId.isEmpty() && !retainedRemoteIds.contains(cached.remoteId)) {
      pruned.insert(cached.remoteId);
    }
  }
  QStringList result(pruned.cbegin(), pruned.cend());
  result.sort();
  return result;
}

struct GoogleSync::SyncJob final {
  QString accountId;
  quint64 generation = 0;
  Account account;
  QString calendarListSyncToken;
  bool calendarListResetAttempted = false;
  bool calendarListFullSync = false;
  QStringList seenCalendarRemoteIds;
  QList<Calendar> calendars;
  int calendarIndex = 0;
  Calendar currentCalendar;
  bool eventResetAttempted = false;
  bool replaceEventCoverage = false;
  bool eventFullSync = false;
  bool hydrationSync = false;
  RangeSyncRequest hydrationRequest;
  QSet<QString> seenRemoteIds;
  QList<Event> stagedEvents;
  QStringList stagedDeletedRemoteIds;
  QDateTime eventWindowStartUtc;
  QDateTime eventWindowEndUtc;
  QList<OutboxItem> outbox;
  int outboxIndex = 0;
  QStringList changedCalendarIds;
  bool tokenRefreshAttempted = false;
  bool waitingForTokenRefresh = false;
  std::function<void(SyncJob*)> resumeAfterTokenRefresh;
  int readRetryAttempts = 0;
};

struct GoogleSync::PendingDisconnect final {
  Account snapshot;
  bool removeCachedData = false;
};

GoogleSync::GoogleSync(Database* database, QObject* parent)
    : Provider(QStringLiteral("google"), ProviderKind::Google, parent),
      m_database(database),
      m_auth(this),
      m_client(&m_auth, this) {
  m_pollTimer.setInterval(5 * 60 * 1000);
  connect(&m_pollTimer, &QTimer::timeout, this, &GoogleSync::syncAll);
  m_pollTimer.start();

  connect(&m_auth, &GoogleAuthManager::authorizationUrlReady, this,
          &GoogleSync::authorizationUrlReady);
  connect(&m_auth, &GoogleAuthManager::authorized, this,
          [this](const QString& accountId) {
            m_provisionalAuthorizationIds.remove(accountId);
            m_reauthorizationSnapshots.remove(accountId);
            setAccountAuthStatus(accountId, QStringLiteral("connected"));
            emit accountChanged(accountId);
            syncAccount(accountId);
          });
  connect(&m_auth, &GoogleAuthManager::tokenChanged, this,
          [this](const QString& accountId) {
            SyncJob* job = m_jobs.value(accountId, nullptr);
            if (job != nullptr && job->waitingForTokenRefresh) {
              const quint64 generation = job->generation;
              auto resume = std::move(job->resumeAfterTokenRefresh);
              job->waitingForTokenRefresh = false;
              job->resumeAfterTokenRefresh = {};
              setAccountAuthStatus(accountId, QStringLiteral("connected"));
              job = activeJob(accountId, generation);
              if (job != nullptr && resume) {
                resume(job);
              }
              return;
            }
            if (job == nullptr) {
              syncAccount(accountId);
            }
          });
  connect(&m_auth, &GoogleAuthManager::clientConfigurationFinished, this,
          [this](const bool success, const QString& code, const QString& message) {
            if (success) {
              return;
            }
            const QJsonObject value = statusObject(
                QStringLiteral("error"),
                code.isEmpty() ? QStringLiteral("keyring_error") : code,
                message.isEmpty()
                    ? QStringLiteral("Google OAuth credentials could not be saved")
                    : message);
            emit syncStatusChanged({}, value);
          });
  connect(&m_auth, &GoogleAuthManager::forgetFinished, this,
          &GoogleSync::finishDisconnect);
  connect(
      &m_auth, &GoogleAuthManager::authorizationFailed, this,
      [this](const QString& accountId, const QString& code, const QString& message) {
        if (SyncJob* job = m_jobs.value(accountId, nullptr); job != nullptr) {
          requireReauthorization(job, code, message);
          return;
        }
        if (m_reauthorizationSnapshots.contains(accountId)) {
          m_reauthorizationSnapshots.remove(accountId);
          m_auth.cancel(accountId);
          setAccountAuthStatus(accountId, QStringLiteral("reauthorization_required"));
          const QJsonObject value = statusObject(
              QStringLiteral("reauthorization_required"),
              code.isEmpty() ? QStringLiteral("authentication_required") : code,
              message);
          m_status.insert(accountId, value);
          emit accountChanged(accountId);
          emit syncStatusChanged(accountId, value);
          return;
        }
        const bool authenticationRequired =
            code == QStringLiteral("authentication_required");
        const QString state = authenticationRequired
                                  ? QStringLiteral("reauthorization_required")
                                  : QStringLiteral("error");
        setAccountAuthStatus(accountId, state);
        const QJsonObject value = statusObject(state, code, message);
        m_status.insert(accountId, value);
        emit accountChanged(accountId);
        emit syncStatusChanged(accountId, value);
      });
}

ProviderCapabilities GoogleSync::capabilities() const {
  ProviderCapabilities value;
  value.incrementalSync = true;
  value.attendees = true;
  value.reminders = true;
  value.conferenceData = true;
  value.serverScheduling = true;
  return value;
}

bool GoogleSync::deleteCalendar(
    const Calendar& calendar,
    std::function<void(bool, const QString&, const QString&)> callback,
    QString* errorMessage) {
  if (calendar.accountId.isEmpty() || calendar.remoteId.isEmpty() ||
      !canDeleteCalendar(calendar)) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("This Google calendar cannot be deleted");
    }
    return false;
  }
  const QString accountId = calendar.accountId;
  m_client.deleteCalendar(
      accountId, calendar.remoteId,
      [this, accountId, callback = std::move(callback)](const ApiResponse& response) {
        if (response.ok || response.notFound) {
          callback(true, {}, {});
          return;
        }
        const QString code =
            response.insufficientScope     ? QStringLiteral("insufficient_scope")
            : response.errorCode.isEmpty() ? QStringLiteral("calendar_delete_failed")
                                           : response.errorCode;
        const QString message = response.insufficientScope
                                    ? QStringLiteral(
                                          "Reauthorize this Google account to grant "
                                          "calendar-management access, then try again")
                                : response.errorMessage.isEmpty()
                                    ? QStringLiteral("Google calendar deletion failed")
                                    : response.errorMessage;
        const bool reauthorizationRequired =
            response.authenticationRequired || response.insufficientScope;
        const QString state = reauthorizationRequired
                                  ? QStringLiteral("reauthorization_required")
                                  : QStringLiteral("error");
        if (reauthorizationRequired) {
          setAccountAuthStatus(accountId, state);
          m_auth.cancel(accountId);
          emit accountChanged(accountId);
        }
        const QJsonObject value = statusObject(state, code, message);
        m_status.insert(accountId, value);
        emit syncStatusChanged(accountId, value);
        callback(false, code, message);
      });
  return true;
}

GoogleSync::~GoogleSync() {
  m_shuttingDown = true;
  m_pollTimer.stop();
  m_retryWakeAt.clear();
  disconnect(&m_auth, nullptr, this, nullptr);
  disconnect(&m_pollTimer, nullptr, this, nullptr);
  qDeleteAll(m_jobs);
  m_jobs.clear();
  qDeleteAll(m_pendingDisconnects);
  m_pendingDisconnects.clear();
}

GoogleSync::SyncJob* GoogleSync::activeJob(const QString& accountId,
                                           const quint64 generation) const {
  if (m_shuttingDown) {
    return nullptr;
  }
  SyncJob* job = m_jobs.value(accountId, nullptr);
  return job != nullptr && job->generation == generation ? job : nullptr;
}

bool GoogleSync::refreshAndResume(SyncJob* job, std::function<void(SyncJob*)> resume,
                                  QString* errorMessage) {
  if (job == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Google sync job is no longer active");
    }
    return false;
  }
  const QString accountId = job->accountId;
  const quint64 generation = job->generation;
  if (activeJob(accountId, generation) != job) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Google sync job is no longer active");
    }
    return false;
  }
  if (job->tokenRefreshAttempted) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Google rejected the refreshed access token");
    }
    return false;
  }

  job->tokenRefreshAttempted = true;
  job->waitingForTokenRefresh = true;
  job->resumeAfterTokenRefresh = std::move(resume);

  QString refreshError;
  const bool started = m_auth.refresh(accountId, &refreshError);
  job = activeJob(accountId, generation);
  if (job == nullptr) {
    // A synchronous authorization failure may already have finished the job.
    return true;
  }
  if (started) {
    return true;
  }

  job->waitingForTokenRefresh = false;
  job->resumeAfterTokenRefresh = {};
  if (errorMessage != nullptr) {
    *errorMessage = refreshError.isEmpty()
                        ? QStringLiteral("Could not refresh Google access token")
                        : refreshError;
  }
  return false;
}

bool GoogleSync::retryReadRequest(SyncJob* job, const ApiResponse& response,
                                  const QString& operationKey,
                                  std::function<void(SyncJob*)> resume) {
  if (job == nullptr || !response.retryable) {
    return false;
  }
  const int completedAttempts = job->readRetryAttempts + 1;
  const RetryDecision decision = m_retryPolicy.evaluate({
      providerErrorForResponse(response),
      completedAttempts,
      true,
      false,
      stableJitterKey(job->accountId + QLatin1Char(':') + operationKey),
  });
  if (decision.action != RetryAction::Retry) {
    return false;
  }

  job->readRetryAttempts = completedAttempts;
  const QString accountId = job->accountId;
  const quint64 generation = job->generation;
  const qint64 delayMs =
      std::clamp<qint64>(static_cast<qint64>(decision.delay.count()), 0,
                         static_cast<qint64>(std::numeric_limits<int>::max()));
  const QJsonObject value = statusObject(QStringLiteral("retry_wait"),
                                         response.errorCode, response.errorMessage);
  m_status.insert(accountId, value);
  emit syncStatusChanged(accountId, value);
  QTimer::singleShot(
      static_cast<int>(delayMs), this,
      [this, accountId, generation, resume = std::move(resume)]() mutable {
        SyncJob* active = activeJob(accountId, generation);
        if (active == nullptr) {
          return;
        }
        const QJsonObject syncing = statusObject(QStringLiteral("syncing"));
        m_status.insert(accountId, syncing);
        emit syncStatusChanged(accountId, syncing);
        resume(active);
      });
  return true;
}

void GoogleSync::scheduleRetryWake(const QString& accountId, const QDateTime& wakeAt) {
  if (accountId.isEmpty() || m_shuttingDown) {
    return;
  }
  const QDateTime normalized =
      wakeAt.isValid() ? wakeAt.toUTC() : QDateTime::currentDateTimeUtc();
  const QDateTime existing = m_retryWakeAt.value(accountId);
  if (existing.isValid() && existing <= normalized) {
    return;
  }
  m_retryWakeAt.insert(accountId, normalized);
  const qint64 delayMs =
      std::clamp<qint64>(QDateTime::currentDateTimeUtc().msecsTo(normalized), 0,
                         static_cast<qint64>(std::numeric_limits<int>::max()));
  QTimer::singleShot(static_cast<int>(delayMs), this, [this, accountId, normalized]() {
    if (m_retryWakeAt.value(accountId) != normalized) {
      return;
    }
    m_retryWakeAt.remove(accountId);
    if (m_jobs.contains(accountId)) {
      scheduleRetryWake(accountId, QDateTime::currentDateTimeUtc().addMSecs(250));
      return;
    }
    syncAccount(accountId);
  });
}

void GoogleSync::requireReauthorization(SyncJob* job, const QString& errorCode,
                                        const QString& errorMessage) {
  if (job == nullptr) {
    return;
  }
  const QString accountId = job->accountId;
  const quint64 generation = job->generation;
  if (activeJob(accountId, generation) != job) {
    return;
  }
  setAccountAuthStatus(accountId, QStringLiteral("reauthorization_required"));
  // Clear the rejected in-memory access token. The persisted refresh token is
  // retained for diagnostics or an explicit reconnect, while periodic sync is
  // suppressed by the account status check.
  m_auth.cancel(accountId);
  finish(job,
         errorCode.isEmpty() ? QStringLiteral("authentication_required") : errorCode,
         errorMessage.isEmpty() ? QStringLiteral("Google account needs authorization")
                                : errorMessage);
}

bool GoogleSync::loadConfiguration(QString* errorMessage) {
  if (m_database == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Database is unavailable");
    }
    return false;
  }
  QString databaseError;
  const QString clientId = m_database
                               ->setting(QString::fromLatin1(kClientIdSetting),
                                         defaultOAuthClientId(), &databaseError)
                               .toString();
  if (!databaseError.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = databaseError;
    }
    return false;
  }
  if (clientId.isEmpty()) {
    return true;
  }
  return m_auth.restoreClient(clientId, errorMessage);
}

bool GoogleSync::configureClient(const QString& clientId, const QString& clientSecret,
                                 QString* errorMessage) {
  if (!m_auth.configureClient(clientId, clientSecret, errorMessage)) {
    return false;
  }
  return m_database->setSetting(QString::fromLatin1(kClientIdSetting), clientId,
                                errorMessage);
}

bool GoogleSync::isConfigured() const { return m_auth.isConfigured(); }

QString GoogleSync::beginAuthorization(const QString& displayName,
                                       QString* errorMessage) {
  if (!isConfigured()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Configure a Google OAuth desktop client first");
    }
    return {};
  }
  Account account;
  account.id = newUuid();
  account.provider = ProviderKind::Google;
  account.displayName = displayName.trimmed().isEmpty()
                            ? QStringLiteral("Google Calendar")
                            : displayName.trimmed();
  account.endpoint = QStringLiteral("https://www.googleapis.com/calendar/v3");
  account.authStatus = QStringLiteral("authorizing");
  if (!m_database->upsertAccount(account, errorMessage)) {
    return {};
  }
  m_provisionalAuthorizationIds.insert(account.id);
  if (!m_auth.startAuthorization(account.id, errorMessage)) {
    m_provisionalAuthorizationIds.remove(account.id);
    m_database->removeAccount(account.id);
    return {};
  }
  emit accountChanged(account.id);
  return account.id;
}

bool GoogleSync::reauthorizeAccount(const QString& accountId, QString* errorMessage) {
  if (!isConfigured()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Configure a Google OAuth desktop client first");
    }
    return false;
  }
  Account account = m_database->account(accountId, errorMessage);
  if (account.id.isEmpty()) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = QStringLiteral("Google account was not found");
    }
    return false;
  }
  if (account.provider != ProviderKind::Google) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Only Google accounts use Google OAuth");
    }
    return false;
  }
  if (m_reauthorizationSnapshots.contains(accountId)) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Google authorization is already in progress");
    }
    return false;
  }

  if (SyncJob* job = m_jobs.take(accountId); job != nullptr) {
    delete job;
  }
  m_retryWakeAt.remove(accountId);
  m_reauthorizationSnapshots.insert(accountId, account);
  account.enabled = true;
  account.authStatus = QStringLiteral("authorizing");
  if (!m_database->upsertAccount(account, errorMessage)) {
    m_reauthorizationSnapshots.remove(accountId);
    return false;
  }
  if (!m_auth.startAuthorization(accountId, errorMessage)) {
    const Account snapshot = m_reauthorizationSnapshots.take(accountId);
    m_database->upsertAccount(snapshot);
    if (snapshot.enabled && snapshot.authStatus == QStringLiteral("connected")) {
      m_auth.restoreAuthorization(accountId);
    }
    return false;
  }
  emit accountChanged(accountId);
  return true;
}

bool GoogleSync::restoreAccounts(QString* errorMessage) {
  if (!loadConfiguration(errorMessage)) {
    return false;
  }
  if (!isConfigured()) {
    return true;
  }
  QString databaseError;
  const QList<Account> accounts = m_database->accounts(&databaseError);
  if (!databaseError.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = databaseError;
    }
    return false;
  }
  const int storedScopeVersion =
      m_database
          ->setting(QString::fromLatin1(kOAuthScopeVersionSetting), 1, &databaseError)
          .toInt(1);
  if (!databaseError.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = databaseError;
    }
    return false;
  }
  if (storedScopeVersion < kOAuthScopeVersion) {
    for (const Account& account : accounts) {
      if (account.provider != ProviderKind::Google) {
        continue;
      }
      setAccountAuthStatus(account.id, QStringLiteral("reauthorization_required"));
      const QJsonObject value =
          statusObject(QStringLiteral("reauthorization_required"),
                       QStringLiteral("oauth_scope_upgrade"),
                       QStringLiteral("Reauthorize this Google account to grant "
                                      "calendar-management access"));
      m_status.insert(account.id, value);
      emit accountChanged(account.id);
      emit syncStatusChanged(account.id, value);
    }
    if (!m_database->setSetting(QString::fromLatin1(kOAuthScopeVersionSetting),
                                kOAuthScopeVersion, &databaseError)) {
      if (errorMessage != nullptr) {
        *errorMessage = databaseError;
      }
      return false;
    }
  }
  for (const Account& account : accounts) {
    if (account.provider != ProviderKind::Google || !account.enabled ||
        storedScopeVersion < kOAuthScopeVersion) {
      continue;
    }
    QString restoreError;
    if (!m_auth.restoreAuthorization(account.id, &restoreError)) {
      setAccountAuthStatus(account.id, QStringLiteral("reauthorization_required"));
      const QJsonObject value =
          statusObject(QStringLiteral("reauthorization_required"),
                       QStringLiteral("authentication_required"), restoreError);
      m_status.insert(account.id, value);
      emit syncStatusChanged(account.id, value);
    }
  }
  return true;
}

void GoogleSync::cancelAuthorization(const QString& accountId) {
  const bool provisional = m_provisionalAuthorizationIds.remove(accountId);
  const Account reauthorizationSnapshot = m_reauthorizationSnapshots.take(accountId);
  m_auth.cancel(accountId);
  if (provisional) {
    m_database->removeAccount(accountId);
    emit accountChanged(accountId);
    return;
  }
  if (!reauthorizationSnapshot.id.isEmpty()) {
    m_database->upsertAccount(reauthorizationSnapshot);
    if (reauthorizationSnapshot.enabled &&
        reauthorizationSnapshot.authStatus == QStringLiteral("connected")) {
      m_auth.restoreAuthorization(accountId);
    }
    emit accountChanged(accountId);
  }
}

bool GoogleSync::disconnectAccount(const QString& accountId,
                                   const bool removeCachedData, QString* errorMessage) {
  if (m_pendingDisconnects.contains(accountId)) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("Google account disconnect is already in progress");
    }
    return false;
  }
  Account account = m_database->account(accountId, errorMessage);
  if (account.id.isEmpty() || account.provider != ProviderKind::Google) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = QStringLiteral("Google account was not found");
    }
    return false;
  }

  m_provisionalAuthorizationIds.remove(accountId);
  m_reauthorizationSnapshots.remove(accountId);
  m_retryWakeAt.remove(accountId);
  m_pendingHydrations.remove(accountId);
  if (SyncJob* job = m_jobs.take(accountId); job != nullptr) {
    delete job;
  }
  auto* pending = new PendingDisconnect;
  pending->snapshot = account;
  pending->removeCachedData = removeCachedData;
  m_pendingDisconnects.insert(accountId, pending);

  account.enabled = false;
  account.authStatus = QStringLiteral("disconnecting");
  if (!m_database->upsertAccount(account, errorMessage)) {
    m_pendingDisconnects.remove(accountId);
    delete pending;
    return false;
  }
  if (!m_auth.forget(accountId, errorMessage)) {
    m_pendingDisconnects.remove(accountId);
    m_database->upsertAccount(pending->snapshot);
    delete pending;
    return false;
  }
  emit accountChanged(accountId);
  return true;
}

void GoogleSync::finishDisconnect(const QString& accountId, const bool secretRemoved,
                                  const QString& errorCode,
                                  const QString& errorMessage) {
  PendingDisconnect* pending = m_pendingDisconnects.take(accountId);
  if (pending == nullptr || m_shuttingDown) {
    delete pending;
    return;
  }
  const Account snapshot = pending->snapshot;
  const bool removeCachedData = pending->removeCachedData;
  delete pending;

  QString databaseError;
  if (!secretRemoved) {
    m_database->upsertAccount(snapshot, &databaseError);
    if (snapshot.enabled && snapshot.authStatus == QStringLiteral("connected")) {
      m_auth.restoreAuthorization(accountId);
    }
    const QJsonObject value =
        statusObject(QStringLiteral("error"),
                     errorCode.isEmpty() ? QStringLiteral("keyring_error") : errorCode,
                     errorMessage.isEmpty()
                         ? QStringLiteral("Google credentials could not be removed")
                         : errorMessage);
    m_status.insert(accountId, value);
    emit accountChanged(accountId);
    emit syncStatusChanged(accountId, value);
    return;
  }

  bool completed = false;
  if (removeCachedData) {
    completed = m_database->removeAccount(accountId, &databaseError);
  } else {
    Account account = m_database->account(accountId, &databaseError);
    if (!account.id.isEmpty()) {
      account.enabled = false;
      account.authStatus = QStringLiteral("disconnected");
      completed = m_database->upsertAccount(account, &databaseError);
    }
  }
  if (!completed) {
    Account recovery = snapshot;
    recovery.authStatus = QStringLiteral("reauthorization_required");
    m_database->upsertAccount(recovery);
    const QJsonObject value = statusObject(
        QStringLiteral("error"), QStringLiteral("database_error"), databaseError);
    m_status.insert(accountId, value);
    emit accountChanged(accountId);
    emit syncStatusChanged(accountId, value);
    return;
  }
  m_status.remove(accountId);
  emit accountChanged(accountId);
}

void GoogleSync::syncAll() {
  QString error;
  const QList<Account> accounts = m_database->accounts(&error);
  if (!error.isEmpty()) {
    return;
  }
  for (const Account& account : accounts) {
    if (account.provider == ProviderKind::Google && account.enabled) {
      syncAccount(account.id);
    }
  }
}

void GoogleSync::syncAccount(const QString& accountId) {
  if (m_jobs.contains(accountId)) {
    return;
  }
  QString error;
  const Account account = m_database->account(accountId, &error);
  if (account.id.isEmpty() || account.provider != ProviderKind::Google ||
      !account.enabled ||
      account.authStatus == QStringLiteral("reauthorization_required") ||
      account.authStatus == QStringLiteral("authorizing") ||
      account.authStatus == QStringLiteral("disconnecting")) {
    return;
  }
  if (!m_auth.hasAccessToken(accountId)) {
    if (!m_auth.refresh(accountId, &error)) {
      setAccountAuthStatus(accountId, QStringLiteral("reauthorization_required"));
      const QJsonObject value =
          statusObject(QStringLiteral("reauthorization_required"),
                       QStringLiteral("authentication_required"), error);
      m_status.insert(accountId, value);
      emit syncStatusChanged(accountId, value);
    }
    return;
  }

  auto* job = new SyncJob;
  job->accountId = accountId;
  job->generation = ++m_nextJobGeneration;
  job->account = account;
  job->calendarListSyncToken =
      m_database
          ->providerState(accountId, {}, QString::fromLatin1(kCalendarListToken),
                          QString())
          .toString();
  m_jobs.insert(accountId, job);
  const QJsonObject value = statusObject(QStringLiteral("syncing"));
  m_status.insert(accountId, value);
  const quint64 generation = job->generation;
  emit syncStatusChanged(accountId, value);
  job = activeJob(accountId, generation);
  if (job != nullptr) {
    startCalendarList(job);
  }
}

bool GoogleSync::syncRange(const RangeSyncRequest& request, QString* errorMessage) {
  if (!request.startUtc.isValid() || !request.endUtc.isValid() ||
      request.startUtc >= request.endUtc ||
      request.startUtc.daysTo(request.endUtc) > 5 * 366) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Google range sync needs bounded UTC dates");
    }
    return false;
  }
  QString databaseError;
  const Calendar calendar = m_database->calendar(request.calendarId, &databaseError);
  const Account account = m_database->account(calendar.accountId, &databaseError);
  if (calendar.id.isEmpty() || account.provider != ProviderKind::Google ||
      !calendar.enabled || !account.enabled) {
    if (errorMessage != nullptr) {
      *errorMessage = databaseError.isEmpty()
                          ? QStringLiteral("Google calendar is unavailable")
                          : databaseError;
    }
    return false;
  }
  if (m_database->isSyncRangeCovered(calendar.id, request.startUtc, request.endUtc,
                                     &databaseError)) {
    return true;
  }
  if (!databaseError.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = databaseError;
    }
    return false;
  }

  RangeSyncRequest normalized{calendar.id, request.startUtc.toUTC(),
                              request.endUtc.toUTC()};
  QList<RangeSyncRequest>& pending = m_pendingHydrations[account.id];
  bool merged = false;
  for (RangeSyncRequest& existing : pending) {
    if (existing.calendarId != normalized.calendarId ||
        existing.endUtc < normalized.startUtc ||
        normalized.endUtc < existing.startUtc) {
      continue;
    }
    const QDateTime mergedStart = normalized.startUtc < existing.startUtc
                                      ? normalized.startUtc
                                      : existing.startUtc;
    const QDateTime mergedEnd =
        normalized.endUtc > existing.endUtc ? normalized.endUtc : existing.endUtc;
    if (mergedStart.daysTo(mergedEnd) > 5 * 366) {
      continue;
    }
    existing.startUtc = mergedStart;
    existing.endUtc = mergedEnd;
    merged = true;
    break;
  }
  if (!merged) {
    pending.append(normalized);
  }
  QTimer::singleShot(0, this,
                     [this, accountId = account.id]() { syncAccount(accountId); });
  return true;
}

void GoogleSync::setAccountAuthStatus(const QString& accountId, const QString& status) {
  Account account = m_database->account(accountId);
  if (account.id.isEmpty()) {
    return;
  }
  account.authStatus = status;
  m_database->upsertAccount(account);
}

void GoogleSync::startCalendarList(SyncJob* job, const QString& pageToken) {
  if (job == nullptr) {
    return;
  }
  const QString accountId = job->accountId;
  const quint64 generation = job->generation;
  if (activeJob(accountId, generation) != job) {
    return;
  }
  if (pageToken.isEmpty()) {
    job->calendarListFullSync = job->calendarListSyncToken.isEmpty();
    job->seenCalendarRemoteIds.clear();
  }
  const QPointer<GoogleSync> guard(this);
  m_client.listCalendars(
      accountId, pageToken, job->calendarListSyncToken,
      [this, guard, accountId, pageToken, generation](const ApiResponse& response) {
        if (guard == nullptr) {
          return;
        }
        SyncJob* currentJob = activeJob(accountId, generation);
        if (currentJob == nullptr) {
          return;
        }
        if (!response.ok) {
          if (response.authenticationRequired) {
            QString refreshError;
            if (refreshAndResume(
                    currentJob,
                    [this, pageToken](SyncJob* resumed) {
                      startCalendarList(resumed, pageToken);
                    },
                    &refreshError)) {
              return;
            }
            requireReauthorization(
                currentJob, response.errorCode,
                refreshError.isEmpty() ? response.errorMessage : refreshError);
            return;
          }
          if (response.httpStatus == 410 &&
              !currentJob->calendarListSyncToken.isEmpty() &&
              !currentJob->calendarListResetAttempted) {
            currentJob->calendarListResetAttempted = true;
            currentJob->calendarListSyncToken.clear();
            QString error;
            if (!m_database->setProviderState(currentJob->accountId, {},
                                              QString::fromLatin1(kCalendarListToken),
                                              QString(), &error)) {
              finish(currentJob, QStringLiteral("database_error"), error);
              return;
            }
            startCalendarList(currentJob);
            return;
          }
          if (retryReadRequest(currentJob, response,
                               QStringLiteral("calendar-list:%1").arg(pageToken),
                               [this, pageToken](SyncJob* resumed) {
                                 startCalendarList(resumed, pageToken);
                               })) {
            return;
          }
          finish(currentJob, response.errorCode, response.errorMessage);
          return;
        }
        currentJob->readRetryAttempts = 0;
        const QJsonArray items = response.body.value(QStringLiteral("items")).toArray();
        for (const QJsonValue& value : items) {
          if (!value.isObject()) {
            continue;
          }
          const QJsonObject resource = value.toObject();
          Calendar calendar = calendarFromGoogleJson(resource, currentJob->accountId);
          if (calendar.remoteId.isEmpty()) {
            continue;
          }
          currentJob->seenCalendarRemoteIds.append(calendar.remoteId);
          const Calendar existing =
              m_database->calendarByRemoteId(currentJob->accountId, calendar.remoteId);
          if (!existing.id.isEmpty()) {
            calendar.id = existing.id;
            calendar.href = existing.href;
            calendar.syncToken = existing.syncToken;
            calendar.lastSyncAt = existing.lastSyncAt;
            calendar.colorOverride = existing.colorOverride;
            calendar.position = existing.position;
            calendar.ignoreAlerts = existing.ignoreAlerts;

            if (calendar.name.isEmpty()) {
              calendar.name = existing.name;
            }
            if (calendar.description.isEmpty()) {
              calendar.description = existing.description;
            }
            if (calendar.color.isEmpty()) {
              calendar.color = existing.color;
            }
            if (calendar.timeZone.isEmpty()) {
              calendar.timeZone = existing.timeZone;
            }

            QJsonObject capabilities = existing.capabilities;
            for (auto iterator = calendar.capabilities.constBegin();
                 iterator != calendar.capabilities.constEnd(); ++iterator) {
              capabilities.insert(iterator.key(), iterator.value());
            }
            capabilities.remove(QStringLiteral("calendarListRemoved"));
            calendar.capabilities = capabilities;

            const bool unavailable =
                resource.value(QStringLiteral("deleted")).toBool(false) ||
                resource.value(QStringLiteral("hidden")).toBool(false);
            if (resource.value(QStringLiteral("deleted")).toBool(false)) {
              // CalendarList tombstones can contain only id/deleted. Retain
              // the useful cached presentation and capability information.
              calendar.name = existing.name;
              calendar.description = existing.description;
              calendar.color = existing.color;
              calendar.timeZone = existing.timeZone;
              calendar.readOnly = existing.readOnly;
              calendar.syncToken.clear();
              calendar.capabilities = existing.capabilities;
              calendar.capabilities.insert(QStringLiteral("provider"),
                                           QStringLiteral("google"));
              calendar.capabilities.insert(QStringLiteral("deleted"), true);
              calendar.capabilities.insert(QStringLiteral("selected"), false);
              calendar.capabilities.insert(QStringLiteral("calendarListRemoved"), true);
            }
            // `enabled` is OmaCalendar's local visibility preference. Google
            // CalendarList's selected flag is only used for a newly discovered
            // calendar and must not turn a user's hidden calendar back on.
            calendar.enabled = unavailable ? false : existing.enabled;
          } else {
            calendar.id = newUuid();
          }
          QString error;
          if (!m_database->upsertCalendar(calendar, &error)) {
            finish(currentJob, QStringLiteral("database_error"), error);
            return;
          }
          if (calendar.capabilities.value(QStringLiteral("primary")).toBool()) {
            currentJob->account.principal = calendar.remoteId;
            if (currentJob->account.displayName == QStringLiteral("Google Calendar")) {
              currentJob->account.displayName = calendar.name;
            }
            m_database->upsertAccount(currentJob->account);
          }
        }
        const QString nextPage =
            response.body.value(QStringLiteral("nextPageToken")).toString();
        if (!nextPage.isEmpty()) {
          startCalendarList(currentJob, nextPage);
          return;
        }
        const QString nextSync =
            response.body.value(QStringLiteral("nextSyncToken")).toString();
        if (!nextSync.isEmpty()) {
          QString error;
          if (!m_database->setProviderState(currentJob->accountId, {},
                                            QString::fromLatin1(kCalendarListToken),
                                            nextSync, &error)) {
            finish(currentJob, QStringLiteral("database_error"), error);
            return;
          }
          currentJob->calendarListSyncToken = nextSync;
        }
        if (currentJob->calendarListFullSync) {
          currentJob->seenCalendarRemoteIds.removeDuplicates();
          QString error;
          const QList<Calendar> known =
              m_database->calendars(currentJob->accountId, &error);
          if (!error.isEmpty()) {
            finish(currentJob, QStringLiteral("database_error"), error);
            return;
          }
          for (Calendar calendar : known) {
            if (calendar.remoteId.isEmpty() ||
                currentJob->seenCalendarRemoteIds.contains(calendar.remoteId)) {
              continue;
            }
            calendar.enabled = false;
            calendar.syncToken.clear();
            calendar.capabilities.insert(QStringLiteral("calendarListRemoved"), true);
            if (!m_database->upsertCalendar(calendar, &error)) {
              finish(currentJob, QStringLiteral("database_error"), error);
              return;
            }
          }
        }
        QString error;
        currentJob->calendars = m_database->calendars(currentJob->accountId, &error);
        if (!error.isEmpty()) {
          finish(currentJob, QStringLiteral("database_error"), error);
          return;
        }
        currentJob->calendarIndex = 0;
        emit calendarsChanged(currentJob->accountId);
        currentJob = activeJob(accountId, generation);
        if (currentJob != nullptr) {
          syncNextCalendar(currentJob);
        }
      });
}

void GoogleSync::syncNextCalendar(SyncJob* job) {
  if (job == nullptr) {
    return;
  }
  const QString accountId = job->accountId;
  const quint64 generation = job->generation;
  if (activeJob(accountId, generation) != job) {
    return;
  }
  while (job->calendarIndex < job->calendars.size()) {
    job->currentCalendar = job->calendars.at(job->calendarIndex++);
    if (job->currentCalendar.enabled) {
      job->hydrationSync = false;
      job->eventResetAttempted = false;
      job->replaceEventCoverage = false;
      startEventPage(job);
      return;
    }
  }
  syncNextHydration(job);
}

void GoogleSync::syncNextHydration(SyncJob* job) {
  if (job == nullptr || activeJob(job->accountId, job->generation) != job) {
    return;
  }
  QList<RangeSyncRequest>& pending = m_pendingHydrations[job->accountId];
  while (!pending.isEmpty()) {
    const RangeSyncRequest request = pending.first();
    QString error;
    if (m_database->isSyncRangeCovered(request.calendarId, request.startUtc,
                                       request.endUtc, &error)) {
      pending.removeFirst();
      continue;
    }
    if (!error.isEmpty()) {
      finish(job, QStringLiteral("database_error"), error);
      return;
    }
    const Calendar calendar = m_database->calendar(request.calendarId, &error);
    if (calendar.id.isEmpty() || calendar.accountId != job->accountId ||
        !calendar.enabled) {
      pending.removeFirst();
      continue;
    }
    job->currentCalendar = calendar;
    job->hydrationSync = true;
    job->hydrationRequest = request;
    job->eventResetAttempted = false;
    job->replaceEventCoverage = false;
    startEventPage(job);
    return;
  }
  m_pendingHydrations.remove(job->accountId);
  drainOutbox(job);
}

void GoogleSync::startEventPage(SyncJob* job, const QString& pageToken) {
  if (job == nullptr) {
    return;
  }
  const QString accountId = job->accountId;
  const quint64 generation = job->generation;
  if (activeJob(accountId, generation) != job) {
    return;
  }
  const Calendar calendar = job->currentCalendar;
  QString requestSyncToken = calendar.syncToken;
  if (pageToken.isEmpty()) {
    QString coverageError;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto [defaultStart, defaultEnd] = initialCoverageWindow(now);
    const bool defaultCovered = m_database->isSyncRangeCovered(
        calendar.id, defaultStart, defaultEnd, &coverageError);
    if (!coverageError.isEmpty()) {
      finish(job, QStringLiteral("database_error"), coverageError);
      return;
    }
    job->eventFullSync =
        job->hydrationSync || calendar.syncToken.isEmpty() || !defaultCovered;
    job->seenRemoteIds.clear();
    job->stagedEvents.clear();
    job->stagedDeletedRemoteIds.clear();
    if (job->hydrationSync) {
      job->eventWindowStartUtc = job->hydrationRequest.startUtc;
      job->eventWindowEndUtc = job->hydrationRequest.endUtc;
      requestSyncToken.clear();
    } else if (job->eventFullSync) {
      job->eventWindowStartUtc = defaultStart;
      job->eventWindowEndUtc = defaultEnd;
      requestSyncToken.clear();
    } else {
      job->eventWindowStartUtc = {};
      job->eventWindowEndUtc = {};
    }
  } else if (job->eventFullSync) {
    requestSyncToken.clear();
  }
  const QPointer<GoogleSync> guard(this);
  m_client.listEvents(
      accountId, calendar.remoteId, pageToken, requestSyncToken,
      job->eventWindowStartUtc, job->eventWindowEndUtc,
      [this, guard, accountId, pageToken, generation](const ApiResponse& response) {
        if (guard == nullptr) {
          return;
        }
        SyncJob* currentJob = activeJob(accountId, generation);
        if (currentJob == nullptr) {
          return;
        }
        if (!response.ok) {
          if (response.authenticationRequired) {
            QString refreshError;
            if (refreshAndResume(
                    currentJob,
                    [this, pageToken](SyncJob* resumed) {
                      startEventPage(resumed, pageToken);
                    },
                    &refreshError)) {
              return;
            }
            requireReauthorization(
                currentJob, response.errorCode,
                refreshError.isEmpty() ? response.errorMessage : refreshError);
            return;
          }
          if (response.httpStatus == 410 && !currentJob->eventFullSync &&
              !currentJob->currentCalendar.syncToken.isEmpty() &&
              !currentJob->eventResetAttempted) {
            currentJob->eventResetAttempted = true;
            currentJob->replaceEventCoverage = true;
            currentJob->currentCalendar.syncToken.clear();
            startEventPage(currentJob);
            return;
          }
          if (response.httpStatus == 404) {
            currentJob->currentCalendar.enabled = false;
            currentJob->currentCalendar.syncToken.clear();
            currentJob->currentCalendar.capabilities.insert(
                QStringLiteral("calendarListRemoved"), true);
            QString error;
            if (!m_database->upsertCalendar(currentJob->currentCalendar, &error)) {
              finish(currentJob, QStringLiteral("database_error"), error);
              return;
            }
            emit calendarsChanged(currentJob->accountId);
            syncNextCalendar(currentJob);
            return;
          }
          if (retryReadRequest(
                  currentJob, response,
                  QStringLiteral("events:%1:%2")
                      .arg(currentJob->currentCalendar.remoteId, pageToken),
                  [this, pageToken](SyncJob* resumed) {
                    startEventPage(resumed, pageToken);
                  })) {
            return;
          }
          finish(currentJob, response.errorCode, response.errorMessage);
          return;
        }
        currentJob->readRetryAttempts = 0;
        const QJsonArray items = response.body.value(QStringLiteral("items")).toArray();
        for (const QJsonValue& value : items) {
          if (!value.isObject()) {
            continue;
          }
          const QJsonObject raw = value.toObject();
          Event event = eventFromGoogleJson(
              raw, currentJob->currentCalendar.id,
              calendarDefaultReminders(currentJob->currentCalendar));
          if (!event.remoteId.isEmpty()) {
            currentJob->seenRemoteIds.insert(event.remoteId);
          }
          QString error;
          bool conflict = false;
          // A cancelled recurring instance is an exception tombstone, not a
          // deletion of the series. Persist it so recurrence expansion can
          // suppress that occurrence. Ordinary/series cancellations remove
          // the matching remote record as before.
          bool applied = true;
          if (currentJob->eventFullSync) {
            if (event.deleted && event.recurrenceId.isEmpty()) {
              currentJob->stagedDeletedRemoteIds.append(event.remoteId);
            } else {
              currentJob->stagedEvents.append(std::move(event));
            }
          } else {
            applied = event.deleted && event.recurrenceId.isEmpty()
                          ? m_database->removeRemoteEvent(
                                currentJob->currentCalendar.id, event.remoteId,
                                event.rawPayload, &error, &conflict)
                          : m_database->applyRemoteEvent(event, &error, &conflict);
          }
          if (!applied) {
            finish(currentJob, QStringLiteral("database_error"), error);
            return;
          }
          if (!currentJob->eventFullSync && !currentJob->changedCalendarIds.contains(
                                                currentJob->currentCalendar.id)) {
            currentJob->changedCalendarIds.append(currentJob->currentCalendar.id);
          }
        }
        const QString nextPage =
            response.body.value(QStringLiteral("nextPageToken")).toString();
        if (!nextPage.isEmpty()) {
          startEventPage(currentJob, nextPage);
          return;
        }
        const QString nextSync =
            response.body.value(QStringLiteral("nextSyncToken")).toString();
        if (!nextSync.isEmpty() && !currentJob->hydrationSync) {
          currentJob->currentCalendar.syncToken = nextSync;
        }
        currentJob->currentCalendar.lastSyncAt = QDateTime::currentDateTimeUtc();
        QString error;
        if (currentJob->eventFullSync) {
          const QList<Event> coveredEvents = m_database->eventsBetween(
              currentJob->eventWindowStartUtc, currentJob->eventWindowEndUtc,
              {currentJob->currentCalendar.id}, &error);
          if (!error.isEmpty()) {
            finish(currentJob, QStringLiteral("database_error"), error);
            return;
          }
          currentJob->stagedDeletedRemoteIds.removeDuplicates();
          const QStringList prunedRemoteIds =
              googleFullSyncPruneCandidates(coveredEvents, currentJob->seenRemoteIds);
          if (!m_database->applyRemoteRangeSyncBatch(
                  currentJob->currentCalendar, currentJob->stagedEvents,
                  currentJob->stagedDeletedRemoteIds, prunedRemoteIds,
                  currentJob->eventWindowStartUtc, currentJob->eventWindowEndUtc,
                  &error, currentJob->replaceEventCoverage)) {
            finish(currentJob, QStringLiteral("database_error"), error);
            return;
          }
          if (!currentJob->changedCalendarIds.contains(
                  currentJob->currentCalendar.id)) {
            currentJob->changedCalendarIds.append(currentJob->currentCalendar.id);
          }
        } else if (!m_database->upsertCalendar(currentJob->currentCalendar, &error)) {
          finish(currentJob, QStringLiteral("database_error"), error);
          return;
        }
        if (currentJob->hydrationSync) {
          QList<RangeSyncRequest>& pending = m_pendingHydrations[currentJob->accountId];
          if (!pending.isEmpty() &&
              pending.first().calendarId == currentJob->hydrationRequest.calendarId &&
              pending.first().startUtc == currentJob->hydrationRequest.startUtc &&
              pending.first().endUtc == currentJob->hydrationRequest.endUtc) {
            pending.removeFirst();
          }
          syncNextHydration(currentJob);
        } else {
          syncNextCalendar(currentJob);
        }
      });
}

void GoogleSync::drainOutbox(SyncJob* job) {
  if (job == nullptr) {
    return;
  }
  const QString accountId = job->accountId;
  const quint64 generation = job->generation;
  if (activeJob(accountId, generation) != job) {
    return;
  }
  QString error;
  const QList<OutboxItem> allItems = m_database->outboxItems(1000, &error);
  if (!error.isEmpty()) {
    finish(job, QStringLiteral("database_error"), error);
    return;
  }
  const QDateTime now = QDateTime::currentDateTimeUtc();
  for (const OutboxItem& item : allItems) {
    if (item.accountId != accountId ||
        (item.state != OutboxState::Pending && item.state != OutboxState::RetryWait)) {
      continue;
    }
    QDateTime wakeAt = item.nextAttemptAt;
    if (!wakeAt.isValid() || (item.notBefore.isValid() && item.notBefore > wakeAt)) {
      wakeAt = item.notBefore;
    }
    if (wakeAt.isValid() && wakeAt > now) {
      scheduleRetryWake(accountId, wakeAt);
    }
  }
  const QList<OutboxItem> ready = m_database->readyOutbox(500, &error);
  if (!error.isEmpty()) {
    finish(job, QStringLiteral("database_error"), error);
    return;
  }
  for (const OutboxItem& item : ready) {
    if (item.accountId == job->accountId) {
      job->outbox.append(item);
    }
  }
  job->outboxIndex = 0;
  dispatchNextOutbox(job);
}

void GoogleSync::dispatchNextOutbox(SyncJob* job) {
  if (job == nullptr) {
    return;
  }
  const QString accountId = job->accountId;
  const quint64 generation = job->generation;
  if (activeJob(accountId, generation) != job) {
    return;
  }
  if (job->outboxIndex >= job->outbox.size()) {
    if (!job->outbox.isEmpty()) {
      // Completing one operation may satisfy a durable dependency which was
      // not ready when this batch was selected.
      scheduleRetryWake(accountId, QDateTime::currentDateTimeUtc());
    }
    finish(job);
    return;
  }
  const OutboxItem item = job->outbox.at(job->outboxIndex++);
  const Calendar targetCalendar = m_database->calendar(item.calendarId);
  Calendar calendar = targetCalendar;
  Event event = eventFromJson(item.payload);
  if (event.id.isEmpty()) {
    event.id = item.eventId;
  }
  if (item.operation == OutboxOperation::Move) {
    Event source = moveSourceEvent(item);
    const Event canonical = m_database->event(item.eventId);
    if (!canonical.remoteId.isEmpty()) {
      // A dependent Create or earlier Move may have established a newer
      // provider identity after this payload was queued.
      source.remoteId = canonical.remoteId;
      event.remoteId = canonical.remoteId;
    } else {
      event.remoteId = source.remoteId;
    }
    if (!item.expectedRevision.isEmpty()) {
      source.etag = item.expectedRevision;
      event.etag = item.expectedRevision;
    }
    const QString sourceCalendarId = moveMetadata(item)
                                         .value(QStringLiteral("sourceCalendarId"))
                                         .toString(source.calendarId);
    calendar = m_database->calendar(sourceCalendarId);
  }
  QString error;
  const int attempt = item.attempts + 1;
  if (!m_database->updateOutboxState(item.id, OutboxState::Sending, attempt,
                                     QDateTime::currentDateTimeUtc(), {}, {}, &error)) {
    finish(job, QStringLiteral("database_error"), error);
    return;
  }

  const auto block = [this, job, &item, attempt](const QString& code,
                                                 const QString& message) {
    QString updateError;
    if (!m_database->updateOutboxState(item.id, OutboxState::Blocked, attempt, {}, code,
                                       message, &updateError)) {
      finish(job, QStringLiteral("database_error"), updateError);
      return;
    }
    dispatchNextOutbox(job);
  };
  if (targetCalendar.id.isEmpty() || targetCalendar.remoteId.isEmpty()) {
    block(QStringLiteral("calendar_not_found"),
          QStringLiteral("The destination Google calendar is unavailable"));
    return;
  }
  if (item.operation == OutboxOperation::Move &&
      (calendar.id.isEmpty() || calendar.remoteId.isEmpty() ||
       calendar.accountId != targetCalendar.accountId)) {
    block(QStringLiteral("move_source_unavailable"),
          QStringLiteral("The source Google calendar is unavailable"));
    return;
  }
  if (!isValidGuestNotificationPolicy(item.sendUpdates)) {
    block(QStringLiteral("invalid_send_updates"),
          QStringLiteral("Choose how Google should notify guests"));
    return;
  }
  if (item.recurrenceScope == QStringLiteral("future")) {
    block(QStringLiteral("unsupported_recurrence_scope"),
          QStringLiteral("Google Calendar cannot safely change this and future "
                         "occurrences in one request"));
    return;
  }
  if (item.recurrenceScope != QStringLiteral("series") &&
      item.recurrenceScope != QStringLiteral("occurrence")) {
    block(QStringLiteral("invalid_recurrence_scope"),
          QStringLiteral("The recurrence scope is invalid"));
    return;
  }

  if (item.operation == OutboxOperation::Create) {
    if (item.idempotencyKey.isEmpty()) {
      block(QStringLiteral("client_mutation_id_required"),
            QStringLiteral("A stable client mutation identifier is required"));
      return;
    }
    dispatchMutation(job, item, event, calendar, {});
    return;
  }

  const QString recurringParent = recurringParentRemoteId(event);
  if (item.recurrenceScope == QStringLiteral("series")) {
    if (!recurringParent.isEmpty() && item.operation == OutboxOperation::Update &&
        rsvpPatchForGoogleEvent(event).isEmpty()) {
      block(QStringLiteral("series_master_required"),
            QStringLiteral("Reload the recurring series master before changing "
                           "the entire series"));
      return;
    }
    if (!recurringParent.isEmpty()) {
      resolveSeriesAndDispatch(job, item, event, calendar, recurringParent);
    } else {
      dispatchMutation(job, item, event, calendar, event.remoteId);
    }
    return;
  }

  if (event.recurrenceId.isEmpty()) {
    block(QStringLiteral("occurrence_reference_required"),
          QStringLiteral("This occurrence has no recurrence identifier"));
    return;
  }
  const GoogleOccurrenceMutationTarget occurrenceTarget =
      googleOccurrenceMutationTarget(event);
  if (occurrenceTarget.kind == GoogleOccurrenceMutationTargetKind::DirectInstance) {
    dispatchMutation(job, item, event, calendar, occurrenceTarget.remoteId);
    return;
  }
  if (occurrenceTarget.kind != GoogleOccurrenceMutationTargetKind::ResolveInstance ||
      occurrenceTarget.remoteId.isEmpty()) {
    block(QStringLiteral("series_remote_id_required"),
          QStringLiteral("This recurring event has no Google series identifier"));
    return;
  }
  resolveOccurrenceAndDispatch(job, item, event, calendar, occurrenceTarget.remoteId);
}

void GoogleSync::resolveSeriesAndDispatch(SyncJob* job, const OutboxItem& item,
                                          const Event& event, const Calendar& calendar,
                                          const QString& recurringEventRemoteId) {
  if (job == nullptr) {
    return;
  }
  const QString accountId = job->accountId;
  const quint64 generation = job->generation;
  if (activeJob(accountId, generation) != job) {
    return;
  }
  const QJsonObject localRsvp = rsvpPatchForGoogleEvent(event);
  const QPointer<GoogleSync> guard(this);
  m_client.getEvent(
      accountId, calendar.remoteId, recurringEventRemoteId,
      [this, guard, accountId, generation, item, event, calendar, localRsvp,
       recurringEventRemoteId](const ApiResponse& response) {
        if (guard == nullptr) {
          return;
        }
        SyncJob* active = activeJob(accountId, generation);
        if (active == nullptr) {
          return;
        }
        if (!response.ok) {
          handleMutationResult(active, item, event, response);
          return;
        }
        Event master = eventFromGoogleJson(response.body, event.calendarId,
                                           calendarDefaultReminders(calendar));
        master.id = event.id;
        if (!localRsvp.isEmpty()) {
          const QJsonObject changed =
              localRsvp.value(QStringLiteral("attendees")).toArray().first().toObject();
          const QString changedEmail =
              changed.value(QStringLiteral("email")).toString();
          QJsonArray attendees = master.attendees;
          bool applied = false;
          for (qsizetype index = 0; index < attendees.size(); ++index) {
            QJsonObject attendee = attendees.at(index).toObject();
            if (attendee.value(QStringLiteral("email"))
                    .toString()
                    .compare(changedEmail, Qt::CaseInsensitive) != 0) {
              continue;
            }
            attendee.insert(QStringLiteral("responseStatus"),
                            changed.value(QStringLiteral("responseStatus")));
            attendee.insert(
                QStringLiteral("partstat"),
                changed.value(QStringLiteral("responseStatus")).toString().toUpper());
            attendees.replace(index, attendee);
            applied = true;
            break;
          }
          if (!applied) {
            ApiResponse missingAttendee;
            missingAttendee.errorCode = QStringLiteral("rsvp_attendee_not_found");
            missingAttendee.errorMessage = QStringLiteral(
                "The signed-in attendee is not present on the series master");
            handleMutationResult(active, item, event, missingAttendee);
            return;
          }
          master.attendees = attendees;
        }
        OutboxItem resolvedItem = item;
        resolvedItem.expectedRevision = master.etag;
        dispatchMutation(active, resolvedItem, master, calendar,
                         recurringEventRemoteId);
      });
}

void GoogleSync::resolveOccurrenceAndDispatch(SyncJob* job, const OutboxItem& item,
                                              const Event& event,
                                              const Calendar& calendar,
                                              const QString& recurringEventRemoteId,
                                              const QString& pageToken) {
  if (job == nullptr) {
    return;
  }
  const QString accountId = job->accountId;
  const quint64 generation = job->generation;
  if (activeJob(accountId, generation) != job) {
    return;
  }
  const auto bounds = occurrenceLookupBounds(event);
  if (!bounds.first.isValid() || !bounds.second.isValid()) {
    ApiResponse invalid;
    invalid.errorCode = QStringLiteral("invalid_recurrence_id");
    invalid.errorMessage =
        QStringLiteral("The occurrence recurrence identifier is invalid");
    handleMutationResult(job, item, event, invalid);
    return;
  }

  const QPointer<GoogleSync> guard(this);
  m_client.listEventInstances(
      accountId, calendar.remoteId, recurringEventRemoteId, pageToken, bounds.first,
      bounds.second,
      [this, guard, accountId, generation, item, event, calendar,
       recurringEventRemoteId, pageToken](const ApiResponse& response) {
        if (guard == nullptr) {
          return;
        }
        SyncJob* active = activeJob(accountId, generation);
        if (active == nullptr) {
          return;
        }
        if (!response.ok) {
          handleMutationResult(active, item, event, response);
          return;
        }
        const QJsonArray instances =
            response.body.value(QStringLiteral("items")).toArray();
        for (const QJsonValue& value : instances) {
          const QJsonObject resource = value.toObject();
          const QString instanceRecurrenceId = recurrenceInstanceId(
              resource.value(QStringLiteral("originalStartTime")).toObject());
          if (!recurrenceIdentityEqual(instanceRecurrenceId, event.recurrenceId,
                                       event.allDay, event.timeKind,
                                       event.startTimeZone)) {
            continue;
          }
          const QString remoteId = resource.value(QStringLiteral("id")).toString();
          if (remoteId.isEmpty()) {
            continue;
          }
          Event resolvedEvent = event;
          resolvedEvent.remoteId = remoteId;
          resolvedEvent.etag = resource.value(QStringLiteral("etag")).toString();
          resolvedEvent.rawFormat = QStringLiteral("google-json");
          resolvedEvent.rawPayload =
              QString::fromUtf8(QJsonDocument(resource).toJson(QJsonDocument::Compact));
          OutboxItem resolvedItem = item;
          resolvedItem.expectedRevision = resolvedEvent.etag;
          dispatchMutation(active, resolvedItem, resolvedEvent, calendar, remoteId);
          return;
        }
        const QString nextPage =
            response.body.value(QStringLiteral("nextPageToken")).toString();
        if (!nextPage.isEmpty()) {
          resolveOccurrenceAndDispatch(active, item, event, calendar,
                                       recurringEventRemoteId, nextPage);
          return;
        }
        ApiResponse missing;
        missing.httpStatus = 404;
        missing.notFound = true;
        missing.errorCode = QStringLiteral("occurrence_not_found");
        missing.errorMessage =
            QStringLiteral("Google no longer has this event occurrence");
        handleMutationResult(active, item, event, missing);
      });
}

void GoogleSync::dispatchMutation(SyncJob* job, const OutboxItem& item,
                                  const Event& event, const Calendar& calendar,
                                  const QString& remoteId) {
  if (job == nullptr) {
    return;
  }
  const QString accountId = job->accountId;
  const quint64 generation = job->generation;
  if (activeJob(accountId, generation) != job) {
    return;
  }
  const QPointer<GoogleSync> guard(this);
  const auto callback = [this, guard, accountId, generation, item,
                         event](const ApiResponse& response) {
    if (guard == nullptr) {
      return;
    }
    SyncJob* active = activeJob(accountId, generation);
    if (active == nullptr) {
      return;
    }
    handleMutationResult(active, item, event, response);
  };

  if (item.operation == OutboxOperation::Create) {
    m_client.createEvent(accountId, calendar.remoteId,
                         eventToGoogleCreateJson(event, item.idempotencyKey),
                         item.sendUpdates, callback);
    return;
  }
  if (item.operation == OutboxOperation::Move) {
    const Calendar targetCalendar = m_database->calendar(item.calendarId);
    if (remoteId.isEmpty() || targetCalendar.id.isEmpty() ||
        targetCalendar.remoteId.isEmpty()) {
      ApiResponse invalid;
      invalid.httpStatus = 400;
      invalid.errorCode = QStringLiteral("move_identity_unavailable");
      invalid.errorMessage =
          QStringLiteral("The Google event move has no remote identity");
      callback(invalid);
      return;
    }
    const QPointer<GoogleSync> moveGuard(this);
    m_client.moveEvent(
        accountId, calendar.remoteId, remoteId, targetCalendar.remoteId,
        item.expectedRevision, item.sendUpdates,
        [this, moveGuard, accountId, generation, item, event,
         targetRemoteId = targetCalendar.remoteId, remoteId,
         callback](const ApiResponse& response) mutable {
          if (moveGuard == nullptr) {
            return;
          }
          if (!response.ok) {
            callback(response);
            return;
          }
          const QString movedRemoteId =
              response.body.value(QStringLiteral("id")).toString(remoteId);
          m_client.getEvent(accountId, targetRemoteId, movedRemoteId,
                            [this, moveGuard, accountId, generation, item,
                             event](const ApiResponse& hydration) {
                              if (moveGuard == nullptr) {
                                return;
                              }
                              SyncJob* active = activeJob(accountId, generation);
                              if (active == nullptr) {
                                return;
                              }
                              handleMoveHydration(active, item, event, hydration);
                            });
        });
    return;
  }
  if (item.operation == OutboxOperation::Remove) {
    if (remoteId.isEmpty()) {
      QString error;
      if (!m_database->completeOutbox(item.id, nullptr, &error)) {
        finish(job, QStringLiteral("database_error"), error);
        return;
      }
      dispatchNextOutbox(job);
      return;
    }
    m_client.deleteEvent(accountId, calendar.remoteId, remoteId, item.expectedRevision,
                         item.sendUpdates, callback);
    return;
  }

  const QJsonObject rsvpPatch = rsvpPatchForGoogleEvent(event);
  if (!rsvpPatch.isEmpty()) {
    m_client.respondToEvent(accountId, calendar.remoteId, remoteId,
                            item.expectedRevision, rsvpPatch, item.sendUpdates,
                            callback);
    return;
  }
  QJsonObject payload = eventToGoogleJson(event);
  if (item.recurrenceScope == QStringLiteral("occurrence")) {
    payload.remove(QStringLiteral("recurrence"));
  }
  m_client.updateEvent(accountId, calendar.remoteId, remoteId, item.expectedRevision,
                       payload, item.sendUpdates, callback);
}

void GoogleSync::handleMoveHydration(SyncJob* job, const OutboxItem& item,
                                     const Event& desiredEvent,
                                     const ApiResponse& response) {
  if (job == nullptr) {
    return;
  }
  const QString accountId = job->accountId;
  const quint64 generation = job->generation;
  if (activeJob(accountId, generation) != job) {
    return;
  }
  if (!response.ok) {
    handleMutationResult(job, item, desiredEvent, response);
    return;
  }
  const Calendar targetCalendar = m_database->calendar(item.calendarId);
  Event remote = eventFromGoogleJson(response.body, item.calendarId,
                                     calendarDefaultReminders(targetCalendar));
  remote.id = desiredEvent.id;
  Event desired = desiredEvent;
  desired.remoteId = remote.remoteId;
  desired.etag = remote.etag;
  desired.rawPayload = remote.rawPayload;
  desired.rawFormat = remote.rawFormat;
  QJsonObject desiredPayload = eventToGoogleJson(desired);
  QJsonObject remotePayload = eventToGoogleJson(remote);
  if (item.recurrenceScope == QStringLiteral("occurrence")) {
    desiredPayload.remove(QStringLiteral("recurrence"));
    remotePayload.remove(QStringLiteral("recurrence"));
  }
  if (desiredPayload == remotePayload) {
    handleMutationResult(job, item, desiredEvent, response);
    return;
  }

  const QPointer<GoogleSync> guard(this);
  m_client.updateEvent(accountId, targetCalendar.remoteId, remote.remoteId, remote.etag,
                       desiredPayload, item.sendUpdates,
                       [this, guard, accountId, generation, item,
                        desiredEvent](const ApiResponse& updateResponse) {
                         if (guard == nullptr) {
                           return;
                         }
                         SyncJob* active = activeJob(accountId, generation);
                         if (active == nullptr) {
                           return;
                         }
                         handleMutationResult(active, item, desiredEvent,
                                              updateResponse);
                       });
}

void GoogleSync::handleMutationResult(SyncJob* job, const OutboxItem& item,
                                      const Event& localEvent,
                                      const ApiResponse& response,
                                      const bool allowCreateReconciliation,
                                      const bool allowConflictHydration) {
  if (job == nullptr) {
    return;
  }
  const QString accountId = job->accountId;
  const quint64 generation = job->generation;
  if (activeJob(accountId, generation) != job) {
    return;
  }
  QString error;
  if (response.ok) {
    if (item.operation == OutboxOperation::Remove) {
      if (!m_database->completeOutbox(item.id, nullptr, &error)) {
        finish(job, QStringLiteral("database_error"), error);
        return;
      }
    } else {
      const Calendar calendar = m_database->calendar(item.calendarId);
      Event remote = eventFromGoogleJson(response.body, item.calendarId,
                                         calendarDefaultReminders(calendar));
      remote.id = localEvent.id;
      if (!m_database->completeOutbox(item.id, &remote, &error)) {
        finish(job, QStringLiteral("database_error"), error);
        return;
      }
    }
    if (!job->changedCalendarIds.contains(item.calendarId)) {
      job->changedCalendarIds.append(item.calendarId);
    }
    if (item.operation == OutboxOperation::Move) {
      const QString sourceCalendarId =
          moveMetadata(item).value(QStringLiteral("sourceCalendarId")).toString();
      if (!sourceCalendarId.isEmpty() &&
          !job->changedCalendarIds.contains(sourceCalendarId)) {
        job->changedCalendarIds.append(sourceCalendarId);
      }
    }
    dispatchNextOutbox(job);
    return;
  }

  if (response.authenticationRequired) {
    // A 401 means Google rejected the request before applying the mutation, so
    // every operation (including create) is safe to replay after one refresh.
    // Return it to pending instead of blocking the user's edit.
    if (!m_database->updateOutboxState(item.id, OutboxState::Pending, item.attempts, {},
                                       response.errorCode, response.errorMessage,
                                       &error)) {
      finish(job, QStringLiteral("database_error"), error);
      return;
    }
    if (job->outboxIndex > 0) {
      --job->outboxIndex;
    }
    QString refreshError;
    if (refreshAndResume(
            job, [this](SyncJob* resumed) { dispatchNextOutbox(resumed); },
            &refreshError)) {
      return;
    }
    requireReauthorization(
        job, response.errorCode,
        refreshError.isEmpty() ? response.errorMessage : refreshError);
    return;
  }

  // DELETE is idempotent from the user's perspective. A retry after a lost
  // successful response commonly returns 404 or 410, either of which is
  // already the requested final state.
  if (item.operation == OutboxOperation::Remove &&
      (response.httpStatus == 404 || response.httpStatus == 410)) {
    if (!m_database->completeOutbox(item.id, nullptr, &error)) {
      finish(job, QStringLiteral("database_error"), error);
      return;
    }
    if (!job->changedCalendarIds.contains(item.calendarId)) {
      job->changedCalendarIds.append(item.calendarId);
    }
    dispatchNextOutbox(job);
    return;
  }

  // A native Google move preserves the event id. If the POST was accepted but
  // its reply was lost, replaying it sees a missing source; hydrate the target
  // before deciding whether anything failed.
  if (item.operation == OutboxOperation::Move &&
      (response.httpStatus == 404 || response.httpStatus == 410)) {
    const Calendar targetCalendar = m_database->calendar(item.calendarId);
    const Event canonical = m_database->event(item.eventId);
    const QString remoteId =
        !canonical.remoteId.isEmpty() ? canonical.remoteId : localEvent.remoteId;
    if (targetCalendar.id.isEmpty() || targetCalendar.remoteId.isEmpty() ||
        remoteId.isEmpty()) {
      if (!m_database->recordProviderConflict(item.id, nullptr, &error)) {
        finish(job, QStringLiteral("database_error"), error);
        return;
      }
      dispatchNextOutbox(job);
      return;
    }
    const QPointer<GoogleSync> guard(this);
    m_client.getEvent(
        accountId, targetCalendar.remoteId, remoteId,
        [this, guard, accountId, generation, item,
         localEvent](const ApiResponse& lookupResponse) {
          if (guard == nullptr) {
            return;
          }
          SyncJob* active = activeJob(accountId, generation);
          if (active == nullptr) {
            return;
          }
          if (lookupResponse.ok) {
            handleMoveHydration(active, item, localEvent, lookupResponse);
            return;
          }
          if ((lookupResponse.httpStatus == 404 || lookupResponse.httpStatus == 410) &&
              item.attempts < 3) {
            ApiResponse pending;
            pending.httpStatus = 503;
            pending.retryable = true;
            pending.errorCode = QStringLiteral("move_reconciliation_pending");
            pending.errorMessage =
                QStringLiteral("Waiting for Google to expose the moved event");
            handleMutationResult(active, item, localEvent, pending, false, false);
            return;
          }
          if (lookupResponse.httpStatus == 404 || lookupResponse.httpStatus == 410) {
            QString databaseError;
            if (!m_database->recordProviderConflict(item.id, nullptr, &databaseError)) {
              finish(active, QStringLiteral("database_error"), databaseError);
              return;
            }
            dispatchNextOutbox(active);
            return;
          }
          handleMutationResult(active, item, localEvent, lookupResponse, false, false);
        });
    return;
  }

  if (item.operation == OutboxOperation::Move &&
      (response.httpStatus == 409 || response.httpStatus == 412)) {
    const Event source = moveSourceEvent(item);
    const Calendar sourceCalendar = m_database->calendar(source.calendarId);
    const Event canonical = m_database->event(item.eventId);
    const QString remoteId =
        !canonical.remoteId.isEmpty() ? canonical.remoteId : source.remoteId;
    if (sourceCalendar.id.isEmpty() || sourceCalendar.remoteId.isEmpty() ||
        remoteId.isEmpty()) {
      if (!m_database->recordProviderConflict(item.id, nullptr, &error)) {
        finish(job, QStringLiteral("database_error"), error);
        return;
      }
      dispatchNextOutbox(job);
      return;
    }
    const QPointer<GoogleSync> guard(this);
    m_client.getEvent(
        accountId, sourceCalendar.remoteId, remoteId,
        [this, guard, accountId, generation, item, localEvent,
         sourceCalendar](const ApiResponse& lookupResponse) {
          if (guard == nullptr) {
            return;
          }
          SyncJob* active = activeJob(accountId, generation);
          if (active == nullptr) {
            return;
          }
          if (lookupResponse.httpStatus == 404 || lookupResponse.httpStatus == 410) {
            handleMutationResult(active, item, localEvent, lookupResponse, false,
                                 false);
            return;
          }
          if (!lookupResponse.ok) {
            handleMutationResult(active, item, localEvent, lookupResponse, false,
                                 false);
            return;
          }
          Event remote = eventFromGoogleJson(lookupResponse.body, sourceCalendar.id,
                                             calendarDefaultReminders(sourceCalendar));
          remote.id = localEvent.id;
          QString databaseError;
          if (!m_database->recordProviderConflict(item.id, &remote, &databaseError)) {
            finish(active, QStringLiteral("database_error"), databaseError);
            return;
          }
          dispatchNextOutbox(active);
        });
    return;
  }

  if (allowCreateReconciliation && item.operation == OutboxOperation::Create &&
      response.httpStatus == 409) {
    const Calendar calendar = m_database->calendar(item.calendarId);
    const QPointer<GoogleSync> guard(this);
    m_client.getEvent(
        accountId, calendar.remoteId, eventIdForClientMutation(item.idempotencyKey),
        [this, guard, accountId, generation, item,
         localEvent](const ApiResponse& lookupResponse) {
          if (guard == nullptr) {
            return;
          }
          SyncJob* active = activeJob(accountId, generation);
          if (active == nullptr) {
            return;
          }
          if (lookupResponse.ok && googleEventHasMutationIdentity(
                                       lookupResponse.body, item.idempotencyKey)) {
            handleMutationResult(active, item, localEvent, lookupResponse, false,
                                 false);
            return;
          }
          if (lookupResponse.ok) {
            ApiResponse mismatch;
            mismatch.httpStatus = 409;
            mismatch.conflict = true;
            mismatch.errorCode = QStringLiteral("mutation_identity_conflict");
            mismatch.errorMessage = QStringLiteral(
                "Google already uses the deterministic event identifier for "
                "a different mutation");
            handleMutationResult(active, item, localEvent, mismatch, false, false);
            return;
          }
          if (lookupResponse.httpStatus == 404 || lookupResponse.httpStatus == 410) {
            // Google may briefly return not-found immediately after accepting
            // the insert. Retrying the deterministic id is safe: acceptance
            // yields either the original event or another 409 to reconcile.
            ApiResponse pending;
            pending.httpStatus = 503;
            pending.retryable = true;
            pending.errorCode = QStringLiteral("create_reconciliation_pending");
            pending.errorMessage =
                QStringLiteral("Waiting for Google to expose the accepted event");
            handleMutationResult(active, item, localEvent, pending, false, false);
            return;
          }
          handleMutationResult(active, item, localEvent, lookupResponse, false, false);
        });
    return;
  }

  if (allowConflictHydration && item.operation != OutboxOperation::Create &&
      item.operation != OutboxOperation::Move &&
      (response.httpStatus == 409 || response.httpStatus == 412) &&
      !localEvent.remoteId.isEmpty()) {
    const Calendar calendar = m_database->calendar(item.calendarId);
    const QPointer<GoogleSync> guard(this);
    m_client.getEvent(
        accountId, calendar.remoteId, localEvent.remoteId,
        [this, guard, accountId, generation, item, localEvent,
         response](const ApiResponse& lookupResponse) {
          if (guard == nullptr) {
            return;
          }
          SyncJob* active = activeJob(accountId, generation);
          if (active == nullptr) {
            return;
          }
          if (!lookupResponse.ok) {
            if (lookupResponse.httpStatus == 404 || lookupResponse.httpStatus == 410) {
              QString databaseError;
              bool conflicted = false;
              if (!m_database->removeRemoteEvent(item.calendarId, localEvent.remoteId,
                                                 {}, &databaseError, &conflicted)) {
                finish(active, QStringLiteral("database_error"), databaseError);
                return;
              }
              if (conflicted && !active->changedCalendarIds.contains(item.calendarId)) {
                active->changedCalendarIds.append(item.calendarId);
              }
              ApiResponse deleted = response;
              deleted.httpStatus = 409;
              deleted.conflict = true;
              deleted.errorCode = QStringLiteral("remote_deleted");
              deleted.errorMessage = QStringLiteral(
                  "Google deleted the event while local changes were pending");
              handleMutationResult(active, item, localEvent, deleted, false, false);
              return;
            }
            handleMutationResult(active, item, localEvent, lookupResponse, false,
                                 false);
            return;
          }
          const Calendar hydratedCalendar = m_database->calendar(item.calendarId);
          Event remote =
              eventFromGoogleJson(lookupResponse.body, item.calendarId,
                                  calendarDefaultReminders(hydratedCalendar));
          QString databaseError;
          bool conflicted = false;
          if (!m_database->applyRemoteEvent(remote, &databaseError, &conflicted)) {
            finish(active, QStringLiteral("database_error"), databaseError);
            return;
          }
          if (conflicted && !active->changedCalendarIds.contains(item.calendarId)) {
            active->changedCalendarIds.append(item.calendarId);
          }
          handleMutationResult(active, item, localEvent, response, false, false);
        });
    return;
  }

  if (allowConflictHydration && item.operation == OutboxOperation::Update &&
      (response.httpStatus == 404 || response.httpStatus == 410) &&
      !localEvent.remoteId.isEmpty()) {
    bool conflicted = false;
    if (!m_database->removeRemoteEvent(item.calendarId, localEvent.remoteId, {}, &error,
                                       &conflicted)) {
      finish(job, QStringLiteral("database_error"), error);
      return;
    }
    if (conflicted && !job->changedCalendarIds.contains(item.calendarId)) {
      job->changedCalendarIds.append(item.calendarId);
    }
  }

  const ProviderError providerError = providerErrorForResponse(response);
  const RetryDecision decision = m_retryPolicy.evaluate({
      providerError,
      item.attempts + 1,
      item.operation != OutboxOperation::Create || !item.idempotencyKey.isEmpty(),
      true,
      stableJitterKey(item.idempotencyKey),
  });
  if (decision.action == RetryAction::Retry) {
    const QDateTime next =
        QDateTime::currentDateTimeUtc().addMSecs(decision.delay.count());
    if (!m_database->updateOutboxState(item.id, OutboxState::RetryWait,
                                       item.attempts + 1, next, response.errorCode,
                                       response.errorMessage, &error)) {
      finish(job, QStringLiteral("database_error"), error);
      return;
    }
    scheduleRetryWake(accountId, next);
  } else {
    if (!m_database->updateOutboxState(item.id, OutboxState::Blocked, item.attempts + 1,
                                       {}, response.errorCode, response.errorMessage,
                                       &error)) {
      finish(job, QStringLiteral("database_error"), error);
      return;
    }
    if (decision.action == RetryAction::Reauthenticate) {
      setAccountAuthStatus(accountId, QStringLiteral("reauthorization_required"));
      m_auth.refresh(accountId);
    }
  }
  job = activeJob(accountId, generation);
  if (job != nullptr) {
    dispatchNextOutbox(job);
  }
}

void GoogleSync::finish(SyncJob* job, const QString& errorCode,
                        const QString& errorMessage) {
  if (job == nullptr || m_jobs.value(job->accountId) != job) {
    return;
  }
  const QString accountId = job->accountId;
  const QStringList changed = job->changedCalendarIds;
  const QDateTime now = QDateTime::currentDateTimeUtc();
  const bool reauthorizationRequired =
      !errorCode.isEmpty() && m_database->account(accountId).authStatus ==
                                  QStringLiteral("reauthorization_required");
  const QJsonObject value =
      errorCode.isEmpty()
          ? statusObject(QStringLiteral("idle"), {}, {}, now)
          : statusObject(reauthorizationRequired
                             ? QStringLiteral("reauthorization_required")
                             : QStringLiteral("error"),
                         errorCode, errorMessage);
  m_status.insert(accountId, value);
  m_jobs.remove(accountId);
  delete job;
  if (!changed.isEmpty()) {
    emit eventsChanged(changed);
  }
  emit syncStatusChanged(accountId, value);
  if (errorCode.isEmpty() && m_pendingHydrations.contains(accountId)) {
    QTimer::singleShot(0, this, [this, accountId]() { syncAccount(accountId); });
  }
}

QJsonObject GoogleSync::status(const QString& accountId) const {
  if (!accountId.isEmpty()) {
    return m_status.value(accountId, statusObject(QStringLiteral("idle")));
  }
  QJsonObject accounts;
  for (auto iterator = m_status.constBegin(); iterator != m_status.constEnd();
       ++iterator) {
    accounts.insert(iterator.key(), iterator.value());
  }
  return {{QStringLiteral("accounts"), accounts}};
}

}  // namespace omacalendar::google
