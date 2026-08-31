#include "providers/ics/icsservice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <algorithm>
#include <utility>

#include "providers/caldav/icalcodec.h"

namespace omacalendar::ics {
namespace {

constexpr qint64 kMaximumIcsBytes = 16 * 1024 * 1024;
constexpr int kMinimumRefreshSeconds = 60;
constexpr int kDefaultRefreshSeconds = 3600;
constexpr int kMaximumRedirects = 5;
constexpr int kNetworkTimeoutMs = 30000;

QString publicNetworkError(const int status, const QNetworkReply::NetworkError error) {
  if (status == 401 || status == 403) {
    return QStringLiteral("The calendar feed rejected its credentials");
  }
  if (status == 404) {
    return QStringLiteral("The calendar feed was not found");
  }
  if (status == 429) {
    return QStringLiteral("The calendar feed is temporarily rate limited");
  }
  if (status >= 500) {
    return QStringLiteral("The calendar feed service is temporarily unavailable");
  }
  if (status > 0) {
    return QStringLiteral("The calendar feed request failed (%1)").arg(status);
  }
  if (error == QNetworkReply::SslHandshakeFailedError) {
    return QStringLiteral("The calendar feed TLS connection could not be verified");
  }
  return QStringLiteral("The calendar feed could not be reached");
}

QDateTime eventStart(const Event& event) {
  return event.allDay ? QDateTime(event.startDate, QTime(0, 0), QTimeZone::UTC)
                      : event.startUtc;
}

QDateTime eventEnd(const Event& event) {
  return event.allDay ? QDateTime(event.endDate, QTime(0, 0), QTimeZone::UTC)
                      : event.endUtc;
}

QStringList calendarIdsFromJson(const QJsonValue& value) {
  QStringList result;
  if (value.isString() && !value.toString().trimmed().isEmpty()) {
    result.append(value.toString().trimmed());
  }
  for (const QJsonValue& item : value.toArray()) {
    if (item.isString() && !item.toString().trimmed().isEmpty()) {
      result.append(item.toString().trimmed());
    }
  }
  result.removeDuplicates();
  return result;
}

}  // namespace

struct IcsService::FetchContext {
  IcsSubscription subscription;
  QString username;
  QString password;
  int redirects = 0;
  bool tooLarge = false;
  bool timedOut = false;
  bool includeValidators = true;
  QByteArray body;
};

IcsService::IcsService(Database* database, QObject* parent)
    : Provider(QStringLiteral("ics"), ProviderKind::Ics, parent), m_database(database) {
  m_refreshTimer.setInterval(60000);
  connect(&m_refreshTimer, &QTimer::timeout, this, [this]() { refreshAll(true); });
}

ProviderCapabilities IcsService::capabilities() const {
  ProviderCapabilities value;
  value.accountDiscovery = false;
  value.calendarDiscovery = false;
  value.incrementalSync = true;
  value.createEvent = false;
  value.updateEvent = false;
  value.removeEvent = false;
  value.attendees = false;
  value.reminders = true;
  return value;
}

bool IcsService::normalizeSubscriptionUrl(const QString& input, QUrl* normalized,
                                          QString* errorMessage) {
  QUrl url = QUrl::fromUserInput(input.trimmed());
  QString scheme = url.scheme().toLower();
  if (scheme == QStringLiteral("webcal") || scheme == QStringLiteral("webcals")) {
    url.setScheme(QStringLiteral("https"));
    scheme = QStringLiteral("https");
  }
  if (!url.isValid() || url.host().isEmpty() || scheme != QStringLiteral("https")) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("Calendar subscriptions require an HTTPS or webcal URL");
    }
    return false;
  }
  if (!url.userInfo().isEmpty() || url.hasFragment()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "Put feed credentials in the credential fields, not in the URL");
    }
    return false;
  }
  url.setFragment({});
  if (normalized != nullptr) {
    *normalized = url.adjusted(QUrl::NormalizePathSegments);
  }
  return true;
}

bool IcsService::payloadFromRequest(const QJsonObject& request, QByteArray* payload,
                                    IcsError* error) {
  QByteArray loaded;
  if (request.value(QStringLiteral("content")).isString()) {
    loaded = request.value(QStringLiteral("content")).toString().toUtf8();
  } else if (request.value(QStringLiteral("contentBase64")).isString()) {
    const auto decoded = QByteArray::fromBase64Encoding(
        request.value(QStringLiteral("contentBase64")).toString().toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_base64"),
                  QStringLiteral("The iCalendar payload is not valid base64")};
      }
      return false;
    }
    loaded = decoded.decoded;
  } else if (request.value(QStringLiteral("path")).isString()) {
    const QString path = request.value(QStringLiteral("path")).toString();
    const QFileInfo info(path);
    if (!info.isAbsolute() || !info.isFile() || info.isSymLink() ||
        info.size() > kMaximumIcsBytes) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_path"),
                  QStringLiteral("The import path must name a regular local file")};
      }
      return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
      if (error != nullptr) {
        *error = {QStringLiteral("read_failed"),
                  QStringLiteral("The iCalendar file could not be read")};
      }
      return false;
    }
    loaded = file.read(kMaximumIcsBytes + 1);
  } else {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("Provide content, contentBase64, or an absolute path")};
    }
    return false;
  }
  if (loaded.isEmpty() || loaded.size() > kMaximumIcsBytes) {
    if (error != nullptr) {
      *error = {
          QStringLiteral("invalid_size"),
          QStringLiteral("The iCalendar payload must be between 1 byte and 16 MiB")};
    }
    return false;
  }
  if (payload != nullptr) {
    *payload = loaded;
  }
  return true;
}

