#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest/QtTest>

#include "core/database.h"
#include "providers/caldav/caldavclient.h"
#include "providers/caldav/caldavtasksync.h"
#include "providers/caldav/vtodocodec.h"

using namespace omacalendar;
using namespace omacalendar::caldav;

namespace {

QByteArray vtodo(const QByteArray& uid, const QByteArray& summary,
                 const QByteArray& extra = {}) {
  return QByteArrayLiteral(
             "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Test//EN\r\n"
             "BEGIN:VTODO\r\nUID:") +
         uid + QByteArrayLiteral("\r\nDTSTAMP:20260901T000000Z\r\nSUMMARY:") + summary +
         QByteArrayLiteral("\r\n") + extra +
         QByteArrayLiteral("END:VTODO\r\nEND:VCALENDAR\r\n");
}

QByteArray xmlEscape(const QByteArray& value) {
  return QString::fromUtf8(value).toHtmlEscaped().toUtf8();
}

// A CalDAV server with one VTODO collection at /dav/tasks/. PUT and DELETE
// honour If-Match and If-None-Match; REPORT answers both calendar-query and
// calendar-multiget.
class TaskServer final : public QObject {
 public:
  struct Resource {
    QByteArray etag;
    QByteArray data;
  };

  TaskServer() {
    connect(&m_server, &QTcpServer::newConnection, this, [this]() {
      while (QTcpSocket* socket = m_server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
          QByteArray& request = m_buffers[socket];
          request += socket->readAll();
          const qsizetype headerEnd = request.indexOf("\r\n\r\n");
          if (headerEnd < 0) {
            return;
          }
          const QByteArray lower = request.left(headerEnd).toLower();
          qsizetype length = 0;
          const qsizetype start = lower.indexOf("content-length:");
          if (start >= 0) {
            const qsizetype valueStart = start + 15;
            length =
                lower.mid(valueStart, lower.indexOf("\r\n", valueStart) - valueStart)
                    .trimmed()
                    .toLongLong();
          }
          if (request.size() < headerEnd + 4 + length) {
            return;
          }
          socket->write(respond(request.left(headerEnd), request.mid(headerEnd + 4)));
          m_buffers.remove(socket);
          socket->disconnectFromHost();
        });
      }
    });
  }

  bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }

  [[nodiscard]] QUrl url(const QString& path) const {
    QUrl result;
    result.setScheme(QStringLiteral("http"));
    result.setHost(QStringLiteral("127.0.0.1"));
    result.setPort(m_server.serverPort());
    result.setPath(path);
    return result;
  }

  void put(const QByteArray& path, const QByteArray& data) {
    m_resources.insert(path,
                       {QByteArrayLiteral("\"e") + QByteArray::number(++m_version) +
                            QByteArrayLiteral("\""),
                        data});
  }
  void remove(const QByteArray& path) { m_resources.remove(path); }
  // Reports this resource as a 500 inside calendar-query answers.
  void failInQuery(const QByteArray& path) { m_failingPath = path; }
  [[nodiscard]] QHash<QByteArray, Resource> resources() const { return m_resources; }
  [[nodiscard]] QStringList log() const { return m_log; }

 private:
  static QByteArray header(const QByteArray& headers, const QByteArray& name) {
    for (const QByteArray& line : headers.split('\n')) {
      const qsizetype colon = line.indexOf(':');
      if (colon > 0 && line.left(colon).trimmed().toLower() == name) {
        return line.mid(colon + 1).trimmed();
      }
    }
    return {};
  }

  static QByteArray reply(const QByteArray& status, const QByteArray& body = {},
                          const QByteArray& etag = {}) {
    QByteArray result =
        QByteArrayLiteral("HTTP/1.1 ") + status + QByteArrayLiteral("\r\n");
    if (!etag.isEmpty()) {
      result += QByteArrayLiteral("ETag: ") + etag + QByteArrayLiteral("\r\n");
    }
    if (!body.isEmpty()) {
      result += QByteArrayLiteral("Content-Type: application/xml; charset=utf-8\r\n");
    }
    return result + QByteArrayLiteral("Content-Length: ") +
           QByteArray::number(body.size()) +
           QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body;
  }

  QByteArray entry(const QByteArray& path) const {
    if (!m_resources.contains(path)) {
      return QByteArrayLiteral("<d:response><d:href>") + path +
             QByteArrayLiteral(
                 "</d:href><d:status>HTTP/1.1 404 Not Found</d:status>"
                 "</d:response>");
    }
    const Resource& resource = m_resources.value(path);
    return QByteArrayLiteral("<d:response><d:href>") + path +
           QByteArrayLiteral("</d:href><d:propstat><d:prop><d:getetag>") +
           xmlEscape(resource.etag) +
           QByteArrayLiteral("</d:getetag><c:calendar-data>") +
           xmlEscape(resource.data) +
           QByteArrayLiteral(
               "</c:calendar-data></d:prop><d:status>HTTP/1.1 200 OK"
               "</d:status></d:propstat></d:response>");
  }

  QByteArray respond(const QByteArray& headers, const QByteArray& body) {
    const QList<QByteArray> requestLine =
        headers.left(headers.indexOf("\r\n")).split(' ');
    const QByteArray method = requestLine.value(0);
    const QByteArray path = QUrl::fromPercentEncoding(requestLine.value(1)).toUtf8();
    m_log.append(QString::fromLatin1(method + ' ' + path));
    const QByteArray ifMatch = header(headers, "if-match");
    const QByteArray ifNoneMatch = header(headers, "if-none-match");
    if (method == "REPORT") {
      QByteArray entries;
      if (body.contains("calendar-multiget")) {
        qsizetype from = 0;
        while ((from = body.indexOf("href>", from)) >= 0) {
          from += 5;
          const qsizetype end = body.indexOf('<', from);
          const QByteArray href =
              QUrl::fromPercentEncoding(body.mid(from, end - from)).toUtf8();
          if (!href.isEmpty() && href.startsWith('/')) {
            entries += entry(href);
          }
          from = end;
        }
      } else {
        for (auto it = m_resources.cbegin(); it != m_resources.cend(); ++it) {
          entries += it.key() == m_failingPath
                         ? QByteArrayLiteral("<d:response><d:href>") + it.key() +
                               QByteArrayLiteral(
                                   "</d:href><d:status>HTTP/1.1 500 "
                                   "Internal Server Error</d:status>"
                                   "</d:response>")
                         : entry(it.key());
        }
      }
      return reply(
          "207 Multi-Status",
          QByteArrayLiteral("<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" "
                            "xmlns:c=\"urn:ietf:params:xml:ns:caldav\">") +
              entries + QByteArrayLiteral("</d:multistatus>"));
    }
    const bool exists = m_resources.contains(path);
    if (method == "PUT") {
      if ((ifNoneMatch == "*" && exists) ||
          (!ifMatch.isEmpty() &&
           (!exists || m_resources.value(path).etag != ifMatch))) {
        return reply("412 Precondition Failed");
      }
      put(path, body);
      return reply(exists ? "204 No Content" : "201 Created", {},
                   m_resources.value(path).etag);
    }
    if (method == "DELETE") {
      if (!exists) {
        return reply("404 Not Found");
      }
      if (!ifMatch.isEmpty() && m_resources.value(path).etag != ifMatch) {
        return reply("412 Precondition Failed");
      }
      m_resources.remove(path);
      return reply("204 No Content");
    }
    return reply("405 Method Not Allowed");
  }

  QTcpServer m_server;
  QHash<QTcpSocket*, QByteArray> m_buffers;
  QHash<QByteArray, Resource> m_resources;
  QStringList m_log;
  QByteArray m_failingPath;
  int m_version = 0;
};

