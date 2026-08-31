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
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace omacalendar {
namespace {

constexpr auto kDesktopEntry = "org.omacalendar.OmaCalendar";
constexpr int kReminderClaimLeaseSeconds = 2 * 60;

QString notificationPrivacy(const Database* database) {
  return database == nullptr ? QStringLiteral("generic")
                             : database
                                   ->setting(QStringLiteral("notificationPrivacy"),
                                             QStringLiteral("full_details"))
                                   .toString();
}

QString invitationFingerprint(const Event& event) {
  QList<QJsonObject> attendees;
  attendees.reserve(event.attendees.size());
  for (const QJsonValue& value : event.attendees) {
    QJsonObject attendee = value.toObject();
    // A local RSVP acknowledgement is not a remote invitation update.
    if (attendee.value(QStringLiteral("self")).toBool()) {
      attendee.remove(QStringLiteral("responseStatus"));
      attendee.remove(QStringLiteral("partstat"));
    }
    attendees.append(attendee);
  }
  std::sort(attendees.begin(), attendees.end(),
            [](const QJsonObject& first, const QJsonObject& second) {
              const auto identity = [](const QJsonObject& attendee) {
                return attendee.value(QStringLiteral("email"))
                    .toString(attendee.value(QStringLiteral("uri")).toString())
                    .toCaseFolded();
              };
              return identity(first) < identity(second);
            });
  QJsonArray encodedAttendees;
  for (const QJsonObject& attendee : std::as_const(attendees)) {
    encodedAttendees.append(attendee);
  }
  const QJsonObject canonical{
      {QStringLiteral("eventId"), event.id},
      {QStringLiteral("uid"), event.uid},
      {QStringLiteral("summary"), event.summary},
      {QStringLiteral("description"), event.description},
      {QStringLiteral("location"), event.location},
      {QStringLiteral("url"), event.url},
      {QStringLiteral("startUtc"), isoUtc(event.startUtc)},
      {QStringLiteral("endUtc"), isoUtc(event.endUtc)},
      {QStringLiteral("startDate"), event.startDate.toString(Qt::ISODate)},
      {QStringLiteral("endDate"), event.endDate.toString(Qt::ISODate)},
      {QStringLiteral("allDay"), event.allDay},
      {QStringLiteral("status"), event.status},
      {QStringLiteral("sequence"), event.sequence},
      {QStringLiteral("organizer"), event.organizer},
      {QStringLiteral("attendees"), encodedAttendees},
      {QStringLiteral("deleted"), event.deleted},
  };
  return QStringLiteral("invitation:") +
         QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(canonical).toJson(
                                                          QJsonDocument::Compact),
                                                      QCryptographicHash::Sha256)
                                 .toHex());
}

QString eventTitle(const Event& event) {
  return event.summary.trimmed().isEmpty() ? QStringLiteral("Calendar event")
                                           : event.summary;
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
  for (const ReminderJob& reminder : due) {
    deliver(reminder);
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
  scanInvitations(calendarIds);
  checkNow();
}

void ReminderScheduler::handlePrepareForSleep(const bool sleeping) {
  m_sleeping = sleeping;
  if (!sleeping) {
    const QStringList changedCalendars = std::exchange(m_deferredCalendarIds, {});
    QTimer::singleShot(0, this, [this, changedCalendars]() {
      if (!changedCalendars.isEmpty()) {
        scanInvitations(changedCalendars);
      }
      checkNow();
    });
  }
}

void ReminderScheduler::deliver(const ReminderJob& reminder) {
  const Event event = m_database->event(reminder.eventId);
  if (event.id.isEmpty() || event.deleted ||
      event.status.compare(QStringLiteral("cancelled"), Qt::CaseInsensitive) == 0) {
    if (m_database->dismissReminder(reminder.id)) {
      emit reminderStateChanged();
    }
    return;
  }
  if (!event.recurrenceId.isEmpty()) {
    QString seriesError;
    const bool seriesRemovalPending =
        m_database->hasPendingSeriesRemoval(event.calendarId, event.uid, &seriesError);
    if (!seriesError.isEmpty()) {
      emit notificationError(reminder.fingerprint, seriesError);
      return;
    }
    if (seriesRemovalPending) {
      // Keep the job pending: acknowledgement deletes the whole series and its
      // jobs atomically, while undo makes this same job deliverable again.
      return;
    }
  }
  const QDateTime claimedAt = now();
  const QDateTime leaseExpiresAt = claimedAt.addSecs(kReminderClaimLeaseSeconds);
  const QString deliveryToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
  bool claimed = false;
  QString error;
  if (!m_database->claimReminderDelivery(reminder.id, deliveryToken, claimedAt,
                                         leaseExpiresAt, &claimed, &error)) {
    emit notificationError(reminder.fingerprint, error);
    return;
  }
  if (!claimed) {
    return;
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
  for (const Event& event : invitations) {
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
  for (const Event& event : invitations) {
    if (event.dirty) {
      continue;
    }
    error.clear();
    const bool durableHistory =
        m_database->hasNotificationDeliveryForEvent(event.id, &error);
    if (!error.isEmpty()) {
      emit notificationError(QStringLiteral("invitation-history"), error);
      continue;
    }
    const QString fingerprint = invitationFingerprint(event);
    const bool baselineHistory = m_invitationBaseline.contains(event.id);
    if (!durableHistory && baselineHistory &&
        m_invitationBaseline.value(event.id) == fingerprint) {
      continue;
    }
    const bool changed = durableHistory || baselineHistory;
    const bool seen =
        m_database
            ->setting(QStringLiteral("invitation_seen_%1").arg(event.id), false, &error)
            .toBool();
    if (!error.isEmpty()) {
      emit notificationError(fingerprint, error);
      continue;
    }
    bool claimed = false;
    if (seen && !changed) {
      if (m_database->claimNotificationDelivery(
              fingerprint, QStringLiteral("invitation_seen"), event.id,
              event.localRevision, now(), &claimed, &error) &&
          claimed) {
        m_database->finishNotificationDelivery(fingerprint, now(), &error);
      }
      if (!error.isEmpty()) {
        emit notificationError(fingerprint, error);
      }
      continue;
    }
    if ((event.deleted ||
         event.status.compare(QStringLiteral("cancelled"), Qt::CaseInsensitive) == 0) &&
        !changed) {
      continue;
    }
    deliverInvitation(event, changed, fingerprint);
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
      notification.body += QStringLiteral(" — ") + event.location;
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
  if (privacy == QStringLiteral("full_details")) {
    notification.body = eventTitle(event);
    if (event.deleted ||
        event.status.compare(QStringLiteral("cancelled"), Qt::CaseInsensitive) == 0) {
      notification.summary = QStringLiteral("Invitation cancelled");
    }
    if (!event.location.isEmpty()) {
      notification.body += QStringLiteral(" — ") + event.location;
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
          : m_database->finishNotificationDelivery(delivery.fingerprint, now(),
                                                   &databaseError);
  // Keep the durable lease on persistence failure. Its expiry causes an
  // at-least-once retry. A process death after D-Bus accepted the notification
  // but before this acknowledgement can therefore duplicate one notification;
  // the stable fingerprint lets cooperating notification servers suppress it.
  if (!persisted) {
    emit notificationError(fingerprint, databaseError);
  }
  if (delivery.kind == DeliveryKind::Invitation) {
    m_invitationBaseline.insert(delivery.eventId, delivery.fingerprint);
  }
  m_active.insert(notificationId, delivery);
  emit reminderStateChanged();
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