void IcsService::start() {
  m_refreshTimer.start();
  QTimer::singleShot(0, this, [this]() { refreshAll(true); });
}

void IcsService::syncAll() { refreshAll(false); }

void IcsService::syncAccount(const QString& accountId) {
  QString ignored;
  refresh(accountId, &ignored);
}

bool IcsService::addSubscription(const QString& url, const QString& displayName,
                                 const int refreshSeconds, const QString& username,
                                 const QString& password, Account* account,
                                 QString* errorMessage) {
  if (m_database == nullptr || !m_database->isOpen()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Database is unavailable");
    }
    return false;
  }
  QUrl normalized;
  if (!normalizeSubscriptionUrl(url, &normalized, errorMessage)) {
    return false;
  }
  if (username.isEmpty() != password.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Both username and password are required");
    }
    return false;
  }

  Account created;
  created.id = newUuid();
  created.provider = ProviderKind::Ics;
  created.displayName =
      displayName.trimmed().isEmpty() ? normalized.host() : displayName.trimmed();
  created.endpoint = normalized.toString(QUrl::FullyEncoded);
  created.enabled = true;
  created.authStatus = username.isEmpty()
                           ? QStringLiteral("connected")
                           : QStringLiteral("credential_storage_pending");
  if (!m_database->upsertAccount(created, errorMessage)) {
    return false;
  }

  Calendar calendar;
  calendar.id = newUuid();
  calendar.accountId = created.id;
  calendar.remoteId = QStringLiteral("subscription");
  calendar.name = created.displayName;
  calendar.readOnly = true;
  calendar.enabled = true;
  calendar.capabilities = {
      {QStringLiteral("provider"), QStringLiteral("ics")},
      {QStringLiteral("createEvent"), false},
      {QStringLiteral("updateEvent"), false},
      {QStringLiteral("removeEvent"), false},
  };
  if (!m_database->upsertCalendar(calendar, errorMessage)) {
    disconnectAccount(created.id, true);
    return false;
  }

  IcsSubscription subscription;
  subscription.accountId = created.id;
  subscription.url = created.endpoint;
  subscription.refreshSeconds = refreshSeconds > 0
                                    ? qMax(kMinimumRefreshSeconds, refreshSeconds)
                                    : kDefaultRefreshSeconds;
  if (!m_database->upsertIcsSubscription(subscription, errorMessage)) {
    disconnectAccount(created.id, true);
    return false;
  }
  if (account != nullptr) {
    *account = m_database->account(created.id);
  }
  emit accountChanged(created.id);
  emit calendarsChanged(created.id);
  if (!username.isEmpty()) {
    storeSubscriptionCredentials(created.id, username, password);
  } else {
    QTimer::singleShot(0, this, [this, accountId = created.id]() {
      QString ignored;
      refresh(accountId, &ignored);
    });
  }
  return true;
}

void IcsService::storeSubscriptionCredentials(const QString& accountId,
                                              const QString& username,
                                              const QString& password) {
  cancelSecretOperations(accountId);
  const quint64 generation = m_credentialGeneration.value(accountId);
  const auto usernameOperation =
      std::make_shared<SecretStoreOperationId>(kInvalidSecretStoreOperationId);
  *usernameOperation = m_secrets.storeAsync(
      accountId, QStringLiteral("ics_username"), username,
      [this, accountId, password, generation,
       usernameOperation](SecretStoreResult usernameResult) {
        forgetSecretOperation(accountId, *usernameOperation);
        if (m_credentialGeneration.value(accountId) != generation) {
          return;
        }
        const Account current =
            m_database != nullptr ? m_database->account(accountId) : Account{};
        if (current.id.isEmpty() || current.provider != ProviderKind::Ics) {
          removeSubscriptionCredentials(accountId);
          return;
        }
        if (!usernameResult.success) {
          markCredentialsUnavailable(
              accountId,
              QStringLiteral("Calendar feed credentials could not be saved"));
          return;
        }
        const auto passwordOperation =
            std::make_shared<SecretStoreOperationId>(kInvalidSecretStoreOperationId);
        *passwordOperation = m_secrets.storeAsync(
            accountId, QStringLiteral("ics_password"), password,
            [this, accountId, generation,
             passwordOperation](SecretStoreResult passwordResult) {
              forgetSecretOperation(accountId, *passwordOperation);
              if (m_credentialGeneration.value(accountId) != generation) {
                return;
              }
              const Account latest =
                  m_database != nullptr ? m_database->account(accountId) : Account{};
              if (latest.id.isEmpty() || latest.provider != ProviderKind::Ics) {
                removeSubscriptionCredentials(accountId);
                return;
              }
              if (!passwordResult.success) {
                removeSubscriptionCredentials(accountId);
                markCredentialsUnavailable(
                    accountId,
                    QStringLiteral("Calendar feed credentials could not be saved"));
                return;
              }
              QString error;
              Account connected = latest;
              connected.authStatus = QStringLiteral("connected");
              if (!m_database->setProviderState(
                      accountId, {}, QStringLiteral("has_credentials"), true, &error) ||
                  !m_database->upsertAccount(connected, &error)) {
                removeSubscriptionCredentials(accountId);
                markCredentialsUnavailable(
                    accountId,
                    QStringLiteral(
                        "Calendar feed credential state could not be saved"));
                return;
              }
              emit accountChanged(accountId);
              QString ignored;
              refresh(accountId, &ignored);
            });
        m_secretOperations[accountId].append(*passwordOperation);
      });
  m_secretOperations[accountId].append(*usernameOperation);
}

