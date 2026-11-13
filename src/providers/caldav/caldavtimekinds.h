#pragma once

#include <QByteArray>
#include <QString>
#include <functional>

#include "core/database.h"
#include "core/domain.h"

namespace omacalendar::caldav {

// Resolves the time kind a cached event had in its retained iCalendar source.
using CalDavTimeKindLookup = std::function<bool(const Event&, TimeKind*, QString*)>;
// Builds a lookup for one retained iCalendar resource payload.
using CalDavTimeKindLookupBuilder =
    std::function<bool(const QByteArray&, CalDavTimeKindLookup*, QString*)>;

// Reclassifies legacy CalDAV time metadata from retained provider source,
// without replacing local edits. Rows, active mutation/conflict snapshots,
// derived instances and the per-profile version checkpoint commit together.
// Returns true at once when the account's checkpoint is already current.
bool refreshCalDavTimeKinds(Database& database, const QString& accountId,
                            const CalDavTimeKindLookupBuilder& buildLookup,
                            QString* errorMessage = nullptr);

}  // namespace omacalendar::caldav
