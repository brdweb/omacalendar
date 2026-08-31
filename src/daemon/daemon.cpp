#include "daemon.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QString>
#include <QTimeZone>
#include <algorithm>
#include <utility>

#include "core/domain.h"
#include "core/paths.h"
#include "core/recurrenceexpander.h"
#include "core/widgeteventquery.h"

namespace {

constexpr int kDefaultEventPageLimit = 500;
constexpr int kMaxEventPageLimit = 5000;
constexpr int kDefaultInvitationPageLimit = 500;
constexpr int kMaxInvitationPageLimit = 500;
constexpr int kWidgetInvitationLimit = 100;

QDateTime invitationStart(const omacalendar::Event& event) {
  return event.allDay ? QDateTime(event.startDate, QTime(0, 0), QTimeZone::UTC)
                      : event.startUtc;
}

QDateTime invitationEnd(const omacalendar::Event& event) {
  return event.allDay ? QDateTime(event.endDate, QTime(0, 0), QTimeZone::UTC)
                      : event.endUtc;
}

void sortInvitations(QList<omacalendar::Event>* events, const QDateTime& now) {
  std::stable_sort(
      events->begin(), events->end(), [&now](const auto& left, const auto& right) {
        const bool leftUpcoming = invitationEnd(left) > now;
        const bool rightUpcoming = invitationEnd(right) > now;
        if (leftUpcoming != rightUpcoming) {
          return leftUpcoming;
        }
        const QDateTime leftStart = invitationStart(left);
        const QDateTime rightStart = invitationStart(right);
        if (leftUpcoming && leftStart != rightStart) {
          return leftStart < rightStart;
        }
        if (!leftUpcoming && left.updatedAt != right.updatedAt) {
          return left.updatedAt > right.updatedAt;
        }
        if (leftStart != rightStart) {
          return leftUpcoming ? leftStart < rightStart : leftStart > rightStart;
        }
        if (left.id != right.id) {
          return left.id < right.id;
        }
        return left.recurrenceId < right.recurrenceId;
      });
}

QString normalizedInvitationResponse(QString response) {
  response = response.trimmed().toLower();
  response.remove(QLatin1Char('-'));
  response.remove(QLatin1Char('_'));
  return response;
}

bool invitationNeedsResponse(const omacalendar::Event& event,
                             const QString& accountPrincipal) {
  for (const QJsonValue& value : event.attendees) {
    const QJsonObject attendee = value.toObject();
    const QString email = attendee.value(QStringLiteral("email")).toString();
    if (!attendee.value(QStringLiteral("self")).toBool() &&
        (accountPrincipal.isEmpty() ||
         email.compare(accountPrincipal, Qt::CaseInsensitive) != 0)) {
      continue;
    }
    QString response = attendee.value(QStringLiteral("responseStatus")).toString();
    if (response.isEmpty()) {
      response = attendee.value(QStringLiteral("partstat")).toString();
    }
    const QString normalized = normalizedInvitationResponse(response);
    return normalized.isEmpty() || normalized == QStringLiteral("needsaction");
  }
  return false;
}

QString validateEventTimes(const omacalendar::Event& event) {
  if (event.allDay) {
    if (!event.startDate.isValid() || !event.endDate.isValid() ||
        event.endDate <= event.startDate) {
      return QStringLiteral(
          "All-day events require valid start and exclusive end dates");
    }
    return {};
  }
  if (!event.startUtc.isValid() || !event.endUtc.isValid() ||
      event.endUtc <= event.startUtc) {
    return QStringLiteral("Timed events require a valid end later than their start");
  }
  return {};
}

QJsonObject normalizedEventDraft(QJsonObject object) {
  const auto copyAlias = [&object](const QString& canonical, const QString& alias) {
    if (!object.contains(canonical) && object.contains(alias)) {
      object.insert(canonical, object.value(alias));
    }
  };
  copyAlias(QStringLiteral("summary"), QStringLiteral("title"));
  copyAlias(QStringLiteral("description"), QStringLiteral("notes"));
  copyAlias(QStringLiteral("startUtc"), QStringLiteral("start"));
  copyAlias(QStringLiteral("endUtc"), QStringLiteral("end"));
  copyAlias(QStringLiteral("timeKind"), QStringLiteral("timeMode"));
  return object;
}

QString eventReferenceId(const QJsonObject& params) {
  const QString direct = params.value(QStringLiteral("eventId")).toString();
  return direct.isEmpty() ? params.value(QStringLiteral("eventRef"))
                                .toObject()
                                .value(QStringLiteral("eventId"))
                                .toString()
                          : direct;
}

QString recurrenceReference(const QJsonObject& params) {
  const QJsonObject reference = params.value(QStringLiteral("eventRef")).toObject();
  return reference.value(QStringLiteral("recurrenceId"))
      .toString(reference.value(QStringLiteral("occurrenceId")).toString());
}

QString normalizedRecurrenceScope(QString scope) {
  if (scope == QStringLiteral("this_occurrence")) {
    scope = QStringLiteral("occurrence");
  } else if (scope == QStringLiteral("this_and_future")) {
    scope = QStringLiteral("future");
  } else if (scope == QStringLiteral("entire_series") || scope.isEmpty()) {
    scope = QStringLiteral("series");
  }
  return scope;
}

QString resourceRemoteId(const QString& remoteId) {
  const qsizetype separator = remoteId.indexOf(QLatin1Char('#'));
  return separator < 0 ? remoteId : remoteId.left(separator);
}

QJsonArray subscriptionTopics(const QJsonObject& params,
                              omacalendar::ipc::Error* error) {
  const QJsonValue value = params.value(QStringLiteral("topics"));
  if (value.isUndefined() || value.isNull()) {
    return QJsonArray{QStringLiteral("*")};
  }
  if (!value.isArray()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("topics must be an array of topic names"), false};
    }
    return {};
  }

  const QJsonArray requested = value.toArray();
  if (requested.size() > 32) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("At most 32 subscription topics are allowed"), false};
    }
    return {};
  }
  static const QSet<QString> families = {
      QStringLiteral("accounts"),    QStringLiteral("calendarSets"),
      QStringLiteral("calendars"),   QStringLiteral("conflicts"),
      QStringLiteral("events"),      QStringLiteral("google"),
      QStringLiteral("invitations"), QStringLiteral("operations"),
      QStringLiteral("reminders"),   QStringLiteral("sync"),
      QStringLiteral("system"),      QStringLiteral("widget"),
  };
  QJsonArray normalized;
  QSet<QString> seen;
  for (const QJsonValue& entry : requested) {
    if (!entry.isString()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_params"),
                  QStringLiteral("Every subscription topic must be a string"), false};
      }
      return {};
    }
    const QString topic = entry.toString().trimmed();
    const qsizetype separator = topic.indexOf(QLatin1Char('.'));
    const QString family = separator < 0 ? topic : topic.left(separator);
    const bool validSuffix =
        separator < 0 || (separator + 1 < topic.size() &&
                          topic.indexOf(QLatin1Char('.'), separator + 1) < 0);
    if (topic != QStringLiteral("*") &&
        (!families.contains(family) || !validSuffix || topic.size() > 64)) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_params"),
                  QStringLiteral("Unsupported subscription topic: %1").arg(topic),
                  false};
      }
      return {};
    }
    if (!seen.contains(topic)) {
      seen.insert(topic);
      normalized.append(topic);
    }
  }
  return normalized;
}

QString recurrencePayload(QString recurrenceId) {
  recurrenceId = recurrenceId.trimmed();
  if ((recurrenceId.startsWith(QStringLiteral("TZID="), Qt::CaseInsensitive) ||
       recurrenceId.startsWith(QStringLiteral("RANGE="), Qt::CaseInsensitive))) {
    const qsizetype separator = recurrenceId.indexOf(QLatin1Char(':'));
    if (separator >= 0) {
      recurrenceId = recurrenceId.sliced(separator + 1);
    }
  }
  return recurrenceId;
}

QDate recurrenceDate(QString recurrenceId) {
  recurrenceId = recurrencePayload(std::move(recurrenceId));
  QDate date = QDate::fromString(recurrenceId, Qt::ISODate);
  if (!date.isValid() && recurrenceId.size() == 8) {
    date = QDate::fromString(recurrenceId, QStringLiteral("yyyyMMdd"));
  }
  return date;
}

QDateTime recurrenceDateTime(QString recurrenceId, const QString& timeZone) {
  recurrenceId = recurrencePayload(std::move(recurrenceId));
  QDateTime result = omacalendar::dateTimeFromIso(recurrenceId);
  if (result.isValid()) {
    return result;
  }
  const bool utc = recurrenceId.endsWith(QLatin1Char('Z'), Qt::CaseInsensitive);
  result =
      QDateTime::fromString(recurrenceId, utc ? QStringLiteral("yyyyMMdd'T'HHmmss'Z'")
                                              : QStringLiteral("yyyyMMdd'T'HHmmss"));
  if (!result.isValid()) {
    return {};
  }
  const QTimeZone zone = utc ? QTimeZone(QTimeZone::UTC) : QTimeZone(timeZone.toUtf8());
  return QDateTime(result.date(), result.time(),
                   zone.isValid() ? zone : QTimeZone::systemTimeZone())
      .toUTC();
}

bool rebaseOccurrenceDraftOntoMaster(omacalendar::Event* draft,
                                     const omacalendar::Event& master,
                                     QString* errorMessage) {
  if (draft == nullptr || draft->recurrenceId.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("The selected occurrence has no identity");
    }
    return false;
  }
  if (draft->allDay != master.allDay) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral(
          "Changing a whole series between timed and all-day from "
          "one occurrence is not supported");
    }
    return false;
  }
  if (master.allDay) {
    const QDate original = recurrenceDate(draft->recurrenceId);
    if (!original.isValid() || !draft->startDate.isValid()) {
      if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("The occurrence date is invalid");
      }
      return false;
    }
    const qint64 delta = original.daysTo(draft->startDate);
    const qint64 duration =
        draft->endDate.isValid()
            ? qMax<qint64>(1, draft->startDate.daysTo(draft->endDate))
            : qMax<qint64>(1, master.startDate.daysTo(master.endDate));
    draft->startDate = master.startDate.addDays(delta);
    draft->endDate = draft->startDate.addDays(duration);
  } else {
    const QDateTime original =
        recurrenceDateTime(draft->recurrenceId, master.startTimeZone);
    if (!original.isValid() || !draft->startUtc.isValid()) {
      if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("The occurrence time is invalid");
      }
      return false;
    }
    const qint64 delta = original.msecsTo(draft->startUtc);
    const qint64 duration = draft->endUtc.isValid()
                                ? draft->startUtc.msecsTo(draft->endUtc)
                                : master.startUtc.msecsTo(master.endUtc);
    draft->startUtc = master.startUtc.addMSecs(delta);
    draft->endUtc = draft->startUtc.addMSecs(qMax<qint64>(1, duration));
  }
  draft->id = master.id;
  draft->calendarId = master.calendarId;
  draft->remoteId = master.remoteId;
  draft->uid = master.uid;
  draft->etag = master.etag;
  draft->recurrenceId.clear();
  draft->recurrenceRule = master.recurrenceRule;
  draft->rawPayload = master.rawPayload;
  draft->rawFormat = master.rawFormat;
  draft->createdAt = master.createdAt;
  draft->localRevision = master.localRevision;
  return true;
}

omacalendar::Event detachedOccurrence(const omacalendar::Event& master,
                                      QString recurrenceId) {
  omacalendar::Event event = master;
  event.id = omacalendar::newUuid();
  event.remoteId = resourceRemoteId(master.remoteId);
  event.recurrenceId = std::move(recurrenceId);
  if (!event.remoteId.isEmpty()) {
    event.remoteId += QLatin1Char('#') + event.recurrenceId;
  }
  event.recurrenceRule.clear();
  event.createdAt = {};
  event.updatedAt = {};
  event.localRevision = 0;
  event.dirty = false;
  event.deleted = false;
  if (event.allDay) {
    const QDate occurrence = recurrenceDate(event.recurrenceId);
    const qint64 duration = qMax<qint64>(1, master.startDate.daysTo(master.endDate));
    event.startDate = occurrence;
    event.endDate = occurrence.addDays(duration);
  } else {
    const QDateTime occurrence =
        recurrenceDateTime(event.recurrenceId, master.startTimeZone);
    const qint64 duration = master.startUtc.msecsTo(master.endUtc);
    event.startUtc = occurrence;
    event.endUtc = occurrence.addMSecs(qMax<qint64>(1, duration));
  }
  return event;
}

}  // namespace

