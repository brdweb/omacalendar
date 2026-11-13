// settings.*, outbox.*, operations.*, reminders.*, ics.*, import/export,
// google.*, sync.* and freeBusy.* IPC handlers.

#include <QDateTime>
#include <QJsonArray>
#include <QSet>
#include <QTimeZone>
#include <QUuid>
#include <iterator>
#include <utility>

#include "core/freebusy.h"
#include "daemon.h"

namespace omacalendar {

QJsonValue Daemon::onSettingsGet(const QJsonObject& params, ipc::Error* error) {
  QString key;
  if (!validateRequiredString(params, QStringLiteral("key"), &key, error)) {
    return {};
  }
  const QJsonValue value =
      settingValue(key, params.value(QStringLiteral("fallback")), error);
  if (error != nullptr && !error->code.isEmpty()) {
    return {};
  }
  return QJsonObject{
      {QStringLiteral("key"), key},
      {QStringLiteral("value"), value},
  };
}

QJsonValue Daemon::onContactsSuggest(const QJsonObject& params,
                                     ipc::Error* error) const {
  const QString prefix = params.value(QStringLiteral("prefix")).toString().trimmed();
  if (!params.value(QStringLiteral("prefix")).isString() || prefix.isEmpty() ||
      prefix.size() > 200) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("prefix must be a non-empty string"), false};
    }
    return {};
  }
  const int limit = qBound(1, params.value(QStringLiteral("limit")).toInt(8), 25);
  QString dbError;
  const QJsonArray contacts = m_database.contactSuggestions(prefix, limit, &dbError);
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }
  return QJsonObject{{QStringLiteral("prefix"), prefix},
                     {QStringLiteral("contacts"), contacts}};
}

QJsonValue Daemon::onSettingsGetMany(const QJsonObject& params, ipc::Error* error) {
  constexpr qsizetype kMaximumKeys = 64;
  const QJsonArray keys = params.value(QStringLiteral("keys")).toArray();
  if (keys.isEmpty() || keys.size() > kMaximumKeys) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("keys must list between 1 and 64 setting names"), false};
    }
    return {};
  }
  const QJsonObject fallbacks = params.value(QStringLiteral("fallbacks")).toObject();
  QJsonObject values;
  for (const QJsonValue& entry : keys) {
    const QString key = entry.toString().trimmed();
    if (!entry.isString() || key.isEmpty()) {
      if (error != nullptr) {
        *error = {QStringLiteral("invalid_params"),
                  QStringLiteral("Every key must be a non-empty string"), false};
      }
      return {};
    }
    const QJsonValue value = settingValue(key, fallbacks.value(key), error);
    if (error != nullptr && !error->code.isEmpty()) {
      return {};
    }
    values.insert(key, value);
  }
  return QJsonObject{{QStringLiteral("values"), values}};
}

