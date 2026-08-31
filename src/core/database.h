#pragma once

#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

#include "core/domain.h"

namespace omacalendar {

// Daemon-private metadata for a read-only iCalendar subscription.  This type
// is intentionally not part of the presentation DTOs: subscription URLs and
// validators can contain bearer-like secrets and must never cross IPC.
struct IcsSubscription {
  QString accountId;
  QString url;
  QString etag;
  QString lastModified;
  int refreshSeconds = 3600;
  QDateTime lastSuccessAt;
  QString lastErrorCode;
  QString lastErrorMessage;
};

// A completed provider fetch window. Coverage is daemon-private state used to
// distinguish "there are no events here" from "this range has not been
// hydrated yet". Rows are normalized to UTC and merged when they overlap.
struct SyncCoverage {
  QString calendarId;
  QDateTime startUtc;
  QDateTime endUtc;
  bool complete = false;
  QDateTime updatedAt;
};

struct EventSearchQuery {
  QString text;
  QStringList calendarIds;
  QDateTime startUtc;
  QDateTime endUtc;
  QString accountId;
  QString invitationState;
  int limit = 100;
  int offset = 0;
};

struct EventSearchPage {
  QList<Event> events;
  int total = 0;
};

class Database final {
 public:
  Database();
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  bool open(const QString& path, QString* errorMessage = nullptr);
  void close();
  [[nodiscard]] bool isOpen() const;
  [[nodiscard]] int schemaVersion() const;
  [[nodiscard]] qint64 changeRevision() const;

  bool upsertAccount(Account account, QString* errorMessage = nullptr);
  [[nodiscard]] QList<Account> accounts(QString* errorMessage = nullptr) const;
  [[nodiscard]] Account account(const QString& accountId,
                                QString* errorMessage = nullptr) const;
  bool removeAccount(const QString& accountId, QString* errorMessage = nullptr);

  bool upsertCalendar(Calendar calendar, QString* errorMessage = nullptr);
  bool removeLocalCalendar(const QString& calendarId, QString* errorMessage = nullptr);
  bool removeCalendarCache(const QString& calendarId, QString* errorMessage = nullptr);
  bool updateCalendarPreferences(const QString& calendarId, bool enabled,
                                 const QString& colorOverride, int position,
                                 bool ignoreAlerts, QString* errorMessage = nullptr);
  [[nodiscard]] QList<Calendar> calendars(const QString& accountId = {},
                                          QString* errorMessage = nullptr) const;
  [[nodiscard]] Calendar calendar(const QString& calendarId,
                                  QString* errorMessage = nullptr) const;
  [[nodiscard]] Calendar calendarByRemoteId(const QString& accountId,
                                            const QString& remoteId,
                                            QString* errorMessage = nullptr) const;

