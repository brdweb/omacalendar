// Direct coverage of Google Tasks sync: the task mapping, the owed-write
// replay contract GoogleTaskSync drives against the database, deletions, and
// the sync driver's own failure and rerun handling. The Google client has no
// injectable transport, so the HTTP exchange itself is simulated by feeding
// the mapper the bodies Google returns. Every address and id is synthetic.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest/QtTest>
#include <memory>

#include "core/database.h"
#include "providers/google/googleclient.h"
#include "providers/google/googletasksync.h"

using namespace omacalendar;

namespace {

const QString kAccountId = QStringLiteral("acc-google-tasks");

QJsonObject googleTask(const QString& id, const QString& title,
                       const QString& etag = QStringLiteral("\"e1\"")) {
  return QJsonObject{
      {QStringLiteral("kind"), QStringLiteral("tasks#task")},
      {QStringLiteral("id"), id},
      {QStringLiteral("etag"), etag},
      {QStringLiteral("title"), title},
      {QStringLiteral("status"), QStringLiteral("needsAction")},
      {QStringLiteral("updated"), QStringLiteral("2026-10-01T09:00:00.000Z")},
      {QStringLiteral("position"), QStringLiteral("00000000000000000000")},
  };
}

// What GoogleTaskSync::recordWrite stores for an acknowledged write.
bool acknowledge(Database* database, const Task& sent, const QJsonObject& responseBody,
                 QString* error) {
  if (sent.pendingOperation == QStringLiteral("remove")) {
    return database->completeTaskWrite(sent.id, sent.localRevision, sent.remoteId, {},
                                       {}, {}, nullptr, error);
  }
  const Task uploaded = google::taskFromGoogleJson(responseBody);
  return database->completeTaskWrite(
      sent.id, sent.localRevision,
      uploaded.remoteId.isEmpty() ? sent.remoteId : uploaded.remoteId, uploaded.etag,
      uploaded.rawPayload, uploaded.rawFormat, &uploaded, error);
}

QList<Task> remoteSnapshot(const QList<QJsonObject>& resources) {
  QList<Task> result;
  for (const QJsonObject& resource : resources) {
    if (!resource.value(QStringLiteral("deleted")).toBool()) {
      result.append(google::taskFromGoogleJson(resource));
    }
  }
  return result;
}

}  // namespace

class GoogleTasksTest final : public QObject {
  Q_OBJECT

 private:
  QTemporaryDir m_directory;
  std::unique_ptr<Database> m_database;
  TaskList m_list;

  Task saveNew(const QString& title) {
    Task task;
    task.listId = m_list.id;
    task.title = title;
    QString error;
    if (!m_database->saveLocalTask(&task, -1, &error)) {
      qWarning("saveLocalTask failed: %s", qPrintable(error));
    }
    return task;
  }

  Task stored(const QString& title) const {
    TaskQuery query;
    query.listIds = {m_list.id};
    for (const Task& task : m_database->tasks(query)) {
      if (task.title == title) {
        return task;
      }
    }
    return {};
  }

  qsizetype storedCount() const {
    TaskQuery query;
    query.listIds = {m_list.id};
    return m_database->tasks(query).size();
  }

 private slots:
  void init() {
    QVERIFY(m_directory.isValid());
    m_database = std::make_unique<Database>();
    QString error;
    QVERIFY2(m_database->open(
                 m_directory.filePath(
                     QStringLiteral("%1.sqlite")
                         .arg(QString::fromLatin1(QTest::currentTestFunction()))),
                 &error),
             qPrintable(error));
    Account account;
    account.id = kAccountId;
    account.provider = ProviderKind::Google;
    account.displayName = QStringLiteral("Tasks");
    account.principal = QStringLiteral("person@example.invalid");
    account.authStatus = QStringLiteral("connected");
    QVERIFY2(m_database->upsertAccount(account, &error), qPrintable(error));
    m_list = TaskList{};
    m_list.id = QStringLiteral("list-local-id");
    m_list.accountId = kAccountId;
    m_list.remoteId = QStringLiteral("list-remote-id");
    m_list.name = QStringLiteral("My Tasks");
    QVERIFY2(m_database->upsertTaskList(m_list, &error), qPrintable(error));
  }

