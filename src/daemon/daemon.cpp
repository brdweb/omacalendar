#include "daemon.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QString>

#include "core/domain.h"
#include "core/paths.h"

namespace {

constexpr int kDefaultEventPageLimit = 500;
constexpr int kMaxEventPageLimit = 5000;

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

}  // namespace

namespace omacalendar {

Daemon::Daemon(QObject* parent)
    : QObject(parent),
      m_server(&m_router, this),
      m_google(&m_database, this),
      m_caldav(&m_database, this) {
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
    m_server.broadcast(
        QStringLiteral("sync.statusChanged"),
        {{QStringLiteral("accountId"), accountId}, {QStringLiteral("status"), status}});
  };
  connect(&m_google, &google::GoogleSync::accountChanged, this, accountChanged);
  connect(&m_google, &google::GoogleSync::calendarsChanged, this, calendarsChanged);
  connect(&m_google, &google::GoogleSync::eventsChanged, this, eventsChanged);
  connect(&m_google, &google::GoogleSync::syncStatusChanged, this, syncChanged);
  connect(&m_caldav, &caldav::CalDavSync::accountChanged, this, accountChanged);
  connect(&m_caldav, &caldav::CalDavSync::calendarsChanged, this, calendarsChanged);
  connect(&m_caldav, &caldav::CalDavSync::eventsChanged, this, eventsChanged);
  connect(&m_caldav, &caldav::CalDavSync::syncStatusChanged, this, syncChanged);
}

