#include "providers/google/googleauth.h"

#include <QAbstractOAuth>
#include <QAbstractOAuthReplyHandler>
#include <QHostAddress>
#include <QMetaEnum>
#include <QOAuth2AuthorizationCodeFlow>
#include <QOAuthHttpServerReplyHandler>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include "core/domain.h"
#include "providers/google/googleoauthconfig.h"

namespace omacalendar::google {
namespace {

constexpr auto kAuthorizationUrl = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr auto kTokenUrl = "https://oauth2.googleapis.com/token";
constexpr auto kClientSecretKind = "client-secret";
constexpr auto kRefreshTokenKind = "refresh-token";

QString oauthErrorName(const QAbstractOAuth::Error error) {
  const QMetaEnum metaEnum = QMetaEnum::fromType<QAbstractOAuth::Error>();
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
  const char* name = metaEnum.valueToKey(static_cast<quint64>(error));
#else
  const char* name = metaEnum.valueToKey(static_cast<int>(error));
#endif
  return name == nullptr ? QStringLiteral("oauth_error") : QString::fromLatin1(name);
}

}  // namespace

struct GoogleAuthManager::Session final {
  QString accountId;
  quint64 generation = 0;
  QPointer<QOAuth2AuthorizationCodeFlow> flow;
  QPointer<QOAuthHttpServerReplyHandler> replyHandler;
  bool interactive = false;
  bool completionEmitted = false;
  bool failureInProgress = false;
  bool refreshInFlight = false;
  bool refreshCompletionNotified = false;
  bool grantObserved = false;
  bool tokenNotificationPending = false;
  bool authorizationNotificationPending = false;
  QString persistedRefreshToken;
  QString persistingRefreshToken;
  SecretStoreOperationId persistOperation = kInvalidSecretStoreOperationId;
  quint64 persistGeneration = 0;
};

struct GoogleAuthManager::PendingAuthorization final {
  QString accountId;
  quint64 generation = 0;
  bool interactive = false;
  bool lookupComplete = false;
  QString refreshToken;
  SecretStoreOperationId lookupOperation = kInvalidSecretStoreOperationId;
};

struct GoogleAuthManager::PendingForget final {
  quint64 generation = 0;
  SecretStoreOperationId operation = kInvalidSecretStoreOperationId;
};

GoogleAuthManager::GoogleAuthManager(QObject* parent) : QObject(parent) {}

GoogleAuthManager::~GoogleAuthManager() {
  ++m_clientSecretGeneration;
  m_clientSecretOperation = kInvalidSecretStoreOperationId;
  qDeleteAll(m_pendingAuthorizations);
  m_pendingAuthorizations.clear();
  qDeleteAll(m_pendingForgets);
  m_pendingForgets.clear();
  m_secrets.cancelAll();
  m_retiredObjects.clear();
  const auto sessions = m_sessions.values();
  m_sessions.clear();
  for (Session* session : sessions) {
    if (session->replyHandler != nullptr) {
      session->replyHandler->close();
    }
    if (session->flow != nullptr) {
      disconnect(session->flow, nullptr, this, nullptr);
      session->flow->setReplyHandler(nullptr);
    }
    if (session->replyHandler != nullptr) {
      disconnect(session->replyHandler, nullptr, this, nullptr);
    }
    // The OAuth objects reference the member network manager, so destroy them
    // before member teardown rather than leaving them to QObject's destructor.
    delete session->flow.data();
    delete session->replyHandler.data();
    delete session;
  }
}

bool GoogleAuthManager::configureClient(const QString& clientId,
                                        const QString& clientSecret,
                                        QString* errorMessage) {
  const QString normalizedClientId = clientId.trimmed();
  if (normalizedClientId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Google OAuth client ID is required");
    }
    return false;
  }
  cancelClientSecretOperation();
  m_clientId = normalizedClientId;
  m_clientSecret = clientSecret;
  m_clientReady = true;
  if (clientSecret.isEmpty()) {
    const quint64 generation = m_clientSecretGeneration;
    QTimer::singleShot(0, this, [this, generation]() {
      if (generation == m_clientSecretGeneration) {
        emit clientConfigurationFinished(true, {}, {});
      }
    });
    continuePendingAuthorizations();
    return true;
  }

