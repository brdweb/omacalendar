#include "sync/localprovider.h"

#include <QDateTime>

namespace omacalendar {

LocalProvider::LocalProvider(Database* database, QObject* parent)
    : Provider(QStringLiteral("local"), ProviderKind::Local, parent),
      m_database(database) {
  m_drainTimer.setInterval(1000);
  connect(&m_drainTimer, &QTimer::timeout, this, &LocalProvider::syncAll);
}

ProviderCapabilities LocalProvider::capabilities() const {
  ProviderCapabilities value;
  value.accountDiscovery = false;
  value.calendarDiscovery = false;
  value.incrementalSync = false;
  value.attendees = true;
  value.reminders = true;
  value.conferenceData = true;
  return value;
}

void LocalProvider::start() {
  if (!m_drainTimer.isActive()) {
    m_drainTimer.start();
  }
  QTimer::singleShot(0, this, &LocalProvider::syncAll);
}

void LocalProvider::syncAll() { drain(); }

void LocalProvider::syncAccount(const QString& accountId) { drain(accountId); }

QJsonObject LocalProvider::status(const QString& accountId) const {
  return {{QStringLiteral("accountId"), accountId},
          {QStringLiteral("provider"), QStringLiteral("local")},
          {QStringLiteral("state"), QStringLiteral("idle")}};
}

void LocalProvider::drain(const QString& accountId) {
  if (m_database == nullptr || !m_database->isOpen()) {
    emit syncStatusChanged(
        accountId,
        {{QStringLiteral("accountId"), accountId},
         {QStringLiteral("provider"), QStringLiteral("local")},
         {QStringLiteral("state"), QStringLiteral("error")},
         {QStringLiteral("errorCode"), QStringLiteral("database_unavailable")},
         {QStringLiteral("message"), QStringLiteral("Database is unavailable")}});
    return;
  }

  QString databaseError;
  const QList<OutboxItem> ready = m_database->readyOutbox(100, &databaseError);
  if (!databaseError.isEmpty()) {
    emit syncStatusChanged(
        accountId, {{QStringLiteral("accountId"), accountId},
                    {QStringLiteral("provider"), QStringLiteral("local")},
                    {QStringLiteral("state"), QStringLiteral("error")},
                    {QStringLiteral("errorCode"), QStringLiteral("database_error")},
                    {QStringLiteral("message"),
                     QStringLiteral("The local mutation queue could not be read")}});
    return;
  }

  QStringList changedCalendars;
  for (const OutboxItem& item : ready) {
    const Account account = m_database->account(item.accountId);
    if (account.provider != ProviderKind::Local ||
        (!accountId.isEmpty() && item.accountId != accountId)) {
      continue;
    }
    if (!m_database->updateOutboxState(item.id, OutboxState::Sending, item.attempts,
                                       QDateTime::currentDateTimeUtc())) {
      continue;
    }
    if (!m_database->completeOutbox(item.id)) {
      m_database->updateOutboxState(
          item.id, OutboxState::RetryWait, item.attempts + 1,
          QDateTime::currentDateTimeUtc().addSecs(1),
          QStringLiteral("local_acknowledgment_failed"),
          QStringLiteral("The local mutation could not be acknowledged"));
      emit operationStateChanged();
      continue;
    }
    changedCalendars.append(item.calendarId);
    emit operationStateChanged();
  }
  changedCalendars.removeDuplicates();
  if (!changedCalendars.isEmpty()) {
    emit eventsChanged(changedCalendars);
  }
}

}  // namespace omacalendar