void IcsService::removeSubscriptionCredentials(const QString& accountId) {
  cancelSecretOperations(accountId);
  const quint64 generation = m_credentialGeneration.value(accountId);
  const auto removeKind = [this, accountId, generation](const QString& kind) {
    const auto operation =
        std::make_shared<SecretStoreOperationId>(kInvalidSecretStoreOperationId);
    *operation = m_secrets.removeAsync(
        accountId, kind, [this, accountId, generation, operation](SecretStoreResult) {
          forgetSecretOperation(accountId, *operation);
          if (m_credentialGeneration.value(accountId) != generation) {
            return;
          }
        });
    m_secretOperations[accountId].append(*operation);
  };
  removeKind(QStringLiteral("ics_username"));
  removeKind(QStringLiteral("ics_password"));
}

void IcsService::cancelSecretOperations(const QString& accountId) {
  m_credentialGeneration.insert(accountId, m_credentialGeneration.value(accountId) + 1);
  const QList<SecretStoreOperationId> operations = m_secretOperations.take(accountId);
  for (const SecretStoreOperationId operation : operations) {
    m_secrets.cancel(operation);
  }
}

void IcsService::forgetSecretOperation(const QString& accountId,
                                       const SecretStoreOperationId operationId) {
  auto iterator = m_secretOperations.find(accountId);
  if (iterator == m_secretOperations.end()) {
    return;
  }
  iterator.value().removeOne(operationId);
  if (iterator.value().isEmpty()) {
    m_secretOperations.erase(iterator);
  }
}

void IcsService::markCredentialsUnavailable(const QString& accountId,
                                            const QString& message) {
  if (m_database == nullptr) {
    return;
  }
  Account account = m_database->account(accountId);
  if (account.id.isEmpty() || account.provider != ProviderKind::Ics) {
    return;
  }
  QString ignored;
  account.authStatus = QStringLiteral("reauthorization_required");
  m_database->setProviderState(accountId, {}, QStringLiteral("has_credentials"), false,
                               &ignored);
  m_database->upsertAccount(account, &ignored);
  const IcsSubscription subscription = m_database->icsSubscription(accountId);
  if (!subscription.accountId.isEmpty()) {
    m_database->updateIcsSubscriptionResult(accountId, {}, {}, {},
                                            QStringLiteral("credentials_unavailable"),
                                            message, &ignored);
  }
  emit accountChanged(accountId);
  emit syncStatusChanged(accountId, status(accountId));
}

bool IcsService::disconnectAccount(const QString& accountId,
                                   const bool removeCachedData, QString* errorMessage) {
  if (m_database == nullptr) {
    return false;
  }
  const Account account = m_database->account(accountId);
  if (account.provider != ProviderKind::Ics) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Account is not an ICS subscription");
    }
    return false;
  }
  removeSubscriptionCredentials(accountId);
  m_refreshing.remove(accountId);
  m_lastAttemptAt.remove(accountId);
  if (removeCachedData) {
    return m_database->removeAccount(accountId, errorMessage);
  }
  Account disconnected = account;
  disconnected.enabled = false;
  disconnected.authStatus = QStringLiteral("disconnected");
  if (!m_database->setProviderState(accountId, {}, QStringLiteral("has_credentials"),
                                    false, errorMessage) ||
      !m_database->upsertAccount(disconnected, errorMessage)) {
    return false;
  }
  emit accountChanged(accountId);
  return true;
}

bool IcsService::updateCredentials(const QString& accountId, const QString& username,
                                   const QString& password, QString* errorMessage) {
  if (m_database == nullptr || !m_database->isOpen()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Database is unavailable");
    }
    return false;
  }
  if (username.trimmed().isEmpty() != password.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("Provide both a username and password, or leave both empty");
    }
    return false;
  }
  Account account = m_database->account(accountId, errorMessage);
  if (account.id.isEmpty() || account.provider != ProviderKind::Ics) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = QStringLiteral("ICS subscription not found");
    }
    return false;
  }
  account.enabled = true;
  account.authStatus = username.trimmed().isEmpty()
                           ? QStringLiteral("connected")
                           : QStringLiteral("credential_storage_pending");
  if (!m_database->setProviderState(accountId, {}, QStringLiteral("has_credentials"),
                                    false, errorMessage) ||
      !m_database->upsertAccount(account, errorMessage)) {
    return false;
  }
  m_refreshing.remove(accountId);
  emit accountChanged(accountId);
  if (username.trimmed().isEmpty()) {
    removeSubscriptionCredentials(accountId);
    QTimer::singleShot(0, this, [this, accountId]() {
      QString ignored;
      refresh(accountId, &ignored);
    });
  } else {
    storeSubscriptionCredentials(accountId, username.trimmed(), password);
  }
  return true;
}

