#include "reminders/reminderscheduler.h"

#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTime>
#include <QTimeZone>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace omacalendar {
namespace {

constexpr auto kDesktopEntry = "org.omacalendar.OmaCalendar";
constexpr int kReminderClaimLeaseSeconds = 2 * 60;
// Above this many simultaneous invitations a single digest notification is
// sent instead of one notification per invitation.
constexpr int kInvitationDigestThreshold = 5;
// Invitations for occurrences that ended before this cutoff are imported
// silently (fingerprint recorded, no notification). Covers historical
// invitation backfill after adding a calendar with past invites.
constexpr int kInvitationPastCutoffHours = 48;

QString notificationPrivacy(const Database* database) {
  return database == nullptr ? QStringLiteral("generic")
                             : database
                                   ->setting(QStringLiteral("notificationPrivacy"),
                                             QStringLiteral("generic"))
                                   .toString();
}

QString eventTitle(const Event& event) {
  return event.summary.trimmed().isEmpty() ? QStringLiteral("Calendar event")
                                           : event.summary.toHtmlEscaped();
}

}  // namespace

FreedesktopNotificationBackend::FreedesktopNotificationBackend(QObject* parent)
    : NotificationBackend(parent),
      m_interface(new QDBusInterface(QStringLiteral("org.freedesktop.Notifications"),
                                     QStringLiteral("/org/freedesktop/Notifications"),
                                     QStringLiteral("org.freedesktop.Notifications"),
                                     QDBusConnection::sessionBus(), this)) {
  QDBusConnection::sessionBus().connect(
      QStringLiteral("org.freedesktop.Notifications"),
      QStringLiteral("/org/freedesktop/Notifications"),
      QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("ActionInvoked"),
      this, SLOT(onActionInvoked(uint, QString)));
}

FreedesktopNotificationBackend::~FreedesktopNotificationBackend() = default;

void FreedesktopNotificationBackend::send(const CalendarNotification& notification) {
  if (m_interface == nullptr || !m_interface->isValid()) {
    QTimer::singleShot(0, this,
                       [this, fingerprint = notification.fingerprint,
                        deliveryToken = notification.deliveryToken]() {
                         emit notificationResult(
                             fingerprint, deliveryToken, 0,
                             QStringLiteral("Notification service unavailable"));
                       });
    return;
  }
  const QVariantList arguments{
      QStringLiteral("OmaCalendar"),
      uint(0),
      QString::fromLatin1(kDesktopEntry),
      notification.summary,
      notification.body,
      notification.actions,
      notification.hints,
      notification.timeoutMilliseconds,
  };
  auto* watcher = new QDBusPendingCallWatcher(
      m_interface->asyncCallWithArgumentList(QStringLiteral("Notify"), arguments),
      this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, watcher, fingerprint = notification.fingerprint,
           deliveryToken = notification.deliveryToken]() {
            const QDBusPendingReply<uint> reply = *watcher;
            if (reply.isError()) {
              emit notificationResult(fingerprint, deliveryToken, 0,
                                      reply.error().message());
            } else {
              emit notificationResult(fingerprint, deliveryToken, reply.value(), {});
            }
            watcher->deleteLater();
          });
}

void FreedesktopNotificationBackend::onActionInvoked(const uint notificationId,
                                                     const QString& action) {
  emit actionInvoked(notificationId, action);
}

ReminderScheduler::ReminderScheduler(Database* database, QObject* parent)
    : ReminderScheduler(
          database, new FreedesktopNotificationBackend(),
          []() { return QDateTime::currentDateTimeUtc(); },
          [](const QUrl& url) {
            return QProcess::startDetached(QStringLiteral("xdg-open"),
                                           {url.toString(QUrl::FullyEncoded)});
          },
          parent) {
  m_backend->setParent(this);
  connectSystemSleep();
}

ReminderScheduler::ReminderScheduler(Database* database, NotificationBackend* backend,
                                     NowProvider nowProvider, LinkOpener linkOpener,
                                     QObject* parent)
    : QObject(parent),
      m_database(database),
      m_backend(backend),
      m_nowProvider(std::move(nowProvider)),
      m_linkOpener(std::move(linkOpener)) {
  m_timer.setInterval(30000);
  connect(&m_timer, &QTimer::timeout, this, &ReminderScheduler::checkNow);
  initializeBackend();
}