  void cleanup() { m_database.reset(); }

  void mappingKeepsOnlyTheDueDay() {
    QJsonObject resource = googleTask(QStringLiteral("t1"), QStringLiteral("Water"));
    resource.insert(QStringLiteral("due"), QStringLiteral("2026-10-05T23:30:00.000Z"));
    resource.insert(QStringLiteral("notes"), QStringLiteral("Plants by the window"));
    resource.insert(QStringLiteral("hidden"), true);
    resource.insert(QStringLiteral("webViewLink"),
                    QStringLiteral("https://tasks.example.invalid/t1"));
    const Task task = google::taskFromGoogleJson(resource);
    QCOMPARE(task.remoteId, QStringLiteral("t1"));
    QCOMPARE(task.uid, QStringLiteral("t1"));
    QCOMPARE(task.etag, QStringLiteral("\"e1\""));
    // The day is taken as written, never shifted by a local zone.
    QCOMPARE(task.dueDate, QDate(2026, 10, 5));
    QVERIFY(!task.dueUtc.isValid());
    QVERIFY(!task.completed);
    QVERIFY(!task.completedAt.isValid());
    QCOMPARE(task.updatedAt,
             QDateTime(QDate(2026, 10, 1), QTime(9, 0), QTimeZone::UTC));
    QCOMPARE(task.rawFormat, QString::fromLatin1(google::kGoogleTaskFormat));
    // Fields with no canonical home stay in the raw payload.
    const QJsonObject raw = QJsonDocument::fromJson(task.rawPayload.toUtf8()).object();
    QCOMPARE(raw.value(QStringLiteral("webViewLink")).toString(),
             QStringLiteral("https://tasks.example.invalid/t1"));
    QVERIFY(raw.value(QStringLiteral("hidden")).toBool());

    // Malformed or absent due values are an undated task.
    resource.insert(QStringLiteral("due"), QStringLiteral("soon"));
    QVERIFY(!google::taskFromGoogleJson(resource).dueDate.isValid());
    resource.remove(QStringLiteral("due"));
    QVERIFY(!google::taskFromGoogleJson(resource).dueDate.isValid());
  }

  void writeBodyRoundTrips() {
    Task task;
    task.title = QStringLiteral("File taxes");
    task.notes = QStringLiteral("Receipts in the blue folder");
    task.dueDate = QDate(2027, 4, 15);
    task.completed = true;
    task.completedAt = QDateTime(QDate(2027, 4, 1), QTime(12, 0), QTimeZone::UTC);
    task.parentId = QStringLiteral("parent");
    task.position = QStringLiteral("0001");
    task.priority = 1;
    QJsonObject body = google::taskToGoogleJson(task);
    // Only the fields OmaCalendar edits: moving and nesting use other calls.
    QStringList keys = body.keys();
    keys.sort();
    QCOMPARE(keys, QStringList({QStringLiteral("completed"), QStringLiteral("due"),
                                QStringLiteral("notes"), QStringLiteral("status"),
                                QStringLiteral("title")}));
    QCOMPARE(body.value(QStringLiteral("due")).toString(),
             QStringLiteral("2027-04-15T00:00:00.000Z"));
    QCOMPARE(body.value(QStringLiteral("completed")).toString(),
             QStringLiteral("2027-04-01T12:00:00.000Z"));

    body.insert(QStringLiteral("id"), QStringLiteral("t2"));
    const Task back = google::taskFromGoogleJson(body);
    QCOMPARE(back.title, task.title);
    QCOMPARE(back.notes, task.notes);
    QCOMPARE(back.dueDate, task.dueDate);
    QCOMPARE(back.completed, task.completed);
    QCOMPARE(back.completedAt, task.completedAt);

    // Completed without a known time lets Google stamp it.
    task.completedAt = {};
    body = google::taskToGoogleJson(task);
    QCOMPARE(body.value(QStringLiteral("status")).toString(),
             QStringLiteral("completed"));
    QVERIFY(body.value(QStringLiteral("completed")).isNull());
  }

