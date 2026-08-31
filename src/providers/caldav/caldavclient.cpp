#include "providers/caldav/caldavclient.h"

#include <QDateTime>
#include <QHostAddress>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <memory>

namespace omacalendar::caldav {
namespace {

QByteArray timestamp(const QDateTime& dateTime) {
  return dateTime.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")).toLatin1();
}

struct ReplyBody final {
  QByteArray value;
  bool tooLarge = false;
};

int effectivePort(const QUrl& url) {
  if (url.port() >= 0) {
    return url.port();
  }
  return url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 ? 443
                                                                                 : 80;
}

DavResponse responseFromReply(QNetworkReply* reply, const QByteArray& body,
                              const bool tooLarge) {
  DavResponse result;
  result.httpStatus =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (tooLarge) {
    result.errorCode = QStringLiteral("response_too_large");
    result.errorMessage =
        QStringLiteral("The CalDAV response exceeded the 16 MiB safety limit");
    return result;
  }
  result.body = body;
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
      "<c:calendar-home-set/><c:calendar-user-address-set/>"
      "<c:schedule-inbox-URL/><c:schedule-outbox-URL/>"
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

QByteArray multiGetBody(const QStringList& hrefs) {
  QByteArray body = QByteArrayLiteral(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<c:calendar-multiget xmlns:d=\"DAV:\" "
      "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop>"
      "<d:getetag/><c:calendar-data/></d:prop>");
  for (const QString& href : hrefs) {
    body += QByteArrayLiteral("<d:href>");
    body += href.toHtmlEscaped().toUtf8();
    body += QByteArrayLiteral("</d:href>");
  }
  body += QByteArrayLiteral("</c:calendar-multiget>");
  return body;
}

QByteArray etagListBody() {
  return QByteArrayLiteral(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:getetag/>"
      "</d:prop></d:propfind>");
}

}  // namespace

CalDavClient::CalDavClient(QObject* parent) : QObject(parent) {}

bool CalDavClient::validateEndpoint(const QUrl& endpoint, QString* errorMessage) {
  if (!endpoint.isValid() || endpoint.host().isEmpty() || endpoint.isRelative()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("A valid CalDAV URL is required");
    }
    return false;
  }
  if (!endpoint.userInfo().isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("CalDAV credentials must not be embedded in the endpoint URL");
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

bool CalDavClient::isSameOrigin(const QUrl& first, const QUrl& second) {
  return first.scheme().compare(second.scheme(), Qt::CaseInsensitive) == 0 &&
         first.host().compare(second.host(), Qt::CaseInsensitive) == 0 &&
         effectivePort(first) == effectivePort(second);
}

QUrl CalDavClient::canonicalUrl(const QUrl& base, const QUrl& href) {
  QUrl result = href.isRelative() ? base.resolved(href) : href;
  result = result.adjusted(QUrl::NormalizePathSegments | QUrl::RemoveFragment |
                           QUrl::RemoveUserInfo);
  const bool defaultHttps =
      result.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 &&
      result.port() == 443;
  const bool defaultHttp =
      result.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 &&
      result.port() == 80;
  if (defaultHttps || defaultHttp) {
    result.setPort(-1);
  }
  result.setScheme(result.scheme().toLower());
  result.setHost(result.host().toLower());
  return result;
}

QString CalDavClient::canonicalResourceId(const QUrl& base, const QString& href) {
  return canonicalUrl(base, QUrl(href)).toString(QUrl::FullyEncoded);
}

QList<QStringList> CalDavClient::batchMultiGetHrefs(const QStringList& hrefs) {
  QList<QStringList> result;
  QStringList batch;
  for (const QString& href : hrefs) {
    QStringList candidate = batch;
    candidate.append(href);
    if (!batch.isEmpty() &&
        (candidate.size() > maximumMultiGetHrefs() ||
         multiGetBody(candidate).size() > maximumMultiGetRequestBytes())) {
      result.append(batch);
      batch = {href};
    } else {
      batch = std::move(candidate);
    }
  }
  if (!batch.isEmpty()) {
    result.append(batch);
  }
  return result;
}

void CalDavClient::setCredentials(const QString& accountId, const QString& username,
                                  const QString& password, const QUrl& endpoint) {
  m_credentials.insert(accountId,
                       {username, password, canonicalUrl(endpoint, endpoint)});
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

void CalDavClient::calendarMultiGet(const QString& accountId, const QUrl& calendarUrl,
                                    const QStringList& hrefs, Callback callback) {
  if (hrefs.isEmpty()) {
    DavResponse response;
    response.ok = true;
    response.httpStatus = 207;
    response.body = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:multistatus xmlns:d=\"DAV:\"/>");
    QTimer::singleShot(0, this, [callback = std::move(callback), response]() mutable {
      callback(response);
    });
    return;
  }
  const QByteArray body = multiGetBody(hrefs);
  if (hrefs.size() > maximumMultiGetHrefs() ||
      body.size() > maximumMultiGetRequestBytes()) {
    DavResponse response;
    response.errorCode = QStringLiteral("multiget_request_too_large");
    response.errorMessage =
        QStringLiteral("The calendar-multiget batch exceeded its safe request limit");
    QTimer::singleShot(0, this, [callback = std::move(callback), response]() mutable {
      callback(response);
    });
    return;
  }
  request(accountId, QByteArrayLiteral("REPORT"), calendarUrl, body,
          QByteArrayLiteral("application/xml; charset=utf-8"), QByteArrayLiteral("1"),
          {}, false, std::move(callback));
}

void CalDavClient::readResource(const QString& accountId, const QUrl& resourceUrl,
                                Callback callback) {
  const QUrl canonicalResource = canonicalUrl(resourceUrl, resourceUrl);
  const QUrl calendarUrl = canonicalResource.adjusted(QUrl::RemoveFilename);
  QString href = canonicalResource.path(QUrl::FullyEncoded);
  if (canonicalResource.hasQuery()) {
    href += QLatin1Char('?') + canonicalResource.query(QUrl::FullyEncoded);
  }
  // CalDAV calendar-data is a REPORT property. Some conforming servers,
  // including Radicale, return getetag but a 404 propstat for calendar-data on
  // a depth-0 PROPFIND. Reuse the bounded multiget path so every read used for
  // conflict snapshots and lost-ack reconciliation gets the actual resource.
  calendarMultiGet(accountId, calendarUrl, {href}, std::move(callback));
}

void CalDavClient::listResourceEtags(const QString& accountId, const QUrl& calendarUrl,
                                     Callback callback) {
  request(accountId, QByteArrayLiteral("PROPFIND"), calendarUrl, etagListBody(),
          QByteArrayLiteral("application/xml; charset=utf-8"), QByteArrayLiteral("1"),
          {}, false, std::move(callback));
}

void CalDavClient::createEvent(const QString& accountId, const QUrl& resourceUrl,
                               const QByteArray& icalendar, Callback callback,
                               const QByteArray& scheduleReply) {
  request(accountId, QByteArrayLiteral("PUT"), resourceUrl, icalendar,
          QByteArrayLiteral("text/calendar; charset=utf-8"), {}, {}, true,
          std::move(callback), scheduleReply);
}

void CalDavClient::updateEvent(const QString& accountId, const QUrl& resourceUrl,
                               const QString& etag, const QByteArray& icalendar,
                               Callback callback, const QByteArray& scheduleReply) {
  request(accountId, QByteArrayLiteral("PUT"), resourceUrl, icalendar,
          QByteArrayLiteral("text/calendar; charset=utf-8"), {}, etag, false,
          std::move(callback), scheduleReply);
}

void CalDavClient::deleteEvent(const QString& accountId, const QUrl& resourceUrl,
                               const QString& etag, Callback callback,
                               const QByteArray& scheduleReply) {
  request(accountId, QByteArrayLiteral("DELETE"), resourceUrl, {}, {}, {}, etag, false,
          std::move(callback), scheduleReply);
}

void CalDavClient::moveEvent(const QString& accountId, const QUrl& sourceResourceUrl,
                             const QUrl& targetResourceUrl, const QString& etag,
                             Callback callback, const QByteArray& scheduleReply) {
  request(accountId, QByteArrayLiteral("MOVE"), sourceResourceUrl, {}, {}, {}, etag,
          false, std::move(callback), scheduleReply, targetResourceUrl);
}

void CalDavClient::request(const QString& accountId, const QByteArray& verb,
                           const QUrl& url, const QByteArray& body,
                           const QByteArray& contentType, const QByteArray& depth,
                           const QString& etag, const bool createOnly,
                           Callback callback, const QByteArray& scheduleReply,
                           const QUrl& moveDestination) {
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
  if (moveDestination.isValid()) {
    QString destinationError;
    if (!validateEndpoint(moveDestination, &destinationError) ||
        !isSameOrigin(url, moveDestination)) {
      DavResponse response;
      response.errorCode = QStringLiteral("invalid_move_destination");
      response.errorMessage =
          QStringLiteral("CalDAV MOVE requires a same-origin HTTPS destination");
      QTimer::singleShot(0, this, [callback = std::move(callback), response]() mutable {
        callback(response);
      });
      return;
    }
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
  if (!isSameOrigin(credentials->origin, url)) {
    DavResponse response;
    response.errorCode = QStringLiteral("cross_origin_request");
    response.errorMessage = QStringLiteral(
        "The CalDAV server attempted to move an authenticated request to a "
        "different origin");
    QTimer::singleShot(0, this, [callback = std::move(callback), response]() mutable {
      callback(response);
    });
    return;
  }
  if (body.size() > maximumResponseBytes()) {
    DavResponse response;
    response.errorCode = QStringLiteral("request_too_large");
    response.errorMessage =
        QStringLiteral("The CalDAV request exceeded the 16 MiB safety limit");
    QTimer::singleShot(0, this, [callback = std::move(callback), response]() mutable {
      callback(response);
    });
    return;
  }
  QNetworkRequest networkRequest(url);
  networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                              QNetworkRequest::SameOriginRedirectPolicy);
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
  if (!scheduleReply.isEmpty()) {
    if (scheduleReply != QByteArrayLiteral("T") &&
        scheduleReply != QByteArrayLiteral("F")) {
      DavResponse response;
      response.errorCode = QStringLiteral("invalid_schedule_reply");
      response.errorMessage =
          QStringLiteral("Schedule-Reply must be T or F for CalDAV mutations");
      QTimer::singleShot(0, this, [callback = std::move(callback), response]() mutable {
        callback(response);
      });
      return;
    }
    networkRequest.setRawHeader(QByteArrayLiteral("Schedule-Reply"), scheduleReply);
  }
  if (moveDestination.isValid()) {
    networkRequest.setRawHeader(QByteArrayLiteral("Destination"),
                                moveDestination.toEncoded(QUrl::FullyEncoded));
    networkRequest.setRawHeader(QByteArrayLiteral("Overwrite"), QByteArrayLiteral("F"));
  }
  if (createOnly) {
    networkRequest.setRawHeader(QByteArrayLiteral("If-None-Match"),
                                QByteArrayLiteral("*"));
  } else if (!etag.isEmpty()) {
    networkRequest.setRawHeader(QByteArrayLiteral("If-Match"), etag.toUtf8());
  }
  QNetworkReply* reply = m_network.sendCustomRequest(networkRequest, verb, body);
  reply->setReadBufferSize(64 * 1024);
  const auto responseBody = std::make_shared<ReplyBody>();
  const auto consume = [reply, responseBody]() {
    if (responseBody->tooLarge) {
      return;
    }
    const qint64 remaining = static_cast<qint64>(CalDavClient::maximumResponseBytes() -
                                                 responseBody->value.size());
    if (remaining <= 0 && reply->bytesAvailable() > 0) {
      responseBody->tooLarge = true;
      responseBody->value.clear();
      reply->abort();
      return;
    }
    const QByteArray chunk = reply->read(remaining + 1);
    if (chunk.size() > remaining) {
      responseBody->tooLarge = true;
      responseBody->value.clear();
      reply->abort();
      return;
    }
    responseBody->value += chunk;
  };
  connect(reply, &QNetworkReply::metaDataChanged, this, [reply, responseBody]() {
    bool validLength = false;
    const qint64 contentLength =
        reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&validLength);
    if (validLength && contentLength > CalDavClient::maximumResponseBytes()) {
      responseBody->tooLarge = true;
      reply->abort();
    }
  });
  connect(reply, &QIODevice::readyRead, this, consume);
  connect(reply, &QNetworkReply::finished, this,
          [reply, responseBody, consume, callback = std::move(callback)]() mutable {
            consume();
            const DavResponse response =
                responseFromReply(reply, responseBody->value, responseBody->tooLarge);
            reply->deleteLater();
            callback(response);
          });
}

}  // namespace omacalendar::caldav