  bool applyRemoteEvent(Event event, QString* errorMessage = nullptr,
                        bool* conflicted = nullptr);
  bool removeRemoteEvent(const QString& calendarId, const QString& remoteId,
                         const QString& remotePayload = {},
                         QString* errorMessage = nullptr, bool* conflicted = nullptr);
  // Commits one provider response as a unit. A parse/database failure rolls
  // back every upsert, deletion, calendar token, and conflict discovered in
  // the batch, leaving the previous cache usable.
  bool applyRemoteSyncBatch(const Calendar& calendar, const QList<Event>& events,
                            const QStringList& deletedRemoteIds,
                            const QStringList& prunedRemoteIds = {},
                            QString* errorMessage = nullptr);
  // Commits a bounded replacement and its completed coverage marker in one
  // savepoint. Failed parsing/database work therefore cannot make an
  // uncovered range look complete after restart.
  bool applyRemoteRangeSyncBatch(const Calendar& calendar, const QList<Event>& events,
                                 const QStringList& deletedRemoteIds,
                                 const QStringList& prunedRemoteIds,
                                 const QDateTime& coverageStartUtc,
                                 const QDateTime& coverageEndUtc,
                                 QString* errorMessage = nullptr,
                                 bool replaceExistingCoverage = false);
  // Replaces one ICS feed cache and its success metadata as a single durable
  // unit. The outer savepoint also contains the remote-sync batch, so a
  // failure after event staging cannot expose new events with old validators
  // (or vice versa).
  bool applyIcsFeedReplacement(const Calendar& calendar,
                               const QList<Event>& replacementEvents,
                               const QStringList& staleRemoteIds, const QString& etag,
                               const QString& lastModified, const QDateTime& successAt,
                               QString* errorMessage = nullptr);
  // Records an HTTP/precondition conflict against the durable mutation that
  // encountered it. Provider payloads remain stored only in the daemon DB;
  // presentation Conflict DTOs expose the canonical snapshots instead.
  bool recordProviderConflict(qint64 mutationId, const Event* remoteEvent,
                              QString* errorMessage = nullptr);
  bool clearCleanRemoteEvents(const QString& calendarId,
                              QString* errorMessage = nullptr);
  bool removeCleanEventsExcept(const QString& calendarId,
                               const QStringList& retainedRemoteIds,
                               QString* errorMessage = nullptr);
  bool saveLocalEvent(Event* event, OutboxOperation operation,
                      QString* errorMessage = nullptr,
                      const QString& clientMutationId = {},
                      const QString& recurrenceScope = QStringLiteral("series"),
                      const QString& sendUpdates = QStringLiteral("none"),
                      qint64 expectedLocalRevision = -1,
                      const QString& dependencyMutationId = {});
  // Atomically updates the canonical event to a calendar on the same account
  // and, for remote providers, queues one daemon-private Move mutation.
  bool moveLocalEvent(Event* event, QString* errorMessage = nullptr,
                      const QString& clientMutationId = {},
                      const QString& recurrenceScope = QStringLiteral("series"),
                      const QString& sendUpdates = QStringLiteral("none"),
                      qint64 expectedLocalRevision = -1);
  // Atomically queues the create-then-delete sequence used for moves between
  // accounts/providers and detached recurrence occurrences. The source removal
  // depends on the destination create, so a crash cannot expose a half-queued
  // move or delete the source early. A detached source may be a newly generated
  // occurrence whose series master supplies the expected local revision.
  bool moveEventAcrossAccounts(
      const Event& source, Event* destination, QString* errorMessage = nullptr,
      const QString& clientMutationId = {},
      const QString& recurrenceScope = QStringLiteral("series"),
      const QString& sendUpdates = QStringLiteral("none"),
      qint64 expectedLocalRevision = -1);
  bool markLocalEventRemoved(const QString& eventId, QString* errorMessage = nullptr);
  [[nodiscard]] Event event(const QString& eventId,
                            QString* errorMessage = nullptr) const;
  [[nodiscard]] Event eventByRemoteId(const QString& calendarId,
                                      const QString& remoteId,
                                      QString* errorMessage = nullptr) const;
  [[nodiscard]] QList<Event> eventsBetween(const QDateTime& startUtc,
                                           const QDateTime& endUtc,
                                           const QStringList& calendarIds = {},
                                           QString* errorMessage = nullptr) const;
  // Invitation reads deliberately stay daemon-private. They apply the same
  // recurrence semantics as eventsBetween(), but discard unrelated bounded
  // rows before decoding them from SQLite.
  [[nodiscard]] QList<Event> invitationEventsBetween(
      const QDateTime& startUtc, const QDateTime& endUtc,
      QString* errorMessage = nullptr) const;
  // Fetches invitation seen flags in bounded batches instead of issuing one
  // settings query per presentation row. Missing/invalid values remain false.
  [[nodiscard]] QHash<QString, bool> invitationSeenStates(
      const QStringList& eventIds, QString* errorMessage = nullptr) const;
  [[nodiscard]] QList<Event> eventsForCalendars(const QStringList& calendarIds,
                                                QString* errorMessage = nullptr) const;
  [[nodiscard]] Event eventByUid(const QString& calendarId, const QString& uid,
                                 const QString& recurrenceId = {},
                                 QString* errorMessage = nullptr) const;
  [[nodiscard]] QList<Event> eventsByUid(const QString& calendarId, const QString& uid,
                                         QString* errorMessage = nullptr) const;
  [[nodiscard]] QList<Event> searchEvents(const QString& text,
                                          const QStringList& calendarIds = {},
                                          int limit = 100, int offset = 0,
                                          QString* errorMessage = nullptr) const;
  // Applies every filter in the indexed SQL query before LIMIT/OFFSET and
  // returns the total for that exact filtered result set.
  [[nodiscard]] EventSearchPage searchEvents(const EventSearchQuery& search,
                                             QString* errorMessage = nullptr) const;

