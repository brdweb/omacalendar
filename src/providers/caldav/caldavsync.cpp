#include "providers/caldav/caldavsync.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QUrl>

#include "core/domain.h"
#include "providers/caldav/caldavxml.h"
#include "providers/caldav/icalcodec.h"

namespace omacalendar::caldav {
namespace {

constexpr auto kPasswordKind = "password";

QJsonObject statusObject(const QString& state, const QString& errorCode = {},
                         const QString& message = {}, const QDateTime& lastSync = {}) {
  return {
      {QStringLiteral("state"), state},
      {QStringLiteral("errorCode"), errorCode},
      {QStringLiteral("message"), message},
      {QStringLiteral("lastSyncAt"), isoUtc(lastSync)},
  };
}

quint64 stableJitterKey(const QString& value) {
  const QByteArray digest =
      QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256);
  quint64 result = 0;
  for (qsizetype index = 0; index < qMin<qsizetype>(8, digest.size()); ++index) {
    result = (result << 8U) | static_cast<quint8>(digest.at(index));
  }
  return result;
}

QString remoteResourceHref(const QString& remoteId) {
  const qsizetype fragment = remoteId.indexOf(QLatin1Char('#'));
  return fragment < 0 ? remoteId : remoteId.left(fragment);
}

QUrl eventResourceUrl(const QUrl& calendarUrl, const Event& event) {
  if (!event.remoteId.isEmpty()) {
    QUrl url = calendarUrl.resolved(QUrl(remoteResourceHref(event.remoteId)));
    url.setFragment({});
    return url;
  }
  QString uid = event.uid.isEmpty() ? event.id : event.uid;
  uid.replace(QLatin1Char('/'), QLatin1Char('_'));
  uid.replace(QLatin1Char('\\'), QLatin1Char('_'));
  const QString fileName =
      QString::fromLatin1(QUrl::toPercentEncoding(uid)) + QStringLiteral(".ics");
  QUrl base = calendarUrl;
  QString path = base.path();
  if (!path.endsWith(QLatin1Char('/'))) {
    path += QLatin1Char('/');
  }
  path += fileName;
  base.setPath(path);
  return base;
}

}  // namespace

struct CalDavSync::SyncJob final {
  QString accountId;
  Account account;
  QUrl endpoint;
  QUrl homeUrl;
  QList<Calendar> calendars;
  int calendarIndex = 0;
  Calendar currentCalendar;
  bool fullSync = false;
  bool fallbackAttempted = false;
  QList<OutboxItem> outbox;
  int outboxIndex = 0;
  QStringList changedCalendarIds;
  bool cancelled = false;
};

CalDavSync::CalDavSync(Database* database, QObject* parent)
    : QObject(parent), m_database(database), m_client(this) {
  m_pollTimer.setInterval(5 * 60 * 1000);
  connect(&m_pollTimer, &QTimer::timeout, this, &CalDavSync::syncAll);
  m_pollTimer.start();
}

CalDavSync::~CalDavSync() {
  qDeleteAll(m_jobs);
  m_jobs.clear();
}

QString CalDavSync::createAccount(const QString& endpoint, const QString& username,
                                  const QString& password, const QString& displayName,
                                  QString* errorMessage) {
  const QUrl url(endpoint.trimmed());
  if (!CalDavClient::validateEndpoint(url, errorMessage)) {
    return {};
  }
  if (username.trimmed().isEmpty() || password.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("CalDAV username and password are required");
    }
    return {};
  }
  Account account;
  account.id = newUuid();
  account.provider = ProviderKind::CalDav;
  account.displayName =
      displayName.trimmed().isEmpty() ? username.trimmed() : displayName.trimmed();
  account.principal = username.trimmed();
  account.endpoint = url.toString();
  account.authStatus = QStringLiteral("connecting");
  if (!m_secrets.store(account.id, QString::fromLatin1(kPasswordKind), password,
                       errorMessage)) {
    return {};
  }
  if (!m_database->upsertAccount(account, errorMessage)) {
    m_secrets.remove(account.id, QString::fromLatin1(kPasswordKind));
    return {};
  }
  m_client.setCredentials(account.id, account.principal, password);
  emit accountChanged(account.id);
  syncAccount(account.id);
  return account.id;
}

