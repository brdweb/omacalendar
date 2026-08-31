#include "providers/google/googleclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimeZone>
#include <QTimer>
#include <QUrlQuery>
#include <limits>

#include "providers/google/googleauth.h"

namespace omacalendar::google {
namespace {

constexpr auto kApiBase = "https://www.googleapis.com/calendar/v3";
constexpr qint64 kMaximumResponseBytes = 8 * 1024 * 1024;
constexpr int kTransferTimeoutMs = 30 * 1000;

QString pathSegment(const QString& value) {
  return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

QString sanitizedErrorMessage(QString value) {
  value.replace(QRegularExpression(QStringLiteral("[\\r\\n\\t]+")),
                QStringLiteral(" "));
  static const QRegularExpression oauthSecret(QStringLiteral(
      "(?i)(access_token|refresh_token|client_secret)(?:=|:|%3D)\\s*[^&\\s]+"));
  static const QRegularExpression authorization(
      QStringLiteral("(?i)authorization\\s*[:=]\\s*(?:bearer\\s+)?[^&\\s]+"));
  value.replace(oauthSecret, QStringLiteral("[redacted]"));
  value.replace(authorization, QStringLiteral("[redacted]"));
  return value.simplified().left(300);
}

qint64 retryAfterMilliseconds(const QByteArray& value, const QDateTime& receivedAtUtc) {
  const QString header = QString::fromLatin1(value).trimmed();
  if (header.isEmpty()) {
    return -1;
  }
  bool secondsOk = false;
  const qint64 seconds = header.toLongLong(&secondsOk);
  if (secondsOk) {
    if (seconds <= 0) {
      return 0;
    }
    return seconds > std::numeric_limits<qint64>::max() / 1000
               ? std::numeric_limits<qint64>::max()
               : seconds * 1000;
  }
  QDateTime date = QDateTime::fromString(header, Qt::RFC2822Date);
  if (!date.isValid()) {
    const QDateTime imfFixdate = QLocale::c().toDateTime(
        header, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
    if (imfFixdate.isValid()) {
      date = QDateTime(imfFixdate.date(), imfFixdate.time(), QTimeZone::UTC);
    }
  }
  if (!date.isValid()) {
    return -1;
  }
  return qMax<qint64>(0, receivedAtUtc.toUTC().msecsTo(date.toUTC()));
}

bool retryableReason(const QString& reason) {
  static const QStringList reasons = {
      QStringLiteral("rateLimitExceeded"), QStringLiteral("userRateLimitExceeded"),
      QStringLiteral("quotaExceeded"),     QStringLiteral("backendError"),
      QStringLiteral("internalError"),
  };
  return reasons.contains(reason);
}

ApiResponse oversizedResponse() {
  ApiResponse result;
  result.errorCode = QStringLiteral("response_too_large");
  result.errorMessage = QStringLiteral("Google Calendar returned too much data");
  return result;
}

ApiResponse parseResponse(QNetworkReply* reply, const bool emptyBodyAllowed) {
  if (reply->property("omacalendarResponseTooLarge").toBool()) {
    return oversizedResponse();
  }
  return parseGoogleApiResponse(
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
      static_cast<int>(reply->error()), reply->readAll(),
      reply->rawHeader(QByteArrayLiteral("ETag")),
      reply->rawHeader(QByteArrayLiteral("Retry-After")),
      QDateTime::currentDateTimeUtc(), emptyBodyAllowed);
}

QUrl eventResourceUrl(const QString& calendarRemoteId, const QString& eventRemoteId) {
  return QUrl(QString::fromLatin1(kApiBase) + QStringLiteral("/calendars/") +
              pathSegment(calendarRemoteId) + QStringLiteral("/events/") +
              pathSegment(eventRemoteId));
}

void addPagination(QUrlQuery* query, const QString& pageToken) {
  if (!pageToken.isEmpty()) {
    query->addQueryItem(QStringLiteral("pageToken"), pageToken);
  }
}

void addBounds(QUrlQuery* query, const QDateTime& timeMinUtc,
               const QDateTime& timeMaxUtc) {
  if (timeMinUtc.isValid()) {
    query->addQueryItem(QStringLiteral("timeMin"),
                        timeMinUtc.toUTC().toString(Qt::ISODateWithMs));
  }
  if (timeMaxUtc.isValid()) {
    query->addQueryItem(QStringLiteral("timeMax"),
                        timeMaxUtc.toUTC().toString(Qt::ISODateWithMs));
  }
}

}  // namespace

bool isValidGuestNotificationPolicy(const QString& value) {
  return value == QStringLiteral("none") || value == QStringLiteral("all") ||
         value == QStringLiteral("externalOnly");
}

QUrl calendarListRequestUrl(const QString& pageToken, const QString& syncToken) {
  QUrl url(QString::fromLatin1(kApiBase) + QStringLiteral("/users/me/calendarList"));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("250"));
  // These flags must remain identical between the initial request and every
  // sync-token request so removals and hidden-calendar changes are observed.
  query.addQueryItem(QStringLiteral("showDeleted"), QStringLiteral("true"));
  query.addQueryItem(QStringLiteral("showHidden"), QStringLiteral("true"));
  addPagination(&query, pageToken);
  if (!syncToken.isEmpty()) {
    query.addQueryItem(QStringLiteral("syncToken"), syncToken);
  }
  url.setQuery(query);
  return url;
}

QUrl calendarDeleteRequestUrl(const QString& calendarRemoteId) {
  return QUrl(QString::fromLatin1(kApiBase) + QStringLiteral("/calendars/") +
              pathSegment(calendarRemoteId));
}

QUrl eventsListRequestUrl(const QString& calendarRemoteId, const QString& pageToken,
                          const QString& syncToken, const QDateTime& timeMinUtc,
                          const QDateTime& timeMaxUtc) {
  QUrl url(QString::fromLatin1(kApiBase) + QStringLiteral("/calendars/") +
           pathSegment(calendarRemoteId) + QStringLiteral("/events"));
  QUrlQuery query;
  // Smaller pages keep parsing and SQLite batch application responsive during
  // a large first sync while retaining Google's page-token semantics.
  query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("250"));
  query.addQueryItem(QStringLiteral("showDeleted"), QStringLiteral("true"));
  query.addQueryItem(QStringLiteral("singleEvents"), QStringLiteral("false"));
  addPagination(&query, pageToken);
  if (!syncToken.isEmpty()) {
    query.addQueryItem(QStringLiteral("syncToken"), syncToken);
  } else {
    addBounds(&query, timeMinUtc, timeMaxUtc);
  }
  url.setQuery(query);
  return url;
}

QUrl eventInstancesRequestUrl(const QString& calendarRemoteId,
                              const QString& recurringEventRemoteId,
                              const QString& pageToken, const QDateTime& timeMinUtc,
                              const QDateTime& timeMaxUtc) {
  QUrl url(QString::fromLatin1(kApiBase) + QStringLiteral("/calendars/") +
           pathSegment(calendarRemoteId) + QStringLiteral("/events/") +
           pathSegment(recurringEventRemoteId) + QStringLiteral("/instances"));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("250"));
  query.addQueryItem(QStringLiteral("showDeleted"), QStringLiteral("true"));
  addPagination(&query, pageToken);
  addBounds(&query, timeMinUtc, timeMaxUtc);
  url.setQuery(query);
  return url;
}