  void unconfirmedCreatesMatchOnlyIdenticalContent() {
    Task pending;
    pending.id = QStringLiteral("local-a");
    pending.title = QStringLiteral("Pay rent");
    pending.dueDate = QDate(2026, 11, 1);
    pending.pendingOperation = QStringLiteral("create");

    Task completedCopy = pending;
    completedCopy.remoteId = QStringLiteral("g-completed");
    completedCopy.completed = true;
    Task otherDay = pending;
    otherDay.remoteId = QStringLiteral("g-other-day");
    otherDay.dueDate = QDate(2026, 11, 2);
    QList<Task> candidates{completedCopy, otherDay};
    QVERIFY(google::matchUnconfirmedCreates({pending}, &candidates).isEmpty());
    QCOMPARE(candidates.size(), 2);

    // An owed update without a Google id is not an unconfirmed create.
    Task update = pending;
    update.pendingOperation = QStringLiteral("update");
    Task same = pending;
    same.remoteId = QStringLiteral("g-same");
    candidates = {same};
    QVERIFY(google::matchUnconfirmedCreates({update}, &candidates).isEmpty());
    QCOMPARE(google::matchUnconfirmedCreates({pending}, &candidates)
                 .value(QStringLiteral("local-a")),
             QStringLiteral("g-same"));
    QVERIFY(candidates.isEmpty());
  }

  void createThenEditsReplayInOrder() {
    QString error;
    Task created = saveNew(QStringLiteral("Book dentist"));
    QCOMPARE(created.pendingOperation, QStringLiteral("create"));
    QList<Task> owed = m_database->pendingTaskWrites(m_list.id, &error);
    QCOMPARE(owed.size(), 1);
    const Task sentCreate = owed.first();
    QVERIFY(sentCreate.remoteId.isEmpty());

    // Google answers the insert with its own id and etag.
    QJsonObject inserted =
        googleTask(QStringLiteral("g-dentist"), QStringLiteral("Book dentist"),
                   QStringLiteral("\"v1\""));
    QVERIFY2(acknowledge(m_database.get(), sentCreate, inserted, &error),
             qPrintable(error));
    Task clean = m_database->task(created.id, &error);
    QCOMPARE(clean.remoteId, QStringLiteral("g-dentist"));
    QCOMPARE(clean.etag, QStringLiteral("\"v1\""));
    QVERIFY(!clean.dirty);
    QVERIFY(clean.pendingOperation.isEmpty());
    QVERIFY(m_database->pendingTaskWrites(m_list.id, &error).isEmpty());

    // A later edit is owed as an update of that Google task.
    clean.notes = QStringLiteral("Call before noon");
    QVERIFY2(m_database->saveLocalTask(&clean, -1, &error), qPrintable(error));
    owed = m_database->pendingTaskWrites(m_list.id, &error);
    QCOMPARE(owed.size(), 1);
    QCOMPARE(owed.first().pendingOperation, QStringLiteral("update"));
    QCOMPARE(owed.first().remoteId, QStringLiteral("g-dentist"));
    QCOMPARE(google::taskToGoogleJson(owed.first()).value(QStringLiteral("notes")),
             QJsonValue(QStringLiteral("Call before noon")));
    const Task sentUpdate = owed.first();

    // The user edits again while that update is in flight: the response must
    // not clear the newer edit, which stays owed as an update.
    Task newer = m_database->task(created.id, &error);
    newer.title = QStringLiteral("Book dentist for Tuesday");
    QVERIFY2(m_database->saveLocalTask(&newer, -1, &error), qPrintable(error));
    QJsonObject patched = inserted;
    patched.insert(QStringLiteral("etag"), QStringLiteral("\"v2\""));
    patched.insert(QStringLiteral("notes"), QStringLiteral("Call before noon"));
    QVERIFY2(acknowledge(m_database.get(), sentUpdate, patched, &error),
             qPrintable(error));
    const Task afterRace = m_database->task(created.id, &error);
    QCOMPARE(afterRace.title, QStringLiteral("Book dentist for Tuesday"));
    QVERIFY(afterRace.dirty);
    QCOMPARE(afterRace.pendingOperation, QStringLiteral("update"));
    QCOMPARE(afterRace.etag, QStringLiteral("\"v2\""));

    // A full read meanwhile does not overwrite the owed edit.
    bool changed = true;
    QVERIFY2(m_database->applyRemoteTasks(m_list.id, remoteSnapshot({patched}), true,
                                          &changed, &error),
             qPrintable(error));
    QVERIFY(!changed);
    QCOMPARE(m_database->task(created.id, &error).title,
             QStringLiteral("Book dentist for Tuesday"));

    // Replaying it settles the row with Google's merged copy.
    owed = m_database->pendingTaskWrites(m_list.id, &error);
    QCOMPARE(owed.size(), 1);
    QJsonObject merged = patched;
    merged.insert(QStringLiteral("etag"), QStringLiteral("\"v3\""));
    merged.insert(QStringLiteral("title"), QStringLiteral("Book dentist for Tuesday"));
    merged.insert(QStringLiteral("due"), QStringLiteral("2026-10-20T00:00:00.000Z"));
    QVERIFY2(acknowledge(m_database.get(), owed.first(), merged, &error),
             qPrintable(error));
    const Task settled = m_database->task(created.id, &error);
    QVERIFY(!settled.dirty);
    QCOMPARE(settled.etag, QStringLiteral("\"v3\""));
    QCOMPARE(settled.dueDate, QDate(2026, 10, 20));
  }

