#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QTimer>
#include <QtTest/QtTest>
#include <algorithm>
#include <functional>

#include "providers/caldav/caldavclient.h"
#include "providers/caldav/caldavsync.h"
#include "providers/caldav/caldavxml.h"
#include "providers/caldav/icalcodec.h"

using namespace omacalendar;

namespace {

QByteArray retainedResource();

class HttpFixture final : public QObject {
 public:
  explicit HttpFixture(QByteArray response, QObject* parent = nullptr)
      : QObject(parent), m_response(std::move(response)) {
    connect(&m_server, &QTcpServer::newConnection, this, [this]() {
      while (QTcpSocket* socket = m_server.nextPendingConnection()) {
        ++m_connections;
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
          m_request += socket->readAll();
          if (!m_request.contains("\r\n\r\n")) {
            return;
          }
          socket->write(m_response);
          socket->disconnectFromHost();
        });
      }
    });
  }

  bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }

  [[nodiscard]] QUrl url(const QString& path = QStringLiteral("/dav/")) const {
    QUrl result;
    result.setScheme(QStringLiteral("http"));
    result.setHost(QStringLiteral("127.0.0.1"));
    result.setPort(m_server.serverPort());
    result.setPath(path);
    return result;
  }

  [[nodiscard]] int connections() const { return m_connections; }
  [[nodiscard]] QByteArray request() const { return m_request; }

 private:
  QTcpServer m_server;
  QByteArray m_response;
  QByteArray m_request;
  int m_connections = 0;
};

class CalDavRangeFixture final : public QObject {
 public:
  explicit CalDavRangeFixture(bool retainProbeRange = true,
                              bool probeCleanupSucceeds = true,
                              bool seedStaleProbe = false,
                              bool loseProbeCreateAck = false,
                              QObject* parent = nullptr)
      : QObject(parent),
        m_retainProbeRange(retainProbeRange),
        m_probeCleanupSucceeds(probeCleanupSucceeds),
        m_seedStaleProbe(seedStaleProbe),
        m_loseProbeCreateAck(loseProbeCreateAck) {
    m_resource = retainedResource();
    connect(&m_server, &QTcpServer::newConnection, this, [this]() {
      while (QTcpSocket* socket = m_server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
          QByteArray& request = m_requests[socket];
          request += socket->readAll();
          const qsizetype headerEnd = request.indexOf("\r\n\r\n");
          if (headerEnd < 0) {
            return;
          }
          const QByteArray headers = request.left(headerEnd).toLower();
          qsizetype contentLength = 0;
          const qsizetype lengthStart = headers.indexOf("content-length:");
          if (lengthStart >= 0) {
            const qsizetype valueStart =
                lengthStart + QByteArray("content-length:").size();
            const qsizetype valueEnd = headers.indexOf("\r\n", valueStart);
            contentLength =
                headers.mid(valueStart, valueEnd - valueStart).trimmed().toLongLong();
          }
          if (request.size() < headerEnd + 4 + contentLength) {
            return;
          }
          socket->write(responseFor(request));
          m_requests.remove(socket);
          socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::destroyed, this,
                [this, socket]() { m_requests.remove(socket); });
      }
    });
  }

  ~CalDavRangeFixture() override {
    // QTcpServer owns accepted sockets. Disconnect callbacks while the request
    // map is still alive so child destruction cannot re-enter a torn-down map.
    const auto sockets = m_server.findChildren<QTcpSocket*>();
    for (QTcpSocket* socket : sockets) {
      QObject::disconnect(socket, nullptr, this, nullptr);
      QObject::disconnect(socket, nullptr, socket, nullptr);
    }
  }

  bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }

  [[nodiscard]] QUrl endpoint() const {
    QUrl result;
    result.setScheme(QStringLiteral("http"));
    result.setHost(QStringLiteral("127.0.0.1"));
    result.setPort(m_server.serverPort());
    result.setPath(QStringLiteral("/dav/"));
    return result;
  }

  [[nodiscard]] int putCount() const { return m_putCount; }
  [[nodiscard]] int probePutCount() const { return m_probePutCount; }
  [[nodiscard]] int probeDeleteCount() const { return m_probeDeleteCount; }
  [[nodiscard]] bool staleProbeWasSeeded() const { return m_staleProbeWasSeeded; }
  [[nodiscard]] bool probeResourceExists() const { return m_probeExists; }
  [[nodiscard]] QByteArray lastPut() const { return m_lastPut; }

 private:
  static QByteArray httpResponse(const QByteArray& status, const QByteArray& body,
                                 const QByteArray& contentType,
                                 const QByteArray& extraHeaders = {}) {
    return QByteArray("HTTP/1.1 ") + status + "\r\nContent-Type: " + contentType +
           "\r\nContent-Length: " + QByteArray::number(body.size()) +
           "\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body;
  }

  QByteArray multiStatusResource() const {
    return QByteArrayLiteral(
               "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
               "<d:multistatus xmlns:d=\"DAV:\" "
               "xmlns:c=\"urn:ietf:params:xml:ns:caldav\">\n"
               "<d:response><d:href>/calendar/series.ics</d:href><d:propstat>"
               "<d:prop><d:getetag>&quot;") +
           m_etag + QByteArrayLiteral("&quot;</d:getetag><c:calendar-data><![CDATA[") +
           m_resource +
           QByteArrayLiteral(
               "]]></c:calendar-data></d:prop>"
               "<d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
               "</d:response><d:sync-token>token-1</d:sync-token>");
  }

  QByteArray probeMultiStatusResource() const {
    if (!m_probeExists) {
      return {};
    }
    return QByteArrayLiteral("<d:response><d:href>") + m_probePath +
           QByteArrayLiteral("</d:href><d:propstat><d:prop><d:getetag>&quot;") +
           m_probeEtag +
           QByteArrayLiteral("&quot;</d:getetag><c:calendar-data><![CDATA[") +
           m_probeResource +
           QByteArrayLiteral(
               "]]></c:calendar-data></d:prop><d:status>HTTP/1.1 200 OK</d:status>"
               "</d:propstat></d:response>");
  }

  QByteArray responseFor(const QByteArray& request) {
    const qsizetype firstLineEnd = request.indexOf("\r\n");
    const QByteArray firstLine = request.left(firstLineEnd);
    const qsizetype headerEnd = request.indexOf("\r\n\r\n");
    const QByteArray body = request.mid(headerEnd + 4);
    if (firstLine.startsWith("PROPFIND /dav/ ")) {
      return httpResponse(
          QByteArrayLiteral("207 Multi-Status"),
          QByteArrayLiteral(
              "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\">"
              "<d:response><d:href>/dav/</d:href><d:propstat><d:prop>"
              "<d:current-user-principal><d:href>/principal/</d:href>"
              "</d:current-user-principal></d:prop>"
              "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
              "</d:multistatus>"),
          QByteArrayLiteral("application/xml"));
    }
    if (firstLine.startsWith("PROPFIND /principal/ ")) {
      return httpResponse(
          QByteArrayLiteral("207 Multi-Status"),
          QByteArrayLiteral(
              "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" "
              "xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
              "<d:response><d:href>/principal/</d:href><d:propstat><d:prop>"
              "<c:calendar-home-set><d:href>/home/</d:href>"
              "</c:calendar-home-set>"
              "<c:calendar-user-address-set><d:href>mailto:fixture-user</d:href>"
              "</c:calendar-user-address-set>"
              "<c:schedule-inbox-URL><d:href>/inbox/</d:href>"
              "</c:schedule-inbox-URL>"
              "<c:schedule-outbox-URL><d:href>/outbox/</d:href>"
              "</c:schedule-outbox-URL></d:prop>"
              "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
              "</d:multistatus>"),
          QByteArrayLiteral("application/xml"));
    }
    if (firstLine.startsWith("PROPFIND /home/ ")) {
      return httpResponse(
          QByteArrayLiteral("207 Multi-Status"),
          QByteArrayLiteral(
              "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" "
              "xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
              "<d:response><d:href>/calendar/</d:href><d:propstat><d:prop>"
              "<d:resourcetype><d:collection/><c:calendar/></d:resourcetype>"
              "<d:displayname>Range fixture</d:displayname>"
              "<d:sync-token>token-1</d:sync-token>"
              "<d:current-user-privilege-set><d:privilege><d:read/></d:privilege>"
              "<d:privilege><d:write-content/></d:privilege>"
              "</d:current-user-privilege-set></d:prop>"
              "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
              "</d:multistatus>"),
          QByteArrayLiteral("application/xml"));
    }
    if (firstLine.startsWith("REPORT /calendar/ ")) {
      return httpResponse(QByteArrayLiteral("207 Multi-Status"),
                          multiStatusResource() + probeMultiStatusResource() +
                              QByteArrayLiteral("</d:multistatus>"),
                          QByteArrayLiteral("application/xml"));
    }
    if (firstLine.startsWith("PUT /calendar/series.ics ")) {
      ++m_putCount;
      m_lastPut = body;
      m_resource = body;
      m_etag = QByteArrayLiteral("v2");
      return httpResponse(QByteArrayLiteral("204 No Content"), {},
                          QByteArrayLiteral("text/calendar"),
                          QByteArrayLiteral("ETag: \"v2\"\r\n"));
    }
    if (firstLine.startsWith("PUT /calendar/.omacalendar-capability-")) {
      ++m_probePutCount;
      const qsizetype pathStart = firstLine.indexOf(' ') + 1;
      const qsizetype pathEnd = firstLine.indexOf(' ', pathStart);
      m_probePath = firstLine.mid(pathStart, pathEnd - pathStart);
      m_probeResource = body;
      m_probeExists = true;
      m_probeEtag = m_probePutCount == 1 ? QByteArrayLiteral("probe-v1")
                                         : QByteArrayLiteral("probe-v2");
      if (m_probePutCount > 1 && !m_retainProbeRange) {
        m_probeResource.replace(";RANGE=THISANDFUTURE", "");
      }
      if (m_loseProbeCreateAck && m_probePutCount == 1) {
        return httpResponse(QByteArrayLiteral("500 Internal Server Error"), {},
                            QByteArrayLiteral("text/plain"));
      }
      return httpResponse(
          QByteArrayLiteral("204 No Content"), {}, QByteArrayLiteral("text/calendar"),
          QByteArrayLiteral("ETag: \"") + m_probeEtag + QByteArrayLiteral("\"\r\n"));
    }
    if (firstLine.startsWith("DELETE /calendar/.omacalendar-capability-")) {
      ++m_probeDeleteCount;
      if (m_seedStaleProbe) {
        m_seedStaleProbe = false;
        m_staleProbeWasSeeded = true;
        const qsizetype pathStart = firstLine.indexOf(' ') + 1;
        const qsizetype pathEnd = firstLine.indexOf(' ', pathStart);
        m_probePath = firstLine.mid(pathStart, pathEnd - pathStart);
        m_probeResource = retainedResource();
        m_probeExists = true;
      }
      if (!m_probeCleanupSucceeds) {
        return httpResponse(QByteArrayLiteral("500 Internal Server Error"), {},
                            QByteArrayLiteral("text/plain"));
      }
      if (!m_probeExists) {
        return httpResponse(QByteArrayLiteral("404 Not Found"), {},
                            QByteArrayLiteral("text/plain"));
      }
      m_probeExists = false;
      return httpResponse(QByteArrayLiteral("204 No Content"), {},
                          QByteArrayLiteral("text/calendar"));
    }
    return httpResponse(QByteArrayLiteral("404 Not Found"),
                        QByteArrayLiteral("not found"),
                        QByteArrayLiteral("text/plain"));
  }

  QHash<QTcpSocket*, QByteArray> m_requests;
  QTcpServer m_server;
  QByteArray m_resource;
  QByteArray m_etag = QByteArrayLiteral("v1");
  QByteArray m_lastPut;
  int m_putCount = 0;
  QByteArray m_probePath;
  QByteArray m_probeResource;
  QByteArray m_probeEtag = QByteArrayLiteral("probe-v0");
  bool m_probeExists = false;
  bool m_retainProbeRange = true;
  bool m_probeCleanupSucceeds = true;
  bool m_seedStaleProbe = false;
  bool m_loseProbeCreateAck = false;
  bool m_staleProbeWasSeeded = false;
  int m_probePutCount = 0;
  int m_probeDeleteCount = 0;
};