QUrl eventMoveRequestUrl(const QString& sourceCalendarRemoteId,
                         const QString& eventRemoteId,
                         const QString& targetCalendarRemoteId,
                         const QString& sendUpdates) {
  QUrl url(QString::fromLatin1(kApiBase) + QStringLiteral("/calendars/") +
           pathSegment(sourceCalendarRemoteId) + QStringLiteral("/events/") +
           pathSegment(eventRemoteId) + QStringLiteral("/move"));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("destination"), targetCalendarRemoteId);
  query.addQueryItem(QStringLiteral("sendUpdates"), sendUpdates);
  url.setQuery(query);
  return url;
}

ApiResponse parseGoogleApiResponse(const int httpStatus, const int networkError,
                                   const QByteArray& body, const QByteArray& etag,
                                   const QByteArray& retryAfter,
                                   const QDateTime& receivedAtUtc,
                                   const bool emptyBodyAllowed) {
  ApiResponse result;
  result.httpStatus = httpStatus;
  result.networkError = networkError;
  result.etag = QString::fromUtf8(etag);
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
  if (document.isObject()) {
    result.body = document.object();
  }
  result.ok = networkError == static_cast<int>(QNetworkReply::NoError) &&
              httpStatus >= 200 && httpStatus < 300;
  if (result.ok) {
    if (!emptyBodyAllowed && (body.isEmpty() || !document.isObject())) {
      result.ok = false;
      result.errorCode = QStringLiteral("malformed_response");
      result.errorMessage =
          QStringLiteral("Google Calendar returned an invalid response");
      result.retryable = true;
    }
    return result;
  }

  const QJsonObject error = result.body.value(QStringLiteral("error")).toObject();
  result.errorMessage =
      sanitizedErrorMessage(error.value(QStringLiteral("message")).toString());
  const QJsonArray details = error.value(QStringLiteral("errors")).toArray();
  if (!details.isEmpty()) {
    result.errorCode =
        details.first().toObject().value(QStringLiteral("reason")).toString();
  }
  if (result.errorCode.isEmpty()) {
    result.errorCode = error.value(QStringLiteral("status")).toString();
  }
  if (result.errorCode.isEmpty()) {
    result.errorCode = QStringLiteral("http_%1").arg(result.httpStatus);
  }
  if (result.errorMessage.isEmpty()) {
    result.errorMessage =
        result.httpStatus > 0
            ? QStringLiteral("Google Calendar request failed (%1)")
                  .arg(result.httpStatus)
            : QStringLiteral("Google Calendar network request failed");
  }
  result.authenticationRequired =
      result.httpStatus == 401 || result.errorCode == QStringLiteral("authError");
  result.insufficientScope =
      result.httpStatus == 403 &&
      (result.errorCode == QStringLiteral("insufficientPermissions") ||
       result.errorMessage.contains(QStringLiteral("insufficient authentication scope"),
                                    Qt::CaseInsensitive));
  result.notFound = result.httpStatus == 404 || result.httpStatus == 410;
  result.conflict = result.httpStatus == 409 || result.httpStatus == 412;
  result.syncTokenExpired = result.httpStatus == 410;
  result.retryable = result.httpStatus == 408 || result.httpStatus == 425 ||
                     result.httpStatus == 429 || result.httpStatus >= 500 ||
                     retryableReason(result.errorCode) ||
                     (result.httpStatus == 0 &&
                      result.networkError != static_cast<int>(QNetworkReply::NoError));
  result.retryAfterMs = retryAfterMilliseconds(retryAfter, receivedAtUtc);
  return result;
}

