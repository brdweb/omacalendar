#include "providers/google/googlesync.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <utility>

#include "core/domain.h"
#include "providers/google/googlemapper.h"
#include "providers/google/googleoauthconfig.h"

namespace omacalendar::google {
namespace {

constexpr auto kClientIdSetting = "google.oauth.clientId";
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

QJsonObject statusObject(const QString& state, const QString& errorCode = {},
                         const QString& message = {}, const QDateTime& lastSync = {}) {
  return {
      {QStringLiteral("state"), state},
      {QStringLiteral("errorCode"), errorCode},
      {QStringLiteral("message"), message},
      {QStringLiteral("lastSyncAt"), isoUtc(lastSync)},
  };
}

}  // namespace

struct GoogleSync::SyncJob final {
  QString accountId;
  quint64 generation = 0;
  Account account;
  QString calendarListSyncToken;
  bool calendarListResetAttempted = false;
  QList<Calendar> calendars;
  int calendarIndex = 0;
  Calendar currentCalendar;
  bool eventResetAttempted = false;
  QList<OutboxItem> outbox;
  int outboxIndex = 0;
  QStringList changedCalendarIds;
  bool tokenRefreshAttempted = false;
  bool waitingForTokenRefresh = false;
  std::function<void(SyncJob*)> resumeAfterTokenRefresh;
};

GoogleSync::GoogleSync(Database* database, QObject* parent)
    : QObject(parent), m_database(database), m_auth(this), m_client(&m_auth, this) {
  m_pollTimer.setInterval(5 * 60 * 1000);
  connect(&m_pollTimer, &QTimer::timeout, this, &GoogleSync::syncAll);
  m_pollTimer.start();

  connect(&m_auth, &GoogleAuthManager::authorizationUrlReady, this,
          &GoogleSync::authorizationUrlReady);
  connect(&m_auth, &GoogleAuthManager::authorized, this,
          [this](const QString& accountId) {
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
  connect(
      &m_auth, &GoogleAuthManager::authorizationFailed, this,
      [this](const QString& accountId, const QString& code, const QString& message) {
        if (SyncJob* job = m_jobs.value(accountId, nullptr); job != nullptr) {
          requireReauthorization(job, code, message);
          return;
        }
        setAccountAuthStatus(accountId, QStringLiteral("error"));
        const QJsonObject value = statusObject(QStringLiteral("error"), code, message);
        m_status.insert(accountId, value);
        emit accountChanged(accountId);
        emit syncStatusChanged(accountId, value);
      });
}

GoogleSync::~GoogleSync() {
  m_shuttingDown = true;
  m_pollTimer.stop();
  disconnect(&m_auth, nullptr, this, nullptr);
  disconnect(&m_pollTimer, nullptr, this, nullptr);
  qDeleteAll(m_jobs);
  m_jobs.clear();
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
  if (!m_auth.startAuthorization(account.id, errorMessage)) {
    m_database->removeAccount(account.id);
    return {};
  }
  emit accountChanged(account.id);
  return account.id;
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
  for (const Account& account : accounts) {
    if (account.provider != ProviderKind::Google || !account.enabled) {
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
  m_auth.cancel(accountId);
  const Account account = m_database->account(accountId);
  if (!account.id.isEmpty() && account.authStatus == QStringLiteral("authorizing")) {
    m_database->removeAccount(accountId);
    emit accountChanged(accountId);
  }
}

bool GoogleSync::disconnectAccount(const QString& accountId,
                                   const bool removeCachedData, QString* errorMessage) {
  if (!m_auth.forget(accountId, errorMessage)) {
    return false;
  }
  if (SyncJob* job = m_jobs.take(accountId); job != nullptr) {
    delete job;
  }
  if (removeCachedData) {
    if (!m_database->removeAccount(accountId, errorMessage)) {
      return false;
    }
  } else {
    Account account = m_database->account(accountId, errorMessage);
    if (account.id.isEmpty()) {
      return false;
    }
    account.enabled = false;
    account.authStatus = QStringLiteral("disconnected");
    if (!m_database->upsertAccount(account, errorMessage)) {
      return false;
    }
  }
  m_status.remove(accountId);
  emit accountChanged(accountId);
  return true;
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
      account.authStatus == QStringLiteral("reauthorization_required")) {
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
            m_database->clearProviderState(currentJob->accountId);
            startCalendarList(currentJob);
            return;
          }
          finish(currentJob, response.errorCode, response.errorMessage);
          return;
        }
        const QJsonArray items = response.body.value(QStringLiteral("items")).toArray();
        for (const QJsonValue& value : items) {
          if (!value.isObject()) {
            continue;
          }
          Calendar calendar =
              calendarFromGoogleJson(value.toObject(), currentJob->accountId);
          if (calendar.remoteId.isEmpty()) {
            continue;
          }
          const Calendar existing =
              m_database->calendarByRemoteId(currentJob->accountId, calendar.remoteId);
          if (!existing.id.isEmpty()) {
            calendar.id = existing.id;
            calendar.syncToken = existing.syncToken;
            calendar.lastSyncAt = existing.lastSyncAt;
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
          m_database->setProviderState(currentJob->accountId, {},
                                       QString::fromLatin1(kCalendarListToken),
                                       nextSync);
        }
        currentJob->calendars = m_database->calendars(currentJob->accountId);
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
      job->eventResetAttempted = false;
      startEventPage(job);
      return;
    }
  }
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
  const QPointer<GoogleSync> guard(this);
  m_client.listEvents(
      accountId, calendar.remoteId, pageToken, calendar.syncToken,
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
          if (response.httpStatus == 410 &&
              !currentJob->currentCalendar.syncToken.isEmpty() &&
              !currentJob->eventResetAttempted) {
            currentJob->eventResetAttempted = true;
            currentJob->currentCalendar.syncToken.clear();
            QString error;
            if (!m_database->clearCleanRemoteEvents(currentJob->currentCalendar.id,
                                                    &error) ||
                !m_database->upsertCalendar(currentJob->currentCalendar, &error)) {
              finish(currentJob, QStringLiteral("database_error"), error);
              return;
            }
            startEventPage(currentJob);
            return;
          }
          finish(currentJob, response.errorCode, response.errorMessage);
          return;
        }
        const QJsonArray items = response.body.value(QStringLiteral("items")).toArray();
        for (const QJsonValue& value : items) {
          if (!value.isObject()) {
            continue;
          }
          const QJsonObject raw = value.toObject();
          Event event = eventFromGoogleJson(raw, currentJob->currentCalendar.id);
          QString error;
          bool conflict = false;
          // A cancelled recurring instance is an exception tombstone, not a
          // deletion of the series. Persist it so recurrence expansion can
          // suppress that occurrence. Ordinary/series cancellations remove
          // the matching remote record as before.
          const bool applied =
              event.deleted && event.recurrenceId.isEmpty()
                  ? m_database->removeRemoteEvent(currentJob->currentCalendar.id,
                                                  event.remoteId, event.rawPayload,
                                                  &error, &conflict)
                  : m_database->applyRemoteEvent(event, &error, &conflict);
          if (!applied) {
            finish(currentJob, QStringLiteral("database_error"), error);
            return;
          }
          if (!currentJob->changedCalendarIds.contains(
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
        if (!nextSync.isEmpty()) {
          currentJob->currentCalendar.syncToken = nextSync;
        }
        currentJob->currentCalendar.lastSyncAt = QDateTime::currentDateTimeUtc();
        QString error;
        if (!m_database->upsertCalendar(currentJob->currentCalendar, &error)) {
          finish(currentJob, QStringLiteral("database_error"), error);
          return;
        }
        syncNextCalendar(currentJob);
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
    finish(job);
    return;
  }
  const OutboxItem item = job->outbox.at(job->outboxIndex++);
  const Calendar calendar = m_database->calendar(item.calendarId);
  Event event = eventFromJson(item.payload);
  if (event.id.isEmpty()) {
    event.id = item.eventId;
  }
  QString error;
  const int attempt = item.attempts + 1;
  if (!m_database->updateOutboxState(item.id, OutboxState::Sending, attempt,
                                     QDateTime::currentDateTimeUtc(), {}, {}, &error)) {
    finish(job, QStringLiteral("database_error"), error);
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
    m_client.createEvent(accountId, calendar.remoteId, eventToGoogleJson(event),
                         callback);
  } else if (item.operation == OutboxOperation::Update) {
    m_client.updateEvent(accountId, calendar.remoteId, event.remoteId,
                         item.expectedRevision, eventToGoogleJson(event), callback);
  } else if (event.remoteId.isEmpty()) {
    m_database->completeOutbox(item.id, nullptr, &error);
    dispatchNextOutbox(job);
  } else {
    m_client.deleteEvent(accountId, calendar.remoteId, event.remoteId,
                         item.expectedRevision, callback);
  }
}

void GoogleSync::handleMutationResult(SyncJob* job, const OutboxItem& item,
                                      const Event& localEvent,
                                      const ApiResponse& response) {
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
      Event remote = eventFromGoogleJson(response.body, item.calendarId);
      remote.id = localEvent.id;
      if (!m_database->completeOutbox(item.id, &remote, &error)) {
        finish(job, QStringLiteral("database_error"), error);
        return;
      }
    }
    if (!job->changedCalendarIds.contains(item.calendarId)) {
      job->changedCalendarIds.append(item.calendarId);
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

  ProviderError providerError;
  providerError.code = response.errorCode;
  providerError.message = response.errorMessage;
  providerError.httpStatus = response.httpStatus;
  providerError.retryAfterMs =
      response.retryAfterSeconds > 0 ? response.retryAfterSeconds * 1000LL : -1;
  if (response.authenticationRequired) {
    providerError.kind = ProviderErrorKind::Authentication;
  } else if (response.httpStatus == 409 || response.httpStatus == 412) {
    providerError.kind = ProviderErrorKind::Conflict;
  } else if (response.httpStatus == 403) {
    providerError.kind = ProviderErrorKind::Permission;
  } else if (response.retryable) {
    providerError.kind = ProviderErrorKind::ServiceUnavailable;
  } else {
    providerError.kind = ProviderErrorKind::InvalidRequest;
  }
  const RetryDecision decision = m_retryPolicy.evaluate({
      providerError,
      item.attempts + 1,
      item.operation != OutboxOperation::Create,
      true,
      stableJitterKey(item.idempotencyKey),
  });
  if (decision.action == RetryAction::Retry) {
    const QDateTime next =
        QDateTime::currentDateTimeUtc().addMSecs(decision.delay.count());
    m_database->updateOutboxState(item.id, OutboxState::RetryWait, item.attempts + 1,
                                  next, response.errorCode, response.errorMessage);
  } else {
    m_database->updateOutboxState(item.id, OutboxState::Blocked, item.attempts + 1, {},
                                  response.errorCode, response.errorMessage);
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