  const quint64 generation = m_clientSecretGeneration;
  m_clientSecretOperation =
      m_secrets.storeAsync(normalizedClientId, QString::fromLatin1(kClientSecretKind),
                           clientSecret, [this, generation](SecretStoreResult result) {
                             if (generation != m_clientSecretGeneration) {
                               return;
                             }
                             m_clientSecretOperation = kInvalidSecretStoreOperationId;
                             emit clientConfigurationFinished(
                                 result.success, result.errorCode,
                                 result.success ? QString() : result.errorMessage);
                           });
  continuePendingAuthorizations();
  return true;
}

bool GoogleAuthManager::restoreClient(const QString& clientId, QString* errorMessage) {
  const QString normalizedClientId = clientId.trimmed();
  if (normalizedClientId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Google OAuth client ID is required");
    }
    return false;
  }
  cancelClientSecretOperation();
  // Desktop/public clients may legitimately have no client secret. An absent
  // keyring item therefore resolves to the configured fallback (usually empty).
  m_clientId = normalizedClientId;
  m_clientSecret = normalizedClientId == defaultOAuthClientId()
                       ? defaultOAuthClientSecret()
                       : QString();
  m_clientReady = false;
  const quint64 generation = m_clientSecretGeneration;
  m_clientSecretOperation = m_secrets.lookupAsync(
      normalizedClientId, QString::fromLatin1(kClientSecretKind),
      [this, normalizedClientId, generation](SecretStoreResult result) {
        if (generation != m_clientSecretGeneration ||
            normalizedClientId != m_clientId) {
          return;
        }
        m_clientSecretOperation = kInvalidSecretStoreOperationId;
        if (result.success && !result.value.isEmpty()) {
          m_clientSecret = result.value;
        }
        m_clientReady = true;
        // Failure is intentionally non-fatal for public OAuth clients. Any
        // client which really requires its missing shared key will be rejected
        // by Google's token endpoint with a sanitized authentication error.
        emit clientConfigurationFinished(true, {}, {});
        continuePendingAuthorizations();
      });
  return true;
}

QString GoogleAuthManager::clientId() const { return m_clientId; }
bool GoogleAuthManager::isConfigured() const { return !m_clientId.isEmpty(); }

bool GoogleAuthManager::hasAccessToken(const QString& accountId) const {
  return !accessToken(accountId).isEmpty();
}

QString GoogleAuthManager::accessToken(const QString& accountId) const {
  const Session* session = m_sessions.value(accountId, nullptr);
  return session == nullptr || session->flow == nullptr ? QString()
                                                        : session->flow->token();
}

GoogleAuthManager::Session* GoogleAuthManager::activeSession(
    const QString& accountId, const quint64 generation) const {
  Session* session = m_sessions.value(accountId, nullptr);
  return session != nullptr && session->generation == generation ? session : nullptr;
}