QDateTime ReminderScheduler::now() const {
  return m_nowProvider ? m_nowProvider().toUTC() : QDateTime::currentDateTimeUtc();
}

void ReminderScheduler::initializeBackend() {
  if (m_backend == nullptr) {
    return;
  }
  connect(m_backend, &NotificationBackend::notificationResult, this,
          &ReminderScheduler::onNotificationResult);
  connect(m_backend, &NotificationBackend::actionInvoked, this,
          &ReminderScheduler::onActionInvoked);
}

void ReminderScheduler::connectSystemSleep() {
  QDBusConnection::systemBus().connect(QStringLiteral("org.freedesktop.login1"),
                                       QStringLiteral("/org/freedesktop/login1"),
                                       QStringLiteral("org.freedesktop.login1.Manager"),
                                       QStringLiteral("PrepareForSleep"), this,
                                       SLOT(onPrepareForSleep(bool)));
}

void ReminderScheduler::start() {
  if (m_database == nullptr || !m_database->isOpen()) {
    return;
  }
  QString error;
  if (!m_database->recoverClaimedNotificationDeliveries(now(), &error)) {
    emit notificationError(QStringLiteral("recovery"), error);
  }
  error.clear();
  QStringList calendarIds;
  for (const Calendar& calendar : m_database->calendars({}, &error)) {
    if (calendar.enabled) {
      calendarIds.append(calendar.id);
    }
  }
  if (!error.isEmpty()) {
    emit notificationError(QStringLiteral("invitation-scan"), error);
  } else {
    baselineInvitations(calendarIds);
    for (const QString& calendarId : std::as_const(calendarIds)) {
      m_initializedInvitationCalendars.insert(calendarId);
    }
  }
  m_timer.start();
  QTimer::singleShot(0, this, &ReminderScheduler::checkNow);
}

void ReminderScheduler::stop() { m_timer.stop(); }

void ReminderScheduler::checkNow() {
  if (m_sleeping || m_database == nullptr || !m_database->isOpen() ||
      m_backend == nullptr) {
    return;
  }
  const QDateTime current = now();
  QString error;
  if (!m_database->recoverExpiredReminderDeliveries(current, &error)) {
    emit notificationError(QStringLiteral("recovery"), error);
    return;
  }
  const qint64 dismissed =
      m_database->dismissStaleReminders(current, kStaleReminderGraceSeconds, &error);
  if (dismissed < 0) {
    emit notificationError(QStringLiteral("stale-sweep"), error);
    return;
  }
  if (dismissed > 0) {
    emit reminderStateChanged();
  }
  for (auto iterator = m_pending.begin(); iterator != m_pending.end();) {
    if (iterator->kind == DeliveryKind::Reminder &&
        iterator->leaseExpiresAt.isValid() && iterator->leaseExpiresAt <= current) {
      iterator = m_pending.erase(iterator);
    } else {
      ++iterator;
    }
  }
  error.clear();
  const QList<ReminderJob> due = m_database->dueReminders(current, 100, &error);
  if (!error.isEmpty()) {
    emit notificationError(QStringLiteral("reminder-query"), error);
    return;
  }
  bool progressed = false;
  for (const ReminderJob& reminder : due) {
    progressed = deliver(reminder) || progressed;
  }
  // A full window with progress means a backlog is still waiting; drain it on
  // the next loop turn instead of waiting a whole timer interval. Without
  // progress (e.g. a failing backend) the timer interval applies instead.
  if (progressed && due.size() >= 100) {
    QTimer::singleShot(0, this, &ReminderScheduler::checkNow);
  }
}

void ReminderScheduler::eventsChanged(const QStringList& calendarIds) {
  if (m_sleeping) {
    for (const QString& calendarId : calendarIds) {
      if (!m_deferredCalendarIds.contains(calendarId)) {
        m_deferredCalendarIds.append(calendarId);
      }
    }
    return;
  }
  QStringList initializedCalendarIds;
  for (const QString& calendarId : calendarIds) {
    if (m_initializedInvitationCalendars.contains(calendarId)) {
      initializedCalendarIds.append(calendarId);
    }
  }
  scanInvitations(initializedCalendarIds);
  checkNow();
}