GoogleClient::GoogleClient(GoogleAuthManager* auth, QObject* parent)
    : QObject(parent), m_auth(auth) {}

void GoogleClient::listCalendars(const QString& accountId, const QString& pageToken,
                                 const QString& syncToken, Callback callback) {
  request(accountId, QByteArrayLiteral("GET"),
          calendarListRequestUrl(pageToken, syncToken), {}, {}, std::move(callback));
}

void GoogleClient::listEvents(const QString& accountId, const QString& calendarRemoteId,
                              const QString& pageToken, const QString& syncToken,
                              const QDateTime& timeMinUtc, const QDateTime& timeMaxUtc,
                              Callback callback) {
  request(accountId, QByteArrayLiteral("GET"),
          eventsListRequestUrl(calendarRemoteId, pageToken, syncToken, timeMinUtc,
                               timeMaxUtc),
          {}, {}, std::move(callback));
}

void GoogleClient::listEventInstances(const QString& accountId,
                                      const QString& calendarRemoteId,
                                      const QString& recurringEventRemoteId,
                                      const QString& pageToken,
                                      const QDateTime& timeMinUtc,
                                      const QDateTime& timeMaxUtc, Callback callback) {
  request(accountId, QByteArrayLiteral("GET"),
          eventInstancesRequestUrl(calendarRemoteId, recurringEventRemoteId, pageToken,
                                   timeMinUtc, timeMaxUtc),
          {}, {}, std::move(callback));
}