GoogleAuthManager::Session* GoogleAuthManager::createSession(const QString& accountId,
                                                             const bool interactive,
                                                             QString* errorMessage) {
  if (!isConfigured()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Google OAuth client is not configured");
    }
    return nullptr;
  }
  destroySession(accountId);

  auto* session = new Session;
  session->accountId = accountId;
  session->generation = ++m_nextSessionGeneration;
  session->interactive = interactive;
  session->flow = new QOAuth2AuthorizationCodeFlow(&m_network, this);
  session->flow->setClientIdentifier(m_clientId);
  if (!m_clientSecret.isEmpty()) {
    session->flow->setClientIdentifierSharedKey(m_clientSecret);
  }
  session->flow->setAuthorizationUrl(QUrl(QString::fromLatin1(kAuthorizationUrl)));
  session->flow->setTokenUrl(QUrl(QString::fromLatin1(kTokenUrl)));
  session->flow->setPkceMethod(QOAuth2AuthorizationCodeFlow::PkceMethod::S256, 64);
  session->flow->setRequestedScopeTokens({
      QByteArrayLiteral("https://www.googleapis.com/auth/calendar.events"),
      QByteArrayLiteral("https://www.googleapis.com/auth/calendar.calendars"),
      QByteArrayLiteral(
          "https://www.googleapis.com/auth/calendar.calendarlist.readonly"),
  });
  session->flow->setState(newUuid());
  session->flow->setUserAgent(
      QStringLiteral("OmaCalendar/%1").arg(QStringLiteral(OMACALENDAR_VERSION)));
  session->flow->setAutoRefresh(true);
  session->flow->setModifyParametersFunction(
      [interactive](const QAbstractOAuth::Stage stage,
                    QMultiMap<QString, QVariant>* parameters) {
        if (stage != QAbstractOAuth::Stage::RequestingAuthorization ||
            parameters == nullptr) {
          return;
        }
        parameters->insert(QStringLiteral("access_type"), QStringLiteral("offline"));
        parameters->insert(QStringLiteral("include_granted_scopes"),
                           QStringLiteral("true"));
        if (interactive) {
          parameters->insert(QStringLiteral("prompt"), QStringLiteral("consent"));
        }
      });

  if (interactive) {
    // Every QOAuthHttpServerReplyHandler constructor starts listening. Use
    // the address/port constructor directly; calling listen() a second time
    // fails because the random loopback port is already active.
    session->replyHandler =
        new QOAuthHttpServerReplyHandler(QHostAddress::LocalHost, 0, this);
    session->replyHandler->setCallbackHost(QStringLiteral("127.0.0.1"));
    session->replyHandler->setCallbackPath(QStringLiteral("/oauth2callback"));
    session->replyHandler->setCallbackText(QStringLiteral(
        "OmaCalendar is connected. You can close this tab and return to the "
        "application."));
    if (!session->replyHandler->isListening()) {
      if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("Could not open the local OAuth callback");
      }
      session->replyHandler->close();
      session->flow->setReplyHandler(nullptr);
      delete session->flow.data();
      delete session->replyHandler.data();
      delete session;
      return nullptr;
    }
    session->flow->setReplyHandler(session->replyHandler);
  }

  const QString sessionAccountId = session->accountId;
  const quint64 sessionGeneration = session->generation;
  connect(session->flow, &QAbstractOAuth::authorizeWithBrowser, this,
          [this, sessionAccountId, sessionGeneration](const QUrl& url) {
            if (activeSession(sessionAccountId, sessionGeneration) == nullptr) {
              return;
            }
            emit authorizationUrlReady(sessionAccountId, url);
          });
  connect(session->flow, &QAbstractOAuth::tokenChanged, this,
          [this, sessionAccountId, sessionGeneration](const QString& token) {
            if (token.isEmpty()) {
              return;
            }
            Session* active = activeSession(sessionAccountId, sessionGeneration);
            if (active == nullptr) {
              return;
            }
            active->tokenNotificationPending = true;
            active->authorizationNotificationPending =
                active->authorizationNotificationPending || !active->completionEmitted;
            persistRefreshToken(active);
          });
  connect(session->flow, &QAbstractOAuth::granted, this,
          [this, sessionAccountId, sessionGeneration]() {
            Session* active = activeSession(sessionAccountId, sessionGeneration);
            if (active == nullptr) {
              return;
            }
            if (active->replyHandler != nullptr) {
              active->replyHandler->close();
            }
            // close() and its observers are allowed to cancel synchronously.
            active = activeSession(sessionAccountId, sessionGeneration);
            if (active == nullptr) {
              return;
            }
            active->grantObserved = true;
            active->tokenNotificationPending =
                active->tokenNotificationPending ||
                (active->refreshInFlight && !active->refreshCompletionNotified &&
                 active->flow != nullptr && !active->flow->token().isEmpty());
            active->authorizationNotificationPending =
                active->authorizationNotificationPending || !active->completionEmitted;
            persistRefreshToken(active);
          });
  connect(
      session->flow, &QAbstractOAuth::requestFailed, this,
      [this, sessionAccountId, sessionGeneration](const QAbstractOAuth::Error error) {
        Session* active = activeSession(sessionAccountId, sessionGeneration);
        if (active == nullptr) {
          return;
        }
        fail(active, oauthErrorName(error),
             QStringLiteral("Google authorization request failed"));
      });
  connect(session->flow, &QAbstractOAuth2::serverReportedErrorOccurred, this,
          [this, sessionAccountId, sessionGeneration](
              const QString& code, const QString& description, const QUrl&) {
            Session* active = activeSession(sessionAccountId, sessionGeneration);
            if (active == nullptr) {
              return;
            }
            fail(active, code.left(64),
                 description.isEmpty() ? QStringLiteral("Google rejected authorization")
                                       : description.left(300));
          });
  if (session->replyHandler != nullptr) {
    connect(session->replyHandler,
            &QAbstractOAuthReplyHandler::tokenRequestErrorOccurred, this,
            [this, sessionAccountId, sessionGeneration](
                const QAbstractOAuth::Error error, const QString&) {
              Session* active = activeSession(sessionAccountId, sessionGeneration);
              if (active == nullptr) {
                return;
              }
              fail(active, oauthErrorName(error),
                   QStringLiteral("Google token exchange failed"));
            });
  }
  m_sessions.insert(accountId, session);
  return session;
}

