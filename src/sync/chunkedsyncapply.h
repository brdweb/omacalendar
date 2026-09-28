#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <functional>

#include "core/database.h"

namespace omacalendar {

// One calendar response is applied on the database-owning thread in short,
// independently committed transactions. Cursors/coverage/validators are only
// published by the last transaction. The owning provider keeps its job active
// until completion; cancel before removing the calendar or account.
class ChunkedSyncApply final : public QObject {
  Q_OBJECT

 public:
  struct Request {
    enum class Kind { Incremental, Range, IcsFeed };
    Kind kind = Kind::Incremental;
    Calendar calendar;
    QList<Event> events;
    QStringList deletedRemoteIds;
    QStringList prunedRemoteIds;
    QList<ProviderResource> resources;
    QDateTime coverageStartUtc;
    QDateTime coverageEndUtc;
    bool replaceCoverage = false;
    QString etag;
    QString lastModified;
    QDateTime successAt;
  };

  explicit ChunkedSyncApply(Database* database, QObject* parent = nullptr);
  void start(Request request, std::function<void(bool, const QString&)> completed);
  void cancel();

 signals:
  void chunkCommitted(const QString& calendarId);

 private:
  void step();

  Database* m_database;
  Request m_request;
  std::function<void(bool, const QString&)> m_completed;
  QHash<QString, ProviderResource> m_resourcesByKey;
  qsizetype m_eventIndex = 0;
  qsizetype m_deletedIndex = 0;
  qsizetype m_prunedIndex = 0;
  qsizetype m_resourceIndex = 0;
  bool m_cancelled = false;
};

}  // namespace omacalendar
