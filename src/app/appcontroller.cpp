#include "appcontroller.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>

#include "core/domain.h"
#include "core/paths.h"
#include "providers/google/googleoauthconfig.h"

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

}  // namespace

AppController::AppController(QObject* parent) : QObject(parent) {
  m_client.setAutoReconnect(true);
  connect(&m_client, &ipc::IpcClient::connectedChanged, this, [this]() {
    emit connectedChanged();
    if (connected()) {
      m_daemonStartAttempted = false;
      setError({});
      setStatus(QStringLiteral("Calendar service connected"));
      refresh();
    } else {
      m_pending.clear();
      m_activeRequests = 0;
      setBusy(false);
      setStatus(QStringLiteral("Reconnecting to calendar service…"));
      QTimer::singleShot(500, this, &AppController::startDaemonIfNeeded);
    }
  });
  connect(&m_client, &ipc::IpcClient::responseReceived, this,
          [this](const QString& id, const QJsonValue& result) {
            const ResultHandler handler = m_pending.take(id);
            if (m_activeRequests > 0) {
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
            if (m_activeRequests > 0) {
              --m_activeRequests;
              setBusy(m_activeRequests > 0);
            }
            setError(error.value(QStringLiteral("message"))
                         .toString(QStringLiteral("Calendar service request failed")));
          });
  connect(&m_client, &ipc::IpcClient::notificationReceived, this,
          [this](const QString& event, const QJsonObject& data) {
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
                event == QStringLiteral("accounts.changed")) {
              refresh();
            } else if (event == QStringLiteral("sync.statusChanged")) {
              const QJsonObject status =
                  data.value(QStringLiteral("status")).toObject();
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
  QTimer::singleShot(600, this, &AppController::startDaemonIfNeeded);
}

bool AppController::connected() const { return m_client.isConnected(); }
bool AppController::busy() const { return m_activeRequests > 0; }
QString AppController::statusText() const { return m_statusText; }
QString AppController::lastError() const { return m_lastError; }
QVariantList AppController::accounts() const { return m_accounts; }
QVariantList AppController::calendars() const { return m_calendars; }
QVariantList AppController::events() const { return m_events; }
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
                            ResultHandler handler) {
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
  ++m_activeRequests;
  setBusy(true);
  return id;
}

void AppController::refresh() {
  if (!connected()) {
    return;
  }
  send(QStringLiteral("accounts.list"), {}, [this](const QJsonValue& value) {
    m_accounts = variantList(value, QStringLiteral("accounts"));
    emit accountsChanged();
  });
  send(QStringLiteral("calendars.list"), {}, [this](const QJsonValue& value) {
    m_calendars = variantList(value, QStringLiteral("calendars"));
    emit calendarsChanged();
  });
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
  send(QStringLiteral("events.list"), params, [this](const QJsonValue& value) {
    m_events = variantList(value, QStringLiteral("events"));
    setStatus(QStringLiteral("Calendar is up to date locally"));
    emit eventsChanged();
  });
}

void AppController::createEvent(const QVariantMap& values) {
  QJsonObject event = QJsonObject::fromVariantMap(values);
  send(QStringLiteral("events.create"), {{QStringLiteral("event"), event}},
       [this](const QJsonValue&) {
         emit eventSaved();
         loadRange(m_rangeStart, m_rangeEnd);
       });
}

void AppController::updateEvent(const QVariantMap& values) {
  QJsonObject event = QJsonObject::fromVariantMap(values);
  send(QStringLiteral("events.update"), {{QStringLiteral("event"), event}},
       [this](const QJsonValue&) {
         emit eventSaved();
         loadRange(m_rangeStart, m_rangeEnd);
       });
}

void AppController::removeEvent(const QString& eventId) {
  if (eventId.isEmpty()) {
    return;
  }
  send(QStringLiteral("events.remove"), {{QStringLiteral("eventId"), eventId}},
       [this](const QJsonValue&) { loadRange(m_rangeStart, m_rangeEnd); });
}

void AppController::connectGoogle(const QString& displayName) {
  setError({});
  send(QStringLiteral("google.configureClient"),
       {{QStringLiteral("clientId"), google::defaultOAuthClientId()},
        {QStringLiteral("clientSecret"), QString()}},
       [this, displayName](const QJsonValue&) {
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
  if (accountId.isEmpty()) {
    return;
  }
  send(QStringLiteral("accounts.remove"), {{QStringLiteral("accountId"), accountId}},
       [this](const QJsonValue&) { refresh(); });
}

void AppController::syncAll() {
  send(QStringLiteral("sync.all"), {}, [this](const QJsonValue&) {
    setStatus(QStringLiteral("Synchronizing calendars…"));
  });
}

void AppController::setSelectedDate(const QDate& date) {
  if (!date.isValid() || date == m_selectedDate) {
    return;
  }
  m_selectedDate = date;
  emit selectedDateChanged();
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