bool GoogleAuthManager::startAuthorization(const QString& accountId,
                                           QString* errorMessage) {
  return queueAuthorization(accountId, true, errorMessage);
}

bool GoogleAuthManager::restoreAuthorization(const QString& accountId,
                                             QString* errorMessage) {
  if (PendingAuthorization* pending = m_pendingAuthorizations.value(accountId);
      pending != nullptr && !pending->interactive) {
    return true;
  }
  return queueAuthorization(accountId, false, errorMessage);
}

bool GoogleAuthManager::refresh(const QString& accountId, QString* errorMessage) {
  Session* session = m_sessions.value(accountId, nullptr);
  if (session == nullptr) {
    if (PendingAuthorization* pending = m_pendingAuthorizations.value(accountId);
        pending != nullptr && !pending->interactive) {
      return true;
    }
    return restoreAuthorization(accountId, errorMessage);
  }
  if (session->flow == nullptr || session->flow->refreshToken().isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Google refresh token is unavailable");
    }
    return false;
  }
  session->refreshInFlight = true;
  session->refreshCompletionNotified = false;
  session->flow->refreshTokens();
  return true;
}

bool GoogleAuthManager::forget(const QString& accountId, QString* errorMessage) {
  if (accountId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Account ID is required");
    }
    return false;
  }
  if (m_pendingForgets.contains(accountId)) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("Google account disconnect is already in progress");
    }
    return false;
  }

  cancel(accountId);
  auto* pending = new PendingForget;
  pending->generation = ++m_nextForgetGeneration;
  const quint64 generation = pending->generation;
  m_pendingForgets.insert(accountId, pending);
  pending->operation = m_secrets.removeAsync(
      accountId, QString::fromLatin1(kRefreshTokenKind),
      [this, accountId, generation](SecretStoreResult result) {
        PendingForget* active = m_pendingForgets.value(accountId, nullptr);
        if (active == nullptr || active->generation != generation) {
          return;
        }
        m_pendingForgets.remove(accountId);
        delete active;
        const bool success =
            result.success || result.errorCode == QStringLiteral("not_found");
        emit forgetFinished(accountId, success, success ? QString() : result.errorCode,
                            success ? QString() : result.errorMessage);
      });
  return true;
}

void GoogleAuthManager::cancel(const QString& accountId) {
  destroyPendingAuthorization(accountId);
  destroySession(accountId);
}

GoogleAuthManager::PendingAuthorization* GoogleAuthManager::activePendingAuthorization(
    const QString& accountId, const quint64 generation) const {
  PendingAuthorization* pending = m_pendingAuthorizations.value(accountId, nullptr);
  return pending != nullptr && pending->generation == generation ? pending : nullptr;
}