  [[nodiscard]] QList<OutboxItem> readyOutbox(int limit = 100,
                                              QString* errorMessage = nullptr) const;
  [[nodiscard]] QList<OutboxItem> outboxItems(int limit = 100,
                                              QString* errorMessage = nullptr) const;
  bool recoverExpiredOutbox(QString* errorMessage = nullptr);
  bool updateOutboxState(qint64 id, OutboxState state, int attempts,
                         const QDateTime& nextAttemptAt, const QString& errorCode = {},
                         const QString& errorMessage = {},
                         QString* databaseError = nullptr);
  bool completeOutbox(qint64 id, const Event* remoteEvent = nullptr,
                      QString* errorMessage = nullptr);
  // A provider acknowledgement that also returns a canonical resource snapshot
  // must commit both pieces of state together. If staging any parsed event fails,
  // the outbox item remains unacknowledged and startup recovery can safely retry.
  bool completeOutboxWithRemoteSyncBatch(qint64 id, const Event* remoteEvent,
                                         const Calendar& calendar,
                                         const QList<Event>& events,
                                         const QStringList& deletedRemoteIds = {},
                                         const QStringList& prunedRemoteIds = {},
                                         QString* errorMessage = nullptr);
  bool discardOutbox(qint64 id, QString* errorMessage = nullptr);

  [[nodiscard]] QList<CalendarSet> calendarSets(QString* errorMessage = nullptr) const;
  bool upsertCalendarSet(CalendarSet* set, QString* errorMessage = nullptr);
  bool removeCalendarSet(const QString& setId, QString* errorMessage = nullptr);
  bool activateCalendarSet(const QString& setId, QString* errorMessage = nullptr);

  [[nodiscard]] QList<Conflict> conflicts(bool unresolvedOnly = true,
                                          QString* errorMessage = nullptr) const;
  bool resolveConflict(qint64 id, const QString& strategy,
                       const QJsonObject& mergedEvent = {},
                       QString* errorMessage = nullptr);
  int resolveNewestConflicts(bool* queuedLocalWrites = nullptr,
                             QString* errorMessage = nullptr);

  [[nodiscard]] QList<ReminderJob> reminders(int limit = 100,
                                             QString* errorMessage = nullptr) const;
  [[nodiscard]] QList<ReminderJob> dueReminders(const QDateTime& now, int limit = 50,
                                                QString* errorMessage = nullptr) const;
  // Detached-exception reminders stay queued while their recurring master is
  // hidden by an active series removal. This makes delete undo restore the
  // existing jobs instead of allowing a notification through or discarding it.
  [[nodiscard]] bool hasPendingSeriesRemoval(const QString& calendarId,
                                             const QString& uid,
                                             QString* errorMessage = nullptr) const;
  bool claimReminderDelivery(qint64 id, const QString& claimToken,
                             const QDateTime& claimedAt,
                             const QDateTime& leaseExpiresAt, bool* claimed,
                             QString* errorMessage = nullptr);
  bool finishReminderDelivery(qint64 id, const QString& claimToken,
                              const QDateTime& deliveredAt,
                              QString* errorMessage = nullptr);
  bool releaseReminderDelivery(qint64 id, const QString& claimToken,
                               QString* errorMessage = nullptr);
  bool recoverExpiredReminderDeliveries(const QDateTime& recoveredAt,
                                        QString* errorMessage = nullptr);
  bool recoverClaimedNotificationDeliveries(const QDateTime& recoveredAt,
                                            QString* errorMessage = nullptr);
  bool snoozeReminder(qint64 id, int minutes, QString* errorMessage = nullptr);
  bool snoozeReminderAt(qint64 id, int minutes, const QDateTime& now,
                        QString* errorMessage = nullptr);
  bool dismissReminder(qint64 id, QString* errorMessage = nullptr);
  bool markReminderDelivered(qint64 id, QString* errorMessage = nullptr);
  [[nodiscard]] QList<Event> notificationEventsForCalendars(
      const QStringList& calendarIds, QString* errorMessage = nullptr) const;
  bool claimNotificationDelivery(const QString& fingerprint, const QString& kind,
                                 const QString& eventId, qint64 eventRevision,
                                 const QDateTime& claimedAt, bool* claimed,
                                 QString* errorMessage = nullptr);
  bool finishNotificationDelivery(const QString& fingerprint,
                                  const QDateTime& deliveredAt,
                                  QString* errorMessage = nullptr);
  bool releaseNotificationDelivery(const QString& fingerprint,
                                   QString* errorMessage = nullptr);
  [[nodiscard]] bool hasNotificationDeliveryForEvent(
      const QString& eventId, QString* errorMessage = nullptr) const;

