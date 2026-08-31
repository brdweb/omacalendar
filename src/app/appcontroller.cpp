#include "appcontroller.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QTimeZone>
#include <QUrlQuery>
#include <utility>

#include "core/domain.h"
#include "core/paths.h"
#include "providers/google/googleoauthconfig.h"
#include "startuprequest.h"

namespace omacalendar {
namespace {

QVariantList variantList(const QJsonValue& value, const QString& objectKey) {
  if (value.isArray()) {
    return value.toArray().toVariantList();
  }
  if (value.isObject()) {
    return value.toObject().value(objectKey).toArray().toVariantList();
  }
  return {};
}

QDateTime startOfDateUtc(const QDate& date) {
  return QDateTime(date, QTime(0, 0), QTimeZone::UTC);
}

QString normalizedRecurrenceScope(const QString& value) {
  if (value == QStringLiteral("this_occurrence")) {
    return QStringLiteral("occurrence");
  }
  if (value == QStringLiteral("this_and_future")) {
    return QStringLiteral("future");
  }
  if (value == QStringLiteral("entire_series") || value.isEmpty()) {
    return QStringLiteral("series");
  }
  return value;
}

QString normalizedGuestPolicy(const QString& value) {
  return value == QStringLiteral("changed") ? QStringLiteral("all") : value;
}

}  // namespace

AppController::AppController(QObject* parent) : QObject(parent) {
  m_refreshTimer.setSingleShot(true);
  m_refreshTimer.setInterval(120);
  connect(&m_refreshTimer, &QTimer::timeout, this, [this]() {
    if (connected()) {
      refresh();
    }
  });
  m_preferences = {
      {QStringLiteral("firstDayOfWeek"), 0},
      {QStringLiteral("workDayStart"), 8},
      {QStringLiteral("workDayEnd"), 18},
      {QStringLiteral("timeFormat"), QStringLiteral("system")},
      {QStringLiteral("displayTimeZone"), QStringLiteral("")},
      {QStringLiteral("defaultDuration"), 60},
      {QStringLiteral("defaultCalendarId"), QStringLiteral("local-default")},
      {QStringLiteral("notificationPrivacy"), QStringLiteral("full_details")},
      {QStringLiteral("currentView"), QStringLiteral("month")},
      {QStringLiteral("widgetConsentDecision"), QStringLiteral("")}};
  m_client.setAutoReconnect(true);
  connect(&m_client, &ipc::IpcClient::connectedChanged, this, [this]() {
    emit connectedChanged();
    if (connected()) {
      m_daemonStartAttempted = false;
      setError({});
      setStatus(QStringLiteral("Calendar service connected"));
      QJsonObject subscription{
          {QStringLiteral("topics"),
           QJsonArray{QStringLiteral("accounts"), QStringLiteral("calendars"),
                      QStringLiteral("calendarSets"), QStringLiteral("events"),
                      QStringLiteral("invitations"), QStringLiteral("reminders"),
                      QStringLiteral("sync"), QStringLiteral("google")}}};
      if (m_subscriptionRevision >= 0) {
        subscription.insert(QStringLiteral("sinceRevision"), m_subscriptionRevision);
      }
      send(QStringLiteral("system.subscribe"), subscription,
           [this](const QJsonValue& value) {
             const QJsonObject result = value.toObject();
             m_subscriptionRevision = result.value(QStringLiteral("revision"))
                                          .toInteger(m_subscriptionRevision);
             if (result.value(QStringLiteral("catchUpRequired")).toBool()) {
               refresh();
             }
           });
      refresh();
      processPendingDeepLink();
    } else {
      m_pending.clear();
      m_backgroundRequests.clear();
      m_refreshTimer.stop();
      m_activeRequests = 0;
      setBusy(false);
      setStatus(QStringLiteral("Reconnecting to calendar service…"));
      QTimer::singleShot(500, this, &AppController::startDaemonIfNeeded);
    }
  });
  connect(&m_client, &ipc::IpcClient::responseReceived, this,
          [this](const QString& id, const QJsonValue& result) {
            const ResultHandler handler = m_pending.take(id);
            const bool background = m_backgroundRequests.remove(id);
            if (!background && m_activeRequests > 0) {
              --m_activeRequests;
              setBusy(m_activeRequests > 0);
            }
            if (handler) {
              handler(result);
            }
          });
  connect(&m_client, &ipc::IpcClient::errorReceived, this,
          [this](const QString& id, const QJsonObject& error) {
            m_pending.remove(id);
            const bool background = m_backgroundRequests.remove(id);
            if (!background && m_activeRequests > 0) {
              --m_activeRequests;
              setBusy(m_activeRequests > 0);
            }
            setError(error.value(QStringLiteral("message"))
                         .toString(QStringLiteral("Calendar service request failed")));
          });
  connect(
      &m_client, &ipc::IpcClient::notificationReceived, this,
      [this](const QString& event, const QJsonObject& data) {
        const qint64 revision = data.value(QStringLiteral("revision")).toInteger(-1);
        if (revision > m_subscriptionRevision) {
          m_subscriptionRevision = revision;
        }
        if (event == QStringLiteral("google.oauthUrl")) {
          const QUrl url(data.value(QStringLiteral("url")).toString());
          if (url.isValid()) {
            QDesktopServices::openUrl(url);
            setStatus(QStringLiteral("Complete Google sign-in in your browser"));
          }
          return;
        }
        if (event == QStringLiteral("events.changed") ||
            event == QStringLiteral("calendars.changed") ||
            event == QStringLiteral("accounts.changed") ||
            event == QStringLiteral("calendarSets.changed") ||
            event == QStringLiteral("invitations.changed") ||
            event == QStringLiteral("reminders.changed")) {
          m_refreshTimer.start();
        } else if (event == QStringLiteral("sync.statusChanged")) {
          const QJsonObject status = data.value(QStringLiteral("status")).toObject();
          const QString state = status.value(QStringLiteral("state")).toString();
          if (state == QStringLiteral("error") ||
              state == QStringLiteral("reauthorization_required")) {
            setError(status.value(QStringLiteral("message")).toString());
          } else if (state == QStringLiteral("syncing")) {
            setStatus(QStringLiteral("Synchronizing calendars…"));
          } else if (state == QStringLiteral("idle")) {
            setError({});
            setStatus(QStringLiteral("Synchronization complete"));
          }
        }
      });
  connect(&m_client, &ipc::IpcClient::protocolError, this,
          [this](const QString& message) { setError(message); });

  reconnect();
  refreshWidgetStatus();
  if (!qEnvironmentVariableIsSet("OMACALENDAR_DISABLE_DAEMON_AUTOSTART")) {
    QTimer::singleShot(600, this, &AppController::startDaemonIfNeeded);
  }
}

QString AppController::systemTimeZoneId() const {
  const QByteArray id = QTimeZone::systemTimeZoneId();
  return id.isEmpty() ? QStringLiteral("UTC") : QString::fromUtf8(id);
}

QStringList AppController::availableTimeZoneIds() const {
  QStringList result;
  const QString systemId = systemTimeZoneId();
  result.push_back(systemId);
  if (systemId != QStringLiteral("UTC")) {
    result.push_back(QStringLiteral("UTC"));
  }
  const QList<QByteArray> ids = QTimeZone::availableTimeZoneIds();
  result.reserve(ids.size() + result.size());
  for (const QByteArray& id : ids) {
    const QString value = QString::fromUtf8(id);
    if (!result.contains(value)) {
      result.push_back(value);
    }
  }
  return result;
}

bool AppController::bundledGoogleOAuthAvailable() const {
  return !google::defaultOAuthClientId().isEmpty();
}

bool AppController::googleOAuthConfigured() const { return m_googleOAuthConfigured; }

bool AppController::connected() const { return m_client.isConnected(); }
bool AppController::busy() const { return m_activeRequests > 0; }
QString AppController::statusText() const { return m_statusText; }
QString AppController::lastError() const { return m_lastError; }
QVariantList AppController::accounts() const { return m_accounts; }
QVariantList AppController::calendars() const { return m_calendars; }
QVariantList AppController::events() const { return m_events; }
QVariantList AppController::calendarSets() const { return m_calendarSets; }
QVariantList AppController::invitations() const { return m_invitations; }
QVariantList AppController::conflicts() const { return m_conflicts; }
QVariantList AppController::operations() const { return m_operations; }
QVariantList AppController::searchResults() const { return m_searchResults; }
PresentationListModel* AppController::accountsModel() { return &m_accountsModel; }
PresentationListModel* AppController::calendarsModel() { return &m_calendarsModel; }
PresentationListModel* AppController::eventsModel() { return &m_eventsModel; }
PresentationListModel* AppController::calendarSetsModel() {
  return &m_calendarSetsModel;
}
PresentationListModel* AppController::invitationsModel() { return &m_invitationsModel; }
PresentationListModel* AppController::conflictsModel() { return &m_conflictsModel; }
PresentationListModel* AppController::operationsModel() { return &m_operationsModel; }
PresentationListModel* AppController::searchResultsModel() {
  return &m_searchResultsModel;
}
QVariantMap AppController::preferences() const { return m_preferences; }
bool AppController::widgetInstalled() const { return m_widgetInstalled; }
QString AppController::activeCalendarSetId() const { return m_activeCalendarSetId; }
bool AppController::preferencesLoaded() const { return m_preferencesLoaded; }
QDate AppController::selectedDate() const { return m_selectedDate; }

void AppController::reconnect() { m_client.connectTo(paths::socketFile()); }

void AppController::startDaemonIfNeeded() {
  if (connected() || m_daemonStartAttempted) {
    return;
  }
  m_daemonStartAttempted = true;
  const QString applicationDirectory = QCoreApplication::applicationDirPath();
  const QStringList localCandidates = {
      QDir(applicationDirectory).filePath(QStringLiteral("omacalendard")),
      QDir(applicationDirectory).filePath(QStringLiteral("../../omacalendard")),
  };
  QString executable;
  for (const QString& candidate : localCandidates) {
    const QFileInfo info(candidate);
    if (info.isExecutable()) {
      executable = info.absoluteFilePath();
      break;
    }
  }
  if (executable.isEmpty()) {
    executable = QStandardPaths::findExecutable(QStringLiteral("omacalendard"));
  }
  if (!executable.isEmpty()) {
    QProcess::startDetached(executable, {});
    QTimer::singleShot(400, this, &AppController::reconnect);
  } else {
    setError(QStringLiteral("omacalendard was not found. Start it, then retry."));
  }
}

QString AppController::send(const QString& method, const QJsonObject& params,
                            ResultHandler handler, const bool contributesToBusy) {
  if (!connected()) {
    setError(QStringLiteral("Calendar service is not connected"));
    startDaemonIfNeeded();
    return {};
  }
  const QString id = m_client.request(method, params);
  if (id.isEmpty()) {
    setError(QStringLiteral("Could not send request to calendar service"));
    return {};
  }
  m_pending.insert(id, std::move(handler));
  if (contributesToBusy) {
    ++m_activeRequests;
    setBusy(true);
  } else {
    m_backgroundRequests.insert(id);
  }
  return id;
}

void AppController::refresh() {
  if (!connected()) {
    return;
  }
  const auto background = [this](const QString& method, const QJsonObject& params,
                                 ResultHandler handler) {
    send(method, params, std::move(handler), false);
  };
  background(QStringLiteral("system.info"), {}, [this](const QJsonValue& value) {
    const bool configured = value.toObject()
                                .value(QStringLiteral("providers"))
                                .toObject()
                                .value(QStringLiteral("google"))
                                .toObject()
                                .value(QStringLiteral("configured"))
                                .toBool();
    if (configured != m_googleOAuthConfigured) {
      m_googleOAuthConfigured = configured;
      emit googleOAuthConfiguredChanged();
    }
  });
  background(QStringLiteral("accounts.list"), {}, [this](const QJsonValue& value) {
    m_accounts = variantList(value, QStringLiteral("accounts"));
    m_accountsModel.replace(m_accounts);
    emit accountsChanged();
  });
  background(QStringLiteral("calendars.list"), {}, [this](const QJsonValue& value) {
    m_calendars = variantList(value, QStringLiteral("calendars"));
    m_calendarsModel.replace(m_calendars);
    emit calendarsChanged();
  });
  background(QStringLiteral("calendarSets.list"), {}, [this](const QJsonValue& value) {
    m_calendarSets = variantList(value, QStringLiteral("calendarSets"));
    m_calendarSetsModel.replace(m_calendarSets);
    const QString active = value.toObject()
                               .value(QStringLiteral("activeId"))
                               .toString(QStringLiteral("all-calendars"));
    if (active != m_activeCalendarSetId) {
      m_activeCalendarSetId = active;
      emit activeCalendarSetIdChanged();
    }
    emit calendarSetsChanged();
  });
  background(QStringLiteral("invitations.list"), {}, [this](const QJsonValue& value) {
    m_invitations = variantList(value, QStringLiteral("invitations"));
    applyDisplayTimes(&m_invitations);
    m_invitationsModel.replace(m_invitations);
    emit invitationsChanged();
  });
  const QStringList preferenceKeys = {
      QStringLiteral("firstDayOfWeek"),    QStringLiteral("workDayStart"),
      QStringLiteral("workDayEnd"),        QStringLiteral("timeFormat"),
      QStringLiteral("displayTimeZone"),   QStringLiteral("defaultDuration"),
      QStringLiteral("defaultCalendarId"), QStringLiteral("notificationPrivacy"),
      QStringLiteral("currentView"),       QStringLiteral("widgetConsentDecision")};
  for (const QString& key : preferenceKeys) {
    background(
        QStringLiteral("settings.get"),
        {{QStringLiteral("key"), key},
         {QStringLiteral("fallback"),
          QJsonValue::fromVariant(m_preferences.value(key))}},
        [this, key](const QJsonValue& value) {
          m_preferences.insert(
              key, value.toObject().value(QStringLiteral("value")).toVariant());
          if (key == QStringLiteral("widgetConsentDecision") && !m_preferencesLoaded) {
            m_preferencesLoaded = true;
            emit preferencesLoadedChanged();
          }
          emit preferencesChanged();
        });
  }
  if (!m_rangeStart.isValid() || !m_rangeEnd.isValid()) {
    m_rangeStart = QDate::currentDate().addDays(-14);
    m_rangeEnd = QDate::currentDate().addDays(45);
  }
  loadRange(m_rangeStart, m_rangeEnd);
}

void AppController::loadRange(const QDate& firstDate, const QDate& lastDate) {
  if (!firstDate.isValid() || !lastDate.isValid() || firstDate > lastDate) {
    setError(QStringLiteral("A valid date range is required"));
    return;
  }
  m_rangeStart = firstDate;
  m_rangeEnd = lastDate;
  const QJsonObject params = {
      {QStringLiteral("start"), isoUtc(startOfDateUtc(firstDate))},
      {QStringLiteral("end"), isoUtc(startOfDateUtc(lastDate.addDays(1)))},
  };
  send(
      QStringLiteral("events.list"), params,
      [this](const QJsonValue& value) {
        m_events = variantList(value, QStringLiteral("events"));
        applyDisplayTimes(&m_events);
        m_eventsModel.replace(m_events);
        setStatus(QStringLiteral("Calendar is up to date locally"));
        emit eventsChanged();
      },
      false);
}

void AppController::createEvent(const QVariantMap& values) { saveEvent(values, {}); }

void AppController::updateEvent(const QVariantMap& values) { saveEvent(values, {}); }

void AppController::removeEvent(const QString& eventId) {
  requestDeleteEvent(
      eventId, {{QStringLiteral("recurrenceScope"), QStringLiteral("series")},
                {QStringLiteral("guestNotificationPolicy"), QStringLiteral("none")}});
}

void AppController::saveEvent(const QVariantMap& values,
                              const QVariantMap& mutationOptions) {
  const QJsonObject event = QJsonObject::fromVariantMap(values);
  const bool updating = !event.value(QStringLiteral("id")).toString().isEmpty();
  const QString mutationId = newUuid();
  QVariantMap priorEvent;
  if (updating) {
    const QString eventId = event.value(QStringLiteral("id")).toString();
    const QString recurrenceId = event.value(QStringLiteral("recurrenceId")).toString();
    for (const QVariant& candidateValue : std::as_const(m_events)) {
      const QVariantMap candidate = candidateValue.toMap();
      if (candidate.value(QStringLiteral("id")).toString() == eventId &&
          recurrenceIdentityEqual(
              recurrenceId, candidate.value(QStringLiteral("recurrenceId")).toString(),
              candidate.value(QStringLiteral("allDay")).toBool(),
              timeKindFromString(
                  candidate.value(QStringLiteral("timeKind")).toString()),
              candidate.value(QStringLiteral("startTimeZone")).toString())) {
        priorEvent = candidate;
        break;
      }
    }
  }
  QString sourceCalendarId =
      mutationOptions.value(QStringLiteral("sourceCalendarId")).toString();
  if (updating && sourceCalendarId.isEmpty()) {
    const QString eventId = event.value(QStringLiteral("id")).toString();
    for (const QVariant& candidateValue : std::as_const(m_events)) {
      const QVariantMap candidate = candidateValue.toMap();
      if (candidate.value(QStringLiteral("id")).toString() == eventId) {
        sourceCalendarId = candidate.value(QStringLiteral("calendarId")).toString();
        break;
      }
    }
  }
  const QString targetCalendarId = event.value(QStringLiteral("calendarId")).toString();
  QVariantMap inverseOptions = mutationOptions;
  inverseOptions.insert(QStringLiteral("sourceCalendarId"), targetCalendarId);
  inverseOptions.insert(QStringLiteral("confirmedCrossProvider"), true);
  const bool calendarChanged =
      updating && !sourceCalendarId.isEmpty() && sourceCalendarId != targetCalendarId;
  const ResultHandler saved = [this, updating, priorEvent, inverseOptions,
                               calendarChanged](const QJsonValue& value) {
    m_lastMutationId.clear();
    m_lastUndoToken.clear();
    if (updating && !priorEvent.isEmpty() && !calendarChanged) {
      QVariantMap inverseEvent = priorEvent;
      QJsonObject responseEvent = value.toObject();
      if (responseEvent.value(QStringLiteral("event")).isObject()) {
        responseEvent = responseEvent.value(QStringLiteral("event")).toObject();
      }
      if (!responseEvent.value(QStringLiteral("id")).toString().isEmpty()) {
        inverseEvent.insert(QStringLiteral("id"),
                            responseEvent.value(QStringLiteral("id")).toVariant());
      }
      if (responseEvent.contains(QStringLiteral("localRevision"))) {
        inverseEvent.insert(
            QStringLiteral("localRevision"),
            responseEvent.value(QStringLiteral("localRevision")).toVariant());
      }
      m_lastUndoKind = QStringLiteral("update");
      m_lastUndoEvent = inverseEvent;
      m_lastUndoOptions = inverseOptions;
    } else if (!updating) {
      m_lastUndoKind = QStringLiteral("create");
      m_lastUndoEvent = value.toObject().toVariantMap();
      m_lastUndoOptions = inverseOptions;
    } else {
      m_lastUndoKind.clear();
      m_lastUndoEvent.clear();
      m_lastUndoOptions.clear();
    }
    setStatus(calendarChanged ? QStringLiteral("Event moved")
                              : QStringLiteral("Saved — press Ctrl+Z to undo"));
    emit eventSaved();
    loadRange(m_rangeStart, m_rangeEnd);
  };
  if (updating && !sourceCalendarId.isEmpty() && !targetCalendarId.isEmpty() &&
      sourceCalendarId != targetCalendarId) {
    QJsonObject eventReference{
        {QStringLiteral("eventId"), event.value(QStringLiteral("id")).toString()}};
    if (!event.value(QStringLiteral("recurrenceId")).toString().isEmpty()) {
      eventReference.insert(QStringLiteral("recurrenceId"),
                            event.value(QStringLiteral("recurrenceId")));
    }
    const QJsonObject params{
        {QStringLiteral("eventRef"), eventReference},
        {QStringLiteral("targetCalendarId"), targetCalendarId},
        {QStringLiteral("draft"), event},
        {QStringLiteral("clientMutationId"), mutationId},
        {QStringLiteral("expectedLocalRevision"),
         event.value(QStringLiteral("localRevision")).toInteger(-1)},
        {QStringLiteral("recurrenceScope"),
         normalizedRecurrenceScope(
             mutationOptions.value(QStringLiteral("recurrenceScope")).toString())},
        {QStringLiteral("guestNotificationPolicy"),
         normalizedGuestPolicy(mutationOptions
                                   .value(QStringLiteral("guestNotificationPolicy"),
                                          QStringLiteral("none"))
                                   .toString())},
        {QStringLiteral("confirmedCrossProvider"),
         mutationOptions.value(QStringLiteral("confirmedCrossProvider")).toBool()},
    };
    send(QStringLiteral("events.move"), params, saved);
    return;
  }
  QJsonObject params{
      {QStringLiteral("event"), event},
      {QStringLiteral("clientMutationId"), mutationId},
      {QStringLiteral("expectedLocalRevision"),
       event.value(QStringLiteral("localRevision")).toInteger(updating ? -1 : 0)},
      {QStringLiteral("recurrenceScope"),
       normalizedRecurrenceScope(
           mutationOptions.value(QStringLiteral("recurrenceScope")).toString())},
      {QStringLiteral("guestNotificationPolicy"),
       normalizedGuestPolicy(
           mutationOptions
               .value(QStringLiteral("guestNotificationPolicy"), QStringLiteral("none"))
               .toString())},
  };
  send(updating ? QStringLiteral("events.update") : QStringLiteral("events.create"),
       params, saved);
}

void AppController::requestDeleteEvent(const QString& eventId,
                                       const QVariantMap& mutationOptions) {
  if (eventId.isEmpty()) {
    return;
  }
  const QString mutationId = newUuid();
  qint64 expectedRevision =
      mutationOptions.value(QStringLiteral("expectedLocalRevision"), -1).toLongLong();
  QString recurrenceId =
      mutationOptions.value(QStringLiteral("recurrenceId")).toString();
  if (expectedRevision < 0) {
    for (const QVariant& candidateValue : std::as_const(m_events)) {
      const QVariantMap candidate = candidateValue.toMap();
      if (candidate.value(QStringLiteral("id")).toString() == eventId) {
        expectedRevision =
            candidate.value(QStringLiteral("localRevision"), -1).toLongLong();
        if (recurrenceId.isEmpty()) {
          recurrenceId = candidate.value(QStringLiteral("recurrenceId")).toString();
        }
        break;
      }
    }
  }
  QJsonObject eventReference{{QStringLiteral("eventId"), eventId}};
  if (!recurrenceId.isEmpty()) {
    eventReference.insert(QStringLiteral("recurrenceId"), recurrenceId);
  }
  const QJsonObject params{
      {QStringLiteral("eventRef"), eventReference},
      {QStringLiteral("clientMutationId"), mutationId},
      {QStringLiteral("expectedLocalRevision"), expectedRevision},
      {QStringLiteral("recurrenceScope"),
       normalizedRecurrenceScope(
           mutationOptions.value(QStringLiteral("recurrenceScope")).toString())},
      {QStringLiteral("guestNotificationPolicy"),
       normalizedGuestPolicy(
           mutationOptions
               .value(QStringLiteral("guestNotificationPolicy"), QStringLiteral("none"))
               .toString())},
  };
  send(QStringLiteral("events.remove"), params,
       [this, mutationId](const QJsonValue& value) {
         m_lastMutationId = mutationId;
         m_lastUndoKind.clear();
         m_lastUndoEvent.clear();
         m_lastUndoOptions.clear();
         m_lastUndoToken =
             value.toObject().value(QStringLiteral("undoToken")).toString();
         setStatus(QStringLiteral("Delete queued — press Ctrl+Z within 10 seconds"));
         const QString undoToken = m_lastUndoToken;
         QTimer::singleShot(10000, this, [this, mutationId, undoToken]() {
           if (m_lastUndoToken == undoToken) {
             m_lastUndoToken.clear();
             if (m_lastMutationId == mutationId) {
               m_lastMutationId.clear();
             }
           }
         });
         loadRange(m_rangeStart, m_rangeEnd);
       });
}

void AppController::searchEvents(const QString& query, const QVariantMap& filters) {
  if (query.trimmed().isEmpty()) {
    m_searchResults.clear();
    m_searchResultsModel.clear();
    emit searchResultsChanged();
    return;
  }
  QJsonObject params = QJsonObject::fromVariantMap(filters);
  params.insert(QStringLiteral("query"), query.trimmed());
  params.insert(QStringLiteral("limit"), 200);
  send(QStringLiteral("events.search"), params, [this](const QJsonValue& value) {
    m_searchResults = variantList(value, QStringLiteral("events"));
    applyDisplayTimes(&m_searchResults);
    m_searchResultsModel.replace(m_searchResults);
    emit searchResultsChanged();
  });
}

void AppController::respondToInvitation(const QString& eventId, const QString& response,
                                        const QString& recurrenceScope,
                                        const QString& recurrenceId,
                                        const qint64 expectedLocalRevision) {
  const QString mutationId = newUuid();
  QJsonObject eventReference{{QStringLiteral("eventId"), eventId}};
  if (!recurrenceId.trimmed().isEmpty()) {
    eventReference.insert(QStringLiteral("recurrenceId"), recurrenceId.trimmed());
  }
  removeInvitation(eventId, recurrenceId);
  send(
      QStringLiteral("events.respond"),
      {{QStringLiteral("eventRef"), eventReference},
       {QStringLiteral("response"), response},
       {QStringLiteral("recurrenceScope"), normalizedRecurrenceScope(recurrenceScope)},
       {QStringLiteral("expectedLocalRevision"), expectedLocalRevision},
       {QStringLiteral("guestNotificationPolicy"), QStringLiteral("all")},
       {QStringLiteral("clientMutationId"), mutationId}},
      [this, mutationId](const QJsonValue&) {
        m_lastMutationId = mutationId;
        refresh();
      },
      false);
}

void AppController::removeInvitation(const QString& eventId,
                                     const QString& recurrenceId) {
  QVariantList retained;
  retained.reserve(m_invitations.size());
  for (const QVariant& row : std::as_const(m_invitations)) {
    const QVariantMap invitation = row.toMap();
    if (invitation.value(QStringLiteral("id")).toString() == eventId &&
        invitation.value(QStringLiteral("recurrenceId")).toString() == recurrenceId) {
      continue;
    }
    retained.append(row);
  }
  if (retained.size() == m_invitations.size()) {
    return;
  }
  m_invitations = std::move(retained);
  m_invitationsModel.replace(m_invitations);
  emit invitationsChanged();
}

void AppController::markInvitationSeen(const QString& eventId) {
  send(QStringLiteral("invitations.markSeen"), {{QStringLiteral("eventId"), eventId}},
       [this](const QJsonValue&) { refresh(); });
}

void AppController::resolveConflict(const QString& conflictId, const QString& strategy,
                                    const QVariantMap& mergedDraft) {
  bool ok = false;
  const qint64 id = conflictId.toLongLong(&ok);
  if (!ok) {
    setError(QStringLiteral("Invalid conflict identifier"));
    return;
  }
  send(QStringLiteral("conflicts.resolve"),
       {{QStringLiteral("id"), id},
        {QStringLiteral("strategy"), strategy},
        {QStringLiteral("mergedEvent"), QJsonObject::fromVariantMap(mergedDraft)}},
       [this](const QJsonValue&) { refresh(); });
}

void AppController::retryOperation(const QString& operationId) {
  bool ok = false;
  const qint64 id = operationId.toLongLong(&ok);
  if (!ok) {
    return;
  }
  send(QStringLiteral("operations.retry"),
       {{QStringLiteral("operationId"), id},
        {QStringLiteral("clientMutationId"), newUuid()}},
       [this](const QJsonValue&) { refresh(); });
}

void AppController::discardOperation(const QString& operationId) {
  bool ok = false;
  const qint64 id = operationId.toLongLong(&ok);
  if (!ok) {
    return;
  }
  send(QStringLiteral("operations.discard"), {{QStringLiteral("operationId"), id}},
       [this](const QJsonValue&) { refresh(); });
}

void AppController::addLocalCalendar(const QString& name, const QString& color) {
  if (name.trimmed().isEmpty()) {
    setError(QStringLiteral("Local calendar name is required"));
    return;
  }
  const QJsonObject calendar{
      {QStringLiteral("accountId"), QStringLiteral("local-account")},
      {QStringLiteral("name"), name.trimmed()},
      {QStringLiteral("color"), color}};
  send(QStringLiteral("calendars.upsert"), {{QStringLiteral("calendar"), calendar}},
       [this](const QJsonValue&) { refresh(); });
}

void AppController::removeCalendar(const QString& calendarId) {
  if (calendarId.isEmpty() || calendarId == QStringLiteral("local-default")) {
    return;
  }
  send(QStringLiteral("calendars.remove"),
       {{QStringLiteral("calendarId"), calendarId},
        {QStringLiteral("confirmed"), true},
        {QStringLiteral("clientMutationId"), newUuid()}},
       [this](const QJsonValue& value) {
         const bool pending =
             value.toObject().value(QStringLiteral("accepted")).toBool();
         setStatus(pending ? QStringLiteral("Deleting calendar…")
                           : QStringLiteral("Calendar deleted"));
         refresh();
       });
}

void AppController::removeLocalCalendar(const QString& calendarId) {
  removeCalendar(calendarId);
}

void AppController::addIcsSubscription(const QVariantMap& config) {
  QJsonObject params = QJsonObject::fromVariantMap(config);
  params.insert(QStringLiteral("clientMutationId"), newUuid());
  send(QStringLiteral("accounts.addIcs"), params, [this](const QJsonValue&) {
    setStatus(QStringLiteral("Refreshing calendar subscription…"));
    refresh();
  });
}

void AppController::previewIcsImport(const QUrl& file,
                                     const QString& destinationCalendarId) {
  const QString path = file.toLocalFile();
  if (path.isEmpty() || destinationCalendarId.trimmed().isEmpty()) {
    setError(QStringLiteral("Choose an iCalendar file and writable destination"));
    return;
  }
  send(QStringLiteral("import.preview"),
       {{QStringLiteral("path"), path},
        {QStringLiteral("destinationCalendarId"), destinationCalendarId}},
       [this](const QJsonValue& value) {
         const QVariantMap preview = value.toObject().toVariantMap();
         setStatus(QStringLiteral("Import preview: %1 event(s), %2 duplicate(s)")
                       .arg(preview.value(QStringLiteral("count")).toInt())
                       .arg(preview.value(QStringLiteral("duplicateCount")).toInt()));
         emit icsImportPreviewReady(preview);
       });
}

void AppController::commitIcsImport(const QUrl& file,
                                    const QString& destinationCalendarId,
                                    const QString& duplicatePolicy) {
  const QString path = file.toLocalFile();
  if (path.isEmpty() || destinationCalendarId.trimmed().isEmpty()) {
    setError(QStringLiteral("Choose an iCalendar file and writable destination"));
    return;
  }
  send(QStringLiteral("import.commit"),
       {{QStringLiteral("path"), path},
        {QStringLiteral("destinationCalendarId"), destinationCalendarId},
        {QStringLiteral("duplicatePolicy"), duplicatePolicy}},
       [this](const QJsonValue& value) {
         const QVariantMap result = value.toObject().toVariantMap();
         setStatus(QStringLiteral("Imported %1 event(s); skipped %2; replaced %3")
                       .arg(result.value(QStringLiteral("imported")).toInt())
                       .arg(result.value(QStringLiteral("skipped")).toInt())
                       .arg(result.value(QStringLiteral("replaced")).toInt()));
         emit icsImportCompleted(result);
         loadRange(m_rangeStart, m_rangeEnd);
       });
}

void AppController::exportIcs(const QVariantMap& scope, const QUrl& destination) {
  QString path = destination.toLocalFile();
  if (path.isEmpty()) {
    setError(QStringLiteral("Choose a local destination for the iCalendar export"));
    return;
  }
  if (!path.endsWith(QStringLiteral(".ics"), Qt::CaseInsensitive)) {
    path.append(QStringLiteral(".ics"));
  }
  QJsonObject params = QJsonObject::fromVariantMap(scope);
  params.insert(QStringLiteral("outputPath"), path);
  // FileDialog's save flow is the user's overwrite confirmation.
  params.insert(QStringLiteral("overwrite"), true);
  send(QStringLiteral("export.run"), params, [this](const QJsonValue& value) {
    const QVariantMap result = value.toObject().toVariantMap();
    setStatus(QStringLiteral("Exported %1 event(s) to %2")
                  .arg(result.value(QStringLiteral("count")).toInt())
                  .arg(result.value(QStringLiteral("path")).toString()));
    emit icsExportCompleted(result);
  });
}

void AppController::connectGoogle(const QString& displayName) {
  const QString clientId = google::defaultOAuthClientId();
  if (clientId.isEmpty()) {
    setError(QStringLiteral(
        "This build has no bundled Google OAuth client. Enter a Desktop OAuth "
        "client ID in Accounts & settings."));
    return;
  }
  setError({});
  send(QStringLiteral("google.configureClient"),
       {{QStringLiteral("clientId"), clientId},
        {QStringLiteral("clientSecret"), google::defaultOAuthClientSecret()}},
       [this, displayName](const QJsonValue&) {
         if (!m_googleOAuthConfigured) {
           m_googleOAuthConfigured = true;
           emit googleOAuthConfiguredChanged();
         }
         send(QStringLiteral("google.oauthStart"),
              {{QStringLiteral("displayName"), displayName.trimmed()}},
              [this](const QJsonValue&) {
                setStatus(QStringLiteral("Opening Google authorization…"));
                emit accountSetupStarted();
                refresh();
              });
       });
}

void AppController::connectGoogleConfigured(const QString& displayName) {
  setError({});
  send(QStringLiteral("google.oauthStart"),
       {{QStringLiteral("displayName"), displayName.trimmed()}},
       [this](const QJsonValue&) {
         setStatus(QStringLiteral("Opening Google authorization…"));
         emit accountSetupStarted();
         refresh();
       });
}

void AppController::connectGoogleWithClientId(const QString& clientId,
                                              const QString& displayName) {
  const QString normalizedClientId = clientId.trimmed();
  if (normalizedClientId.isEmpty()) {
    setError(QStringLiteral("Enter a Google Desktop OAuth client ID"));
    return;
  }
  setError({});
  send(QStringLiteral("google.configureClient"),
       {{QStringLiteral("clientId"), normalizedClientId},
        {QStringLiteral("clientSecret"), QString()}},
       [this, displayName](const QJsonValue&) {
         if (!m_googleOAuthConfigured) {
           m_googleOAuthConfigured = true;
           emit googleOAuthConfiguredChanged();
         }
         send(QStringLiteral("google.oauthStart"),
              {{QStringLiteral("displayName"), displayName.trimmed()}},
              [this](const QJsonValue&) {
                setStatus(QStringLiteral("Opening Google authorization…"));
                emit accountSetupStarted();
                refresh();
              });
       });
}

void AppController::connectGoogleWithCredentials(const QUrl& credentialsFile,
                                                 const QString& displayName) {
  const QString path = credentialsFile.isLocalFile() ? credentialsFile.toLocalFile()
                                                     : credentialsFile.toString();
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    setError(QStringLiteral("Could not read the selected Google credentials file"));
    return;
  }
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    setError(QStringLiteral("The selected file is not valid Google credentials JSON"));
    return;
  }
  const QJsonObject root = document.object();
  const QJsonObject installed = root.value(QStringLiteral("installed")).toObject();
  const QString clientId = installed.value(QStringLiteral("client_id")).toString();
  const QString clientSecret =
      installed.value(QStringLiteral("client_secret")).toString();
  if (clientId.isEmpty()) {
    setError(
        QStringLiteral("Choose OAuth credentials created as a Google Desktop app"));
    return;
  }
  setError({});
  send(QStringLiteral("google.configureClient"),
       {{QStringLiteral("clientId"), clientId},
        {QStringLiteral("clientSecret"), clientSecret}},
       [this, displayName](const QJsonValue&) {
         send(QStringLiteral("google.oauthStart"),
              {{QStringLiteral("displayName"), displayName}},
              [this](const QJsonValue&) {
                setStatus(QStringLiteral("Opening Google authorization…"));
                emit accountSetupStarted();
                refresh();
              });
       });
}

