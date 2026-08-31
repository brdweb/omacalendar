#include "sync/synccoordinator.h"

#include <QJsonArray>
#include <QTimeZone>

#include "providers/caldav/caldavsync.h"
#include "providers/google/googlesync.h"
#include "providers/ics/icsservice.h"

namespace omacalendar {

SyncCoordinator::SyncCoordinator(Database* database, google::GoogleSync* google,
                                 caldav::CalDavSync* caldav, ics::IcsService* ics,
                                 QObject* parent)
    : QObject(parent),
      m_database(database),
      m_local(database, this),
      m_providers{google, caldav, ics, &m_local} {
  m_backfillTimer.setInterval(15 * 60 * 1000);
  connect(&m_backfillTimer, &QTimer::timeout, this,
          &SyncCoordinator::runOlderHistoryBackfill);
  for (Provider* provider : std::as_const(m_providers)) {
    if (provider == nullptr) {
      continue;
    }
    connect(provider, &Provider::accountChanged, this,
            &SyncCoordinator::accountChanged);
    connect(provider, &Provider::calendarsChanged, this,
            &SyncCoordinator::calendarsChanged);
    connect(provider, &Provider::eventsChanged, this, &SyncCoordinator::eventsChanged);
    connect(provider, &Provider::syncStatusChanged, this,
            &SyncCoordinator::syncStatusChanged);
    connect(provider, &Provider::operationStateChanged, this,
            &SyncCoordinator::operationStateChanged);
  }
}

void SyncCoordinator::start() {
  for (Provider* provider : std::as_const(m_providers)) {
    if (provider != nullptr) {
      provider->start();
    }
  }
  if (!m_backfillTimer.isActive()) {
    m_backfillTimer.start();
    // Initial foreground hydration gets priority. Older history starts later
    // and advances only one one-year chunk per timer tick.
    QTimer::singleShot(60 * 1000, this, &SyncCoordinator::runOlderHistoryBackfill);
  }
}

void SyncCoordinator::syncAll() {
  for (Provider* provider : std::as_const(m_providers)) {
    if (provider != nullptr) {
      provider->syncAll();
    }
  }
}

bool SyncCoordinator::syncAccount(const QString& accountId, QString* errorMessage) {
  if (m_database == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Database is unavailable");
    }
    return false;
  }
  const Account account = m_database->account(accountId, errorMessage);
  if (account.id.isEmpty()) {
    return false;
  }
  Provider* provider = providerForKind(account.provider);
  if (provider == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Unsupported account provider");
    }
    return false;
  }
  provider->syncAccount(accountId);
  return true;
}

bool SyncCoordinator::syncCalendar(const QString& calendarId, QString* errorMessage) {
  const Calendar calendar = m_database->calendar(calendarId, errorMessage);
  return !calendar.id.isEmpty() && syncAccount(calendar.accountId, errorMessage);
}