namespace omacalendar {

Daemon::Daemon(QObject* parent)
    : QObject(parent),
      m_server(&m_router, this),
      m_google(&m_database, this),
      m_caldav(&m_database, this),
      m_ics(&m_database, this),
      m_sync(&m_database, &m_google, &m_caldav, &m_ics, this),
      m_reminders(&m_database, this) {
  registerHandlers();
  connect(&m_google, &google::GoogleSync::authorizationUrlReady, this,
          [this](const QString& accountId, const QUrl& url) {
            m_server.broadcast(QStringLiteral("google.oauthUrl"),
                               {{QStringLiteral("accountId"), accountId},
                                {QStringLiteral("url"), url.toString()}});
          });
  const auto accountChanged = [this](const QString& accountId) {
    m_server.broadcast(QStringLiteral("accounts.changed"),
                       {{QStringLiteral("accountId"), accountId}});
  };
  const auto calendarsChanged = [this](const QString& accountId) {
    m_server.broadcast(QStringLiteral("calendars.changed"),
                       {{QStringLiteral("accountId"), accountId}});
  };
  const auto eventsChanged = [this](const QStringList& calendarIds) {
    emitEventsChanged(calendarIds);
  };
  const auto syncChanged = [this](const QString& accountId, const QJsonObject& status) {
    if (status.value(QStringLiteral("state")).toString() == QStringLiteral("idle")) {
      bool queuedLocalWrites = false;
      QString error;
      const int resolved =
          m_database.resolveNewestConflicts(&queuedLocalWrites, &error);
      if (resolved > 0) {
        emitEventsChanged({});
        m_server.broadcast(QStringLiteral("conflicts.changed"),
                           {{QStringLiteral("revision"), m_database.changeRevision()}});
        if (queuedLocalWrites) {
          QTimer::singleShot(0, this,
                             [this, accountId]() { m_sync.syncAccount(accountId); });
        }
      } else if (resolved < 0) {
        qWarning() << "Unable to resolve newest calendar conflict:" << error;
      }
    }
    m_server.broadcast(
        QStringLiteral("sync.statusChanged"),
        {{QStringLiteral("accountId"), accountId}, {QStringLiteral("status"), status}});
  };
  connect(&m_sync, &SyncCoordinator::accountChanged, this, accountChanged);
  connect(&m_sync, &SyncCoordinator::calendarsChanged, this, calendarsChanged);
  connect(&m_sync, &SyncCoordinator::syncStatusChanged, this, syncChanged);
  connect(&m_sync, &SyncCoordinator::eventsChanged, this, eventsChanged);
  connect(&m_sync, &SyncCoordinator::operationStateChanged, this, [this]() {
    m_server.broadcast(QStringLiteral("operations.changed"),
                       {{QStringLiteral("revision"), m_database.changeRevision()}});
  });
  connect(&m_reminders, &ReminderScheduler::reminderStateChanged, this, [this]() {
    m_server.broadcast(QStringLiteral("reminders.changed"),
                       {{QStringLiteral("revision"), m_database.changeRevision()}});
  });
}

bool Daemon::start(QString* errorMessage, const qintptr socketDescriptor) {
  if (!paths::ensureDirectories(errorMessage)) {
    return false;
  }
  if (!m_database.open(paths::databaseFile(), errorMessage)) {
    return false;
  }
  if (!m_database.recoverExpiredOutbox(errorMessage)) {
    return false;
  }
  QString socketError;
  const bool listening =
      socketDescriptor >= 0
          ? m_server.listen(socketDescriptor, paths::socketFile(), &socketError)
          : m_server.listen(paths::socketFile(), &socketError);
  if (!listening) {
    if (errorMessage != nullptr) {
      *errorMessage = socketError;
    }
    return false;
  }
  QString restoreError;
  if (!m_google.restoreAccounts(&restoreError)) {
    qWarning().noquote() << "Google account restore failed:" << restoreError;
  }
  restoreError.clear();
  if (!m_caldav.restoreAccounts(&restoreError)) {
    qWarning().noquote() << "CalDAV account restore failed:" << restoreError;
  }
  m_reminders.start();
  m_sync.start();
  return true;
}

QString Daemon::socketPath() const { return m_server.path(); }

int Daemon::connectedClients() const { return m_server.clientCount(); }

QStringList Daemon::methods() const { return m_router.methods(); }

void Daemon::registerHandlers() {
  m_router.registerHandler(QStringLiteral("system.ping"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onSystemPing(params, error);
                           });
  m_router.registerHandler(QStringLiteral("system.info"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onSystemInfo(params, error);
                           });
  m_router.registerHandler(QStringLiteral("system.health"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onSystemHealth(params, error);
                           });
  m_router.registerHandler(QStringLiteral("system.subscribe"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onSystemSubscribe(params, error);
                           });
  m_router.registerHandler(QStringLiteral("accounts.list"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onAccountsList(params, error);
                           });
  m_router.registerHandler(QStringLiteral("accounts.reauthorize"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onAccountsReauthorize(params, error);
                           });
  m_router.registerHandler(QStringLiteral("accounts.addGoogle"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onGoogleOauthStart(params, error);
                           });
  m_router.registerHandler(QStringLiteral("accounts.update"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onAccountsUpdate(params, error);
                           });
  m_router.registerHandler(QStringLiteral("accounts.disconnect"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onAccountsDisconnect(params, error);
                           });
  m_router.registerHandler(QStringLiteral("accounts.createCalDav"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onAccountsCreateCalDav(params, error);
                           });
  m_router.registerHandler(QStringLiteral("accounts.addCalDav"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onAccountsCreateCalDav(params, error);
                           });
  m_router.registerHandler(QStringLiteral("accounts.addIcs"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onAccountsAddIcs(params, error);
                           });
  m_router.registerHandler(QStringLiteral("accounts.remove"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onAccountsRemove(params, error);
                           });
  m_router.registerHandler(QStringLiteral("accounts.test"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onAccountsTest(params, error);
                           });
  m_router.registerHandler(QStringLiteral("calendars.list"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onCalendarsList(params, error);
                           });
  m_router.registerHandler(QStringLiteral("calendars.upsert"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onCalendarsUpsert(params, error);
                           });
  m_router.registerHandler(QStringLiteral("calendars.remove"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onCalendarsRemove(params, error);
                           });
  m_router.registerHandler(QStringLiteral("calendars.updatePreferences"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onCalendarsUpdatePreferences(params, error);
                           });
  m_router.registerHandler(QStringLiteral("calendarSets.list"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onCalendarSetsList(params, error);
                           });
  m_router.registerHandler(QStringLiteral("calendarSets.upsert"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onCalendarSetsUpsert(params, error);
                           });
  m_router.registerHandler(QStringLiteral("calendarSets.remove"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onCalendarSetsRemove(params, error);
                           });
  m_router.registerHandler(QStringLiteral("calendarSets.activate"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onCalendarSetsActivate(params, error);
                           });
  m_router.registerHandler(QStringLiteral("events.list"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onEventsList(params, error);
                           });
  m_router.registerHandler(QStringLiteral("events.get"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onEventsGet(params, error);
                           });
  m_router.registerHandler(QStringLiteral("events.create"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onEventsCreate(params, error);
                           });
  m_router.registerHandler(QStringLiteral("events.update"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onEventsUpdate(params, error);
                           });
  m_router.registerHandler(QStringLiteral("events.remove"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onEventsRemove(params, error);
                           });
  m_router.registerHandler(QStringLiteral("events.undo"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onEventsUndo(params, error);
                           });
  m_router.registerHandler(QStringLiteral("events.search"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onEventsSearch(params, error);
                           });
  m_router.registerHandler(QStringLiteral("events.move"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onEventsMove(params, error);
                           });
  m_router.registerHandler(QStringLiteral("events.respond"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onEventsRespond(params, error);
                           });
  m_router.registerHandler(QStringLiteral("invitations.list"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onInvitationsList(params, error);
                           });
  m_router.registerHandler(QStringLiteral("invitations.markSeen"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onInvitationsMarkSeen(params, error);
                           });
  m_router.registerHandler(QStringLiteral("conflicts.list"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onConflictsList(params, error);
                           });
  m_router.registerHandler(QStringLiteral("conflicts.resolve"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onConflictsResolve(params, error);
                           });
  m_router.registerHandler(QStringLiteral("settings.get"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onSettingsGet(params, error);
                           });
  m_router.registerHandler(QStringLiteral("settings.set"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onSettingsSet(params, error);
                           });
  m_router.registerHandler(QStringLiteral("outbox.list"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onOutboxList(params, error);
                           });
  m_router.registerHandler(QStringLiteral("outbox.retry"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onOutboxRetry(params, error);
                           });
  m_router.registerHandler(QStringLiteral("operations.list"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onOutboxList(params, error);
                           });
  m_router.registerHandler(QStringLiteral("operations.retry"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onOutboxRetry(params, error);
                           });
  m_router.registerHandler(QStringLiteral("operations.discard"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onOperationsDiscard(params, error);
                           });
  m_router.registerHandler(QStringLiteral("reminders.list"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onRemindersList(params, error);
                           });
  m_router.registerHandler(QStringLiteral("reminders.snooze"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onRemindersSnooze(params, error);
                           });
  m_router.registerHandler(QStringLiteral("reminders.dismiss"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onRemindersDismiss(params, error);
                           });
  m_router.registerHandler(QStringLiteral("ics.refresh"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onIcsRefresh(params, error);
                           });
  m_router.registerHandler(QStringLiteral("ics.status"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onIcsStatus(params, error);
                           });
  m_router.registerHandler(QStringLiteral("import.preview"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onImportPreview(params, error);
                           });
  m_router.registerHandler(QStringLiteral("import.commit"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onImportCommit(params, error);
                           });
  m_router.registerHandler(QStringLiteral("export.create"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onExport(params, error);
                           });
  m_router.registerHandler(QStringLiteral("export.run"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onExport(params, error);
                           });
  m_router.registerHandler(QStringLiteral("google.configureClient"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onGoogleConfigureClient(params, error);
                           });
  m_router.registerHandler(QStringLiteral("google.oauthStart"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onGoogleOauthStart(params, error);
                           });
  m_router.registerHandler(QStringLiteral("google.oauthCancel"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onGoogleOauthCancel(params, error);
                           });
  m_router.registerHandler(QStringLiteral("google.disconnect"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onGoogleDisconnect(params, error);
                           });
  m_router.registerHandler(QStringLiteral("sync.all"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onSyncAll(params, error);
                           });
  m_router.registerHandler(QStringLiteral("sync.account"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onSyncAccount(params, error);
                           });
  m_router.registerHandler(QStringLiteral("sync.status"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onSyncStatus(params, error);
                           });
  m_router.registerHandler(QStringLiteral("sync.calendar"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onSyncCalendar(params, error);
                           });
  m_router.registerHandler(QStringLiteral("widget.snapshot"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onWidgetSnapshot(params, error);
                           });
}

bool Daemon::validateRequiredString(const QJsonObject& params, const QString& key,
                                    QString* value, ipc::Error* error) const {
  if (!params.contains(key) || !params.value(key).isString()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("%1 must be a string").arg(key), false};
    }
    return false;
  }
  const QString normalized = params.value(key).toString().trimmed();
  if (normalized.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("%1 must be non-empty").arg(key), false};
    }
    return false;
  }
  if (value != nullptr) {
    *value = normalized;
  }
  return true;
}

QStringList Daemon::parseCalendarIds(const QJsonValue& value, ipc::Error* error) {
  if (value.isUndefined() || value.isNull()) {
    return {};
  }
  if (value.isString()) {
    const QString id = value.toString().trimmed();
    if (!id.isEmpty()) {
      return {id};
    }
    return {};
  }
  if (!value.isArray()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("calendarIds must be a string or array of strings"),
                false};
    }
    return {};
  }
  QStringList result;
  const QJsonArray array = value.toArray();
  for (const QJsonValue& entry : array) {
    if (!entry.isString()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_params"),
                  QStringLiteral("calendarIds entries must be strings"), false};
      }
      return {};
    }
    const QString id = entry.toString().trimmed();
    if (!id.isEmpty()) {
      result.append(id);
    }
  }
  return result;
}

void Daemon::emitEventsChanged(const QStringList& calendarIds) {
  m_reminders.eventsChanged(calendarIds);
  QJsonArray list;
  for (const QString& id : calendarIds) {
    list.append(id);
  }
  const QJsonObject data{
      {QStringLiteral("calendarIds"), list},
      {QStringLiteral("revision"), m_database.changeRevision()},
  };
  m_server.broadcast(QStringLiteral("events.changed"), data);
}

QJsonValue Daemon::onSystemPing(const QJsonObject&, ipc::Error*) const {
  return QJsonObject{
      {QStringLiteral("ok"), true},
      {QStringLiteral("protocolMajor"), kIpcProtocolMajor},
      {QStringLiteral("protocolMinor"), kIpcProtocolMinor},
  };
}

QJsonValue Daemon::onSystemInfo(const QJsonObject&, ipc::Error*) const {
  QJsonArray methods;
  for (const QString& method : m_router.methods()) {
    methods.append(method);
  }
  return QJsonObject{
      {QStringLiteral("server"), QStringLiteral("omacalendard")},
      {QStringLiteral("version"), QStringLiteral(OMACALENDAR_VERSION)},
      {QStringLiteral("schemaVersion"), m_database.schemaVersion()},
      {QStringLiteral("protocolMajor"), kIpcProtocolMajor},
      {QStringLiteral("protocolMinor"), kIpcProtocolMinor},
      {QStringLiteral("methods"), methods},
      {QStringLiteral("providers"),
       QJsonObject{
           {QStringLiteral("caldav"), QJsonObject{{QStringLiteral("available"), true}}},
           {QStringLiteral("google"),
            QJsonObject{{QStringLiteral("available"), true},
                        {QStringLiteral("configured"), m_google.isConfigured()}}},
           {QStringLiteral("local"), QJsonObject{{QStringLiteral("available"), true},
                                                 {QStringLiteral("writable"), true}}},
           {QStringLiteral("ics"), QJsonObject{{QStringLiteral("available"), true},
                                               {QStringLiteral("writable"), false}}},
       }},
      {QStringLiteral("revision"), m_database.changeRevision()},
  };
}

QJsonValue Daemon::onSystemHealth(const QJsonObject&, ipc::Error*) const {
  const QList<OutboxItem> operations = m_database.outboxItems(500);
  int pending = 0;
  int blocked = 0;
  for (const OutboxItem& operation : operations) {
    if (operation.state == OutboxState::Blocked) {
      ++blocked;
    } else if (operation.state != OutboxState::Done) {
      ++pending;
    }
  }
  return QJsonObject{
      {QStringLiteral("ok"), m_database.isOpen() && m_server.isListening()},
      {QStringLiteral("database"), m_database.isOpen()},
      {QStringLiteral("socket"), m_server.isListening()},
      {QStringLiteral("connectedClients"), m_server.clientCount()},
      {QStringLiteral("pendingOperations"), pending},
      {QStringLiteral("blockedOperations"), blocked},
      {QStringLiteral("unresolvedConflicts"),
       static_cast<int>(m_database.conflicts(true).size())},
      {QStringLiteral("revision"), m_database.changeRevision()},
  };
}

QJsonValue Daemon::onSystemSubscribe(const QJsonObject& params,
                                     ipc::Error* error) const {
  const QJsonArray topics = subscriptionTopics(params, error);
  if (error != nullptr && !error->code.isEmpty()) {
    return {};
  }
  const qint64 since = params.value(QStringLiteral("sinceRevision")).toInteger(-1);
  const qint64 current = m_database.changeRevision();
  return QJsonObject{
      {QStringLiteral("subscribed"), true},
      {QStringLiteral("topics"), topics},
      {QStringLiteral("revision"), current},
      {QStringLiteral("catchUpRequired"), since >= 0 && since < current},
      {QStringLiteral("protocolMajor"), kIpcProtocolMajor},
      {QStringLiteral("protocolMinor"), kIpcProtocolMinor},
  };
}

QJsonValue Daemon::onAccountsList(const QJsonObject&, ipc::Error* error) {
  QString databaseError;
  const QList<Account> accounts = m_database.accounts(&databaseError);
  if (!databaseError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), databaseError, false};
    }
    return {};
  }
  QJsonArray result;
  for (const Account& account : accounts) {
    result.append(toJson(account));
  }
  return QJsonObject{
      {QStringLiteral("accounts"), result},
      {QStringLiteral("count"), static_cast<int>(result.size())},
  };
}

QJsonValue Daemon::onAccountsReauthorize(const QJsonObject& params, ipc::Error* error) {
  QString accountId;
  if (!validateRequiredString(params, QStringLiteral("accountId"), &accountId, error)) {
    return {};
  }
  QString message;
  const Account account = m_database.account(accountId, &message);
  if (account.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Account not found"),
                false};
    }
    return {};
  }
  if (account.provider != ProviderKind::Google) {
    if (error != nullptr) {
      *error = {QStringLiteral("unsupported"),
                QStringLiteral("Interactive reauthorization is currently "
                               "available for Google accounts"),
                false};
    }
    return {};
  }
  if (!m_google.reauthorizeAccount(accountId, &message)) {
    if (error != nullptr) {
      *error = {QStringLiteral("reauthorization_start_failed"), message.left(300),
                false};
    }
    return {};
  }
  return QJsonObject{
      {QStringLiteral("accountId"), accountId},
      {QStringLiteral("state"), QStringLiteral("waiting_for_browser")},
      {QStringLiteral("revision"), m_database.changeRevision()},
  };
}

QJsonValue Daemon::onAccountsCreateCalDav(const QJsonObject& params,
                                          ipc::Error* error) {
  const QString endpoint = params.value(QStringLiteral("endpoint")).toString();
  const QString username = params.value(QStringLiteral("username")).toString();
  const QString password = params.value(QStringLiteral("password")).toString();
  const QString displayName = params.value(QStringLiteral("displayName")).toString();
  if (endpoint.isEmpty() || username.isEmpty() || password.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("endpoint, username, and password are required"), false};
    }
    return {};
  }
  QString message;
  const QString accountId =
      m_caldav.createAccount(endpoint, username, password, displayName, &message);
  if (accountId.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("caldav_setup_failed"), message, false};
    }
    return {};
  }
  return toJson(m_database.account(accountId));
}