void AppController::addCalDavAccount(const QString& endpoint, const QString& username,
                                     const QString& password,
                                     const QString& displayName) {
  if (endpoint.trimmed().isEmpty() || username.trimmed().isEmpty() ||
      password.isEmpty()) {
    setError(QStringLiteral("CalDAV URL, username, and password are required"));
    return;
  }
  setError({});
  send(QStringLiteral("accounts.createCalDav"),
       {{QStringLiteral("endpoint"), endpoint.trimmed()},
        {QStringLiteral("username"), username.trimmed()},
        {QStringLiteral("password"), password},
        {QStringLiteral("displayName"), displayName.trimmed()}},
       [this](const QJsonValue&) {
         setStatus(QStringLiteral("Discovering CalDAV calendars…"));
         emit accountSetupStarted();
         refresh();
       });
}

void AppController::removeAccount(const QString& accountId) {
  removeAccountWithOptions(accountId, true);
}

void AppController::removeAccountWithOptions(const QString& accountId,
                                             const bool removeCachedData) {
  if (accountId.isEmpty()) {
    return;
  }
  send(QStringLiteral("accounts.remove"),
       {{QStringLiteral("accountId"), accountId},
        {QStringLiteral("removeCachedData"), removeCachedData}},
       [this](const QJsonValue&) { refresh(); });
}