void GoogleClient::getEvent(const QString& accountId, const QString& calendarRemoteId,
                            const QString& eventRemoteId, Callback callback) {
  const QUrl url = eventResourceUrl(calendarRemoteId, eventRemoteId);
  request(accountId, QByteArrayLiteral("GET"), url, {}, {}, std::move(callback));
}

void GoogleClient::deleteCalendar(const QString& accountId,
                                  const QString& calendarRemoteId, Callback callback) {
  request(accountId, QByteArrayLiteral("DELETE"),
          calendarDeleteRequestUrl(calendarRemoteId), {}, {}, std::move(callback),
          true);
}

void GoogleClient::createEvent(const QString& accountId,
                               const QString& calendarRemoteId,
                               const QJsonObject& event, const QString& sendUpdates,
                               Callback callback) {
  if (!isValidGuestNotificationPolicy(sendUpdates)) {
    invalidGuestNotificationPolicy(std::move(callback));
    return;
  }
  QUrl url(QString::fromLatin1(kApiBase) + QStringLiteral("/calendars/") +
           pathSegment(calendarRemoteId) + QStringLiteral("/events"));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("conferenceDataVersion"), QStringLiteral("1"));
  query.addQueryItem(QStringLiteral("supportsAttachments"), QStringLiteral("true"));
  query.addQueryItem(QStringLiteral("sendUpdates"), sendUpdates);
  url.setQuery(query);
  request(accountId, QByteArrayLiteral("POST"), url, event, {}, std::move(callback));
}

void GoogleClient::updateEvent(const QString& accountId,
                               const QString& calendarRemoteId,
                               const QString& eventRemoteId, const QString& etag,
                               const QJsonObject& event, const QString& sendUpdates,
                               Callback callback) {
  if (!isValidGuestNotificationPolicy(sendUpdates)) {
    invalidGuestNotificationPolicy(std::move(callback));
    return;
  }
  QUrl url = eventResourceUrl(calendarRemoteId, eventRemoteId);
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("conferenceDataVersion"), QStringLiteral("1"));
  query.addQueryItem(QStringLiteral("supportsAttachments"), QStringLiteral("true"));
  query.addQueryItem(QStringLiteral("sendUpdates"), sendUpdates);
  url.setQuery(query);
  request(accountId, QByteArrayLiteral("PATCH"), url, event, etag, std::move(callback));
}

void GoogleClient::deleteEvent(const QString& accountId,
                               const QString& calendarRemoteId,
                               const QString& eventRemoteId, const QString& etag,
                               const QString& sendUpdates, Callback callback) {
  if (!isValidGuestNotificationPolicy(sendUpdates)) {
    invalidGuestNotificationPolicy(std::move(callback));
    return;
  }
  QUrl url = eventResourceUrl(calendarRemoteId, eventRemoteId);
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("sendUpdates"), sendUpdates);
  url.setQuery(query);
  request(accountId, QByteArrayLiteral("DELETE"), url, {}, etag, std::move(callback),
          true);
}

void GoogleClient::moveEvent(const QString& accountId,
                             const QString& sourceCalendarRemoteId,
                             const QString& eventRemoteId,
                             const QString& targetCalendarRemoteId, const QString& etag,
                             const QString& sendUpdates, Callback callback) {
  if (!isValidGuestNotificationPolicy(sendUpdates)) {
    invalidGuestNotificationPolicy(std::move(callback));
    return;
  }
  request(accountId, QByteArrayLiteral("POST"),
          eventMoveRequestUrl(sourceCalendarRemoteId, eventRemoteId,
                              targetCalendarRemoteId, sendUpdates),
          {}, etag, std::move(callback));
}

