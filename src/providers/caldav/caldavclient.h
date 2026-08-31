#pragma once

#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QStringList>
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
  [[nodiscard]] static bool isSameOrigin(const QUrl& first, const QUrl& second);
  [[nodiscard]] static QUrl canonicalUrl(const QUrl& base, const QUrl& href);
  [[nodiscard]] static QString canonicalResourceId(const QUrl& base,
                                                   const QString& href);
  [[nodiscard]] static constexpr qsizetype maximumResponseBytes() {
    return 16 * 1024 * 1024;
  }
  [[nodiscard]] static constexpr qsizetype maximumMultiGetHrefs() { return 100; }
  [[nodiscard]] static constexpr qsizetype maximumMultiGetRequestBytes() {
    return 256 * 1024;
  }
  [[nodiscard]] static QList<QStringList> batchMultiGetHrefs(const QStringList& hrefs);
  void setCredentials(const QString& accountId, const QString& username,
                      const QString& password, const QUrl& endpoint);
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
  void calendarMultiGet(const QString& accountId, const QUrl& calendarUrl,
                        const QStringList& hrefs, Callback callback);
  void listResourceEtags(const QString& accountId, const QUrl& calendarUrl,
                         Callback callback);
  void readResource(const QString& accountId, const QUrl& resourceUrl,
                    Callback callback);
  void createEvent(const QString& accountId, const QUrl& resourceUrl,
                   const QByteArray& icalendar, Callback callback,
                   const QByteArray& scheduleReply = {});
  void updateEvent(const QString& accountId, const QUrl& resourceUrl,
                   const QString& etag, const QByteArray& icalendar, Callback callback,
                   const QByteArray& scheduleReply = {});
  void deleteEvent(const QString& accountId, const QUrl& resourceUrl,
                   const QString& etag, Callback callback,
                   const QByteArray& scheduleReply = {});
  void moveEvent(const QString& accountId, const QUrl& sourceResourceUrl,
                 const QUrl& targetResourceUrl, const QString& etag, Callback callback,
                 const QByteArray& scheduleReply = {});

 private:
  struct Credential {
    QString username;
    QString password;
    QUrl origin;
  };

  void request(const QString& accountId, const QByteArray& verb, const QUrl& url,
               const QByteArray& body, const QByteArray& contentType,
               const QByteArray& depth, const QString& etag, bool createOnly,
               Callback callback, const QByteArray& scheduleReply = {},
               const QUrl& moveDestination = {});

  QHash<QString, Credential> m_credentials;
  QNetworkAccessManager m_network;
};

}  // namespace omacalendar::caldav