void ReminderScheduler::syncCompleted(const QString& accountId) {
  if (m_database == nullptr || !m_database->isOpen() || accountId.isEmpty()) {
    return;
  }
  QString error;
  QStringList newCalendarIds;
  for (const Calendar& calendar : m_database->calendars(accountId, &error)) {
    if (calendar.enabled && !m_initializedInvitationCalendars.contains(calendar.id)) {
      newCalendarIds.append(calendar.id);
    }
  }
  if (!error.isEmpty()) {
    emit notificationError(QStringLiteral("invitation-baseline"), error);
    return;
  }
  baselineInvitations(newCalendarIds);
  for (const QString& calendarId : std::as_const(newCalendarIds)) {
    m_initializedInvitationCalendars.insert(calendarId);
  }
}

void ReminderScheduler::handlePrepareForSleep(const bool sleeping) {
  m_sleeping = sleeping;
  if (!sleeping) {
    const QStringList changedCalendars = std::exchange(m_deferredCalendarIds, {});
    QTimer::singleShot(0, this, [this, changedCalendars]() {
      if (!changedCalendars.isEmpty()) {
        eventsChanged(changedCalendars);
      } else {
        checkNow();
      }
    });
  }
}

bool ReminderScheduler::deliver(const ReminderJob& reminder) {
  const Event event = m_database->event(reminder.eventId);
  if (event.id.isEmpty() || event.deleted ||
      event.status.compare(QStringLiteral("cancelled"), Qt::CaseInsensitive) == 0) {
    if (m_database->dismissReminder(reminder.id)) {
      emit reminderStateChanged();
    }
    return false;
  }
  if (!event.recurrenceId.isEmpty()) {
    QString seriesError;
    const bool seriesRemovalPending =
        m_database->hasPendingSeriesRemoval(event.calendarId, event.uid, &seriesError);
    if (!seriesError.isEmpty()) {
      emit notificationError(reminder.fingerprint, seriesError);
      return false;
    }
    if (seriesRemovalPending) {
      // Keep the job pending: acknowledgement deletes the whole series and its
      // jobs atomically, while undo makes this same job deliverable again.
      return false;
    }
  }
  const QDateTime claimedAt = now();
  const QDateTime leaseExpiresAt = claimedAt.addSecs(kReminderClaimLeaseSeconds);
  const QString deliveryToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
  // Defensive re-check for jobs that aged past the grace window between the
  // due query and this claim (e.g. a long event-loop stall). Snoozed jobs are
  // explicit user intent and always deliver. An unparseable occurrence id is
  // not evidence of a past occurrence: the due query resolves chronology from
  // event_instances, so only a validly parsed past occurrence dismisses here.
  if (reminder.state.compare(QStringLiteral("pending"), Qt::CaseInsensitive) == 0 &&
      reminder.fireAt < claimedAt.addSecs(-kStaleReminderGraceSeconds)) {
    const QDateTime occurrence = dateTimeFromIso(reminder.occurrenceId);
    if (occurrence.isValid() && occurrence <= claimedAt) {
      if (m_database->dismissReminder(reminder.id)) {
        emit reminderStateChanged();
      }
      return false;
    }
  }
  bool claimed = false;
  QString error;
  if (!m_database->claimReminderDelivery(reminder.id, deliveryToken, claimedAt,
                                         leaseExpiresAt, &claimed, &error)) {
    emit notificationError(reminder.fingerprint, error);
    return false;
  }
  if (!claimed) {
    return false;
  }
  PendingDelivery pending;
  pending.kind = DeliveryKind::Reminder;
  pending.reminderId = reminder.id;
  pending.eventId = event.id;
  pending.occurrenceId = reminder.occurrenceId;
  pending.fingerprint = QStringLiteral("reminder:") + reminder.fingerprint;
  pending.deliveryToken = deliveryToken;
  pending.leaseExpiresAt = leaseExpiresAt;
  m_pending.insert(deliveryToken, pending);
  CalendarNotification notification = reminderNotification(event, reminder);
  notification.deliveryToken = deliveryToken;
  m_backend->send(notification);
  return true;
}