bool IcsService::refresh(const QString& accountId, QString* errorMessage) {
  if (m_database == nullptr || m_refreshing.contains(accountId)) {
    if (errorMessage != nullptr && m_refreshing.contains(accountId)) {
      *errorMessage = QStringLiteral("The subscription is already refreshing");
    }
    return false;
  }
  QString databaseError;
  const IcsSubscription subscription =
      m_database->icsSubscription(accountId, &databaseError);
  if (subscription.accountId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = databaseError;
    }
    return false;
  }
  const Account owner = m_database->account(accountId);
  if (owner.provider != ProviderKind::Ics || !owner.enabled) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("The ICS subscription is disconnected");
    }
    return false;
  }
  QUrl url;
  if (!normalizeSubscriptionUrl(subscription.url, &url, errorMessage)) {
    return false;
  }
  auto context = std::make_shared<FetchContext>();
  context->subscription = subscription;
  const bool hasCredentials =
      m_database->providerState(accountId, {}, QStringLiteral("has_credentials"), false)
          .toBool();
  m_refreshing.insert(accountId);
  m_lastAttemptAt.insert(accountId, QDateTime::currentDateTimeUtc());
  emit syncStatusChanged(accountId, status(accountId));
  if (!hasCredentials) {
    beginFetch(context, url, false);
    return true;
  }
  (void)m_secrets.lookupAsync(
      accountId, QStringLiteral("ics_username"),
      [this, context, url, accountId](SecretStoreResult usernameResult) {
        if (!m_refreshing.contains(accountId)) {
          return;
        }
        if (!usernameResult.success || usernameResult.value.isEmpty()) {
          finishFetch(context, QStringLiteral("credentials_unavailable"),
                      QStringLiteral("Calendar feed credentials are unavailable"));
          return;
        }
        context->username = usernameResult.value;
        (void)m_secrets.lookupAsync(
            accountId, QStringLiteral("ics_password"),
            [this, context, url, accountId](SecretStoreResult passwordResult) {
              if (!m_refreshing.contains(accountId)) {
                return;
              }
              if (!passwordResult.success || passwordResult.value.isEmpty()) {
                finishFetch(
                    context, QStringLiteral("credentials_unavailable"),
                    QStringLiteral("Calendar feed credentials are unavailable"));
                return;
              }
              context->password = passwordResult.value;
              beginFetch(context, url, true);
            });
      });
  return true;
}

void IcsService::refreshAll(const bool dueOnly) {
  if (m_database == nullptr || !m_database->isOpen()) {
    return;
  }
  const QDateTime now = QDateTime::currentDateTimeUtc();
  for (const IcsSubscription& subscription : m_database->icsSubscriptions()) {
    const Account account = m_database->account(subscription.accountId);
    if (!account.enabled || account.provider != ProviderKind::Ics) {
      continue;
    }
    if (m_refreshing.contains(subscription.accountId)) {
      continue;
    }
    const QDateTime basis =
        m_lastAttemptAt.value(subscription.accountId, subscription.lastSuccessAt);
    if (dueOnly && basis.isValid() && basis.secsTo(now) < subscription.refreshSeconds) {
      continue;
    }
    QString ignored;
    refresh(subscription.accountId, &ignored);
  }
}

bool IcsService::sameOrigin(const QUrl& first, const QUrl& second) {
  const auto port = [](const QUrl& url) {
    return url.port(url.scheme() == QStringLiteral("https") ? 443 : -1);
  };
  return first.scheme().compare(second.scheme(), Qt::CaseInsensitive) == 0 &&
         first.host().compare(second.host(), Qt::CaseInsensitive) == 0 &&
         port(first) == port(second);
}

