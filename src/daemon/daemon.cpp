#include "daemon.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonParseError>
#include <QSet>
#include <QString>
#include <algorithm>
#include <utility>

#include "core/domain.h"
#include "core/paths.h"
#include "core/widgeteventquery.h"
#include "daemon/daemon_p.h"
#include "daemon/invitationclassification.h"

namespace {

constexpr int kDefaultInvitationPageLimit = 500;
constexpr int kMaxInvitationPageLimit = 500;
constexpr int kWidgetInvitationLimit = 100;

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
      QStringLiteral("system"),      QStringLiteral("tasks"),
      QStringLiteral("widget"),
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

}  // namespace

namespace omacalendar {

using namespace detail;

Daemon::Daemon(QObject* parent)
    : QObject(parent),
      m_server(&m_router, this),
      m_google(&m_database, this),
      m_caldav(&m_database, this),
      m_ics(&m_database, this),
      m_sync(&m_database, &m_google, &m_caldav, &m_ics, this),
      m_activity({}, this),
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
      m_reminders.syncCompleted(accountId);
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
  connect(&m_caldav, &caldav::CalDavSync::tasksChanged, this,
          [this](const QStringList& listIds) { emitTasksChanged(listIds); });
  connect(&m_google, &google::GoogleSync::tasksChanged, this,
          [this](const QStringList& listIds) { emitTasksChanged(listIds); });
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
  connect(&m_activity, &SyncActivity::pollIntervalChanged, this,
          [this](const int intervalMs) { m_sync.setPollInterval(intervalMs); });
  connect(&m_activity, &SyncActivity::syncRequested, this, [this]() {
    m_sync.syncAll();
    m_ics.refreshAll(true);
  });
  m_activity.connectSystemSignals();
  return true;
}

QString Daemon::socketPath() const { return m_server.path(); }

int Daemon::connectedClients() const { return m_server.clientCount(); }

QStringList Daemon::methods() const { return m_router.methods(); }

void Daemon::registerHandlers() {
  registerTaskHandlers();
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
  m_router.registerHandler(QStringLiteral("system.backup"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onSystemBackup(params, error);
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
  m_router.registerHandler(QStringLiteral("calendars.probeThisAndFuture"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onCalendarsProbeThisAndFuture(params, error);
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
  m_router.registerHandler(QStringLiteral("stats.dailyCounts"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onStatsDailyCounts(params, error);
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
  m_router.registerHandler(QStringLiteral("contacts.suggest"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onContactsSuggest(params, error);
                           });
  m_router.registerHandler(QStringLiteral("settings.getMany"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onSettingsGetMany(params, error);
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
  m_router.registerHandler(QStringLiteral("freebusy.query"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onFreeBusyQuery(params, error);
                           });
  m_router.registerHandler(QStringLiteral("sync.setInteractive"),
                           [this](const QJsonObject& params, ipc::Error* error) {
                             return onSyncSetInteractive(params, error);
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

QJsonValue Daemon::onSystemBackup(const QJsonObject&, ipc::Error* error) {
  // Older copies beyond this many are pruned after each successful backup.
  constexpr int kBackupsKept = 5;
  DatabaseBackup backup;
  QString dbError;
  if (!m_database.createBackup(paths::backupDirectory(), kBackupsKept,
                               QDateTime::currentDateTimeUtc(), &backup, &dbError)) {
    if (error != nullptr) {
      *error = {QStringLiteral("backup_failed"), dbError, false};
    }
    return {};
  }
  return QJsonObject{
      {QStringLiteral("path"), backup.path},
      {QStringLiteral("sizeBytes"), backup.sizeBytes},
      {QStringLiteral("createdAt"), backup.createdAt.toString(Qt::ISODate)},
      {QStringLiteral("kept"), kBackupsKept},
  };
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
    m_invitationReadCacheExpiresAt = invitationCacheExpiry(m_invitationReadCache, now);
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
  const InvitationBucketTotals bucketTotals = invitationBucketTotals(events, now);
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
                     {QStringLiteral("upcomingTotal"), bucketTotals.upcoming},
                     {QStringLiteral("pastTotal"), bucketTotals.past},
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
