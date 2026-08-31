#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QTimer>

#include "core/database.h"
#include "sync/localprovider.h"
#include "sync/provider.h"

namespace omacalendar::caldav {
class CalDavSync;
}
namespace omacalendar::google {
class GoogleSync;
}
namespace omacalendar::ics {
class IcsService;
}

namespace omacalendar {

// Owns provider routing and the cross-provider dependency queue. Provider
// adapters remain responsible for protocol details; clients never select an
// adapter or handle provider-owned metadata directly.
class SyncCoordinator final : public QObject {
  Q_OBJECT

 public:
  SyncCoordinator(Database* database, google::GoogleSync* google,
                  caldav::CalDavSync* caldav, ics::IcsService* ics,
                  QObject* parent = nullptr);

  void start();
  void syncAll();
  bool syncAccount(const QString& accountId, QString* errorMessage = nullptr);
  bool syncCalendar(const QString& calendarId, QString* errorMessage = nullptr);
  [[nodiscard]] QJsonObject ensureRangeHydrated(const QDateTime& startUtc,
                                                const QDateTime& endUtc,
                                                const QStringList& calendarIds = {},
                                                QString* errorMessage = nullptr);
  [[nodiscard]] QJsonObject status(const QString& accountId = {}) const;

 signals:
  void accountChanged(const QString& accountId);
  void calendarsChanged(const QString& accountId);
  void eventsChanged(const QStringList& calendarIds);
  void syncStatusChanged(const QString& accountId, const QJsonObject& status);
  void operationStateChanged();
  void rangeHydrationScheduled(const QString& calendarId, const QDateTime& startUtc,
                               const QDateTime& endUtc);

 private:
  [[nodiscard]] Provider* providerForKind(ProviderKind kind) const;
  void runOlderHistoryBackfill();

  Database* m_database = nullptr;
  LocalProvider m_local;
  QList<Provider*> m_providers;
  QTimer m_backfillTimer;
  qsizetype m_backfillCursor = 0;
};

}  // namespace omacalendar
