#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>
#include <functional>

namespace omacalendar::google {

class GoogleAuthManager;

struct ApiResponse {
  bool ok = false;
  int httpStatus = 0;
  QJsonObject body;
  QString etag;
  QString errorCode;
  QString errorMessage;
  bool authenticationRequired = false;
  bool retryable = false;
  int retryAfterSeconds = 0;
};

class GoogleClient final : public QObject {
  Q_OBJECT

 public:
  using Callback = std::function<void(ApiResponse)>;

  explicit GoogleClient(GoogleAuthManager* auth, QObject* parent = nullptr);

  void listCalendars(const QString& accountId, const QString& pageToken,
                     const QString& syncToken, Callback callback);
  void listEvents(const QString& accountId, const QString& calendarRemoteId,
                  const QString& pageToken, const QString& syncToken,
                  Callback callback);
  void createEvent(const QString& accountId, const QString& calendarRemoteId,
                   const QJsonObject& event, Callback callback);
  void updateEvent(const QString& accountId, const QString& calendarRemoteId,
                   const QString& eventRemoteId, const QString& etag,
                   const QJsonObject& event, Callback callback);
  void deleteEvent(const QString& accountId, const QString& calendarRemoteId,
                   const QString& eventRemoteId, const QString& etag,
                   Callback callback);

 private:
  void request(const QString& accountId, const QByteArray& verb, QUrl url,
               const QJsonObject& body, const QString& etag, Callback callback);

  GoogleAuthManager* m_auth = nullptr;
  QNetworkAccessManager m_network;
};

}  // namespace omacalendar::google