QJsonValue Daemon::onAccountsUpdate(const QJsonObject& params, ipc::Error* error) {
  QString accountId;
  if (!validateRequiredString(params, QStringLiteral("accountId"), &accountId, error)) {
    return {};
  }
  QString message;
  Account account = m_database.account(accountId, &message);
  if (account.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Account not found"),
                false};
    }
    return {};
  }

  const bool credentialsSupplied = params.contains(QStringLiteral("username")) ||
                                   params.contains(QStringLiteral("password"));
  if (credentialsSupplied) {
    const QString username = params.value(QStringLiteral("username")).toString();
    const QString password = params.value(QStringLiteral("password")).toString();
    bool accepted = false;
    if (account.provider == ProviderKind::Ics) {
      accepted = m_ics.updateCredentials(accountId, username, password, &message);
    } else if (account.provider == ProviderKind::CalDav) {
      accepted = m_caldav.updateCredentials(accountId, username, password, &message);
    } else {
      if (error != nullptr) {
        *error = {
            QStringLiteral("unsupported"),
            account.provider == ProviderKind::Google
                ? QStringLiteral("Use accounts.reauthorize for Google accounts")
                : QStringLiteral("This account does not use replaceable credentials"),
            false};
      }
      return {};
    }
    if (!accepted) {
      if (error != nullptr) {
        *error = {QStringLiteral("account_update_failed"), message.left(300), false};
      }
      return {};
    }
  }

  if (params.contains(QStringLiteral("displayName"))) {
    account = m_database.account(accountId, &message);
    const QString displayName =
        params.value(QStringLiteral("displayName")).toString().trimmed();
    if (displayName.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_params"),
                  QStringLiteral("displayName cannot be empty"), false};
      }
      return {};
    }
    account.displayName = displayName;
    if (!m_database.upsertAccount(account, &message)) {
      if (error != nullptr) {
        *error = {QStringLiteral("database_error"), message.left(300), false};
      }
      return {};
    }
  }
  return QJsonObject{{QStringLiteral("account"), toJson(m_database.account(accountId))},
                     {QStringLiteral("accepted"), true},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onAccountsDisconnect(const QJsonObject& params, ipc::Error* error) {
  QJsonObject disconnectParams = params;
  disconnectParams.insert(QStringLiteral("removeCachedData"), false);
  return onAccountsRemove(disconnectParams, error);
}

QJsonValue Daemon::onAccountsAddIcs(const QJsonObject& params, ipc::Error* error) {
  QString url;
  if (!validateRequiredString(params, QStringLiteral("url"), &url, error)) {
    return {};
  }
  const int refreshSeconds = params.value(QStringLiteral("refreshSeconds")).toInt(3600);
  if (refreshSeconds < 60) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("refreshSeconds must be at least 60"), false};
    }
    return {};
  }
  Account account;
  QString message;
  if (!m_ics.addSubscription(
          url, params.value(QStringLiteral("displayName")).toString(), refreshSeconds,
          params.value(QStringLiteral("username")).toString(),
          params.value(QStringLiteral("password")).toString(), &account, &message)) {
    if (error != nullptr) {
      *error = {QStringLiteral("ics_setup_failed"), message.left(300), false};
    }
    return {};
  }
  return QJsonObject{{QStringLiteral("account"), toJson(account)},
                     {QStringLiteral("refreshing"), true},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onAccountsRemove(const QJsonObject& params, ipc::Error* error) {
  QString accountId;
  if (!validateRequiredString(params, QStringLiteral("accountId"), &accountId, error)) {
    return {};
  }
  QString dbError;
  const Account account = m_database.account(accountId, &dbError);
  if (account.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Account not found"),
                false};
    }
    return {};
  }
  if (account.provider == ProviderKind::Local) {
    if (error != nullptr) {
      *error = {QStringLiteral("unsupported"),
                QStringLiteral("The device-only account cannot be removed"), false};
    }
    return {};
  }
  const bool removeCachedData =
      params.value(QStringLiteral("removeCachedData")).toBool(true);
  bool removed = false;
  if (account.provider == ProviderKind::Google) {
    removed = m_google.disconnectAccount(accountId, removeCachedData, &dbError);
  } else if (account.provider == ProviderKind::CalDav) {
    removed = m_caldav.disconnectAccount(accountId, removeCachedData, &dbError);
  } else if (account.provider == ProviderKind::Ics) {
    removed = m_ics.disconnectAccount(accountId, removeCachedData, &dbError);
  } else {
    removed = m_database.removeAccount(accountId, &dbError);
  }
  if (!removed) {
    if (error != nullptr) {
      *error = {QStringLiteral("account_remove_failed"), dbError, false};
    }
    return {};
  }
  return QJsonObject{
      {QStringLiteral("ok"), true},
      {QStringLiteral("accountId"), accountId},
      {QStringLiteral("cachedDataRemoved"), removeCachedData},
  };
}

QJsonValue Daemon::onAccountsTest(const QJsonObject& params, ipc::Error* error) {
  QString accountId;
  if (!validateRequiredString(params, QStringLiteral("accountId"), &accountId, error)) {
    return {};
  }
  QString message;
  const Account account = m_database.account(accountId, &message);
  if (account.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Account not found"),
                false};
    }
    return {};
  }
  if (account.provider == ProviderKind::Ics) {
    if (!m_ics.refresh(accountId, &message)) {
      if (error != nullptr) {
        *error = {QStringLiteral("sync_failed"), message.left(300), false};
      }
      return {};
    }
  } else {
    m_sync.syncAccount(accountId);
  }
  return QJsonObject{{QStringLiteral("started"), true},
                     {QStringLiteral("accountId"), accountId}};
}

QJsonValue Daemon::onCalendarsList(const QJsonObject& params, ipc::Error* error) {
  QString accountId;
  if (params.contains(QStringLiteral("accountId")) &&
      params.value(QStringLiteral("accountId")).isString()) {
    accountId = params.value(QStringLiteral("accountId")).toString().trimmed();
  }
  QString dbError;
  const QList<Calendar> calendars = m_database.calendars(accountId, &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  QJsonArray result;
  for (const Calendar& calendar : calendars) {
    if (calendar.capabilities.value(QStringLiteral("deleted")).toBool() ||
        calendar.capabilities.value(QStringLiteral("calendarListRemoved")).toBool()) {
      continue;
    }
    result.append(toJson(calendar));
  }
  return QJsonObject{
      {QStringLiteral("calendars"), result},
      {QStringLiteral("count"), static_cast<int>(result.size())},
  };
}

QJsonValue Daemon::onCalendarsUpsert(const QJsonObject& params, ipc::Error* error) {
  const QJsonObject calendarPayload =
      params.value(QStringLiteral("calendar")).isObject()
          ? params.value(QStringLiteral("calendar")).toObject()
          : params;
  if (calendarPayload.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("calendar is required"), false};
    }
    return {};
  }
  Calendar calendar = calendarFromJson(calendarPayload);
  if (calendar.accountId.trimmed().isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("accountId is required"), false};
    }
    return {};
  }
  if (calendar.id.isEmpty()) {
    calendar.id = newUuid();
  }
  QString dbError;
  const Account owner = m_database.account(calendar.accountId, &dbError);
  if (owner.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Account not found"),
                false};
    }
    return {};
  }
  if (owner.provider != ProviderKind::Local) {
    if (error != nullptr) {
      *error = {QStringLiteral("unsupported"),
                QStringLiteral("Remote calendar creation is provider-managed"), false};
    }
    return {};
  }
  calendar.readOnly = false;
  calendar.capabilities = {
      {QStringLiteral("provider"), QStringLiteral("local")},
      {QStringLiteral("createEvent"), true},
      {QStringLiteral("updateEvent"), true},
      {QStringLiteral("removeEvent"), true},
      {QStringLiteral("attendees"), false},
  };
  if (!m_database.upsertCalendar(calendar, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"),
                dbError.isEmpty() ? QStringLiteral("Unable to save calendar") : dbError,
                false};
    }
    return {};
  }
  const qint64 currentRevision = m_database.changeRevision();
  m_server.broadcast(QStringLiteral("calendars.changed"),
                     {{QStringLiteral("accountId"), calendar.accountId},
                      {QStringLiteral("calendarId"), calendar.id},
                      {QStringLiteral("revision"), currentRevision}});
  QJsonObject result = toJson(calendar);
  result.insert(QStringLiteral("revision"), currentRevision);
  return result;
}

QJsonValue Daemon::onCalendarsRemove(const QJsonObject& params, ipc::Error* error) {
  QString calendarId;
  if (!validateRequiredString(params, QStringLiteral("calendarId"), &calendarId,
                              error)) {
    return {};
  }
  QString dbError;
  const Calendar calendar = m_database.calendar(calendarId, &dbError);
  if (calendar.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Calendar not found"),
                false};
    }
    return {};
  }
  const Account owner = m_database.account(calendar.accountId, &dbError);
  if (owner.provider == ProviderKind::Local &&
      calendarId == QStringLiteral("local-default")) {
    if (error != nullptr) {
      *error = {QStringLiteral("protected_calendar"),
                QStringLiteral("The default device calendar cannot be removed"), false};
    }
    return {};
  }
  if (!params.value(QStringLiteral("confirmed")).toBool()) {
    if (error != nullptr) {
      *error = {
          QStringLiteral("confirmation_required"),
          QStringLiteral("Confirm permanent removal of this calendar and its events"),
          false};
    }
    return {};
  }
  if (owner.provider == ProviderKind::Google) {
    if (!canDeleteCalendar(calendar)) {
      if (error != nullptr) {
        *error = {
            QStringLiteral("protected_calendar"),
            QStringLiteral("Primary and non-owned Google calendars cannot be deleted"),
            false};
      }
      return {};
    }
    if (!m_google.deleteCalendar(
            calendar,
            [this, calendar](const bool success, const QString& code,
                             const QString& message) {
              if (!success) {
                qWarning().noquote()
                    << "Google calendar deletion failed:" << code << message;
                return;
              }
              QString finishError;
              if (!finishCalendarRemoval(calendar, &finishError)) {
                qWarning().noquote()
                    << "Unable to remove deleted Google calendar cache:" << finishError;
              }
            },
            &dbError)) {
      if (error != nullptr) {
        *error = {QStringLiteral("calendar_remove_failed"), dbError, false};
      }
      return {};
    }
    return QJsonObject{{QStringLiteral("accepted"), true},
                       {QStringLiteral("calendarId"), calendarId}};
  }
  if (owner.provider != ProviderKind::Local) {
    if (error != nullptr) {
      *error = {QStringLiteral("unsupported"),
                QStringLiteral("This provider does not support calendar deletion"),
                false};
    }
    return {};
  }
  if (!finishCalendarRemoval(calendar, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("calendar_remove_failed"), dbError, false};
    }
    return {};
  }
  const qint64 currentRevision = m_database.changeRevision();
  return QJsonObject{{QStringLiteral("removed"), true},
                     {QStringLiteral("calendarId"), calendarId},
                     {QStringLiteral("revision"), currentRevision}};
}

bool Daemon::finishCalendarRemoval(const Calendar& calendar, QString* errorMessage) {
  const QString previousDefault =
      m_database
          .setting(QStringLiteral("defaultCalendarId"), QStringLiteral("local-default"))
          .toString();
  if (!m_database.removeCalendarCache(calendar.id, errorMessage)) {
    return false;
  }
  if (previousDefault == calendar.id) {
    QString loadError;
    const QList<Calendar> remaining = m_database.calendars({}, &loadError);
    if (!loadError.isEmpty()) {
      if (errorMessage != nullptr) *errorMessage = loadError;
      return false;
    }
    QString fallback;
    for (const Calendar& candidate : remaining) {
      if (!candidate.enabled || candidate.readOnly ||
          candidate.capabilities.value(QStringLiteral("deleted")).toBool()) {
        continue;
      }
      if (fallback.isEmpty() ||
          candidate.capabilities.value(QStringLiteral("primary")).toBool()) {
        fallback = candidate.id;
      }
      if (candidate.capabilities.value(QStringLiteral("primary")).toBool()) break;
    }
    if (!m_database.setSetting(QStringLiteral("defaultCalendarId"), fallback,
                               errorMessage)) {
      return false;
    }
  }
  const qint64 revision = m_database.changeRevision();
  m_server.broadcast(QStringLiteral("calendars.changed"),
                     {{QStringLiteral("accountId"), calendar.accountId},
                      {QStringLiteral("calendarId"), calendar.id},
                      {QStringLiteral("removed"), true},
                      {QStringLiteral("revision"), revision}});
  return true;
}

