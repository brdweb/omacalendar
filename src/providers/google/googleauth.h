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

 private:
  struct Session;

  Session* createSession(const QString& accountId, bool interactive,
                         QString* errorMessage);
  [[nodiscard]] Session* activeSession(const QString& accountId,
                                       quint64 generation) const;
  void destroySession(const QString& accountId);
  void fail(Session* session, const QString& code, const QString& message);
  bool persistRefreshToken(Session* session, QString* errorMessage = nullptr);

  QNetworkAccessManager m_network;
  // OAuth objects cancelled from inside one of their own signals must use
  // deleteLater(). Keep ownership here so they are still destroyed before the
  // member network manager if the application exits first.
  QObjectCleanupHandler m_retiredObjects;
  SecretStore m_secrets;
  QHash<QString, Session*> m_sessions;
  QString m_clientId;
  QString m_clientSecret;
  quint64 m_nextSessionGeneration = 0;
};

}  // namespace omacalendar::google