void AppController::reauthorizeAccount(const QString& accountId) {
  if (accountId.isEmpty()) {
    return;
  }
  send(QStringLiteral("accounts.reauthorize"),
       {{QStringLiteral("accountId"), accountId}}, [this](const QJsonValue&) {
         setStatus(QStringLiteral("Opening account authorization…"));
         emit accountSetupStarted();
         refresh();
       });
}

void AppController::updateAccountCredentials(const QString& accountId,
                                             const QString& username,
                                             const QString& password) {
  if (accountId.trimmed().isEmpty()) {
    setError(QStringLiteral("An account is required"));
    return;
  }
  setError({});
  send(QStringLiteral("accounts.update"),
       {{QStringLiteral("accountId"), accountId.trimmed()},
        {QStringLiteral("username"), username.trimmed()},
        {QStringLiteral("password"), password}},
       [this](const QJsonValue&) {
         setStatus(QStringLiteral("Account credentials are being updated…"));
         refresh();
       });
}

void AppController::setCalendarPreference(const QString& calendarId, const QString& key,
                                          const QVariant& value) {
  QVariantMap calendar;
  for (const QVariant& candidate : std::as_const(m_calendars)) {
    const QVariantMap map = candidate.toMap();
    if (map.value(QStringLiteral("id")).toString() == calendarId) {
      calendar = map;
      break;
    }
  }
  if (calendar.isEmpty()) {
    return;
  }
  QJsonObject params{
      {QStringLiteral("calendarId"), calendarId},
      {QStringLiteral("enabled"),
       calendar.value(QStringLiteral("enabled"), true).toBool()},
      {QStringLiteral("colorOverride"),
       calendar.value(QStringLiteral("colorOverride")).toString()},
      {QStringLiteral("position"), calendar.value(QStringLiteral("position")).toInt()},
      {QStringLiteral("ignoreAlerts"),
       calendar.value(QStringLiteral("ignoreAlerts")).toBool()},
  };
  QString normalizedKey = key;
  if (normalizedKey == QStringLiteral("visible")) {
    normalizedKey = QStringLiteral("enabled");
  } else if (normalizedKey == QStringLiteral("color")) {
    normalizedKey = QStringLiteral("colorOverride");
  }
  params.insert(normalizedKey, QJsonValue::fromVariant(value));
  send(QStringLiteral("calendars.updatePreferences"), params,
       [this](const QJsonValue&) { refresh(); });
}