QJsonValue Daemon::onCalendarsUpdatePreferences(const QJsonObject& params,
                                                ipc::Error* error) {
  QString calendarId;
  if (!validateRequiredString(params, QStringLiteral("calendarId"), &calendarId,
                              error)) {
    return {};
  }
  const Calendar current = m_database.calendar(calendarId);
  if (current.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Calendar not found"),
                false};
    }
    return {};
  }
  const bool enabled = params.value(QStringLiteral("enabled")).toBool(current.enabled);
  const QString colorOverride =
      params.value(QStringLiteral("colorOverride")).toString(current.colorOverride);
  const int position = params.value(QStringLiteral("position")).toInt(current.position);
  const bool ignoreAlerts =
      params.value(QStringLiteral("ignoreAlerts")).toBool(current.ignoreAlerts);
  QString dbError;
  if (!m_database.updateCalendarPreferences(calendarId, enabled, colorOverride,
                                            position, ignoreAlerts, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  m_server.broadcast(QStringLiteral("calendars.changed"),
                     {{QStringLiteral("accountId"), current.accountId},
                      {QStringLiteral("revision"), m_database.changeRevision()}});
  return toJson(m_database.calendar(calendarId));
}

QJsonValue Daemon::onCalendarSetsList(const QJsonObject&, ipc::Error* error) {
  QString dbError;
  const QList<CalendarSet> sets = m_database.calendarSets(&dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  QJsonArray encoded;
  for (const CalendarSet& set : sets) {
    encoded.append(toJson(set));
  }
  return QJsonObject{
      {QStringLiteral("calendarSets"), encoded},
      {QStringLiteral("activeId"), m_database
                                       .setting(QStringLiteral("active_calendar_set"),
                                                QStringLiteral("all-calendars"))
                                       .toString()},
      {QStringLiteral("revision"), m_database.changeRevision()},
  };
}

QJsonValue Daemon::onCalendarSetsUpsert(const QJsonObject& params, ipc::Error* error) {
  const QJsonObject payload =
      params.value(QStringLiteral("calendarSet")).isObject()
          ? params.value(QStringLiteral("calendarSet")).toObject()
          : params;
  CalendarSet set;
  set.id = payload.value(QStringLiteral("id")).toString();
  set.name = payload.value(QStringLiteral("name")).toString();
  set.defaultCalendarId = payload.value(QStringLiteral("defaultCalendarId")).toString();
  for (const QJsonValue& value :
       payload.value(QStringLiteral("calendarIds")).toArray()) {
    if (value.isString()) {
      set.calendarIds.append(value.toString());
    }
  }
  QString dbError;
  if (!m_database.upsertCalendarSet(&set, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  m_server.broadcast(QStringLiteral("calendarSets.changed"),
                     {{QStringLiteral("revision"), m_database.changeRevision()}});
  return toJson(set);
}

QJsonValue Daemon::onCalendarSetsRemove(const QJsonObject& params, ipc::Error* error) {
  QString setId;
  if (!validateRequiredString(params, QStringLiteral("calendarSetId"), &setId, error)) {
    return {};
  }
  QString dbError;
  if (!m_database.removeCalendarSet(setId, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  m_server.broadcast(QStringLiteral("calendarSets.changed"),
                     {{QStringLiteral("revision"), m_database.changeRevision()}});
  return QJsonObject{{QStringLiteral("removed"), true},
                     {QStringLiteral("calendarSetId"), setId}};
}

QJsonValue Daemon::onCalendarSetsActivate(const QJsonObject& params,
                                          ipc::Error* error) {
  QString setId;
  if (!validateRequiredString(params, QStringLiteral("calendarSetId"), &setId, error)) {
    return {};
  }
  QString dbError;
  if (!m_database.activateCalendarSet(setId, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  m_server.broadcast(QStringLiteral("calendarSets.changed"),
                     {{QStringLiteral("activeId"), setId},
                      {QStringLiteral("revision"), m_database.changeRevision()}});
  return QJsonObject{{QStringLiteral("activeId"), setId},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onEventsList(const QJsonObject& params, ipc::Error* error) {
  const QString startText = params.value(QStringLiteral("start")).toString();
  const QString endText = params.value(QStringLiteral("end")).toString();
  const QDateTime startUtc = dateTimeFromIso(startText);
  const QDateTime endUtc = dateTimeFromIso(endText);
  if (!startUtc.isValid() || !endUtc.isValid() || startUtc >= endUtc) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("start and end must be bounded ISO-8601 datetimes"),
                false};
    }
    return {};
  }
  QStringList calendarIds =
      parseCalendarIds(params.value(QStringLiteral("calendarIds")), error);
  if (error != nullptr && !error->code.isEmpty()) {
    return {};
  }
  QString dbError;
  const QJsonObject coverage =
      m_sync.ensureRangeHydrated(startUtc, endUtc, calendarIds, &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  const QList<Event> events =
      m_database.eventsBetween(startUtc, endUtc, calendarIds, &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }

  int offset = qMax(0, params.value(QStringLiteral("offset")).toInt());
  int limit = params.value(QStringLiteral("limit")).toInt(kDefaultEventPageLimit);
  if (limit <= 0) {
    limit = kDefaultEventPageLimit;
  }
  if (limit > kMaxEventPageLimit) {
    limit = kMaxEventPageLimit;
  }
  const int total = static_cast<int>(events.size());
  const int safeOffset = qMin(offset, total);
  const int safeCount = (safeOffset >= total) ? 0 : qMin(limit, total - safeOffset);

  QJsonArray encodedEvents;
  for (int i = 0; i < safeCount; ++i) {
    encodedEvents.append(toJson(events[safeOffset + i]));
  }
  return QJsonObject{
      {QStringLiteral("events"), encodedEvents},
      {QStringLiteral("offset"), safeOffset},
      {QStringLiteral("limit"), limit},
      {QStringLiteral("total"), total},
      {QStringLiteral("hasMore"), safeOffset + safeCount < total},
      {QStringLiteral("nextOffset"), safeOffset + safeCount < total
                                         ? (safeOffset + safeCount)
                                         : safeOffset + safeCount},
      {QStringLiteral("coverage"), coverage},
  };
}

QJsonValue Daemon::onEventsGet(const QJsonObject& params, ipc::Error* error) {
  QString eventId;
  if (!validateRequiredString(params, QStringLiteral("eventId"), &eventId, error)) {
    return {};
  }
  QString dbError;
  Event event = m_database.event(eventId, &dbError);
  if (event.id.isEmpty()) {
    if (error != nullptr) {
      const bool notFound =
          dbError.isEmpty() || dbError == QStringLiteral("Event not found");
      *error = {
          notFound ? QStringLiteral("not_found") : QStringLiteral("database_error"),
          notFound ? QStringLiteral("Event not found") : dbError, false};
    }
    return {};
  }
  QString requestedRecurrenceId =
      params.value(QStringLiteral("recurrenceId")).toString().trimmed();
  if (requestedRecurrenceId.isEmpty()) {
    requestedRecurrenceId = recurrenceReference(params).trimmed();
  }
  if (!requestedRecurrenceId.isEmpty()) {
    QString seriesError;
    const QList<Event> series =
        m_database.eventsByUid(event.calendarId, event.uid, &seriesError);
    if (!seriesError.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("database_error"), seriesError, false};
      }
      return {};
    }
    Event master;
    for (const Event& candidate : series) {
      if (candidate.recurrenceId.isEmpty() && !candidate.recurrenceRule.isEmpty()) {
        master = candidate;
        break;
      }
    }
    if (master.id.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("not_found"),
                  QStringLiteral("Recurring event series not found"), false};
      }
      return {};
    }
    const auto matchesReference = [&requestedRecurrenceId,
                                   &master](const Event& candidate) {
      return recurrenceIdentityEqual(requestedRecurrenceId, candidate.recurrenceId,
                                     master.allDay, master.timeKind,
                                     master.startTimeZone);
    };
    for (const Event& candidate : series) {
      if (!candidate.recurrenceId.isEmpty() && matchesReference(candidate)) {
        if (candidate.deleted) {
          if (error != nullptr) {
            *error = {QStringLiteral("not_found"),
                      QStringLiteral("The requested occurrence was cancelled"), false};
          }
          return {};
        }
        return toJson(candidate);
      }
    }

    QDateTime anchor;
    if (master.allDay) {
      const QDate date = recurrenceDate(requestedRecurrenceId);
      if (date.isValid()) {
        anchor = QDateTime(date, QTime(0, 0), QTimeZone::UTC);
      }
    } else {
      anchor = recurrenceDateTime(requestedRecurrenceId, master.startTimeZone);
    }
    if (!anchor.isValid()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_params"),
                  QStringLiteral("recurrenceId is not a valid occurrence reference"),
                  false};
      }
      return {};
    }
    const RecurrenceExpansionResult expanded =
        RecurrenceExpander::expand(series, anchor.addDays(-2), anchor.addDays(2), 1000);
    for (const Event& occurrence : expanded.occurrences) {
      if (matchesReference(occurrence)) {
        return toJson(occurrence);
      }
    }
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"),
                QStringLiteral("The requested occurrence does not exist"), false};
    }
    return {};
  }
  return toJson(event);
}

QJsonValue Daemon::onEventsCreate(const QJsonObject& params, ipc::Error* error) {
  const QJsonObject eventPayload =
      normalizedEventDraft(params.value(QStringLiteral("event")).isObject()
                               ? params.value(QStringLiteral("event")).toObject()
                               : params.value(QStringLiteral("draft")).toObject());
  if (eventPayload.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("events.create requires {event:{...}} payload"), false};
    }
    return {};
  }
  Event event = eventFromJson(eventPayload);
  if (event.calendarId.trimmed().isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("calendarId is required"), false};
    }
    return {};
  }
  if (event.id.isEmpty()) {
    event.id = newUuid();
  }
  QString dbError;
  const Calendar targetCalendar = m_database.calendar(event.calendarId, &dbError);
  if (targetCalendar.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Calendar not found"),
                false};
    }
    return {};
  }
  if (targetCalendar.readOnly) {
    if (error != nullptr) {
      *error = {QStringLiteral("calendar_read_only"),
                QStringLiteral("This calendar is read-only"), false};
    }
    return {};
  }
  const QString timeError = validateEventTimes(event);
  if (!timeError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_event_time"), timeError, false};
    }
    return {};
  }
  const QString recurrenceScope = normalizedRecurrenceScope(
      params.value(QStringLiteral("recurrenceScope")).toString());
  if (recurrenceScope != QStringLiteral("series")) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_recurrence_scope"),
                QStringLiteral("New events must use the entire-series recurrence "
                               "scope"),
                false};
    }
    return {};
  }
  // Exception rows are provider-owned representations of a series occurrence.
  // Creating one from a client draft would let a caller invent an occurrence
  // identity that cannot be reconciled safely with the provider resource.
  if (!event.recurrenceId.trimmed().isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_recurrence_creation"),
                QStringLiteral("New events cannot include a recurrence occurrence "
                               "identity"),
                false};
    }
    return {};
  }
  QString guestPolicy =
      params.value(QStringLiteral("guestNotificationPolicy")).toString();
  if (!event.attendees.isEmpty() && guestPolicy.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("guest_notification_policy_required"),
                QStringLiteral("Choose whether guests should be notified"), false};
    }
    return {};
  }
  if (guestPolicy.isEmpty()) {
    guestPolicy = QStringLiteral("none");
  }
  const QString clientMutationId =
      params.value(QStringLiteral("clientMutationId")).toString(newUuid());
  if (!m_database.saveLocalEvent(&event, OutboxOperation::Create, &dbError,
                                 clientMutationId, recurrenceScope, guestPolicy, 0)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"),
                dbError.isEmpty() ? QStringLiteral("Unable to save event") : dbError,
                false};
    }
    return {};
  }
  emitEventsChanged({event.calendarId});
  const Calendar calendar = m_database.calendar(event.calendarId);
  m_sync.syncAccount(calendar.accountId);
  QJsonObject result = toJson(event);
  result.insert(QStringLiteral("clientMutationId"), clientMutationId);
  result.insert(QStringLiteral("revision"), m_database.changeRevision());
  return result;
}

QJsonValue Daemon::onEventsUpdate(const QJsonObject& params, ipc::Error* error) {
  QJsonObject eventPayload =
      normalizedEventDraft(params.value(QStringLiteral("event")).isObject()
                               ? params.value(QStringLiteral("event")).toObject()
                               : params.value(QStringLiteral("patch")).toObject());
  const QString referencedId = eventReferenceId(params);
  if (!referencedId.isEmpty() && !eventPayload.contains(QStringLiteral("id"))) {
    eventPayload.insert(QStringLiteral("id"), referencedId);
  }
  if (eventPayload.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("events.update requires {event:{...}} payload"), false};
    }
    return {};
  }
  const QString requestedId = eventPayload.value(QStringLiteral("id")).toString();
  const Event requestedCurrent = m_database.event(requestedId);
  if (params.value(QStringLiteral("patch")).isObject() &&
      !requestedCurrent.id.isEmpty()) {
    QJsonObject merged = toJson(requestedCurrent);
    const QJsonObject patch =
        normalizedEventDraft(params.value(QStringLiteral("patch")).toObject());
    for (auto iterator = patch.constBegin(); iterator != patch.constEnd(); ++iterator) {
      merged.insert(iterator.key(), iterator.value());
    }
    merged.insert(QStringLiteral("id"), requestedId);
    eventPayload = merged;
  }
  Event event = eventFromJson(eventPayload);
  if (event.id.trimmed().isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("event.id is required"), false};
    }
    return {};
  }
  QString currentError;
  Event currentEvent = m_database.event(event.id, &currentError);
  if (currentEvent.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Event not found"), false};
    }
    return {};
  }
  const QString recurrenceScope = normalizedRecurrenceScope(
      params.value(QStringLiteral("recurrenceScope")).toString());
  if (!QStringList{QStringLiteral("series"), QStringLiteral("occurrence"),
                   QStringLiteral("future")}
           .contains(recurrenceScope)) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_recurrence_scope"),
                QStringLiteral("Unknown recurrence scope"), false};
    }
    return {};
  }
  // A recurrence identity is part of the event reference, never an editable
  // draft field.  In particular, an untrusted draft must not be able to turn a
  // non-recurring event into an occurrence mutation.  Persisted detached
  // exceptions retain their own identity, while generated master occurrences
  // require it on eventRef.
  const QString referencedRecurrenceId = recurrenceReference(params).trimmed();
  const bool storedDetachedException = !currentEvent.recurrenceId.isEmpty();
  const bool storedRecurringMaster =
      !currentEvent.recurrenceRule.isEmpty() && !storedDetachedException;
  if (recurrenceScope == QStringLiteral("series")) {
    event.recurrenceId =
        storedDetachedException ? currentEvent.recurrenceId : QString();
  } else {
    if (!storedDetachedException && !storedRecurringMaster) {
      if (error != nullptr) {
        *error = {QStringLiteral("recurrence_scope_requires_recurring_event"),
                  QStringLiteral("This recurrence scope requires a recurring event"),
                  false};
      }
      return {};
    }
    if (storedDetachedException) {
      if (!referencedRecurrenceId.isEmpty() &&
          !recurrenceIdentityEqual(referencedRecurrenceId, currentEvent.recurrenceId,
                                   currentEvent.allDay, currentEvent.timeKind,
                                   currentEvent.startTimeZone)) {
        if (error != nullptr) {
          *error = {QStringLiteral("occurrence_identity_mismatch"),
                    QStringLiteral("eventRef does not match this detached occurrence"),
                    false};
        }
        return {};
      }
      event.recurrenceId = currentEvent.recurrenceId;
    } else {
      if (referencedRecurrenceId.isEmpty()) {
        if (error != nullptr) {
          *error = {QStringLiteral("occurrence_identity_required"),
                    QStringLiteral("This recurrence scope requires an occurrence ID"),
                    false};
        }
        return {};
      }
      event.recurrenceId = referencedRecurrenceId;
    }
  }
  Event providerSource = currentEvent;
  bool newDetachedException = false;
  const bool generatedOccurrence = !currentEvent.recurrenceRule.isEmpty() &&
                                   currentEvent.recurrenceId.isEmpty() &&
                                   !event.recurrenceId.isEmpty();
  if ((generatedOccurrence || !currentEvent.recurrenceId.isEmpty()) &&
      recurrenceScope == QStringLiteral("series")) {
    const Event master =
        currentEvent.recurrenceId.isEmpty()
            ? currentEvent
            : m_database.eventByUid(currentEvent.calendarId, currentEvent.uid);
    QString rebaseError;
    if (master.id.isEmpty() ||
        !rebaseOccurrenceDraftOntoMaster(&event, master, &rebaseError)) {
      if (error != nullptr) {
        *error = {QStringLiteral("series_rebase_unsupported"),
                  rebaseError.isEmpty()
                      ? QStringLiteral("The recurring master could not be loaded")
                      : rebaseError,
                  false};
      }
      return {};
    }
    currentEvent = master;
    providerSource = master;
  } else if (generatedOccurrence) {
    const QString recurrenceId = event.recurrenceId;
    event.id = newUuid();
    event.calendarId = currentEvent.calendarId;
    event.remoteId = resourceRemoteId(currentEvent.remoteId);
    if (!event.remoteId.isEmpty()) {
      event.remoteId += QLatin1Char('#') + recurrenceId;
    }
    event.uid = currentEvent.uid;
    event.etag = currentEvent.etag;
    event.recurrenceId = recurrenceId;
    event.recurrenceRule.clear();
    event.rawPayload = currentEvent.rawPayload;
    event.rawFormat = currentEvent.rawFormat;
    event.createdAt = {};
    event.localRevision = 0;
    providerSource = currentEvent;
    newDetachedException = true;
  } else if (recurrenceScope != QStringLiteral("series") &&
             event.recurrenceId.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("occurrence_identity_required"),
                QStringLiteral("This recurrence scope requires an occurrence ID"),
                false};
    }
    return {};
  }
  if (event.calendarId.trimmed().isEmpty()) {
    event.calendarId = currentEvent.calendarId;
  } else if (event.calendarId != currentEvent.calendarId) {
    if (error != nullptr) {
      *error = {QStringLiteral("calendar_move_not_supported"),
                QStringLiteral(
                    "Moving an existing event between calendars is not available yet"),
                false};
    }
    return {};
  }
  // Provider identity and retained lossless payload are daemon-owned. Clients
  // can edit canonical fields but cannot redirect an update to a different
  // remote object or erase metadata required for a safe round trip.
  if (!newDetachedException) {
    event.remoteId = currentEvent.remoteId;
    event.uid = currentEvent.uid;
    event.etag = currentEvent.etag;
    event.rawPayload = currentEvent.rawPayload;
    event.rawFormat = currentEvent.rawFormat;
    event.createdAt = currentEvent.createdAt;
  }
  const Calendar targetCalendar = m_database.calendar(event.calendarId);
  if (targetCalendar.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Calendar not found"),
                false};
    }
    return {};
  }
  if (recurrenceScope == QStringLiteral("future") &&
      !targetCalendar.capabilities.value(QStringLiteral("thisAndFuture")).toBool()) {
    if (error != nullptr) {
      *error = {QStringLiteral("recurrence_scope_unsupported"),
                QStringLiteral("This calendar cannot safely apply this-and-future "
                               "changes"),
                false};
    }
    return {};
  }
  if (targetCalendar.readOnly) {
    if (error != nullptr) {
      *error = {QStringLiteral("calendar_read_only"),
                QStringLiteral("This calendar is read-only"), false};
    }
    return {};
  }
  const QString timeError = validateEventTimes(event);
  if (!timeError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_event_time"), timeError, false};
    }
    return {};
  }
  QString guestPolicy =
      params.value(QStringLiteral("guestNotificationPolicy")).toString();
  if (event.attendees != providerSource.attendees && guestPolicy.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("guest_notification_policy_required"),
                QStringLiteral("Choose whether guests should be notified"), false};
    }
    return {};
  }
  if (guestPolicy.isEmpty()) {
    guestPolicy = QStringLiteral("none");
  }
  const QString clientMutationId =
      params.value(QStringLiteral("clientMutationId")).toString(newUuid());
  const qint64 requestedLocalRevision =
      params.value(QStringLiteral("expectedLocalRevision"))
          .toInteger(currentEvent.localRevision);
  if (newDetachedException && requestedLocalRevision != providerSource.localRevision) {
    if (error != nullptr) {
      *error = {QStringLiteral("revision_mismatch"),
                QStringLiteral("The recurring series changed since it was opened"),
                false};
    }
    return {};
  }
  const qint64 expectedLocalRevision =
      newDetachedException ? 0 : requestedLocalRevision;
  QString dbError;
  if (!m_database.saveLocalEvent(&event, OutboxOperation::Update, &dbError,
                                 clientMutationId, recurrenceScope, guestPolicy,
                                 expectedLocalRevision)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"),
                dbError.isEmpty() ? QStringLiteral("Unable to update event") : dbError,
                false};
    }
    return {};
  }
  emitEventsChanged({event.calendarId});
  const Calendar calendar = m_database.calendar(event.calendarId);
  m_sync.syncAccount(calendar.accountId);
  QJsonObject result = toJson(event);
  result.insert(QStringLiteral("clientMutationId"), clientMutationId);
  result.insert(QStringLiteral("revision"), m_database.changeRevision());
  return result;
}

