#include "providers/caldav/caldavclient.h"

#include <QDateTime>
#include <QHostAddress>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace omacalendar::caldav {
namespace {

QByteArray timestamp(const QDateTime& dateTime) {
  return dateTime.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1();
}

DavResponse responseFromReply(QNetworkReply* reply) {
  DavResponse result;
  result.httpStatus =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  result.body = reply->readAll();
  result.etag = QString::fromUtf8(reply->rawHeader(QByteArrayLiteral("ETag")));
  result.ok = reply->error() == QNetworkReply::NoError && result.httpStatus >= 200 &&
              result.httpStatus < 300;
  if (result.ok) {
    return result;
  }
  result.authenticationRequired = result.httpStatus == 401;
  result.retryable =
      result.httpStatus == 408 || result.httpStatus == 425 ||
      result.httpStatus == 429 || result.httpStatus >= 500 ||
      (result.httpStatus == 0 && reply->error() != QNetworkReply::NoError);
  result.errorCode = result.httpStatus > 0
                         ? QStringLiteral("http_%1").arg(result.httpStatus)
                         : QStringLiteral("network_error");
  result.errorMessage =
      result.authenticationRequired ? QStringLiteral("CalDAV credentials were rejected")
      : result.httpStatus > 0
          ? QStringLiteral("CalDAV request failed (%1)").arg(result.httpStatus)
          : QStringLiteral("CalDAV network request failed");
  bool ok = false;
  result.retryAfterSeconds =
      QString::fromLatin1(reply->rawHeader(QByteArrayLiteral("Retry-After")))
          .toInt(&ok);
  if (!ok) {
    result.retryAfterSeconds = 0;
  }
  return result;
}

QByteArray principalBody() {
  return QByteArrayLiteral(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
      "<d:current-user-principal/>"
      "</d:prop></d:propfind>");
}

QByteArray homeBody() {
  return QByteArrayLiteral(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<d:propfind xmlns:d=\"DAV:\" "
      "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop>"
      "<c:calendar-home-set/>"
      "</d:prop></d:propfind>");
}

QByteArray calendarsBody() {
  return QByteArrayLiteral(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<d:propfind xmlns:d=\"DAV:\" "
      "xmlns:c=\"urn:ietf:params:xml:ns:caldav\" "
      "xmlns:a=\"http://apple.com/ns/ical/\" "
      "xmlns:cs=\"http://calendarserver.org/ns/\"><d:prop>"
      "<d:resourcetype/><d:displayname/><c:calendar-description/>"
      "<a:calendar-color/><cs:getctag/><d:sync-token/>"
      "<d:current-user-privilege-set/>"
      "</d:prop></d:propfind>");
}

QByteArray queryBody(const QDateTime& startUtc, const QDateTime& endUtc) {
  return QByteArrayLiteral(
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
             "<c:calendar-query xmlns:d=\"DAV:\" "
             "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop>"
             "<d:getetag/><c:calendar-data/></d:prop><c:filter>"
             "<c:comp-filter name=\"VCALENDAR\"><c:comp-filter "
             "name=\"VEVENT\"><c:time-range start=\"") +
         timestamp(startUtc) + QByteArrayLiteral("\" end=\"") + timestamp(endUtc) +
         QByteArrayLiteral(
             "\"/></c:comp-filter></c:comp-filter>"
             "</c:filter></c:calendar-query>");
}

QByteArray syncBody(const QString& syncToken) {
  return QByteArrayLiteral(
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
             "<d:sync-collection xmlns:d=\"DAV:\" "
             "xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
             "<d:sync-token>") +
         syncToken.toHtmlEscaped().toUtf8() +
         QByteArrayLiteral(
             "</d:sync-token><d:sync-level>1</d:sync-level><d:prop>"
             "<d:getetag/><c:calendar-data/></d:prop>"
             "</d:sync-collection>");
}

}  // namespace

CalDavClient::CalDavClient(QObject* parent) : QObject(parent) {}

bool CalDavClient::validateEndpoint(const QUrl& endpoint, QString* errorMessage) {
  if (!endpoint.isValid() || endpoint.host().isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("A valid CalDAV URL is required");
    }
    return false;
  }
  if (endpoint.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0) {
    return true;
  }
  const QHostAddress address(endpoint.host());
  const bool local =
      endpoint.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0 ||
      (address.isLoopback());
  if (endpoint.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 &&
      local) {
    return true;
  }
  if (errorMessage != nullptr) {
    *errorMessage = QStringLiteral(
        "CalDAV requires HTTPS (HTTP is allowed only for localhost testing)");
  }
  return false;
}

void CalDavClient::setCredentials(const QString& accountId, const QString& username,
                                  const QString& password) {
  m_credentials.insert(accountId, {username, password});
}

void CalDavClient::forgetCredentials(const QString& accountId) {
  m_credentials.remove(accountId);
}

void CalDavClient::discoverPrincipal(const QString& accountId, const QUrl& endpoint,
                                     Callback callback) {
  request(accountId, QByteArrayLiteral("PROPFIND"), endpoint, principalBody(),
          QByteArrayLiteral("application/xml; charset=utf-8"), QByteArrayLiteral("0"),
          {}, false, std::move(callback));
}