void IcsService::beginFetch(const std::shared_ptr<FetchContext>& context,
                            const QUrl& url, const bool includeCredentials) {
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setTransferTimeout(kNetworkTimeoutMs);
  request.setRawHeader(QByteArrayLiteral("Accept"),
                       QByteArrayLiteral("text/calendar, text/plain;q=0.8"));
  request.setRawHeader(
      QByteArrayLiteral("User-Agent"),
      QByteArrayLiteral("OmaCalendar/") + QByteArrayLiteral(OMACALENDAR_VERSION));
  if (context->includeValidators && !context->subscription.etag.isEmpty()) {
    request.setRawHeader(QByteArrayLiteral("If-None-Match"),
                         context->subscription.etag.toUtf8());
  }
  if (context->includeValidators && !context->subscription.lastModified.isEmpty()) {
    request.setRawHeader(QByteArrayLiteral("If-Modified-Since"),
                         context->subscription.lastModified.toUtf8());
  }
  if (includeCredentials && !context->username.isEmpty()) {
    const QByteArray credential =
        (context->username + QLatin1Char(':') + context->password).toUtf8().toBase64();
    request.setRawHeader(QByteArrayLiteral("Authorization"),
                         QByteArrayLiteral("Basic ") + credential);
  }

  QNetworkReply* reply = m_network.get(request);
  auto* timeout = new QTimer(reply);
  timeout->setSingleShot(true);
  timeout->start(kNetworkTimeoutMs);
  connect(timeout, &QTimer::timeout, reply, [context, reply]() {
    context->timedOut = true;
    reply->abort();
  });
  connect(reply, &QIODevice::readyRead, this, [context, reply]() {
    context->body.append(reply->readAll());
    if (context->body.size() > kMaximumIcsBytes) {
      context->tooLarge = true;
      reply->abort();
    }
  });
  connect(
      reply, &QNetworkReply::finished, this,
      [this, context, reply, url, includeCredentials]() {
        context->body.append(reply->readAll());
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QUrl redirect =
            reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const QString etag =
            QString::fromUtf8(reply->rawHeader(QByteArrayLiteral("ETag")));
        const QString modified =
            QString::fromUtf8(reply->rawHeader(QByteArrayLiteral("Last-Modified")));
        const QNetworkReply::NetworkError networkError = reply->error();
        reply->deleteLater();

        if (context->tooLarge) {
          finishFetch(context, QStringLiteral("response_too_large"),
                      QStringLiteral("The calendar feed exceeded 16 MiB"));
          return;
        }
        if (context->timedOut) {
          finishFetch(context, QStringLiteral("timeout"),
                      QStringLiteral("The calendar feed request timed out"), true);
          return;
        }
        if (!redirect.isEmpty() && status >= 300 && status < 400) {
          if (++context->redirects > kMaximumRedirects) {
            finishFetch(context, QStringLiteral("too_many_redirects"),
                        QStringLiteral("The calendar feed redirected too many times"));
            return;
          }
          const QUrl target = url.resolved(redirect);
          QString urlError;
          QUrl normalized;
          if (!normalizeSubscriptionUrl(target.toString(), &normalized, &urlError)) {
            finishFetch(context, QStringLiteral("unsafe_redirect"), urlError);
            return;
          }
          if (includeCredentials && !sameOrigin(url, normalized)) {
            finishFetch(
                context, QStringLiteral("credential_redirect_blocked"),
                QStringLiteral(
                    "A credentialed calendar feed redirected to another origin"));
            return;
          }
          if (!sameOrigin(url, normalized)) {
            // Validators are opaque provider metadata and may themselves be
            // sensitive. Never forward them to a different origin.
            context->includeValidators = false;
          }
          context->body.clear();
          beginFetch(context, normalized, includeCredentials);
          return;
        }
        if (status == 304) {
          QString databaseError;
          const QDateTime now = QDateTime::currentDateTimeUtc();
          if (!m_database->updateIcsSubscriptionResult(context->subscription.accountId,
                                                       etag, modified, now, {}, {},
                                                       &databaseError)) {
            finishFetch(context, QStringLiteral("database_error"),
                        QStringLiteral("The subscription status could not be saved"));
            return;
          }
          const QList<Calendar> calendars =
              m_database->calendars(context->subscription.accountId);
          if (!calendars.isEmpty()) {
            Calendar calendar = calendars.first();
            calendar.lastSyncAt = now;
            if (!m_database->upsertCalendar(calendar, &databaseError)) {
              finishFetch(
                  context, QStringLiteral("database_error"),
                  QStringLiteral("The calendar refresh time could not be saved"));
              return;
            }
            emit calendarsChanged(context->subscription.accountId);
          }
          finishFetch(context);
          return;
        }
        if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
          const bool retryable = status == 408 || status == 425 || status == 429 ||
                                 status >= 500 || status == 0;
          finishFetch(context,
                      status > 0 ? QStringLiteral("http_%1").arg(status)
                                 : QStringLiteral("network_error"),
                      publicNetworkError(status, networkError), retryable);
          return;
        }
        QString applyError;
        if (!applyFeed(context->subscription, context->body, etag, modified,
                       &applyError)) {
          finishFetch(context, QStringLiteral("invalid_calendar"), applyError);
          return;
        }
        finishFetch(context);
      });
}

void IcsService::finishFetch(const std::shared_ptr<FetchContext>& context,
                             const QString& errorCode, const QString& errorMessage,
                             const bool retryable) {
  const QString accountId = context->subscription.accountId;
  if (!errorCode.isEmpty() && m_database != nullptr) {
    m_database->updateIcsSubscriptionResult(accountId, {}, {}, {}, errorCode,
                                            errorMessage);
    if (errorCode == QStringLiteral("http_401") ||
        errorCode == QStringLiteral("http_403") ||
        errorCode == QStringLiteral("credentials_unavailable")) {
      Account account = m_database->account(accountId);
      if (!account.id.isEmpty()) {
        account.authStatus = QStringLiteral("reauthorization_required");
        m_database->upsertAccount(account);
      }
    }
  } else if (m_database != nullptr) {
    Account account = m_database->account(accountId);
    if (!account.id.isEmpty() && account.authStatus != QStringLiteral("connected")) {
      account.authStatus = QStringLiteral("connected");
      m_database->upsertAccount(account);
    }
  }
  m_refreshing.remove(accountId);
  QJsonObject current = status(accountId);
  if (!errorCode.isEmpty()) {
    current.insert(QStringLiteral("retryable"), retryable);
  }
  emit syncStatusChanged(accountId, current);
  emit accountChanged(accountId);
}

QString IcsService::remoteIdentity(const Event& event) {
  return event.recurrenceId.isEmpty()
             ? event.uid
             : event.uid + QStringLiteral("#") + event.recurrenceId;
}