void AppController::setPreference(const QString& key, const QVariant& value) {
  send(QStringLiteral("settings.set"),
       {{QStringLiteral("key"), key},
        {QStringLiteral("value"), QJsonValue::fromVariant(value)}},
       [this, key, value](const QJsonValue&) {
         m_preferences.insert(key, value);
         emit preferencesChanged();
         if (key == QStringLiteral("displayTimeZone")) {
           loadRange(m_rangeStart, m_rangeEnd);
         }
       });
}

QString AppController::wallTimeToUtc(const QString& dateText, const QString& timeText,
                                     const QString& timeZoneId) const {
  const QDate date = QDate::fromString(dateText, Qt::ISODate);
  QTime time = QTime::fromString(timeText, QStringLiteral("HH:mm"));
  if (!time.isValid()) {
    time = QTime::fromString(timeText, Qt::ISODate);
  }
  const QTimeZone zone = timeZoneId.trimmed().isEmpty()
                             ? QTimeZone::systemTimeZone()
                             : QTimeZone(timeZoneId.trimmed().toUtf8());
  if (!date.isValid() || !time.isValid() || !zone.isValid()) {
    return {};
  }
  // Repeated wall times during a fall-back transition resolve to the standard
  // offset deterministically. Spring-forward gaps remain rejected by the
  // round-trip check below.
  const QDateTime local(date, time, zone,
                        QDateTime::TransitionResolution::PreferStandard);
  if (!local.isValid()) {
    return {};
  }
  const QDateTime utc = local.toUTC();
  const QDateTime roundTrip = utc.toTimeZone(zone);
  // Reject wall times inside a DST gap instead of silently shifting them.
  if (roundTrip.date() != date || roundTrip.time() != time) {
    return {};
  }
  return isoUtc(utc);
}

