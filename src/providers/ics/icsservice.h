#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <memory>

#include "core/database.h"
#include "core/secretstore.h"
#include "sync/provider.h"

namespace omacalendar::ics {

struct IcsError {
  QString code;
  QString message;
  bool retryable = false;

  [[nodiscard]] bool isEmpty() const { return code.isEmpty(); }
};

// Owns read-only RFC 5545 subscriptions and the import/export workflows.  The
// service is the only layer that can see feed URLs, HTTP validators, response
// bodies, or optional credentials; all values returned to clients are reduced
// to presentation-safe status and Event DTOs.
class IcsService final : public Provider {
  Q_OBJECT

 public:
  explicit IcsService(Database* database, QObject* parent = nullptr);

  static bool normalizeSubscriptionUrl(const QString& input, QUrl* normalized,
                                       QString* errorMessage = nullptr);
  static bool payloadFromRequest(const QJsonObject& request, QByteArray* payload,
                                 IcsError* error);

  [[nodiscard]] ProviderCapabilities capabilities() const override;
  void start() override;
  void syncAll() override;
  void syncAccount(const QString& accountId) override;
  bool addSubscription(const QString& url, const QString& displayName,
                       int refreshSeconds, const QString& username,
                       const QString& password, Account* account,
                       QString* errorMessage = nullptr);
  bool disconnectAccount(const QString& accountId, bool removeCachedData,
                         QString* errorMessage = nullptr);
  bool updateCredentials(const QString& accountId, const QString& username,
                         const QString& password, QString* errorMessage = nullptr);
  bool refresh(const QString& accountId, QString* errorMessage = nullptr);
  void refreshAll(bool dueOnly = false);

  [[nodiscard]] QJsonObject status(const QString& accountId = {}) const override;
  [[nodiscard]] QJsonObject previewImport(const QByteArray& payload,
                                          const QString& destinationCalendarId,
                                          IcsError* error) const;
  [[nodiscard]] QJsonObject commitImport(const QByteArray& payload,
                                         const QString& destinationCalendarId,
                                         const QString& duplicatePolicy,
                                         IcsError* error);
  [[nodiscard]] QJsonObject exportCalendar(const QJsonObject& request,
                                           IcsError* error) const;

 private:
  struct FetchContext;

  static bool sameOrigin(const QUrl& first, const QUrl& second);
  static QString remoteIdentity(const Event& event);
  static QByteArray serializeEvents(const QList<Event>& events, IcsError* error);
  static QString safeFileName(const QString& value);

  void beginFetch(const std::shared_ptr<FetchContext>& context, const QUrl& url,
                  bool includeCredentials);
  void storeSubscriptionCredentials(const QString& accountId, const QString& username,
                                    const QString& password);
  void removeSubscriptionCredentials(const QString& accountId);
  void cancelSecretOperations(const QString& accountId);
  void forgetSecretOperation(const QString& accountId,
                             SecretStoreOperationId operationId);
  void markCredentialsUnavailable(const QString& accountId, const QString& message);
  void finishFetch(const std::shared_ptr<FetchContext>& context,
                   const QString& errorCode = {}, const QString& errorMessage = {},
                   bool retryable = false);
  bool applyFeed(const IcsSubscription& subscription, const QByteArray& payload,
                 const QString& etag, const QString& lastModified,
                 QString* errorMessage);
  [[nodiscard]] QJsonObject subscriptionStatus(
      const IcsSubscription& subscription) const;

  Database* m_database = nullptr;
  AsyncSecretStore m_secrets;
  QNetworkAccessManager m_network;
  QTimer m_refreshTimer;
  QSet<QString> m_refreshing;
  QHash<QString, QDateTime> m_lastAttemptAt;
  QHash<QString, QList<SecretStoreOperationId>> m_secretOperations;
  QHash<QString, quint64> m_credentialGeneration;
};

}  // namespace omacalendar::ics