bool IcsService::applyFeed(const IcsSubscription& subscription,
                           const QByteArray& payload, const QString& etag,
                           const QString& lastModified, QString* errorMessage) {
  const caldav::ICalendarParseResult parsed = caldav::ICalendarCodec::parse(payload);
  const QByteArray upperPayload = payload.toUpper();
  const bool emptyCalendar = parsed.error.code == QStringLiteral("no_events") &&
                             upperPayload.contains("BEGIN:VCALENDAR") &&
                             upperPayload.contains("END:VCALENDAR");
  if (!parsed.ok() && !emptyCalendar) {
    if (errorMessage != nullptr) {
      *errorMessage = parsed.error.message.left(300);
    }
    return false;
  }
  const QList<Calendar> calendars =
      m_database->calendars(subscription.accountId, errorMessage);
  if (calendars.isEmpty()) {
    if (errorMessage != nullptr && errorMessage->isEmpty()) {
      *errorMessage = QStringLiteral("The subscription calendar is missing");
    }
    return false;
  }
  Calendar calendar = calendars.first();
  const QList<Event> previous =
      m_database->eventsForCalendars({calendar.id}, errorMessage);
  if (errorMessage != nullptr && !errorMessage->isEmpty()) {
    return false;
  }

  QSet<QString> retained;
  for (const Event& event : parsed.events) {
    const QString identity = remoteIdentity(event);
    if (identity.isEmpty() || retained.contains(identity)) {
      if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("The feed contains duplicate event identities");
      }
      return false;
    }
    retained.insert(identity);
  }
  QList<Event> replacementEvents;
  replacementEvents.reserve(parsed.events.size());
  for (Event event : parsed.events) {
    const QString identity = remoteIdentity(event);
    event.id.clear();
    event.calendarId = calendar.id;
    event.remoteId = identity;
    event.etag = etag;
    event.rawPayload.clear();
    event.rawFormat = QStringLiteral("text/calendar");
    event.timeKind = event.allDay ? TimeKind::AllDay
                                  : (event.startTimeZone.isEmpty() ? TimeKind::Floating
                                                                   : TimeKind::Zoned);
    event.dirty = false;
    event.deleted = false;
    replacementEvents.append(std::move(event));
  }

  QStringList staleRemoteIds;
  for (const Event& event : previous) {
    if (!retained.contains(event.remoteId)) {
      staleRemoteIds.append(event.remoteId);
    }
  }
  const QDateTime now = QDateTime::currentDateTimeUtc();
  calendar.lastSyncAt = now;
  calendar.etag = etag;
  if (!m_database->applyIcsFeedReplacement(calendar, replacementEvents, staleRemoteIds,
                                           etag, lastModified, now, errorMessage)) {
    return false;
  }
  emit eventsChanged({calendar.id});
  emit calendarsChanged(subscription.accountId);
  return true;
}

QJsonObject IcsService::subscriptionStatus(const IcsSubscription& subscription) const {
  const bool refreshing = m_refreshing.contains(subscription.accountId);
  const QDateTime now = QDateTime::currentDateTimeUtc();
  const bool stale = !subscription.lastSuccessAt.isValid() ||
                     subscription.lastSuccessAt.secsTo(now) >
                         std::max(7200, subscription.refreshSeconds * 2);
  QString state = QStringLiteral("idle");
  if (refreshing) {
    state = QStringLiteral("refreshing");
  } else if (!subscription.lastErrorCode.isEmpty()) {
    state = QStringLiteral("error");
  } else if (stale) {
    state = QStringLiteral("stale");
  }
  return {
      {QStringLiteral("accountId"), subscription.accountId},
      {QStringLiteral("provider"), QStringLiteral("ics")},
      {QStringLiteral("state"), state},
      {QStringLiteral("refreshSeconds"), subscription.refreshSeconds},
      {QStringLiteral("lastSuccessAt"), isoUtc(subscription.lastSuccessAt)},
      {QStringLiteral("stale"), stale},
      {QStringLiteral("errorCode"), subscription.lastErrorCode},
      {QStringLiteral("errorMessage"), subscription.lastErrorMessage},
  };
}

QJsonObject IcsService::status(const QString& accountId) const {
  if (m_database == nullptr || !m_database->isOpen()) {
    return {{QStringLiteral("state"), QStringLiteral("unavailable")}};
  }
  if (!accountId.isEmpty()) {
    const IcsSubscription subscription = m_database->icsSubscription(accountId);
    return subscription.accountId.isEmpty()
               ? QJsonObject{{QStringLiteral("accountId"), accountId},
                             {QStringLiteral("state"), QStringLiteral("not_found")}}
               : subscriptionStatus(subscription);
  }
  QJsonArray subscriptions;
  for (const IcsSubscription& subscription : m_database->icsSubscriptions()) {
    subscriptions.append(subscriptionStatus(subscription));
  }
  return {{QStringLiteral("provider"), QStringLiteral("ics")},
          {QStringLiteral("subscriptions"), subscriptions}};
}

QJsonObject IcsService::previewImport(const QByteArray& payload,
                                      const QString& destinationCalendarId,
                                      IcsError* error) const {
  if (!destinationCalendarId.isEmpty()) {
    const Calendar calendar = m_database->calendar(destinationCalendarId);
    if (calendar.id.isEmpty() || calendar.readOnly) {
      if (error != nullptr) {
        *error = {calendar.id.isEmpty() ? QStringLiteral("calendar_not_found")
                                        : QStringLiteral("calendar_read_only"),
                  calendar.id.isEmpty()
                      ? QStringLiteral("The destination calendar was not found")
                      : QStringLiteral("The destination calendar is read-only")};
      }
      return {};
    }
  }
  const caldav::ICalendarParseResult parsed = caldav::ICalendarCodec::parse(payload);
  if (!parsed.ok()) {
    if (error != nullptr) {
      *error = {parsed.error.code, parsed.error.message.left(300)};
    }
    return {};
  }
  QJsonArray candidates;
  int duplicateCount = 0;
  for (qsizetype index = 0; index < parsed.events.size(); ++index) {
    const Event& event = parsed.events.at(index);
    const Event duplicate = destinationCalendarId.isEmpty()
                                ? Event{}
                                : m_database->eventByUid(destinationCalendarId,
                                                         event.uid, event.recurrenceId);
    if (!duplicate.id.isEmpty()) {
      ++duplicateCount;
    }
    candidates.append(QJsonObject{
        {QStringLiteral("index"), static_cast<qint64>(index)},
        {QStringLiteral("event"), toJson(event)},
        {QStringLiteral("duplicate"), !duplicate.id.isEmpty()},
        {QStringLiteral("matchingEventId"), duplicate.id},
    });
  }
  return {{QStringLiteral("events"), candidates},
          {QStringLiteral("count"), candidates.size()},
          {QStringLiteral("duplicateCount"), duplicateCount},
          {QStringLiteral("destinationCalendarId"), destinationCalendarId}};
}