Task onlyTask(Database& database, const QString& listId, const QString& title) {
  TaskQuery query;
  query.listIds = {listId};
  for (const Task& task : database.tasks(query)) {
    if (task.title == title) {
      return task;
    }
  }
  return {};
}

}  // namespace

class CalDavTaskTest final : public QObject {
  Q_OBJECT

 private slots:
  void discoveryReadsSupportedComponents();
  void codecReadsDueDatesAndCompletion();
  void codecPatchKeepsProviderProperties();
  void syncPushesPullsAndMergesConflicts();
};

void CalDavTaskTest::discoveryReadsSupportedComponents() {
  const QByteArray xml = QByteArrayLiteral(
      "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" "
      "xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
      "<d:response><d:href>/dav/both/</d:href><d:propstat><d:prop>"
      "<d:resourcetype><d:collection/><c:calendar/></d:resourcetype>"
      "<c:supported-calendar-component-set><c:comp name=\"VEVENT\"/>"
      "<c:comp name=\"vtodo\"/></c:supported-calendar-component-set>"
      "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
      "<d:response><d:href>/dav/todo/</d:href><d:propstat><d:prop>"
      "<d:resourcetype><d:collection/><c:calendar/></d:resourcetype>"
      "<c:supported-calendar-component-set><c:comp name=\"VTODO\"/>"
      "</c:supported-calendar-component-set>"
      "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
      "<d:response><d:href>/dav/plain/</d:href><d:propstat><d:prop>"
      "<d:resourcetype><d:collection/><c:calendar/></d:resourcetype>"
      "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
      "</d:multistatus>");
  const QList<CalDavCollection> collections =
      CalDavXml::collections(CalDavXml::parseMultiStatus(xml));
  QCOMPARE(collections.size(), 3);
  QVERIFY(collections.at(0).holdsEvents() && collections.at(0).holdsTasks());
  QVERIFY(!collections.at(1).holdsEvents() && collections.at(1).holdsTasks());
  // Without the property a collection is a calendar, not a task list.
  QVERIFY(collections.at(2).holdsEvents() && !collections.at(2).holdsTasks());
}