void ReminderScheduler::baselineInvitations(const QStringList& calendarIds) {
  if (calendarIds.isEmpty()) {
    return;
  }
  QString error;
  const QList<Event> invitations =
      m_database->notificationEventsForCalendars(calendarIds, &error);
  if (!error.isEmpty()) {
    emit notificationError(QStringLiteral("invitation-baseline"), error);
    return;
  }
  // Durable baselines survive daemon restarts. Only events without any
  // durable delivery record are baselined here: re-recording the current
  // fingerprint of a known event would swallow changes that arrived while
  // the daemon was offline.
  QList<Event> fresh;
  fresh.reserve(invitations.size());
  for (const Event& event : invitations) {
    error.clear();
    const bool durable = m_database->hasNotificationDeliveryForEvent(event.id, &error);
    if (!error.isEmpty()) {
      emit notificationError(QStringLiteral("invitation-baseline"), error);
      return;
    }
    if (!durable) {
      fresh.append(event);
    }
  }
  if (fresh.isEmpty()) {
    return;
  }
  if (!m_database->persistInvitationBaselines(fresh, &error)) {
    emit notificationError(QStringLiteral("invitation-baseline"), error);
    return;
  }
  for (const Event& event : fresh) {
    m_invitationBaseline.insert(event.id, invitationFingerprint(event));
  }
}

void ReminderScheduler::scanInvitations(const QStringList& calendarIds) {
  if (m_database == nullptr || !m_database->isOpen() || m_backend == nullptr ||
      calendarIds.isEmpty()) {
    return;
  }
  QString error;
  const QList<Event> invitations =
      m_database->notificationEventsForCalendars(calendarIds, &error);
  if (!error.isEmpty()) {
    emit notificationError(QStringLiteral("invitation-query"), error);
    return;
  }
  const QDateTime current = now();

  struct Deliverable {
    Event event;
    bool changed = false;
    QString fingerprint;
  };
  QList<Deliverable> deliverables;
  deliverables.reserve(invitations.size());
  QList<Event> silentBaselines;
  silentBaselines.reserve(invitations.size());

  for (const Event& event : invitations) {
    if (event.dirty) {
      continue;
    }
    error.clear();
    const QString fingerprint = invitationFingerprint(event);
    if (m_invitationBaseline.contains(event.id)) {
      if (m_invitationBaseline.value(event.id) == fingerprint) {
        continue;
      }
    } else {
      // In-memory state is only a fast path. After a daemon restart the
      // durable record for this exact version decides: any prior claim,
      // delivery, digest or baseline for this fingerprint means it was seen.
      const bool fingerprintKnown =
          m_database->notificationDeliveryExists(fingerprint, &error);
      if (!error.isEmpty()) {
        emit notificationError(fingerprint, error);
        continue;
      }
      if (fingerprintKnown) {
        m_invitationBaseline.insert(event.id, fingerprint);
        continue;
      }
    }

    error.clear();
    const bool durableHistory =
        m_database->hasNotificationDeliveryForEvent(event.id, &error);
    if (!error.isEmpty()) {
      emit notificationError(QStringLiteral("invitation-history"), error);
      continue;
    }
    const bool seen =
        m_database
            ->setting(QStringLiteral("invitation_seen_%1").arg(event.id), false, &error)
            .toBool();
    if (!error.isEmpty()) {
      emit notificationError(fingerprint, error);
      continue;
    }
    if (seen && !durableHistory) {
      const bool claimOk = m_database->claimNotificationDelivery(
          fingerprint, QStringLiteral("invitation_seen"), event.id, event.localRevision,
          current, nullptr, &error);
      if (!claimOk && !error.isEmpty()) {
        emit notificationError(fingerprint, error);
        continue;
      }
      error.clear();
      if (!m_database->finishNotificationDelivery(fingerprint, current, &error) &&
          !error.isEmpty()) {
        emit notificationError(fingerprint, error);
        continue;
      }
      m_invitationBaseline.insert(event.id, fingerprint);
      continue;
    }

    const bool changed = durableHistory;
    const bool cancelled =
        event.deleted ||
        event.status.compare(QStringLiteral("cancelled"), Qt::CaseInsensitive) == 0;
    // Notifications cover recent invitations only. Older invitation history
    // (for example pulled in by the one-year backfill chunks after a new
    // calendar is added) and cancellations of long-past meetings are imported
    // silently; the baseline batch records their fingerprints once.
    if (cancelled || invitationEndedBeforeCutoff(event, current)) {
      silentBaselines.append(event);
      continue;
    }
    deliverables.append({event, changed, fingerprint});
  }

  if (!silentBaselines.isEmpty() &&
      !m_database->persistInvitationBaselines(silentBaselines, &error)) {
    emit notificationError(QStringLiteral("invitation-baseline"), error);
  }
  if (deliverables.isEmpty()) {
    return;
  }
  if (deliverables.size() > kInvitationDigestThreshold) {
    // Claim every member fingerprint before sending. A claimed fingerprint is
    // a durable in-flight marker: finishing it on success or releasing it on
    // failure keeps digest members exactly-once across restarts and backend
    // failures instead of silently swallowing them.
    PendingDelivery digest;
    digest.kind = DeliveryKind::InvitationDigest;
    digest.fingerprint = QStringLiteral("invitation-digest:") +
                         QUuid::createUuid().toString(QUuid::WithoutBraces);
    digest.deliveryToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool changedOnly = true;
    for (const Deliverable& deliverable : deliverables) {
      const bool claimOk = m_database->claimNotificationDelivery(
          deliverable.fingerprint, QStringLiteral("invitation_digest"),
          deliverable.event.id, deliverable.event.localRevision, current, nullptr,
          &error);
      if (!claimOk && !error.isEmpty()) {
        emit notificationError(deliverable.fingerprint, error);
        continue;
      }
      digest.memberFingerprints.append(deliverable.fingerprint);
      digest.memberEventIds.append(deliverable.event.id);
      changedOnly = changedOnly && deliverable.changed;
      m_invitationBaseline.insert(deliverable.event.id, deliverable.fingerprint);
    }
    if (digest.memberFingerprints.isEmpty()) {
      return;
    }
    QList<Event> events;
    events.reserve(digest.memberFingerprints.size());
    for (const Deliverable& deliverable : deliverables) {
      if (digest.memberEventIds.contains(deliverable.event.id)) {
        events.append(deliverable.event);
      }
    }
    m_pending.insert(digest.deliveryToken, digest);
    CalendarNotification notification =
        invitationDigestNotification(events, changedOnly);
    notification.fingerprint = digest.fingerprint;
    notification.deliveryToken = digest.deliveryToken;
    m_backend->send(notification);
    emit reminderStateChanged();
    return;
  }
  for (const Deliverable& deliverable : std::as_const(deliverables)) {
    deliverInvitation(deliverable.event, deliverable.changed, deliverable.fingerprint);
  }
}

