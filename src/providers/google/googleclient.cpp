#include "providers/google/googleclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>

#include "providers/google/googleauth.h"

namespace omacalendar::google {
namespace {

constexpr auto kApiBase = "https://www.googleapis.com/calendar/v3";

QString pathSegment(const QString& value) {
  return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

ApiResponse parseResponse(QNetworkReply* reply) {
  ApiResponse result;
  result.httpStatus =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  result.etag = QString::fromUtf8(reply->rawHeader(QByteArrayLiteral("ETag")));
  const QByteArray bytes = reply->readAll();
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
  if (document.isObject()) {
    result.body = document.object();
  }
  result.ok = reply->error() == QNetworkReply::NoError && result.httpStatus >= 200 &&
              result.httpStatus < 300;
  if (result.ok) {
    return result;
  }

  const QJsonObject error = result.body.value(QStringLiteral("error")).toObject();
  result.errorMessage = error.value(QStringLiteral("message")).toString();
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
  result.errorMessage = result.errorMessage.left(300);
  result.authenticationRequired = result.httpStatus == 401;
  result.retryable =
      result.httpStatus == 408 || result.httpStatus == 429 ||
      result.httpStatus >= 500 ||
      result.errorCode == QStringLiteral("rateLimitExceeded") ||
      result.errorCode == QStringLiteral("userRateLimitExceeded") ||
      result.errorCode == QStringLiteral("backendError") ||
      (result.httpStatus == 0 && reply->error() != QNetworkReply::NoError);
  bool retryOk = false;
  result.retryAfterSeconds =
      QString::fromLatin1(reply->rawHeader(QByteArrayLiteral("Retry-After")))
          .toInt(&retryOk);
  if (!retryOk) {
    result.retryAfterSeconds = 0;
  }
  return result;
}

}  // namespace

GoogleClient::GoogleClient(GoogleAuthManager* auth, QObject* parent)
    : QObject(parent), m_auth(auth) {}

void GoogleClient::listCalendars(const QString& accountId, const QString& pageToken,
                                 const QString& syncToken, Callback callback) {
  QUrl url(QString::fromLatin1(kApiBase) + QStringLiteral("/users/me/calendarList"));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("250"));
  if (!pageToken.isEmpty()) {
    query.addQueryItem(QStringLiteral("pageToken"), pageToken);
  }
  if (!syncToken.isEmpty()) {
    query.addQueryItem(QStringLiteral("syncToken"), syncToken);
  } else {
    query.addQueryItem(QStringLiteral("showDeleted"), QStringLiteral("true"));
    query.addQueryItem(QStringLiteral("showHidden"), QStringLiteral("true"));
  }
  url.setQuery(query);
  request(accountId, QByteArrayLiteral("GET"), url, {}, {}, std::move(callback));
}

void GoogleClient::listEvents(const QString& accountId, const QString& calendarRemoteId,
                              const QString& pageToken, const QString& syncToken,
                              Callback callback) {
  QUrl url(QString::fromLatin1(kApiBase) + QStringLiteral("/calendars/") +
           pathSegment(calendarRemoteId) + QStringLiteral("/events"));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("2500"));
  query.addQueryItem(QStringLiteral("showDeleted"), QStringLiteral("true"));
  query.addQueryItem(QStringLiteral("singleEvents"), QStringLiteral("false"));
  if (!pageToken.isEmpty()) {
    query.addQueryItem(QStringLiteral("pageToken"), pageToken);
  }
  if (!syncToken.isEmpty()) {
    query.addQueryItem(QStringLiteral("syncToken"), syncToken);
  }
  url.setQuery(query);
  request(accountId, QByteArrayLiteral("GET"), url, {}, {}, std::move(callback));
}

void GoogleClient::createEvent(const QString& accountId,
                               const QString& calendarRemoteId,
                               const QJsonObject& event, Callback callback) {
  QUrl url(QString::fromLatin1(kApiBase) + QStringLiteral("/calendars/") +
           pathSegment(calendarRemoteId) + QStringLiteral("/events"));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("conferenceDataVersion"), QStringLiteral("1"));
  query.addQueryItem(QStringLiteral("sendUpdates"), QStringLiteral("all"));
  url.setQuery(query);
  request(accountId, QByteArrayLiteral("POST"), url, event, {}, std::move(callback));
}

void GoogleClient::updateEvent(const QString& accountId,
                               const QString& calendarRemoteId,
                               const QString& eventRemoteId, const QString& etag,
                               const QJsonObject& event, Callback callback) {
  QUrl url(QString::fromLatin1(kApiBase) + QStringLiteral("/calendars/") +
           pathSegment(calendarRemoteId) + QStringLiteral("/events/") +
           pathSegment(eventRemoteId));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("conferenceDataVersion"), QStringLiteral("1"));
  query.addQueryItem(QStringLiteral("sendUpdates"), QStringLiteral("all"));
  url.setQuery(query);
  request(accountId, QByteArrayLiteral("PATCH"), url, event, etag, std::move(callback));
}

void GoogleClient::deleteEvent(const QString& accountId,
                               const QString& calendarRemoteId,
                               const QString& eventRemoteId, const QString& etag,
                               Callback callback) {
  QUrl url(QString::fromLatin1(kApiBase) + QStringLiteral("/calendars/") +
           pathSegment(calendarRemoteId) + QStringLiteral("/events/") +
           pathSegment(eventRemoteId));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("sendUpdates"), QStringLiteral("all"));
  url.setQuery(query);
  request(accountId, QByteArrayLiteral("DELETE"), url, {}, etag, std::move(callback));
}

void GoogleClient::request(const QString& accountId, const QByteArray& verb, QUrl url,
                           const QJsonObject& body, const QString& etag,
                           Callback callback) {
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
  connect(reply, &QNetworkReply::finished, this,
          [reply, callback = std::move(callback)]() mutable {
            const ApiResponse result = parseResponse(reply);
            reply->deleteLater();
            callback(result);
          });
}

}  // namespace omacalendar::google
