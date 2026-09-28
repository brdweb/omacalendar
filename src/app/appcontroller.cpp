#include "appcontroller.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QTimeZone>
#include <QUrlQuery>
#include <utility>

#include "calendarpdf.h"
#include "core/domain.h"
#include "core/freebusy.h"
#include "core/paths.h"
#include "core/quickadd.h"
#include "providers/google/googleoauthconfig.h"
#include "startuprequest.h"

namespace omacalendar {
namespace {

constexpr auto kWidgetSourceUrl = "https://github.com/brdweb/omacalendar-widget.git";
constexpr auto kWidgetSourceRef = "v0.1.0";
#ifndef OMACALENDAR_WIDGET_SOURCE_COMMIT
#define OMACALENDAR_WIDGET_SOURCE_COMMIT "1aeaf5de3460b87053d494f82b9435b3c371f309"
#endif
constexpr auto kWidgetSourceCommit = OMACALENDAR_WIDGET_SOURCE_COMMIT;

QUrl validatedExternalEventUrl(const QString& value) {
  if (value.isEmpty() || value != value.trimmed()) {
    return {};
  }
  const QUrl url(value, QUrl::StrictMode);
  const QString scheme = url.scheme().toLower();
  if (!url.isValid() || url.isRelative() || url.host().isEmpty() ||
      (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))) {
    return {};
  }
  return url;
}

bool isExactGitCommit(const QString& value) {
  static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{40}$"));
  return expression.match(value).hasMatch();
}

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
  // The daemon's interactive lease is 10 minutes.
  m_interactiveRenewal.setInterval(5 * 60 * 1000);
  connect(&m_interactiveRenewal, &QTimer::timeout, this,
          &AppController::sendInteractive);
  m_pdfHydrationWait.setSingleShot(true);
  connect(&m_pdfHydrationWait, &QTimer::timeout, this,
          [this]() { resumePdfExport(false); });
  m_refreshTimer.setInterval(120);
  connect(&m_refreshTimer, &QTimer::timeout, this, [this]() {
    const int parts = std::exchange(m_pendingRefreshParts, 0);
    if (connected()) {
      refreshParts(parts);
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
      {QStringLiteral("notificationPrivacy"), QStringLiteral("generic")},
      {QStringLiteral("currentView"), QStringLiteral("month")},
      {QStringLiteral("widgetConsentDecision"), QStringLiteral("")},
      {QStringLiteral("showWeekNumbers"), false},
      {QStringLiteral("secondaryTimeZone"), QStringLiteral("")}};
  m_client.setAutoReconnect(true);
  connect(&m_client, &ipc::IpcClient::connectedChanged, this, [this]() {
    emit connectedChanged();
    if (connected()) {
      m_daemonStartAttempted = false;
      m_calendarsReady = false;
      m_calendarSetsReady = false;
      m_scopeRequestsInFlight = 0;
      // The daemon may have been upgraded while disconnected.
      m_settingsGetManySupported = true;
      setError({});
      setStatus(tr("Calendar service connected"));
      if (m_interactive) {
        // A restarted daemon has forgotten that the window is in use.
        sendInteractive();
      }
      subscribe(true);
      refresh();
      processPendingDeepLink();
    } else {
      // Requests in flight are lost; an undo among them can be tried again.
      m_historyInFlight = 0;
      m_pending.clear();
      m_pendingErrors.clear();
      m_backgroundRequests.clear();
      m_refreshTimer.stop();
      m_pendingRefreshParts = 0;
      m_scopeRequestsInFlight = 0;
      m_activeRequests = 0;
      setBusy(false);
      setStatus(tr("Reconnecting to calendar service…"));
      QTimer::singleShot(500, this, &AppController::startDaemonIfNeeded);
    }
  });
  connect(&m_client, &ipc::IpcClient::responseReceived, this,
          [this](const QString& id, const QJsonValue& result) {
            const ResultHandler handler = m_pending.take(id);
            m_pendingErrors.remove(id);
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
            const ErrorHandler errorHandler = m_pendingErrors.take(id);
            const bool background = m_backgroundRequests.remove(id);
            if (!background && m_activeRequests > 0) {
              --m_activeRequests;
              setBusy(m_activeRequests > 0);
            }
            if (errorHandler && errorHandler(error)) {
              return;
            }
            setError(error.value(QStringLiteral("message"))
                         .toString(tr("Calendar service request failed")));
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
            setStatus(tr("Complete Google sign-in in your browser"));
          }
          return;
        }
        const int parts = refreshPartsForNotification(event);
        if (event == QStringLiteral("events.freeBusy")) {
          // A remote answer for the free/busy request on screen.
          if (data.value(QStringLiteral("requestId")).toString() ==
              m_freeBusy.value(QStringLiteral("requestId")).toString()) {
            QVariantMap attendees =
                m_freeBusy.value(QStringLiteral("attendees")).toMap();
            const QVariantMap answered =
                data.value(QStringLiteral("attendees")).toObject().toVariantMap();
            for (auto it = answered.cbegin(); it != answered.cend(); ++it) {
              attendees.insert(it.key(), it.value());
            }
            QVariantList unavailable =
                m_freeBusy.value(QStringLiteral("unavailable")).toList();
            unavailable.append(
                data.value(QStringLiteral("unavailable")).toArray().toVariantList());
            m_freeBusy.insert(QStringLiteral("attendees"), attendees);
            m_freeBusy.insert(QStringLiteral("unavailable"), unavailable);
            m_freeBusy.insert(QStringLiteral("pending"), QVariantList{});
            emit freeBusyChanged();
          }
          return;
        }
        if (event == QStringLiteral("events.changed")) {
          // Subscriptions announce a refresh only through changed events.
          refreshAccountSyncStates(true);
          resumePdfExport(true);
        }
        if (parts != 0) {
          scheduleRefresh(parts);
        } else if (event == QStringLiteral("sync.statusChanged")) {
          const QJsonObject status = data.value(QStringLiteral("status")).toObject();
          setAccountSyncState(data.value(QStringLiteral("accountId")).toString(),
                              status);
          const QString state = status.value(QStringLiteral("state")).toString();
          if (state == QStringLiteral("error") ||
              state == QStringLiteral("reauthorization_required")) {
            setError(status.value(QStringLiteral("message")).toString());
          } else if (state == QStringLiteral("syncing")) {
            setStatus(tr("Synchronizing calendars…"));
          } else if (state == QStringLiteral("idle")) {
            setError({});
            setStatus(tr("Synchronization complete"));
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
QVariantMap AppController::accountSyncStates() const { return m_accountSyncStates; }
QVariantList AppController::calendars() const { return m_calendars; }
QVariantList AppController::events() const { return m_events; }
QVariantList AppController::calendarSets() const { return m_calendarSets; }
QVariantList AppController::invitations() const { return m_invitations; }
QVariantList AppController::conflicts() const { return m_conflicts; }

QVariantList AppController::taskLists() const { return m_taskLists; }

QVariantList AppController::tasks() const { return m_tasks; }

bool AppController::tasksSupported() const { return m_tasksSupported; }

void AppController::loadTasks() {
  // Older daemons answer method_not_found; tasks then stay hidden.
  const auto unsupported = [this](const QJsonObject& error) {
    if (error.value(QStringLiteral("code")).toString() !=
        QStringLiteral("method_not_found")) {
      return false;
    }
    if (m_tasksSupported) {
      m_tasksSupported = false;
      m_taskLists.clear();
      m_tasks.clear();
      emit tasksChanged();
    }
    return true;
  };
  send(
      QStringLiteral("taskLists.list"), {},
      [this](const QJsonValue& value) {
        requestTaskPage(variantList(value, QStringLiteral("lists")), {}, 0,
                        ++m_taskGeneration);
      },
      false, unsupported);
}

void AppController::requestTaskPage(const QVariantList& lists, QVariantList tasks,
                                    const int offset, const quint64 generation) {
  send(
      QStringLiteral("tasks.list"),
      {{QStringLiteral("offset"), offset}, {QStringLiteral("limit"), kTaskPageLimit}},
      [this, lists, tasks = std::move(tasks), offset,
       generation](const QJsonValue& value) mutable {
        if (generation != m_taskGeneration) {
          return;
        }
        tasks.append(variantList(value, QStringLiteral("tasks")));
        const QJsonObject page = value.toObject();
        const int nextOffset = page.value(QStringLiteral("nextOffset")).toInt(-1);
        // Keep reading until the daemon reports the last page.
        if (page.value(QStringLiteral("hasMore")).toBool() && nextOffset > offset) {
          requestTaskPage(lists, std::move(tasks), nextOffset, generation);
          return;
        }
        m_taskLists = lists;
        m_tasks = std::move(tasks);
        m_tasksSupported = true;
        emit tasksChanged();
      },
      false);
}

void AppController::sendTaskMutation(const QString& method, const QJsonObject& params) {
  if (!connected()) {
    setError(tr("Connect to the calendar service to change tasks"));
    return;
  }
  // tasks.changed reloads the lists once the daemon has saved the change.
  send(method, params, [this](const QJsonValue&) { setError({}); });
}

void AppController::createTask(const QVariantMap& task) {
  sendTaskMutation(QStringLiteral("tasks.create"),
                   {{QStringLiteral("task"), QJsonObject::fromVariantMap(task)}});
}

void AppController::updateTask(const QVariantMap& task) {
  QJsonObject params{{QStringLiteral("task"), QJsonObject::fromVariantMap(task)}};
  if (task.contains(QStringLiteral("localRevision"))) {
    params.insert(QStringLiteral("expectedLocalRevision"),
                  task.value(QStringLiteral("localRevision")).toLongLong());
  }
  sendTaskMutation(QStringLiteral("tasks.update"), params);
}

void AppController::setTaskCompleted(const QString& taskId, const bool completed) {
  sendTaskMutation(QStringLiteral("tasks.update"),
                   {{QStringLiteral("task"),
                     QJsonObject{{QStringLiteral("id"), taskId},
                                 {QStringLiteral("completed"), completed}}}});
}

void AppController::removeTask(const QString& taskId) {
  sendTaskMutation(QStringLiteral("tasks.remove"),
                   {{QStringLiteral("taskId"), taskId}});
}

void AppController::setTaskListEnabled(const QString& listId, const bool enabled) {
  sendTaskMutation(
      QStringLiteral("taskLists.setEnabled"),
      {{QStringLiteral("listId"), listId}, {QStringLiteral("enabled"), enabled}});
}
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
QDateTime AppController::selectedDate() const {
  // QML converts QDate to UTC midnight, which is the previous local date west
  // of UTC. A local noon preserves the calendar date used by JS Date getters
  // and avoids ordinary daylight-saving transitions at midnight.
  return QDateTime(m_selectedDate, QTime(12, 0), QTimeZone::LocalTime);
}

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
    setError(tr("omacalendard was not found. Start it, then retry."));
  }
}

QString AppController::send(const QString& method, const QJsonObject& params,
                            ResultHandler handler, const bool contributesToBusy,
                            ErrorHandler errorHandler) {
  if (!connected()) {
    setError(tr("Calendar service is not connected"));
    startDaemonIfNeeded();
    return {};
  }
  const QString id = m_client.request(method, params);
  if (id.isEmpty()) {
    setError(tr("Could not send request to calendar service"));
    return {};
  }
  m_pending.insert(id, std::move(handler));
  if (errorHandler) {
    m_pendingErrors.insert(id, std::move(errorHandler));
  }
  if (contributesToBusy) {
    ++m_activeRequests;
    setBusy(true);
  } else {
    m_backgroundRequests.insert(id);
  }
  return id;
}

void AppController::subscribe(const bool includeTasks) {
  QJsonArray topics{QStringLiteral("accounts"),     QStringLiteral("calendars"),
                    QStringLiteral("calendarSets"), QStringLiteral("events"),
                    QStringLiteral("invitations"),  QStringLiteral("reminders"),
                    QStringLiteral("sync"),         QStringLiteral("google"),
                    QStringLiteral("operations"),   QStringLiteral("conflicts")};
  if (includeTasks) {
    topics.append(QStringLiteral("tasks"));
  }
  QJsonObject subscription{{QStringLiteral("topics"), topics}};
  if (m_subscriptionRevision >= 0) {
    subscription.insert(QStringLiteral("sinceRevision"), m_subscriptionRevision);
  }
  send(
      QStringLiteral("system.subscribe"), subscription,
      [this](const QJsonValue& value) {
        const QJsonObject result = value.toObject();
        m_subscriptionRevision =
            result.value(QStringLiteral("revision")).toInteger(m_subscriptionRevision);
        if (result.value(QStringLiteral("catchUpRequired")).toBool()) {
          refresh();
        }
      },
      true,
      [this, includeTasks](const QJsonObject& error) {
        // A daemon from before tasks rejects the whole subscription over the
        // unknown topic; subscribe to the rest instead.
        if (includeTasks && error.value(QStringLiteral("message"))
                                .toString()
                                .contains(QStringLiteral("topic: tasks"))) {
          subscribe(false);
          return true;
        }
        return false;
      });
}

void AppController::refresh() { refreshParts(RefreshAll); }

int AppController::refreshPartsForNotification(const QString& event) {
  // Each notification names what changed; reload only the lists it can
  // affect. Reminder state is not presented, so reminders.changed, which the
  // daemon sends for every delivered or dismissed reminder, reloads nothing.
  if (event == QStringLiteral("events.changed")) {
    return RefreshEvents | RefreshInvitations;
  }
  if (event == QStringLiteral("invitations.changed")) {
    return RefreshInvitations;
  }
  if (event == QStringLiteral("calendarSets.changed")) {
    return RefreshCalendarSets;
  }
  if (event == QStringLiteral("operations.changed")) {
    return RefreshOperations;
  }
  if (event == QStringLiteral("conflicts.changed")) {
    return RefreshConflicts;
  }
  if (event == QStringLiteral("tasks.changed")) {
    return RefreshTasks;
  }
  // Calendar changes can hide or remove events, invalidate the default
  // calendar, and change calendar-set membership: new calendars join the
  // built-in set and removed ones leave every set. Account changes can do
  // all of that through their calendars.
  if (event == QStringLiteral("calendars.changed")) {
    return RefreshCalendars | RefreshCalendarSets | RefreshEvents | RefreshInvitations |
           RefreshPreferences;
  }
  if (event == QStringLiteral("accounts.changed")) {
    // Removing an account also removes its task lists.
    return RefreshAccounts | RefreshCalendars | RefreshCalendarSets | RefreshEvents |
           RefreshInvitations | RefreshPreferences | RefreshTasks;
  }
  return 0;
}

void AppController::scheduleRefresh(const int parts) {
  m_pendingRefreshParts |= parts;
  m_refreshTimer.start();
}

void AppController::refreshParts(const int parts) {
  if (!connected() || parts == 0) {
    return;
  }
  const auto background = [this](const QString& method, const QJsonObject& params,
                                 ResultHandler handler) {
    send(method, params, std::move(handler), false);
  };
  if ((parts & RefreshSystemInfo) != 0) {
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
  }
  if ((parts & RefreshAccounts) != 0) {
    background(QStringLiteral("accounts.list"), {}, [this](const QJsonValue& value) {
      m_accounts = variantList(value, QStringLiteral("accounts"));
      m_accountsModel.replace(m_accounts);
      emit accountsChanged();
      refreshAccountSyncStates(false);
    });
  }
  m_scopeRequestsInFlight +=
      ((parts & RefreshCalendars) != 0) + ((parts & RefreshCalendarSets) != 0);
  if ((parts & RefreshCalendars) != 0) {
    send(
        QStringLiteral("calendars.list"), {},
        [this](const QJsonValue& value) {
          m_calendars = variantList(value, QStringLiteral("calendars"));
          m_calendarsModel.replace(m_calendars);
          m_calendarsReady = true;
          emit calendarsChanged();
          finishScopeRequest();
        },
        false,
        [this](const QJsonObject&) {
          finishScopeRequest();
          return false;
        });
  }
  if ((parts & RefreshCalendarSets) != 0) {
    send(
        QStringLiteral("calendarSets.list"), {},
        [this](const QJsonValue& value) {
          m_calendarSets = variantList(value, QStringLiteral("calendarSets"));
          m_calendarSetsModel.replace(m_calendarSets);
          const QString active = value.toObject()
                                     .value(QStringLiteral("activeId"))
                                     .toString(QStringLiteral("all-calendars"));
          if (active != m_activeCalendarSetId) {
            m_activeCalendarSetId = active;
            emit activeCalendarSetIdChanged();
          }
          m_calendarSetsReady = true;
          emit calendarSetsChanged();
          finishScopeRequest();
        },
        false,
        [this](const QJsonObject&) {
          finishScopeRequest();
          return false;
        });
  }
  if ((parts & RefreshInvitations) != 0) {
    background(QStringLiteral("invitations.list"), {}, [this](const QJsonValue& value) {
      m_invitations = variantList(value, QStringLiteral("invitations"));
      applyDisplayTimes(&m_invitations);
      m_invitationsModel.replace(m_invitations);
      emit invitationsChanged();
    });
  }
  if ((parts & RefreshOperations) != 0) {
    background(QStringLiteral("outbox.list"), {}, [this](const QJsonValue& value) {
      m_operations = variantList(value, QStringLiteral("items"));
      m_operationsModel.replace(m_operations);
      emit operationsChanged();
    });
  }
  if ((parts & RefreshConflicts) != 0) {
    background(QStringLiteral("conflicts.list"), {}, [this](const QJsonValue& value) {
      m_conflicts = variantList(value, QStringLiteral("conflicts"));
      m_conflictsModel.replace(m_conflicts);
      emit conflictsChanged();
    });
  }
  if ((parts & RefreshPreferences) != 0) {
    loadPreferences();
  }
  if ((parts & RefreshTasks) != 0) {
    loadTasks();
  }
  if ((parts & RefreshEvents) != 0) {
    if (!m_rangeStart.isValid() || !m_rangeEnd.isValid()) {
      m_rangeStart = QDate::currentDate().addDays(-14);
      m_rangeEnd = QDate::currentDate().addDays(45);
    }
    loadRange(m_rangeStart, m_rangeEnd);
  }
}

QStringList AppController::preferenceKeys() {
  return {QStringLiteral("firstDayOfWeek"),    QStringLiteral("workDayStart"),
          QStringLiteral("workDayEnd"),        QStringLiteral("timeFormat"),
          QStringLiteral("displayTimeZone"),   QStringLiteral("defaultDuration"),
          QStringLiteral("defaultCalendarId"), QStringLiteral("notificationPrivacy"),
          QStringLiteral("currentView"),       QStringLiteral("widgetConsentDecision"),
          QStringLiteral("showWeekNumbers"),   QStringLiteral("secondaryTimeZone")};
}

void AppController::markPreferencesLoaded() {
  if (!m_preferencesLoaded) {
    m_preferencesLoaded = true;
    emit preferencesLoadedChanged();
  }
}

void AppController::loadPreferences() {
  if (!m_settingsGetManySupported) {
    loadPreferencesIndividually();
    return;
  }
  QJsonArray keys;
  QJsonObject fallbacks;
  for (const QString& key : preferenceKeys()) {
    keys.append(key);
    fallbacks.insert(key, QJsonValue::fromVariant(m_preferences.value(key)));
  }
  send(
      QStringLiteral("settings.getMany"),
      {{QStringLiteral("keys"), keys}, {QStringLiteral("fallbacks"), fallbacks}},
      [this](const QJsonValue& value) {
        const QJsonObject values =
            value.toObject().value(QStringLiteral("values")).toObject();
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
          m_preferences.insert(it.key(), it.value().toVariant());
        }
        markPreferencesLoaded();
        emit preferencesChanged();
      },
      false,
      [this](const QJsonObject& error) {
        // An IPC 2.1 daemon, for example one still running from before an
        // upgrade, has no settings.getMany.
        if (error.value(QStringLiteral("code")).toString() !=
            QStringLiteral("method_not_found")) {
          return false;
        }
        m_settingsGetManySupported = false;
        loadPreferencesIndividually();
        return true;
      });
}

void AppController::loadPreferencesIndividually() {
  for (const QString& key : preferenceKeys()) {
    send(
        QStringLiteral("settings.get"),
        {{QStringLiteral("key"), key},
         {QStringLiteral("fallback"),
          QJsonValue::fromVariant(m_preferences.value(key))}},
        [this, key](const QJsonValue& value) {
          m_preferences.insert(
              key, value.toObject().value(QStringLiteral("value")).toVariant());
          if (key == QStringLiteral("widgetConsentDecision")) {
            markPreferencesLoaded();
          }
          emit preferencesChanged();
        },
        false);
  }
}

void AppController::finishScopeRequest() {
  --m_scopeRequestsInFlight;
  if (m_scopeRequestsInFlight == 0 && m_calendarsReady && m_calendarSetsReady &&
      m_rangeStart.isValid() &&
      (m_rangeNeedsReload || visibleCalendarIds() != m_visibleCalendarIds)) {
    loadRange(m_rangeStart, m_rangeEnd);
  }
}

QStringList AppController::visibleCalendarIds() const {
  QSet<QString> activeIds;
  if (m_activeCalendarSetId != QStringLiteral("all-calendars")) {
    for (const QVariant& value : m_calendarSets) {
      const QVariantMap calendarSet = value.toMap();
      if (calendarSet.value(QStringLiteral("id")).toString() == m_activeCalendarSetId) {
        const QVariantList ids =
            calendarSet.value(QStringLiteral("calendarIds")).toList();
        for (const QVariant& id : ids) {
          activeIds.insert(id.toString());
        }
        break;
      }
    }
  }
  QStringList visible;
  for (const QVariant& value : m_calendars) {
    const QVariantMap calendar = value.toMap();
    const QString id = calendar.value(QStringLiteral("id")).toString();
    if (!id.isEmpty() && calendar.value(QStringLiteral("enabled"), true).toBool() &&
        (m_activeCalendarSetId == QStringLiteral("all-calendars") ||
         activeIds.contains(id))) {
      visible.append(id);
    }
  }
  return visible;
}

void AppController::loadRange(const QDate& firstDate, const QDate& lastDate) {
  if (!firstDate.isValid() || !lastDate.isValid() || firstDate > lastDate) {
    setError(tr("A valid date range is required"));
    return;
  }
  m_rangeStart = firstDate;
  m_rangeEnd = lastDate;
  // A newer range or visibility scope supersedes pages still in flight.
  ++m_rangeGeneration;
  m_rangePages.clear();
  if (!m_calendarsReady || !m_calendarSetsReady || m_scopeRequestsInFlight != 0) {
    m_rangeNeedsReload = true;
    return;
  }
  m_rangeNeedsReload = false;
  m_visibleCalendarIds = visibleCalendarIds();
  if (m_visibleCalendarIds.isEmpty()) {
    m_events.clear();
    m_eventsModel.replace(m_events);
    emit eventsChanged();
    return;
  }
  requestRangePage(m_rangeGeneration, 0, kEventPageLimit);
}

void AppController::requestRangePage(const quint64 generation, const int offset,
                                     const int limit) {
  QJsonArray calendarIds;
  for (const QString& id : std::as_const(m_visibleCalendarIds)) {
    calendarIds.append(id);
  }
  const QJsonObject params = {
      {QStringLiteral("start"), isoUtc(startOfDateUtc(m_rangeStart))},
      {QStringLiteral("end"), isoUtc(startOfDateUtc(m_rangeEnd.addDays(1)))},
      {QStringLiteral("calendarIds"), calendarIds},
      {QStringLiteral("offset"), offset},
      {QStringLiteral("limit"), limit},
  };
  send(
      QStringLiteral("events.list"), params,
      [this, generation, offset, limit](const QJsonValue& value) {
        if (generation != m_rangeGeneration) {
          return;
        }
        const QJsonObject page = value.toObject();
        m_rangePages.append(variantList(value, QStringLiteral("events")));
        const int nextOffset = page.value(QStringLiteral("nextOffset")).toInt(-1);
        // The daemon caps each page, so keep reading until it reports the
        // range complete. A next offset that does not advance would loop
        // forever; treat it as the end of the range.
        if (page.value(QStringLiteral("hasMore")).toBool() && nextOffset > offset) {
          requestRangePage(generation, nextOffset, limit);
          return;
        }
        m_events = std::exchange(m_rangePages, {});
        applyDisplayTimes(&m_events);
        m_eventsModel.replace(m_events);
        emit eventsChanged();
      },
      false,
      [this, generation, offset, limit](const QJsonObject& error) {
        if (generation != m_rangeGeneration) {
          return true;
        }
        // Events with long descriptions or many attendees can push a page
        // past the IPC frame limit. Retry the same offset with smaller pages.
        if (error.value(QStringLiteral("code")).toString() ==
                QStringLiteral("response_too_large") &&
            limit > 1) {
          requestRangePage(generation, offset, limit / 2);
          return true;
        }
        return false;
      });
}

QVariantMap AppController::parseQuickAdd(const QString& text) const {
  const QuickAddDraft draft = omacalendar::parseQuickAdd(text, QDate::currentDate());
  return {
      {QStringLiteral("title"), draft.title},
      {QStringLiteral("location"), draft.location},
      {QStringLiteral("recurrenceRule"), draft.recurrenceRule},
      {QStringLiteral("date"),
       draft.date.isValid() ? draft.date.toString(Qt::ISODate) : QString()},
      {QStringLiteral("endDate"),
       draft.endDate.isValid() ? draft.endDate.toString(Qt::ISODate) : QString()},
      {QStringLiteral("allDay"), draft.allDay},
      {QStringLiteral("startMinute"), draft.startMinute},
      {QStringLiteral("durationMinutes"), draft.durationMinutes},
  };
}

void AppController::suggestContacts(const QString& prefix) {
  const QString trimmed = prefix.trimmed();
  if (trimmed.isEmpty() || !connected()) {
    return;
  }
  send(
      QStringLiteral("contacts.suggest"),
      {{QStringLiteral("prefix"), trimmed}, {QStringLiteral("limit"), 8}},
      [this, trimmed](const QJsonValue& value) {
        emit contactSuggestionsReady(trimmed,
                                     variantList(value, QStringLiteral("contacts")));
      },
      false,
      // Suggestions are optional; an older daemon without the method, or any
      // other failure, simply offers none.
      [](const QJsonObject&) { return true; });
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
  saveEventWithHistory(values, mutationOptions, HistoryMode::Record);
}

void AppController::saveEventWithHistory(const QVariantMap& values,
                                         const QVariantMap& mutationOptions,
                                         const HistoryMode mode,
                                         const QVariantMap& knownPrior) {
  const QJsonObject event = QJsonObject::fromVariantMap(values);
  const bool updating = !event.value(QStringLiteral("id")).toString().isEmpty();
  const QString mutationId = newUuid();
  QVariantMap priorEvent = knownPrior;
  if (updating && priorEvent.isEmpty()) {
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
  const ResultHandler saved = [this, mode, updating, priorEvent, inverseOptions, values,
                               calendarChanged](const QJsonValue& value) {
    finishHistoryStep(mode);
    QJsonObject responseEvent = value.toObject();
    if (responseEvent.value(QStringLiteral("event")).isObject()) {
      responseEvent = responseEvent.value(QStringLiteral("event")).toObject();
    }
    bool undoable = false;
    if (updating && !priorEvent.isEmpty() && !calendarChanged) {
      // Undoing an edit saves the prior state over the new revision.
      HistoryEntry inverse;
      inverse.kind = HistoryEntry::Kind::Restore;
      inverse.event = priorEvent;
      if (!responseEvent.value(QStringLiteral("id")).toString().isEmpty()) {
        inverse.event.insert(QStringLiteral("id"),
                             responseEvent.value(QStringLiteral("id")).toVariant());
      }
      if (responseEvent.contains(QStringLiteral("localRevision"))) {
        inverse.event.insert(
            QStringLiteral("localRevision"),
            responseEvent.value(QStringLiteral("localRevision")).toVariant());
      }
      inverse.previous = values;
      inverse.options = inverseOptions;
      recordInverse(mode, inverse);
      undoable = true;
    } else if (!updating) {
      HistoryEntry inverse;
      inverse.kind = HistoryEntry::Kind::Remove;
      inverse.event = responseEvent.toVariantMap();
      inverse.options = inverseOptions;
      recordInverse(mode, inverse);
      undoable = true;
    } else if (mode == HistoryMode::Record && !m_redoHistory.isEmpty()) {
      // A change history cannot reverse still makes older redo steps stale.
      m_redoHistory.clear();
      emit historyChanged();
    }
    if (mode == HistoryMode::Record) {
      const QString message = calendarChanged ? tr("Event moved")
                              : updating      ? tr("Event updated")
                                              : tr("Event created");
      setStatus(message);
      emit mutationCompleted(message, undoable);
    } else {
      setStatus(mode == HistoryMode::Undo ? tr("Undone") : tr("Redone"));
    }
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
    send(QStringLiteral("events.move"), params, saved, true,
         historyFailureHandler(mode));
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
       params, saved, true, historyFailureHandler(mode));
}

void AppController::requestDeleteEvent(const QString& eventId,
                                       const QVariantMap& mutationOptions) {
  deleteEventWithHistory(eventId, mutationOptions, HistoryMode::Record);
}

void AppController::deleteEventWithHistory(const QString& eventId,
                                           const QVariantMap& mutationOptions,
                                           const HistoryMode mode) {
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
  send(
      QStringLiteral("events.remove"), params,
      [this, mode, eventId, recurrenceId, mutationOptions](const QJsonValue& value) {
        finishHistoryStep(mode);
        const QString undoToken =
            value.toObject().value(QStringLiteral("undoToken")).toString();
        if (!undoToken.isEmpty()) {
          // The daemon holds a delete back briefly; its token restores the
          // event until then.
          HistoryEntry inverse;
          inverse.kind = HistoryEntry::Kind::Undelete;
          inverse.undoToken = undoToken;
          inverse.event = {{QStringLiteral("id"), eventId},
                           {QStringLiteral("recurrenceId"), recurrenceId}};
          inverse.options = mutationOptions;
          inverse.expiresAt = QDateTime::currentDateTimeUtc().addSecs(10);
          recordInverse(mode, inverse);
        }
        if (mode == HistoryMode::Record) {
          setStatus(tr("Event deleted"));
          emit mutationCompleted(tr("Event deleted"), !undoToken.isEmpty());
        } else {
          setStatus(mode == HistoryMode::Undo ? tr("Undone") : tr("Redone"));
        }
        loadRange(m_rangeStart, m_rangeEnd);
      },
      true, historyFailureHandler(mode));
}

void AppController::searchEvents(const QString& query, const QVariantMap& filters) {
  // The daemon accepts a guest filter alone.
  if (query.trimmed().isEmpty() &&
      filters.value(QStringLiteral("attendee")).toString().trimmed().isEmpty()) {
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
      [this](const QJsonValue&) { refresh(); }, false);
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
    setError(tr("Invalid conflict identifier"));
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

void AppController::addLocalCalendar(const QString& name, const QString& color,
                                     const bool muteAlerts) {
  if (name.trimmed().isEmpty()) {
    setError(tr("Local calendar name is required"));
    return;
  }
  // ignoreAlerts rides in the creation payload: calendars.upsert persists it
  // atomically, so no follow-up preference call can race the first sync.
  const QJsonObject calendar{
      {QStringLiteral("accountId"), QStringLiteral("local-account")},
      {QStringLiteral("name"), name.trimmed()},
      {QStringLiteral("color"), color},
      {QStringLiteral("ignoreAlerts"), muteAlerts}};
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
         setStatus(pending ? tr("Deleting calendar…") : tr("Calendar deleted"));
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
    setStatus(tr("Refreshing calendar subscription…"));
    refresh();
  });
}

void AppController::previewIcsImport(const QUrl& file,
                                     const QString& destinationCalendarId) {
  const QString path = file.toLocalFile();
  if (path.isEmpty() || destinationCalendarId.trimmed().isEmpty()) {
    setError(tr("Choose an iCalendar file and writable destination"));
    return;
  }
  send(QStringLiteral("import.preview"),
       {{QStringLiteral("path"), path},
        {QStringLiteral("destinationCalendarId"), destinationCalendarId}},
       [this](const QJsonValue& value) {
         const QVariantMap preview = value.toObject().toVariantMap();
         setStatus(tr("Import preview: %1 event(s), %2 duplicate(s)")
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
    setError(tr("Choose an iCalendar file and writable destination"));
    return;
  }
  send(QStringLiteral("import.commit"),
       {{QStringLiteral("path"), path},
        {QStringLiteral("destinationCalendarId"), destinationCalendarId},
        {QStringLiteral("duplicatePolicy"), duplicatePolicy}},
       [this](const QJsonValue& value) {
         const QVariantMap result = value.toObject().toVariantMap();
         setStatus(tr("Imported %1 event(s); skipped %2; replaced %3")
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
    setError(tr("Choose a local destination for the iCalendar export"));
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
    setStatus(tr("Exported %1 event(s) to %2")
                  .arg(result.value(QStringLiteral("count")).toInt())
                  .arg(result.value(QStringLiteral("path")).toString()));
    emit icsExportCompleted(result);
  });
}

struct AppController::PdfExportJob {
  QString path;
  QJsonArray calendarIds;
  PrintOptions options;
  QVariantList events;
  // The last page's coverage; incomplete while providers still download
  // part of the range.
  QJsonObject coverage;
  int hydrationRounds = 0;
};

namespace {
// How often, and how long, a PDF export waits for a range that providers are
// still downloading before it prints what the cache holds.
constexpr int kPdfHydrationRounds = 3;
constexpr int kPdfHydrationWaitMs = 30 * 1000;
}  // namespace

void AppController::exportPdf(const QVariantMap& options, const QUrl& destination) {
  QString path = destination.toLocalFile();
  if (path.isEmpty()) {
    setError(tr("Choose a local destination for the PDF"));
    return;
  }
  if (!path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
    path.append(QStringLiteral(".pdf"));
  }
  auto job = std::make_shared<PdfExportJob>();
  job->path = path;
  PrintOptions& print = job->options;
  print.firstDate = QDate::fromString(
      options.value(QStringLiteral("firstDate")).toString(), Qt::ISODate);
  print.lastDate = QDate::fromString(
      options.value(QStringLiteral("lastDate")).toString(), Qt::ISODate);
  if (!print.firstDate.isValid() || !print.lastDate.isValid() ||
      print.firstDate > print.lastDate) {
    setError(tr("Choose a valid date range to print"));
    return;
  }
  print.layout =
      options.value(QStringLiteral("layout")).toString() == QStringLiteral("month")
          ? PrintLayout::Month
          : PrintLayout::List;
  if (print.layout == PrintLayout::Month) {
    // A month grid always shows whole months.
    print.firstDate = QDate(print.firstDate.year(), print.firstDate.month(), 1);
    print.lastDate = QDate(print.lastDate.year(), print.lastDate.month(), 1)
                         .addMonths(1)
                         .addDays(-1);
  }
  if (print.firstDate.daysTo(print.lastDate) > 366) {
    setError(tr("Print at most one year at a time"));
    return;
  }
  print.includeDetails = options.value(QStringLiteral("includeDetails")).toBool();
  print.title = options.value(QStringLiteral("title")).toString().trimmed();
  if (print.title.isEmpty()) {
    print.title = QStringLiteral("OmaCalendar");
  }
  const QString timePattern = options.value(QStringLiteral("timePattern")).toString();
  if (!timePattern.isEmpty()) {
    print.timeFormat = timePattern;
  }
  const int firstDay = options.value(QStringLiteral("firstDayOfWeek")).toInt();
  print.firstDayOfWeek = firstDay >= 1 && firstDay <= 7 ? firstDay : 7;

  const bool visibleOnly = options.value(QStringLiteral("visibleOnly"), true).toBool();
  const QStringList visible = visibleCalendarIds();
  for (const QVariant& value : std::as_const(m_calendars)) {
    const QVariantMap calendar = value.toMap();
    const QString id = calendar.value(QStringLiteral("id")).toString();
    if (visibleOnly ? visible.contains(id)
                    : calendar.value(QStringLiteral("enabled"), true).toBool()) {
      job->calendarIds.append(id);
    }
  }
  if (!connected()) {
    setError(tr("Connect to the calendar service to print"));
    return;
  }
  m_pendingPdfJob.reset();
  m_pdfHydrationWait.stop();
  setStatus(tr("Preparing PDF…"));
  requestPdfPage(job, 0, kEventPageLimit);
}

void AppController::requestPdfPage(const std::shared_ptr<PdfExportJob>& job,
                                   const int offset, const int limit) {
  if (job->calendarIds.isEmpty()) {
    finishPdfExport(job);
    return;
  }
  const QJsonObject params = {
      {QStringLiteral("start"),
       isoUtc(startOfDateUtc(job->options.firstDate.addDays(-1)))},
      {QStringLiteral("end"), isoUtc(startOfDateUtc(job->options.lastDate.addDays(2)))},
      {QStringLiteral("calendarIds"), job->calendarIds},
      {QStringLiteral("offset"), offset},
      {QStringLiteral("limit"), limit},
  };
  send(
      QStringLiteral("events.list"), params,
      [this, job, offset, limit](const QJsonValue& value) {
        const QJsonObject page = value.toObject();
        job->events.append(variantList(value, QStringLiteral("events")));
        job->coverage = page.value(QStringLiteral("coverage")).toObject();
        const int nextOffset = page.value(QStringLiteral("nextOffset")).toInt(-1);
        if (page.value(QStringLiteral("hasMore")).toBool() && nextOffset > offset) {
          requestPdfPage(job, nextOffset, limit);
          return;
        }
        // The cache may not hold the whole range yet. The daemon announces
        // finished hydration with events.changed; read the range again then.
        if (!job->coverage.value(QStringLiteral("complete")).toBool(true) &&
            job->coverage.value(QStringLiteral("hydrationScheduled")).toBool() &&
            job->hydrationRounds < kPdfHydrationRounds) {
          m_pendingPdfJob = job;
          m_pdfHydrationWait.start(kPdfHydrationWaitMs);
          setStatus(tr("Downloading events to print…"));
          return;
        }
        finishPdfExport(job);
      },
      false,
      [this, job, offset, limit](const QJsonObject& error) {
        if (error.value(QStringLiteral("code")).toString() ==
                QStringLiteral("response_too_large") &&
            limit > 1) {
          requestPdfPage(job, offset, limit / 2);
          return true;
        }
        return false;
      });
}

void AppController::resumePdfExport(const bool refetch) {
  if (!m_pendingPdfJob) {
    return;
  }
  const std::shared_ptr<PdfExportJob> job = std::move(m_pendingPdfJob);
  m_pendingPdfJob.reset();
  m_pdfHydrationWait.stop();
  if (!refetch) {
    // Waited long enough: print what the cache holds and say so.
    finishPdfExport(job);
    return;
  }
  ++job->hydrationRounds;
  job->events.clear();
  requestPdfPage(job, 0, kEventPageLimit);
}

void AppController::finishPdfExport(const std::shared_ptr<PdfExportJob>& job) {
  applyDisplayTimes(&job->events);
  QHash<QString, QVariantMap> calendarsById;
  for (const QVariant& value : std::as_const(m_calendars)) {
    const QVariantMap calendar = value.toMap();
    calendarsById.insert(calendar.value(QStringLiteral("id")).toString(), calendar);
  }
  const auto wallTime = [](const QVariant& value) {
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
  };
  QList<PrintableEvent> printable;
  printable.reserve(job->events.size());
  for (const QVariant& value : std::as_const(job->events)) {
    const QVariantMap event = value.toMap();
    if (event.value(QStringLiteral("deleted")).toBool() ||
        event.value(QStringLiteral("status")).toString() ==
            QStringLiteral("cancelled")) {
      continue;
    }
    const QVariantMap calendar =
        calendarsById.value(event.value(QStringLiteral("calendarId")).toString());
    PrintableEvent item;
    item.title = event.value(QStringLiteral("summary")).toString();
    item.location = event.value(QStringLiteral("location")).toString();
    item.description = event.value(QStringLiteral("description")).toString();
    item.calendarName = calendar.value(QStringLiteral("name")).toString();
    const QString colorOverride =
        calendar.value(QStringLiteral("colorOverride")).toString();
    item.color = QColor(colorOverride.isEmpty()
                            ? calendar.value(QStringLiteral("color")).toString()
                            : colorOverride);
    item.allDay = event.value(QStringLiteral("allDay")).toBool();
    if (item.allDay) {
      item.startDate = QDate::fromString(
          event.value(QStringLiteral("startDate")).toString(), Qt::ISODate);
      item.endDate = QDate::fromString(
          event.value(QStringLiteral("endDate")).toString(), Qt::ISODate);
    } else {
      item.start = wallTime(event.value(QStringLiteral("displayStartLocal")));
      item.end = wallTime(event.value(QStringLiteral("displayEndLocal")));
    }
    printable.append(item);
  }

  QString error;
  const int pages = writeCalendarPdf(job->path, printable, job->options, &error);
  if (pages <= 0) {
    setError(error.isEmpty() ? tr("The PDF could not be written") : error);
    return;
  }
  setError({});
  const bool complete = job->coverage.value(QStringLiteral("complete")).toBool(true);
  setStatus(complete ? tr("Saved %n page(s) to %1", nullptr, pages).arg(job->path)
                     : tr("Saved %n page(s) to %1; some events may be missing because "
                          "calendars are still downloading",
                          nullptr, pages)
                           .arg(job->path));
  emit pdfExportCompleted(job->path, pages);
}

void AppController::connectGoogle(const QString& displayName) {
  const QString clientId = google::defaultOAuthClientId();
  if (clientId.isEmpty()) {
    setError(
        tr("This build has no bundled Google OAuth client. Enter a Desktop OAuth "
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
                setStatus(tr("Opening Google authorization…"));
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
         setStatus(tr("Opening Google authorization…"));
         emit accountSetupStarted();
         refresh();
       });
}

void AppController::connectGoogleWithClientId(const QString& clientId,
                                              const QString& displayName) {
  const QString normalizedClientId = clientId.trimmed();
  if (normalizedClientId.isEmpty()) {
    setError(tr("Enter a Google Desktop OAuth client ID"));
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
                setStatus(tr("Opening Google authorization…"));
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
    setError(tr("Could not read the selected Google credentials file"));
    return;
  }
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    setError(tr("The selected file is not valid Google credentials JSON"));
    return;
  }
  const QJsonObject root = document.object();
  const QJsonObject installed = root.value(QStringLiteral("installed")).toObject();
  const QString clientId = installed.value(QStringLiteral("client_id")).toString();
  const QString clientSecret =
      installed.value(QStringLiteral("client_secret")).toString();
  if (clientId.isEmpty()) {
    setError(tr("Choose OAuth credentials created as a Google Desktop app"));
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
                setStatus(tr("Opening Google authorization…"));
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
    setError(tr("CalDAV URL, username, and password are required"));
    return;
  }
  setError({});
  send(QStringLiteral("accounts.createCalDav"),
       {{QStringLiteral("endpoint"), endpoint.trimmed()},
        {QStringLiteral("username"), username.trimmed()},
        {QStringLiteral("password"), password},
        {QStringLiteral("displayName"), displayName.trimmed()}},
       [this](const QJsonValue&) {
         setStatus(tr("Discovering CalDAV calendars…"));
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
         setStatus(tr("Opening account authorization…"));
         emit accountSetupStarted();
         refresh();
       });
}

void AppController::updateAccountCredentials(const QString& accountId,
                                             const QString& username,
                                             const QString& password) {
  if (accountId.trimmed().isEmpty()) {
    setError(tr("An account is required"));
    return;
  }
  setError({});
  send(QStringLiteral("accounts.update"),
       {{QStringLiteral("accountId"), accountId.trimmed()},
        {QStringLiteral("username"), username.trimmed()},
        {QStringLiteral("password"), password}},
       [this](const QJsonValue&) {
         setStatus(tr("Account credentials are being updated…"));
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

QVariantMap AppController::secondaryTimeLabels(const QString& dateText,
                                               const QString& zoneId) const {
  const QDate date = QDate::fromString(dateText, Qt::ISODate);
  const QTimeZone secondary(zoneId.trimmed().toUtf8());
  if (!date.isValid() || !secondary.isValid()) {
    return {};
  }
  const QString requestedZone =
      m_preferences.value(QStringLiteral("displayTimeZone")).toString().trimmed();
  QTimeZone displayZone = requestedZone.isEmpty() ? QTimeZone(QTimeZone::LocalTime)
                                                  : QTimeZone(requestedZone.toUtf8());
  if (!displayZone.isValid()) {
    displayZone = QTimeZone(QTimeZone::LocalTime);
  }
  QVariantList hours;
  for (int hour = 0; hour <= 24; ++hour) {
    // Offsets are looked up per hour, so a DST change in either zone on this
    // day shows where it happens.
    const QDateTime wall(date.addDays(hour / 24), QTime(hour % 24, 0), displayZone);
    const QDateTime there = wall.toTimeZone(secondary);
    hours.append(QVariantMap{
        {QStringLiteral("minute"), there.time().hour() * 60 + there.time().minute()},
        {QStringLiteral("dayOffset"), static_cast<int>(date.daysTo(there.date()))}});
  }
  const QDateTime noon(date, QTime(12, 0), displayZone);
  const int offsetMinutes = secondary.offsetFromUtc(noon) / 60;
  QString offset = QStringLiteral("UTC%1%2")
                       .arg(offsetMinutes < 0 ? QLatin1Char('-') : QLatin1Char('+'))
                       .arg(qAbs(offsetMinutes) / 60);
  if (offsetMinutes % 60 != 0) {
    offset +=
        QStringLiteral(":%1").arg(qAbs(offsetMinutes) % 60, 2, 10, QLatin1Char('0'));
  }
  QString city = QString::fromUtf8(secondary.id()).section(QLatin1Char('/'), -1);
  city.replace(QLatin1Char('_'), QLatin1Char(' '));
  return {{QStringLiteral("label"), city},
          {QStringLiteral("offsetLabel"), offset},
          {QStringLiteral("hours"), hours}};
}

QString AppController::wallTimeToUtc(const QString& dateText, const QString& timeText,
                                     const QString& timeZoneId) const {
  const QDate date = QDate::fromString(dateText, Qt::ISODate);
  QTime time = QTime::fromString(timeText, QStringLiteral("HH:mm"));
  if (!time.isValid()) {
    time = QTime::fromString(timeText, Qt::ISODate);
  }
  const QTimeZone zone = timeZoneId.trimmed().isEmpty()
                             ? QTimeZone(QTimeZone::LocalTime)
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
                       ? QTimeZone(QTimeZone::LocalTime)
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
  QTimeZone displayZone = requestedZone.isEmpty() ? QTimeZone(QTimeZone::LocalTime)
                                                  : QTimeZone(requestedZone.toUtf8());
  if (!displayZone.isValid()) {
    displayZone = QTimeZone(QTimeZone::LocalTime);
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

void AppController::setInteractive(const bool interactive) {
  if (interactive == m_interactive) {
    return;
  }
  m_interactive = interactive;
  if (interactive) {
    m_interactiveRenewal.start();
  } else {
    m_interactiveRenewal.stop();
  }
  sendInteractive();
}

void AppController::sendInteractive() {
  if (!connected()) {
    return;
  }
  // Daemons from before this method answer method_not_found; polling then
  // simply stays at its default.
  send(QStringLiteral("sync.setInteractive"),
       {{QStringLiteral("interactive"), m_interactive}}, {}, false,
       [](const QJsonObject&) { return true; });
}

QVariantMap AppController::freeBusy() const { return m_freeBusy; }

QVariantMap AppController::eventAttachments() const { return m_eventAttachments; }

void AppController::loadEventAttachments(const QString& eventId,
                                         const QString& recurrenceId) {
  m_eventAttachments = {{QStringLiteral("eventId"), eventId},
                        {QStringLiteral("recurrenceId"), recurrenceId},
                        {QStringLiteral("attachments"), QVariantList{}}};
  emit eventAttachmentsChanged();
  if (!connected() || eventId.isEmpty()) {
    return;
  }
  QJsonObject params{{QStringLiteral("eventId"), eventId}};
  if (!recurrenceId.isEmpty()) {
    params.insert(QStringLiteral("recurrenceId"), recurrenceId);
  }
  // A failed lookup only means no attachments are shown.
  send(
      QStringLiteral("events.get"), params,
      [this, eventId, recurrenceId](const QJsonValue& value) {
        if (m_eventAttachments.value(QStringLiteral("eventId")).toString() != eventId ||
            m_eventAttachments.value(QStringLiteral("recurrenceId")).toString() !=
                recurrenceId) {
          return;
        }
        m_eventAttachments.insert(QStringLiteral("attachments"),
                                  value.toObject()
                                      .value(QStringLiteral("attachments"))
                                      .toArray()
                                      .toVariantList());
        emit eventAttachmentsChanged();
      },
      false, [](const QJsonObject&) { return true; });
}

QTimeZone AppController::displayTimeZone() const {
  const QString requested =
      m_preferences.value(QStringLiteral("displayTimeZone")).toString().trimmed();
  const QTimeZone zone = requested.isEmpty() ? QTimeZone(QTimeZone::LocalTime)
                                             : QTimeZone(requested.toUtf8());
  return zone.isValid() ? zone : QTimeZone(QTimeZone::LocalTime);
}

void AppController::queryFreeBusy(const QString& start, const QString& end,
                                  const QStringList& emails, const QString& calendarId,
                                  const QString& excludeEventId,
                                  const QString& excludeRecurrenceId) {
  if (!connected()) {
    return;
  }
  QJsonObject params{{QStringLiteral("start"), start},
                     {QStringLiteral("end"), end},
                     {QStringLiteral("attendees"), QJsonArray::fromStringList(emails)}};
  if (!calendarId.isEmpty()) {
    params.insert(QStringLiteral("calendarId"), calendarId);
  }
  if (!excludeEventId.isEmpty()) {
    params.insert(QStringLiteral("excludeEventId"), excludeEventId);
    params.insert(QStringLiteral("excludeRecurrenceId"), excludeRecurrenceId);
  }
  // Older daemons lack freebusy.query; the editor then shows no strip.
  send(
      QStringLiteral("freebusy.query"), params,
      [this](const QJsonValue& value) {
        m_freeBusy = value.toObject().toVariantMap();
        emit freeBusyChanged();
      },
      false, [](const QJsonObject&) { return true; });
}

QString AppController::nextFreeSlot(const QVariantList& busy, const QString& earliest,
                                    const int durationMinutes,
                                    const int workDayStartHour,
                                    const int workDayEndHour, const QString& horizon,
                                    const QString& timeZoneId) const {
  QList<BusyInterval> intervals;
  for (const QVariant& value : busy) {
    const QVariantMap span = value.toMap();
    intervals.append({dateTimeFromIso(span.value(QStringLiteral("start")).toString()),
                      dateTimeFromIso(span.value(QStringLiteral("end")).toString())});
  }
  const QDateTime slot = omacalendar::nextFreeSlot(
      intervals, dateTimeFromIso(earliest), durationMinutes, workDayStartHour,
      workDayEndHour,
      timeZoneId.trimmed().isEmpty() ||
              !QTimeZone(timeZoneId.trimmed().toUtf8()).isValid()
          ? displayTimeZone()
          : QTimeZone(timeZoneId.trimmed().toUtf8()),
      dateTimeFromIso(horizon));
  return slot.isValid() ? isoUtc(slot) : QString();
}

bool AppController::canUndo() const { return !m_undoHistory.isEmpty(); }
bool AppController::canRedo() const { return !m_redoHistory.isEmpty(); }

void AppController::undoLastMutation() { undo(); }

void AppController::undo() {
  if (m_undoHistory.isEmpty()) {
    setStatus(tr("Nothing to undo"));
    return;
  }
  startHistoryStep(HistoryMode::Undo);
}

void AppController::redo() {
  if (m_redoHistory.isEmpty()) {
    setStatus(tr("Nothing to redo"));
    return;
  }
  startHistoryStep(HistoryMode::Redo);
}

void AppController::startHistoryStep(const HistoryMode mode) {
  if (m_historyInFlight != 0) {
    setStatus(tr("Still applying the previous undo"));
    return;
  }
  if (!connected()) {
    setError(tr("Calendar service is not connected"));
    return;
  }
  const HistoryEntry entry =
      mode == HistoryMode::Undo ? m_undoHistory.last() : m_redoHistory.last();
  m_historyInFlight = entry.serial;
  applyHistoryEntry(entry, mode);
}

void AppController::finishHistoryStep(const HistoryMode mode) {
  if (mode == HistoryMode::Record) {
    return;
  }
  dropHistoryStep(mode);
}

void AppController::dropHistoryStep(const HistoryMode mode) {
  QList<HistoryEntry>& source =
      mode == HistoryMode::Undo ? m_undoHistory : m_redoHistory;
  const quint64 serial = std::exchange(m_historyInFlight, 0);
  for (qsizetype index = source.size() - 1; index >= 0; --index) {
    if (source.at(index).serial == serial) {
      source.removeAt(index);
      emit historyChanged();
      return;
    }
  }
}

AppController::ErrorHandler AppController::historyFailureHandler(
    const HistoryMode mode) {
  if (mode == HistoryMode::Record) {
    return {};
  }
  return [this](const QJsonObject&) {
    m_historyInFlight = 0;
    return false;
  };
}

void AppController::recordInverse(const HistoryMode mode, const HistoryEntry& inverse) {
  QList<HistoryEntry>& target =
      mode == HistoryMode::Undo ? m_redoHistory : m_undoHistory;
  if (mode == HistoryMode::Record) {
    m_redoHistory.clear();
  }
  HistoryEntry entry = inverse;
  entry.serial = ++m_historySerial;
  target.append(entry);
  while (target.size() > kHistoryLimit) {
    target.removeFirst();
  }
  emit historyChanged();
}

void AppController::applyHistoryEntry(const HistoryEntry& entry,
                                      const HistoryMode mode) {
  switch (entry.kind) {
    case HistoryEntry::Kind::Restore:
      saveEventWithHistory(entry.event, entry.options, mode, entry.previous);
      return;
    case HistoryEntry::Kind::Remove: {
      QVariantMap options = entry.options;
      options.insert(QStringLiteral("expectedLocalRevision"),
                     entry.event.value(QStringLiteral("localRevision"), -1));
      options.insert(QStringLiteral("recurrenceId"),
                     entry.event.value(QStringLiteral("recurrenceId")));
      deleteEventWithHistory(entry.event.value(QStringLiteral("id")).toString(),
                             options, mode);
      return;
    }
    case HistoryEntry::Kind::Undelete:
      if (QDateTime::currentDateTimeUtc() > entry.expiresAt) {
        dropHistoryStep(mode);
        setStatus(tr("The delete can no longer be undone"));
        return;
      }
      undeleteWithHistory(entry, mode);
      return;
  }
}

void AppController::undeleteWithHistory(const HistoryEntry& entry,
                                        const HistoryMode mode) {
  send(
      QStringLiteral("events.undo"),
      {{QStringLiteral("clientMutationId"), newUuid()},
       {QStringLiteral("undoToken"), entry.undoToken}},
      [this, entry, mode](const QJsonValue& value) {
        finishHistoryStep(mode);
        // Deleting the restored event again redoes (or re-undoes) the step.
        HistoryEntry inverse;
        inverse.kind = HistoryEntry::Kind::Remove;
        inverse.event =
            value.toObject().value(QStringLiteral("event")).toObject().toVariantMap();
        if (inverse.event.value(QStringLiteral("id")).toString().isEmpty()) {
          inverse.event = entry.event;
        }
        inverse.options = entry.options;
        recordInverse(mode, inverse);
        setStatus(mode == HistoryMode::Undo ? tr("Undone") : tr("Redone"));
        refresh();
      },
      true,
      [this, mode](const QJsonObject& error) {
        if (error.value(QStringLiteral("code")).toString() ==
            QStringLiteral("undo_expired")) {
          // The token is spent for good, so the entry goes too.
          dropHistoryStep(mode);
          setStatus(tr("The delete can no longer be undone"));
          return true;
        }
        m_historyInFlight = 0;
        return false;
      });
}

bool AppController::canOpenExternalEventUrl(const QString& value) const {
  return validatedExternalEventUrl(value).isValid();
}

void AppController::openExternalEventUrl(const QString& value) {
  const QUrl url = validatedExternalEventUrl(value);
  if (!url.isValid()) {
    setError(tr("Only valid HTTP or HTTPS event links can be opened"));
    return;
  }
  if (!QDesktopServices::openUrl(url)) {
    setError(tr("The event link could not be opened"));
    return;
  }
  setError({});
}

void AppController::installWidget() {
  const QString sourceCommit = QString::fromLatin1(kWidgetSourceCommit);
  if (!isExactGitCommit(sourceCommit)) {
    setError(tr("No verified OmaCalendar widget beta commit is configured"));
    return;
  }
  const QString executable =
      QStandardPaths::findExecutable(QStringLiteral("omacalendar-widgetctl"));
  if (executable.isEmpty()) {
    setError(tr("omacalendar-widgetctl is not installed"));
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
              setStatus(installed ? tr("OmaCalendar widget installed")
                                  : tr("Widget activation completed"));
            } else {
              const QString message = result.value(QStringLiteral("error"))
                                          .toObject()
                                          .value(QStringLiteral("message"))
                                          .toString();
              setError(!message.isEmpty()  ? message
                       : failure.isEmpty() ? tr("Widget install failed")
                                           : failure.left(500));
            }
          });
  process->start(executable,
                 {QStringLiteral("install"), QStringLiteral("--source"),
                  QString::fromLatin1(kWidgetSourceUrl), QStringLiteral("--source-ref"),
                  QString::fromLatin1(kWidgetSourceRef),
                  QStringLiteral("--source-commit"), sourceCommit});
}

void AppController::restoreOmarchyClock() {
  const QString executable =
      QStandardPaths::findExecutable(QStringLiteral("omacalendar-widgetctl"));
  if (executable.isEmpty()) {
    setError(tr("omacalendar-widgetctl is not installed"));
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
              setStatus(tr("Omarchy clock restored"));
            } else {
              const QString message = result.value(QStringLiteral("error"))
                                          .toObject()
                                          .value(QStringLiteral("message"))
                                          .toString();
              setError(!message.isEmpty()  ? message
                       : failure.isEmpty() ? tr("Clock restore failed")
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
    setError(tr("This OmaCalendar link is not valid"));
    return;
  }
  m_pendingDeepLink = url;
  processPendingDeepLink();
}

void AppController::handleIcsImportFile(const QUrl& file) {
  const QUrl normalized = normalizedLocalIcsUrl(file);
  if (!normalized.isValid()) {
    setError(tr("Only a readable local .ics file can be imported"));
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
      setError(tr("The event link is missing an event identifier"));
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
    setError(tr("This OmaCalendar link is not supported"));
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
      setError(tr("Could not create diagnostics preview"));
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
      setError(tr("Could not save diagnostics preview"));
      return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    setStatus(tr("Opened privacy-safe diagnostics preview"));
  });
}

void AppController::setAccountSyncState(const QString& accountId,
                                        const QJsonObject& status) {
  if (accountId.isEmpty()) {
    return;
  }
  // Calendar providers and ICS subscriptions name their fields differently.
  const QString message = status.value(QStringLiteral("message")).toString();
  const QString lastSyncAt = status.value(QStringLiteral("lastSyncAt")).toString();
  const QVariantMap state{
      {QStringLiteral("state"), status.value(QStringLiteral("state")).toString()},
      {QStringLiteral("errorCode"),
       status.value(QStringLiteral("errorCode")).toString()},
      {QStringLiteral("message"),
       message.isEmpty() ? status.value(QStringLiteral("errorMessage")).toString()
                         : message},
      {QStringLiteral("lastSyncAt"),
       lastSyncAt.isEmpty() ? status.value(QStringLiteral("lastSuccessAt")).toString()
                            : lastSyncAt}};
  if (m_accountSyncStates.value(accountId).toMap() == state) {
    return;
  }
  m_accountSyncStates.insert(accountId, state);
  emit accountSyncStatesChanged();
}

void AppController::refreshAccountSyncStates(const bool icsOnly) {
  if (!connected()) {
    return;
  }
  QSet<QString> known;
  for (const QVariant& value : std::as_const(m_accounts)) {
    const QVariantMap account = value.toMap();
    const QString accountId = account.value(QStringLiteral("id")).toString();
    const QString provider = account.value(QStringLiteral("provider")).toString();
    known.insert(accountId);
    if (accountId.isEmpty() || provider == QStringLiteral("local") ||
        (icsOnly && provider != QStringLiteral("ics"))) {
      continue;
    }
    send(
        QStringLiteral("sync.status"), {{QStringLiteral("accountId"), accountId}},
        [this, accountId](const QJsonValue& result) {
          setAccountSyncState(accountId, result.toObject());
        },
        false, [](const QJsonObject&) { return true; });
  }
  bool removed = false;
  for (auto it = m_accountSyncStates.begin(); it != m_accountSyncStates.end();) {
    if (known.contains(it.key())) {
      ++it;
    } else {
      it = m_accountSyncStates.erase(it);
      removed = true;
    }
  }
  if (removed) {
    emit accountSyncStatesChanged();
  }
}

void AppController::syncAll() {
  send(QStringLiteral("sync.all"), {},
       [this](const QJsonValue&) { setStatus(tr("Synchronizing calendars…")); });
}

void AppController::syncAccount(const QString& accountId) {
  if (accountId.isEmpty()) {
    return;
  }
  send(QStringLiteral("sync.account"), {{QStringLiteral("accountId"), accountId}},
       [this](const QJsonValue&) {
         setStatus(tr("Account sync started"));
         refresh();
       });
}

void AppController::probeThisAndFuture(const QString& calendarId) {
  if (calendarId.isEmpty()) {
    return;
  }
  send(QStringLiteral("calendars.probeThisAndFuture"),
       {{QStringLiteral("calendarId"), calendarId}}, [this](const QJsonValue& result) {
         setStatus(result.toObject().value(QStringLiteral("state")).toString() ==
                           QStringLiteral("supported")
                       ? tr("This and future occurrences are supported")
                       : tr("Checking this-and-future support; a temporary test event "
                            "will be removed after the check"));
         refresh();
       });
}

void AppController::setSelectedDate(const QDateTime& date) {
  const QDate localDate = date.toLocalTime().date();
  if (!date.isValid() || localDate == m_selectedDate) {
    return;
  }
  m_selectedDate = localDate;
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