caldav::DavResponse awaitResponse(
    const std::function<void(caldav::CalDavClient::Callback)>& start) {
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  caldav::DavResponse result;
  bool completed = false;
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  start([&](const caldav::DavResponse& response) {
    result = response;
    completed = true;
    loop.quit();
  });
  timeout.start(5000);
  loop.exec();
  if (!completed) {
    result.errorCode = QStringLiteral("test_timeout");
  }
  return result;
}

QByteArray retainedResource() {
  return QByteArrayLiteral(
      "BEGIN:VCALENDAR\r\n"
      "VERSION:2.0\r\n"
      "PRODID:-//Example Server//EN\r\n"
      "X-WR-CALNAME:Preserved calendar\r\n"
      "BEGIN:VTIMEZONE\r\n"
      "TZID:America/New_York\r\n"
      "X-LIC-LOCATION:America/New_York\r\n"
      "BEGIN:STANDARD\r\n"
      "DTSTART:19701101T020000\r\n"
      "TZOFFSETFROM:-0400\r\n"
      "TZOFFSETTO:-0500\r\n"
      "TZNAME:EST\r\n"
      "END:STANDARD\r\n"
      "END:VTIMEZONE\r\n"
      "BEGIN:VEVENT\r\n"
      "UID:series@example.test\r\n"
      "DTSTAMP:20260801T120000Z\r\n"
      "DTSTART;TZID=America/New_York:20260828T090000\r\n"
      "DTEND;TZID=America/New_York:20260828T100000\r\n"
      "RRULE:FREQ=WEEKLY;COUNT=4\r\n"
      "RDATE;TZID=America/New_York:20260930T090000\r\n"
      "EXDATE;TZID=America/New_York:20260911T090000\r\n"
      "SUMMARY:Original master\r\n"
      "ORGANIZER;CN=Owner:mailto:owner@example.test\r\n"
      "ATTENDEE;CN=Guest;PARTSTAT=ACCEPTED:mailto:guest@example.test\r\n"
      "URL:https://meet.example.test/room\r\n"
      "X-EXAMPLE-PROVIDER-ID:opaque-value\r\n"
      "BEGIN:VALARM\r\n"
      "TRIGGER:-PT15M\r\n"
      "ACTION:DISPLAY\r\n"
      "DESCRIPTION:Reminder\r\n"
      "X-EXAMPLE-ALARM-ID:opaque-alarm\r\n"
      "END:VALARM\r\n"
      "BEGIN:VALARM\r\n"
      "TRIGGER;RELATED=END:-PT2M\r\n"
      "ACTION:PROCEDURE\r\n"
      "ATTACH:file:///provider/alarm-handler\r\n"
      "X-UNEDITABLE-ALARM:keep-whole-component\r\n"
      "END:VALARM\r\n"
      "END:VEVENT\r\n"
      "BEGIN:VEVENT\r\n"
      "UID:series@example.test\r\n"
      "RECURRENCE-ID;TZID=America/New_York:20260904T090000\r\n"
      "DTSTAMP:20260802T120000Z\r\n"
      "DTSTART;TZID=America/New_York:20260904T110000\r\n"
      "DTEND;TZID=America/New_York:20260904T120000\r\n"
      "SUMMARY:Preserved exception\r\n"
      "X-EXCEPTION-DATA:keep-me\r\n"
      "END:VEVENT\r\n"
      "BEGIN:VTODO\r\n"
      "UID:unrelated-task@example.test\r\n"
      "SUMMARY:Unrelated component\r\n"
      "END:VTODO\r\n"
      "END:VCALENDAR\r\n");
}

bool installFastSecretTool(QTemporaryDir* helperDirectory, QByteArray* originalPath) {
  if (helperDirectory == nullptr || originalPath == nullptr ||
      !helperDirectory->isValid()) {
    return false;
  }
  const QString helperPath = helperDirectory->filePath(QStringLiteral("secret-tool"));
  QFile helper(helperPath);
  if (!helper.open(QIODevice::WriteOnly)) {
    return false;
  }
  const QByteArray script = QByteArrayLiteral(R"SH(#!/bin/sh
case "$1" in
  store) IFS= read -r ignored; exit 0 ;;
  lookup) printf '%s\n' 'fixture-password'; exit 0 ;;
  clear) exit 0 ;;
  *) exit 2 ;;
esac
)SH");
  if (helper.write(script) != script.size()) {
    return false;
  }
  helper.close();
  if (!QFile::setPermissions(helperPath, QFileDevice::ReadOwner |
                                             QFileDevice::WriteOwner |
                                             QFileDevice::ExeOwner)) {
    return false;
  }
  *originalPath = qgetenv("PATH");
  qputenv("PATH", helperDirectory->path().toUtf8() + ':' + *originalPath);
  return true;
}

QString setupRangeAccount(CalDavRangeFixture* server, Database* database,
                          caldav::CalDavSync* sync, QString* error) {
  const QString accountId = sync->createAccount(
      server->endpoint().toString(), QStringLiteral("fixture-user"),
      QStringLiteral("fixture-password"), QStringLiteral("Range fixture"), error);
  if (accountId.isEmpty()) {
    return {};
  }
  QElapsedTimer wait;
  wait.start();
  while (wait.elapsed() < 5000 &&
         (database->calendars(accountId).isEmpty() ||
          sync->status(accountId).value(QStringLiteral("state")).toString() !=
              QStringLiteral("idle"))) {
    QTest::qWait(20);
  }
  return database->calendars(accountId).isEmpty() ? QString() : accountId;
}