void ReminderScheduler::deliverInvitation(const Event& event, const bool changed,
                                          const QString& fingerprint) {
  bool claimed = false;
  QString error;
  const QString kind =
      changed ? QStringLiteral("invitation_changed") : QStringLiteral("invitation_new");
  if (!m_database->claimNotificationDelivery(
          fingerprint, kind, event.id, event.localRevision, now(), &claimed, &error)) {
    emit notificationError(fingerprint, error);
    return;
  }
  if (!claimed) {
    return;
  }
  PendingDelivery pending;
  pending.kind = DeliveryKind::Invitation;
  pending.eventId = event.id;
  pending.occurrenceId = event.recurrenceId;
  pending.fingerprint = fingerprint;
  pending.deliveryToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_pending.insert(pending.deliveryToken, pending);
  CalendarNotification notification =
      invitationNotification(event, changed, fingerprint);
  notification.deliveryToken = pending.deliveryToken;
  m_backend->send(notification);
}

CalendarNotification ReminderScheduler::reminderNotification(
    const Event& event, const ReminderJob& reminder) const {
  CalendarNotification notification;
  notification.fingerprint = QStringLiteral("reminder:") + reminder.fingerprint;
  const QString privacy = notificationPrivacy(m_database);
  notification.summary = QStringLiteral("Calendar reminder");
  if (privacy == QStringLiteral("full_details")) {
    notification.summary = eventTitle(event);
    const QDateTime occurrence = dateTimeFromIso(reminder.occurrenceId);
    const QDateTime start =
        occurrence.isValid() ? occurrence.toLocalTime() : event.startUtc.toLocalTime();
    notification.body = event.allDay ? QStringLiteral("All day")
                                     : start.toString(QStringLiteral("ddd h:mm AP"));
    if (!event.location.isEmpty()) {
      notification.body += QStringLiteral(" — ") + event.location.toHtmlEscaped();
    }
  } else if (privacy == QStringLiteral("title_only")) {
    notification.summary = eventTitle(event);
  } else {
    notification.body = QStringLiteral("An event is starting soon");
  }
  notification.actions = {
      QStringLiteral("default"),  QStringLiteral("Open"),
      QStringLiteral("snooze5"),  QStringLiteral("Snooze 5 min"),
      QStringLiteral("snooze10"), QStringLiteral("Snooze 10 min"),
      QStringLiteral("snooze30"), QStringLiteral("Snooze 30 min"),
      QStringLiteral("snooze60"), QStringLiteral("Snooze 1 hour"),
      QStringLiteral("dismiss"),  QStringLiteral("Dismiss"),
  };
  notification.hints = {
      {QStringLiteral("desktop-entry"), QString::fromLatin1(kDesktopEntry)},
      {QStringLiteral("category"), QStringLiteral("x-omacalendar.reminder")},
      {QStringLiteral("x-omacalendar-fingerprint"), notification.fingerprint},
  };
  return notification;
}