QJsonValue Daemon::onEventsRemove(const QJsonObject& params, ipc::Error* error) {
  const QString eventId = eventReferenceId(params);
  if (eventId.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("eventRef.eventId is required"), false};
    }
    return {};
  }
  QString dbError;
  Event event = m_database.event(eventId, &dbError);
  if (!dbError.isEmpty() && event.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  if (event.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Event not found"), false};
    }
    return {};
  }
  const QString recurrenceScope = normalizedRecurrenceScope(
      params.value(QStringLiteral("recurrenceScope")).toString());
  if (!QStringList{QStringLiteral("series"), QStringLiteral("occurrence"),
                   QStringLiteral("future")}
           .contains(recurrenceScope)) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_recurrence_scope"),
                QStringLiteral("Unknown recurrence scope"), false};
    }
    return {};
  }
  bool newDetachedException = false;
  Event providerSource = event;
  if (recurrenceScope == QStringLiteral("series") && !event.recurrenceId.isEmpty()) {
    const Event master =
        m_database.eventByUid(event.calendarId, event.uid, {}, &dbError);
    if (master.id.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("series_master_not_found"),
                  QStringLiteral("The recurring master could not be loaded"), false};
      }
      return {};
    }
    event = master;
    providerSource = master;
  } else if (recurrenceScope != QStringLiteral("series") &&
             event.recurrenceId.isEmpty()) {
    const QString recurrenceId = recurrenceReference(params);
    if (event.recurrenceRule.isEmpty() || recurrenceId.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("occurrence_identity_required"),
                  QStringLiteral("Deleting one recurrence requires an occurrence ID"),
                  false};
      }
      return {};
    }
    providerSource = event;
    event = detachedOccurrence(providerSource, recurrenceId);
    newDetachedException = true;
    const QString occurrenceTimeError = validateEventTimes(event);
    if (!occurrenceTimeError.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_occurrence"), occurrenceTimeError, false};
      }
      return {};
    }
  }
  const Calendar eventCalendar = m_database.calendar(event.calendarId);
  if (eventCalendar.readOnly) {
    if (error != nullptr) {
      *error = {QStringLiteral("calendar_read_only"),
                QStringLiteral("This calendar is read-only"), false};
    }
    return {};
  }
  if (recurrenceScope == QStringLiteral("future") &&
      !eventCalendar.capabilities.value(QStringLiteral("thisAndFuture")).toBool()) {
    if (error != nullptr) {
      *error = {QStringLiteral("recurrence_scope_unsupported"),
                QStringLiteral("This calendar cannot safely delete this and future "
                               "occurrences"),
                false};
    }
    return {};
  }
  Event removal = event;
  removal.deleted = true;
  QString guestPolicy =
      params.value(QStringLiteral("guestNotificationPolicy")).toString();
  if (!providerSource.attendees.isEmpty() && guestPolicy.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("guest_notification_policy_required"),
                QStringLiteral("Choose whether guests should be notified"), false};
    }
    return {};
  }
  if (guestPolicy.isEmpty()) {
    guestPolicy = QStringLiteral("none");
  }
  const QString clientMutationId =
      params.value(QStringLiteral("clientMutationId")).toString(newUuid());
  const qint64 requestedLocalRevision =
      params.value(QStringLiteral("expectedLocalRevision"))
          .toInteger(providerSource.localRevision);
  if (newDetachedException && requestedLocalRevision != providerSource.localRevision) {
    if (error != nullptr) {
      *error = {QStringLiteral("revision_mismatch"),
                QStringLiteral("The recurring series changed since it was opened"),
                false};
    }
    return {};
  }
  const qint64 expectedLocalRevision =
      newDetachedException ? 0 : requestedLocalRevision;
  QString removeError;
  if (!m_database.saveLocalEvent(&removal, OutboxOperation::Remove, &removeError,
                                 clientMutationId, recurrenceScope, guestPolicy,
                                 expectedLocalRevision)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"),
                removeError.isEmpty() ? QStringLiteral("Unable to remove event")
                                      : removeError,
                false};
    }
    return {};
  }
  const Event updated = m_database.event(removal.id, &dbError);
  emitEventsChanged({removal.calendarId});
  const Calendar calendar = m_database.calendar(removal.calendarId);
  m_sync.syncAccount(calendar.accountId);
  if (!updated.id.isEmpty()) {
    QJsonObject result = toJson(updated);
    result.insert(QStringLiteral("clientMutationId"), clientMutationId);
    const QString undoToken = clientMutationId;
    result.insert(QStringLiteral("undoUntil"),
                  isoUtc(QDateTime::currentDateTimeUtc().addSecs(10)));
    result.insert(QStringLiteral("undoToken"), undoToken);
    result.insert(QStringLiteral("undoLabel"), QStringLiteral("Undo delete"));
    result.insert(QStringLiteral("revision"), m_database.changeRevision());
    return result;
  }
  QJsonObject result = toJson(event);
  result.insert(QStringLiteral("clientMutationId"), clientMutationId);
  result.insert(QStringLiteral("revision"), m_database.changeRevision());
  return result;
}

QJsonValue Daemon::onEventsUndo(const QJsonObject& params, ipc::Error* error) {
  const QString mutationId =
      params.value(QStringLiteral("undoToken"))
          .toString(params.value(QStringLiteral("clientMutationId")).toString());
  const QString eventId = params.value(QStringLiteral("eventId")).toString();
  if (mutationId.isEmpty() && eventId.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("undoToken, clientMutationId, or eventId is required"),
                false};
    }
    return {};
  }
  const QList<OutboxItem> operations = m_database.outboxItems(500);
  for (const OutboxItem& operation : operations) {
    if (operation.operation != OutboxOperation::Remove ||
        operation.state == OutboxState::Done ||
        (!mutationId.isEmpty() && operation.idempotencyKey != mutationId &&
         !operation.idempotencyKey.startsWith(mutationId + QLatin1Char(':'))) ||
        (!eventId.isEmpty() && operation.eventId != eventId)) {
      continue;
    }
    QString dbError;
    if (!m_database.discardOutbox(operation.id, &dbError)) {
      if (error != nullptr) {
        *error = {QStringLiteral("undo_failed"), dbError, false};
      }
      return {};
    }
    const Event restored = m_database.event(operation.eventId);
    emitEventsChanged({operation.calendarId});
    return QJsonObject{{QStringLiteral("undone"), true},
                       {QStringLiteral("event"), toJson(restored)},
                       {QStringLiteral("revision"), m_database.changeRevision()}};
  }
  if (error != nullptr) {
    *error = {QStringLiteral("undo_expired"),
              QStringLiteral("The delete can no longer be undone"), false};
  }
  return {};
}