OutboxItem queueFutureMutation(Database* database, caldav::CalDavSync* sync,
                               const QString& accountId, QString* error) {
  const Calendar calendar = database->calendars(accountId).first();
  Event master = database->eventByUid(calendar.id,
                                      QStringLiteral("series@example.test"), {}, error);
  if (master.id.isEmpty()) {
    return {};
  }
  Event future = master;
  future.id = newUuid();
  future.remoteId += QStringLiteral("#TZID=America/New_York:20260918T090000");
  future.recurrenceRule.clear();
  future.recurrenceId = QStringLiteral("TZID=America/New_York:20260918T090000");
  future.summary = QStringLiteral("Future probe test");
  future.startUtc = QDateTime(QDate(2026, 9, 18), QTime(15, 0), QTimeZone::UTC);
  future.endUtc = future.startUtc.addSecs(3600);
  future.createdAt = {};
  future.localRevision = 0;
  if (!database->saveLocalEvent(&future, OutboxOperation::Update, error,
                                QStringLiteral("future-probe-test"),
                                QStringLiteral("future"), QStringLiteral("none"), 0)) {
    return {};
  }
  sync->syncAccount(accountId);
  QElapsedTimer wait;
  wait.start();
  OutboxItem result;
  while (wait.elapsed() < 5000) {
    const QList<OutboxItem> items = database->outboxItems(100);
    const auto found =
        std::find_if(items.cbegin(), items.cend(), [](const OutboxItem& item) {
          return item.idempotencyKey == QStringLiteral("future-probe-test");
        });
    if (found != items.cend()) {
      result = *found;
      if (result.state == OutboxState::Done || result.state == OutboxState::Blocked) {
        break;
      }
    }
    QTest::qWait(20);
  }
  return result;
}

}  // namespace

class CalDavHardeningTest final : public QObject {
  Q_OBJECT

 private slots:
  void endpointAndHrefSafety();
  void crossOriginCredentialsAreRejected();
  void oversizedResponseIsRejectedBeforeBuffering();
  void crossOriginRedirectIsNotFollowed();
  void schedulingRequiresProof();
  void valarmsParseIntoStructuredReminders();
  void multipleValarmsSerializeAndRoundTrip();
  void invalidAndEmptyReminderWritesAreSafe();
  void reminderPatchPreservesProviderAlarmData();
  void patchPreservesRetainedResource();
  void patchTargetsOneRecurrenceInstance();
  void attendeeRsvpPatchPreservesProviderParameters();
  void scopedRecurrenceMutationsPreserveSiblings();
  void multigetBatchesAreBounded();
  void readResourceUsesCalendarMultiGetReport();
  void scheduleReplyHeaderIsExplicit();
  void moveRequestIsConditionalAndSameOrigin();
  void mutationIdentitySurvivesPatch();
  void credentialStorageAndRestoreAreAsynchronous();
  void boundedCalendarQueryUsesRequestedUtcRange();
  void futureRangeRequiresProofAndCommitsCanonicalEcho();
  void futureRangeProbeRejectsNonRetention();
  void futureRangeProbeRequiresCleanup();
  void futureRangeProbeRemovesDeterministicStaleResource();
  void futureRangeProbeCleansUpLostCreateAcknowledgment();
};

void CalDavHardeningTest::endpointAndHrefSafety() {
  QString error;
  QVERIFY(caldav::CalDavClient::validateEndpoint(
      QUrl(QStringLiteral("https://calendar.example.test/dav/")), &error));
  QVERIFY(!caldav::CalDavClient::validateEndpoint(
      QUrl(QStringLiteral("http://calendar.example.test/dav/")), &error));
  QVERIFY(!caldav::CalDavClient::validateEndpoint(
      QUrl(QStringLiteral("https://alice:secret@calendar.example.test/dav/")), &error));
  QVERIFY(caldav::CalDavClient::validateEndpoint(
      QUrl(QStringLiteral("http://127.0.0.1:8080/dav/")), &error));

  const QUrl base(QStringLiteral("https://CALENDAR.example.test:443/a/b/"));
  const QString relative = caldav::CalDavClient::canonicalResourceId(
      base, QStringLiteral("../b/event%20one.ics#ignored"));
  const QString absolute = caldav::CalDavClient::canonicalResourceId(
      base, QStringLiteral("https://calendar.example.test/a/b/event%20one.ics"));
  QCOMPARE(relative, absolute);
  QCOMPARE(relative,
           QStringLiteral("https://calendar.example.test/a/b/event%20one.ics"));
  QVERIFY(caldav::CalDavClient::isSameOrigin(
      base, QUrl(QStringLiteral("https://calendar.example.test/a"))));
  QVERIFY(!caldav::CalDavClient::isSameOrigin(
      base, QUrl(QStringLiteral("https://calendar.example.test:444/a"))));
}

void CalDavHardeningTest::crossOriginCredentialsAreRejected() {
  caldav::CalDavClient client;
  const QUrl origin(QStringLiteral("http://127.0.0.1:18181/dav/"));
  client.setCredentials(QStringLiteral("account"), QStringLiteral("alice"),
                        QStringLiteral("secret"), origin);
  const caldav::DavResponse response =
      awaitResponse([&](caldav::CalDavClient::Callback callback) {
        client.discoverPrincipal(QStringLiteral("account"),
                                 QUrl(QStringLiteral("http://127.0.0.1:18182/dav/")),
                                 std::move(callback));
      });
  QVERIFY(!response.ok);
  QCOMPARE(response.errorCode, QStringLiteral("cross_origin_request"));
}