void CalDavTaskTest::codecReadsDueDatesAndCompletion() {
  const std::optional<Task> dated =
      VTodoCodec::parse(vtodo("a", "Pay rent",
                              "DUE;VALUE=DATE:20261005\r\nPRIORITY:1\r\n"
                              "DESCRIPTION:Before noon\r\n"));
  QVERIFY(dated.has_value());
  QCOMPARE(dated->uid, QStringLiteral("a"));
  QCOMPARE(dated->title, QStringLiteral("Pay rent"));
  QCOMPARE(dated->notes, QStringLiteral("Before noon"));
  QCOMPARE(dated->dueDate, QDate(2026, 10, 5));
  QVERIFY(!dated->dueUtc.isValid());
  QCOMPARE(dated->priority, 1);
  QVERIFY(!dated->completed);

  const std::optional<Task> timed =
      VTodoCodec::parse(vtodo("b", "Call",
                              "DUE;TZID=Europe/Berlin:20261005T090000\r\n"
                              "STATUS:COMPLETED\r\nCOMPLETED:20261004T120000Z\r\n"
                              "RELATED-TO;RELTYPE=PARENT:a\r\n"));
  QVERIFY(timed.has_value());
  QCOMPARE(timed->dueDate, QDate(2026, 10, 5));
  QCOMPARE(timed->dueUtc, QDateTime(QDate(2026, 10, 5), QTime(7, 0), QTimeZone::UTC));
  QVERIFY(timed->completed);
  QCOMPARE(timed->completedAt,
           QDateTime(QDate(2026, 10, 4), QTime(12, 0), QTimeZone::UTC));
  QCOMPARE(timed->parentId, QStringLiteral("a"));

  QVERIFY(VTodoCodec::parse(vtodo("c", "x", "PERCENT-COMPLETE:100\r\n"))->completed);
  QString error;
  QVERIFY(
      !VTodoCodec::parse("BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n", &error).has_value());
  QVERIFY(!error.isEmpty());
}

