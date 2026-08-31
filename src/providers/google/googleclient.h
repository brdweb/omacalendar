#pragma once

#include <QByteArray>
#include <QDateTime>
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
  int networkError = 0;
  bool authenticationRequired = false;
  bool insufficientScope = false;
  bool retryable = false;
  bool notFound = false;
  bool conflict = false;
  bool syncTokenExpired = false;
  qint64 retryAfterMs = -1;
};

// Pure request/response helpers kept public for deterministic contract tests.
[[nodiscard]] bool isValidGuestNotificationPolicy(const QString& value);
[[nodiscard]] QUrl calendarListRequestUrl(const QString& pageToken,
                                          const QString& syncToken);
[[nodiscard]] QUrl calendarDeleteRequestUrl(const QString& calendarRemoteId);
[[nodiscard]] QUrl eventsListRequestUrl(const QString& calendarRemoteId,
                                        const QString& pageToken,
                                        const QString& syncToken,
                                        const QDateTime& timeMinUtc = {},
                                        const QDateTime& timeMaxUtc = {});
[[nodiscard]] QUrl eventInstancesRequestUrl(const QString& calendarRemoteId,
                                            const QString& recurringEventRemoteId,
                                            const QString& pageToken,
                                            const QDateTime& timeMinUtc,
                                            const QDateTime& timeMaxUtc);
[[nodiscard]] QUrl eventMoveRequestUrl(const QString& sourceCalendarRemoteId,
                                       const QString& eventRemoteId,
                                       const QString& targetCalendarRemoteId,
                                       const QString& sendUpdates);
[[nodiscard]] ApiResponse parseGoogleApiResponse(
    int httpStatus, int networkError, const QByteArray& body,
    const QByteArray& etag = {}, const QByteArray& retryAfter = {},
    const QDateTime& receivedAtUtc = QDateTime::currentDateTimeUtc(),
    bool emptyBodyAllowed = false);

class GoogleClient final : public QObject {
  Q_OBJECT

 public:
  using Callback = std::function<void(ApiResponse)>;

  explicit GoogleClient(GoogleAuthManager* auth, QObject* parent = nullptr);

  void listCalendars(const QString& accountId, const QString& pageToken,
                     const QString& syncToken, Callback callback);
  void deleteCalendar(const QString& accountId, const QString& calendarRemoteId,
                      Callback callback);
  void listEvents(const QString& accountId, const QString& calendarRemoteId,
                  const QString& pageToken, const QString& syncToken,
                  const QDateTime& timeMinUtc, const QDateTime& timeMaxUtc,
                  Callback callback);
  void listEventInstances(const QString& accountId, const QString& calendarRemoteId,
                          const QString& recurringEventRemoteId,
                          const QString& pageToken, const QDateTime& timeMinUtc,
                          const QDateTime& timeMaxUtc, Callback callback);
  void getEvent(const QString& accountId, const QString& calendarRemoteId,
                const QString& eventRemoteId, Callback callback);
  void createEvent(const QString& accountId, const QString& calendarRemoteId,
                   const QJsonObject& event, const QString& sendUpdates,
                   Callback callback);
  void updateEvent(const QString& accountId, const QString& calendarRemoteId,
                   const QString& eventRemoteId, const QString& etag,
                   const QJsonObject& event, const QString& sendUpdates,
                   Callback callback);
  void deleteEvent(const QString& accountId, const QString& calendarRemoteId,
                   const QString& eventRemoteId, const QString& etag,
                   const QString& sendUpdates, Callback callback);
  void moveEvent(const QString& accountId, const QString& sourceCalendarRemoteId,
                 const QString& eventRemoteId, const QString& targetCalendarRemoteId,
                 const QString& etag, const QString& sendUpdates, Callback callback);
  void respondToEvent(const QString& accountId, const QString& calendarRemoteId,
                      const QString& eventRemoteId, const QString& etag,
                      const QJsonObject& attendeePatch, const QString& sendUpdates,
                      Callback callback);

 private:
  void request(const QString& accountId, const QByteArray& verb, QUrl url,
               const QJsonObject& body, const QString& etag, Callback callback,
               bool emptyBodyAllowed = false);
  void invalidGuestNotificationPolicy(Callback callback);

  GoogleAuthManager* m_auth = nullptr;
  QNetworkAccessManager m_network;
};

}  // namespace omacalendar::google