void CalDavHardeningTest::boundedCalendarQueryUsesRequestedUtcRange() {
  const QByteArray body = QByteArrayLiteral(
      "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" "
      "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"></d:multistatus>");
  HttpFixture server(
      QByteArrayLiteral("HTTP/1.1 207 Multi-Status\r\n"
                        "Content-Type: application/xml\r\nContent-Length: ") +
      QByteArray::number(body.size()) +
      QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body);
  QVERIFY(server.listen());
  caldav::CalDavClient client;
  client.setCredentials(QStringLiteral("account"), QStringLiteral("alice"),
                        QStringLiteral("secret"), server.url());
  const QDateTime start(QDate(2010, 1, 1), QTime(0, 0), QTimeZone::UTC);
  const QDateTime end(QDate(2011, 1, 1), QTime(0, 0), QTimeZone::UTC);
  const caldav::DavResponse response =
      awaitResponse([&](caldav::CalDavClient::Callback callback) {
        client.queryCalendar(QStringLiteral("account"), server.url(), start, end,
                             std::move(callback));
      });
  QVERIFY2(response.ok, qPrintable(response.errorMessage));
  const QByteArray request = server.request();
  QVERIFY(request.startsWith("REPORT "));
  QVERIFY(
      request.contains("<c:time-range start=\"20100101T000000Z\" "
                       "end=\"20110101T000000Z\"/>"));
}

void CalDavHardeningTest::oversizedResponseIsRejectedBeforeBuffering() {
  const QByteArray response =
      QByteArrayLiteral("HTTP/1.1 207 Multi-Status\r\nContent-Length: ") +
      QByteArray::number(caldav::CalDavClient::maximumResponseBytes() + 1) +
      QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
  HttpFixture server(response);
  QVERIFY(server.listen());
  caldav::CalDavClient client;
  client.setCredentials(QStringLiteral("account"), QStringLiteral("alice"),
                        QStringLiteral("secret"), server.url());
  const caldav::DavResponse result =
      awaitResponse([&](caldav::CalDavClient::Callback callback) {
        client.discoverPrincipal(QStringLiteral("account"), server.url(),
                                 std::move(callback));
      });
  QCOMPARE(result.errorCode, QStringLiteral("response_too_large"));
  QVERIFY(result.body.isEmpty());
}

void CalDavHardeningTest::crossOriginRedirectIsNotFollowed() {
  HttpFixture destination(QByteArrayLiteral(
      "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
  QVERIFY(destination.listen());
  const QByteArray redirect =
      QByteArrayLiteral("HTTP/1.1 302 Found\r\nLocation: ") +
      destination.url().toEncoded() +
      QByteArrayLiteral("\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  HttpFixture source(redirect);
  QVERIFY(source.listen());

  caldav::CalDavClient client;
  client.setCredentials(QStringLiteral("account"), QStringLiteral("alice"),
                        QStringLiteral("secret"), source.url());
  const caldav::DavResponse result =
      awaitResponse([&](caldav::CalDavClient::Callback callback) {
        client.discoverPrincipal(QStringLiteral("account"), source.url(),
                                 std::move(callback));
      });
  QVERIFY(!result.ok);
  QTRY_COMPARE_WITH_TIMEOUT(destination.connections(), 0, 250);
  QVERIFY(source.request().contains("Authorization: Basic "));
}

void CalDavHardeningTest::schedulingRequiresProof() {
  const QByteArray withoutOutbox = QByteArrayLiteral(
      "<d:multistatus xmlns:d=\"DAV:\" "
      "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:response>"
      "<d:href>/principal/</d:href><d:propstat><d:prop>"
      "<c:calendar-user-address-set><d:href>mailto:alice@example.test</d:href>"
      "</c:calendar-user-address-set></d:prop>"
      "<d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
      "</d:response></d:multistatus>");
  auto parsed = caldav::CalDavXml::parseMultiStatus(withoutOutbox);
  QVERIFY(parsed.ok());
  QVERIFY(!caldav::CalDavXml::schedulingCapabilities(parsed).canSend());

  const QByteArray withOutbox = QByteArrayLiteral(
      "<d:multistatus xmlns:d=\"DAV:\" "
      "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:response>"
      "<d:href>/principal/</d:href><d:propstat><d:prop>"
      "<c:calendar-user-address-set><d:href>mailto:alice@example.test</d:href>"
      "</c:calendar-user-address-set>"
      "<c:schedule-inbox-URL><d:href>/inbox/</d:href></c:schedule-inbox-URL>"
      "<c:schedule-outbox-URL><d:href>/outbox/</d:href></c:schedule-outbox-URL>"
      "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
      "</d:response></d:multistatus>");
  parsed = caldav::CalDavXml::parseMultiStatus(withOutbox);
  QVERIFY(parsed.ok());
  const caldav::CalDavSchedulingCapabilities scheduling =
      caldav::CalDavXml::schedulingCapabilities(parsed);
  QVERIFY(scheduling.canSend());
  QCOMPARE(scheduling.outboxHref, QStringLiteral("/outbox/"));
  QCOMPARE(scheduling.userAddresses,
           QStringList{QStringLiteral("mailto:alice@example.test")});
}

void CalDavHardeningTest::valarmsParseIntoStructuredReminders() {
  const QByteArray resource = QByteArrayLiteral(
      "BEGIN:VCALENDAR\r\n"
      "VERSION:2.0\r\n"
      "PRODID:-//Reminder fixture//EN\r\n"
      "BEGIN:VEVENT\r\n"
      "UID:alarm-parse@example.test\r\n"
      "DTSTAMP:20260801T120000Z\r\n"
      "DTSTART:20260828T130000Z\r\n"
      "DTEND:20260828T140000Z\r\n"
      "SUMMARY:Alarm parsing\r\n"
      "BEGIN:VALARM\r\n"
      "ACTION:DISPLAY\r\n"
      "TRIGGER:-PT15M\r\n"
      "DESCRIPTION:Popup\r\n"
      "END:VALARM\r\n"
      "BEGIN:VALARM\r\n"
      "ACTION:EMAIL\r\n"
      "TRIGGER:-PT1H\r\n"
      "SUMMARY:Email\r\n"
      "DESCRIPTION:Email reminder\r\n"
      "ATTENDEE:mailto:owner@example.test\r\n"
      "END:VALARM\r\n"
      "BEGIN:VALARM\r\n"
      "ACTION:AUDIO\r\n"
      "TRIGGER;VALUE=DATE-TIME:20260828T120000Z\r\n"
      "END:VALARM\r\n"
      "BEGIN:VALARM\r\n"
      "ACTION:DISPLAY\r\n"
      "TRIGGER;RELATED=END:-PT5M\r\n"
      "DESCRIPTION:Unrepresentable end-relative reminder\r\n"
      "END:VALARM\r\n"
      "END:VEVENT\r\n"
      "END:VCALENDAR\r\n");

  const caldav::ICalendarParseResult parsed = caldav::ICalendarCodec::parse(resource);
  QVERIFY2(parsed.ok(), qPrintable(parsed.error.message));
  QCOMPARE(parsed.events.size(), 1);
  const QJsonArray reminders = parsed.events.first().reminders;
  QCOMPARE(reminders.size(), 3);
  QCOMPARE(reminders.at(0).toObject().value(QStringLiteral("method")).toString(),
           QStringLiteral("popup"));
  QCOMPARE(reminders.at(0).toObject().value(QStringLiteral("minutes")).toInt(), 15);
  QCOMPARE(reminders.at(1).toObject().value(QStringLiteral("method")).toString(),
           QStringLiteral("email"));
  QCOMPARE(reminders.at(1).toObject().value(QStringLiteral("minutes")).toInt(), 60);
  QCOMPARE(reminders.at(2).toObject().value(QStringLiteral("method")).toString(),
           QStringLiteral("audio"));
  QCOMPARE(reminders.at(2).toObject().value(QStringLiteral("at")).toString(),
           QStringLiteral("2026-08-28T12:00:00.000Z"));
}

void CalDavHardeningTest::multipleValarmsSerializeAndRoundTrip() {
  Event event;
  event.uid = QStringLiteral("alarm-write@example.test");
  event.summary = QStringLiteral("Multiple reminders");
  event.startUtc = QDateTime(QDate(2026, 8, 28), QTime(13, 0), QTimeZone::UTC);
  event.endUtc = event.startUtc.addSecs(3600);
  event.startTimeZone = QStringLiteral("UTC");
  event.endTimeZone = QStringLiteral("UTC");
  event.reminders = QJsonArray{
      QJsonObject{{QStringLiteral("method"), QStringLiteral("popup")},
                  {QStringLiteral("minutes"), 15}},
      QJsonObject{{QStringLiteral("method"), QStringLiteral("email")},
                  {QStringLiteral("minutesBefore"), 60}},
      QJsonObject{{QStringLiteral("method"), QStringLiteral("audio")},
                  {QStringLiteral("at"), QStringLiteral("2026-08-28T12:00:00.000Z")}}};

  const caldav::ICalendarSerializeResult encoded =
      caldav::ICalendarCodec::serialize(event);
  QVERIFY2(encoded.ok(), qPrintable(encoded.error.message));
  QCOMPARE(encoded.payload.count("BEGIN:VALARM"), 3);
  QVERIFY(encoded.payload.contains("ACTION:DISPLAY\r\n"));
  QVERIFY(encoded.payload.contains("ACTION:EMAIL\r\n"));
  QVERIFY(encoded.payload.contains("ACTION:AUDIO\r\n"));
  QVERIFY2(encoded.payload.contains("TRIGGER:-PT900S\r\n"),
           encoded.payload.constData());
  QVERIFY(encoded.payload.contains("TRIGGER:-PT3600S\r\n"));
  QVERIFY(encoded.payload.contains("TRIGGER;VALUE=DATE-TIME:20260828T120000Z\r\n"));

  const caldav::ICalendarParseResult reparsed =
      caldav::ICalendarCodec::parse(encoded.payload);
  QVERIFY2(reparsed.ok(), qPrintable(reparsed.error.message));
  QCOMPARE(reparsed.events.first().reminders.size(), 3);
  QHash<QString, QJsonObject> remindersByMethod;
  for (const QJsonValue& value : reparsed.events.first().reminders) {
    const QJsonObject reminder = value.toObject();
    remindersByMethod.insert(reminder.value(QStringLiteral("method")).toString(),
                             reminder);
  }
  QCOMPARE(remindersByMethod.value(QStringLiteral("popup"))
               .value(QStringLiteral("minutes"))
               .toInt(),
           15);
  QCOMPARE(remindersByMethod.value(QStringLiteral("email"))
               .value(QStringLiteral("minutes"))
               .toInt(),
           60);
  QCOMPARE(remindersByMethod.value(QStringLiteral("audio"))
               .value(QStringLiteral("at"))
               .toString(),
           QStringLiteral("2026-08-28T12:00:00.000Z"));
}

void CalDavHardeningTest::invalidAndEmptyReminderWritesAreSafe() {
  Event event;
  event.uid = QStringLiteral("alarm-validation@example.test");
  event.summary = QStringLiteral("Reminder validation");
  event.startUtc = QDateTime(QDate(2026, 8, 28), QTime(13, 0), QTimeZone::UTC);
  event.endUtc = event.startUtc.addSecs(3600);
  event.startTimeZone = QStringLiteral("UTC");
  event.endTimeZone = QStringLiteral("UTC");

  const caldav::ICalendarSerializeResult withoutReminders =
      caldav::ICalendarCodec::serialize(event);
  QVERIFY2(withoutReminders.ok(), qPrintable(withoutReminders.error.message));
  QVERIFY(!withoutReminders.payload.contains("BEGIN:VALARM"));

  event.reminders =
      QJsonArray{QJsonObject{{QStringLiteral("method"), QStringLiteral("sms")},
                             {QStringLiteral("minutes"), 10}}};
  const caldav::ICalendarSerializeResult unsupported =
      caldav::ICalendarCodec::serialize(event);
  QVERIFY(!unsupported.ok());
  QCOMPARE(unsupported.error.code, QStringLiteral("invalid_reminder"));

  event.reminders = QJsonArray{12.5};
  const caldav::ICalendarSerializeResult fractional =
      caldav::ICalendarCodec::serialize(event);
  QVERIFY(!fractional.ok());
  QCOMPARE(fractional.error.code, QStringLiteral("invalid_reminder"));
}

void CalDavHardeningTest::reminderPatchPreservesProviderAlarmData() {
  const caldav::ICalendarParseResult parsed =
      caldav::ICalendarCodec::parse(retainedResource());
  QVERIFY2(parsed.ok(), qPrintable(parsed.error.message));
  Event master = parsed.events.first();
  QCOMPARE(master.reminders.size(), 1);
  QJsonObject edited = master.reminders.first().toObject();
  edited.insert(QStringLiteral("minutes"), 30);
  master.reminders = QJsonArray{
      edited, QJsonObject{{QStringLiteral("method"), QStringLiteral("popup")},
                          {QStringLiteral("offsetMinutes"), 5}}};

  const caldav::ICalendarSerializeResult patched =
      caldav::ICalendarCodec::patch(master, retainedResource());
  QVERIFY2(patched.ok(), qPrintable(patched.error.message));
  QCOMPARE(patched.payload.count("BEGIN:VALARM"), 3);
  QVERIFY2(patched.payload.contains("TRIGGER:-PT1800S\r\n"),
           patched.payload.constData());
  QVERIFY(patched.payload.contains("TRIGGER:-PT300S\r\n"));
  QVERIFY(patched.payload.contains("DESCRIPTION:Reminder\r\n"));
  QVERIFY(patched.payload.contains("X-EXAMPLE-ALARM-ID:opaque-alarm\r\n"));
  QVERIFY(patched.payload.contains("X-UNEDITABLE-ALARM:keep-whole-component\r\n"));
  QVERIFY(patched.payload.contains("ATTACH:file:///provider/alarm-handler\r\n"));

  const caldav::ICalendarParseResult reparsed =
      caldav::ICalendarCodec::parse(patched.payload);
  QVERIFY2(reparsed.ok(), qPrintable(reparsed.error.message));
  Event cleared = reparsed.events.first();
  QCOMPARE(cleared.reminders.size(), 2);
  cleared.reminders = {};
  const caldav::ICalendarSerializeResult removed =
      caldav::ICalendarCodec::patch(cleared, patched.payload);
  QVERIFY2(removed.ok(), qPrintable(removed.error.message));
  QCOMPARE(removed.payload.count("BEGIN:VALARM"), 1);
  QVERIFY(!removed.payload.contains("X-EXAMPLE-ALARM-ID:opaque-alarm\r\n"));
  QVERIFY(removed.payload.contains("X-UNEDITABLE-ALARM:keep-whole-component\r\n"));
  const caldav::ICalendarParseResult removedParsed =
      caldav::ICalendarCodec::parse(removed.payload);
  QVERIFY2(removedParsed.ok(), qPrintable(removedParsed.error.message));
  QVERIFY(removedParsed.events.first().reminders.isEmpty());
}

void CalDavHardeningTest::patchPreservesRetainedResource() {
  const caldav::ICalendarParseResult parsed =
      caldav::ICalendarCodec::parse(retainedResource());
  QVERIFY2(parsed.ok(), qPrintable(parsed.error.message));
  QCOMPARE(parsed.events.size(), 2);
  Event master = parsed.events.first();
  master.summary = QStringLiteral("Updated master");
  master.description = QStringLiteral("New description");
  master.startUtc = master.startUtc.addSecs(30 * 60);
  master.endUtc = master.endUtc.addSecs(30 * 60);
  master.visibility = QStringLiteral("private");
  master.sequence = 9;

  const caldav::ICalendarSerializeResult patched =
      caldav::ICalendarCodec::patch(master, retainedResource());
  QVERIFY2(patched.ok(), qPrintable(patched.error.message));
  QVERIFY(patched.payload.contains("SUMMARY:Updated master\r\n"));
  QVERIFY(patched.payload.contains("SUMMARY:Preserved exception\r\n"));
  QVERIFY(patched.payload.contains("ORGANIZER;CN=Owner:mailto:owner@example.test"));
  QVERIFY(patched.payload.contains(
      "ATTENDEE;CN=Guest;PARTSTAT=ACCEPTED:mailto:guest@example.test"));
  QVERIFY(patched.payload.contains("BEGIN:VALARM\r\n"));
  QVERIFY(patched.payload.contains("URL:https://meet.example.test/room\r\n"));
  QVERIFY(patched.payload.contains("RDATE;TZID=America/New_York:"));
  QVERIFY(patched.payload.contains("EXDATE;TZID=America/New_York:"));
  QVERIFY(patched.payload.contains("X-EXAMPLE-PROVIDER-ID:opaque-value\r\n"));
  QVERIFY(patched.payload.contains("X-EXCEPTION-DATA:keep-me\r\n"));
  QVERIFY(patched.payload.contains("BEGIN:VTODO\r\n"));
  QVERIFY(patched.payload.contains("X-WR-CALNAME:Preserved calendar\r\n"));

  const caldav::ICalendarParseResult reparsed =
      caldav::ICalendarCodec::parse(patched.payload);
  QVERIFY2(reparsed.ok(), qPrintable(reparsed.error.message));
  QCOMPARE(reparsed.events.size(), 2);
  QCOMPARE(reparsed.events.first().summary, QStringLiteral("Updated master"));
  QCOMPARE(reparsed.events.first().url,
           QStringLiteral("https://meet.example.test/room"));
  QCOMPARE(reparsed.events.first().visibility, QStringLiteral("private"));
}

void CalDavHardeningTest::patchTargetsOneRecurrenceInstance() {
  const caldav::ICalendarParseResult parsed =
      caldav::ICalendarCodec::parse(retainedResource());
  QVERIFY(parsed.ok());
  Event exception = parsed.events.at(1);
  exception.summary = QStringLiteral("Updated exception only");

  const caldav::ICalendarSerializeResult patched =
      caldav::ICalendarCodec::patch(exception, retainedResource());
  QVERIFY2(patched.ok(), qPrintable(patched.error.message));
  QVERIFY(patched.payload.contains("SUMMARY:Original master\r\n"));
  QVERIFY(patched.payload.contains("SUMMARY:Updated exception only\r\n"));
  QVERIFY(patched.payload.contains("X-EXCEPTION-DATA:keep-me\r\n"));

  exception.recurrenceId = QStringLiteral("20261002T090000");
  const caldav::ICalendarSerializeResult missing =
      caldav::ICalendarCodec::patch(exception, retainedResource());
  QVERIFY(!missing.ok());
  QCOMPARE(missing.error.code, QStringLiteral("event_not_found"));
}

void CalDavHardeningTest::attendeeRsvpPatchPreservesProviderParameters() {
  QByteArray resource = retainedResource();
  resource.replace("ATTENDEE;CN=Guest;PARTSTAT=ACCEPTED:mailto:guest@example.test",
                   "ATTENDEE;CN=Guest;PARTSTAT=ACCEPTED;ROLE=REQ-PARTICIPANT;RSVP=TRUE;"
                   "X-PROVIDER-ATTENDEE=opaque:mailto:guest@example.test");
  const caldav::ICalendarParseResult parsed = caldav::ICalendarCodec::parse(resource);
  QVERIFY2(parsed.ok(), qPrintable(parsed.error.message));
  Event master = parsed.events.first();
  QCOMPARE(master.organizer.value(QStringLiteral("email")).toString(),
           QStringLiteral("owner@example.test"));
  QCOMPARE(master.attendees.size(), 1);
  QJsonObject guest = master.attendees.first().toObject();
  QCOMPARE(guest.value(QStringLiteral("responseStatus")).toString(),
           QStringLiteral("accepted"));
  QCOMPARE(guest.value(QStringLiteral("role")).toString(),
           QStringLiteral("REQ-PARTICIPANT"));
  QVERIFY(guest.value(QStringLiteral("rsvp")).toBool());

  guest.insert(QStringLiteral("responseStatus"), QStringLiteral("declined"));
  guest.insert(QStringLiteral("partstat"), QStringLiteral("DECLINED"));
  master.attendees.replace(0, guest);
  const caldav::ICalendarSerializeResult patched =
      caldav::ICalendarCodec::patch(master, resource);
  QVERIFY2(patched.ok(), qPrintable(patched.error.message));
  QVERIFY(patched.payload.contains("PARTSTAT=DECLINED"));
  QVERIFY(patched.payload.contains("X-PROVIDER-ATTENDEE=opaque"));
  QVERIFY(patched.payload.contains("ROLE=REQ-PARTICIPANT"));
  QVERIFY(patched.payload.contains("BEGIN:VALARM"));

  const caldav::ICalendarParseResult reparsed =
      caldav::ICalendarCodec::parse(patched.payload);
  QVERIFY(reparsed.ok());
  QCOMPARE(reparsed.events.first()
               .attendees.first()
               .toObject()
               .value(QStringLiteral("partstat"))
               .toString(),
           QStringLiteral("DECLINED"));
}

void CalDavHardeningTest::scopedRecurrenceMutationsPreserveSiblings() {
  const caldav::ICalendarParseResult parsed =
      caldav::ICalendarCodec::parse(retainedResource());
  QVERIFY(parsed.ok());
  Event occurrence = parsed.events.first();
  occurrence.recurrenceId = QStringLiteral("TZID=America/New_York:20260918T090000");
  occurrence.recurrenceRule.clear();
  occurrence.startUtc = QDateTime(QDate(2026, 9, 18), QTime(14, 0), QTimeZone::UTC);
  occurrence.endUtc = occurrence.startUtc.addSecs(3600);
  occurrence.summary = QStringLiteral("New detached occurrence");

  const caldav::ICalendarSerializeResult added = caldav::ICalendarCodec::patchScoped(
      occurrence, retainedResource(), QStringLiteral("occurrence"));
  QVERIFY2(added.ok(), qPrintable(added.error.message));
  QVERIFY(added.payload.contains("SUMMARY:Original master"));
  QVERIFY(added.payload.contains("SUMMARY:Preserved exception"));
  QVERIFY(added.payload.contains("SUMMARY:New detached occurrence"));
  QCOMPARE(caldav::ICalendarCodec::parse(added.payload).events.size(), 3);

  const caldav::ICalendarSerializeResult cancelled =
      caldav::ICalendarCodec::patchScoped(occurrence, added.payload,
                                          QStringLiteral("occurrence"), true);
  QVERIFY2(cancelled.ok(), qPrintable(cancelled.error.message));
  const caldav::ICalendarParseResult cancelledParsed =
      caldav::ICalendarCodec::parse(cancelled.payload);
  QVERIFY(cancelledParsed.ok());
  QCOMPARE(cancelledParsed.events.size(), 3);
  const auto cancelledEvent =
      std::find_if(cancelledParsed.events.cbegin(), cancelledParsed.events.cend(),
                   [&occurrence](const Event& event) {
                     return event.recurrenceId == occurrence.recurrenceId;
                   });
  QVERIFY(cancelledEvent != cancelledParsed.events.cend());
  QCOMPARE(cancelledEvent->status, QStringLiteral("cancelled"));

  Event future = occurrence;
  future.recurrenceId = QStringLiteral("TZID=America/New_York:20260925T090000");
  future.summary = QStringLiteral("Changed from here");
  const caldav::ICalendarSerializeResult ranged = caldav::ICalendarCodec::patchScoped(
      future, retainedResource(), QStringLiteral("future"));
  QVERIFY2(ranged.ok(), qPrintable(ranged.error.message));
  QVERIFY(ranged.payload.contains("RANGE=THISANDFUTURE"));
  const caldav::ICalendarParseResult rangedParsed =
      caldav::ICalendarCodec::parse(ranged.payload);
  QVERIFY(rangedParsed.ok());
  QVERIFY(std::any_of(
      rangedParsed.events.cbegin(), rangedParsed.events.cend(), [](const Event& event) {
        return event.recurrenceId.contains(QStringLiteral("RANGE=THISANDFUTURE"));
      }));

  Event generated = parsed.events.first();
  generated.recurrenceRule.clear();
  generated.recurrenceId = QStringLiteral("2026-09-20T13:00:00.000Z");
  generated.startUtc = QDateTime(QDate(2026, 9, 20), QTime(15, 0), QTimeZone::UTC);
  generated.endUtc = generated.startUtc.addSecs(3600);
  generated.summary = QStringLiteral("Generated ISO occurrence");
  const caldav::ICalendarSerializeResult generatedPatch =
      caldav::ICalendarCodec::patchScoped(generated, retainedResource(),
                                          QStringLiteral("occurrence"));
  QVERIFY2(generatedPatch.ok(), qPrintable(generatedPatch.error.message));
  QVERIFY(generatedPatch.payload.contains(
      "RECURRENCE-ID;TZID=America/New_York:20260920T090000"));
  const caldav::ICalendarParseResult generatedParsed =
      caldav::ICalendarCodec::parse(generatedPatch.payload);
  QVERIFY(generatedParsed.ok());
  QVERIFY(std::any_of(generatedParsed.events.cbegin(), generatedParsed.events.cend(),
                      [](const Event& event) {
                        return event.summary ==
                               QStringLiteral("Generated ISO occurrence");
                      }));
}

void CalDavHardeningTest::multigetBatchesAreBounded() {
  QStringList hrefs;
  for (int index = 0; index < 251; ++index) {
    hrefs.append(QStringLiteral("/calendar/event-%1.ics").arg(index));
  }
  const QList<QStringList> batches = caldav::CalDavClient::batchMultiGetHrefs(hrefs);
  QCOMPARE(batches.size(), 3);
  qsizetype total = 0;
  for (const QStringList& batch : batches) {
    QVERIFY(batch.size() <= caldav::CalDavClient::maximumMultiGetHrefs());
    total += batch.size();
  }
  QCOMPARE(total, hrefs.size());
  QCOMPARE(batches.first().first(), hrefs.first());
  QCOMPARE(batches.last().last(), hrefs.last());
}

void CalDavHardeningTest::readResourceUsesCalendarMultiGetReport() {
  const QByteArray calendarData = QByteArrayLiteral(
      "BEGIN:VCALENDAR\r\n"
      "VERSION:2.0\r\n"
      "PRODID:-//Conflict fixture//EN\r\n"
      "BEGIN:VEVENT\r\n"
      "UID:remote-conflict@example.test\r\n"
      "DTSTAMP:20260829T120000Z\r\n"
      "DTSTART:20260830T130000Z\r\n"
      "DTEND:20260830T140000Z\r\n"
      "SUMMARY:Current remote snapshot\r\n"
      "END:VEVENT\r\n"
      "END:VCALENDAR\r\n");
  const QByteArray multistatus =
      QByteArrayLiteral(
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
          "<d:multistatus xmlns:d=\"DAV:\" "
          "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:response>"
          "<d:href>/dav/calendar/event%20one.ics</d:href><d:propstat><d:prop>"
          "<d:getetag>\"remote-v2\"</d:getetag><c:calendar-data><![CDATA[") +
      calendarData +
      QByteArrayLiteral(
          "]]></c:calendar-data></d:prop><d:status>HTTP/1.1 200 OK</d:status>"
          "</d:propstat></d:response></d:multistatus>");
  const QByteArray response =
      QByteArrayLiteral(
          "HTTP/1.1 207 Multi-Status\r\nContent-Type: application/xml\r\n"
          "Content-Length: ") +
      QByteArray::number(multistatus.size()) +
      QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + multistatus;
  HttpFixture server(response);
  QVERIFY(server.listen());

  caldav::CalDavClient client;
  client.setCredentials(QStringLiteral("account"), QStringLiteral("alice"),
                        QStringLiteral("secret"), server.url());
  const caldav::DavResponse result =
      awaitResponse([&](caldav::CalDavClient::Callback callback) {
        client.readResource(QStringLiteral("account"),
                            server.url(QStringLiteral("/dav/calendar/event one.ics")),
                            std::move(callback));
      });
  QVERIFY2(result.ok, qPrintable(result.errorMessage));
  QVERIFY(server.request().startsWith("REPORT /dav/calendar/ HTTP/1.1\r\n"));
  QVERIFY(server.request().contains("Depth: 1\r\n"));
  QVERIFY(server.request().contains("<c:calendar-multiget"));
  QVERIFY(server.request().contains("<d:href>/dav/calendar/event%20one.ics</d:href>"));
  QVERIFY(!server.request().startsWith("PROPFIND "));

  const caldav::CalDavMultiStatusResult parsed =
      caldav::CalDavXml::parseMultiStatus(result.body);
  QVERIFY2(parsed.ok(), qPrintable(parsed.error.message));
  const QList<caldav::CalDavResource> resources = caldav::CalDavXml::resources(parsed);
  QCOMPARE(resources.size(), 1);
  QCOMPARE(resources.first().href, QStringLiteral("/dav/calendar/event%20one.ics"));
  QCOMPARE(resources.first().etag, QStringLiteral("\"remote-v2\""));
  QByteArray normalizedCalendarData = calendarData;
  normalizedCalendarData.replace("\r\n", "\n");
  QCOMPARE(resources.first().calendarData.toUtf8(), normalizedCalendarData);
  const caldav::ICalendarParseResult event =
      caldav::ICalendarCodec::parse(resources.first().calendarData.toUtf8());
  QVERIFY2(event.ok(), qPrintable(event.error.message));
  QCOMPARE(event.events.first().summary, QStringLiteral("Current remote snapshot"));
}

void CalDavHardeningTest::scheduleReplyHeaderIsExplicit() {
  HttpFixture server(
      QByteArrayLiteral("HTTP/1.1 204 No Content\r\nETag: v2\r\nContent-Length: 0\r\n"
                        "Connection: close\r\n\r\n"));
  QVERIFY(server.listen());
  caldav::CalDavClient client;
  client.setCredentials(QStringLiteral("account"), QStringLiteral("alice"),
                        QStringLiteral("secret"), server.url());
  const caldav::DavResponse response =
      awaitResponse([&](caldav::CalDavClient::Callback callback) {
        client.updateEvent(QStringLiteral("account"), server.url("/dav/event.ics"),
                           QStringLiteral("v1"), retainedResource(),
                           std::move(callback), QByteArrayLiteral("T"));
      });
  QVERIFY(response.ok);
  QVERIFY(server.request().contains("Schedule-Reply: T\r\n"));
  QVERIFY(server.request().contains("If-Match: v1\r\n"));
}

void CalDavHardeningTest::moveRequestIsConditionalAndSameOrigin() {
  HttpFixture server(
      QByteArrayLiteral("HTTP/1.1 201 Created\r\nContent-Length: 0\r\n"
                        "Connection: close\r\n\r\n"));
  QVERIFY(server.listen());
  caldav::CalDavClient client;
  client.setCredentials(QStringLiteral("account"), QStringLiteral("alice"),
                        QStringLiteral("secret"), server.url());
  const QUrl source = server.url(QStringLiteral("/dav/source/event.ics"));
  const QUrl target = server.url(QStringLiteral("/dav/target/event.ics"));
  const caldav::DavResponse response =
      awaitResponse([&](caldav::CalDavClient::Callback callback) {
        client.moveEvent(QStringLiteral("account"), source, target,
                         QStringLiteral("source-etag"), std::move(callback),
                         QByteArrayLiteral("F"));
      });
  QVERIFY(response.ok);
  QVERIFY(server.request().startsWith("MOVE /dav/source/event.ics HTTP/1.1\r\n"));
  QVERIFY(server.request().contains("If-Match: source-etag\r\n"));
  QVERIFY(server.request().contains("Overwrite: F\r\n"));
  QVERIFY(server.request().contains("Schedule-Reply: F\r\n"));
  QVERIFY(server.request().contains(QByteArrayLiteral("Destination: ") +
                                    target.toEncoded(QUrl::FullyEncoded) +
                                    QByteArrayLiteral("\r\n")));

  const caldav::DavResponse rejected =
      awaitResponse([&](caldav::CalDavClient::Callback callback) {
        client.moveEvent(QStringLiteral("account"), source,
                         QUrl(QStringLiteral("https://other.example.test/event.ics")),
                         QStringLiteral("source-etag"), std::move(callback));
      });
  QCOMPARE(rejected.errorCode, QStringLiteral("invalid_move_destination"));
}

void CalDavHardeningTest::mutationIdentitySurvivesPatch() {
  Event event;
  event.uid = QStringLiteral("create@example.test");
  event.summary = QStringLiteral("Created once");
  event.startUtc = QDateTime(QDate(2026, 8, 28), QTime(13, 0), QTimeZone::UTC);
  event.endUtc = event.startUtc.addSecs(3600);
  event.startTimeZone = QStringLiteral("UTC");
  event.endTimeZone = QStringLiteral("UTC");

  const caldav::ICalendarSerializeResult encoded =
      caldav::ICalendarCodec::serialize(event);
  QVERIFY(encoded.ok());
  const caldav::ICalendarSerializeResult stamped =
      caldav::ICalendarCodec::stampClientMutationId(encoded.payload,
                                                    QStringLiteral("mutation-123"));
  QVERIFY2(stamped.ok(), qPrintable(stamped.error.message));
  QVERIFY(caldav::ICalendarCodec::hasClientMutationId(stamped.payload,
                                                      QStringLiteral("mutation-123")));
  QVERIFY(!caldav::ICalendarCodec::hasClientMutationId(stamped.payload,
                                                       QStringLiteral("mutation-456")));

  event.rawPayload = QString::fromUtf8(stamped.payload);
  event.summary = QStringLiteral("Updated later");
  const caldav::ICalendarSerializeResult patched =
      caldav::ICalendarCodec::patch(event, stamped.payload);
  QVERIFY2(patched.ok(), qPrintable(patched.error.message));
  QVERIFY(caldav::ICalendarCodec::hasClientMutationId(patched.payload,
                                                      QStringLiteral("mutation-123")));
}

void CalDavHardeningTest::credentialStorageAndRestoreAreAsynchronous() {
  QTemporaryDir helperDirectory;
  QVERIFY(helperDirectory.isValid());
  const QString helperPath = helperDirectory.filePath(QStringLiteral("secret-tool"));
  QFile helper(helperPath);
  QVERIFY(helper.open(QIODevice::WriteOnly));
  const QByteArray script = QByteArrayLiteral(R"SH(#!/bin/sh
case "$1" in
  store) /bin/sleep 0.15; IFS= read -r ignored; exit 0 ;;
  lookup) /bin/sleep 0.15; printf '%s\n' 'fixture-password'; exit 0 ;;
  clear) exit 0 ;;
  *) exit 2 ;;
esac
)SH");
  QCOMPARE(helper.write(script), script.size());
  helper.close();
  QVERIFY(QFile::setPermissions(helperPath, QFileDevice::ReadOwner |
                                                QFileDevice::WriteOwner |
                                                QFileDevice::ExeOwner));
  const QByteArray originalPath = qgetenv("PATH");
  qputenv("PATH", helperDirectory.path().toUtf8() + ':' + originalPath);

  Database database;
  QString error;
  QVERIFY2(database.open(QStringLiteral(":memory:"), &error), qPrintable(error));
  caldav::CalDavSync sync(&database);
  QElapsedTimer elapsed;
  elapsed.start();
  const QString accountId = sync.createAccount(
      QStringLiteral("http://127.0.0.1:9/dav/"), QStringLiteral("fixture-user"),
      QStringLiteral("fixture-password"), QStringLiteral("Fixture CalDAV"), &error);
  QVERIFY2(!accountId.isEmpty(), qPrintable(error));
  QVERIFY2(elapsed.elapsed() < 100,
           "createAccount waited for Secret Service instead of returning");
  QCOMPARE(database.account(accountId).authStatus,
           QStringLiteral("credential_storage_pending"));
  QTRY_VERIFY_WITH_TIMEOUT(database.account(accountId).authStatus !=
                               QStringLiteral("credential_storage_pending"),
                           2000);

  QVERIFY(sync.disconnectAccount(accountId, false, &error));
  Account disconnected = database.account(accountId);
  QCOMPARE(disconnected.authStatus, QStringLiteral("disconnected"));
  elapsed.restart();
  QVERIFY(sync.updateCredentials(accountId, QStringLiteral("replacement-user"),
                                 QStringLiteral("replacement-password"), &error));
  QVERIFY2(elapsed.elapsed() < 100,
           "updateCredentials waited for Secret Service instead of returning");
  QCOMPARE(database.account(accountId).principal, QStringLiteral("replacement-user"));

  caldav::CalDavSync restored(&database);
  QSignalSpy statusChanged(&restored, &caldav::CalDavSync::syncStatusChanged);
  elapsed.restart();
  QVERIFY2(restored.restoreAccounts(&error), qPrintable(error));
  QVERIFY2(elapsed.elapsed() < 100,
           "restoreAccounts waited for Secret Service instead of returning");
  QTRY_VERIFY_WITH_TIMEOUT(statusChanged.count() > 0, 2000);
  qputenv("PATH", originalPath);
}