void GoogleAuthManager::destroyPendingAuthorization(const QString& accountId) {
  PendingAuthorization* pending = m_pendingAuthorizations.take(accountId);
  if (pending == nullptr) {
    return;
  }
  const SecretStoreOperationId operation = pending->lookupOperation;
  pending->lookupOperation = kInvalidSecretStoreOperationId;
  delete pending;
  if (operation != kInvalidSecretStoreOperationId) {
    m_secrets.cancel(operation);
  }
}

bool GoogleAuthManager::queueAuthorization(const QString& accountId,
                                           const bool interactive,
                                           QString* errorMessage) {
  if (accountId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Account ID is required");
    }
    return false;
  }
  if (!isConfigured()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Google OAuth client is not configured");
    }
    return false;
  }
  if (m_pendingForgets.contains(accountId)) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Google account disconnect is in progress");
    }
    return false;
  }

  destroyPendingAuthorization(accountId);
  destroySession(accountId);
  auto* pending = new PendingAuthorization;
  pending->accountId = accountId;
  pending->generation = ++m_nextAuthorizationGeneration;
  pending->interactive = interactive;
  pending->lookupComplete = interactive;
  const quint64 generation = pending->generation;
  m_pendingAuthorizations.insert(accountId, pending);
  if (!interactive) {
    pending->lookupOperation = m_secrets.lookupAsync(
        accountId, QString::fromLatin1(kRefreshTokenKind),
        [this, accountId, generation](SecretStoreResult result) {
          PendingAuthorization* active =
              activePendingAuthorization(accountId, generation);
          if (active == nullptr) {
            return;
          }
          active->lookupOperation = kInvalidSecretStoreOperationId;
          if (!result.success || result.value.isEmpty()) {
            m_pendingAuthorizations.remove(accountId);
            delete active;
            emit authorizationFailed(
                accountId, QStringLiteral("authentication_required"),
                result.errorMessage.isEmpty()
                    ? QStringLiteral("Google account needs authorization")
                    : result.errorMessage);
            return;
          }
          active->refreshToken = result.value;
          active->lookupComplete = true;
          continuePendingAuthorization(accountId, generation);
        });
  }
  continuePendingAuthorization(accountId, generation);
  return true;
}

void GoogleAuthManager::continuePendingAuthorization(const QString& accountId,
                                                     const quint64 generation) {
  PendingAuthorization* pending = activePendingAuthorization(accountId, generation);
  if (pending == nullptr || !pending->lookupComplete || !m_clientReady) {
    return;
  }
  const bool interactive = pending->interactive;
  const QString refreshToken = pending->refreshToken;
  m_pendingAuthorizations.remove(accountId);
  delete pending;

  QString error;
  Session* session = createSession(accountId, interactive, &error);
  if (session == nullptr) {
    emit authorizationFailed(
        accountId, QStringLiteral("authorization_start_failed"),
        error.isEmpty() ? QStringLiteral("Could not start Google authorization")
                        : error);
    return;
  }
  if (interactive) {
    session->flow->grant();
    return;
  }
  session->flow->setRefreshToken(refreshToken);
  session->persistedRefreshToken = refreshToken;
  session->refreshInFlight = true;
  session->refreshCompletionNotified = false;
  session->flow->refreshTokens();
}

void GoogleAuthManager::continuePendingAuthorizations() {
  const QStringList accountIds = m_pendingAuthorizations.keys();
  for (const QString& accountId : accountIds) {
    PendingAuthorization* pending = m_pendingAuthorizations.value(accountId, nullptr);
    if (pending != nullptr) {
      continuePendingAuthorization(accountId, pending->generation);
    }
  }
}

void GoogleAuthManager::destroySession(const QString& accountId) {
  Session* session = m_sessions.take(accountId);
  if (session == nullptr) {
    return;
  }
  const SecretStoreOperationId persistOperation = session->persistOperation;
  session->persistOperation = kInvalidSecretStoreOperationId;
  ++session->persistGeneration;
  if (session->replyHandler != nullptr) {
    session->replyHandler->close();
  }
  if (session->flow != nullptr) {
    disconnect(session->flow, nullptr, this, nullptr);
    session->flow->setReplyHandler(nullptr);
    m_retiredObjects.add(session->flow);
    session->flow->deleteLater();
  }
  if (session->replyHandler != nullptr) {
    disconnect(session->replyHandler, nullptr, this, nullptr);
    m_retiredObjects.add(session->replyHandler);
    session->replyHandler->deleteLater();
  }
  delete session;
  if (persistOperation != kInvalidSecretStoreOperationId) {
    m_secrets.cancel(persistOperation);
  }
}