QJsonObject SyncCoordinator::ensureRangeHydrated(const QDateTime& startUtc,
                                                 const QDateTime& endUtc,
                                                 const QStringList& calendarIds,
                                                 QString* errorMessage) {
  QJsonArray uncovered;
  bool scheduled = false;
  if (m_database == nullptr || !startUtc.isValid() || !endUtc.isValid() ||
      startUtc >= endUtc) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("Range hydration needs valid UTC bounds");
    }
    return {{QStringLiteral("complete"), false},
            {QStringLiteral("hydrationScheduled"), false}};
  }
  // UI views are bounded to a year, but keep a generous ceiling for callers
  // that prefetch adjacent periods. Very large exports must not trigger an
  // unbounded provider request as a side effect of reading the cache.
  if (startUtc.daysTo(endUtc) > 5 * 366) {
    return {{QStringLiteral("complete"), false},
            {QStringLiteral("hydrationScheduled"), false},
            {QStringLiteral("rangeTooLarge"), true}};
  }

  QString databaseError;
  QList<Calendar> calendars;
  if (calendarIds.isEmpty()) {
    calendars = m_database->calendars({}, &databaseError);
  } else {
    for (const QString& calendarId : calendarIds) {
      const Calendar calendar = m_database->calendar(calendarId, &databaseError);
      if (calendar.id.isEmpty() || !databaseError.isEmpty()) {
        break;
      }
      calendars.append(calendar);
    }
  }
  if (!databaseError.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = databaseError;
    }
    return {{QStringLiteral("complete"), false},
            {QStringLiteral("hydrationScheduled"), false}};
  }

  for (const Calendar& calendar : std::as_const(calendars)) {
    if (!calendar.enabled) {
      continue;
    }
    const Account account = m_database->account(calendar.accountId, &databaseError);
    if (!databaseError.isEmpty()) {
      break;
    }
    // Device calendars and full-feed ICS subscriptions have no partial range
    // semantics. Coverage is meaningful only for providers that can issue a
    // bounded server query.
    if (account.provider != ProviderKind::Google &&
        account.provider != ProviderKind::CalDav) {
      continue;
    }
    if (m_database->isSyncRangeCovered(calendar.id, startUtc, endUtc, &databaseError)) {
      continue;
    }
    if (!databaseError.isEmpty()) {
      break;
    }
    uncovered.append(calendar.id);
    Provider* provider = providerForKind(account.provider);
    if (provider == nullptr) {
      continue;
    }
    RangeSyncRequest request{calendar.id, startUtc.toUTC(), endUtc.toUTC()};
    QString providerError;
    if (provider->syncRange(request, &providerError)) {
      scheduled = true;
      emit rangeHydrationScheduled(calendar.id, request.startUtc, request.endUtc);
    }
  }
  if (!databaseError.isEmpty() && errorMessage != nullptr) {
    *errorMessage = databaseError;
  }
  return {{QStringLiteral("complete"), uncovered.isEmpty()},
          {QStringLiteral("hydrationScheduled"), scheduled},
          {QStringLiteral("uncoveredCalendarIds"), uncovered}};
}

void SyncCoordinator::runOlderHistoryBackfill() {
  if (m_database == nullptr || !m_database->isOpen()) {
    return;
  }
  QString error;
  const QList<Calendar> calendars = m_database->calendars({}, &error);
  if (!error.isEmpty() || calendars.isEmpty()) {
    return;
  }
  const QDateTime floor(QDate(1970, 1, 1), QTime(0, 0), QTimeZone::UTC);
  for (qsizetype checked = 0; checked < calendars.size(); ++checked) {
    const qsizetype index = (m_backfillCursor + checked) % calendars.size();
    const Calendar& calendar = calendars.at(index);
    const Account account = m_database->account(calendar.accountId);
    if (!calendar.enabled || (account.provider != ProviderKind::Google &&
                              account.provider != ProviderKind::CalDav)) {
      continue;
    }
    const QList<SyncCoverage> coverage = m_database->syncCoverage(calendar.id, &error);
    if (!error.isEmpty() || coverage.isEmpty()) {
      return;
    }
    const QDateTime end = coverage.first().startUtc;
    const QDateTime start = end.addYears(-1);
    if (!start.isValid() || end <= floor) {
      continue;
    }
    Provider* provider = providerForKind(account.provider);
    if (provider != nullptr &&
        provider->syncRange({calendar.id, qMax(start, floor), end})) {
      emit rangeHydrationScheduled(calendar.id, qMax(start, floor), end);
      m_backfillCursor = (index + 1) % calendars.size();
      return;
    }
  }
}

QJsonObject SyncCoordinator::status(const QString& accountId) const {
  if (!accountId.isEmpty()) {
    const Account account = m_database->account(accountId);
    if (Provider* provider = providerForKind(account.provider); provider != nullptr) {
      return provider->status(accountId);
    }
    return {{QStringLiteral("accountId"), accountId},
            {QStringLiteral("state"), QStringLiteral("unsupported")},
            {QStringLiteral("provider"), providerKindToString(account.provider)}};
  }
  QJsonObject result;
  for (Provider* provider : m_providers) {
    if (provider != nullptr) {
      result.insert(provider->id(), provider->status());
    }
  }
  return result;
}

Provider* SyncCoordinator::providerForKind(const ProviderKind kind) const {
  for (Provider* provider : m_providers) {
    if (provider != nullptr && provider->kind() == kind) {
      return provider;
    }
  }
  return nullptr;
}

}  // namespace omacalendar