QJsonValue Daemon::onEventsSearch(const QJsonObject& params, ipc::Error* error) {
  const QString query = params.value(QStringLiteral("query")).toString().trimmed();
  if (query.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("A non-empty search query is required"), false};
    }
    return {};
  }
  const QStringList calendarIds =
      parseCalendarIds(params.value(QStringLiteral("calendarIds")), error);
  if (error != nullptr && !error->code.isEmpty()) {
    return {};
  }
  const int limit =
      qBound(1, params.value(QStringLiteral("limit")).toInt(kDefaultEventPageLimit),
             kMaxEventPageLimit);
  const int offset = qMax(0, params.value(QStringLiteral("offset")).toInt());
  const QString startText = params.value(QStringLiteral("start")).toString().trimmed();
  const QString endText = params.value(QStringLiteral("end")).toString().trimmed();
  const QDateTime start = dateTimeFromIso(startText);
  const QDateTime end = dateTimeFromIso(endText);
  if ((!startText.isEmpty() && !start.isValid()) ||
      (!endText.isEmpty() && !end.isValid()) ||
      (start.isValid() && end.isValid() && start >= end)) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("Search date filters must be valid ISO-8601 bounds"),
                false};
    }
    return {};
  }
  EventSearchQuery search;
  search.text = query;
  search.calendarIds = calendarIds;
  search.startUtc = start;
  search.endUtc = end;
  search.accountId = params.value(QStringLiteral("accountId")).toString().trimmed();
  search.invitationState =
      params.value(QStringLiteral("invitationState")).toString().trimmed();
  search.limit = limit;
  search.offset = offset;
  QString dbError;
  const EventSearchPage page = m_database.searchEvents(search, &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  QJsonArray matches;
  for (const Event& event : page.events) {
    matches.append(toJson(event));
  }
  return QJsonObject{{QStringLiteral("events"), matches},
                     {QStringLiteral("total"), page.total},
                     {QStringLiteral("offset"), offset},
                     {QStringLiteral("limit"), limit},
                     {QStringLiteral("hasMore"), offset + matches.size() < page.total},
                     {QStringLiteral("nextOffset"), offset + matches.size()},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onEventsMove(const QJsonObject& params, ipc::Error* error) {
  QString eventId = eventReferenceId(params);
  QString targetCalendarId;
  if (eventId.isEmpty() ||
      !validateRequiredString(params, QStringLiteral("targetCalendarId"),
                              &targetCalendarId, error)) {
    if (eventId.isEmpty() && error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("eventRef.eventId is required"), false};
    }
    return {};
  }
  QString dbError;
  Event source = m_database.event(eventId, &dbError);
  Event revisionSource = source;
  const QString recurrenceScope = normalizedRecurrenceScope(
      params.value(QStringLiteral("recurrenceScope")).toString());
  if (!QStringList{QStringLiteral("series"), QStringLiteral("occurrence"),
                   QStringLiteral("future")}
           .contains(recurrenceScope)) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_recurrence_scope"),
                QStringLiteral("Unknown recurrence scope"), false};
    }
    return {};
  }
  if (recurrenceScope == QStringLiteral("future")) {
    if (error != nullptr) {
      *error = {
          QStringLiteral("recurrence_scope_unsupported"),
          QStringLiteral("Moving this and future occurrences cannot be represented "
                         "safely by the selected providers"),
          false};
    }
    return {};
  }
  const QString requestedRecurrenceId = recurrenceReference(params).trimmed();
  if (!source.id.isEmpty() && recurrenceScope == QStringLiteral("series") &&
      !source.recurrenceId.isEmpty()) {
    const Event master =
        m_database.eventByUid(source.calendarId, source.uid, {}, &dbError);
    if (master.id.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("series_master_not_found"),
                  QStringLiteral("The recurring master could not be loaded"), false};
      }
      return {};
    }
    source = master;
    revisionSource = master;
  } else if (!source.id.isEmpty() && recurrenceScope == QStringLiteral("occurrence")) {
    if (!source.recurrenceId.isEmpty()) {
      if (!requestedRecurrenceId.isEmpty() &&
          !recurrenceIdentityEqual(requestedRecurrenceId, source.recurrenceId,
                                   source.allDay, source.timeKind,
                                   source.startTimeZone)) {
        if (error != nullptr) {
          *error = {QStringLiteral("occurrence_identity_mismatch"),
                    QStringLiteral("The event and occurrence reference do not match"),
                    false};
        }
        return {};
      }
    } else {
      if (source.recurrenceRule.isEmpty() || requestedRecurrenceId.isEmpty()) {
        if (error != nullptr) {
          *error = {QStringLiteral("occurrence_identity_required"),
                    QStringLiteral("Moving one recurrence requires an occurrence ID"),
                    false};
        }
        return {};
      }
      const Event existing = m_database.eventByUid(source.calendarId, source.uid,
                                                   requestedRecurrenceId, &dbError);
      if (!existing.id.isEmpty()) {
        source = existing;
        revisionSource = existing;
      } else {
        revisionSource = source;
        source = detachedOccurrence(revisionSource, requestedRecurrenceId);
        const QString occurrenceTimeError = validateEventTimes(source);
        if (!occurrenceTimeError.isEmpty()) {
          if (error != nullptr) {
            *error = {QStringLiteral("invalid_occurrence"), occurrenceTimeError, false};
          }
          return {};
        }
      }
    }
  }
  const Calendar sourceCalendar = m_database.calendar(source.calendarId);
  const Calendar targetCalendar = m_database.calendar(targetCalendarId);
  if (source.id.isEmpty() || targetCalendar.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"),
                source.id.isEmpty() ? QStringLiteral("Event not found")
                                    : QStringLiteral("Target calendar not found"),
                false};
    }
    return {};
  }
  if (source.calendarId == targetCalendarId) {
    return toJson(source);
  }
  const qint64 expectedLocalRevision =
      params.value(QStringLiteral("expectedLocalRevision"))
          .toInteger(revisionSource.localRevision);
  if (expectedLocalRevision >= 0 &&
      expectedLocalRevision != revisionSource.localRevision) {
    if (error != nullptr) {
      *error = {QStringLiteral("stale_local_revision"),
                QStringLiteral("The event changed after this editor was opened; reload "
                               "it before moving"),
                false};
    }
    return {};
  }
  if (sourceCalendar.readOnly || targetCalendar.readOnly) {
    if (error != nullptr) {
      *error = {QStringLiteral("calendar_read_only"),
                QStringLiteral("Source and target calendars must be writable"), false};
    }
    return {};
  }
  if (sourceCalendar.accountId != targetCalendar.accountId &&
      !params.value(QStringLiteral("confirmedCrossProvider")).toBool()) {
    if (error != nullptr) {
      *error = {QStringLiteral("confirmation_required"),
                QStringLiteral("Confirm the cross-account calendar move"), false};
    }
    return {};
  }
  Event destination = source;
  if (params.value(QStringLiteral("draft")).isObject()) {
    QJsonObject merged = toJson(source);
    const QJsonObject draft =
        normalizedEventDraft(params.value(QStringLiteral("draft")).toObject());
    for (auto iterator = draft.constBegin(); iterator != draft.constEnd(); ++iterator) {
      merged.insert(iterator.key(), iterator.value());
    }
    destination = eventFromJson(merged);
    destination.uid = source.uid;
  }
  destination.calendarId = targetCalendarId;
  if (recurrenceScope == QStringLiteral("occurrence")) {
    destination.id.clear();
    destination.remoteId.clear();
    destination.uid.clear();
    destination.etag.clear();
    destination.recurrenceId.clear();
    destination.recurrenceRule.clear();
    destination.rawPayload.clear();
    destination.rawFormat.clear();
    destination.createdAt = {};
    destination.localRevision = 0;
  }
  const QString timeError = validateEventTimes(destination);
  if (!timeError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_event_time"), timeError, false};
    }
    return {};
  }

  QString guestPolicy =
      params.value(QStringLiteral("guestNotificationPolicy")).toString();
  if (!destination.attendees.isEmpty() && guestPolicy.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("guest_notification_policy_required"),
                QStringLiteral("Choose whether guests should be notified"), false};
    }
    return {};
  }
  if (guestPolicy.isEmpty()) {
    guestPolicy = QStringLiteral("none");
  }
  const QString mutationId =
      params.value(QStringLiteral("clientMutationId")).toString(newUuid());

  if (sourceCalendar.accountId == targetCalendar.accountId &&
      recurrenceScope == QStringLiteral("series")) {
    if (!m_database.moveLocalEvent(&destination, &dbError, mutationId, recurrenceScope,
                                   guestPolicy, expectedLocalRevision)) {
      if (error != nullptr) {
        *error = {dbError.contains(QStringLiteral("changed since"))
                      ? QStringLiteral("stale_local_revision")
                      : QStringLiteral("move_failed"),
                  dbError, false};
      }
      return {};
    }
    emitEventsChanged({source.calendarId, destination.calendarId});
    if (m_database.account(sourceCalendar.accountId).provider != ProviderKind::Local) {
      m_sync.syncAccount(sourceCalendar.accountId);
    }
    return QJsonObject{
        {QStringLiteral("sourceEventId"), source.id},
        {QStringLiteral("event"), toJson(destination)},
        {QStringLiteral("clientMutationId"), mutationId},
        {QStringLiteral("state"),
         destination.dirty ? QStringLiteral("pending") : QStringLiteral("complete")},
        {QStringLiteral("revision"), m_database.changeRevision()},
    };
  }

  if (!m_database.moveEventAcrossAccounts(source, &destination, &dbError, mutationId,
                                          recurrenceScope, guestPolicy,
                                          expectedLocalRevision)) {
    if (error != nullptr) {
      *error = {dbError.contains(QStringLiteral("changed since"))
                    ? QStringLiteral("stale_local_revision")
                : dbError.contains(QStringLiteral("pending event changes"))
                    ? QStringLiteral("pending_operations")
                    : QStringLiteral("move_failed"),
                dbError, false};
    }
    return {};
  }
  emitEventsChanged({source.calendarId, destination.calendarId});
  // The source removal depends on destination acknowledgement. Starting the
  // destination first lets an immediately acknowledged local destination
  // release the source operation before its provider drain begins.
  m_sync.syncAccount(targetCalendar.accountId);
  if (sourceCalendar.accountId != targetCalendar.accountId) {
    m_sync.syncAccount(sourceCalendar.accountId);
  } else if (recurrenceScope == QStringLiteral("occurrence") &&
             m_database.account(targetCalendar.accountId).provider ==
                 ProviderKind::Local) {
    // The detached cancellation depends on the destination create. Local
    // acknowledgement is synchronous, so a second bounded drain completes the
    // newly released dependency before returning to the client.
    m_sync.syncAccount(targetCalendar.accountId);
  }
  return QJsonObject{{QStringLiteral("sourceEventId"), source.id},
                     {QStringLiteral("event"), toJson(destination)},
                     {QStringLiteral("clientMutationId"), mutationId},
                     {QStringLiteral("state"),
                      targetCalendar.accountId == sourceCalendar.accountId &&
                              m_database.account(targetCalendar.accountId).provider ==
                                  ProviderKind::Local
                          ? QStringLiteral("complete")
                          : QStringLiteral("pending")},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onEventsRespond(const QJsonObject& params, ipc::Error* error) {
  QString eventId = eventReferenceId(params);
  QString response;
  if (eventId.isEmpty() ||
      !validateRequiredString(params, QStringLiteral("response"), &response, error)) {
    if (eventId.isEmpty() && error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("eventRef.eventId is required"), false};
    }
    return {};
  }
  response = response.toLower();
  if (!QStringList{QStringLiteral("accepted"), QStringLiteral("tentative"),
                   QStringLiteral("declined")}
           .contains(response)) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("response must be accepted, tentative, or declined"),
                false};
    }
    return {};
  }
  QString dbError;
  Event event = m_database.event(eventId, &dbError);
  if (event.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Event not found"), false};
    }
    return {};
  }
  const QString recurrenceScope = normalizedRecurrenceScope(
      params.value(QStringLiteral("recurrenceScope")).toString());
  if (!QStringList{QStringLiteral("series"), QStringLiteral("occurrence"),
                   QStringLiteral("future")}
           .contains(recurrenceScope)) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_recurrence_scope"),
                QStringLiteral("Unknown recurrence scope"), false};
    }
    return {};
  }

  Event providerSource = event;
  bool newDetachedException = false;
  const QString requestedRecurrenceId = recurrenceReference(params).trimmed();
  if (recurrenceScope == QStringLiteral("series") && !event.recurrenceId.isEmpty()) {
    const Event master =
        m_database.eventByUid(event.calendarId, event.uid, {}, &dbError);
    if (master.id.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("series_master_not_found"),
                  QStringLiteral("The recurring master could not be loaded"), false};
      }
      return {};
    }
    event = master;
    providerSource = master;
  } else if (recurrenceScope != QStringLiteral("series") &&
             event.recurrenceId.isEmpty()) {
    if (event.recurrenceRule.isEmpty() || requestedRecurrenceId.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("occurrence_identity_required"),
                  QStringLiteral("This RSVP requires an occurrence ID"), false};
      }
      return {};
    }
    providerSource = event;
    event = detachedOccurrence(providerSource, requestedRecurrenceId);
    newDetachedException = true;
    const QString occurrenceTimeError = validateEventTimes(event);
    if (!occurrenceTimeError.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_occurrence"), occurrenceTimeError, false};
      }
      return {};
    }
  } else if (recurrenceScope != QStringLiteral("series") &&
             !requestedRecurrenceId.isEmpty() &&
             !recurrenceIdentityEqual(requestedRecurrenceId, event.recurrenceId,
                                      event.allDay, event.timeKind,
                                      event.startTimeZone)) {
    if (error != nullptr) {
      *error = {QStringLiteral("occurrence_identity_mismatch"),
                QStringLiteral("The event and occurrence reference do not match"),
                false};
    }
    return {};
  }

  const Calendar calendar = m_database.calendar(event.calendarId);
  const Account account = m_database.account(calendar.accountId);
  if (calendar.id.isEmpty() || calendar.readOnly) {
    if (error != nullptr) {
      *error = {calendar.id.isEmpty() ? QStringLiteral("not_found")
                                      : QStringLiteral("calendar_read_only"),
                calendar.id.isEmpty() ? QStringLiteral("Calendar not found")
                                      : QStringLiteral("This calendar is read-only"),
                false};
    }
    return {};
  }
  if (recurrenceScope == QStringLiteral("future") &&
      !calendar.capabilities.value(QStringLiteral("thisAndFuture")).toBool()) {
    if (error != nullptr) {
      *error = {QStringLiteral("recurrence_scope_unsupported"),
                QStringLiteral("This calendar cannot safely apply this-and-future "
                               "responses"),
                false};
    }
    return {};
  }
  if (account.provider == ProviderKind::CalDav &&
      !calendar.capabilities.value(QStringLiteral("serverScheduling")).toBool()) {
    if (error != nullptr) {
      *error = {QStringLiteral("unsupported"),
                QStringLiteral("This CalDAV server did not advertise scheduling"),
                false};
    }
    return {};
  }
  QJsonArray attendees = event.attendees;
  int attendeeIndex = -1;
  for (qsizetype i = 0; i < attendees.size(); ++i) {
    const QJsonObject attendee = attendees.at(i).toObject();
    const QString email = attendee.value(QStringLiteral("email")).toString();
    if (attendee.value(QStringLiteral("self")).toBool() ||
        (!account.principal.isEmpty() &&
         email.compare(account.principal, Qt::CaseInsensitive) == 0)) {
      attendeeIndex = static_cast<int>(i);
      break;
    }
  }
  if (attendeeIndex < 0) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_an_invitation"),
                QStringLiteral("No attendee identity for this account was found"),
                false};
    }
    return {};
  }
  QJsonObject attendee = attendees.at(attendeeIndex).toObject();
  attendee.insert(QStringLiteral("responseStatus"), response);
  attendee.insert(QStringLiteral("partstat"), response.toUpper());
  attendees.replace(attendeeIndex, attendee);
  event.attendees = attendees;
  const QString mutationId =
      params.value(QStringLiteral("clientMutationId")).toString(newUuid());
  QString responseNotificationPolicy =
      params.value(QStringLiteral("guestNotificationPolicy")).toString();
  if (responseNotificationPolicy.isEmpty()) {
    responseNotificationPolicy = account.provider == ProviderKind::CalDav
                                     ? QStringLiteral("all")
                                     : QStringLiteral("none");
  }
  const qint64 requestedLocalRevision =
      params.value(QStringLiteral("expectedLocalRevision"))
          .toInteger(providerSource.localRevision);
  if (newDetachedException && requestedLocalRevision != providerSource.localRevision) {
    if (error != nullptr) {
      *error = {QStringLiteral("revision_mismatch"),
                QStringLiteral("The recurring series changed since it was opened"),
                false};
    }
    return {};
  }
  const qint64 expectedLocalRevision =
      newDetachedException ? 0 : requestedLocalRevision;
  if (!m_database.saveLocalEvent(&event, OutboxOperation::Update, &dbError, mutationId,
                                 recurrenceScope, responseNotificationPolicy,
                                 expectedLocalRevision)) {
    if (error != nullptr) {
      *error = {QStringLiteral("response_failed"), dbError, false};
    }
    return {};
  }
  emitEventsChanged({event.calendarId});
  m_sync.syncAccount(account.id);
  return QJsonObject{{QStringLiteral("event"), toJson(event)},
                     {QStringLiteral("response"), response},
                     {QStringLiteral("clientMutationId"), mutationId},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onInvitationsList(const QJsonObject& params, ipc::Error* error) {
  QString dbError;
  const QDateTime now = QDateTime::currentDateTimeUtc();
  const qint64 revision = m_database.changeRevision();
  if (m_invitationReadCacheRevision != revision ||
      !m_invitationReadCacheExpiresAt.isValid() ||
      now >= m_invitationReadCacheExpiresAt) {
    QList<Event> refreshed = m_database.invitationEventsBetween(
        now.addMonths(-1), now.addYears(2), &dbError);
    if (!dbError.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("database_error"), dbError, false};
      }
      return {};
    }
    QHash<QString, QString> principalByCalendar;
    QList<Event> awaitingResponse;
    awaitingResponse.reserve(refreshed.size());
    for (const Event& event : std::as_const(refreshed)) {
      if (!principalByCalendar.contains(event.calendarId)) {
        const Calendar calendar = m_database.calendar(event.calendarId, &dbError);
        if (!dbError.isEmpty()) {
          break;
        }
        const Account account = m_database.account(calendar.accountId, &dbError);
        if (!dbError.isEmpty()) {
          break;
        }
        principalByCalendar.insert(event.calendarId, account.principal);
      }
      if (invitationNeedsResponse(event, principalByCalendar.value(event.calendarId))) {
        awaitingResponse.append(event);
      }
    }
    if (!dbError.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("database_error"), dbError, false};
      }
      return {};
    }
    sortInvitations(&awaitingResponse, now);
    m_invitationReadCache = std::move(awaitingResponse);
    m_invitationReadCacheRevision = revision;
    m_invitationReadCacheExpiresAt = now.addSecs(60);
    // An invitation changing from current/upcoming to past changes the sort
    // bucket even without a database revision. Expire at that boundary.
    for (const Event& event : std::as_const(m_invitationReadCache)) {
      const QDateTime end = invitationEnd(event);
      if (end > now && end < m_invitationReadCacheExpiresAt) {
        m_invitationReadCacheExpiresAt = end;
      }
    }
  }
  const QList<Event>& events = m_invitationReadCache;
  int requestedLimit =
      params.value(QStringLiteral("limit")).toInt(kDefaultInvitationPageLimit);
  if (requestedLimit <= 0) {
    requestedLimit = kDefaultInvitationPageLimit;
  }
  const int limit = qBound(1, requestedLimit, kMaxInvitationPageLimit);
  const int requestedOffset = qMax(0, params.value(QStringLiteral("offset")).toInt());
  const int total = static_cast<int>(events.size());
  const int offset = qMin(requestedOffset, total);
  const int count = qMin(limit, total - offset);

  QStringList eventIds;
  eventIds.reserve(count);
  for (int index = 0; index < count; ++index) {
    eventIds.append(events.at(offset + index).id);
  }
  const QHash<QString, bool> seen = m_database.invitationSeenStates(eventIds, &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  QJsonArray invitations;
  for (int index = 0; index < count; ++index) {
    const Event& event = events.at(offset + index);
    QJsonObject encoded = toJson(event);
    encoded.insert(QStringLiteral("seen"), seen.value(event.id, false));
    invitations.append(encoded);
  }
  return QJsonObject{{QStringLiteral("invitations"), invitations},
                     {QStringLiteral("count"), invitations.size()},
                     {QStringLiteral("offset"), offset},
                     {QStringLiteral("limit"), limit},
                     {QStringLiteral("total"), total},
                     {QStringLiteral("hasMore"), offset + count < total},
                     {QStringLiteral("nextOffset"), offset + count},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onInvitationsMarkSeen(const QJsonObject& params, ipc::Error* error) {
  QString eventId;
  if (!validateRequiredString(params, QStringLiteral("eventId"), &eventId, error)) {
    return {};
  }
  if (m_database.event(eventId).id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Invitation not found"),
                false};
    }
    return {};
  }
  QString dbError;
  if (!m_database.setSetting(QStringLiteral("invitation_seen_%1").arg(eventId), true,
                             &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  return QJsonObject{{QStringLiteral("eventId"), eventId},
                     {QStringLiteral("seen"), true}};
}

QJsonValue Daemon::onConflictsList(const QJsonObject& params, ipc::Error* error) {
  QString dbError;
  const QList<Conflict> conflicts = m_database.conflicts(
      params.value(QStringLiteral("unresolvedOnly")).toBool(true), &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  QJsonArray encoded;
  for (const Conflict& conflict : conflicts) {
    encoded.append(toJson(conflict));
  }
  return QJsonObject{{QStringLiteral("conflicts"), encoded},
                     {QStringLiteral("count"), encoded.size()},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onConflictsResolve(const QJsonObject& params, ipc::Error* error) {
  const qint64 id = params.value(QStringLiteral("id")).toInteger();
  const QString strategy = params.value(QStringLiteral("strategy")).toString();
  if (id <= 0 || strategy.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("id and strategy are required"), false};
    }
    return {};
  }
  QString dbError;
  QJsonObject validatedMerge;
  if (strategy == QStringLiteral("merge")) {
    const QList<Conflict> allConflicts = m_database.conflicts(false, &dbError);
    const auto conflict =
        std::find_if(allConflicts.cbegin(), allConflicts.cend(),
                     [id](const Conflict& candidate) { return candidate.id == id; });
    if (conflict == allConflicts.cend() ||
        conflict->state != QStringLiteral("unresolved")) {
      if (error != nullptr) {
        *error = {QStringLiteral("not_found"),
                  QStringLiteral("Unresolved conflict not found"), false};
      }
      return {};
    }
    if (!params.value(QStringLiteral("mergedEvent")).isObject()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_params"),
                  QStringLiteral("merge requires a complete mergedEvent"), false};
      }
      return {};
    }
    validatedMerge = conflict->localSnapshot;
    const QJsonObject requested =
        normalizedEventDraft(params.value(QStringLiteral("mergedEvent")).toObject());
    static const QStringList editableFields = {
        QStringLiteral("summary"),       QStringLiteral("description"),
        QStringLiteral("location"),      QStringLiteral("url"),
        QStringLiteral("startUtc"),      QStringLiteral("endUtc"),
        QStringLiteral("startDate"),     QStringLiteral("endDate"),
        QStringLiteral("startTimeZone"), QStringLiteral("endTimeZone"),
        QStringLiteral("allDay"),        QStringLiteral("timeKind"),
        QStringLiteral("status"),        QStringLiteral("transparency"),
        QStringLiteral("visibility"),    QStringLiteral("recurrenceRule"),
        QStringLiteral("recurrenceId"),  QStringLiteral("sequence"),
        QStringLiteral("organizer"),     QStringLiteral("attendees"),
        QStringLiteral("reminders"),
    };
    for (const QString& field : editableFields) {
      if (requested.contains(field)) {
        validatedMerge.insert(field, requested.value(field));
      }
    }
    const QJsonObject providerSnapshot = conflict->remoteSnapshot.isEmpty()
                                             ? conflict->localSnapshot
                                             : conflict->remoteSnapshot;
    for (const QString& field :
         {QStringLiteral("id"), QStringLiteral("calendarId"),
          QStringLiteral("remoteId"), QStringLiteral("uid"), QStringLiteral("etag"),
          QStringLiteral("rawPayload"), QStringLiteral("rawFormat"),
          QStringLiteral("createdAt")}) {
      if (providerSnapshot.contains(field)) {
        validatedMerge.insert(field, providerSnapshot.value(field));
      }
    }
    const Event merged = eventFromJson(validatedMerge);
    const Calendar calendar = m_database.calendar(merged.calendarId, &dbError);
    const QString timeError = validateEventTimes(merged);
    if (calendar.id.isEmpty() || calendar.readOnly || !timeError.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_merge"),
                  calendar.id.isEmpty()
                      ? QStringLiteral("The merged event calendar does not exist")
                  : calendar.readOnly
                      ? QStringLiteral("The merged event calendar is read-only")
                      : timeError,
                  false};
      }
      return {};
    }
  }
  if (!m_database.resolveConflict(id, strategy, validatedMerge, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("conflict_resolution_failed"), dbError, false};
    }
    return {};
  }
  m_sync.syncAll();
  m_server.broadcast(QStringLiteral("conflicts.changed"),
                     {{QStringLiteral("revision"), m_database.changeRevision()}});
  return QJsonObject{{QStringLiteral("id"), id},
                     {QStringLiteral("strategy"), strategy},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onSettingsGet(const QJsonObject& params, ipc::Error* error) {
  QString key;
  if (!validateRequiredString(params, QStringLiteral("key"), &key, error)) {
    return {};
  }
  const QJsonValue fallback = params.value(QStringLiteral("fallback"));
  QString dbError;
  QJsonValue value = m_database.setting(key, fallback, &dbError);
  if (key == QStringLiteral("defaultCalendarId")) {
    const QString configured = value.toString();
    const Calendar current = m_database.calendar(configured);
    if (current.id.isEmpty() || current.readOnly || !current.enabled ||
        current.capabilities.value(QStringLiteral("deleted")).toBool()) {
      QString resolved;
      const QList<Calendar> calendars = m_database.calendars({}, &dbError);
      if (!dbError.isEmpty()) {
        if (error != nullptr) {
          *error = {QStringLiteral("database_error"), dbError, false};
        }
        return {};
      }
      for (const Calendar& candidate : calendars) {
        if (!candidate.enabled || candidate.readOnly ||
            candidate.capabilities.value(QStringLiteral("deleted")).toBool() ||
            candidate.capabilities.value(QStringLiteral("calendarListRemoved"))
                .toBool()) {
          continue;
        }
        if (resolved.isEmpty() ||
            candidate.capabilities.value(QStringLiteral("primary")).toBool()) {
          resolved = candidate.id;
        }
        if (candidate.capabilities.value(QStringLiteral("primary")).toBool()) break;
      }
      if (!resolved.isEmpty() &&
          !m_database.setSetting(QStringLiteral("defaultCalendarId"), resolved,
                                 &dbError)) {
        if (error != nullptr) {
          *error = {QStringLiteral("database_error"), dbError, false};
        }
        return {};
      }
      value = resolved;
    }
  }
  return QJsonObject{
      {QStringLiteral("key"), key},
      {QStringLiteral("value"), value},
  };
}

QJsonValue Daemon::onSettingsSet(const QJsonObject& params, ipc::Error* error) {
  QString key;
  if (!validateRequiredString(params, QStringLiteral("key"), &key, error)) {
    return {};
  }
  if (!params.contains(QStringLiteral("value"))) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"), QStringLiteral("value is required"),
                false};
    }
    return {};
  }
  const QJsonValue value = params.value(QStringLiteral("value"));
  QString dbError;
  if (key == QStringLiteral("defaultCalendarId")) {
    const Calendar calendar = m_database.calendar(value.toString(), &dbError);
    if (calendar.id.isEmpty() || calendar.readOnly || !calendar.enabled ||
        calendar.capabilities.value(QStringLiteral("deleted")).toBool()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_default_calendar"),
                  QStringLiteral("The default calendar must be writable and enabled"),
                  false};
      }
      return {};
    }
  }
  if (!m_database.setSetting(key, value, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"),
                dbError.isEmpty() ? QStringLiteral("Unable to save setting") : dbError,
                false};
    }
    return {};
  }
  return QJsonObject{
      {QStringLiteral("ok"), true},
      {QStringLiteral("key"), key},
      {QStringLiteral("value"), value},
  };
}