void CalDavHardeningTest::futureRangeRequiresProofAndCommitsCanonicalEcho() {
  QTemporaryDir helperDirectory;
  QVERIFY(helperDirectory.isValid());
  const QString helperPath = helperDirectory.filePath(QStringLiteral("secret-tool"));
  QFile helper(helperPath);
  QVERIFY(helper.open(QIODevice::WriteOnly));
  const QByteArray script = QByteArrayLiteral(R"SH(#!/bin/sh
case "$1" in
  store) IFS= read -r ignored; exit 0 ;;
  lookup) printf '%s\n' 'fixture-password'; exit 0 ;;
  clear) exit 0 ;;
  *) exit 2 ;;
esac
)SH");
  QCOMPARE(helper.write(script), script.size());
  helper.close();
  QVERIFY(QFile::setPermissions(helperPath, QFileDevice::ReadOwner |
                                                QFileDevice::WriteOwner |
                                                QFileDevice::ExeOwner));
  const QByteArray originalPath = qgetenv("PATH");
  qputenv("PATH", helperDirectory.path().toUtf8() + ':' + originalPath);

  CalDavRangeFixture server;
  QVERIFY(server.listen());
  Database database;
  QString error;
  QVERIFY2(database.open(QStringLiteral(":memory:"), &error), qPrintable(error));
  caldav::CalDavSync sync(&database);
  const QString accountId = sync.createAccount(
      server.endpoint().toString(), QStringLiteral("fixture-user"),
      QStringLiteral("fixture-password"), QStringLiteral("Range fixture"), &error);
  QVERIFY2(!accountId.isEmpty(), qPrintable(error));
  QTRY_VERIFY_WITH_TIMEOUT(!database.calendars(accountId).isEmpty(), 5000);
  QTRY_COMPARE_WITH_TIMEOUT(sync.status(accountId).value(QStringLiteral("state")),
                            QJsonValue(QStringLiteral("idle")), 5000);

  const Calendar calendar = database.calendars(accountId).first();
  QVERIFY(!calendar.capabilities.value(QStringLiteral("thisAndFuture")).toBool());
  QVERIFY(!calendar.capabilities.value(QStringLiteral("thisAndFutureProven")).toBool());
  Event master = database.eventByUid(calendar.id, QStringLiteral("series@example.test"),
                                     {}, &error);
  QVERIFY2(!master.id.isEmpty(), qPrintable(error));

  Event future = master;
  future.id = newUuid();
  future.remoteId += QStringLiteral("#TZID=America/New_York:20260918T090000");
  future.recurrenceRule.clear();
  future.recurrenceId = QStringLiteral("TZID=America/New_York:20260918T090000");
  future.summary = QStringLiteral("Future echo");
  future.startUtc = QDateTime(QDate(2026, 9, 18), QTime(15, 0), QTimeZone::UTC);
  future.endUtc = future.startUtc.addSecs(3600);
  future.createdAt = {};
  future.localRevision = 0;
  QVERIFY2(database.saveLocalEvent(&future, OutboxOperation::Update, &error,
                                   QStringLiteral("future-echo"),
                                   QStringLiteral("future"), QStringLiteral("none"), 0),
           qPrintable(error));
  sync.syncAccount(accountId);

  const auto operationByKey = [&database](const QString& key) {
    const QList<OutboxItem> operations = database.outboxItems(100);
    const auto found = std::find_if(
        operations.cbegin(), operations.cend(),
        [&key](const OutboxItem& item) { return item.idempotencyKey == key; });
    return found == operations.cend() ? OutboxItem{} : *found;
  };
  QElapsedTimer operationWait;
  operationWait.start();
  while (operationWait.elapsed() < 5000 &&
         operationByKey(QStringLiteral("future-echo")).state != OutboxState::Done) {
    QTest::qWait(20);
  }
  const OutboxItem echoed = operationByKey(QStringLiteral("future-echo"));
  QVERIFY2(
      echoed.state == OutboxState::Done,
      qPrintable(
          QStringLiteral("state=%1 error=%2: %3 sync=%4 puts=%5")
              .arg(outboxStateToString(echoed.state), echoed.errorCode,
                   echoed.errorMessage,
                   sync.status(accountId).value(QStringLiteral("state")).toString())
              .arg(server.putCount())));
  QCOMPARE(server.putCount(), 1);
  QCOMPARE(server.probePutCount(), 2);
  QCOMPARE(server.probeDeleteCount(), 2);
  QVERIFY(!server.probeResourceExists());
  QVERIFY(server.lastPut().contains("RANGE=THISANDFUTURE"));
  const Event canonical = database.event(future.id, &error);
  QVERIFY(canonical.recurrenceId.contains(QStringLiteral("RANGE=THISANDFUTURE")));
  QVERIFY(canonical.rawPayload.contains(QStringLiteral("RANGE=THISANDFUTURE")));
  QVERIFY(!canonical.dirty);
  QTRY_COMPARE_WITH_TIMEOUT(sync.status(accountId).value(QStringLiteral("state")),
                            QJsonValue(QStringLiteral("idle")), 5000);

  Calendar unproven = calendar;
  unproven.id = QStringLiteral("unproven-calendar");
  unproven.remoteId = QStringLiteral("/unproven/");
  unproven.href = unproven.remoteId;
  unproven.name = QStringLiteral("Unproven range calendar");
  // Keep the synthetic collection out of pull traversal; the account-level
  // outbox drain still exercises the provider's persisted-capability guard.
  unproven.enabled = false;
  // A legacy/stale true flag is insufficient without the explicit proof marker.
  unproven.capabilities.insert(QStringLiteral("thisAndFuture"), true);
  unproven.capabilities.insert(QStringLiteral("thisAndFutureProven"), false);
  QVERIFY2(database.upsertCalendar(unproven, &error), qPrintable(error));
  Event unprovenMaster = master;
  unprovenMaster.id = QStringLiteral("unproven-master");
  unprovenMaster.calendarId = unproven.id;
  unprovenMaster.remoteId = QStringLiteral("/unproven/series.ics");
  unprovenMaster.uid = QStringLiteral("unproven-series@example.test");
  unprovenMaster.recurrenceId.clear();
  unprovenMaster.dirty = false;
  unprovenMaster.localRevision = 0;
  QVERIFY2(database.applyRemoteEvent(unprovenMaster, &error), qPrintable(error));
  Event rejected = unprovenMaster;
  rejected.id = QStringLiteral("unproven-future");
  rejected.remoteId += QStringLiteral("#20260918T090000Z");
  rejected.recurrenceRule.clear();
  rejected.recurrenceId = QStringLiteral("20260918T090000Z");
  rejected.createdAt = {};
  rejected.localRevision = 0;
  QVERIFY2(database.saveLocalEvent(&rejected, OutboxOperation::Update, &error,
                                   QStringLiteral("future-unproven"),
                                   QStringLiteral("future"), QStringLiteral("none"), 0),
           qPrintable(error));
  const int putsBeforeRejection = server.putCount();
  sync.syncAccount(accountId);
  operationWait.restart();
  while (operationWait.elapsed() < 5000 &&
         operationByKey(QStringLiteral("future-unproven")).state !=
             OutboxState::Blocked) {
    QTest::qWait(20);
  }
  const OutboxItem blocked = operationByKey(QStringLiteral("future-unproven"));
  const QJsonObject finalStatus = sync.status(accountId);
  QVERIFY2(
      blocked.state == OutboxState::Blocked,
      qPrintable(QStringLiteral("state=%1 error=%2: %3 sync=%4/%5: %6 puts=%7")
                     .arg(outboxStateToString(blocked.state), blocked.errorCode,
                          blocked.errorMessage,
                          finalStatus.value(QStringLiteral("state")).toString(),
                          finalStatus.value(QStringLiteral("errorCode")).toString(),
                          finalStatus.value(QStringLiteral("message")).toString())
                     .arg(server.putCount())));
  QCOMPARE(blocked.errorCode, QStringLiteral("recurrence_scope_unsupported"));
  QCOMPARE(server.putCount(), putsBeforeRejection);
  qputenv("PATH", originalPath);
}