void GoogleAuthManager::fail(Session* session, const QString& code,
                             const QString& message) {
  if (session == nullptr ||
      activeSession(session->accountId, session->generation) != session ||
      session->failureInProgress) {
    return;
  }
  const QString accountId = session->accountId;
  const quint64 generation = session->generation;
  session->failureInProgress = true;
  if (session->replyHandler != nullptr) {
    session->replyHandler->close();
  }
  emit authorizationFailed(accountId, code, message);
  // Reacquire after emitting: observers may have forgotten/cancelled it.
  session = activeSession(accountId, generation);
  if (session != nullptr) {
    session->failureInProgress = false;
  }
}

void GoogleAuthManager::persistRefreshToken(Session* session) {
  if (session == nullptr || session->flow == nullptr ||
      session->flow->refreshToken().isEmpty()) {
    completePendingTokenNotifications(session);
    return;
  }
  const QString refreshToken = session->flow->refreshToken();
  if (session->persistedRefreshToken == refreshToken) {
    completePendingTokenNotifications(session);
    return;
  }
  if (session->persistOperation != kInvalidSecretStoreOperationId &&
      session->persistingRefreshToken == refreshToken) {
    return;
  }

  const SecretStoreOperationId previousOperation = session->persistOperation;
  session->persistOperation = kInvalidSecretStoreOperationId;
  const QString accountId = session->accountId;
  const quint64 sessionGeneration = session->generation;
  const quint64 persistGeneration = ++session->persistGeneration;
  session->persistingRefreshToken = refreshToken;
  if (previousOperation != kInvalidSecretStoreOperationId) {
    m_secrets.cancel(previousOperation);
  }
  session->persistOperation = m_secrets.storeAsync(
      accountId, QString::fromLatin1(kRefreshTokenKind), refreshToken,
      [this, accountId, sessionGeneration, persistGeneration,
       refreshToken](SecretStoreResult result) {
        Session* active = activeSession(accountId, sessionGeneration);
        if (active == nullptr || active->persistGeneration != persistGeneration) {
          return;
        }
        active->persistOperation = kInvalidSecretStoreOperationId;
        active->persistingRefreshToken.clear();
        if (!result.success) {
          fail(active, QStringLiteral("keyring_error"), result.errorMessage);
          return;
        }
        active->persistedRefreshToken = refreshToken;
        completePendingTokenNotifications(active);
      });
}

void GoogleAuthManager::completePendingTokenNotifications(Session* session) {
  if (session == nullptr ||
      activeSession(session->accountId, session->generation) != session) {
    return;
  }
  const QString accountId = session->accountId;
  const quint64 generation = session->generation;
  const bool notifyToken = session->tokenNotificationPending;
  const bool notifyAuthorized =
      session->authorizationNotificationPending && !session->completionEmitted;
  session->tokenNotificationPending = false;
  session->authorizationNotificationPending = false;
  if (notifyToken && session->refreshInFlight) {
    session->refreshCompletionNotified = true;
  }
  if (session->grantObserved) {
    session->grantObserved = false;
    session->refreshInFlight = false;
    session->refreshCompletionNotified = false;
  }
  if (notifyAuthorized) {
    session->completionEmitted = true;
  }

  if (notifyToken) {
    emit tokenChanged(accountId);
  }
  session = activeSession(accountId, generation);
  if (session != nullptr && notifyAuthorized) {
    emit authorized(accountId);
  }
}

void GoogleAuthManager::cancelClientSecretOperation() {
  const SecretStoreOperationId operation = m_clientSecretOperation;
  m_clientSecretOperation = kInvalidSecretStoreOperationId;
  ++m_clientSecretGeneration;
  if (operation != kInvalidSecretStoreOperationId) {
    m_secrets.cancel(operation);
  }
}

}  // namespace omacalendar::google