QJsonValue Daemon::onOutboxList(const QJsonObject& params, ipc::Error* error) {
  int limit = params.value(QStringLiteral("limit")).toInt(100);
  if (limit <= 0) {
    limit = 100;
  }
  if (limit > 500) {
    limit = 500;
  }
  QString dbError;
  const QList<OutboxItem> items = m_database.outboxItems(limit, &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  QJsonArray encoded;
  for (const OutboxItem& item : items) {
    encoded.append(toJson(item));
  }
  return QJsonObject{
      {QStringLiteral("items"), encoded},
      {QStringLiteral("count"), static_cast<int>(encoded.size())},
      {QStringLiteral("limit"), limit},
  };
}

QJsonValue Daemon::onOutboxRetry(const QJsonObject& params, ipc::Error* error) {
  const QJsonValue idValue = params.contains(QStringLiteral("id"))
                                 ? params.value(QStringLiteral("id"))
                                 : params.value(QStringLiteral("operationId"));
  if (!idValue.isDouble()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("id is required for outbox.retry"), false};
    }
    return {};
  }
  const qint64 id = idValue.toInteger();
  if (id <= 0) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"), QStringLiteral("id must be positive"),
                false};
    }
    return {};
  }
  QString dbError;
  if (!m_database.updateOutboxState(id, OutboxState::Pending, 0,
                                    QDateTime::currentDateTimeUtc(), QString(),
                                    QString(), &dbError)) {
    if (error != nullptr) {
      if (dbError.isEmpty()) {
        *error = {QStringLiteral("not_found"), QStringLiteral("Outbox item not found"),
                  false};
      } else {
        *error = {QStringLiteral("database_error"), dbError, false};
      }
    }
    return {};
  }
  m_sync.syncAll();
  return QJsonObject{
      {QStringLiteral("ok"), true},
      {QStringLiteral("id"), id},
  };
}