QJsonObject IcsService::commitImport(const QByteArray& payload,
                                     const QString& destinationCalendarId,
                                     const QString& duplicatePolicy, IcsError* error) {
  const QString policy = duplicatePolicy.trimmed().toLower();
  if (!QStringList{QStringLiteral("skip"), QStringLiteral("copy"),
                   QStringLiteral("replace")}
           .contains(policy)) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_duplicate_policy"),
                QStringLiteral("duplicatePolicy must be skip, copy, or replace")};
    }
    return {};
  }
  const Calendar calendar = m_database->calendar(destinationCalendarId);
  if (calendar.id.isEmpty() || calendar.readOnly) {
    if (error != nullptr) {
      *error = {calendar.id.isEmpty() ? QStringLiteral("calendar_not_found")
                                      : QStringLiteral("calendar_read_only"),
                calendar.id.isEmpty()
                    ? QStringLiteral("The destination calendar was not found")
                    : QStringLiteral("The destination calendar is read-only")};
    }
    return {};
  }
  const caldav::ICalendarParseResult parsed = caldav::ICalendarCodec::parse(payload);
  if (!parsed.ok()) {
    if (error != nullptr) {
      *error = {parsed.error.code, parsed.error.message.left(300)};
    }
    return {};
  }

  int imported = 0;
  int skipped = 0;
  int replaced = 0;
  QJsonArray eventIds;
  for (Event candidate : parsed.events) {
    candidate.timeKind = candidate.allDay
                             ? TimeKind::AllDay
                             : (candidate.startTimeZone.isEmpty() ? TimeKind::Floating
                                                                  : TimeKind::Zoned);
    Event duplicate = m_database->eventByUid(destinationCalendarId, candidate.uid,
                                             candidate.recurrenceId);
    if (!duplicate.id.isEmpty() && policy == QStringLiteral("skip")) {
      ++skipped;
      continue;
    }
    OutboxOperation operation = OutboxOperation::Create;
    qint64 expectedRevision = 0;
    if (!duplicate.id.isEmpty() && policy == QStringLiteral("replace")) {
      candidate.id = duplicate.id;
      candidate.calendarId = duplicate.calendarId;
      candidate.remoteId = duplicate.remoteId;
      candidate.uid = duplicate.uid;
      candidate.etag = duplicate.etag;
      candidate.createdAt = duplicate.createdAt;
      operation = OutboxOperation::Update;
      expectedRevision = duplicate.localRevision;
      ++replaced;
    } else {
      candidate.id.clear();
      candidate.calendarId = destinationCalendarId;
      candidate.remoteId.clear();
      candidate.etag.clear();
      if (!duplicate.id.isEmpty()) {
        candidate.uid = newUuid() + QStringLiteral("@omacalendar.local");
        candidate.rawPayload.clear();
      }
    }
    QString databaseError;
    if (!m_database->saveLocalEvent(&candidate, operation, &databaseError,
                                    QStringLiteral("ics-import-%1").arg(newUuid()),
                                    QStringLiteral("series"), QStringLiteral("none"),
                                    expectedRevision)) {
      if (error != nullptr) {
        *error = {QStringLiteral("import_failed"),
                  databaseError.isEmpty()
                      ? QStringLiteral("The imported event could not be saved")
                      : databaseError.left(300)};
      }
      return {};
    }
    ++imported;
    eventIds.append(candidate.id);
  }
  emit eventsChanged({destinationCalendarId});
  return {{QStringLiteral("imported"), imported},
          {QStringLiteral("skipped"), skipped},
          {QStringLiteral("replaced"), replaced},
          {QStringLiteral("eventIds"), eventIds},
          {QStringLiteral("revision"), m_database->changeRevision()}};
}

QByteArray IcsService::serializeEvents(const QList<Event>& events, IcsError* error) {
  QByteArray output = QByteArrayLiteral(
      "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//OmaCalendar//OmaCalendar//EN\r\n"
      "CALSCALE:GREGORIAN\r\n");
  QSet<QByteArray> components;
  for (const Event& event : events) {
    const caldav::ICalendarSerializeResult serialized =
        caldav::ICalendarCodec::serialize(
            event, QStringLiteral("-//OmaCalendar//OmaCalendar//EN"));
    if (!serialized.ok()) {
      if (error != nullptr) {
        *error = {serialized.error.code, serialized.error.message.left(300)};
      }
      return {};
    }
    QByteArray normalized = serialized.payload;
    normalized.replace("\r\n", "\n");
    const QList<QByteArray> lines = normalized.split('\n');
    QByteArray component;
    int depth = 0;
    for (const QByteArray& line : lines) {
      if (depth == 0 && (line == QByteArrayLiteral("BEGIN:VEVENT") ||
                         line == QByteArrayLiteral("BEGIN:VTIMEZONE"))) {
        component.clear();
        depth = 1;
      } else if (depth > 0 && line.startsWith(QByteArrayLiteral("BEGIN:"))) {
        ++depth;
      }
      if (depth > 0) {
        component.append(line);
        component.append("\r\n");
      }
      if (depth > 0 && line.startsWith(QByteArrayLiteral("END:"))) {
        --depth;
        if (depth == 0 && !components.contains(component)) {
          components.insert(component);
          output.append(component);
        }
      }
    }
  }
  output.append("END:VCALENDAR\r\n");
  return output;
}

