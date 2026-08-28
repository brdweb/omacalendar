#pragma once

#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>
#include <functional>

namespace omacalendar::caldav {

struct DavResponse {
  bool ok = false;
  int httpStatus = 0;
  QByteArray body;
  QString etag;
  QString errorCode;
  QString errorMessage;
  bool authenticationRequired = false;
  bool retryable = false;
  int retryAfterSeconds = 0;
};

class CalDavClient final : public QObject {
  Q_OBJECT

 public:
  using Callback = std::function<void(DavResponse)>;

  explicit CalDavClient(QObject* parent = nullptr);

  static bool validateEndpoint(const QUrl& endpoint, QString* errorMessage = nullptr);
  void setCredentials(const QString& accountId, const QString& username,
                      const QString& password);
  void forgetCredentials(const QString& accountId);

  void discoverPrincipal(const QString& accountId, const QUrl& endpoint,
                         Callback callback);
  void discoverHome(const QString& accountId, const QUrl& principalUrl,
                    Callback callback);
  void discoverCalendars(const QString& accountId, const QUrl& homeUrl,
                         Callback callback);
  void queryCalendar(const QString& accountId, const QUrl& calendarUrl,
                     const QDateTime& startUtc, const QDateTime& endUtc,
                     Callback callback);
  void syncCollection(const QString& accountId, const QUrl& calendarUrl,
                      const QString& syncToken, Callback callback);
  void createEvent(const QString& accountId, const QUrl& resourceUrl,
                   const QByteArray& icalendar, Callback callback);
  void updateEvent(const QString& accountId, const QUrl& resourceUrl,
                   const QString& etag, const QByteArray& icalendar, Callback callback);
  void deleteEvent(const QString& accountId, const QUrl& resourceUrl,
                   const QString& etag, Callback callback);

 private:
  struct Credential {
    QString username;
    QString password;
  };

  void request(const QString& accountId, const QByteArray& verb, const QUrl& url,
               const QByteArray& body, const QByteArray& contentType,
               const QByteArray& depth, const QString& etag, bool createOnly,
               Callback callback);

  QHash<QString, Credential> m_credentials;
  QNetworkAccessManager m_network;
};

}  // namespace omacalendar::caldav