CalendarNotification ReminderScheduler::invitationNotification(
    const Event& event, const bool changed, const QString& fingerprint) const {
  CalendarNotification notification;
  notification.fingerprint = fingerprint;
  const QString privacy = notificationPrivacy(m_database);
  notification.summary = changed ? QStringLiteral("Invitation updated")
                                 : QStringLiteral("New calendar invitation");
  if (event.deleted ||
      event.status.compare(QStringLiteral("cancelled"), Qt::CaseInsensitive) == 0) {
    notification.summary = QStringLiteral("Invitation cancelled");
  }
  if (privacy == QStringLiteral("full_details")) {
    notification.body = eventTitle(event);
    if (!event.location.isEmpty()) {
      notification.body += QStringLiteral(" — ") + event.location.toHtmlEscaped();
    }
  } else if (privacy == QStringLiteral("title_only")) {
    notification.body = eventTitle(event);
  } else {
    notification.body = changed ? QStringLiteral("A calendar invitation changed")
                                : QStringLiteral("You received an invitation");
  }
  notification.actions = {QStringLiteral("default"), QStringLiteral("Open"),
                          QStringLiteral("dismiss"), QStringLiteral("Dismiss")};
  notification.hints = {
      {QStringLiteral("desktop-entry"), QString::fromLatin1(kDesktopEntry)},
      {QStringLiteral("category"), QStringLiteral("x-omacalendar.invitation")},
      {QStringLiteral("x-omacalendar-fingerprint"), fingerprint},
  };
  return notification;
}

bool ReminderScheduler::invitationEndedBeforeCutoff(const Event& event,
                                                    const QDateTime& current) const {
  // endDate is exclusive (see daemon's invitationEnd): it is already the day
  // after the final day of an all-day occurrence.
  const QDateTime end = event.allDay
                            ? QDateTime(event.endDate, QTime(0, 0), QTimeZone::UTC)
                            : event.endUtc;
  if (!end.isValid()) {
    return false;
  }
  return end <= current.addSecs(-kInvitationPastCutoffHours * 3600);
}

CalendarNotification ReminderScheduler::invitationDigestNotification(
    const QList<Event>& events, const bool changedOnly) const {
  CalendarNotification notification;
  notification.fingerprint = QStringLiteral("invitation-digest");
  const QString privacy = notificationPrivacy(m_database);
  notification.summary =
      changedOnly ? QStringLiteral("%1 invitations updated").arg(events.size())
                  : QStringLiteral("%1 calendar invitations").arg(events.size());
  if (privacy == QStringLiteral("full_details") ||
      privacy == QStringLiteral("title_only")) {
    QStringList titles;
    for (const Event& event : events) {
      titles.append(eventTitle(event));
    }
    notification.body = titles.join(QStringLiteral(", "));
  } else {
    notification.body = QStringLiteral("Open OmaCalendar to review them");
  }
  notification.actions = {QStringLiteral("default"), QStringLiteral("Open")};
  notification.hints = {
      {QStringLiteral("desktop-entry"), QString::fromLatin1(kDesktopEntry)},
      {QStringLiteral("category"), QStringLiteral("x-omacalendar.invitation")},
  };
  return notification;
}