  void removalsReplayAndTombstonesClear() {
    QString error;
    // Uploaded, then removed locally: a remove is owed with the Google id.
    Task uploaded = saveNew(QStringLiteral("Return library book"));
    QVERIFY2(
        acknowledge(
            m_database.get(), m_database->pendingTaskWrites(m_list.id, &error).first(),
            googleTask(QStringLiteral("g-book"), QStringLiteral("Return library book")),
            &error),
        qPrintable(error));
    QVERIFY2(m_database->removeLocalTask(uploaded.id, &error), qPrintable(error));
    QVERIFY(m_database->task(uploaded.id, &error).id.isEmpty());
    QList<Task> owed = m_database->pendingTaskWrites(m_list.id, &error);
    QCOMPARE(owed.size(), 1);
    QCOMPARE(owed.first().pendingOperation, QStringLiteral("remove"));
    QCOMPARE(owed.first().remoteId, QStringLiteral("g-book"));
    // The tombstone still claims its Google id, so reconciliation never
    // mistakes the Google copy for someone else's task.
    QVERIFY(m_database->taskRemoteIds(m_list.id, &error)
                .contains(QStringLiteral("g-book")));
    // A full read that still lists it does not resurrect it.
    QVERIFY2(m_database->applyRemoteTasks(
                 m_list.id,
                 remoteSnapshot({googleTask(QStringLiteral("g-book"),
                                            QStringLiteral("Return library book"),
                                            QStringLiteral("\"changed\""))}),
                 true, nullptr, &error),
             qPrintable(error));
    QVERIFY(m_database->task(uploaded.id, &error).id.isEmpty());
    QCOMPARE(m_database->pendingTaskWrites(m_list.id, &error).size(), 1);

    // Google's 204 (or 404/410: already gone) clears the tombstone.
    QVERIFY2(acknowledge(m_database.get(), owed.first(), {}, &error),
             qPrintable(error));
    QVERIFY(m_database->pendingTaskWrites(m_list.id, &error).isEmpty());
    QVERIFY(m_database->taskRemoteIds(m_list.id, &error).isEmpty());

    // Created and removed before any upload: nothing goes to Google, and the
    // sync drops the tombstone (sent with an empty Google id).
    Task neverSent = saveNew(QStringLiteral("Typo"));
    QVERIFY2(m_database->removeLocalTask(neverSent.id, &error), qPrintable(error));
    owed = m_database->pendingTaskWrites(m_list.id, &error);
    QCOMPARE(owed.size(), 1);
    QVERIFY(owed.first().remoteId.isEmpty());
    QVERIFY2(acknowledge(m_database.get(), owed.first(), {}, &error),
             qPrintable(error));
    QVERIFY(m_database->pendingTaskWrites(m_list.id, &error).isEmpty());
  }

