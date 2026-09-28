#include "sync/chunkedsyncapply.h"

#include <QSet>
#include <QTimer>
#include <algorithm>
#include <utility>

namespace omacalendar {
namespace {

constexpr qsizetype kChangesPerTurn = 32;

QString resourceKey(const QString& remoteId) {
  const qsizetype separator = remoteId.indexOf(QLatin1Char('#'));
  return separator < 0 ? remoteId : remoteId.left(separator);
}

}  // namespace

ChunkedSyncApply::ChunkedSyncApply(Database* database, QObject* parent)
    : QObject(parent), m_database(database) {}

void ChunkedSyncApply::start(Request request,
                             std::function<void(bool, const QString&)> completed) {
  m_request = std::move(request);
  m_completed = std::move(completed);
  for (const ProviderResource& resource : std::as_const(m_request.resources)) {
    m_resourcesByKey.insert(resource.canonicalKey, resource);
  }
  QTimer::singleShot(0, this, &ChunkedSyncApply::step);
}

void ChunkedSyncApply::cancel() { m_cancelled = true; }

void ChunkedSyncApply::step() {
  if (m_cancelled) {
    return;
  }
  const auto takeCount = [](const qsizetype size, const qsizetype index) {
    return std::min(kChangesPerTurn, size - index);
  };
  const qsizetype eventCount = takeCount(m_request.events.size(), m_eventIndex);
  const qsizetype deletedCount =
      takeCount(m_request.deletedRemoteIds.size(), m_deletedIndex);
  const qsizetype prunedCount =
      takeCount(m_request.prunedRemoteIds.size(), m_prunedIndex);
  const qsizetype resourceCount =
      takeCount(m_request.resources.size(), m_resourceIndex);
  const QList<Event> events = m_request.events.mid(m_eventIndex, eventCount);
  const QStringList deleted =
      m_request.deletedRemoteIds.mid(m_deletedIndex, deletedCount);
  const QStringList pruned = m_request.prunedRemoteIds.mid(m_prunedIndex, prunedCount);
  QList<ProviderResource> resources =
      m_request.resources.mid(m_resourceIndex, resourceCount);
  QSet<QString> includedResources;
  for (const ProviderResource& resource : std::as_const(resources)) {
    includedResources.insert(resource.canonicalKey);
  }
  for (const Event& event : events) {
    const QString key = resourceKey(event.remoteId);
    const auto resource = m_resourcesByKey.constFind(key);
    if (resource != m_resourcesByKey.constEnd() && !includedResources.contains(key)) {
      resources.append(*resource);
      includedResources.insert(key);
    }
  }
  m_eventIndex += eventCount;
  m_deletedIndex += deletedCount;
  m_prunedIndex += prunedCount;
  m_resourceIndex += resourceCount;
  const bool finalChunk = m_eventIndex == m_request.events.size() &&
                          m_deletedIndex == m_request.deletedRemoteIds.size() &&
                          m_prunedIndex == m_request.prunedRemoteIds.size() &&
                          m_resourceIndex == m_request.resources.size();

  QString error;
  Calendar calendar = m_database->calendar(m_request.calendar.id, &error);
  if (calendar.id.isEmpty() || !error.isEmpty()) {
    if (error.isEmpty()) {
      error = QStringLiteral("The calendar was removed during sync");
    }
    auto completed = std::move(m_completed);
    completed(false, error);
    return;
  }
  if (finalChunk) {
    calendar.syncToken = m_request.calendar.syncToken;
    calendar.lastSyncAt = m_request.calendar.lastSyncAt;
    calendar.etag = m_request.calendar.etag;
    for (const QString& key :
         {QStringLiteral("syncedCtag"), QStringLiteral("thisAndFuture"),
          QStringLiteral("thisAndFutureProven")}) {
      if (m_request.calendar.capabilities.contains(key)) {
        calendar.capabilities.insert(key, m_request.calendar.capabilities.value(key));
      }
    }
  }
  bool applied = false;
  if (!finalChunk) {
    applied = m_database->applyRemoteSyncBatch(calendar, events, deleted, pruned,
                                               &error, resources, false);
  } else if (m_request.kind == Request::Kind::Range) {
    applied = m_database->applyRemoteRangeSyncBatch(
        calendar, events, deleted, pruned, m_request.coverageStartUtc,
        m_request.coverageEndUtc, &error, m_request.replaceCoverage, resources);
  } else if (m_request.kind == Request::Kind::IcsFeed) {
    applied = m_database->applyIcsFeedReplacement(
        calendar, events, pruned, m_request.etag, m_request.lastModified,
        m_request.successAt, &error);
  } else {
    applied = m_database->applyRemoteSyncBatch(calendar, events, deleted, pruned,
                                               &error, resources);
  }
  if (!applied) {
    auto completed = std::move(m_completed);
    completed(false, error);
    return;
  }
  if (eventCount != 0 || deletedCount != 0 || prunedCount != 0) {
    emit chunkCommitted(calendar.id);
  }
  if (m_cancelled) {
    return;
  }
  if (finalChunk) {
    auto completed = std::move(m_completed);
    completed(true, {});
  } else {
    QTimer::singleShot(1, this, &ChunkedSyncApply::step);
  }
}

}  // namespace omacalendar
