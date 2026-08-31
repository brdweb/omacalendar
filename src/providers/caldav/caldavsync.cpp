#include "providers/caldav/caldavsync.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QSet>
#include <QTimeZone>
#include <QUrl>
#include <algorithm>
#include <limits>
#include <memory>

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

QPair<QDateTime, QDateTime> initialCoverageWindow(const QDateTime& nowUtc) {
  const int year = nowUtc.toUTC().date().year();
  return {QDateTime(QDate(year - 2, 1, 1), QTime(0, 0), QTimeZone::UTC),
          QDateTime(QDate(year + 6, 1, 1), QTime(0, 0), QTimeZone::UTC)};
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

bool sameRecurrenceIdentity(const Event& first, const Event& second) {
  return recurrenceIdentityEqual(first, second);
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

QJsonObject moveMetadata(const OutboxItem& item) {
  return item.payload.value(QStringLiteral("_move")).toObject();
}

Event moveSourceEvent(const OutboxItem& item) {
  return eventFromJson(
      moveMetadata(item).value(QStringLiteral("sourceEvent")).toObject());
}

QUrl moveTargetResourceUrl(const QUrl& targetCalendarUrl, const QUrl& sourceResourceUrl,
                           const Event& targetEvent) {
  QString fileName = sourceResourceUrl.fileName();
  if (fileName.isEmpty()) {
    Event identity = targetEvent;
    identity.remoteId.clear();
    return eventResourceUrl(targetCalendarUrl, identity);
  }
  QUrl target = targetCalendarUrl;
  QString path = target.path();
  if (!path.endsWith(QLatin1Char('/'))) {
    path += QLatin1Char('/');
  }
  path += fileName;
  target.setPath(path);
  target.setFragment({});
  return target;
}

bool moveChangesEvent(const Event& source, const Event& target) {
  QJsonObject sourceJson = toJson(source);
  QJsonObject targetJson = toJson(target);
  const QStringList transportKeys = {
      QStringLiteral("calendarId"), QStringLiteral("dirty"),
      QStringLiteral("deleted"),    QStringLiteral("localRevision"),
      QStringLiteral("syncState"),  QStringLiteral("createdAt"),
      QStringLiteral("updatedAt"),
  };
  for (const QString& key : transportKeys) {
    sourceJson.remove(key);
    targetJson.remove(key);
  }
  return sourceJson != targetJson;
}

DavResponse resourcePayload(const DavResponse& response, const QUrl& calendarUrl,
                            const QUrl& resourceUrl) {
  if (!response.ok) {
    return response;
  }
  const CalDavMultiStatusResult parsed = CalDavXml::parseMultiStatus(response.body);
  if (!parsed.ok()) {
    DavResponse invalid;
    invalid.httpStatus = 502;
    invalid.errorCode = parsed.error.code;
    invalid.errorMessage = parsed.error.message;
    return invalid;
  }
  const QString expected =
      CalDavClient::canonicalResourceId(calendarUrl, resourceUrl.toString());
  for (const CalDavResource& resource : CalDavXml::resources(parsed)) {
    if (CalDavClient::canonicalResourceId(calendarUrl, resource.href) != expected) {
      continue;
    }
    if (resource.deleted() || resource.calendarData.isEmpty()) {
      DavResponse missing;
      missing.httpStatus = resource.statusCode > 0 ? resource.statusCode : 404;
      missing.errorCode = QStringLiteral("resource_not_found");
      missing.errorMessage = QStringLiteral("The CalDAV resource was not found");
      return missing;
    }
    DavResponse result;
    result.ok = true;
    result.httpStatus = 200;
    result.etag = resource.etag;
    result.body = resource.calendarData.toUtf8();
    return result;
  }
  DavResponse missing;
  missing.httpStatus = 404;
  missing.errorCode = QStringLiteral("resource_not_found");
  missing.errorMessage = QStringLiteral("The CalDAV resource was not found");
  return missing;
}

QString schedulingIdentity(QString value) {
  value = value.trimmed();
  if (value.startsWith(QStringLiteral("mailto:"), Qt::CaseInsensitive)) {
    value.remove(0, 7);
  }
  return value.toCaseFolded();
}

void markSchedulingIdentity(Event* event, const QSet<QString>& identities) {
  if (event == nullptr || identities.isEmpty()) {
    return;
  }
  if (!event->organizer.isEmpty()) {
    QJsonObject organizer = event->organizer;
    const QString identity = schedulingIdentity(
        organizer.value(QStringLiteral("email"))
            .toString(organizer.value(QStringLiteral("uri")).toString()));
    if (identities.contains(identity)) {
      organizer.insert(QStringLiteral("self"), true);
      event->organizer = organizer;
    }
  }
  for (qsizetype index = 0; index < event->attendees.size(); ++index) {
    QJsonObject attendee = event->attendees.at(index).toObject();
    const QString identity = schedulingIdentity(
        attendee.value(QStringLiteral("email"))
            .toString(attendee.value(QStringLiteral("uri")).toString()));
    if (identities.contains(identity)) {
      attendee.insert(QStringLiteral("self"), true);
      event->attendees.replace(index, attendee);
    }
  }
}

QJsonArray canonicalAttendees(QJsonArray attendees) {
  for (qsizetype index = 0; index < attendees.size(); ++index) {
    QJsonObject attendee = attendees.at(index).toObject();
    attendee.remove(QStringLiteral("self"));
    attendee.remove(QStringLiteral("organizer"));
    attendees.replace(index, attendee);
  }
  return attendees;
}

bool attendeeMutation(const Event& event) {
  if (event.rawPayload.isEmpty()) {
    return !event.attendees.isEmpty();
  }
  const ICalendarParseResult parsed = ICalendarCodec::parse(event.rawPayload.toUtf8());
  if (!parsed.ok()) {
    return !event.attendees.isEmpty();
  }
  for (const Event& retained : parsed.events) {
    if (retained.uid == event.uid && recurrenceIdentityEqual(retained, event)) {
      return canonicalAttendees(retained.attendees) !=
             canonicalAttendees(event.attendees);
    }
  }
  return !event.attendees.isEmpty();
}

QByteArray futureCapabilityProbePayload(const QString& uid, const bool ranged) {
  const QByteArray recurrence =
      ranged ? QByteArrayLiteral("RECURRENCE-ID;RANGE=THISANDFUTURE:19700102T090000Z")
             : QByteArrayLiteral("RECURRENCE-ID:19700102T090000Z");
  return QByteArrayLiteral(
             "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//OmaCalendar//Capability "
             "Probe//EN\r\n"
             "BEGIN:VEVENT\r\nUID:") +
         uid.toUtf8() +
         QByteArrayLiteral(
             "\r\nDTSTAMP:19700101T000000Z\r\n"
             "DTSTART:19700101T090000Z\r\nDTEND:19700101T100000Z\r\n"
             "RRULE:FREQ=DAILY;COUNT=2\r\nSUMMARY:OmaCalendar capability probe\r\n"
             "STATUS:CANCELLED\r\nTRANSP:TRANSPARENT\r\nEND:VEVENT\r\n"
             "BEGIN:VEVENT\r\nUID:") +
         uid.toUtf8() + QByteArrayLiteral("\r\n") + recurrence +
         QByteArrayLiteral(
             "\r\nDTSTAMP:19700101T000000Z\r\nDTSTART:19700102T090000Z\r\n"
             "DTEND:19700102T100000Z\r\nSUMMARY:OmaCalendar capability probe\r\n"
             "STATUS:CANCELLED\r\nTRANSP:TRANSPARENT\r\nEND:VEVENT\r\nEND:"
             "VCALENDAR\r\n");
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
  bool hydrationSync = false;
  bool replaceCoverage = false;
  RangeSyncRequest hydrationRequest;
  bool fallbackAttempted = false;
  QDateTime queryStartUtc;
  QDateTime queryEndUtc;
  QList<OutboxItem> outbox;
  int outboxIndex = 0;
  QStringList changedCalendarIds;
  bool schedulingSupported = false;
  QSet<QString> schedulingIdentities;
  bool futureProbeInProgress = false;
  bool cancelled = false;
};

struct CalDavSync::FutureCapabilityProbe final {
  OutboxItem item;
  Calendar calendar;
  QUrl calendarUrl;
  QUrl resourceUrl;
  QString uid;
  QByteArray updatePayload;
  QString etag;
  bool created = false;
  bool creationAttempted = false;
};

CalDavSync::CalDavSync(Database* database, QObject* parent)
    : Provider(QStringLiteral("caldav"), ProviderKind::CalDav, parent),
      m_database(database),
      m_client(this) {
  m_pollTimer.setInterval(5 * 60 * 1000);
  connect(&m_pollTimer, &QTimer::timeout, this, &CalDavSync::syncAll);
  m_pollTimer.start();
}

ProviderCapabilities CalDavSync::capabilities() const {
  ProviderCapabilities value;
  value.incrementalSync = true;
  value.attendees = true;
  value.reminders = true;
  // Scheduling is advertised per discovered server/calendar. The aggregate
  // capability intentionally remains conservative until discovery proves it.
  value.serverScheduling = false;
  return value;
}

CalDavSync::~CalDavSync() {
  m_shuttingDown = true;
  m_retryWakeAt.clear();
  m_retryWakeGeneration.clear();
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
  account.authStatus = QStringLiteral("credential_storage_pending");
  if (!m_database->upsertAccount(account, errorMessage)) {
    return {};
  }
  emit accountChanged(account.id);
  storeCredentialsAsync(account, password);
  return account.id;
}

void CalDavSync::storeCredentialsAsync(const Account& account,
                                       const QString& password) {
  if (const SecretStoreOperationId previous = m_secretOperations.take(account.id);
      previous != kInvalidSecretStoreOperationId) {
    m_secrets.cancel(previous);
  }
  const quint64 generation = m_credentialGeneration.value(account.id) + 1;
  m_credentialGeneration.insert(account.id, generation);
  const auto operationHolder =
      std::make_shared<SecretStoreOperationId>(kInvalidSecretStoreOperationId);
  *operationHolder = m_secrets.storeAsync(
      account.id, QString::fromLatin1(kPasswordKind), password,
      [this, accountId = account.id, password, generation,
       operationHolder](SecretStoreResult result) {
        if (m_credentialGeneration.value(accountId) != generation) {
          return;
        }
        if (m_secretOperations.value(accountId) == *operationHolder) {
          m_secretOperations.remove(accountId);
        }
        const Account current = m_database->account(accountId);
        if (current.id.isEmpty() || current.provider != ProviderKind::CalDav ||
            !current.enabled) {
          (void)m_secrets.removeAsync(accountId, QString::fromLatin1(kPasswordKind),
                                      [](SecretStoreResult) {});
          return;
        }
        if (!result.success) {
          handleCredentialFailure(
              accountId, QStringLiteral("CalDAV credentials could not be saved"));
          return;
        }
        m_client.setCredentials(accountId, current.principal, password,
                                QUrl(current.endpoint));
        m_loadedCredentials.insert(accountId);
        setAccountAuthStatus(accountId, QStringLiteral("connecting"));
        if (m_jobs.contains(accountId)) {
          m_syncAfterCurrentJob.insert(accountId);
        } else {
          syncAccount(accountId);
        }
      });
  m_secretOperations.insert(account.id, *operationHolder);
}

void CalDavSync::loadCredentialsAsync(const Account& account) {
  if (m_loadingCredentials.contains(account.id)) {
    return;
  }
  if (const SecretStoreOperationId previous = m_secretOperations.take(account.id);
      previous != kInvalidSecretStoreOperationId) {
    m_secrets.cancel(previous);
  }
  const quint64 generation = m_credentialGeneration.value(account.id) + 1;
  m_credentialGeneration.insert(account.id, generation);
  m_loadingCredentials.insert(account.id);
  const auto operationHolder =
      std::make_shared<SecretStoreOperationId>(kInvalidSecretStoreOperationId);
  *operationHolder = m_secrets.lookupAsync(
      account.id, QString::fromLatin1(kPasswordKind),
      [this, accountId = account.id, generation,
       operationHolder](SecretStoreResult result) {
        if (m_credentialGeneration.value(accountId) != generation) {
          return;
        }
        m_loadingCredentials.remove(accountId);
        if (m_secretOperations.value(accountId) == *operationHolder) {
          m_secretOperations.remove(accountId);
        }
        const Account current = m_database->account(accountId);
        if (current.id.isEmpty() || current.provider != ProviderKind::CalDav ||
            !current.enabled) {
          return;
        }
        if (!result.success || result.value.isEmpty()) {
          handleCredentialFailure(accountId,
                                  QStringLiteral("CalDAV credentials are unavailable"));
          return;
        }
        m_client.setCredentials(accountId, current.principal, result.value,
                                QUrl(current.endpoint));
        m_loadedCredentials.insert(accountId);
        syncAccount(accountId);
      });
  m_secretOperations.insert(account.id, *operationHolder);
}

void CalDavSync::handleCredentialFailure(const QString& accountId,
                                         const QString& message) {
  m_loadingCredentials.remove(accountId);
  m_loadedCredentials.remove(accountId);
  m_client.forgetCredentials(accountId);
  setAccountAuthStatus(accountId, QStringLiteral("reauthorization_required"));
  const QJsonObject value =
      statusObject(QStringLiteral("reauthorization_required"),
                   QStringLiteral("authentication_required"), message);
  m_status.insert(accountId, value);
  emit syncStatusChanged(accountId, value);
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
  m_loadedCredentials.remove(accountId);
  m_loadingCredentials.remove(accountId);
  m_syncAfterCurrentJob.remove(accountId);
  m_pendingHydrations.remove(accountId);
  m_retryWakeAt.remove(accountId);
  m_retryWakeGeneration.remove(accountId);
  m_credentialGeneration.insert(accountId, m_credentialGeneration.value(accountId) + 1);
  if (const SecretStoreOperationId operation = m_secretOperations.take(accountId);
      operation != kInvalidSecretStoreOperationId) {
    m_secrets.cancel(operation);
  }
  const auto removalHolder =
      std::make_shared<SecretStoreOperationId>(kInvalidSecretStoreOperationId);
  *removalHolder = m_secrets.removeAsync(
      accountId, QString::fromLatin1(kPasswordKind),
      [this, accountId, removalHolder](SecretStoreResult) {
        if (m_secretOperations.value(accountId) == *removalHolder) {
          m_secretOperations.remove(accountId);
        }
      });
  m_secretOperations.insert(accountId, *removalHolder);
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

bool CalDavSync::updateCredentials(const QString& accountId, const QString& username,
                                   const QString& password, QString* errorMessage) {
  if (username.trimmed().isEmpty() || password.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("CalDAV username and password are required");
    }
    return false;
  }
  Account account = m_database->account(accountId, errorMessage);
  if (account.id.isEmpty() || account.provider != ProviderKind::CalDav) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = QStringLiteral("CalDAV account not found");
    }
    return false;
  }
  if (SyncJob* job = m_jobs.value(accountId, nullptr); job != nullptr) {
    job->cancelled = true;
  }
  m_client.forgetCredentials(accountId);
  m_loadedCredentials.remove(accountId);
  account.principal = username.trimmed();
  account.enabled = true;
  account.authStatus = QStringLiteral("credential_storage_pending");
  if (!m_database->upsertAccount(account, errorMessage)) {
    return false;
  }
  emit accountChanged(accountId);
  storeCredentialsAsync(account, password);
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
  if (!m_loadedCredentials.contains(accountId)) {
    loadCredentialsAsync(account);
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

bool CalDavSync::syncRange(const RangeSyncRequest& request, QString* errorMessage) {
  if (!request.startUtc.isValid() || !request.endUtc.isValid() ||
      request.startUtc >= request.endUtc ||
      request.startUtc.daysTo(request.endUtc) > 5 * 366) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("CalDAV range sync needs bounded UTC dates");
    }
    return false;
  }
  QString databaseError;
  const Calendar calendar = m_database->calendar(request.calendarId, &databaseError);
  const Account account = m_database->account(calendar.accountId, &databaseError);
  if (calendar.id.isEmpty() || account.provider != ProviderKind::CalDav ||
      !calendar.enabled || !account.enabled) {
    if (errorMessage != nullptr) {
      *errorMessage = databaseError.isEmpty()
                          ? QStringLiteral("CalDAV calendar is unavailable")
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

void CalDavSync::scheduleRetryWake(const QString& accountId, const QDateTime& wakeAt) {
  if (accountId.isEmpty() || m_shuttingDown) {
    return;
  }
  const QDateTime normalized =
      wakeAt.isValid() ? wakeAt.toUTC() : QDateTime::currentDateTimeUtc();
  const QDateTime existing = m_retryWakeAt.value(accountId);
  if (existing.isValid() && existing <= normalized) {
    return;
  }
  const quint64 generation = ++m_nextRetryWakeGeneration;
  m_retryWakeAt.insert(accountId, normalized);
  m_retryWakeGeneration.insert(accountId, generation);
  const qint64 delayMs =
      std::clamp<qint64>(QDateTime::currentDateTimeUtc().msecsTo(normalized), 0,
                         static_cast<qint64>(std::numeric_limits<int>::max()));
  QTimer::singleShot(
      static_cast<int>(delayMs), this, [this, accountId, normalized, generation]() {
        if (m_shuttingDown || m_retryWakeAt.value(accountId) != normalized ||
            m_retryWakeGeneration.value(accountId) != generation) {
          return;
        }
        m_retryWakeAt.remove(accountId);
        m_retryWakeGeneration.remove(accountId);
        if (m_jobs.contains(accountId)) {
          scheduleRetryWake(accountId, QDateTime::currentDateTimeUtc().addMSecs(250));
          return;
        }
        syncAccount(accountId);
      });
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
        discoverHome(job, CalDavClient::canonicalUrl(job->endpoint, QUrl(href)));
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
        const CalDavSchedulingCapabilities scheduling =
            CalDavXml::schedulingCapabilities(parsed);
        job->schedulingSupported = scheduling.canSend();
        for (const QString& address : scheduling.userAddresses) {
          job->schedulingIdentities.insert(schedulingIdentity(address));
        }
        job->schedulingIdentities.insert(schedulingIdentity(job->account.principal));
        discoverCollections(job, CalDavClient::canonicalUrl(principalUrl, QUrl(href)));
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
          const QString canonicalHref =
              CalDavClient::canonicalResourceId(job->homeUrl, remote.href);
          Calendar existing =
              m_database->calendarByRemoteId(job->accountId, canonicalHref);
          if (existing.id.isEmpty() && canonicalHref != remote.href) {
            existing = m_database->calendarByRemoteId(job->accountId, remote.href);
          }
          // CalDAV has no interoperable discovery property for RFC 5545
          // THISANDFUTURE write support. Keep it disabled until a prior scoped
          // write was read back with RANGE intact (or another explicit
          // provisioning path persists the same proof marker).
          const bool thisAndFutureProven =
              !remote.readOnly &&
              existing.capabilities.value(QStringLiteral("thisAndFutureProven"))
                  .toBool();
          Calendar calendar;
          calendar.accountId = job->accountId;
          calendar.remoteId = canonicalHref;
          calendar.href = canonicalHref;
          calendar.name = remote.displayName.isEmpty() ? QStringLiteral("Calendar")
                                                       : remote.displayName;
          calendar.description = remote.description;
          if (!remote.color.isEmpty()) {
            calendar.color = remote.color.left(7);
          }
          calendar.readOnly = remote.readOnly;
          const bool schedulingWritable = job->schedulingSupported && !remote.readOnly;
          calendar.capabilities = {
              {QStringLiteral("provider"), QStringLiteral("caldav")},
              {QStringLiteral("ctag"), remote.ctag},
              {QStringLiteral("serverSyncToken"), remote.syncToken},
              {QStringLiteral("incrementalSync"), !remote.syncToken.isEmpty()},
              {QStringLiteral("schedulingAdvertised"), job->schedulingSupported},
              {QStringLiteral("serverScheduling"), schedulingWritable},
              {QStringLiteral("attendeeWrites"), schedulingWritable},
              {QStringLiteral("rsvp"), schedulingWritable},
              {QStringLiteral("thisAndFuture"), thisAndFutureProven},
              {QStringLiteral("thisAndFutureProven"), thisAndFutureProven},
              {QStringLiteral("resourceMove"),
               !remote.readOnly && remote.canBind && remote.canUnbind},
          };
          if (!existing.id.isEmpty()) {
            calendar.id = existing.id;
            calendar.syncToken = existing.syncToken;
            calendar.enabled = existing.enabled;
            calendar.lastSyncAt = existing.lastSyncAt;
            calendar.capabilities.insert(
                QStringLiteral("syncedCtag"),
                existing.capabilities.value(QStringLiteral("syncedCtag")));
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
    job->hydrationSync = false;
    job->replaceCoverage = false;
    job->fallbackAttempted = false;
    const QUrl calendarUrl =
        CalDavClient::canonicalUrl(job->homeUrl, QUrl(job->currentCalendar.href));
    QString coverageError;
    const auto [defaultStart, defaultEnd] =
        initialCoverageWindow(QDateTime::currentDateTimeUtc());
    const bool defaultCovered = m_database->isSyncRangeCovered(
        job->currentCalendar.id, defaultStart, defaultEnd, &coverageError);
    if (!coverageError.isEmpty()) {
      finish(job, QStringLiteral("database_error"), coverageError);
      return;
    }
    if (!defaultCovered) {
      job->fullSync = true;
      job->queryStartUtc = defaultStart;
      job->queryEndUtc = defaultEnd;
      m_client.queryCalendar(job->accountId, calendarUrl, job->queryStartUtc,
                             job->queryEndUtc,
                             [this, job](const DavResponse& response) {
                               if (job->cancelled) {
                                 finish(job);
                                 return;
                               }
                               applyCalendarResponse(job, response, true);
                             });
      return;
    }
    const QString remoteCtag =
        job->currentCalendar.capabilities.value(QStringLiteral("ctag")).toString();
    const QString syncedCtag =
        job->currentCalendar.capabilities.value(QStringLiteral("syncedCtag"))
            .toString();
    if (job->currentCalendar.syncToken.isEmpty() &&
        job->currentCalendar.lastSyncAt.isValid() && !remoteCtag.isEmpty() &&
        remoteCtag == syncedCtag) {
      job->currentCalendar.lastSyncAt = QDateTime::currentDateTimeUtc();
      QString databaseError;
      if (!m_database->upsertCalendar(job->currentCalendar, &databaseError)) {
        finish(job, QStringLiteral("database_error"), databaseError);
        return;
      }
      continue;
    }
    if (job->currentCalendar.syncToken.isEmpty() &&
        job->currentCalendar.lastSyncAt.isValid() && remoteCtag.isEmpty()) {
      syncCalendarWithEtags(job, calendarUrl);
      return;
    }
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
              job->replaceCoverage = true;
              job->currentCalendar.syncToken.clear();
              const auto [start, end] =
                  initialCoverageWindow(QDateTime::currentDateTimeUtc());
              job->queryStartUtc = start;
              job->queryEndUtc = end;
              const QUrl url = CalDavClient::canonicalUrl(
                  job->homeUrl, QUrl(job->currentCalendar.href));
              m_client.queryCalendar(job->accountId, url, job->queryStartUtc,
                                     job->queryEndUtc,
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
    const auto [start, end] = initialCoverageWindow(QDateTime::currentDateTimeUtc());
    job->queryStartUtc = start;
    job->queryEndUtc = end;
    m_client.queryCalendar(job->accountId, calendarUrl, job->queryStartUtc,
                           job->queryEndUtc, [this, job](const DavResponse& response) {
                             if (job->cancelled) {
                               finish(job);
                               return;
                             }
                             applyCalendarResponse(job, response, true);
                           });
    return;
  }
  if (job != nullptr) {
    syncNextHydration(job);
  }
}

void CalDavSync::syncNextHydration(SyncJob* job) {
  if (job == nullptr || job->cancelled || m_jobs.value(job->accountId) != job) {
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
    job->replaceCoverage = false;
    job->hydrationRequest = request;
    job->fullSync = true;
    job->fallbackAttempted = false;
    job->queryStartUtc = request.startUtc;
    job->queryEndUtc = request.endUtc;
    const QUrl calendarUrl =
        CalDavClient::canonicalUrl(job->homeUrl, QUrl(calendar.href));
    m_client.queryCalendar(job->accountId, calendarUrl, request.startUtc,
                           request.endUtc, [this, job](const DavResponse& response) {
                             if (job->cancelled) {
                               finish(job);
                               return;
                             }
                             applyCalendarResponse(job, response, true);
                           });
    return;
  }
  m_pendingHydrations.remove(job->accountId);
  drainOutbox(job);
}

void CalDavSync::fetchResourceBatches(SyncJob* job, const QUrl& calendarUrl,
                                      const QStringList& hrefs,
                                      ResourceBatchCallback callback) {
  struct BatchState final {
    QList<QStringList> batches;
    qsizetype index = 0;
    QList<CalDavResource> resources;
  };
  auto state = std::make_shared<BatchState>();
  state->batches = CalDavClient::batchMultiGetHrefs(hrefs);
  auto step = std::make_shared<std::function<void()>>();
  const std::weak_ptr<std::function<void()>> weakStep = step;
  *step = [this, job, calendarUrl, callback = std::move(callback), state,
           weakStep]() mutable {
    if (job == nullptr || job->cancelled) {
      callback({}, QStringLiteral("cancelled"),
               QStringLiteral("The CalDAV sync was cancelled"));
      return;
    }
    if (state->index >= state->batches.size()) {
      callback(std::move(state->resources), {}, {});
      return;
    }
    const QStringList batch = state->batches.at(state->index++);
    const auto continuation = weakStep.lock();
    if (!continuation) {
      callback({}, QStringLiteral("internal_error"),
               QStringLiteral("The multiget batch continuation was lost"));
      return;
    }
    m_client.calendarMultiGet(
        job->accountId, calendarUrl, batch,
        [callback, state, continuation](const DavResponse& response) mutable {
          if (!response.ok) {
            callback({}, response.errorCode, response.errorMessage);
            return;
          }
          const CalDavMultiStatusResult parsed =
              CalDavXml::parseMultiStatus(response.body);
          if (!parsed.ok()) {
            callback({}, parsed.error.code, parsed.error.message);
            return;
          }
          state->resources.append(CalDavXml::resources(parsed));
          (*continuation)();
        });
  };
  (*step)();
}

void CalDavSync::syncCalendarWithEtags(SyncJob* job, const QUrl& calendarUrl) {
  const auto fullQuery = [this, job, calendarUrl]() {
    job->fullSync = true;
    job->replaceCoverage = true;
    const auto [start, end] = initialCoverageWindow(QDateTime::currentDateTimeUtc());
    job->queryStartUtc = start;
    job->queryEndUtc = end;
    m_client.queryCalendar(job->accountId, calendarUrl, job->queryStartUtc,
                           job->queryEndUtc, [this, job](const DavResponse& response) {
                             if (job->cancelled) {
                               finish(job);
                               return;
                             }
                             applyCalendarResponse(job, response, true);
                           });
  };
  m_client.listResourceEtags(
      job->accountId, calendarUrl,
      [this, job, calendarUrl, fullQuery](const DavResponse& response) {
        if (job->cancelled) {
          finish(job);
          return;
        }
        if (!response.ok) {
          fullQuery();
          return;
        }
        const CalDavMultiStatusResult parsed =
            CalDavXml::parseMultiStatus(response.body);
        if (!parsed.ok()) {
          finish(job, parsed.error.code, parsed.error.message);
          return;
        }

        QString databaseError;
        const QList<Event> cachedEvents =
            m_database->eventsForCalendars({job->currentCalendar.id}, &databaseError);
        if (!databaseError.isEmpty()) {
          finish(job, QStringLiteral("database_error"), databaseError);
          return;
        }
        QHash<QString, QString> cachedEtags;
        for (const Event& event : cachedEvents) {
          if (event.remoteId.isEmpty()) {
            continue;
          }
          const QString resourceId = CalDavClient::canonicalResourceId(
              calendarUrl, remoteResourceHref(event.remoteId));
          if (!cachedEtags.contains(resourceId) || !event.etag.isEmpty()) {
            cachedEtags.insert(resourceId, event.etag);
          }
        }

        const QString calendarId =
            CalDavClient::canonicalResourceId(calendarUrl, calendarUrl.toString());
        QSet<QString> remoteIds;
        QStringList changedHrefs;
        for (const CalDavResponse& davResponse : parsed.responses) {
          if (!davResponse.isSuccess() || davResponse.href.isEmpty()) {
            continue;
          }
          const QString resourceId =
              CalDavClient::canonicalResourceId(calendarUrl, davResponse.href);
          if (resourceId == calendarId) {
            continue;
          }
          if (davResponse.etag.isEmpty()) {
            fullQuery();
            return;
          }
          remoteIds.insert(resourceId);
          if (!cachedEtags.contains(resourceId) ||
              cachedEtags.value(resourceId) != davResponse.etag) {
            changedHrefs.append(davResponse.href);
          }
        }

        QList<CalDavResource> deletions;
        for (auto iterator = cachedEtags.constBegin();
             iterator != cachedEtags.constEnd(); ++iterator) {
          if (!remoteIds.contains(iterator.key())) {
            deletions.append({iterator.key(), {}, {}, 410});
          }
        }
        if (changedHrefs.isEmpty()) {
          applyCalendarResources(job, deletions, {}, false);
          return;
        }

        fetchResourceBatches(
            job, calendarUrl, changedHrefs,
            [this, job, calendarUrl, changedHrefs, deletions = std::move(deletions),
             fullQuery](QList<CalDavResource> resources, const QString& errorCode,
                        const QString&) mutable {
              if (job->cancelled) {
                finish(job);
                return;
              }
              if (!errorCode.isEmpty()) {
                fullQuery();
                return;
              }
              QSet<QString> hydratedIds;
              for (const CalDavResource& resource : resources) {
                if (!resource.calendarData.isEmpty()) {
                  hydratedIds.insert(
                      CalDavClient::canonicalResourceId(calendarUrl, resource.href));
                }
              }
              for (const QString& href : changedHrefs) {
                if (!hydratedIds.contains(
                        CalDavClient::canonicalResourceId(calendarUrl, href))) {
                  fullQuery();
                  return;
                }
              }
              resources.append(deletions);
              applyCalendarResources(job, resources, {}, false);
            });
      });
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
  const QUrl calendarUrl =
      CalDavClient::canonicalUrl(job->homeUrl, QUrl(job->currentCalendar.href));
  QList<CalDavResource> resources = CalDavXml::resources(parsed);
  QStringList missingHrefs;
  for (const CalDavResource& resource : resources) {
    if (!resource.deleted() && resource.calendarData.isEmpty()) {
      missingHrefs.append(resource.href);
    }
  }
  if (!missingHrefs.isEmpty()) {
    fetchResourceBatches(
        job, calendarUrl, missingHrefs,
        [this, job, resources = std::move(resources),
         responseSyncToken = parsed.syncToken, fullSync,
         calendarUrl](QList<CalDavResource> fetched, const QString& errorCode,
                      const QString& errorMessage) mutable {
          if (job->cancelled) {
            finish(job);
            return;
          }
          if (!errorCode.isEmpty()) {
            finish(job, errorCode, errorMessage);
            return;
          }
          QHash<QString, CalDavResource> hydrated;
          for (const CalDavResource& resource : fetched) {
            hydrated.insert(
                CalDavClient::canonicalResourceId(calendarUrl, resource.href),
                resource);
          }
          for (CalDavResource& resource : resources) {
            if (resource.deleted() || !resource.calendarData.isEmpty()) {
              continue;
            }
            const QString resourceId =
                CalDavClient::canonicalResourceId(calendarUrl, resource.href);
            const auto replacement = hydrated.constFind(resourceId);
            if (replacement == hydrated.constEnd() ||
                replacement->calendarData.isEmpty()) {
              finish(job, QStringLiteral("incomplete_multiget"),
                     QStringLiteral("The CalDAV server omitted requested event data"));
              return;
            }
            const QString originalEtag = resource.etag;
            resource = *replacement;
            if (resource.etag.isEmpty()) {
              resource.etag = originalEtag;
            }
          }
          applyCalendarResources(job, resources, responseSyncToken, fullSync);
        });
    return;
  }
  applyCalendarResources(job, resources, parsed.syncToken, fullSync);
}

void CalDavSync::applyCalendarResources(SyncJob* job,
                                        const QList<CalDavResource>& resources,
                                        const QString& responseSyncToken,
                                        const bool fullSync) {
  if (job == nullptr || job->cancelled) {
    finish(job);
    return;
  }
  const QUrl calendarUrl =
      CalDavClient::canonicalUrl(job->homeUrl, QUrl(job->currentCalendar.href));
  QStringList retainedRemoteIds;
  QStringList deletedRemoteIds;
  QList<Event> stagedEvents;
  bool observedThisAndFuture = false;
  for (const CalDavResource& resource : resources) {
    const QString resourceId =
        CalDavClient::canonicalResourceId(calendarUrl, resource.href);
    if (resource.deleted()) {
      deletedRemoteIds.append(resourceId);
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
      observedThisAndFuture =
          observedThisAndFuture ||
          event.recurrenceId.contains(QStringLiteral("RANGE=THISANDFUTURE"),
                                      Qt::CaseInsensitive);
      event.calendarId = job->currentCalendar.id;
      event.remoteId = resourceId;
      if (!event.recurrenceId.isEmpty()) {
        event.remoteId += QLatin1Char('#') + event.recurrenceId;
      }
      retainedRemoteIds.append(event.remoteId);
      event.etag = resource.etag;
      markSchedulingIdentity(&event, job->schedulingIdentities);
      stagedEvents.append(std::move(event));
    }
  }
  retainedRemoteIds.removeDuplicates();
  QString error;
  QStringList prunedRemoteIds;
  if (fullSync) {
    const QList<Event> coveredEvents = m_database->eventsBetween(
        job->queryStartUtc, job->queryEndUtc, {job->currentCalendar.id}, &error);
    if (!error.isEmpty()) {
      finish(job, QStringLiteral("database_error"), error);
      return;
    }
    for (const Event& cached : coveredEvents) {
      if (cached.remoteId.isEmpty() || retainedRemoteIds.contains(cached.remoteId)) {
        continue;
      }
      prunedRemoteIds.append(cached.remoteId);
    }
    prunedRemoteIds.removeDuplicates();
  }
  const QString newSyncToken =
      !responseSyncToken.isEmpty() ? responseSyncToken
      : fullSync
          ? job->currentCalendar.capabilities.value(QStringLiteral("serverSyncToken"))
                .toString()
          : QString();
  // A calendar-query only proves the requested time range was observed.  Do not
  // advance the collection-wide incremental cursor for a historical hydration:
  // changes elsewhere in the calendar between the old and returned tokens were
  // not part of this response and still need the normal sync-collection pass.
  if (!job->hydrationSync && !newSyncToken.isEmpty()) {
    job->currentCalendar.syncToken = newSyncToken;
  }
  job->currentCalendar.lastSyncAt = QDateTime::currentDateTimeUtc();
  const QString remoteCtag =
      job->currentCalendar.capabilities.value(QStringLiteral("ctag")).toString();
  // Likewise, a bounded hydration cannot establish that the cached collection
  // matches the current CTag outside the requested range.
  if (!job->hydrationSync && !remoteCtag.isEmpty()) {
    job->currentCalendar.capabilities.insert(QStringLiteral("syncedCtag"), remoteCtag);
  }
  if (observedThisAndFuture && !job->currentCalendar.readOnly) {
    // Receiving a canonical RANGE exception from this collection proves that
    // the server persists and returns the RFC 5545 representation. Future
    // writes still require a read-after-write verification before completion.
    job->currentCalendar.capabilities.insert(QStringLiteral("thisAndFuture"), true);
    job->currentCalendar.capabilities.insert(QStringLiteral("thisAndFutureProven"),
                                             true);
  }
  const bool applied =
      fullSync
          ? m_database->applyRemoteRangeSyncBatch(
                job->currentCalendar, stagedEvents, deletedRemoteIds, prunedRemoteIds,
                job->queryStartUtc, job->queryEndUtc, &error, job->replaceCoverage)
          : m_database->applyRemoteSyncBatch(job->currentCalendar, stagedEvents,
                                             deletedRemoteIds, prunedRemoteIds, &error);
  if (!applied) {
    finish(job, QStringLiteral("database_error"), error);
    return;
  }
  if (!job->changedCalendarIds.contains(job->currentCalendar.id)) {
    job->changedCalendarIds.append(job->currentCalendar.id);
  }
  if (job->hydrationSync) {
    QList<RangeSyncRequest>& pending = m_pendingHydrations[job->accountId];
    if (!pending.isEmpty() &&
        pending.first().calendarId == job->hydrationRequest.calendarId &&
        pending.first().startUtc == job->hydrationRequest.startUtc &&
        pending.first().endUtc == job->hydrationRequest.endUtc) {
      pending.removeFirst();
    }
    syncNextHydration(job);
  } else {
    syncNextCalendar(job);
  }
}

void CalDavSync::drainOutbox(SyncJob* job) {
  if (job == nullptr || job->cancelled) {
    finish(job);
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
    if (item.accountId != job->accountId ||
        (item.state != OutboxState::Pending && item.state != OutboxState::RetryWait)) {
      continue;
    }
    QDateTime wakeAt = item.nextAttemptAt;
    if (!wakeAt.isValid() || (item.notBefore.isValid() && item.notBefore > wakeAt)) {
      wakeAt = item.notBefore;
    }
    if (wakeAt.isValid() && wakeAt > now) {
      scheduleRetryWake(job->accountId, wakeAt);
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

void CalDavSync::startFutureCapabilityProbe(SyncJob* job, const OutboxItem& item,
                                            const Calendar& calendar,
                                            const QUrl& calendarUrl) {
  auto probe = std::make_shared<FutureCapabilityProbe>();
  probe->item = item;
  probe->calendar = calendar;
  probe->calendarUrl = calendarUrl;
  const QByteArray identity = (job->accountId + QLatin1Char('\n') + calendar.id +
                               QLatin1Char('\n') + item.idempotencyKey)
                                  .toUtf8();
  const QString token = QString::fromLatin1(
      QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(32));
  probe->uid = QStringLiteral("omacalendar-capability-%1@example.invalid").arg(token);
  const QString fileName = QStringLiteral(".omacalendar-capability-%1.ics").arg(token);
  QUrl resourceUrl = calendarUrl;
  QString path = resourceUrl.path();
  if (!path.endsWith(QLatin1Char('/'))) {
    path += QLatin1Char('/');
  }
  path += fileName;
  resourceUrl.setPath(path);
  probe->resourceUrl = resourceUrl;
  const QByteArray createPayload = futureCapabilityProbePayload(probe->uid, false);
  probe->updatePayload = futureCapabilityProbePayload(probe->uid, true);
  m_client.deleteEvent(
      job->accountId, probe->resourceUrl, {},
      [this, job, probe, createPayload](const DavResponse& cleanupResponse) {
        if (job == nullptr || m_jobs.value(job->accountId) != job) {
          return;
        }
        const bool cleanupOk = cleanupResponse.ok ||
                               cleanupResponse.httpStatus == 404 ||
                               cleanupResponse.httpStatus == 410;
        if (!cleanupOk) {
          finishFutureCapabilityProbe(
              job, probe, false,
              QStringLiteral("recurrence_capability_probe_cleanup_failed"),
              QStringLiteral("The CalDAV capability probe could not clean up its "
                             "temporary resource"));
          return;
        }
        probe->creationAttempted = true;
        m_client.createEvent(
            job->accountId, probe->resourceUrl, createPayload,
            [this, job, probe](const DavResponse& response) {
              if (job == nullptr || m_jobs.value(job->accountId) != job) {
                return;
              }
              if (!response.ok) {
                finishFutureCapabilityProbe(job, probe, false, response.errorCode,
                                            response.errorMessage);
                return;
              }
              probe->created = true;
              probe->etag = response.etag;
              m_client.readResource(
                  job->accountId, probe->resourceUrl,
                  [this, job, probe](const DavResponse& readResponse) {
                    probeReadCreated(job, probe, readResponse);
                  });
            });
      });
}

void CalDavSync::probeReadCreated(SyncJob* job,
                                  std::shared_ptr<FutureCapabilityProbe> probe,
                                  const DavResponse& response) {
  if (job == nullptr || m_jobs.value(job->accountId) != job) {
    return;
  }
  const DavResponse resource =
      resourcePayload(response, probe->calendarUrl, probe->resourceUrl);
  if (!resource.ok) {
    finishFutureCapabilityProbe(job, probe, false, resource.errorCode,
                                resource.errorMessage);
    return;
  }
  if (!resource.etag.isEmpty()) {
    probe->etag = resource.etag;
  }
  m_client.updateEvent(
      job->accountId, probe->resourceUrl, probe->etag, probe->updatePayload,
      [this, job, probe](const DavResponse& updateResponse) {
        if (job == nullptr || m_jobs.value(job->accountId) != job) {
          return;
        }
        if (!updateResponse.ok) {
          finishFutureCapabilityProbe(job, probe, false, updateResponse.errorCode,
                                      updateResponse.errorMessage);
          return;
        }
        if (!updateResponse.etag.isEmpty()) {
          probe->etag = updateResponse.etag;
        }
        m_client.readResource(job->accountId, probe->resourceUrl,
                              [this, job, probe](const DavResponse& readResponse) {
                                probeReadUpdated(job, probe, readResponse);
                              });
      });
}

void CalDavSync::probeReadUpdated(SyncJob* job,
                                  std::shared_ptr<FutureCapabilityProbe> probe,
                                  const DavResponse& response) {
  if (job == nullptr || m_jobs.value(job->accountId) != job) {
    return;
  }
  const DavResponse resource =
      resourcePayload(response, probe->calendarUrl, probe->resourceUrl);
  bool retainedRange = false;
  if (resource.ok) {
    const ICalendarParseResult parsed = ICalendarCodec::parse(resource.body);
    if (parsed.ok()) {
      retainedRange = std::any_of(
          parsed.events.cbegin(), parsed.events.cend(), [](const Event& event) {
            return event.recurrenceId.contains(QStringLiteral("RANGE=THISANDFUTURE"),
                                               Qt::CaseInsensitive);
          });
    }
    if (!resource.etag.isEmpty()) {
      probe->etag = resource.etag;
    }
  }
  if (!retainedRange) {
    finishFutureCapabilityProbe(
        job, probe, false, QStringLiteral("recurrence_range_not_retained"),
        QStringLiteral("The CalDAV server did not retain safe this-and-future "
                       "recurrence data"));
    return;
  }
  finishFutureCapabilityProbe(job, probe, true, {}, {});
}

void CalDavSync::finishFutureCapabilityProbe(
    SyncJob* job, std::shared_ptr<FutureCapabilityProbe> probe, const bool verified,
    QString errorCode, QString errorMessage) {
  if (job == nullptr || m_jobs.value(job->accountId) != job) {
    return;
  }
  auto complete = [this, job, probe, verified, errorCode = std::move(errorCode),
                   errorMessage = std::move(errorMessage)](
                      const DavResponse& cleanupResponse) mutable {
    const bool cleanupOk = cleanupResponse.ok || cleanupResponse.httpStatus == 404 ||
                           cleanupResponse.httpStatus == 410;
    bool success = verified && cleanupOk;
    QString code = errorCode;
    QString message = errorMessage;
    if (!cleanupOk) {
      success = false;
      code = QStringLiteral("recurrence_capability_probe_cleanup_failed");
      message = QStringLiteral(
          "The CalDAV capability probe could not clean up its "
          "temporary resource");
    }
    if (success) {
      Calendar proven = m_database->calendar(probe->calendar.id, &message);
      if (proven.id.isEmpty()) {
        code = QStringLiteral("database_error");
        if (message.isEmpty()) {
          message = QStringLiteral("The CalDAV capability proof could not be stored");
        }
        success = false;
      } else {
        proven.capabilities.insert(QStringLiteral("thisAndFuture"), true);
        proven.capabilities.insert(QStringLiteral("thisAndFutureProven"), true);
        if (!m_database->upsertCalendar(proven, &message)) {
          code = QStringLiteral("database_error");
          success = false;
        }
      }
    }
    job->futureProbeInProgress = false;
    if (!success) {
      if (code.isEmpty()) {
        code = QStringLiteral("recurrence_capability_probe_failed");
      }
      if (message.isEmpty()) {
        message = QStringLiteral(
            "The CalDAV server did not prove safe this-and-future "
            "write support");
      }
      m_database->updateOutboxState(probe->item.id, OutboxState::Blocked,
                                    probe->item.attempts + 1, {}, code, message);
      // The item was rewound before the probe started so success can replay
      // it. A failed probe must consume that slot again or the blocked item
      // would be probed repeatedly in the same sync job.
      ++job->outboxIndex;
    }
    if (job->cancelled) {
      finish(job);
      return;
    }
    dispatchNextOutbox(job);
  };
  if (!probe->creationAttempted) {
    DavResponse notCreated;
    notCreated.ok = true;
    notCreated.httpStatus = 204;
    complete(notCreated);
    return;
  }
  m_client.deleteEvent(job->accountId, probe->resourceUrl, probe->etag, complete);
}

void CalDavSync::dispatchNextOutbox(SyncJob* job) {
  if (job == nullptr || job->cancelled) {
    finish(job);
    return;
  }
  if (job->outboxIndex >= job->outbox.size()) {
    if (!job->outbox.isEmpty()) {
      // An acknowledged dependency can make another durable mutation ready.
      scheduleRetryWake(job->accountId, QDateTime::currentDateTimeUtc());
    }
    finish(job);
    return;
  }
  const OutboxItem item = job->outbox.at(job->outboxIndex++);
  const Calendar calendar = m_database->calendar(item.calendarId);
  Event event = eventFromJson(item.payload);
  if (event.id.isEmpty()) {
    event.id = item.eventId;
  }
  const QUrl calendarUrl =
      CalDavClient::canonicalUrl(job->homeUrl, QUrl(calendar.href));
  const QUrl resourceUrl = eventResourceUrl(calendarUrl, event);
  QString error;
  const auto blockRecurrenceMutation = [this, job, &item](const QString& code,
                                                          const QString& message) {
    m_database->updateOutboxState(item.id, OutboxState::Blocked, item.attempts + 1, {},
                                  code, message);
    dispatchNextOutbox(job);
  };
  if (item.recurrenceScope != QStringLiteral("series") &&
      item.recurrenceScope != QStringLiteral("occurrence") &&
      item.recurrenceScope != QStringLiteral("future")) {
    blockRecurrenceMutation(
        QStringLiteral("invalid_recurrence_scope"),
        QStringLiteral("The mutation has an invalid recurrence scope"));
    return;
  }
  if (item.recurrenceScope == QStringLiteral("future") &&
      (!calendar.capabilities.value(QStringLiteral("thisAndFuture")).toBool() ||
       !calendar.capabilities.value(QStringLiteral("thisAndFutureProven")).toBool())) {
    if (calendar.readOnly || !calendar.enabled || job->futureProbeInProgress) {
      blockRecurrenceMutation(
          QStringLiteral("recurrence_scope_unsupported"),
          QStringLiteral("This CalDAV server has not proven safe this-and-future "
                         "write support"));
      return;
    }
    // dispatchNextOutbox has already advanced past this item. Put it back while
    // the disposable capability resource is exercised, so a successful probe
    // can replay the exact durable mutation without creating a second outbox
    // item or touching the user's event first.
    --job->outboxIndex;
    job->futureProbeInProgress = true;
    startFutureCapabilityProbe(job, item, calendar, calendarUrl);
    return;
  }
  if (item.sendUpdates == QStringLiteral("externalOnly")) {
    blockRecurrenceMutation(
        QStringLiteral("notification_policy_unsupported"),
        QStringLiteral("CalDAV cannot reliably notify only external guests"));
    return;
  }
  const bool scheduling =
      calendar.capabilities.value(QStringLiteral("serverScheduling")).toBool();
  if ((item.sendUpdates == QStringLiteral("all") || attendeeMutation(event)) &&
      !scheduling) {
    blockRecurrenceMutation(
        QStringLiteral("scheduling_unsupported"),
        QStringLiteral("This server has not proven CalDAV scheduling support"));
    return;
  }
  const QByteArray scheduleReply =
      scheduling ? (item.sendUpdates == QStringLiteral("all") ? QByteArrayLiteral("T")
                                                              : QByteArrayLiteral("F"))
                 : QByteArray();
  if (item.operation == OutboxOperation::Move) {
    if (item.recurrenceScope != QStringLiteral("series")) {
      blockRecurrenceMutation(
          QStringLiteral("move_recurrence_scope_unsupported"),
          QStringLiteral(
              "This CalDAV server can only move an entire recurring series"));
      return;
    }
    Event source = moveSourceEvent(item);
    const Event canonical = m_database->event(item.eventId);
    if (!canonical.remoteId.isEmpty()) {
      source.remoteId = canonical.remoteId;
    }
    if (!item.expectedRevision.isEmpty()) {
      source.etag = item.expectedRevision;
    }
    const QString sourceCalendarId = moveMetadata(item)
                                         .value(QStringLiteral("sourceCalendarId"))
                                         .toString(source.calendarId);
    source.calendarId = sourceCalendarId;
    const Calendar sourceCalendar = m_database->calendar(sourceCalendarId);
    if (calendar.id.isEmpty() || sourceCalendar.id.isEmpty() ||
        calendar.accountId != sourceCalendar.accountId || source.remoteId.isEmpty()) {
      blockRecurrenceMutation(
          QStringLiteral("move_source_unavailable"),
          QStringLiteral("The source CalDAV resource is unavailable"));
      return;
    }
    if (!m_database->updateOutboxState(item.id, OutboxState::Sending, item.attempts + 1,
                                       QDateTime::currentDateTimeUtc(), {}, {},
                                       &error)) {
      finish(job, QStringLiteral("database_error"), error);
      return;
    }
    dispatchMove(job, item, event, source, sourceCalendar, calendar, scheduleReply);
    return;
  }
  const bool resourceDelete = item.operation == OutboxOperation::Remove &&
                              item.recurrenceScope == QStringLiteral("series");
  ICalendarSerializeResult encoded;
  if (item.operation == OutboxOperation::Create) {
    encoded = ICalendarCodec::serialize(event);
  } else if (!resourceDelete) {
    encoded = ICalendarCodec::patchScoped(event, event.rawPayload.toUtf8(),
                                          item.recurrenceScope,
                                          item.operation == OutboxOperation::Remove);
  }
  if (item.operation == OutboxOperation::Create && encoded.ok()) {
    encoded =
        ICalendarCodec::stampClientMutationId(encoded.payload, item.idempotencyKey);
  }
  if (!resourceDelete && !encoded.ok()) {
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
  const auto callback = [this, job, item, event, resourceUrl,
                         sentPayload = encoded.payload](const DavResponse& response) {
    if (job->cancelled) {
      finish(job);
      return;
    }
    handleMutationResult(job, item, event, resourceUrl, sentPayload, response);
  };
  if (item.operation == OutboxOperation::Create) {
    m_client.createEvent(job->accountId, resourceUrl, encoded.payload, callback,
                         scheduleReply);
  } else if (!resourceDelete) {
    m_client.updateEvent(job->accountId, resourceUrl, item.expectedRevision,
                         encoded.payload, callback, scheduleReply);
  } else if (event.remoteId.isEmpty()) {
    m_database->completeOutbox(item.id, nullptr, &error);
    dispatchNextOutbox(job);
  } else {
    m_client.deleteEvent(job->accountId, resourceUrl, item.expectedRevision, callback,
                         scheduleReply);
  }
}

void CalDavSync::dispatchMove(SyncJob* job, const OutboxItem& item,
                              const Event& targetEvent, const Event& sourceEvent,
                              const Calendar& sourceCalendar,
                              const Calendar& targetCalendar,
                              const QByteArray& scheduleReply) {
  if (job == nullptr || job->cancelled) {
    finish(job);
    return;
  }
  const QUrl sourceCalendarUrl =
      CalDavClient::canonicalUrl(job->homeUrl, QUrl(sourceCalendar.href));
  const QUrl targetCalendarUrl =
      CalDavClient::canonicalUrl(job->homeUrl, QUrl(targetCalendar.href));
  const QUrl sourceResourceUrl = eventResourceUrl(sourceCalendarUrl, sourceEvent);
  const QUrl targetResourceUrl =
      moveTargetResourceUrl(targetCalendarUrl, sourceResourceUrl, targetEvent);
  if (!CalDavClient::isSameOrigin(sourceResourceUrl, targetResourceUrl)) {
    m_database->updateOutboxState(
        item.id, OutboxState::Blocked, item.attempts + 1, {},
        QStringLiteral("cross_origin_move_rejected"),
        QStringLiteral("CalDAV events can only move within one server origin"));
    dispatchNextOutbox(job);
    return;
  }

  ICalendarSerializeResult encoded = ICalendarCodec::patchScoped(
      targetEvent, sourceEvent.rawPayload.toUtf8(), QStringLiteral("series"));
  if (encoded.ok()) {
    encoded =
        ICalendarCodec::stampClientMutationId(encoded.payload, item.idempotencyKey);
  }
  if (!encoded.ok()) {
    m_database->updateOutboxState(item.id, OutboxState::Blocked, item.attempts + 1, {},
                                  encoded.error.code, encoded.error.message);
    dispatchNextOutbox(job);
    return;
  }

  const bool directMove =
      sourceCalendar.capabilities.value(QStringLiteral("resourceMove")).toBool() &&
      targetCalendar.capabilities.value(QStringLiteral("resourceMove")).toBool() &&
      !moveChangesEvent(sourceEvent, targetEvent);
  if (!directMove) {
    startFallbackMove(job, item, targetEvent, sourceEvent, sourceResourceUrl,
                      targetResourceUrl, encoded.payload, scheduleReply);
    return;
  }

  m_client.moveEvent(
      job->accountId, sourceResourceUrl, targetResourceUrl, item.expectedRevision,
      [this, job, item, targetEvent, sourceEvent, sourceResourceUrl, targetResourceUrl,
       targetPayload = encoded.payload, scheduleReply](const DavResponse& response) {
        if (job->cancelled) {
          finish(job);
          return;
        }
        if (response.ok) {
          hydrateMoveTarget(job, item, targetEvent, targetResourceUrl, targetPayload);
          return;
        }
        if (response.httpStatus == 405 || response.httpStatus == 501) {
          // The privilege set was optimistic. A PUT-then-DELETE sequence is
          // still safe because the source remains untouched until the target
          // resource is proven durable.
          startFallbackMove(job, item, targetEvent, sourceEvent, sourceResourceUrl,
                            targetResourceUrl, targetPayload, scheduleReply);
          return;
        }
        const bool ambiguous = response.httpStatus == 0 || response.httpStatus == 408 ||
                               response.httpStatus == 409 ||
                               response.httpStatus == 412 ||
                               response.httpStatus == 404 ||
                               response.httpStatus == 410 || response.httpStatus >= 500;
        if (ambiguous) {
          reconcileMoveTarget(job, item, targetEvent, sourceEvent, sourceResourceUrl,
                              targetResourceUrl, targetPayload, scheduleReply, true,
                              response);
          return;
        }
        handleMutationResult(job, item, targetEvent, targetResourceUrl, targetPayload,
                             response);
      },
      scheduleReply);
}

void CalDavSync::startFallbackMove(SyncJob* job, const OutboxItem& item,
                                   const Event& targetEvent, const Event& sourceEvent,
                                   const QUrl& sourceResourceUrl,
                                   const QUrl& targetResourceUrl,
                                   const QByteArray& targetPayload,
                                   const QByteArray& scheduleReply) {
  if (job == nullptr || job->cancelled) {
    finish(job);
    return;
  }
  m_client.createEvent(
      job->accountId, targetResourceUrl, targetPayload,
      [this, job, item, targetEvent, sourceEvent, sourceResourceUrl, targetResourceUrl,
       targetPayload, scheduleReply](const DavResponse& response) {
        if (job->cancelled) {
          finish(job);
          return;
        }
        if (response.ok) {
          // Even a successful PUT is verified before the source is touched;
          // this also covers servers that omit an ETag or acknowledge before
          // the new resource is immediately readable.
          reconcileMoveTarget(job, item, targetEvent, sourceEvent, sourceResourceUrl,
                              targetResourceUrl, targetPayload, scheduleReply, false,
                              response);
          return;
        }
        const bool ambiguous = response.httpStatus == 0 || response.httpStatus == 408 ||
                               response.httpStatus == 409 ||
                               response.httpStatus == 412 || response.httpStatus >= 500;
        if (ambiguous) {
          reconcileMoveTarget(job, item, targetEvent, sourceEvent, sourceResourceUrl,
                              targetResourceUrl, targetPayload, scheduleReply, false,
                              response);
          return;
        }
        handleMutationResult(job, item, targetEvent, targetResourceUrl, targetPayload,
                             response);
      },
      scheduleReply);
}

void CalDavSync::reconcileMoveTarget(SyncJob* job, const OutboxItem& item,
                                     const Event& targetEvent, const Event& sourceEvent,
                                     const QUrl& sourceResourceUrl,
                                     const QUrl& targetResourceUrl,
                                     const QByteArray& targetPayload,
                                     const QByteArray& scheduleReply,
                                     const bool directMove,
                                     const DavResponse& originalResponse) {
  if (job == nullptr || job->cancelled) {
    finish(job);
    return;
  }
  const QUrl targetCalendarUrl = targetResourceUrl.adjusted(QUrl::RemoveFilename);
  m_client.readResource(
      job->accountId, targetResourceUrl,
      [this, job, item, targetEvent, sourceEvent, sourceResourceUrl, targetResourceUrl,
       targetCalendarUrl, targetPayload, scheduleReply, directMove,
       originalResponse](const DavResponse& lookupResponse) {
        if (job->cancelled) {
          finish(job);
          return;
        }
        const DavResponse target =
            resourcePayload(lookupResponse, targetCalendarUrl, targetResourceUrl);
        if (!target.ok) {
          if ((target.httpStatus == 404 || target.httpStatus == 410) &&
              item.attempts < 3) {
            DavResponse pending = originalResponse;
            pending.httpStatus = 503;
            pending.retryable = true;
            pending.errorCode = QStringLiteral("move_reconciliation_pending");
            pending.errorMessage = QStringLiteral(
                "Waiting for the CalDAV server to expose the move result");
            handleMutationResult(job, item, targetEvent, targetResourceUrl,
                                 targetPayload, pending);
            return;
          }
          if (lookupResponse.authenticationRequired || lookupResponse.retryable) {
            handleMutationResult(job, item, targetEvent, targetResourceUrl,
                                 targetPayload, lookupResponse);
            return;
          }
          if (!m_database->recordProviderConflict(item.id, nullptr)) {
            finish(job, QStringLiteral("database_error"),
                   QStringLiteral("Unable to record the failed CalDAV move"));
            return;
          }
          dispatchNextOutbox(job);
          return;
        }

        const ICalendarParseResult parsed = ICalendarCodec::parse(target.body);
        const bool matchingUid =
            parsed.ok() && std::any_of(parsed.events.cbegin(), parsed.events.cend(),
                                       [&sourceEvent](const Event& event) {
                                         return event.uid == sourceEvent.uid;
                                       });
        const bool matchingMutation =
            ICalendarCodec::hasClientMutationId(target.body, item.idempotencyKey);
        if (!matchingUid || (!directMove && !matchingMutation)) {
          m_database->updateOutboxState(
              item.id, OutboxState::Blocked, item.attempts + 1, {},
              QStringLiteral("move_target_collision"),
              QStringLiteral("The target CalDAV resource belongs to another event"));
          dispatchNextOutbox(job);
          return;
        }
        if (!directMove) {
          deleteMoveSource(job, item, targetEvent, sourceResourceUrl, targetResourceUrl,
                           targetPayload, scheduleReply);
          return;
        }

        const QUrl sourceCalendarUrl = sourceResourceUrl.adjusted(QUrl::RemoveFilename);
        m_client.readResource(
            job->accountId, sourceResourceUrl,
            [this, job, item, targetEvent, sourceResourceUrl, sourceCalendarUrl,
             targetResourceUrl, targetPayload,
             target](const DavResponse& sourceLookup) {
              if (job->cancelled) {
                finish(job);
                return;
              }
              const DavResponse source =
                  resourcePayload(sourceLookup, sourceCalendarUrl, sourceResourceUrl);
              if (source.httpStatus == 404 || source.httpStatus == 410) {
                completeSuccessfulMutation(job, item, targetEvent, targetResourceUrl,
                                           targetPayload, target);
                return;
              }
              if (source.ok) {
                m_database->updateOutboxState(
                    item.id, OutboxState::Blocked, item.attempts + 1, {},
                    QStringLiteral("move_target_collision"),
                    QStringLiteral(
                        "Both source and target CalDAV resources still exist"));
                dispatchNextOutbox(job);
                return;
              }
              handleMutationResult(job, item, targetEvent, targetResourceUrl,
                                   targetPayload, sourceLookup);
            });
      });
}

void CalDavSync::deleteMoveSource(SyncJob* job, const OutboxItem& item,
                                  const Event& targetEvent,
                                  const QUrl& sourceResourceUrl,
                                  const QUrl& targetResourceUrl,
                                  const QByteArray& targetPayload,
                                  const QByteArray& scheduleReply) {
  if (job == nullptr || job->cancelled) {
    finish(job);
    return;
  }
  m_client.deleteEvent(
      job->accountId, sourceResourceUrl, item.expectedRevision,
      [this, job, item, targetEvent, targetResourceUrl,
       targetPayload](const DavResponse& response) {
        if (job->cancelled) {
          finish(job);
          return;
        }
        if (response.ok || response.httpStatus == 404 || response.httpStatus == 410) {
          hydrateMoveTarget(job, item, targetEvent, targetResourceUrl, targetPayload);
          return;
        }
        handleMutationResult(job, item, targetEvent, targetResourceUrl, targetPayload,
                             response);
      },
      scheduleReply);
}

void CalDavSync::hydrateMoveTarget(SyncJob* job, const OutboxItem& item,
                                   const Event& targetEvent,
                                   const QUrl& targetResourceUrl,
                                   const QByteArray& targetPayload) {
  if (job == nullptr || job->cancelled) {
    finish(job);
    return;
  }
  const QUrl targetCalendarUrl = targetResourceUrl.adjusted(QUrl::RemoveFilename);
  m_client.readResource(
      job->accountId, targetResourceUrl,
      [this, job, item, targetEvent, targetResourceUrl, targetCalendarUrl,
       targetPayload](const DavResponse& response) {
        if (job->cancelled) {
          finish(job);
          return;
        }
        const DavResponse target =
            resourcePayload(response, targetCalendarUrl, targetResourceUrl);
        if (!target.ok) {
          handleMutationResult(job, item, targetEvent, targetResourceUrl, targetPayload,
                               target);
          return;
        }
        completeSuccessfulMutation(job, item, targetEvent, targetResourceUrl,
                                   targetPayload, target);
      });
}

void CalDavSync::handleMutationResult(SyncJob* job, const OutboxItem& item,
                                      const Event& localEvent, const QUrl& resourceUrl,
                                      const QByteArray& sentPayload,
                                      const DavResponse& response) {
  if (job == nullptr || job->cancelled) {
    finish(job);
    return;
  }
  QString error;
  if (response.ok) {
    const bool resourceRemains = item.operation != OutboxOperation::Remove ||
                                 item.recurrenceScope != QStringLiteral("series");
    if (resourceRemains &&
        (response.etag.isEmpty() || item.recurrenceScope == QStringLiteral("future"))) {
      m_client.readResource(
          job->accountId, resourceUrl,
          [this, job, item, localEvent, resourceUrl, sentPayload,
           response](const DavResponse& metadataResponse) {
            if (job->cancelled) {
              finish(job);
              return;
            }
            DavResponse recovered;
            recovered.ok = true;
            recovered.httpStatus = 200;
            recovered.etag = response.etag;
            if (metadataResponse.ok) {
              const CalDavMultiStatusResult parsed =
                  CalDavXml::parseMultiStatus(metadataResponse.body);
              if (parsed.ok()) {
                const QUrl calendarUrl = resourceUrl.adjusted(QUrl::RemoveFilename);
                const QString expected = CalDavClient::canonicalResourceId(
                    calendarUrl, resourceUrl.toString());
                for (const CalDavResource& resource : CalDavXml::resources(parsed)) {
                  if (CalDavClient::canonicalResourceId(calendarUrl, resource.href) ==
                      expected) {
                    recovered.etag = resource.etag;
                    recovered.body = resource.calendarData.toUtf8();
                    break;
                  }
                }
              }
            }
            if (item.recurrenceScope == QStringLiteral("future")) {
              const ICalendarParseResult verification =
                  ICalendarCodec::parse(recovered.body);
              bool retainedRange = false;
              if (verification.ok()) {
                for (const Event& event : verification.events) {
                  if (event.uid == localEvent.uid &&
                      event.recurrenceId.contains(QStringLiteral("RANGE=THISANDFUTURE"),
                                                  Qt::CaseInsensitive)) {
                    retainedRange = true;
                    break;
                  }
                }
              }
              if (!retainedRange) {
                m_database->updateOutboxState(
                    item.id, OutboxState::Blocked, item.attempts + 1, {},
                    QStringLiteral("recurrence_range_not_retained"),
                    QStringLiteral("The CalDAV server accepted the event but did "
                                   "not retain RANGE=THISANDFUTURE"));
                dispatchNextOutbox(job);
                return;
              }
            }
            completeSuccessfulMutation(job, item, localEvent, resourceUrl, sentPayload,
                                       recovered);
          });
      return;
    }
    completeSuccessfulMutation(job, item, localEvent, resourceUrl, sentPayload,
                               response);
    return;
  }

  const bool ambiguousCreate =
      item.operation == OutboxOperation::Create &&
      (response.httpStatus == 0 || response.httpStatus == 408 ||
       response.httpStatus == 409 || response.httpStatus == 412 ||
       response.httpStatus >= 500);
  if (ambiguousCreate) {
    reconcileAmbiguousCreate(job, item, localEvent, resourceUrl, sentPayload, response);
    return;
  }

  if (response.httpStatus == 409 || response.httpStatus == 412) {
    m_client.readResource(
        job->accountId, resourceUrl,
        [this, job, item, localEvent, resourceUrl](const DavResponse& lookupResponse) {
          if (job->cancelled) {
            finish(job);
            return;
          }
          QString databaseError;
          if (lookupResponse.httpStatus == 404 || lookupResponse.httpStatus == 410) {
            if (!m_database->recordProviderConflict(item.id, nullptr, &databaseError)) {
              finish(job, QStringLiteral("database_error"), databaseError);
              return;
            }
            dispatchNextOutbox(job);
            return;
          }
          if (!lookupResponse.ok) {
            m_database->updateOutboxState(
                item.id, OutboxState::Blocked, item.attempts + 1, {},
                QStringLiteral("conflict_snapshot_unavailable"),
                QStringLiteral("The remote event changed, but its current snapshot "
                               "could not be loaded"));
            dispatchNextOutbox(job);
            return;
          }
          const CalDavMultiStatusResult multistatus =
              CalDavXml::parseMultiStatus(lookupResponse.body);
          if (!multistatus.ok()) {
            m_database->updateOutboxState(
                item.id, OutboxState::Blocked, item.attempts + 1, {},
                QStringLiteral("conflict_snapshot_invalid"), multistatus.error.message);
            dispatchNextOutbox(job);
            return;
          }
          const QUrl calendarUrl = resourceUrl.adjusted(QUrl::RemoveFilename);
          const QString expected =
              CalDavClient::canonicalResourceId(calendarUrl, resourceUrl.toString());
          for (const CalDavResource& resource : CalDavXml::resources(multistatus)) {
            if (CalDavClient::canonicalResourceId(calendarUrl, resource.href) !=
                expected) {
              continue;
            }
            if (resource.deleted()) {
              if (!m_database->recordProviderConflict(item.id, nullptr,
                                                      &databaseError)) {
                finish(job, QStringLiteral("database_error"), databaseError);
                return;
              }
              dispatchNextOutbox(job);
              return;
            }
            const ICalendarParseResult decoded =
                ICalendarCodec::parse(resource.calendarData.toUtf8());
            if (!decoded.ok()) {
              break;
            }
            const Event* selected = nullptr;
            for (const Event& candidate : decoded.events) {
              if (candidate.uid != localEvent.uid) {
                continue;
              }
              if (sameRecurrenceIdentity(candidate, localEvent)) {
                selected = &candidate;
                break;
              }
              if (selected == nullptr && localEvent.recurrenceId.isEmpty() &&
                  candidate.recurrenceId.isEmpty()) {
                selected = &candidate;
              }
            }
            if (selected != nullptr) {
              Event remote = *selected;
              remote.calendarId = item.calendarId;
              remote.remoteId = expected;
              if (!remote.recurrenceId.isEmpty()) {
                remote.remoteId += QLatin1Char('#') + remote.recurrenceId;
              }
              remote.etag = resource.etag;
              if (!m_database->recordProviderConflict(item.id, &remote,
                                                      &databaseError)) {
                finish(job, QStringLiteral("database_error"), databaseError);
                return;
              }
              dispatchNextOutbox(job);
              return;
            }
            if (!localEvent.recurrenceId.isEmpty() &&
                !m_database->recordProviderConflict(item.id, nullptr, &databaseError)) {
              finish(job, QStringLiteral("database_error"), databaseError);
              return;
            } else if (!localEvent.recurrenceId.isEmpty()) {
              dispatchNextOutbox(job);
              return;
            }
          }
          m_database->updateOutboxState(
              item.id, OutboxState::Blocked, item.attempts + 1, {},
              QStringLiteral("conflict_snapshot_invalid"),
              QStringLiteral("The current CalDAV resource did not contain the "
                             "conflicting event"));
          dispatchNextOutbox(job);
        });
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
    const QDateTime wakeAt =
        QDateTime::currentDateTimeUtc().addMSecs(decision.delay.count());
    QString databaseError;
    if (!m_database->updateOutboxState(item.id, OutboxState::RetryWait,
                                       item.attempts + 1, wakeAt, response.errorCode,
                                       response.errorMessage, &databaseError)) {
      finish(job, QStringLiteral("database_error"), databaseError);
      return;
    }
    scheduleRetryWake(job->accountId, wakeAt);
  } else {
    m_database->updateOutboxState(item.id, OutboxState::Blocked, item.attempts + 1, {},
                                  response.errorCode, response.errorMessage);
    if (decision.action == RetryAction::Reauthenticate) {
      setAccountAuthStatus(job->accountId, QStringLiteral("reauthorization_required"));
    }
  }
  dispatchNextOutbox(job);
}

void CalDavSync::reconcileAmbiguousCreate(SyncJob* job, const OutboxItem& item,
                                          const Event& localEvent,
                                          const QUrl& resourceUrl,
                                          const QByteArray& sentPayload,
                                          const DavResponse& originalResponse) {
  m_client.readResource(
      job->accountId, resourceUrl,
      [this, job, item, localEvent, resourceUrl, sentPayload,
       originalResponse](const DavResponse& lookupResponse) {
        if (job->cancelled) {
          finish(job);
          return;
        }

        bool resourceExists = false;
        if (lookupResponse.ok) {
          const CalDavMultiStatusResult parsed =
              CalDavXml::parseMultiStatus(lookupResponse.body);
          if (parsed.ok()) {
            const QUrl calendarUrl = resourceUrl.adjusted(QUrl::RemoveFilename);
            const QString expected =
                CalDavClient::canonicalResourceId(calendarUrl, resourceUrl.toString());
            for (const CalDavResource& resource : CalDavXml::resources(parsed)) {
              if (CalDavClient::canonicalResourceId(calendarUrl, resource.href) !=
                  expected) {
                continue;
              }
              if (resource.deleted()) {
                break;
              }
              resourceExists = !resource.calendarData.isEmpty();
              if (resourceExists &&
                  ICalendarCodec::hasClientMutationId(resource.calendarData.toUtf8(),
                                                      item.idempotencyKey)) {
                DavResponse accepted;
                accepted.ok = true;
                accepted.httpStatus = 200;
                accepted.etag = resource.etag;
                accepted.body = resource.calendarData.toUtf8();
                completeSuccessfulMutation(job, item, localEvent, resourceUrl,
                                           sentPayload, accepted);
                return;
              }
              break;
            }
          }
        }

        if (resourceExists) {
          m_database->updateOutboxState(
              item.id, OutboxState::Blocked, item.attempts + 1, {},
              QStringLiteral("resource_collision"),
              QStringLiteral("The CalDAV resource exists but belongs to a "
                             "different create operation"));
          dispatchNextOutbox(job);
          return;
        }

        ProviderError providerError;
        providerError.kind = ProviderErrorKind::ServiceUnavailable;
        providerError.code = originalResponse.errorCode;
        providerError.message = originalResponse.errorMessage;
        providerError.httpStatus = originalResponse.httpStatus;
        providerError.retryAfterMs = originalResponse.retryAfterSeconds > 0
                                         ? originalResponse.retryAfterSeconds * 1000LL
                                         : -1;
        if (lookupResponse.authenticationRequired) {
          providerError.kind = ProviderErrorKind::Authentication;
          providerError.code = lookupResponse.errorCode;
          providerError.message = lookupResponse.errorMessage;
          providerError.httpStatus = lookupResponse.httpStatus;
        }
        const RetryDecision decision = m_retryPolicy.evaluate({
            providerError,
            item.attempts + 1,
            true,
            false,
            stableJitterKey(item.idempotencyKey),
        });
        if (decision.action == RetryAction::Retry) {
          const QDateTime wakeAt =
              QDateTime::currentDateTimeUtc().addMSecs(decision.delay.count());
          QString databaseError;
          if (!m_database->updateOutboxState(
                  item.id, OutboxState::RetryWait, item.attempts + 1, wakeAt,
                  originalResponse.errorCode, originalResponse.errorMessage,
                  &databaseError)) {
            finish(job, QStringLiteral("database_error"), databaseError);
            return;
          }
          scheduleRetryWake(job->accountId, wakeAt);
        } else {
          m_database->updateOutboxState(item.id, OutboxState::Blocked,
                                        item.attempts + 1, {}, providerError.code,
                                        providerError.message);
          if (decision.action == RetryAction::Reauthenticate) {
            setAccountAuthStatus(job->accountId,
                                 QStringLiteral("reauthorization_required"));
          }
        }
        dispatchNextOutbox(job);
      });
}

void CalDavSync::completeSuccessfulMutation(SyncJob* job, const OutboxItem& item,
                                            const Event& localEvent,
                                            const QUrl& resourceUrl,
                                            const QByteArray& sentPayload,
                                            const DavResponse& response) {
  if (job == nullptr || job->cancelled) {
    finish(job);
    return;
  }
  QString error;
  const bool resourceDelete = item.operation == OutboxOperation::Remove &&
                              item.recurrenceScope == QStringLiteral("series");
  const QByteArray acknowledgedPayload =
      response.body.startsWith("BEGIN:VCALENDAR") ? response.body : sentPayload;
  if (resourceDelete) {
    if (!m_database->completeOutbox(item.id, nullptr, &error)) {
      finish(job, QStringLiteral("database_error"), error);
      return;
    }
  } else {
    const ICalendarParseResult decoded = ICalendarCodec::parse(acknowledgedPayload);
    if (!decoded.ok()) {
      finish(job, decoded.error.code, decoded.error.message);
      return;
    }
    const QUrl calendarUrl = resourceUrl.adjusted(QUrl::RemoveFilename);
    const QString resourceId =
        CalDavClient::canonicalResourceId(calendarUrl, resourceUrl.toString());
    QList<Event> stagedEvents;
    qsizetype acknowledgedIndex = -1;
    for (Event remote : decoded.events) {
      remote.calendarId = item.calendarId;
      if (remote.uid == localEvent.uid && sameRecurrenceIdentity(remote, localEvent)) {
        // The presentation recurrence reference can be ISO 8601 while the
        // server returns RFC 5545 basic syntax. Reuse the optimistic row so a
        // successful detached mutation cannot create a duplicate exception.
        remote.id = item.eventId;
        acknowledgedIndex = stagedEvents.size();
      }
      remote.remoteId = resourceId;
      if (!remote.recurrenceId.isEmpty()) {
        remote.remoteId += QLatin1Char('#') + remote.recurrenceId;
      }
      remote.etag = response.etag;
      markSchedulingIdentity(&remote, job->schedulingIdentities);
      stagedEvents.append(std::move(remote));
    }
    if (acknowledgedIndex < 0) {
      finish(job, QStringLiteral("provider_payload_invalid"),
             QStringLiteral("The acknowledged CalDAV resource did not contain the "
                            "mutated event"));
      return;
    }
    Calendar calendar = m_database->calendar(item.calendarId, &error);
    if (item.recurrenceScope == QStringLiteral("future")) {
      // handleMutationResult read the resource back and verified that the
      // server retained RANGE. Persist that proof in the same transaction as
      // the canonical exception and outbox acknowledgement.
      calendar.capabilities.insert(QStringLiteral("thisAndFuture"), true);
      calendar.capabilities.insert(QStringLiteral("thisAndFutureProven"), true);
    }
    const Event* acknowledged = item.operation == OutboxOperation::Remove
                                    ? nullptr
                                    : &stagedEvents.at(acknowledgedIndex);
    if (calendar.id.isEmpty() ||
        !m_database->completeOutboxWithRemoteSyncBatch(item.id, acknowledged, calendar,
                                                       stagedEvents, {}, {}, &error)) {
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
}

void CalDavSync::finish(SyncJob* job, const QString& errorCode,
                        const QString& errorMessage) {
  if (job == nullptr || m_jobs.value(job->accountId) != job) {
    return;
  }
  const QString accountId = job->accountId;
  const QStringList changed = job->changedCalendarIds;
  const bool cancelled = job->cancelled;
  const bool shouldResync =
      m_syncAfterCurrentJob.remove(accountId) ||
      (errorCode.isEmpty() && m_pendingHydrations.contains(accountId));
  m_jobs.remove(accountId);
  delete job;
  if (cancelled) {
    if (shouldResync) {
      QTimer::singleShot(0, this, [this, accountId]() { syncAccount(accountId); });
    }
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
  if (shouldResync) {
    QTimer::singleShot(0, this, [this, accountId]() { syncAccount(accountId); });
  }
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