QString AppController::utcToWallTime(const QString& utcText,
                                     const QString& timeZoneId) const {
  const QDateTime utc = dateTimeFromIso(utcText);
  QTimeZone zone = timeZoneId.trimmed().isEmpty()
                       ? QTimeZone::systemTimeZone()
                       : QTimeZone(timeZoneId.trimmed().toUtf8());
  if (!utc.isValid() || !zone.isValid()) {
    return {};
  }
  return utc.toTimeZone(zone).toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz"));
}

bool AppController::isValidTimeZone(const QString& timeZoneId) const {
  return timeZoneId.trimmed().isEmpty() ||
         QTimeZone(timeZoneId.trimmed().toUtf8()).isValid();
}

void AppController::applyDisplayTimes(QVariantList* events) const {
  if (events == nullptr) {
    return;
  }
  const QString requestedZone =
      m_preferences.value(QStringLiteral("displayTimeZone")).toString().trimmed();
  QTimeZone displayZone = requestedZone.isEmpty() ? QTimeZone::systemTimeZone()
                                                  : QTimeZone(requestedZone.toUtf8());
  if (!displayZone.isValid()) {
    displayZone = QTimeZone::systemTimeZone();
  }
  const auto wallText = [](const QDateTime& value, const QTimeZone& zone,
                           const bool floating) {
    if (!value.isValid()) {
      return QString();
    }
    const QDateTime wall = floating ? value.toUTC() : value.toTimeZone(zone);
    return wall.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz"));
  };
  for (QVariant& eventValue : *events) {
    QVariantMap event = eventValue.toMap();
    if (event.value(QStringLiteral("allDay")).toBool()) {
      continue;
    }
    const QDateTime start =
        dateTimeFromIso(event.value(QStringLiteral("startUtc")).toString());
    const QDateTime end =
        dateTimeFromIso(event.value(QStringLiteral("endUtc")).toString());
    const bool floating = event.value(QStringLiteral("timeKind")).toString() ==
                          QStringLiteral("floating");
    QTimeZone eventZone =
        QTimeZone(event.value(QStringLiteral("startTimeZone")).toString().toUtf8());
    if (!eventZone.isValid()) {
      eventZone = displayZone;
    }
    event.insert(QStringLiteral("displayStartLocal"),
                 wallText(start, displayZone, floating));
    event.insert(QStringLiteral("displayEndLocal"),
                 wallText(end, displayZone, floating));
    event.insert(QStringLiteral("eventStartLocal"),
                 wallText(start, eventZone, floating));
    event.insert(QStringLiteral("eventEndLocal"), wallText(end, eventZone, floating));
    eventValue = event;
  }
}