void ReminderScheduler::onNotificationResult(const QString& fingerprint,
                                             const QString& deliveryToken,
                                             const uint notificationId,
                                             const QString& errorMessage) {
  const auto iterator = m_pending.find(deliveryToken);
  if (iterator == m_pending.end() || iterator->fingerprint != fingerprint) {
    return;
  }
  const PendingDelivery delivery = iterator.value();
  m_pending.erase(iterator);
  if (!errorMessage.isEmpty() || notificationId == 0) {
    QString databaseError;
    if (delivery.kind == DeliveryKind::Reminder) {
      m_database->releaseReminderDelivery(delivery.reminderId, delivery.deliveryToken,
                                          &databaseError);
    } else if (delivery.kind == DeliveryKind::InvitationDigest) {
      for (const QString& memberFingerprint :
           std::as_const(delivery.memberFingerprints)) {
        m_database->releaseNotificationDelivery(memberFingerprint, &databaseError);
      }
      for (const QString& memberEventId : std::as_const(delivery.memberEventIds)) {
        m_invitationBaseline.remove(memberEventId);
      }
    } else {
      m_database->releaseNotificationDelivery(delivery.fingerprint, &databaseError);
    }
    emit notificationError(fingerprint,
                           !databaseError.isEmpty() ? databaseError : errorMessage);
    return;
  }

  QString databaseError;
  const bool persisted =
      delivery.kind == DeliveryKind::Reminder
          ? m_database->finishReminderDelivery(
                delivery.reminderId, delivery.deliveryToken, now(), &databaseError)
      : delivery.kind == DeliveryKind::InvitationDigest
          ? finishDigestMembers(delivery, &databaseError)
          : m_database->finishNotificationDelivery(delivery.fingerprint, now(),
                                                   &databaseError);
  // Keep the durable lease on persistence failure. Its expiry causes an
  // at-least-once retry. A process death after D-Bus accepted the notification
  // but before this acknowledgement can therefore duplicate one notification;
  // the stable fingerprint lets cooperating notification servers suppress it.
  if (!persisted) {
    emit notificationError(fingerprint, databaseError);
  }
  if (delivery.kind == DeliveryKind::Invitation && !delivery.eventId.isEmpty()) {
    m_invitationBaseline.insert(delivery.eventId, delivery.fingerprint);
  }
  m_active.insert(notificationId, delivery);
  emit reminderStateChanged();
}

bool ReminderScheduler::finishDigestMembers(const PendingDelivery& digest,
                                            QString* errorMessage) {
  // Members were claimed before the digest was sent; the digest callback
  // finishes every claim. A member claim that cannot be finished stays
  // claimed until its recovery path retries it.
  for (const QString& memberFingerprint : std::as_const(digest.memberFingerprints)) {
    QString memberError;
    if (!m_database->finishNotificationDelivery(memberFingerprint, now(),
                                                &memberError) &&
        !memberError.isEmpty()) {
      if (errorMessage != nullptr && errorMessage->isEmpty()) {
        *errorMessage = memberError;
      }
    }
  }
  return true;
}

void ReminderScheduler::onActionInvoked(const uint notificationId,
                                        const QString& action) {
  const auto iterator = m_active.find(notificationId);
  if (iterator == m_active.end()) {
    return;
  }
  const PendingDelivery delivery = iterator.value();
  bool changed = false;
  if (action.startsWith(QStringLiteral("snooze")) &&
      delivery.kind == DeliveryKind::Reminder) {
    bool ok = false;
    const int minutes = action.sliced(6).toInt(&ok);
    changed = ok && m_database->snoozeReminderAt(delivery.reminderId, minutes, now());
  } else if (action == QStringLiteral("dismiss") &&
             delivery.kind == DeliveryKind::Reminder) {
    changed = m_database->dismissReminder(delivery.reminderId);
  } else if (action == QStringLiteral("default")) {
    if (m_linkOpener) {
      m_linkOpener(eventDeepLink(delivery));
    }
  }
  if (changed) {
    emit reminderStateChanged();
  }
  m_active.erase(iterator);
}

QUrl ReminderScheduler::eventDeepLink(const PendingDelivery& delivery) const {
  QUrl url;
  url.setScheme(QStringLiteral("omacalendar"));
  if (delivery.kind == DeliveryKind::InvitationDigest) {
    url.setHost(QStringLiteral("invitations"));
    return url;
  }
  url.setHost(QStringLiteral("event"));
  url.setPath(QLatin1Char('/') + delivery.eventId);
  if (!delivery.occurrenceId.isEmpty()) {
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("recurrenceId"), delivery.occurrenceId);
    url.setQuery(query);
  }
  return url;
}

void ReminderScheduler::onPrepareForSleep(const bool sleeping) {
  handlePrepareForSleep(sleeping);
}

}  // namespace omacalendar