  void removalDuringCreateStillReachesGoogle() {
    QString error;
    const Task created = saveNew(QStringLiteral("Draft"));
    const Task sentCreate = m_database->pendingTaskWrites(m_list.id, &error).first();
    // Removed while the insert is in flight.
    QVERIFY2(m_database->removeLocalTask(created.id, &error), qPrintable(error));
    QVERIFY2(acknowledge(m_database.get(), sentCreate,
                         googleTask(QStringLiteral("g-draft"), QStringLiteral("Draft")),
                         &error),
             qPrintable(error));
    const QList<Task> owed = m_database->pendingTaskWrites(m_list.id, &error);
    QCOMPARE(owed.size(), 1);
    QCOMPARE(owed.first().pendingOperation, QStringLiteral("remove"));
    QCOMPARE(owed.first().remoteId, QStringLiteral("g-draft"));
  }

  void fullReadAppliesRemoteChangesAndDeletions() {
    QString error;
    bool changed = false;
    QJsonObject keep = googleTask(QStringLiteral("g-keep"), QStringLiteral("Keep"));
    QJsonObject gone = googleTask(QStringLiteral("g-gone"), QStringLiteral("Gone"));
    QJsonObject edited =
        googleTask(QStringLiteral("g-edited"), QStringLiteral("Edit me"));
    QVERIFY2(
        m_database->applyRemoteTasks(m_list.id, remoteSnapshot({keep, gone, edited}),
                                     true, &changed, &error),
        qPrintable(error));
    QVERIFY(changed);
    QCOMPARE(storedCount(), 3);

    // Same etags: nothing moves.
    QVERIFY2(
        m_database->applyRemoteTasks(m_list.id, remoteSnapshot({keep, gone, edited}),
                                     true, &changed, &error),
        qPrintable(error));
    QVERIFY(!changed);

    // A local edit to one task is owed when Google deletes it: it survives.
    Task local = stored(QStringLiteral("Edit me"));
    local.notes = QStringLiteral("Local note");
    QVERIFY2(m_database->saveLocalTask(&local, -1, &error), qPrintable(error));

    // Google deleted "Gone" and "Edit me", changed "Keep", and reports a
    // deleted-flagged item, which is not a task.
    keep.insert(QStringLiteral("etag"), QStringLiteral("\"e2\""));
    keep.insert(QStringLiteral("status"), QStringLiteral("completed"));
    keep.insert(QStringLiteral("completed"),
                QStringLiteral("2026-10-02T10:00:00.000Z"));
    QJsonObject tombstone =
        googleTask(QStringLiteral("g-tomb"), QStringLiteral("Tomb"));
    tombstone.insert(QStringLiteral("deleted"), true);
    QVERIFY2(m_database->applyRemoteTasks(m_list.id, remoteSnapshot({keep, tombstone}),
                                          true, &changed, &error),
             qPrintable(error));
    QVERIFY(changed);
    QVERIFY(stored(QStringLiteral("Gone")).id.isEmpty());
    QVERIFY(stored(QStringLiteral("Tomb")).id.isEmpty());
    QVERIFY(stored(QStringLiteral("Keep")).completed);
    const Task survivor = stored(QStringLiteral("Edit me"));
    QCOMPARE(survivor.id, local.id);
    QCOMPARE(survivor.notes, QStringLiteral("Local note"));
    QCOMPARE(survivor.pendingOperation, QStringLiteral("update"));

    // A partial read never removes anything.
    QVERIFY2(m_database->applyRemoteTasks(m_list.id, {}, false, &changed, &error),
             qPrintable(error));
    QVERIFY(!changed);
    QVERIFY(!stored(QStringLiteral("Keep")).id.isEmpty());
  }