QJsonValue Daemon::settingValue(const QString& key, const QJsonValue& fallback,
                                ipc::Error* error) {
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
  return value;
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

namespace {

QJsonArray busyToJson(const QList<BusyInterval>& intervals) {
  QJsonArray result;
  for (const BusyInterval& interval : intervals) {
    result.append(QJsonObject{{QStringLiteral("start"), isoUtc(interval.start)},
                              {QStringLiteral("end"), isoUtc(interval.end)}});
  }
  return result;
}

}  // namespace

QJsonValue Daemon::onFreeBusyQuery(const QJsonObject& params, ipc::Error* error) {
  const auto invalid = [error](const QString& message) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"), message, false};
    }
    return QJsonValue();
  };
  const QDateTime startUtc =
      dateTimeFromIso(params.value(QStringLiteral("start")).toString()).toUTC();
  const QDateTime endUtc =
      dateTimeFromIso(params.value(QStringLiteral("end")).toString()).toUTC();
  if (!startUtc.isValid() || !endUtc.isValid() || startUtc >= endUtc ||
      startUtc.daysTo(endUtc) > 8) {
    return invalid(
        QStringLiteral("start and end must be ISO-8601 at most 8 days apart"));
  }
  const QJsonValue attendeesValue = params.value(QStringLiteral("attendees"));
  if (!attendeesValue.isUndefined() && !attendeesValue.isArray()) {
    return invalid(QStringLiteral("attendees must be an array of email addresses"));
  }
  QStringList emails;
  for (const QJsonValue& value : attendeesValue.toArray()) {
    const QString email = value.toString().trimmed().toLower();
    if (!value.isString() || email.size() > 320 || !email.contains(QLatin1Char('@'))) {
      return invalid(QStringLiteral("attendees must be email addresses"));
    }
    if (!emails.contains(email)) {
      emails.append(email);
    }
  }
  if (emails.size() > 20) {
    return invalid(QStringLiteral("At most 20 attendees can be checked at once"));
  }

  QString dbError;
  const QString zoneId =
      m_database.setting(QStringLiteral("displayTimeZone"), {}, &dbError).toString();
  QTimeZone zone =
      zoneId.isEmpty() ? QTimeZone(QTimeZone::LocalTime) : QTimeZone(zoneId.toUtf8());
  if (!zone.isValid()) {
    zone = QTimeZone(QTimeZone::LocalTime);
  }
  // The user's own busy time: enabled calendars they can write, excluding
  // subscriptions and other people's shared calendars.
  QHash<QString, ProviderKind> providerByAccount;
  QSet<QString> principals;
  QString googleAccountId;
  const QString preferredCalendarId =
      params.value(QStringLiteral("calendarId")).toString();
  for (const Account& account : m_database.accounts(&dbError)) {
    providerByAccount.insert(account.id, account.provider);
    if (!account.principal.trimmed().isEmpty()) {
      principals.insert(account.principal.trimmed().toLower());
    }
    if (account.provider == ProviderKind::Google && account.enabled &&
        account.authStatus == QStringLiteral("connected") &&
        googleAccountId.isEmpty()) {
      googleAccountId = account.id;
    }
  }
  QStringList ownCalendarIds;
  for (const Calendar& calendar : m_database.calendars({}, &dbError)) {
    if (calendar.enabled && !calendar.readOnly &&
        providerByAccount.value(calendar.accountId) != ProviderKind::Ics) {
      ownCalendarIds.append(calendar.id);
    }
    // Prefer the account of the calendar the event is being saved to.
    if (calendar.id == preferredCalendarId &&
        providerByAccount.value(calendar.accountId) == ProviderKind::Google) {
      googleAccountId = calendar.accountId;
    }
  }
  // The event being edited does not make its own time busy.
  const QString excludeId = params.value(QStringLiteral("excludeEventId")).toString();
  const QString excludeRecurrenceId =
      params.value(QStringLiteral("excludeRecurrenceId")).toString();
  QList<BusyInterval> selfBusy;
  if (!ownCalendarIds.isEmpty()) {
    QList<Event> events =
        m_database.eventsBetween(startUtc, endUtc, ownCalendarIds, &dbError);
    if (!excludeId.isEmpty()) {
      events.removeIf([&excludeId, &excludeRecurrenceId](const Event& event) {
        return event.id == excludeId && (excludeRecurrenceId.isEmpty() ||
                                         event.recurrenceId == excludeRecurrenceId);
      });
    }
    selfBusy = busyIntervalsFromEvents(events, startUtc, endUtc, zone);
  }
  if (!dbError.isEmpty()) {
    if (error != nullptr) {
      *error = {QStringLiteral("database_error"), dbError, false};
    }
    return {};
  }

  const QDateTime now = QDateTime::currentDateTimeUtc();
  // Free/busy visibility differs between accounts, so the account that asked
  // is part of the cache key.
  const QString rangeKey = QLatin1Char('|') + googleAccountId + QLatin1Char('|') +
                           isoUtc(startUtc) + QLatin1Char('|') + isoUtc(endUtc);
  QJsonObject known;
  QStringList remote;
  QJsonArray unavailable;
  for (const QString& email : std::as_const(emails)) {
    if (principals.contains(email)) {
      known.insert(email, busyToJson(selfBusy));
      continue;
    }
    const auto cached = m_freeBusyCache.constFind(email + rangeKey);
    if (cached != m_freeBusyCache.cend() && cached->expiresAt > now) {
      known.insert(email, busyToJson(cached->busy));
    } else if (!googleAccountId.isEmpty()) {
      remote.append(email);
    } else {
      // CalDAV scheduling (RFC 6638) free/busy is not implemented yet.
      unavailable.append(
          QJsonObject{{QStringLiteral("email"), email},
                      {QStringLiteral("reason"), QStringLiteral("unsupported")}});
    }
  }
  const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (!remote.isEmpty()) {
    m_google.queryFreeBusy(
        googleAccountId, remote, startUtc, endUtc,
        [this, requestId, rangeKey](const QHash<QString, QList<BusyInterval>>& busy,
                                    const QHash<QString, QString>& missing) {
          const QDateTime expiresAt = QDateTime::currentDateTimeUtc().addSecs(5 * 60);
          QJsonObject attendees;
          for (auto it = busy.cbegin(); it != busy.cend(); ++it) {
            m_freeBusyCache.insert(it.key() + rangeKey, {it.value(), expiresAt});
            attendees.insert(it.key(), busyToJson(it.value()));
          }
          QJsonArray missingJson;
          for (auto it = missing.cbegin(); it != missing.cend(); ++it) {
            missingJson.append(QJsonObject{{QStringLiteral("email"), it.key()},
                                           {QStringLiteral("reason"), it.value()}});
          }
          // Drop expired answers so the cache stays small.
          const QDateTime current = QDateTime::currentDateTimeUtc();
          for (auto it = m_freeBusyCache.begin(); it != m_freeBusyCache.end();) {
            it = it->expiresAt <= current ? m_freeBusyCache.erase(it) : std::next(it);
          }
          m_server.broadcast(QStringLiteral("events.freeBusy"),
                             {{QStringLiteral("requestId"), requestId},
                              {QStringLiteral("attendees"), attendees},
                              {QStringLiteral("unavailable"), missingJson}});
        });
  }
  return QJsonObject{{QStringLiteral("requestId"), requestId},
                     {QStringLiteral("start"), isoUtc(startUtc)},
                     {QStringLiteral("end"), isoUtc(endUtc)},
                     {QStringLiteral("self"), busyToJson(selfBusy)},
                     {QStringLiteral("attendees"), known},
                     {QStringLiteral("pending"), QJsonArray::fromStringList(remote)},
                     {QStringLiteral("unavailable"), unavailable}};
}

QJsonValue Daemon::onSyncSetInteractive(const QJsonObject& params, ipc::Error* error) {
  if (!params.value(QStringLiteral("interactive")).isBool()) {
    if (error != nullptr) {
      *error = {QStringLiteral("invalid_params"),
                QStringLiteral("interactive must be a boolean"), false};
    }
    return {};
  }
  m_activity.setInteractive(params.value(QStringLiteral("interactive")).toBool());
  return QJsonObject{
      {QStringLiteral("interactive"), m_activity.interactive()},
      {QStringLiteral("pollIntervalSeconds"), m_activity.pollIntervalMs() / 1000},
      {QStringLiteral("leaseSeconds"), SyncActivity::kInteractiveLeaseSeconds}};
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

}  // namespace omacalendar
