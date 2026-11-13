// accounts.*, calendars.* and calendarSets.* IPC handlers.

#include <QDebug>
#include <QJsonArray>

#include "daemon.h"

namespace omacalendar {

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

QJsonValue Daemon::onCalendarsProbeThisAndFuture(const QJsonObject& params,
                                                 ipc::Error* error) {
  QString calendarId;
  if (!validateRequiredString(params, QStringLiteral("calendarId"), &calendarId,
                              error)) {
    return {};
  }
  const Calendar calendar = m_database.calendar(calendarId);
  const Account account = m_database.account(calendar.accountId);
  if (calendar.id.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("not_found"), QStringLiteral("Calendar not found"),
                false};
    }
    return {};
  }
  if (account.provider != ProviderKind::CalDav || calendar.readOnly ||
      !calendar.enabled || !account.enabled) {
    if (error != nullptr) {
      *error = {QStringLiteral("capability_probe_unavailable"),
                QStringLiteral("Choose an enabled, writable CalDAV calendar"), false};
    }
    return {};
  }
  QString message;
  if (!m_caldav.probeThisAndFuture(calendarId, &message)) {
    if (error != nullptr) {
      *error = {QStringLiteral("capability_probe_not_started"), message, true};
    }
    return {};
  }
  return QJsonObject{
      {QStringLiteral("calendarId"), calendarId},
      {QStringLiteral("state"),
       calendar.capabilities.value(QStringLiteral("thisAndFutureProven")).toBool()
           ? QStringLiteral("supported")
           : QStringLiteral("checking")}};
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

}  // namespace omacalendar