void AppController::setCalendarVisibility(const QString& calendarId,
                                          const bool visible) {
  setCalendarPreference(calendarId, QStringLiteral("enabled"), visible);
}

void AppController::upsertCalendarSet(const QVariantMap& calendarSet) {
  send(QStringLiteral("calendarSets.upsert"),
       {{QStringLiteral("calendarSet"), QJsonObject::fromVariantMap(calendarSet)},
        {QStringLiteral("clientMutationId"), newUuid()}},
       [this](const QJsonValue&) { refresh(); });
}

void AppController::removeCalendarSet(const QString& calendarSetId) {
  if (calendarSetId.isEmpty() || calendarSetId == QStringLiteral("all-calendars")) {
    return;
  }
  send(QStringLiteral("calendarSets.remove"),
       {{QStringLiteral("calendarSetId"), calendarSetId},
        {QStringLiteral("clientMutationId"), newUuid()}},
       [this](const QJsonValue&) { refresh(); });
}

void AppController::activateCalendarSet(const QString& setId) {
  if (setId.isEmpty()) {
    return;
  }
  send(QStringLiteral("calendarSets.activate"),
       {{QStringLiteral("calendarSetId"), setId},
        {QStringLiteral("clientMutationId"), newUuid()}},
       [this](const QJsonValue&) { refresh(); });
}