void CalDavTaskTest::codecPatchKeepsProviderProperties() {
  const QByteArray original = vtodo("keep", "Original",
                                    "CATEGORIES:home\r\nX-CUSTOM:value\r\n"
                                    "DUE;VALUE=DATE:20261005\r\n");
  Task task = *VTodoCodec::parse(original);
  task.title = QStringLiteral("Renamed");
  task.completed = true;
  const QByteArray patched = VTodoCodec::serialize(task, original);
  QVERIFY(patched.contains("SUMMARY:Renamed"));
  QVERIFY(patched.contains("STATUS:COMPLETED"));
  QVERIFY(patched.contains("PERCENT-COMPLETE:100"));
  QVERIFY(patched.contains("CATEGORIES:home"));
  QVERIFY(patched.contains("X-CUSTOM:value"));
  QVERIFY(patched.contains("DUE;VALUE=DATE:20261005"));

  task.completed = false;
  task.dueDate = {};
  const QByteArray reopened = VTodoCodec::serialize(task, patched);
  QVERIFY(reopened.contains("STATUS:NEEDS-ACTION"));
  QVERIFY(!reopened.contains("COMPLETED:"));
  QVERIFY(!reopened.contains("DUE"));
  const std::optional<Task> readBack = VTodoCodec::parse(reopened);
  QVERIFY(!readBack->completed);

  Task fresh;
  fresh.uid = QStringLiteral("new-uid");
  fresh.title = QStringLiteral("Brand new");
  fresh.dueDate = QDate(2026, 11, 1);
  const std::optional<Task> created = VTodoCodec::parse(VTodoCodec::serialize(fresh));
  QVERIFY(created.has_value());
  QCOMPARE(created->uid, QStringLiteral("new-uid"));
  QCOMPARE(created->dueDate, QDate(2026, 11, 1));
}