void CalDavHardeningTest::futureRangeProbeRejectsNonRetention() {
  QTemporaryDir helperDirectory;
  QByteArray originalPath;
  QVERIFY(installFastSecretTool(&helperDirectory, &originalPath));
  CalDavRangeFixture server(false);
  QVERIFY(server.listen());
  Database database;
  QString error;
  QVERIFY2(database.open(QStringLiteral(":memory:"), &error), qPrintable(error));
  caldav::CalDavSync sync(&database);
  const QString accountId = setupRangeAccount(&server, &database, &sync, &error);
  QVERIFY2(!accountId.isEmpty(), qPrintable(error));
  const OutboxItem result = queueFutureMutation(&database, &sync, accountId, &error);
  QVERIFY2(result.state == OutboxState::Blocked, qPrintable(error));
  QCOMPARE(result.errorCode, QStringLiteral("recurrence_range_not_retained"));
  QCOMPARE(server.probePutCount(), 2);
  QCOMPARE(server.probeDeleteCount(), 2);
  QVERIFY(!server.probeResourceExists());
  const Calendar calendar = database.calendars(accountId).first();
  QVERIFY(!calendar.capabilities.value(QStringLiteral("thisAndFuture")).toBool());
  QVERIFY(!calendar.capabilities.value(QStringLiteral("thisAndFutureProven")).toBool());
  qputenv("PATH", originalPath);
}

