#pragma once

#include <QDateTime>
#include <QList>

#include "core/domain.h"

namespace omacalendar {

struct InvitationBucketTotals {
  int upcoming = 0;
  int past = 0;
};

[[nodiscard]] QDateTime invitationStartBoundary(const Event& event);
[[nodiscard]] QDateTime invitationEndBoundary(const Event& event);
[[nodiscard]] bool invitationIsUpcoming(const Event& event, const QDateTime& now);
void sortInvitations(QList<Event>* events, const QDateTime& now);
[[nodiscard]] InvitationBucketTotals invitationBucketTotals(const QList<Event>& events,
                                                            const QDateTime& now);
[[nodiscard]] QDateTime invitationCacheExpiry(const QList<Event>& events,
                                              const QDateTime& now,
                                              int maximumAgeSeconds = 60);

}  // namespace omacalendar