void AppController::setCurrentView(const QString& view) {
  setPreference(QStringLiteral("currentView"), view);
}

void AppController::undoLastMutation() {
  if (m_lastMutationId.isEmpty() && m_lastUndoToken.isEmpty() &&
      m_lastUndoKind.isEmpty()) {
    setStatus(QStringLiteral("Nothing to undo"));
    return;
  }
  if (m_lastUndoToken.isEmpty() && !m_lastUndoKind.isEmpty()) {
    const QString kind = m_lastUndoKind;
    const QVariantMap event = m_lastUndoEvent;
    const QVariantMap options = m_lastUndoOptions;
    m_lastUndoKind.clear();
    m_lastUndoEvent.clear();
    m_lastUndoOptions.clear();
    if (kind == QStringLiteral("create")) {
      QVariantMap deleteOptions = options;
      deleteOptions.insert(QStringLiteral("expectedLocalRevision"),
                           event.value(QStringLiteral("localRevision"), -1));
      requestDeleteEvent(event.value(QStringLiteral("id")).toString(), deleteOptions);
    } else {
      saveEvent(event, options);
    }
    return;
  }
  send(QStringLiteral("events.undo"),
       {{QStringLiteral("clientMutationId"), m_lastMutationId},
        {QStringLiteral("undoToken"), m_lastUndoToken}},
       [this](const QJsonValue&) {
         m_lastMutationId.clear();
         m_lastUndoToken.clear();
         setStatus(QStringLiteral("Delete undone"));
         refresh();
       });
}

void AppController::installWidget() {
  const QString executable =
      QStandardPaths::findExecutable(QStringLiteral("omacalendar-widgetctl"));
  if (executable.isEmpty()) {
    setError(QStringLiteral("omacalendar-widgetctl is not installed"));
    return;
  }
  auto* process = new QProcess(this);
  connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, process](const int exitCode, QProcess::ExitStatus) {
            const QByteArray rawOutput = process->readAllStandardOutput();
            const QJsonObject result = QJsonDocument::fromJson(rawOutput).object();
            const QString failure =
                QString::fromUtf8(process->readAllStandardError()).trimmed();
            process->deleteLater();
            if (exitCode == 0) {
              const QString state = result.value(QStringLiteral("status")).toString();
              const bool installed = state == QStringLiteral("installed") ||
                                     state == QStringLiteral("already_installed");
              if (installed != m_widgetInstalled) {
                m_widgetInstalled = installed;
                emit widgetInstalledChanged();
              }
              setError({});
              setStatus(installed ? QStringLiteral("OmaCalendar widget installed")
                                  : QStringLiteral("Widget activation completed"));
            } else {
              const QString message = result.value(QStringLiteral("error"))
                                          .toObject()
                                          .value(QStringLiteral("message"))
                                          .toString();
              setError(!message.isEmpty()  ? message
                       : failure.isEmpty() ? QStringLiteral("Widget install failed")
                                           : failure.left(500));
            }
          });
  process->start(executable,
                 {QStringLiteral("install"), QStringLiteral("--source"),
                  QStringLiteral("https://github.com/brdweb/omacalendar-widget.git")});
}

void AppController::restoreOmarchyClock() {
  const QString executable =
      QStandardPaths::findExecutable(QStringLiteral("omacalendar-widgetctl"));
  if (executable.isEmpty()) {
    setError(QStringLiteral("omacalendar-widgetctl is not installed"));
    return;
  }
  auto* process = new QProcess(this);
  connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, process](const int exitCode, QProcess::ExitStatus) {
            const QByteArray rawOutput = process->readAllStandardOutput();
            const QJsonObject result = QJsonDocument::fromJson(rawOutput).object();
            const QString failure =
                QString::fromUtf8(process->readAllStandardError()).trimmed();
            process->deleteLater();
            if (exitCode == 0) {
              m_widgetInstalled = false;
              emit widgetInstalledChanged();
              setError({});
              setStatus(QStringLiteral("Omarchy clock restored"));
            } else {
              const QString message = result.value(QStringLiteral("error"))
                                          .toObject()
                                          .value(QStringLiteral("message"))
                                          .toString();
              setError(!message.isEmpty()  ? message
                       : failure.isEmpty() ? QStringLiteral("Clock restore failed")
                                           : failure.left(500));
            }
          });
  process->start(executable, {QStringLiteral("restore")});
}

void AppController::refreshWidgetStatus() {
  const QString executable =
      QStandardPaths::findExecutable(QStringLiteral("omacalendar-widgetctl"));
  if (executable.isEmpty()) {
    return;
  }
  auto* process = new QProcess(this);
  connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, process](const int exitCode, QProcess::ExitStatus) {
            const QJsonObject result =
                QJsonDocument::fromJson(process->readAllStandardOutput()).object();
            process->deleteLater();
            if (exitCode != 0) {
              return;
            }
            const bool installed = result.value(QStringLiteral("status")).toString() ==
                                   QStringLiteral("installed");
            if (installed != m_widgetInstalled) {
              m_widgetInstalled = installed;
              emit widgetInstalledChanged();
            }
          });
  process->start(executable, {QStringLiteral("status")});
}