  [[nodiscard]] QJsonValue providerState(const QString& accountId,
                                         const QString& calendarId, const QString& key,
                                         const QJsonValue& fallback = {},
                                         QString* errorMessage = nullptr) const;
  bool setProviderState(const QString& accountId, const QString& calendarId,
                        const QString& key, const QJsonValue& value,
                        QString* errorMessage = nullptr);
  bool clearProviderState(const QString& accountId, const QString& calendarId = {},
                          QString* errorMessage = nullptr);

  bool upsertIcsSubscription(const IcsSubscription& subscription,
                             QString* errorMessage = nullptr);
  [[nodiscard]] IcsSubscription icsSubscription(const QString& accountId,
                                                QString* errorMessage = nullptr) const;
  [[nodiscard]] QList<IcsSubscription> icsSubscriptions(
      QString* errorMessage = nullptr) const;
  bool updateIcsSubscriptionResult(const QString& accountId, const QString& etag,
                                   const QString& lastModified,
                                   const QDateTime& successAt,
                                   const QString& errorCode = {},
                                   const QString& errorText = {},
                                   QString* errorMessage = nullptr);

  [[nodiscard]] QList<SyncCoverage> syncCoverage(const QString& calendarId,
                                                 QString* errorMessage = nullptr) const;
  [[nodiscard]] bool isSyncRangeCovered(const QString& calendarId,
                                        const QDateTime& startUtc,
                                        const QDateTime& endUtc,
                                        QString* errorMessage = nullptr) const;
  bool recordSyncCoverage(const QString& calendarId, const QDateTime& startUtc,
                          const QDateTime& endUtc, QString* errorMessage = nullptr);

  [[nodiscard]] QJsonValue setting(const QString& key, const QJsonValue& fallback = {},
                                   QString* errorMessage = nullptr) const;
  bool setSetting(const QString& key, const QJsonValue& value,
                  QString* errorMessage = nullptr);

 private:
  bool migrate(QString* errorMessage);
  bool ensureOutboxMoveSchema(QString* errorMessage);
  bool ensureConflictUniquenessSchema(QString* errorMessage);
  bool ensureReminderDeliverySchema(QString* errorMessage);
  bool ensureSyncCoverageSchema(QString* errorMessage);
  bool ensureReadPerformanceIndexes(QString* errorMessage);
  bool archiveLegacyDatabase(const QString& path, QString* errorMessage);
  bool execute(const QString& sql, QString* errorMessage) const;
  bool bumpChangeRevision(QString* errorMessage = nullptr) const;
  bool upsertEventRecord(const Event& event, QString* errorMessage);
  bool completeOutboxInternal(qint64 id, const Event* remoteEvent,
                              bool manageTransaction, QString* errorMessage);
  bool upsertUnresolvedConflict(const QString& eventId, qint64 mutationId,
                                const QString& kind, const QString& localRevision,
                                const QString& remoteRevision,
                                const QJsonObject& localSnapshot,
                                const QJsonObject& remoteSnapshot,
                                const QString& remotePayload,
                                const QDateTime& createdAt, QString* errorMessage);
  bool rebuildEventInstances(const Event& event, QString* errorMessage);
  bool rebuildReminderJobs(const Event& event, QString* errorMessage);
  bool removeRemoteEventInternal(const QString& calendarId, const QString& remoteId,
                                 const QString& remotePayload, QString* errorMessage,
                                 bool* conflicted, bool manageTransaction);
  bool mergeSyncCoverageInternal(const QString& calendarId, const QDateTime& startUtc,
                                 const QDateTime& endUtc, QString* errorMessage);
  bool activeMoveReservesRemote(const QString& calendarId, const QString& remoteId,
                                bool* reserved, QString* errorMessage = nullptr) const;
  [[nodiscard]] Event eventFromQuery(const QSqlQuery& query) const;
  [[nodiscard]] OutboxItem outboxFromQuery(const QSqlQuery& query) const;
  [[nodiscard]] QList<Event> eventsBetweenInternal(const QDateTime& startUtc,
                                                   const QDateTime& endUtc,
                                                   const QStringList& calendarIds,
                                                   bool invitationsOnly,
                                                   QString* errorMessage) const;

  QString m_connectionName;
  QSqlDatabase m_database;
};

}  // namespace omacalendar