void CalDavClient::discoverHome(const QString& accountId, const QUrl& principalUrl,
                                Callback callback) {
  request(accountId, QByteArrayLiteral("PROPFIND"), principalUrl, homeBody(),
          QByteArrayLiteral("application/xml; charset=utf-8"), QByteArrayLiteral("0"),
          {}, false, std::move(callback));
}

void CalDavClient::discoverCalendars(const QString& accountId, const QUrl& homeUrl,
                                     Callback callback) {
  request(accountId, QByteArrayLiteral("PROPFIND"), homeUrl, calendarsBody(),
          QByteArrayLiteral("application/xml; charset=utf-8"), QByteArrayLiteral("1"),
          {}, false, std::move(callback));
}

void CalDavClient::queryCalendar(const QString& accountId, const QUrl& calendarUrl,
                                 const QDateTime& startUtc, const QDateTime& endUtc,
                                 Callback callback) {
  request(accountId, QByteArrayLiteral("REPORT"), calendarUrl,
          queryBody(startUtc, endUtc),
          QByteArrayLiteral("application/xml; charset=utf-8"), QByteArrayLiteral("1"),
          {}, false, std::move(callback));
}

void CalDavClient::syncCollection(const QString& accountId, const QUrl& calendarUrl,
                                  const QString& syncToken, Callback callback) {
  request(accountId, QByteArrayLiteral("REPORT"), calendarUrl, syncBody(syncToken),
          QByteArrayLiteral("application/xml; charset=utf-8"), QByteArrayLiteral("1"),
          {}, false, std::move(callback));
}

void CalDavClient::createEvent(const QString& accountId, const QUrl& resourceUrl,
                               const QByteArray& icalendar, Callback callback) {
  request(accountId, QByteArrayLiteral("PUT"), resourceUrl, icalendar,
          QByteArrayLiteral("text/calendar; charset=utf-8"), {}, {}, true,
          std::move(callback));
}

void CalDavClient::updateEvent(const QString& accountId, const QUrl& resourceUrl,
                               const QString& etag, const QByteArray& icalendar,
                               Callback callback) {
  request(accountId, QByteArrayLiteral("PUT"), resourceUrl, icalendar,
          QByteArrayLiteral("text/calendar; charset=utf-8"), {}, etag, false,
          std::move(callback));
}

void CalDavClient::deleteEvent(const QString& accountId, const QUrl& resourceUrl,
                               const QString& etag, Callback callback) {
  request(accountId, QByteArrayLiteral("DELETE"), resourceUrl, {}, {}, {}, etag, false,
          std::move(callback));
}

void CalDavClient::request(const QString& accountId, const QByteArray& verb,
                           const QUrl& url, const QByteArray& body,
                           const QByteArray& contentType, const QByteArray& depth,
                           const QString& etag, const bool createOnly,
                           Callback callback) {
  QString endpointError;
  if (!validateEndpoint(url, &endpointError)) {
    DavResponse response;
    response.errorCode = QStringLiteral("invalid_endpoint");
    response.errorMessage = endpointError;
    QTimer::singleShot(0, this, [callback = std::move(callback), response]() mutable {
      callback(response);
    });
    return;
  }
  const auto credentials = m_credentials.constFind(accountId);
  if (credentials == m_credentials.constEnd()) {
    DavResponse response;
    response.errorCode = QStringLiteral("authentication_required");
    response.errorMessage = QStringLiteral("CalDAV credentials are unavailable");
    response.authenticationRequired = true;
    QTimer::singleShot(0, this, [callback = std::move(callback), response]() mutable {
      callback(response);
    });
    return;
  }
  QNetworkRequest networkRequest(url);
  networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                              QNetworkRequest::NoLessSafeRedirectPolicy);
  networkRequest.setTransferTimeout(30000);
  const QByteArray basic =
      (credentials->username + QLatin1Char(':') + credentials->password)
          .toUtf8()
          .toBase64();
  networkRequest.setRawHeader(QByteArrayLiteral("Authorization"),
                              QByteArrayLiteral("Basic ") + basic);
  networkRequest.setRawHeader(
      QByteArrayLiteral("User-Agent"),
      QByteArrayLiteral("OmaCalendar/") + QByteArrayLiteral(OMACALENDAR_VERSION));
  if (!contentType.isEmpty()) {
    networkRequest.setRawHeader(QByteArrayLiteral("Content-Type"), contentType);
  }
  if (!depth.isEmpty()) {
    networkRequest.setRawHeader(QByteArrayLiteral("Depth"), depth);
  }
  if (createOnly) {
    networkRequest.setRawHeader(QByteArrayLiteral("If-None-Match"),
                                QByteArrayLiteral("*"));
  } else if (!etag.isEmpty()) {
    networkRequest.setRawHeader(QByteArrayLiteral("If-Match"), etag.toUtf8());
  }
  QNetworkReply* reply = m_network.sendCustomRequest(networkRequest, verb, body);
  connect(reply, &QNetworkReply::finished, this,
          [reply, callback = std::move(callback)]() mutable {
            const DavResponse response = responseFromReply(reply);
            reply->deleteLater();
            callback(response);
          });
}

}  // namespace omacalendar::caldav