bool Daemon::start(QString* errorMessage) {
  if (!paths::ensureDirectories(errorMessage)) {
    return false;
  }
  if (!m_database.open(paths::databaseFile(), errorMessage)) {
    return false;
  }
  QString socketError;
  if (!m_server.listen(paths::socketFile(), &socketError)) {
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
  m_router.registerHandler(QStringLiteral("accounts.list"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onAccountsList(params, error);
                           });
  m_router.registerHandler(QStringLiteral("accounts.createCalDav"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onAccountsCreateCalDav(params, error);
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
  m_router.registerHandler(QStringLiteral("events.search"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onEventsSearch(params, error);
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
  QJsonArray list;
  for (const QString& id : calendarIds) {
    list.append(id);
  }
  const QJsonObject data{
      {QStringLiteral("calendarIds"), list},
      {QStringLiteral("revision"), QDateTime::currentDateTimeUtc().toSecsSinceEpoch()},
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
       }},
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
  const bool removed = account.provider == ProviderKind::Google
                           ? m_google.disconnectAccount(accountId, true, &dbError)
                           : m_caldav.disconnectAccount(accountId, true, &dbError);
  if (!removed) {
    if (error != nullptr) {
      *error = {QStringLiteral("account_remove_failed"), dbError, false};
    }
    return {};
  }
  return QJsonObject{
      {QStringLiteral("ok"), true},
      {QStringLiteral("accountId"), accountId},
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
  if (account.provider == ProviderKind::Google) {
    m_google.syncAccount(accountId);
  } else {
    m_caldav.syncAccount(accountId);
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
  if (!m_database.upsertCalendar(calendar, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"),
                dbError.isEmpty() ? QStringLiteral("Unable to save calendar") : dbError,
                false};
    }
    return {};
  }
  return toJson(calendar);
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
  };
}

QJsonValue Daemon::onEventsGet(const QJsonObject& params, ipc::Error* error) {
  QString eventId;
  if (!validateRequiredString(params, QStringLiteral("eventId"), &eventId, error)) {
    return {};
  }
  QString dbError;
  const Event event = m_database.event(eventId, &dbError);
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
  return toJson(event);
}

QJsonValue Daemon::onEventsCreate(const QJsonObject& params, ipc::Error* error) {
  const QJsonObject eventPayload = params.value(QStringLiteral("event")).toObject();
  if (!params.contains(QStringLiteral("event")) ||
      !params.value(QStringLiteral("event")).isObject() || eventPayload.isEmpty()) {
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
  if (!m_database.saveLocalEvent(&event, OutboxOperation::Create, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"),
                dbError.isEmpty() ? QStringLiteral("Unable to save event") : dbError,
                false};
    }
    return {};
  }
  emitEventsChanged({event.calendarId});
  const Calendar calendar = m_database.calendar(event.calendarId);
  const Account account = m_database.account(calendar.accountId);
  if (account.provider == ProviderKind::Google) {
    m_google.syncAccount(account.id);
  } else if (account.provider == ProviderKind::CalDav) {
    m_caldav.syncAccount(account.id);
  }
  return toJson(event);
}

QJsonValue Daemon::onEventsUpdate(const QJsonObject& params, ipc::Error* error) {
  const QJsonObject eventPayload = params.value(QStringLiteral("event")).toObject();
  if (!params.contains(QStringLiteral("event")) ||
      !params.value(QStringLiteral("event")).isObject() || eventPayload.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("events.update requires {event:{...}} payload"), false};
    }
    return {};
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
  const Event currentEvent = m_database.event(event.id, &currentError);
  if (currentEvent.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Event not found"), false};
    }
    return {};
  }
  if (!currentEvent.recurrenceRule.isEmpty() && currentEvent.recurrenceId.isEmpty() &&
      !event.recurrenceId.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("recurring_instance_edit_not_supported"),
                QStringLiteral("Editing one generated recurrence is not available yet"),
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
  event.remoteId = currentEvent.remoteId;
  event.uid = currentEvent.uid;
  event.etag = currentEvent.etag;
  event.rawPayload = currentEvent.rawPayload;
  event.rawFormat = currentEvent.rawFormat;
  event.createdAt = currentEvent.createdAt;
  const Calendar targetCalendar = m_database.calendar(event.calendarId);
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
  QString dbError;
  if (!m_database.saveLocalEvent(&event, OutboxOperation::Update, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"),
                dbError.isEmpty() ? QStringLiteral("Unable to update event") : dbError,
                false};
    }
    return {};
  }
  emitEventsChanged({event.calendarId});
  const Calendar calendar = m_database.calendar(event.calendarId);
  const Account account = m_database.account(calendar.accountId);
  if (account.provider == ProviderKind::Google) {
    m_google.syncAccount(account.id);
  } else if (account.provider == ProviderKind::CalDav) {
    m_caldav.syncAccount(account.id);
  }
  return toJson(event);
}

QJsonValue Daemon::onEventsRemove(const QJsonObject& params, ipc::Error* error) {
  QString eventId;
  if (!validateRequiredString(params, QStringLiteral("eventId"), &eventId, error)) {
    return {};
  }
  QString dbError;
  const Event event = m_database.event(eventId, &dbError);
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
  const Calendar eventCalendar = m_database.calendar(event.calendarId);
  if (eventCalendar.readOnly) {
    if (error != nullptr) {
      *error = {QStringLiteral("calendar_read_only"),
                QStringLiteral("This calendar is read-only"), false};
    }
    return {};
  }
  QString removeError;
  if (!m_database.markLocalEventRemoved(eventId, &removeError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"),
                removeError.isEmpty() ? QStringLiteral("Unable to remove event")
                                      : removeError,
                false};
    }
    return {};
  }
  const Event updated = m_database.event(eventId, &dbError);
  emitEventsChanged({event.calendarId});
  const Calendar calendar = m_database.calendar(event.calendarId);
  const Account account = m_database.account(calendar.accountId);
  if (account.provider == ProviderKind::Google) {
    m_google.syncAccount(account.id);
  } else if (account.provider == ProviderKind::CalDav) {
    m_caldav.syncAccount(account.id);
  }
  if (!updated.id.isEmpty()) {
    return toJson(updated);
  }
  return toJson(event);
}

QJsonValue Daemon::onEventsSearch(const QJsonObject& params, ipc::Error* error) {
  QJsonValue resultValue = onEventsList(params, error);
  if (error != nullptr && !error->code.isEmpty()) {
    return {};
  }
  const QJsonObject raw = resultValue.toObject();
  const QString query = params.value(QStringLiteral("query")).toString().trimmed();
  if (query.isEmpty()) {
    return raw;
  }
  QJsonArray events = raw.value(QStringLiteral("events")).toArray();
  QJsonArray matches;
  for (const QJsonValue& value : events) {
    const QJsonObject object = value.toObject();
    const QString summary = object.value(QStringLiteral("summary")).toString();
    const QString description = object.value(QStringLiteral("description")).toString();
    const QString location = object.value(QStringLiteral("location")).toString();
    if (summary.contains(query, Qt::CaseInsensitive) ||
        description.contains(query, Qt::CaseInsensitive) ||
        location.contains(query, Qt::CaseInsensitive)) {
      matches.append(object);
    }
  }
  QJsonObject filtered = raw;
  filtered.insert(QStringLiteral("events"), matches);
  filtered.insert(QStringLiteral("total"), static_cast<int>(matches.size()));
  filtered.insert(QStringLiteral("hasMore"), false);
  filtered.remove(QStringLiteral("offset"));
  filtered.insert(QStringLiteral("offset"), 0);
  filtered.remove(QStringLiteral("nextOffset"));
  return filtered;
}

QJsonValue Daemon::onSettingsGet(const QJsonObject& params, ipc::Error* error) {
  QString key;
  if (!validateRequiredString(params, QStringLiteral("key"), &key, error)) {
    return {};
  }
  const QJsonValue fallback = params.value(QStringLiteral("fallback"));
  QString dbError;
  const QJsonValue value = m_database.setting(key, fallback, &dbError);
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
  const QList<OutboxItem> items = m_database.readyOutbox(limit, &dbError);
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
  if (!params.contains(QStringLiteral("id")) ||
      !params.value(QStringLiteral("id")).isDouble()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("id is required for outbox.retry"), false};
    }
    return {};
  }
  const qint64 id = params.value(QStringLiteral("id")).toInteger();
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
  return QJsonObject{
      {QStringLiteral("ok"), true},
      {QStringLiteral("id"), id},
  };
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
  m_google.syncAll();
  m_caldav.syncAll();
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
  if (account.provider == ProviderKind::Google) {
    m_google.syncAccount(accountId);
  } else {
    m_caldav.syncAccount(accountId);
  }
  return QJsonObject{{QStringLiteral("started"), true},
                     {QStringLiteral("accountId"), accountId}};
}

QJsonValue Daemon::onSyncStatus(const QJsonObject& params, ipc::Error*) const {
  const QString accountId = params.value(QStringLiteral("accountId")).toString();
  if (!accountId.isEmpty()) {
    const Account account = m_database.account(accountId);
    return account.provider == ProviderKind::Google ? m_google.status(accountId)
                                                    : m_caldav.status(accountId);
  }
  return QJsonObject{{QStringLiteral("google"), m_google.status()},
                     {QStringLiteral("caldav"), m_caldav.status()}};
}

}  // namespace omacalendar