void GoogleClient::respondToEvent(const QString& accountId,
                                  const QString& calendarRemoteId,
                                  const QString& eventRemoteId, const QString& etag,
                                  const QJsonObject& attendeePatch,
                                  const QString& sendUpdates, Callback callback) {
  if (!isValidGuestNotificationPolicy(sendUpdates)) {
    invalidGuestNotificationPolicy(std::move(callback));
    return;
  }
  QUrl url = eventResourceUrl(calendarRemoteId, eventRemoteId);
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("sendUpdates"), sendUpdates);
  url.setQuery(query);
  request(accountId, QByteArrayLiteral("PATCH"), url, attendeePatch, etag,
          std::move(callback));
}

void GoogleClient::request(const QString& accountId, const QByteArray& verb, QUrl url,
                           const QJsonObject& body, const QString& etag,
                           Callback callback, const bool emptyBodyAllowed) {
  if (m_auth == nullptr || !m_auth->hasAccessToken(accountId)) {
    ApiResponse response;
    response.errorCode = QStringLiteral("authentication_required");
    response.errorMessage = QStringLiteral("Google account is not authorized");
    response.authenticationRequired = true;
    QTimer::singleShot(0, this, [callback = std::move(callback), response]() mutable {
      callback(response);
    });
    return;
  }
  QNetworkRequest networkRequest(url);
  networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                              QNetworkRequest::NoLessSafeRedirectPolicy);
  networkRequest.setMaximumRedirectsAllowed(3);
  networkRequest.setTransferTimeout(kTransferTimeoutMs);
  networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                           QStringLiteral("application/json; charset=utf-8"));
  networkRequest.setRawHeader(
      QByteArrayLiteral("Authorization"),
      QByteArrayLiteral("Bearer ") + m_auth->accessToken(accountId).toUtf8());
  networkRequest.setRawHeader(
      QByteArrayLiteral("User-Agent"),
      QByteArrayLiteral("OmaCalendar/") + QByteArrayLiteral(OMACALENDAR_VERSION));
  if (!etag.isEmpty()) {
    networkRequest.setRawHeader(QByteArrayLiteral("If-Match"), etag.toUtf8());
  }
  const QByteArray payload = body.isEmpty()
                                 ? QByteArray()
                                 : QJsonDocument(body).toJson(QJsonDocument::Compact);
  QNetworkReply* reply = nullptr;
  if (verb == QByteArrayLiteral("GET")) {
    reply = m_network.get(networkRequest);
  } else if (verb == QByteArrayLiteral("POST")) {
    reply = m_network.post(networkRequest, payload);
  } else {
    reply = m_network.sendCustomRequest(networkRequest, verb, payload);
  }
  reply->setReadBufferSize(kMaximumResponseBytes + 1);
  reply->setProperty("omacalendarResponseTooLarge", false);
  connect(reply, &QIODevice::readyRead, this, [reply]() {
    if (reply->bytesAvailable() > kMaximumResponseBytes) {
      reply->setProperty("omacalendarResponseTooLarge", true);
      reply->abort();
    }
  });
  connect(reply, &QNetworkReply::finished, this,
          [reply, callback = std::move(callback), emptyBodyAllowed]() mutable {
            const ApiResponse result = parseResponse(reply, emptyBodyAllowed);
            reply->deleteLater();
            callback(result);
          });
}

void GoogleClient::invalidGuestNotificationPolicy(Callback callback) {
  ApiResponse response;
  response.errorCode = QStringLiteral("invalid_send_updates");
  response.errorMessage = QStringLiteral("A guest notification policy is required");
  QTimer::singleShot(0, this, [callback = std::move(callback), response]() mutable {
    callback(response);
  });
}

}  // namespace omacalendar::google