bool CalDavSync::loadCredentials(const Account& account, QString* errorMessage) {
  const QString password =
      m_secrets.lookup(account.id, QString::fromLatin1(kPasswordKind), errorMessage);
  if (password.isEmpty()) {
    return false;
  }
  m_client.setCredentials(account.id, account.principal, password);
  return true;
}

bool CalDavSync::restoreAccounts(QString* errorMessage) {
  QString databaseError;
  const QList<Account> accounts = m_database->accounts(&databaseError);
  if (!databaseError.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = databaseError;
    }
    return false;
  }
  for (const Account& account : accounts) {
    if (account.provider != ProviderKind::CalDav || !account.enabled) {
      continue;
    }
    QString credentialError;
    if (!loadCredentials(account, &credentialError)) {
      setAccountAuthStatus(account.id, QStringLiteral("reauthorization_required"));
      const QJsonObject value =
          statusObject(QStringLiteral("reauthorization_required"),
                       QStringLiteral("authentication_required"), credentialError);
      m_status.insert(account.id, value);
      emit syncStatusChanged(account.id, value);
      continue;
    }
    syncAccount(account.id);
  }
  return true;
}

bool CalDavSync::disconnectAccount(const QString& accountId,
                                   const bool removeCachedData, QString* errorMessage) {
  if (SyncJob* job = m_jobs.value(accountId, nullptr); job != nullptr) {
    // A QNetworkReply may still own a callback that references this job. Keep
    // the small job object alive until that callback returns, but prevent it
    // from touching an account that is being removed.
    job->cancelled = true;
  }
  m_client.forgetCredentials(accountId);
  QString removalError;
  m_secrets.remove(accountId, QString::fromLatin1(kPasswordKind), &removalError);
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

void CalDavSync::syncAll() {
  QString error;
  const QList<Account> accounts = m_database->accounts(&error);
  if (!error.isEmpty()) {
    return;
  }
  for (const Account& account : accounts) {
    if (account.provider == ProviderKind::CalDav && account.enabled) {
      syncAccount(account.id);
    }
  }
}

void CalDavSync::syncAccount(const QString& accountId) {
  if (m_jobs.contains(accountId)) {
    return;
  }
  QString error;
  const Account account = m_database->account(accountId, &error);
  if (account.id.isEmpty() || account.provider != ProviderKind::CalDav ||
      !account.enabled) {
    return;
  }
  if (!loadCredentials(account, &error)) {
    setAccountAuthStatus(accountId, QStringLiteral("reauthorization_required"));
    const QJsonObject value =
        statusObject(QStringLiteral("reauthorization_required"),
                     QStringLiteral("authentication_required"), error);
    m_status.insert(accountId, value);
    emit syncStatusChanged(accountId, value);
    return;
  }
  auto* job = new SyncJob;
  job->accountId = accountId;
  job->account = account;
  job->endpoint = QUrl(account.endpoint);
  m_jobs.insert(accountId, job);
  const QJsonObject value = statusObject(QStringLiteral("syncing"));
  m_status.insert(accountId, value);
  emit syncStatusChanged(accountId, value);
  discoverPrincipal(job);
}

void CalDavSync::discoverPrincipal(SyncJob* job) {
  m_client.discoverPrincipal(
      job->accountId, job->endpoint, [this, job](const DavResponse& response) {
        if (job->cancelled) {
          finish(job);
          return;
        }
        if (!response.ok) {
          finish(job, response.errorCode, response.errorMessage);
          return;
        }
        const CalDavMultiStatusResult parsed =
            CalDavXml::parseMultiStatus(response.body);
        if (!parsed.ok()) {
          finish(job, parsed.error.code, parsed.error.message);
          return;
        }
        const QString href = CalDavXml::principalHref(parsed);
        if (href.isEmpty()) {
          finish(job, QStringLiteral("discovery_failed"),
                 QStringLiteral("CalDAV server did not report a principal URL"));
          return;
        }
        discoverHome(job, job->endpoint.resolved(QUrl(href)));
      });
}

void CalDavSync::discoverHome(SyncJob* job, const QUrl& principalUrl) {
  m_client.discoverHome(
      job->accountId, principalUrl,
      [this, job, principalUrl](const DavResponse& response) {
        if (job->cancelled) {
          finish(job);
          return;
        }
        if (!response.ok) {
          finish(job, response.errorCode, response.errorMessage);
          return;
        }
        const CalDavMultiStatusResult parsed =
            CalDavXml::parseMultiStatus(response.body);
        if (!parsed.ok()) {
          finish(job, parsed.error.code, parsed.error.message);
          return;
        }
        const QString href = CalDavXml::calendarHomeSetHref(parsed);
        if (href.isEmpty()) {
          finish(job, QStringLiteral("discovery_failed"),
                 QStringLiteral("CalDAV server did not report a calendar home"));
          return;
        }
        discoverCollections(job, principalUrl.resolved(QUrl(href)));
      });
}

void CalDavSync::discoverCollections(SyncJob* job, const QUrl& homeUrl) {
  job->homeUrl = homeUrl;
  m_client.discoverCalendars(
      job->accountId, homeUrl, [this, job](const DavResponse& response) {
        if (job->cancelled) {
          finish(job);
          return;
        }
        if (!response.ok) {
          finish(job, response.errorCode, response.errorMessage);
          return;
        }
        const CalDavMultiStatusResult parsed =
            CalDavXml::parseMultiStatus(response.body);
        if (!parsed.ok()) {
          finish(job, parsed.error.code, parsed.error.message);
          return;
        }
        for (const CalDavCollection& remote : CalDavXml::collections(parsed)) {
          Calendar calendar;
          calendar.accountId = job->accountId;
          calendar.remoteId = remote.href;
          calendar.href = remote.href;
          calendar.name = remote.displayName.isEmpty() ? QStringLiteral("Calendar")
                                                       : remote.displayName;
          calendar.description = remote.description;
          if (!remote.color.isEmpty()) {
            calendar.color = remote.color.left(7);
          }
          calendar.readOnly = remote.readOnly;
          calendar.capabilities = {
              {QStringLiteral("provider"), QStringLiteral("caldav")},
              {QStringLiteral("ctag"), remote.ctag},
              {QStringLiteral("serverSyncToken"), remote.syncToken},
              {QStringLiteral("incrementalSync"), !remote.syncToken.isEmpty()},
          };
          const Calendar existing =
              m_database->calendarByRemoteId(job->accountId, remote.href);
          if (!existing.id.isEmpty()) {
            calendar.id = existing.id;
            calendar.syncToken = existing.syncToken;
            calendar.enabled = existing.enabled;
            calendar.lastSyncAt = existing.lastSyncAt;
          } else {
            calendar.id = newUuid();
          }
          QString error;
          if (!m_database->upsertCalendar(calendar, &error)) {
            finish(job, QStringLiteral("database_error"), error);
            return;
          }
        }
        job->calendars = m_database->calendars(job->accountId);
        job->calendarIndex = 0;
        setAccountAuthStatus(job->accountId, QStringLiteral("connected"));
        emit accountChanged(job->accountId);
        emit calendarsChanged(job->accountId);
        syncNextCalendar(job);
      });
}

void CalDavSync::syncNextCalendar(SyncJob* job) {
  if (job != nullptr && job->cancelled) {
    finish(job);
    return;
  }
  while (job != nullptr && job->calendarIndex < job->calendars.size()) {
    job->currentCalendar = job->calendars.at(job->calendarIndex++);
    if (!job->currentCalendar.enabled) {
      continue;
    }
    job->fallbackAttempted = false;
    const QUrl calendarUrl = job->homeUrl.resolved(QUrl(job->currentCalendar.href));
    if (!job->currentCalendar.syncToken.isEmpty()) {
      job->fullSync = false;
      m_client.syncCollection(
          job->accountId, calendarUrl, job->currentCalendar.syncToken,
          [this, job](const DavResponse& response) {
            if (job->cancelled) {
              finish(job);
              return;
            }
            if (!response.ok && !job->fallbackAttempted &&
                (response.httpStatus == 403 || response.httpStatus == 409 ||
                 response.httpStatus == 410 || response.httpStatus == 501)) {
              job->fallbackAttempted = true;
              job->fullSync = true;
              job->currentCalendar.syncToken.clear();
              const QUrl url = job->homeUrl.resolved(QUrl(job->currentCalendar.href));
              m_client.queryCalendar(job->accountId, url,
                                     QDateTime::currentDateTimeUtc().addYears(-2),
                                     QDateTime::currentDateTimeUtc().addYears(5),
                                     [this, job](const DavResponse& fullResponse) {
                                       if (job->cancelled) {
                                         finish(job);
                                         return;
                                       }
                                       applyCalendarResponse(job, fullResponse, true);
                                     });
              return;
            }
            applyCalendarResponse(job, response, false);
          });
      return;
    }
    job->fullSync = true;
    m_client.queryCalendar(job->accountId, calendarUrl,
                           QDateTime::currentDateTimeUtc().addYears(-2),
                           QDateTime::currentDateTimeUtc().addYears(5),
                           [this, job](const DavResponse& response) {
                             if (job->cancelled) {
                               finish(job);
                               return;
                             }
                             applyCalendarResponse(job, response, true);
                           });
    return;
  }
  if (job != nullptr) {
    drainOutbox(job);
  }
}

void CalDavSync::applyCalendarResponse(SyncJob* job, const DavResponse& response,
                                       const bool fullSync) {
  if (job == nullptr || job->cancelled) {
    finish(job);
    return;
  }
  if (!response.ok) {
    finish(job, response.errorCode, response.errorMessage);
    return;
  }
  const CalDavMultiStatusResult parsed = CalDavXml::parseMultiStatus(response.body);
  if (!parsed.ok()) {
    finish(job, parsed.error.code, parsed.error.message);
    return;
  }
  if (fullSync) {
    // Rebuild only after a complete, parseable response is available. This
    // preserves the offline cache when the server cannot be reached and also
    // removes resources deleted on servers that do not support sync-token.
    QString error;
    if (!m_database->clearCleanRemoteEvents(job->currentCalendar.id, &error)) {
      finish(job, QStringLiteral("database_error"), error);
      return;
    }
  }
  for (const CalDavResource& resource : CalDavXml::resources(parsed)) {
    QString error;
    bool conflict = false;
    if (!job->changedCalendarIds.contains(job->currentCalendar.id)) {
      job->changedCalendarIds.append(job->currentCalendar.id);
    }
    if (resource.deleted()) {
      if (!m_database->removeRemoteEvent(job->currentCalendar.id, resource.href, {},
                                         &error, &conflict)) {
        finish(job, QStringLiteral("database_error"), error);
        return;
      }
      continue;
    }
    if (resource.calendarData.isEmpty()) {
      continue;
    }
    const ICalendarParseResult decoded =
        ICalendarCodec::parse(resource.calendarData.toUtf8());
    if (!decoded.ok()) {
      finish(job, decoded.error.code, decoded.error.message);
      return;
    }
    for (Event event : decoded.events) {
      event.calendarId = job->currentCalendar.id;
      event.remoteId = resource.href;
      if (!event.recurrenceId.isEmpty()) {
        event.remoteId += QLatin1Char('#') + event.recurrenceId;
      }
      event.etag = resource.etag;
      if (!m_database->applyRemoteEvent(event, &error, &conflict)) {
        finish(job, QStringLiteral("database_error"), error);
        return;
      }
    }
  }
  const QString newSyncToken =
      !parsed.syncToken.isEmpty() ? parsed.syncToken
      : fullSync
          ? job->currentCalendar.capabilities.value(QStringLiteral("serverSyncToken"))
                .toString()
          : QString();
  if (!newSyncToken.isEmpty()) {
    job->currentCalendar.syncToken = newSyncToken;
  }
  job->currentCalendar.lastSyncAt = QDateTime::currentDateTimeUtc();
  QString error;
  if (!m_database->upsertCalendar(job->currentCalendar, &error)) {
    finish(job, QStringLiteral("database_error"), error);
    return;
  }
  syncNextCalendar(job);
}

void CalDavSync::drainOutbox(SyncJob* job) {
  if (job == nullptr || job->cancelled) {
    finish(job);
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

void CalDavSync::dispatchNextOutbox(SyncJob* job) {
  if (job == nullptr || job->cancelled) {
    finish(job);
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
  const QUrl calendarUrl = job->homeUrl.resolved(QUrl(calendar.href));
  const QUrl resourceUrl = eventResourceUrl(calendarUrl, event);
  QString error;
  const ICalendarSerializeResult encoded = ICalendarCodec::serialize(event);
  if (item.operation != OutboxOperation::Remove && !encoded.ok()) {
    m_database->updateOutboxState(item.id, OutboxState::Blocked, item.attempts + 1, {},
                                  encoded.error.code, encoded.error.message);
    dispatchNextOutbox(job);
    return;
  }
  if (!m_database->updateOutboxState(item.id, OutboxState::Sending, item.attempts + 1,
                                     QDateTime::currentDateTimeUtc(), {}, {}, &error)) {
    finish(job, QStringLiteral("database_error"), error);
    return;
  }
  const auto callback = [this, job, item, event,
                         resourceUrl](const DavResponse& response) {
    if (job->cancelled) {
      finish(job);
      return;
    }
    handleMutationResult(job, item, event, resourceUrl, response);
  };
  if (item.operation == OutboxOperation::Create) {
    m_client.createEvent(job->accountId, resourceUrl, encoded.payload, callback);
  } else if (item.operation == OutboxOperation::Update) {
    m_client.updateEvent(job->accountId, resourceUrl, item.expectedRevision,
                         encoded.payload, callback);
  } else if (event.remoteId.isEmpty()) {
    m_database->completeOutbox(item.id, nullptr, &error);
    dispatchNextOutbox(job);
  } else {
    m_client.deleteEvent(job->accountId, resourceUrl, item.expectedRevision, callback);
  }
}

void CalDavSync::handleMutationResult(SyncJob* job, const OutboxItem& item,
                                      const Event& localEvent, const QUrl& resourceUrl,
                                      const DavResponse& response) {
  if (job == nullptr || job->cancelled) {
    finish(job);
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
      Event acknowledged = localEvent;
      acknowledged.remoteId = resourceUrl.toString();
      acknowledged.etag = response.etag;
      acknowledged.rawPayload =
          QString::fromUtf8(ICalendarCodec::serialize(acknowledged).payload);
      acknowledged.rawFormat = QStringLiteral("text/calendar");
      if (!m_database->completeOutbox(item.id, &acknowledged, &error)) {
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
    m_database->updateOutboxState(
        item.id, OutboxState::RetryWait, item.attempts + 1,
        QDateTime::currentDateTimeUtc().addMSecs(decision.delay.count()),
        response.errorCode, response.errorMessage);
  } else {
    m_database->updateOutboxState(item.id, OutboxState::Blocked, item.attempts + 1, {},
                                  response.errorCode, response.errorMessage);
    if (decision.action == RetryAction::Reauthenticate) {
      setAccountAuthStatus(job->accountId, QStringLiteral("reauthorization_required"));
    }
  }
  dispatchNextOutbox(job);
}

void CalDavSync::finish(SyncJob* job, const QString& errorCode,
                        const QString& errorMessage) {
  if (job == nullptr || m_jobs.value(job->accountId) != job) {
    return;
  }
  const QString accountId = job->accountId;
  const QStringList changed = job->changedCalendarIds;
  const bool cancelled = job->cancelled;
  m_jobs.remove(accountId);
  delete job;
  if (cancelled) {
    return;
  }
  const QJsonObject value =
      errorCode.isEmpty() ? statusObject(QStringLiteral("idle"), {}, {},
                                         QDateTime::currentDateTimeUtc())
      : errorCode == QStringLiteral("http_401") ||
              errorCode == QStringLiteral("authentication_required")
          ? statusObject(QStringLiteral("reauthorization_required"), errorCode,
                         errorMessage)
          : statusObject(QStringLiteral("error"), errorCode, errorMessage);
  if (value.value(QStringLiteral("state")).toString() ==
      QStringLiteral("reauthorization_required")) {
    setAccountAuthStatus(accountId, QStringLiteral("reauthorization_required"));
    emit accountChanged(accountId);
  }
  m_status.insert(accountId, value);
  if (!changed.isEmpty()) {
    emit eventsChanged(changed);
  }
  emit syncStatusChanged(accountId, value);
}

void CalDavSync::setAccountAuthStatus(const QString& accountId, const QString& status) {
  Account account = m_database->account(accountId);
  if (account.id.isEmpty()) {
    return;
  }
  account.authStatus = status;
  m_database->upsertAccount(account);
}

QJsonObject CalDavSync::status(const QString& accountId) const {
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

}  // namespace omacalendar::caldav
