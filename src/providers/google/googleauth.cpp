#include "providers/google/googleauth.h"

#include <QAbstractOAuth>
#include <QAbstractOAuthReplyHandler>
#include <QHostAddress>
#include <QMetaEnum>
#include <QOAuth2AuthorizationCodeFlow>
#include <QOAuthHttpServerReplyHandler>
#include <QPointer>
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
  const char* name = metaEnum.valueToKey(static_cast<int>(error));
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
};

GoogleAuthManager::GoogleAuthManager(QObject* parent) : QObject(parent) {}

GoogleAuthManager::~GoogleAuthManager() {
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
  if (!clientSecret.isEmpty() &&
      !m_secrets.store(normalizedClientId, QString::fromLatin1(kClientSecretKind),
                       clientSecret, errorMessage)) {
    return false;
  }
  m_clientId = normalizedClientId;
  m_clientSecret = clientSecret;
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
  // Desktop/public clients may legitimately have no client secret. Treat an
  // absent keyring item as an empty optional secret; token refresh will still
  // surface an authentication error if this particular client requires one.
  m_clientId = normalizedClientId;
  m_clientSecret =
      m_secrets.lookup(normalizedClientId, QString::fromLatin1(kClientSecretKind));
  if (m_clientSecret.isEmpty() && normalizedClientId == defaultOAuthClientId()) {
    m_clientSecret = defaultOAuthClientSecret();
  }
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
            QString error;
            if (!persistRefreshToken(active, &error)) {
              fail(active, QStringLiteral("keyring_error"), error);
              return;
            }
            const bool shouldEmitAuthorized = !active->completionEmitted;
            if (shouldEmitAuthorized) {
              active->completionEmitted = true;
            }
            if (active->refreshInFlight) {
              active->refreshCompletionNotified = true;
            }
            emit tokenChanged(sessionAccountId);
            // A tokenChanged observer may synchronously disconnect the account.
            if (shouldEmitAuthorized &&
                activeSession(sessionAccountId, sessionGeneration) != nullptr) {
              emit authorized(sessionAccountId);
            }
          });
  connect(session->flow, &QAbstractOAuth::granted, this,
          [this, sessionAccountId, sessionGeneration]() {
            Session* active = activeSession(sessionAccountId, sessionGeneration);
            if (active == nullptr) {
              return;
            }
            QString error;
            if (!persistRefreshToken(active, &error)) {
              fail(active, QStringLiteral("keyring_error"), error);
              return;
            }
            if (active->replyHandler != nullptr) {
              active->replyHandler->close();
            }
            // close() and its observers are allowed to cancel synchronously.
            active = activeSession(sessionAccountId, sessionGeneration);
            const bool shouldNotifyRefresh =
                active != nullptr && active->refreshInFlight &&
                !active->refreshCompletionNotified && active->flow != nullptr &&
                !active->flow->token().isEmpty();
            if (active != nullptr) {
              active->refreshInFlight = false;
              active->refreshCompletionNotified = false;
            }
            if (shouldNotifyRefresh) {
              emit tokenChanged(sessionAccountId);
              active = activeSession(sessionAccountId, sessionGeneration);
            }
            if (active != nullptr && !active->completionEmitted) {
              active->completionEmitted = true;
              emit authorized(sessionAccountId);
            }
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
  if (accountId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Account ID is required");
    }
    return false;
  }
  Session* session = createSession(accountId, true, errorMessage);
  if (session == nullptr) {
    return false;
  }
  session->flow->grant();
  return true;
}

bool GoogleAuthManager::restoreAuthorization(const QString& accountId,
                                             QString* errorMessage) {
  QString lookupError;
  const QString refreshToken =
      m_secrets.lookup(accountId, QString::fromLatin1(kRefreshTokenKind), &lookupError);
  if (refreshToken.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = lookupError.isEmpty()
                          ? QStringLiteral("Google account needs authorization")
                          : lookupError;
    }
    return false;
  }
  Session* session = createSession(accountId, false, errorMessage);
  if (session == nullptr) {
    return false;
  }
  session->flow->setRefreshToken(refreshToken);
  session->refreshInFlight = true;
  session->refreshCompletionNotified = false;
  session->flow->refreshTokens();
  return true;
}

bool GoogleAuthManager::refresh(const QString& accountId, QString* errorMessage) {
  Session* session = m_sessions.value(accountId, nullptr);
  if (session == nullptr) {
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
  destroySession(accountId);
  QString removalError;
  if (!m_secrets.remove(accountId, QString::fromLatin1(kRefreshTokenKind),
                        &removalError)) {
    // Secret Service treats an absent item as a failure. Forgetting an account
    // remains idempotent unless a concrete keyring error was reported.
    if (!removalError.contains(QStringLiteral("exit code 1")) &&
        !removalError.isEmpty()) {
      if (errorMessage != nullptr) {
        *errorMessage = removalError;
      }
      return false;
    }
  }
  return true;
}

void GoogleAuthManager::cancel(const QString& accountId) { destroySession(accountId); }

void GoogleAuthManager::destroySession(const QString& accountId) {
  Session* session = m_sessions.take(accountId);
  if (session == nullptr) {
    return;
  }
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

bool GoogleAuthManager::persistRefreshToken(Session* session, QString* errorMessage) {
  if (session == nullptr || session->flow == nullptr ||
      session->flow->refreshToken().isEmpty()) {
    return true;
  }
  return m_secrets.store(session->accountId, QString::fromLatin1(kRefreshTokenKind),
                         session->flow->refreshToken(), errorMessage);
}

}  // namespace omacalendar::google