QString IcsService::safeFileName(const QString& value) {
  QString result = value.trimmed();
  result.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")),
                 QStringLiteral("-"));
  result = result.left(80);
  return result.isEmpty() ? QStringLiteral("omacalendar-export") : result;
}

QJsonObject IcsService::exportCalendar(const QJsonObject& request,
                                       IcsError* error) const {
  QList<Event> events;
  bool scopeFound = false;
  QString title = QStringLiteral("omacalendar-export");
  QString databaseError;
  const QString eventId = request.value(QStringLiteral("eventId")).toString();
  const QString calendarId = request.value(QStringLiteral("calendarId")).toString();
  const QString calendarSetId =
      request.value(QStringLiteral("calendarSetId")).toString();
  const QDateTime start =
      dateTimeFromIso(request.value(QStringLiteral("start")).toString());
  const QDateTime end =
      dateTimeFromIso(request.value(QStringLiteral("end")).toString());

  if (!eventId.isEmpty()) {
    const Event event = m_database->event(eventId, &databaseError);
    if (!event.id.isEmpty()) {
      events.append(event);
      title = event.summary;
      scopeFound = true;
    }
  } else if (!calendarId.isEmpty()) {
    const Calendar calendar = m_database->calendar(calendarId, &databaseError);
    const Account account = m_database->account(calendar.accountId);
    if (!calendar.id.isEmpty() && account.provider != ProviderKind::Local) {
      if (error != nullptr) {
        *error = {
            QStringLiteral("not_local_calendar"),
            QStringLiteral("Whole-calendar export is limited to local calendars")};
      }
      return {};
    }
    if (!calendar.id.isEmpty()) {
      events = m_database->eventsForCalendars({calendar.id}, &databaseError);
      title = calendar.name;
      scopeFound = true;
    }
  } else if (!calendarSetId.isEmpty()) {
    for (const CalendarSet& set : m_database->calendarSets(&databaseError)) {
      if (set.id == calendarSetId) {
        if (!set.calendarIds.isEmpty()) {
          events = m_database->eventsForCalendars(set.calendarIds, &databaseError);
        }
        title = set.name;
        scopeFound = true;
        break;
      }
    }
    if (!scopeFound && databaseError.isEmpty()) {
      databaseError = QStringLiteral("Calendar set not found");
    }
  } else if (start.isValid() && end.isValid() && start < end) {
    QStringList calendarIds =
        calendarIdsFromJson(request.value(QStringLiteral("calendarIds")));
    if (calendarIds.isEmpty()) {
      for (const Calendar& calendar : m_database->calendars({}, &databaseError)) {
        if (calendar.enabled) {
          calendarIds.append(calendar.id);
        }
      }
    }
    const QList<Event> candidates =
        m_database->eventsForCalendars(calendarIds, &databaseError);
    for (const Event& event : candidates) {
      if (!event.recurrenceRule.isEmpty() ||
          (eventStart(event).isValid() && eventEnd(event).isValid() &&
           eventEnd(event) > start && eventStart(event) < end)) {
        events.append(event);
      }
    }
    title =
        QStringLiteral("omacalendar-%1-to-%2")
            .arg(start.date().toString(Qt::ISODate), end.date().toString(Qt::ISODate));
    scopeFound = true;
  } else {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_scope"),
                QStringLiteral("Export requires eventId, local calendarId, "
                               "calendarSetId, or a date range")};
    }
    return {};
  }
  if (!databaseError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), databaseError.left(300)};
    }
    return {};
  }
  if (!scopeFound) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"),
                QStringLiteral("The requested export source was not found")};
    }
    return {};
  }

  QByteArray content = serializeEvents(events, error);
  if (content.isEmpty()) {
    return {};
  }
  const QString filename = safeFileName(title) + QStringLiteral(".ics");
  const QString outputPath = request.value(QStringLiteral("outputPath")).toString();
  if (!outputPath.isEmpty()) {
    const QFileInfo info(outputPath);
    if (!info.isAbsolute() ||
        info.suffix().compare(QStringLiteral("ics"), Qt::CaseInsensitive) != 0 ||
        !info.dir().exists() ||
        (info.exists() && !request.value(QStringLiteral("overwrite")).toBool())) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_output_path"),
                  QStringLiteral("Use an absolute .ics path in an existing directory; "
                                 "confirm overwrite if it exists")};
      }
      return {};
    }
    QSaveFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() ||
        !file.commit()) {
      if (error != nullptr) {
        *error = {QStringLiteral("write_failed"),
                  QStringLiteral("The iCalendar export could not be written")};
      }
      return {};
    }
  }
  return {{QStringLiteral("content"), QString::fromUtf8(content)},
          {QStringLiteral("mimeType"), QStringLiteral("text/calendar")},
          {QStringLiteral("fileName"), filename},
          {QStringLiteral("path"), outputPath},
          {QStringLiteral("count"), events.size()}};
}

}  // namespace omacalendar::ics
