#include "providers/caldav/caldavtimekinds.h"

#include <QCryptographicHash>
#include <QHash>
#include <QList>
#include <utility>

namespace omacalendar::caldav {

namespace {

QString timeKindCheckpointKey() { return QStringLiteral("caldav_time_kind_version"); }
constexpr int kTimeKindCheckpointVersion = 1;

// Identifies the retained resource an event came from: detached occurrences
// share their series resource, whose href is the remote id before any '#'.
QString retainedResourceIdentity(const Event& event) {
  const qsizetype fragment = event.remoteId.indexOf(QLatin1Char('#'));
  return event.calendarId + QLatin1Char('\n') +
         (fragment < 0 ? event.remoteId : event.remoteId.left(fragment));
}

// Memoizes one lookup per distinct retained payload and per retained resource,
// so a long series is parsed and hydrated once rather than once per row.
class TimeKindLookupCache final {
 public:
  TimeKindLookupCache(const Database& database,
                      const CalDavTimeKindLookupBuilder& buildLookup,
                      QString* errorMessage)
      : m_database(database),
        m_buildLookup(buildLookup),
        m_errorMessage(errorMessage) {}

  bool lookupFor(const Event& value, CalDavTimeKindLookup* lookup) {
    if (!value.rawPayload.isEmpty()) {
      return lookupForPayload(value.rawPayload.toUtf8(), lookup);
    }
    const QString resourceIdentity = retainedResourceIdentity(value);
    const auto cached = m_resourceLookups.constFind(resourceIdentity);
    if (cached != m_resourceLookups.constEnd()) {
      *lookup = *cached;
      return true;
    }
    Event hydrated = value;
    if (!m_database.hydrateProviderResource(&hydrated, m_errorMessage)) {
      return false;
    }
    CalDavTimeKindLookup built;
    if (!lookupForPayload(hydrated.rawPayload.toUtf8(), &built)) {
      return false;
    }
    m_resourceLookups.insert(resourceIdentity, built);
    *lookup = std::move(built);
    return true;
  }

 private:
  struct CachedLookup final {
    QByteArray payload;
    CalDavTimeKindLookup lookup;
  };

  bool lookupForPayload(const QByteArray& payload, CalDavTimeKindLookup* lookup) {
    const QByteArray digest =
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
    const auto cached = m_payloadLookups.constFind(digest);
    if (cached != m_payloadLookups.constEnd()) {
      for (const CachedLookup& candidate : *cached) {
        if (candidate.payload == payload) {
          *lookup = candidate.lookup;
          return true;
        }
      }
    }
    CalDavTimeKindLookup built;
    if (!m_buildLookup(payload, &built, m_errorMessage)) {
      return false;
    }
    if (!built) {
      if (m_errorMessage != nullptr) {
        *m_errorMessage =
            QStringLiteral("CalDAV time metadata lookup builder returned no lookup");
      }
      return false;
    }
    m_payloadLookups[digest].append({payload, built});
    *lookup = std::move(built);
    return true;
  }

  const Database& m_database;
  const CalDavTimeKindLookupBuilder& m_buildLookup;
  QString* m_errorMessage = nullptr;
  QHash<QByteArray, QList<CachedLookup>> m_payloadLookups;
  QHash<QString, CalDavTimeKindLookup> m_resourceLookups;
};

}  // namespace

bool refreshCalDavTimeKinds(Database& database, const QString& accountId,
                            const CalDavTimeKindLookupBuilder& buildLookup,
                            QString* errorMessage) {
  QString error;
  if (database.providerState(accountId, {}, timeKindCheckpointKey(), 0, &error)
          .toInt() >= kTimeKindCheckpointVersion) {
    return true;
  }
  if (!error.isEmpty() ||
      database.account(accountId, &error).provider != ProviderKind::CalDav) {
    if (errorMessage != nullptr) {
      *errorMessage =
          error.isEmpty() ? QStringLiteral("CalDAV account required") : error;
    }
    return false;
  }
  TimeKindLookupCache lookups(database, buildLookup, errorMessage);
  const auto classify = [&lookups](Event* value, QString* classifyError) {
    // A nonempty timezone may be an intentional local conversion. New local
    // events without provider source are also left as explicitly authored.
    if (value->timeKind != TimeKind::Zoned || !value->startTimeZone.isEmpty() ||
        !value->endTimeZone.isEmpty()) {
      return true;
    }
    if (value->allDay) {
      value->timeKind = TimeKind::AllDay;
      return true;
    }
    if (value->rawPayload.isEmpty() && value->remoteId.isEmpty()) {
      return true;
    }
    CalDavTimeKindLookup lookup;
    return lookups.lookupFor(*value, &lookup) &&
           lookup(*value, &value->timeKind, classifyError);
  };
  return database.reclassifyEventTimeKinds(accountId, classify, timeKindCheckpointKey(),
                                           kTimeKindCheckpointVersion, errorMessage);
}

}  // namespace omacalendar::caldav
