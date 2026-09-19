// Copyright (c) 2026

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest/QtTest>

#include "core/database.h"
#include "sync/localprovider.h"

using namespace omacalendar;

namespace {

QDateTime utcDateTime(const int year, const int month, const int day) {
  return QDateTime(QDate(year, month, day), QTime(9, 0), QTimeZone::UTC);
}

}  // namespace

class LocalProviderTest final : public QObject {
  Q_OBJECT

 private slots:
  void capabilitiesExposeLocalSurface();
  void statusReportsIdle();
  void drainCompletesQueuedMutations();
  void drainEmitsErrorStatusWithoutDatabase();
  void drainFiltersByAccount();

 private:
  static Event sampleEvent(const QString& summary, const QDateTime& start);
};

Event LocalProviderTest::sampleEvent(const QString& summary, const QDateTime& start) {
  Event event;
  event.calendarId = QStringLiteral("local-default");
  event.summary = summary;
  event.startUtc = start.toUTC();
  event.endUtc = event.startUtc.addSecs(3600);
  event.startDate = event.startUtc.date();
  event.endDate = event.endUtc.date();
  event.startTimeZone = QStringLiteral("UTC");
  event.endTimeZone = QStringLiteral("UTC");
  return event;
}

void LocalProviderTest::capabilitiesExposeLocalSurface() {
  Database database;
  LocalProvider provider(&database);

  const ProviderCapabilities capabilities = provider.capabilities();
  QVERIFY(capabilities.createEvent);
  QVERIFY(capabilities.updateEvent);
  QVERIFY(capabilities.removeEvent);
  QVERIFY(capabilities.fetchEvent);
  QVERIFY(capabilities.attendees);
  QVERIFY(capabilities.reminders);
  QVERIFY(!capabilities.accountDiscovery);
  QVERIFY(!capabilities.calendarDiscovery);
  QVERIFY(!capabilities.incrementalSync);
}

void LocalProviderTest::statusReportsIdle() {
  Database database;
  LocalProvider provider(&database);

  const QJsonObject status = provider.status(QStringLiteral("local-account"));
  QCOMPARE(status.value(QStringLiteral("accountId")).toString(),
           QStringLiteral("local-account"));
  QCOMPARE(status.value(QStringLiteral("provider")).toString(),
           QStringLiteral("local"));
  QCOMPARE(status.value(QStringLiteral("state")).toString(), QStringLiteral("idle"));
}

void LocalProviderTest::drainCompletesQueuedMutations() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  Database database;
  QString error;
  QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
           qPrintable(error));

  LocalProvider provider(&database);
  QSignalSpy eventsSpy(&provider, &LocalProvider::eventsChanged);
  QSignalSpy operationsSpy(&provider, &LocalProvider::operationStateChanged);

  // Local creates and updates are final immediately and never enter the
  // outbox (database.cpp skips queuing for Local saves). A local remove is
  // the durable mutation the provider acknowledges. A remove chained to a
  // dependency mutation skips the tombstone undo window and queues ready.
  Event event = sampleEvent(QStringLiteral("Local meetup"), utcDateTime(2027, 5, 6));
  QVERIFY2(database.saveLocalEvent(&event, OutboxOperation::Create, &error),
           qPrintable(error));
  QVERIFY(database.readyOutbox(100, &error).isEmpty());

  event = database.event(event.id, &error);
  QVERIFY(!event.id.isEmpty());
  QVERIFY2(
      database.saveLocalEvent(&event, OutboxOperation::Remove, &error,
                              QStringLiteral("probe-mutation-id"),
                              QStringLiteral("series"), QStringLiteral("none"),
                              event.localRevision, QStringLiteral("probe-dependency")),
      qPrintable(error));
  const OutboxItem queued = database.readyOutbox(100, &error).constFirst();
  QCOMPARE(queued.operation, OutboxOperation::Remove);
  QCOMPARE(queued.accountId, QStringLiteral("local-account"));
  QCOMPARE(queued.state, OutboxState::Pending);

  provider.syncAccount(QStringLiteral("local-account"));
  QVERIFY(database.readyOutbox(100, &error).isEmpty());
  // The completed remove cascades away with its deleted event row, so the
  // outbox is empty again rather than holding a done entry.
  QCOMPARE(database.outboxItems(100, &error).size(), 0);
  QCOMPARE(eventsSpy.count(), 1);
  const QVariantList calendars = eventsSpy.constFirst().at(0).value<QVariantList>();
  QCOMPARE(calendars.size(), 1);
  QCOMPARE(calendars.constFirst().toString(), event.calendarId);
  QVERIFY(operationsSpy.count() >= 1);
}

void LocalProviderTest::drainEmitsErrorStatusWithoutDatabase() {
  Database database;
  LocalProvider provider(&database);

  QSignalSpy statusSpy(&provider, &LocalProvider::syncStatusChanged);
  provider.syncAll();

  QCOMPARE(statusSpy.count(), 1);
  const QVariantList arguments = statusSpy.constFirst();
  const QJsonObject status = arguments.at(1).value<QJsonObject>();
  QCOMPARE(status.value(QStringLiteral("state")).toString(), QStringLiteral("error"));
  QCOMPARE(status.value(QStringLiteral("errorCode")).toString(),
           QStringLiteral("database_unavailable"));
}

void LocalProviderTest::drainFiltersByAccount() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  Database database;
  QString error;
  QVERIFY2(database.open(directory.filePath(QStringLiteral("store.sqlite")), &error),
           qPrintable(error));

  LocalProvider provider(&database);
  QSignalSpy eventsSpy(&provider, &LocalProvider::eventsChanged);

  Event event =
      sampleEvent(QStringLiteral("Queued for local"), utcDateTime(2027, 5, 6));
  QVERIFY2(database.saveLocalEvent(&event, OutboxOperation::Create, &error),
           qPrintable(error));
  event = database.event(event.id, &error);
  QVERIFY(!event.id.isEmpty());
  QVERIFY2(
      database.saveLocalEvent(&event, OutboxOperation::Remove, &error,
                              QStringLiteral("probe-mutation-id"),
                              QStringLiteral("series"), QStringLiteral("none"),
                              event.localRevision, QStringLiteral("probe-dependency")),
      qPrintable(error));
  QVERIFY(!database.readyOutbox(100, &error).isEmpty());

  provider.syncAccount(QStringLiteral("some-other-account"));
  QVERIFY(!database.readyOutbox(100, &error).isEmpty());

  provider.syncAccount(QStringLiteral("local-account"));
  QVERIFY(database.readyOutbox(100, &error).isEmpty());
  QCOMPARE(eventsSpy.count(), 1);
}

QTEST_GUILESS_MAIN(LocalProviderTest)
#include "test_localprovider.moc"
