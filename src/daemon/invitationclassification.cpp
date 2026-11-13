#include "invitationclassification.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QTime>
#include <QTimeZone>
#include <algorithm>

namespace omacalendar {
namespace {

QString normalizedInvitationResponse(QString response) {
  response = response.trimmed().toLower();
  response.remove(QLatin1Char('-'));
  response.remove(QLatin1Char('_'));
  return response;
}

}  // namespace

bool invitationNeedsResponse(const Event& event, const QString& accountPrincipal) {
  // The organizer has nothing to answer, even when listed as an attendee
  // without a participation state. Google and CalDAV sync mark the account's
  // own organizer entry with self; the principal covers anything else.
  if (event.organizer.value(QStringLiteral("self")).toBool()) {
    return false;
  }
  if (!accountPrincipal.isEmpty()) {
    QString organizer = event.organizer.value(QStringLiteral("email")).toString();
    if (organizer.isEmpty()) {
      organizer = event.organizer.value(QStringLiteral("uri")).toString().trimmed();
      if (organizer.startsWith(QStringLiteral("mailto:"), Qt::CaseInsensitive)) {
        organizer.remove(0, 7);
      }
    }
    if (organizer.compare(accountPrincipal, Qt::CaseInsensitive) == 0) {
      return false;
    }
  }
  for (const QJsonValue& value : event.attendees) {
    const QJsonObject attendee = value.toObject();
    const QString email = attendee.value(QStringLiteral("email")).toString();
    if (!attendee.value(QStringLiteral("self")).toBool() &&
        (accountPrincipal.isEmpty() ||
         email.compare(accountPrincipal, Qt::CaseInsensitive) != 0)) {
      continue;
    }
    QString response = attendee.value(QStringLiteral("responseStatus")).toString();
    if (response.isEmpty()) {
      response = attendee.value(QStringLiteral("partstat")).toString();
    }
    const QString normalized = normalizedInvitationResponse(response);
    return normalized.isEmpty() || normalized == QStringLiteral("needsaction");
  }
  return false;
}

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