void AppController::handleDeepLink(const QUrl& url) {
  if (!url.isValid() || url.scheme() != QStringLiteral("omacalendar")) {
    setError(QStringLiteral("This OmaCalendar link is not valid"));
    return;
  }
  m_pendingDeepLink = url;
  processPendingDeepLink();
}

void AppController::handleIcsImportFile(const QUrl& file) {
  const QUrl normalized = normalizedLocalIcsUrl(file);
  if (!normalized.isValid()) {
    setError(QStringLiteral("Only a readable local .ics file can be imported"));
    return;
  }
  emit openIcsImportRequested(normalized);
}

void AppController::processPendingDeepLink() {
  if (!m_pendingDeepLink.isValid()) {
    return;
  }
  const QUrl url = m_pendingDeepLink;
  QString route = url.host().toLower();
  QString path = url.path();
  while (path.startsWith(QLatin1Char('/'))) {
    path.remove(0, 1);
  }
  if (route.isEmpty() && !path.isEmpty()) {
    route = path.section(QLatin1Char('/'), 0, 0).toLower();
    path = path.section(QLatin1Char('/'), 1);
  }

  if (route == QStringLiteral("event")) {
    if (!connected()) {
      return;
    }
    const QString eventId = QUrl::fromPercentEncoding(path.toUtf8());
    if (eventId.isEmpty()) {
      setError(QStringLiteral("The event link is missing an event identifier"));
      m_pendingDeepLink = QUrl();
      return;
    }
    m_pendingDeepLink = QUrl();
    QJsonObject params{{QStringLiteral("eventId"), eventId}};
    const QUrlQuery query(url);
    const QString recurrenceId =
        query.queryItemValue(QStringLiteral("recurrenceId")).trimmed();
    if (!recurrenceId.isEmpty()) {
      params.insert(QStringLiteral("recurrenceId"), recurrenceId);
    }
    send(QStringLiteral("events.get"), params, [this](const QJsonValue& value) {
      emit openEventRequested(value.toObject().toVariantMap());
    });
    return;
  }

  m_pendingDeepLink = QUrl();
  if (route == QStringLiteral("new") || route == QStringLiteral("create")) {
    const QUrlQuery query(url);
    QDateTime start = dateTimeFromIso(query.queryItemValue(QStringLiteral("start")));
    if (!start.isValid()) {
      start = QDateTime::currentDateTime();
      const int nextHalfHour = ((start.time().minute() / 30) + 1) * 30;
      start.setTime(QTime(start.time().hour(), 0));
      start = start.addSecs(nextHalfHour * 60);
    }
    QDateTime end = dateTimeFromIso(query.queryItemValue(QStringLiteral("end")));
    if (!end.isValid() || end <= start) {
      end = start.addSecs(60 * 60);
    }
    QVariantMap draft{
        {QStringLiteral("summary"), query.queryItemValue(QStringLiteral("title"))},
        {QStringLiteral("description"), query.queryItemValue(QStringLiteral("notes"))},
        {QStringLiteral("location"), query.queryItemValue(QStringLiteral("location"))},
        {QStringLiteral("calendarId"),
         query.queryItemValue(QStringLiteral("calendarId"))},
        {QStringLiteral("startUtc"), isoUtc(start)},
        {QStringLiteral("endUtc"), isoUtc(end)},
        {QStringLiteral("startTimeZone"),
         QString::fromUtf8(QTimeZone::systemTimeZoneId())},
        {QStringLiteral("endTimeZone"),
         QString::fromUtf8(QTimeZone::systemTimeZoneId())},
        {QStringLiteral("allDay"), false},
    };
    emit createEventRequested(draft);
  } else if (route == QStringLiteral("invitations")) {
    emit openSectionRequested(QStringLiteral("invitations"));
  } else if (route == QStringLiteral("conflicts")) {
    emit openSectionRequested(QStringLiteral("conflicts"));
  } else if (route == QStringLiteral("settings")) {
    emit openSectionRequested(path.startsWith(QStringLiteral("accounts"))
                                  ? QStringLiteral("accounts")
                                  : QStringLiteral("settings"));
  } else if (!route.isEmpty()) {
    setError(QStringLiteral("This OmaCalendar link is not supported"));
  }
}

void AppController::previewDiagnostics() {
  send(QStringLiteral("system.health"), {}, [this](const QJsonValue& value) {
    QString directoryError;
    if (!paths::ensureDirectories(&directoryError)) {
      setError(directoryError);
      return;
    }
    const QString path = QDir(paths::cacheDirectory())
                             .filePath(QStringLiteral("diagnostics-preview.json"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
      setError(QStringLiteral("Could not create diagnostics preview"));
      return;
    }
    QJsonObject document{
        {QStringLiteral("generatedAt"), isoUtc(QDateTime::currentDateTimeUtc())},
        {QStringLiteral("health"), value},
        {QStringLiteral("privacy"),
         QStringLiteral(
             "Credentials, provider URLs, ETags, and raw payloads are excluded")},
    };
    file.write(QJsonDocument(document).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
      setError(QStringLiteral("Could not save diagnostics preview"));
      return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    setStatus(QStringLiteral("Opened privacy-safe diagnostics preview"));
  });
}

void AppController::syncAll() {
  send(QStringLiteral("sync.all"), {}, [this](const QJsonValue&) {
    setStatus(QStringLiteral("Synchronizing calendars…"));
  });
}

void AppController::syncAccount(const QString& accountId) {
  if (accountId.isEmpty()) {
    return;
  }
  send(QStringLiteral("sync.account"), {{QStringLiteral("accountId"), accountId}},
       [this](const QJsonValue&) {
         setStatus(QStringLiteral("Account sync started"));
         refresh();
       });
}

void AppController::setSelectedDate(const QDate& date) {
  if (!date.isValid() || date == m_selectedDate) {
    return;
  }
  m_selectedDate = date;
  emit selectedDateChanged();
}

void AppController::activateWindow() {
  emit windowActivationRequested();
  if (!qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE")) {
    return;
  }

  // Wayland compositors may reject QWindow::requestActivate() when the request
  // was forwarded from a short-lived second process. On the target Omarchy
  // desktop, ask Hyprland to focus the already restored application window.
  QTimer::singleShot(0, this, []() {
    const QString hyprctl = QStandardPaths::findExecutable(QStringLiteral("hyprctl"));
    if (!hyprctl.isEmpty()) {
      QProcess::startDetached(
          hyprctl, {QStringLiteral("eval"),
                    QStringLiteral("return hl.dispatch(hl.dsp.focus({ window = "
                                   "\"title:OmaCalendar\" }))")});
    }
  });
}

void AppController::setError(const QString& message) {
  if (message == m_lastError) {
    return;
  }
  m_lastError = message;
  emit lastErrorChanged();
}

void AppController::setStatus(const QString& message) {
  if (message == m_statusText) {
    return;
  }
  m_statusText = message;
  emit statusTextChanged();
}

void AppController::setBusy(const bool busyValue) {
  Q_UNUSED(busyValue)
  emit busyChanged();
}

}  // namespace omacalendar