QJsonValue Daemon::onOperationsDiscard(const QJsonObject& params, ipc::Error* error) {
  const qint64 id =
      params.value(QStringLiteral("id"))
          .toInteger(params.value(QStringLiteral("operationId")).toInteger());
  if (id <= 0) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("operationId must be positive"), false};
    }
    return {};
  }
  QString dbError;
  if (!m_database.discardOutbox(id, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("discard_failed"), dbError, false};
    }
    return {};
  }
  m_server.broadcast(QStringLiteral("operations.changed"),
                     {{QStringLiteral("revision"), m_database.changeRevision()}});
  return QJsonObject{{QStringLiteral("discarded"), true},
                     {QStringLiteral("operationId"), id},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onRemindersList(const QJsonObject& params, ipc::Error* error) {
  QString dbError;
  const QList<ReminderJob> reminders = m_database.reminders(
      qBound(1, params.value(QStringLiteral("limit")).toInt(100), 500), &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  QJsonArray encoded;
  for (const ReminderJob& reminder : reminders) {
    QJsonObject item = toJson(reminder);
    const Event event = m_database.event(reminder.eventId);
    item.insert(QStringLiteral("event"), toJson(event));
    encoded.append(item);
  }
  return QJsonObject{{QStringLiteral("reminders"), encoded},
                     {QStringLiteral("count"), encoded.size()},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onRemindersSnooze(const QJsonObject& params, ipc::Error* error) {
  const qint64 id =
      params.value(QStringLiteral("id"))
          .toInteger(params.value(QStringLiteral("reminderId")).toInteger());
  const int minutes = params.value(QStringLiteral("minutes")).toInt();
  QString dbError;
  if (id <= 0 || !m_database.snoozeReminder(id, minutes, &dbError)) {
    if (error != nullptr) {
      *error = {
          id <= 0 ? QStringLiteral("invalid_params") : QStringLiteral("snooze_failed"),
          id <= 0 ? QStringLiteral("reminderId is required") : dbError, false};
    }
    return {};
  }
  return QJsonObject{{QStringLiteral("snoozed"), true},
                     {QStringLiteral("reminderId"), id},
                     {QStringLiteral("minutes"), minutes},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onRemindersDismiss(const QJsonObject& params, ipc::Error* error) {
  const qint64 id =
      params.value(QStringLiteral("id"))
          .toInteger(params.value(QStringLiteral("reminderId")).toInteger());
  QString dbError;
  if (id <= 0 || !m_database.dismissReminder(id, &dbError)) {
    if (error != nullptr) {
      *error = {
          id <= 0 ? QStringLiteral("invalid_params") : QStringLiteral("dismiss_failed"),
          id <= 0 ? QStringLiteral("reminderId is required") : dbError, false};
    }
    return {};
  }
  return QJsonObject{{QStringLiteral("dismissed"), true},
                     {QStringLiteral("reminderId"), id},
                     {QStringLiteral("revision"), m_database.changeRevision()}};
}

QJsonValue Daemon::onIcsRefresh(const QJsonObject& params, ipc::Error* error) {
  QString accountId;
  if (!validateRequiredString(params, QStringLiteral("accountId"), &accountId, error)) {
    return {};
  }
  QString message;
  if (!m_ics.refresh(accountId, &message)) {
    if (error != nullptr) {
      *error = {QStringLiteral("ics_refresh_failed"), message.left(300), false};
    }
    return {};
  }
  return QJsonObject{{QStringLiteral("started"), true},
                     {QStringLiteral("accountId"), accountId}};
}

QJsonValue Daemon::onIcsStatus(const QJsonObject& params, ipc::Error*) const {
  return m_ics.status(params.value(QStringLiteral("accountId")).toString());
}

QJsonValue Daemon::onImportPreview(const QJsonObject& params, ipc::Error* error) const {
  QByteArray payload;
  ics::IcsError importError;
  if (!ics::IcsService::payloadFromRequest(params, &payload, &importError)) {
    if (error != nullptr) {
      *error = {importError.code, importError.message, false};
    }
    return {};
  }
  const QJsonObject result = m_ics.previewImport(
      payload, params.value(QStringLiteral("destinationCalendarId")).toString(),
      &importError);
  if (!importError.isEmpty() && error != nullptr) {
    *error = {importError.code, importError.message, importError.retryable};
  }
  return result;
}

QJsonValue Daemon::onImportCommit(const QJsonObject& params, ipc::Error* error) {
  QByteArray payload;
  ics::IcsError importError;
  if (!ics::IcsService::payloadFromRequest(params, &payload, &importError)) {
    if (error != nullptr) {
      *error = {importError.code, importError.message, false};
    }
    return {};
  }
  QString destinationCalendarId;
  if (!validateRequiredString(params, QStringLiteral("destinationCalendarId"),
                              &destinationCalendarId, error)) {
    return {};
  }
  const QJsonObject result = m_ics.commitImport(
      payload, destinationCalendarId,
      params.value(QStringLiteral("duplicatePolicy")).toString(QStringLiteral("skip")),
      &importError);
  if (!importError.isEmpty() && error != nullptr) {
    *error = {importError.code, importError.message, importError.retryable};
  }
  return result;
}

QJsonValue Daemon::onExport(const QJsonObject& params, ipc::Error* error) const {
  ics::IcsError exportError;
  const QJsonObject result = m_ics.exportCalendar(params, &exportError);
  if (!exportError.isEmpty() && error != nullptr) {
    *error = {exportError.code, exportError.message, exportError.retryable};
  }
  return result;
}

QJsonValue Daemon::onGoogleConfigureClient(const QJsonObject& params,
                                           ipc::Error* error) {
  QString clientId;
  if (!validateRequiredString(params, QStringLiteral("clientId"), &clientId, error)) {
    return {};
  }
  const QString clientSecret = params.value(QStringLiteral("clientSecret")).toString();
  QString message;
  if (!m_google.configureClient(clientId, clientSecret, &message)) {
    if (error != nullptr) {
      *error = {QStringLiteral("google_configuration_failed"), message, false};
    }
    return {};
  }
  return QJsonObject{{QStringLiteral("configured"), true},
                     {QStringLiteral("clientId"), clientId}};
}

QJsonValue Daemon::onGoogleOauthStart(const QJsonObject& params, ipc::Error* error) {
  QString message;
  const QString accountId = m_google.beginAuthorization(
      params.value(QStringLiteral("displayName")).toString(), &message);
  if (accountId.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("google_oauth_start_failed"), message, false};
    }
    return {};
  }
  return QJsonObject{{QStringLiteral("accountId"), accountId},
                     {QStringLiteral("state"), QStringLiteral("waiting_for_browser")}};
}

QJsonValue Daemon::onGoogleOauthCancel(const QJsonObject& params, ipc::Error* error) {
  QString accountId;
  if (!validateRequiredString(params, QStringLiteral("accountId"), &accountId, error)) {
    return {};
  }
  m_google.cancelAuthorization(accountId);
  return QJsonObject{{QStringLiteral("cancelled"), true},
                     {QStringLiteral("accountId"), accountId}};
}

QJsonValue Daemon::onGoogleDisconnect(const QJsonObject& params, ipc::Error* error) {
  QString accountId;
  if (!validateRequiredString(params, QStringLiteral("accountId"), &accountId, error)) {
    return {};
  }
  const bool removeCachedData =
      params.value(QStringLiteral("removeCachedData")).toBool(false);
  QString message;
  if (!m_google.disconnectAccount(accountId, removeCachedData, &message)) {
    if (error != nullptr) {
      *error = {QStringLiteral("google_disconnect_failed"), message, false};
    }
    return {};
  }
  return QJsonObject{{QStringLiteral("disconnected"), true},
                     {QStringLiteral("accountId"), accountId},
                     {QStringLiteral("cachedDataRemoved"), removeCachedData}};
}

QJsonValue Daemon::onSyncAll(const QJsonObject&, ipc::Error*) {
  m_sync.syncAll();
  m_ics.refreshAll(false);
  return QJsonObject{{QStringLiteral("started"), true}};
}

QJsonValue Daemon::onSyncAccount(const QJsonObject& params, ipc::Error* error) {
  QString accountId;
  if (!validateRequiredString(params, QStringLiteral("accountId"), &accountId, error)) {
    return {};
  }
  QString message;
  const Account account = m_database.account(accountId, &message);
  if (account.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Account not found"),
                false};
    }
    return {};
  }
  const bool started = account.provider == ProviderKind::Ics
                           ? m_ics.refresh(accountId, &message)
                           : m_sync.syncAccount(accountId, &message);
  if (!started) {
    if (error != nullptr) {
      *error = {QStringLiteral("sync_failed"), message, false};
    }
    return {};
  }
  return QJsonObject{{QStringLiteral("started"), true},
                     {QStringLiteral("accountId"), accountId}};
}

QJsonValue Daemon::onSyncStatus(const QJsonObject& params, ipc::Error*) const {
  const QString accountId = params.value(QStringLiteral("accountId")).toString();
  if (!accountId.isEmpty()) {
    if (m_database.account(accountId).provider == ProviderKind::Ics) {
      return m_ics.status(accountId);
    }
    return m_sync.status(accountId);
  }
  QJsonObject result = m_sync.status();
  result.insert(QStringLiteral("ics"), m_ics.status());
  return result;
}

QJsonValue Daemon::onSyncCalendar(const QJsonObject& params, ipc::Error* error) {
  QString calendarId;
  if (!validateRequiredString(params, QStringLiteral("calendarId"), &calendarId,
                              error)) {
    return {};
  }
  QString message;
  const Calendar calendar = m_database.calendar(calendarId, &message);
  const Account account = m_database.account(calendar.accountId);
  const bool started = account.provider == ProviderKind::Ics
                           ? m_ics.refresh(account.id, &message)
                           : m_sync.syncCalendar(calendarId, &message);
  if (calendar.id.isEmpty() || !started) {
    if (error != nullptr) {
      *error = {QStringLiteral("sync_failed"), message, false};
    }
    return {};
  }
  return QJsonObject{{QStringLiteral("started"), true},
                     {QStringLiteral("calendarId"), calendarId}};
}

QJsonValue Daemon::onWidgetSnapshot(const QJsonObject& params, ipc::Error* error) {
  const qint64 revision = m_database.changeRevision();
  const qint64 since = params.value(QStringLiteral("sinceRevision")).toInteger(-1);
  const QDateTime now = QDateTime::currentDateTimeUtc();
  QDateTime start = dateTimeFromIso(params.value(QStringLiteral("start")).toString());
  QDateTime end = dateTimeFromIso(params.value(QStringLiteral("end")).toString());
  if (!start.isValid()) {
    start = now.addDays(-7);
  }
  if (!end.isValid()) {
    end = now.addDays(45);
  }
  if (start >= end || start.daysTo(end) > 370) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("Widget snapshot range must be at most one year"),
                false};
    }
    return {};
  }

  QString dbError;
  const QList<CalendarSet> sets = m_database.calendarSets(&dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  QString activeSetId = params.value(QStringLiteral("calendarSetId")).toString();
  if (activeSetId.isEmpty()) {
    activeSetId = m_database
                      .setting(QStringLiteral("active_calendar_set"),
                               QStringLiteral("all-calendars"))
                      .toString();
  }
  CalendarSet activeSet;
  QJsonArray encodedSets;
  for (const CalendarSet& set : sets) {
    encodedSets.append(toJson(set));
    if (set.id == activeSetId) {
      activeSet = set;
    }
  }
  if (activeSet.id.isEmpty() && !sets.isEmpty()) {
    activeSet = sets.first();
    activeSetId = activeSet.id;
  }

  const QList<Calendar> calendars = m_database.calendars({}, &dbError);
  QHash<QString, Calendar> calendarById;
  QJsonArray encodedCalendars;
  for (const Calendar& calendar : calendars) {
    if (calendar.capabilities.value(QStringLiteral("deleted")).toBool() ||
        calendar.capabilities.value(QStringLiteral("calendarListRemoved")).toBool()) {
      continue;
    }
    if (!activeSet.calendarIds.isEmpty() &&
        !activeSet.calendarIds.contains(calendar.id)) {
      continue;
    }
    calendarById.insert(calendar.id, calendar);
    QJsonObject encoded = toJson(calendar);
    encoded.insert(QStringLiteral("color"), calendar.colorOverride.isEmpty()
                                                ? calendar.color
                                                : calendar.colorOverride);
    encoded.insert(QStringLiteral("writable"), !calendar.readOnly);
    encodedCalendars.append(encoded);
  }
  QString defaultCalendarId =
      m_database
          .setting(QStringLiteral("defaultCalendarId"), QStringLiteral("local-default"))
          .toString();
  const auto usableDefault = [&calendarById](const QString& id) {
    const Calendar value = calendarById.value(id);
    return !value.id.isEmpty() && value.enabled && !value.readOnly &&
           !value.capabilities.value(QStringLiteral("deleted")).toBool();
  };
  if (!usableDefault(defaultCalendarId)) {
    defaultCalendarId = usableDefault(activeSet.defaultCalendarId)
                            ? activeSet.defaultCalendarId
                            : QString();
  }
  if (defaultCalendarId.isEmpty()) {
    for (const Calendar& candidate : std::as_const(calendarById)) {
      if (candidate.enabled && !candidate.readOnly &&
          !candidate.capabilities.value(QStringLiteral("deleted")).toBool()) {
        defaultCalendarId = candidate.id;
        if (candidate.capabilities.value(QStringLiteral("primary")).toBool()) break;
      }
    }
  }

  // An explicit set whose members are no longer available represents no
  // calendars, whereas an empty all-calendars set intentionally means every
  // calendar. Avoid passing the former as Database's "all calendars" sentinel.
  const bool explicitEmptyScope =
      !activeSet.calendarIds.isEmpty() && calendarById.isEmpty();
  const QJsonObject coverage =
      explicitEmptyScope
          ? QJsonObject{{QStringLiteral("complete"), true},
                        {QStringLiteral("hydrationScheduled"), false},
                        {QStringLiteral("uncoveredCalendarIds"), QJsonArray{}}}
          : m_sync.ensureRangeHydrated(start, end, calendarById.keys(), &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  if (since >= revision && since >= 0 &&
      coverage.value(QStringLiteral("complete")).toBool()) {
    return QJsonObject{{QStringLiteral("unchanged"), true},
                       {QStringLiteral("revision"), revision},
                       {QStringLiteral("coverage"), coverage}};
  }

  const QString searchQuery =
      params.value(QStringLiteral("searchQuery")).toString().trimmed();
  WidgetEventQueryResult eventQuery;
  if (!explicitEmptyScope) {
    eventQuery = queryWidgetEvents(m_database, start, end, now, calendarById.keys(),
                                   searchQuery, &dbError);
  }
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }

  QHash<QString, qint64> reminderByEvent;
  for (const ReminderJob& reminder : m_database.reminders(500)) {
    if (!reminderByEvent.contains(reminder.eventId)) {
      reminderByEvent.insert(reminder.eventId, reminder.id);
    }
  }
  const auto encodeWidgetEvent = [&calendarById, &reminderByEvent](const Event& event) {
    const Calendar calendar = calendarById.value(event.calendarId);
    QJsonObject encoded{
        {QStringLiteral("id"), event.id},
        {QStringLiteral("localRevision"), event.localRevision},
        {QStringLiteral("calendarId"), event.calendarId},
        {QStringLiteral("title"), event.summary},
        {QStringLiteral("start"), isoUtc(event.startUtc)},
        {QStringLiteral("end"), isoUtc(event.endUtc)},
        {QStringLiteral("startDate"), event.startDate.toString(Qt::ISODate)},
        {QStringLiteral("endDate"), event.endDate.toString(Qt::ISODate)},
        {QStringLiteral("allDay"), event.allDay},
        {QStringLiteral("location"), event.location},
        {QStringLiteral("notes"), event.description},
        {QStringLiteral("meetingUrl"),
         event.conferenceUrl.isEmpty() ? event.url : event.conferenceUrl},
        {QStringLiteral("readOnly"), calendar.readOnly},
        {QStringLiteral("pending"), event.syncState == QStringLiteral("pending")},
        {QStringLiteral("failed"), event.syncState == QStringLiteral("failed") ||
                                       event.syncState == QStringLiteral("blocked")},
        {QStringLiteral("recurring"),
         !event.recurrenceRule.isEmpty() || !event.recurrenceId.isEmpty()},
        {QStringLiteral("recurrenceRule"), event.recurrenceRule},
        {QStringLiteral("recurrenceId"), event.recurrenceId},
        {QStringLiteral("occurrenceStart"), isoUtc(event.startUtc)},
        {QStringLiteral("invitation"),
         !event.organizer.isEmpty() && !event.attendees.isEmpty()},
        {QStringLiteral("organizer"), event.organizer},
        {QStringLiteral("attendees"), event.attendees},
    };
    if (reminderByEvent.contains(event.id)) {
      encoded.insert(QStringLiteral("activeReminderId"),
                     reminderByEvent.value(event.id));
    }
    for (const QJsonValue& attendeeValue : event.attendees) {
      const QJsonObject attendee = attendeeValue.toObject();
      if (attendee.value(QStringLiteral("self")).toBool()) {
        encoded.insert(
            QStringLiteral("attendeeStatus"),
            attendee.value(QStringLiteral("responseStatus"))
                .toString(attendee.value(QStringLiteral("partstat")).toString()));
        break;
      }
    }
    return encoded;
  };

  QJsonArray encodedEvents;
  const qsizetype eventCount = qMin<qsizetype>(eventQuery.events.size(), 1000);
  for (qsizetype i = 0; i < eventCount; ++i) {
    encodedEvents.append(encodeWidgetEvent(eventQuery.events.at(i)));
  }
  const QJsonObject currentEvent = eventQuery.currentEvent.id.isEmpty()
                                       ? QJsonObject{}
                                       : encodeWidgetEvent(eventQuery.currentEvent);
  const QJsonObject upNext = eventQuery.upNext.id.isEmpty()
                                 ? QJsonObject{}
                                 : encodeWidgetEvent(eventQuery.upNext);

  const QJsonObject invitationPage =
      onInvitationsList(QJsonObject{{QStringLiteral("limit"), kWidgetInvitationLimit}},
                        error)
          .toObject();
  const QJsonArray invitations =
      invitationPage.value(QStringLiteral("invitations")).toArray();
  if (error != nullptr && !error->code.isEmpty()) {
    return {};
  }
  QJsonArray conflicts;
  for (const Conflict& conflict : m_database.conflicts(true)) {
    conflicts.append(toJson(conflict));
  }
  QJsonArray operations;
  for (const OutboxItem& operation : m_database.outboxItems(100)) {
    if (operation.state != OutboxState::Done) {
      operations.append(toJson(operation));
    }
  }

  return QJsonObject{
      {QStringLiteral("revision"), revision},
      {QStringLiteral("generatedAt"), isoUtc(now)},
      {QStringLiteral("stale"), !coverage.value(QStringLiteral("complete")).toBool()},
      {QStringLiteral("coverage"), coverage},
      {QStringLiteral("status"), m_sync.status()},
      {QStringLiteral("activeCalendarSet"), toJson(activeSet)},
      {QStringLiteral("defaultCalendarId"), defaultCalendarId},
      {QStringLiteral("calendarSets"), encodedSets},
      {QStringLiteral("calendars"), encodedCalendars},
      {QStringLiteral("events"), encodedEvents},
      {QStringLiteral("currentEvent"), currentEvent},
      {QStringLiteral("upNext"), upNext},
      {QStringLiteral("invitations"), invitations},
      {QStringLiteral("invitationCount"),
       invitationPage.value(QStringLiteral("total"))
           .toInt(static_cast<int>(invitations.size()))},
      {QStringLiteral("invitationsTruncated"),
       invitationPage.value(QStringLiteral("hasMore")).toBool()},
      {QStringLiteral("conflicts"), conflicts},
      {QStringLiteral("operations"), operations},
  };
}

}  // namespace omacalendar