  void lostCreateResponseIsReconciledNotDuplicated() {
    QString error;
    const Task created = saveNew(QStringLiteral("Order printer ink"));
    // The insert reached Google but its response was lost; the next sync
    // reads the list first and finds an unclaimed identical copy, next to a
    // task the store already knows.
    QVERIFY2(m_database->applyRemoteTasks(
                 m_list.id,
                 remoteSnapshot({googleTask(QStringLiteral("g-known"),
                                            QStringLiteral("Order printer ink"))}),
                 false, nullptr, &error),
             qPrintable(error));
    const QSet<QString> known = m_database->taskRemoteIds(m_list.id, &error);
    QList<Task> candidates;
    for (const Task& remote :
         remoteSnapshot({googleTask(QStringLiteral("g-known"),
                                    QStringLiteral("Order printer ink")),
                         googleTask(QStringLiteral("g-landed"),
                                    QStringLiteral("Order printer ink"))})) {
      if (!known.contains(remote.remoteId)) {
        candidates.append(remote);
      }
    }
    QList<Task> owed = m_database->pendingTaskWrites(m_list.id, &error);
    const QHash<QString, QString> matched =
        google::matchUnconfirmedCreates(owed, &candidates);
    QCOMPARE(matched.value(created.id), QStringLiteral("g-landed"));

    // The sync then patches that copy instead of inserting another.
    Task write = owed.first();
    write.remoteId = matched.value(created.id);
    write.pendingOperation = QStringLiteral("update");
    QVERIFY2(acknowledge(m_database.get(), write,
                         googleTask(QStringLiteral("g-landed"),
                                    QStringLiteral("Order printer ink")),
                         &error),
             qPrintable(error));
    QVERIFY2(m_database->applyRemoteTasks(
                 m_list.id,
                 remoteSnapshot({googleTask(QStringLiteral("g-known"),
                                            QStringLiteral("Order printer ink")),
                                 googleTask(QStringLiteral("g-landed"),
                                            QStringLiteral("Order printer ink"))}),
                 true, nullptr, &error),
             qPrintable(error));
    QCOMPARE(storedCount(), 2);
    QCOMPARE(m_database->task(created.id, &error).remoteId, QStringLiteral("g-landed"));
  }

  void syncWithoutAuthorizationFailsWithoutTouchingTheStore() {
    const Task owed = saveNew(QStringLiteral("Still owed"));
    google::GoogleClient client(nullptr);
    google::GoogleTaskSync sync(m_database.get(), &client);
    QSignalSpy failed(&sync, &google::GoogleTaskSync::syncFailed);
    QSignalSpy changed(&sync, &google::GoogleTaskSync::tasksChanged);

    sync.syncAccount(kAccountId);
    QVERIFY(sync.isSyncing(kAccountId));
    QTRY_COMPARE(failed.size(), 1);
    QCOMPARE(failed.first().at(0).toString(), kAccountId);
    QVERIFY(failed.first().at(1).toString().isEmpty());
    QCOMPARE(failed.first().at(2).toString(),
             QStringLiteral("authentication_required"));
    QVERIFY(!sync.isSyncing(kAccountId));
    QVERIFY(changed.isEmpty());
    // Nothing was dropped: the list and the owed write remain.
    QCOMPARE(m_database->taskList(m_list.id).name, m_list.name);
    QCOMPARE(m_database->pendingTaskWrites(m_list.id).size(), 1);
    QCOMPARE(m_database->pendingTaskWrites(m_list.id).first().id, owed.id);

    // A request during a running sync queues exactly one rerun.
    failed.clear();
    sync.syncAccount(kAccountId);
    sync.syncAccount(kAccountId);
    sync.syncAccount(kAccountId);
    QTRY_COMPARE(failed.size(), 2);
    QTRY_VERIFY(!sync.isSyncing(kAccountId));
    QTest::qWait(20);
    QCOMPARE(failed.size(), 2);

    // A cancelled sync reports nothing.
    failed.clear();
    sync.syncAccount(kAccountId);
    sync.cancel(kAccountId);
    QVERIFY(!sync.isSyncing(kAccountId));
    QTest::qWait(20);
    QVERIFY(failed.isEmpty());
  }
};

QTEST_GUILESS_MAIN(GoogleTasksTest)

#include "test_googletasks.moc"