void CalDavHardeningTest::futureRangeProbeRequiresCleanup() {
  QTemporaryDir helperDirectory;
  QByteArray originalPath;
  QVERIFY(installFastSecretTool(&helperDirectory, &originalPath));
  CalDavRangeFixture server(true, false);
  QVERIFY(server.listen());
  Database database;
  QString error;
  QVERIFY2(database.open(QStringLiteral(":memory:"), &error), qPrintable(error));
  caldav::CalDavSync sync(&database);
  const QString accountId = setupRangeAccount(&server, &database, &sync, &error);
  QVERIFY2(!accountId.isEmpty(), qPrintable(error));
  const OutboxItem result = queueFutureMutation(&database, &sync, accountId, &error);
  QVERIFY2(result.state == OutboxState::Blocked, qPrintable(error));
  QCOMPARE(result.errorCode,
           QStringLiteral("recurrence_capability_probe_cleanup_failed"));
  QCOMPARE(server.probePutCount(), 0);
  QVERIFY(server.probeDeleteCount() >= 1);
  QVERIFY(!server.probeResourceExists());
  const Calendar calendar = database.calendars(accountId).first();
  QVERIFY(!calendar.capabilities.value(QStringLiteral("thisAndFuture")).toBool());
  QVERIFY(!calendar.capabilities.value(QStringLiteral("thisAndFutureProven")).toBool());
  qputenv("PATH", originalPath);
}

