#include "invitationclassification.h"

#include <QTime>
#include <QTimeZone>
#include <algorithm>

namespace omacalendar {

QDateTime invitationStartBoundary(const Event& event) {
  return event.allDay ? QDateTime(event.startDate, QTime(0, 0), QTimeZone::LocalTime)
                      : event.startUtc;
}

QDateTime invitationEndBoundary(const Event& event) {
  return event.allDay ? QDateTime(event.endDate, QTime(0, 0), QTimeZone::LocalTime)
                      : event.endUtc;
}

bool invitationIsUpcoming(const Event& event, const QDateTime& now) {
  return invitationEndBoundary(event) > now;
}

void sortInvitations(QList<Event>* events, const QDateTime& now) {
  std::stable_sort(
      events->begin(), events->end(), [&now](const Event& left, const Event& right) {
        const bool leftUpcoming = invitationIsUpcoming(left, now);
        const bool rightUpcoming = invitationIsUpcoming(right, now);
        if (leftUpcoming != rightUpcoming) {
          return leftUpcoming;
        }
        const QDateTime leftStart = invitationStartBoundary(left);
        const QDateTime rightStart = invitationStartBoundary(right);
        if (leftUpcoming && leftStart != rightStart) {
          return leftStart < rightStart;
        }
        if (!leftUpcoming && left.updatedAt != right.updatedAt) {
          return left.updatedAt > right.updatedAt;
        }
        if (leftStart != rightStart) {
          return leftUpcoming ? leftStart < rightStart : leftStart > rightStart;
        }
        if (left.id != right.id) {
          return left.id < right.id;
        }
        return left.recurrenceId < right.recurrenceId;
      });
}

InvitationBucketTotals invitationBucketTotals(const QList<Event>& events,
                                              const QDateTime& now) {
  InvitationBucketTotals totals;
  for (const Event& event : events) {
    if (invitationIsUpcoming(event, now)) {
      ++totals.upcoming;
    } else {
      ++totals.past;
    }
  }
  return totals;
}

QDateTime invitationCacheExpiry(const QList<Event>& events, const QDateTime& now,
                                const int maximumAgeSeconds) {
  QDateTime expiry = now.addSecs(maximumAgeSeconds);
  for (const Event& event : events) {
    const QDateTime end = invitationEndBoundary(event);
    if (end > now && end < expiry) {
      expiry = end;
    }
  }
  return expiry;
}

}  // namespace omacalendar