void CalDavTaskTest::syncPushesPullsAndMergesConflicts() {
  TaskServer server;
  QVERIFY(server.listen());
  server.put("/dav/tasks/existing.ics",
             vtodo("existing", "Existing", "X-CUSTOM:value\r\n"));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  Database database;
  QString error;
  QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
           qPrintable(error));
  Account account;
  account.id = QStringLiteral("dav");
  account.provider = ProviderKind::CalDav;
  account.displayName = QStringLiteral("DAV");
  QVERIFY(database.upsertAccount(account, &error));

  CalDavClient client;
  client.setCredentials(account.id, QStringLiteral("alice"), QStringLiteral("secret"),
                        server.url(QStringLiteral("/dav/")));
  CalDavTaskSync sync(&database, &client);
  QSignalSpy changed(&sync, &CalDavTaskSync::tasksChanged);
  QSignalSpy failed(&sync, &CalDavTaskSync::listFailed);
  const auto settle = [&sync, &account]() {
    QTRY_VERIFY_WITH_TIMEOUT(!sync.isSyncing(account.id), 10000);
  };

  CalDavCollection tasks;
  tasks.href = QStringLiteral("/dav/tasks/");
  tasks.displayName = QStringLiteral("Chores");
  tasks.ctag = QStringLiteral("1");
  tasks.supportedComponents = {QStringLiteral("VTODO")};
  CalDavCollection events;
  events.href = QStringLiteral("/dav/calendar/");
  events.supportedComponents = {QStringLiteral("VEVENT")};
  sync.syncDiscovered(account.id, server.url(QStringLiteral("/dav/")), {tasks, events},
                      true);
  settle();
  QCOMPARE(failed.count(), 0);
  TaskList list;
  for (const TaskList& candidate : database.taskLists()) {
    if (candidate.accountId == account.id) {
      QVERIFY2(list.id.isEmpty(), "only the VTODO collection becomes a task list");
      list = candidate;
    }
  }
  QCOMPARE(list.name, QStringLiteral("Chores"));
  QCOMPARE(list.capabilities.value(QStringLiteral("provider")).toString(),
           QStringLiteral("caldav"));
  QVERIFY(!onlyTask(database, list.id, QStringLiteral("Existing")).id.isEmpty());
  QVERIFY(changed.count() >= 1);

  // A local create is uploaded with If-None-Match and becomes clean.
  Task milk;
  milk.listId = list.id;
  milk.title = QStringLiteral("Buy milk");
  QVERIFY2(database.saveLocalTask(&milk, -1, &error), qPrintable(error));
  sync.syncStored(account.id);
  settle();
  QCOMPARE(failed.count(), 0);
  milk = database.task(milk.id);
  QVERIFY(!milk.dirty);
  QVERIFY(milk.pendingOperation.isEmpty());
  QVERIFY(milk.remoteId.endsWith(milk.uid + QStringLiteral(".ics")));
  QVERIFY(server.resources()
              .value(QUrl(milk.remoteId).path().toUtf8())
              .data.contains("SUMMARY:Buy milk"));

  // The server renames the task while it is completed locally: both survive.
  Task existing = onlyTask(database, list.id, QStringLiteral("Existing"));
  existing.completed = true;
  QVERIFY2(database.saveLocalTask(&existing, -1, &error), qPrintable(error));
  server.put("/dav/tasks/existing.ics",
             vtodo("existing", "Existing (renamed)", "X-CUSTOM:value\r\n"));
  sync.syncStored(account.id);
  settle();
  QCOMPARE(failed.count(), 0);
  const QByteArray merged = server.resources().value("/dav/tasks/existing.ics").data;
  QVERIFY2(merged.contains("SUMMARY:Existing (renamed)"), merged.constData());
  QVERIFY(merged.contains("STATUS:COMPLETED"));
  QVERIFY(merged.contains("X-CUSTOM:value"));
  const Task mergedLocal =
      onlyTask(database, list.id, QStringLiteral("Existing (renamed)"));
  QVERIFY(mergedLocal.completed);
  QVERIFY(!mergedLocal.dirty);

  // A local removal is deleted on the server.
  QVERIFY2(database.removeLocalTask(milk.id, &error), qPrintable(error));
  sync.syncStored(account.id);
  settle();
  QVERIFY(!server.resources().contains(QUrl(milk.remoteId).path().toUtf8()));
  QVERIFY(database.task(milk.id).id.isEmpty());

  // A task removed on the server goes locally too.
  server.remove("/dav/tasks/existing.ics");
  sync.syncStored(account.id);
  settle();
  TaskQuery query;
  query.listIds = {list.id};
  QVERIFY(database.tasks(query).isEmpty());

  // A task removed while its upload is under way does not come back: the
  // upload lands, then the removal follows.
  Task quick;
  quick.listId = list.id;
  quick.title = QStringLiteral("Changed my mind");
  QVERIFY2(database.saveLocalTask(&quick, -1, &error), qPrintable(error));
  sync.syncStored(account.id);
  QVERIFY2(database.removeLocalTask(quick.id, &error), qPrintable(error));
  sync.syncStored(account.id);
  settle();
  QTRY_VERIFY_WITH_TIMEOUT(!sync.isSyncing(account.id), 10000);
  QVERIFY(database.pendingTaskWrites(list.id).isEmpty());
  for (auto it = server.resources().cbegin(); it != server.resources().cend(); ++it) {
    QVERIFY2(!it.value().data.contains("Changed my mind"), it.key().constData());
  }
  QVERIFY(onlyTask(database, list.id, QStringLiteral("Changed my mind")).id.isEmpty());

  // A resource the server fails to report is not taken as removed.
  server.put("/dav/tasks/kept.ics", vtodo("kept", "Kept"));
  sync.syncStored(account.id);
  settle();
  QVERIFY(!onlyTask(database, list.id, QStringLiteral("Kept")).id.isEmpty());
  server.failInQuery("/dav/tasks/kept.ics");
  sync.syncStored(account.id);
  settle();
  QVERIFY(!onlyTask(database, list.id, QStringLiteral("Kept")).id.isEmpty());
  server.failInQuery({});

  // A collection whose component list could not be read keeps its list, and
  // so does one missing from a discovery with failed responses.
  CalDavCollection unknown = tasks;
  unknown.supportedComponents.clear();
  sync.syncDiscovered(account.id, server.url(QStringLiteral("/dav/")),
                      {unknown, events}, true);
  settle();
  QVERIFY(!database.taskList(list.id).id.isEmpty());
  sync.syncDiscovered(account.id, server.url(QStringLiteral("/dav/")), {events}, false);
  settle();
  QVERIFY(!database.taskList(list.id).id.isEmpty());

  // A collection that is no longer discovered takes its list with it.
  sync.syncDiscovered(account.id, server.url(QStringLiteral("/dav/")), {events}, true);
  settle();
  QVERIFY(database.taskList(list.id).id.isEmpty());
  QCOMPARE(failed.count(), 0);
}

QTEST_GUILESS_MAIN(CalDavTaskTest)
#include "test_caldavtasks.moc"