void CalDavHardeningTest::futureRangeProbeRemovesDeterministicStaleResource() {
  QTemporaryDir helperDirectory;
  QByteArray originalPath;
  QVERIFY(installFastSecretTool(&helperDirectory, &originalPath));
  CalDavRangeFixture server(true, true, true);
  QVERIFY(server.listen());
  Database database;
  QString error;
  QVERIFY2(database.open(QStringLiteral(":memory:"), &error), qPrintable(error));
  caldav::CalDavSync sync(&database);
  const QString accountId = setupRangeAccount(&server, &database, &sync, &error);
  QVERIFY2(!accountId.isEmpty(), qPrintable(error));
  const OutboxItem result = queueFutureMutation(&database, &sync, accountId, &error);
  QVERIFY2(result.state == OutboxState::Done, qPrintable(error));
  QVERIFY(server.staleProbeWasSeeded());
  QCOMPARE(server.probePutCount(), 2);
  QCOMPARE(server.probeDeleteCount(), 2);
  QVERIFY(!server.probeResourceExists());
  qputenv("PATH", originalPath);
}

void CalDavHardeningTest::futureRangeProbeCleansUpLostCreateAcknowledgment() {
  QTemporaryDir helperDirectory;
  QByteArray originalPath;
  QVERIFY(installFastSecretTool(&helperDirectory, &originalPath));
  CalDavRangeFixture server(true, true, false, true);
  QVERIFY(server.listen());
  Database database;
  QString error;
  QVERIFY2(database.open(QStringLiteral(":memory:"), &error), qPrintable(error));
  caldav::CalDavSync sync(&database);
  const QString accountId = setupRangeAccount(&server, &database, &sync, &error);
  QVERIFY2(!accountId.isEmpty(), qPrintable(error));
  const OutboxItem result = queueFutureMutation(&database, &sync, accountId, &error);
  QVERIFY2(result.state == OutboxState::Blocked, qPrintable(error));
  QCOMPARE(result.errorCode, QStringLiteral("http_500"));
  QCOMPARE(server.putCount(), 0);
  QCOMPARE(server.probePutCount(), 1);
  QCOMPARE(server.probeDeleteCount(), 2);
  QVERIFY(!server.probeResourceExists());
  qputenv("PATH", originalPath);
}

QTEST_GUILESS_MAIN(CalDavHardeningTest)
#include "test_caldav.moc"
