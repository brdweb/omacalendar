#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QObjectCleanupHandler>
#include <QUrl>

#include "core/secretstore.h"

class QOAuth2AuthorizationCodeFlow;
class QOAuthHttpServerReplyHandler;

namespace omacalendar::google {

class GoogleAuthManager final : public QObject {
  Q_OBJECT

 public:
  explicit GoogleAuthManager(QObject* parent = nullptr);
  ~GoogleAuthManager() override;

  bool configureClient(const QString& clientId, const QString& clientSecret,
                       QString* errorMessage = nullptr);
  bool restoreClient(const QString& clientId, QString* errorMessage = nullptr);
  [[nodiscard]] QString clientId() const;
  [[nodiscard]] bool isConfigured() const;
  [[nodiscard]] bool hasAccessToken(const QString& accountId) const;
  [[nodiscard]] QString accessToken(const QString& accountId) const;

  bool startAuthorization(const QString& accountId, QString* errorMessage = nullptr);
  bool restoreAuthorization(const QString& accountId, QString* errorMessage = nullptr);
  bool refresh(const QString& accountId, QString* errorMessage = nullptr);
  bool forget(const QString& accountId, QString* errorMessage = nullptr);
  void cancel(const QString& accountId);

 signals:
  void authorizationUrlReady(const QString& accountId, const QUrl& url);
  void authorized(const QString& accountId);
  void tokenChanged(const QString& accountId);
  void authorizationFailed(const QString& accountId, const QString& code,
                           const QString& message);
  void clientConfigurationFinished(bool success, const QString& code,
                                   const QString& message);
  void forgetFinished(const QString& accountId, bool success, const QString& code,
                      const QString& message);

 private:
  struct Session;
  struct PendingAuthorization;
  struct PendingForget;

  Session* createSession(const QString& accountId, bool interactive,
                         QString* errorMessage);
  [[nodiscard]] Session* activeSession(const QString& accountId,
                                       quint64 generation) const;
  void destroySession(const QString& accountId);
  [[nodiscard]] PendingAuthorization* activePendingAuthorization(
      const QString& accountId, quint64 generation) const;
  void destroyPendingAuthorization(const QString& accountId);
  bool queueAuthorization(const QString& accountId, bool interactive,
                          QString* errorMessage);
  void continuePendingAuthorization(const QString& accountId, quint64 generation);
  void continuePendingAuthorizations();
  void fail(Session* session, const QString& code, const QString& message);
  void persistRefreshToken(Session* session);
  void completePendingTokenNotifications(Session* session);
  void cancelClientSecretOperation();

  QNetworkAccessManager m_network;
  // OAuth objects cancelled from inside one of their own signals must use
  // deleteLater(). Keep ownership here so they are still destroyed before the
  // member network manager if the application exits first.
  QObjectCleanupHandler m_retiredObjects;
  AsyncSecretStore m_secrets;
  QHash<QString, Session*> m_sessions;
  QHash<QString, PendingAuthorization*> m_pendingAuthorizations;
  QHash<QString, PendingForget*> m_pendingForgets;
  QString m_clientId;
  QString m_clientSecret;
  SecretStoreOperationId m_clientSecretOperation = kInvalidSecretStoreOperationId;
  quint64 m_clientSecretGeneration = 0;
  bool m_clientReady = false;
  quint64 m_nextSessionGeneration = 0;
  quint64 m_nextAuthorizationGeneration = 0;
  quint64 m_nextForgetGeneration = 0;
};

}  // namespace omacalendar::google
