#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QtTest/QtTest>
#include <ctime>

#include "daemon/invitationclassification.h"

using namespace omacalendar;

namespace {

QDateTime utc(const int year, const int month, const int day, const int hour,
              const int minute = 0, const int second = 0) {
  return QDateTime(QDate(year, month, day), QTime(hour, minute, second),
                   QTimeZone::UTC);
}

Event allDayInvitation(const QString& id) {
  Event event;
  event.id = id;
  event.allDay = true;
  event.timeKind = TimeKind::AllDay;
  event.startDate = QDate(2027, 5, 5);
  event.endDate = QDate(2027, 5, 6);
  event.updatedAt = utc(2027, 5, 5, 6, 30);
  return event;
}

Event timedInvitation(const QString& id, const QDateTime& start, const QDateTime& end,
                      const QDateTime& updatedAt) {
  Event event;
  event.id = id;
  event.startUtc = start;
  event.endUtc = end;
  event.updatedAt = updatedAt;
  return event;
}

const QString kPrincipal = QStringLiteral("me@example.invalid");

QJsonObject attendee(const QString& email, const QString& response,
                     const bool self = false) {
  QJsonObject result{{QStringLiteral("email"), email}};
  if (!response.isEmpty()) {
    result.insert(QStringLiteral("responseStatus"), response);
  }
  if (self) {
    result.insert(QStringLiteral("self"), true);
  }
  return result;
}

Event meeting(const QJsonArray& attendees,
              const QString& organizer = QStringLiteral("boss@example.invalid")) {
  Event event = timedInvitation(QStringLiteral("meeting"), utc(2027, 5, 6, 16),
                                utc(2027, 5, 6, 17), utc(2027, 5, 5, 9));
  event.uid = QStringLiteral("meeting@example.invalid");
  event.summary = QStringLiteral("Quarterly review");
  event.organizer = QJsonObject{{QStringLiteral("email"), organizer}};
  event.attendees = attendees;
  return event;
}

}  // namespace

class DaemonInvitationTest final : public QObject {
  Q_OBJECT

 private slots:
  void allDayClassificationChangesAtLocalExclusiveEnd() {
    const Event event = allDayInvitation(QStringLiteral("all-day"));
    const QDateTime beforeBoundary = utc(2027, 5, 6, 6, 59, 30);
    const QDateTime boundary = utc(2027, 5, 6, 7);

    QCOMPARE(invitationStartBoundary(event), utc(2027, 5, 5, 7));
    QCOMPARE(invitationEndBoundary(event), boundary);
    QVERIFY(invitationIsUpcoming(event, beforeBoundary));
    QVERIFY(!invitationIsUpcoming(event, boundary));
  }

  void sortingTotalsAndExpiryUseTheSameLocalBoundary() {
    const Event allDay = allDayInvitation(QStringLiteral("all-day"));
    const Event timedUpcoming =
        timedInvitation(QStringLiteral("timed-upcoming"), utc(2027, 5, 6, 7, 15),
                        utc(2027, 5, 6, 7, 30), utc(2027, 5, 5, 6));
    const Event timedPast =
        timedInvitation(QStringLiteral("timed-past"), utc(2027, 5, 6, 6, 30),
                        utc(2027, 5, 6, 6, 59), utc(2027, 5, 5, 5));
    const QDateTime beforeBoundary = utc(2027, 5, 6, 6, 59, 30);
    const QDateTime boundary = utc(2027, 5, 6, 7);
    QList<Event> invitations{timedPast, timedUpcoming, allDay};

    sortInvitations(&invitations, beforeBoundary);
    QCOMPARE(invitations.at(0).id, QStringLiteral("all-day"));
    QCOMPARE(invitations.at(1).id, QStringLiteral("timed-upcoming"));
    QCOMPARE(invitations.at(2).id, QStringLiteral("timed-past"));
    InvitationBucketTotals totals = invitationBucketTotals(invitations, beforeBoundary);
    QCOMPARE(totals.upcoming, 2);
    QCOMPARE(totals.past, 1);
    QCOMPARE(invitationCacheExpiry(invitations, beforeBoundary), boundary);

    sortInvitations(&invitations, boundary);
    QCOMPARE(invitations.at(0).id, QStringLiteral("timed-upcoming"));
    QCOMPARE(invitations.at(1).id, QStringLiteral("all-day"));
    QCOMPARE(invitations.at(2).id, QStringLiteral("timed-past"));
    totals = invitationBucketTotals(invitations, boundary);
    QCOMPARE(totals.upcoming, 1);
    QCOMPARE(totals.past, 2);
    QCOMPARE(invitationCacheExpiry(invitations, boundary), boundary.addSecs(60));
  }

  void timedClassificationRemainsUtc() {
    const Event event = timedInvitation(QStringLiteral("timed"), utc(2027, 5, 6, 6, 30),
                                        utc(2027, 5, 6, 6, 59, 45), utc(2027, 5, 5, 5));

    QCOMPARE(invitationStartBoundary(event), utc(2027, 5, 6, 6, 30));
    QCOMPARE(invitationEndBoundary(event), utc(2027, 5, 6, 6, 59, 45));
    QVERIFY(invitationIsUpcoming(event, utc(2027, 5, 6, 6, 59, 44)));
    QVERIFY(!invitationIsUpcoming(event, utc(2027, 5, 6, 6, 59, 45)));
  }

  void attendeeAwaitingAnAnswerNeedsResponse() {
    // Google marks the account's own entry with self.
    Event event = meeting(
        {attendee(QStringLiteral("boss@example.invalid"), QStringLiteral("accepted")),
         attendee(kPrincipal, QStringLiteral("needsAction"), true)});
    QVERIFY(invitationNeedsResponse(event, {}));
    QVERIFY(invitationNeedsResponse(event, kPrincipal));

    // Without the self flag, the account principal is matched by address,
    // ignoring case.
    event.attendees = {
        attendee(QStringLiteral("ME@Example.Invalid"), QStringLiteral("needsAction"))};
    QVERIFY(invitationNeedsResponse(event, kPrincipal));
    QVERIFY(!invitationNeedsResponse(event, {}));

    // iCalendar participation states from CalDAV, in any spelling, and a
    // missing state (RFC 5545 defaults to NEEDS-ACTION) all await an answer.
    for (const QString& partstat :
         {QStringLiteral("NEEDS-ACTION"), QStringLiteral("needs_action"),
          QStringLiteral(" NeedsAction "), QString()}) {
      QJsonObject own = attendee(kPrincipal, {}, true);
      if (!partstat.isEmpty()) {
        own.insert(QStringLiteral("partstat"), partstat);
      }
      event.attendees = {own};
      QVERIFY2(invitationNeedsResponse(event, kPrincipal), qPrintable(partstat));
    }
  }

  void answeredInvitationsDoNotNeedResponse() {
    for (const QString& answer :
         {QStringLiteral("accepted"), QStringLiteral("declined"),
          QStringLiteral("tentative")}) {
      const Event event = meeting({attendee(kPrincipal, answer, true)});
      QVERIFY2(!invitationNeedsResponse(event, kPrincipal), qPrintable(answer));
    }
    // A CalDAV DECLINED state without a Google response.
    QJsonObject declined = attendee(kPrincipal, {}, true);
    declined.insert(QStringLiteral("partstat"), QStringLiteral("DECLINED"));
    QVERIFY(!invitationNeedsResponse(meeting({declined}), kPrincipal));

    // A local RSVP writes responseStatus; it wins over a stale state.
    QJsonObject answered = attendee(kPrincipal, QStringLiteral("declined"), true);
    answered.insert(QStringLiteral("partstat"), QStringLiteral("NEEDS-ACTION"));
    QVERIFY(!invitationNeedsResponse(meeting({answered}), kPrincipal));
  }

  void eventsWithoutTheAccountAsAttendeeAreNotInvitations() {
    // Other people awaiting an answer are not the account's invitation.
    const Event others = meeting(
        {attendee(QStringLiteral("a@example.invalid"), QStringLiteral("needsAction")),
         attendee(QStringLiteral("b@example.invalid"), {})});
    QVERIFY(!invitationNeedsResponse(others, kPrincipal));
    QVERIFY(!invitationNeedsResponse(others, {}));
    QVERIFY(!invitationNeedsResponse(meeting({}), kPrincipal));
  }

  void resourceRoomsAreNotTheAccount() {
    QJsonObject room = attendee(QStringLiteral("room-4@resource.example.invalid"),
                                QStringLiteral("needsAction"));
    room.insert(QStringLiteral("resource"), true);
    room.insert(QStringLiteral("displayName"), QStringLiteral("Room 4"));
    // A room still waiting to confirm is not the account's answer to give.
    QVERIFY(!invitationNeedsResponse(meeting({room}), kPrincipal));
    // Listed first, a room does not hide the account's own answer either way.
    QVERIFY(!invitationNeedsResponse(
        meeting({room, attendee(kPrincipal, QStringLiteral("accepted"), true)}),
        kPrincipal));
    QVERIFY(invitationNeedsResponse(
        meeting({room, attendee(kPrincipal, QStringLiteral("needsAction"), true)}),
        kPrincipal));
  }

  void organizerNeverAnswersTheirOwnEvent() {
    // Google lists the organizer's own entry as accepted.
    Event own = meeting({attendee(kPrincipal, QStringLiteral("accepted"), true),
                         attendee(QStringLiteral("guest@example.invalid"),
                                  QStringLiteral("needsAction"))},
                        kPrincipal);
    own.organizer.insert(QStringLiteral("self"), true);
    QVERIFY(!invitationNeedsResponse(own, kPrincipal));

    // Organizer not in the attendee list.
    own.attendees = {attendee(QStringLiteral("guest@example.invalid"),
                              QStringLiteral("needsAction"))};
    QVERIFY(!invitationNeedsResponse(own, kPrincipal));

    // CalDAV clients may list the organizer as an attendee without PARTSTAT,
    // which RFC 5545 reads as NEEDS-ACTION. The organizer still has nothing
    // to answer.
    own.attendees = {attendee(kPrincipal, {}, true),
                     attendee(QStringLiteral("guest@example.invalid"),
                              QStringLiteral("needsAction"))};
    QEXPECT_FAIL("",
                 "Bug: invitationNeedsResponse() never looks at the organizer, so the "
                 "account's own event appears in the invitation list when its own "
                 "attendee entry has no answer",
                 Continue);
    QVERIFY(!invitationNeedsResponse(own, kPrincipal));
  }

  void fingerprintSeparatesChangedFromUnchanged() {
    const Event original = meeting(
        {attendee(QStringLiteral("boss@example.invalid"), QStringLiteral("accepted")),
         attendee(kPrincipal, QStringLiteral("needsAction"), true)});
    const QString baseline = invitationFingerprint(original);
    QVERIFY(baseline.startsWith(QStringLiteral("invitation:")));
    QCOMPARE(invitationFingerprint(original), baseline);

    // Not a change: sync metadata, attendee order, and the account's own RSVP.
    Event same = original;
    same.etag = QStringLiteral("\"etag-2\"");
    same.updatedAt = original.updatedAt.addSecs(3600);
    same.localRevision = original.localRevision + 3;
    same.attendees = {original.attendees.at(1), original.attendees.at(0)};
    QCOMPARE(invitationFingerprint(same), baseline);
    QJsonObject own = same.attendees.at(0).toObject();
    own.insert(QStringLiteral("responseStatus"), QStringLiteral("accepted"));
    own.insert(QStringLiteral("partstat"), QStringLiteral("ACCEPTED"));
    same.attendees.replace(0, own);
    QCOMPARE(invitationFingerprint(same), baseline);

    // A change: anything the organizer sends again.
    const auto differs = [&baseline](const Event& event) {
      return invitationFingerprint(event) != baseline;
    };
    Event moved = original;
    moved.startUtc = moved.startUtc.addSecs(1800);
    moved.endUtc = moved.endUtc.addSecs(1800);
    QVERIFY(differs(moved));
    Event renamed = original;
    renamed.summary = QStringLiteral("Quarterly review (agenda)");
    QVERIFY(differs(renamed));
    Event relocated = original;
    relocated.location = QStringLiteral("Room 9");
    QVERIFY(differs(relocated));
    Event resent = original;
    resent.sequence = 1;
    QVERIFY(differs(resent));
    Event cancelled = original;
    cancelled.status = QStringLiteral("cancelled");
    cancelled.deleted = true;
    QVERIFY(differs(cancelled));
    Event morePeople = original;
    morePeople.attendees.append(
        attendee(QStringLiteral("new@example.invalid"), QStringLiteral("needsAction")));
    QVERIFY(differs(morePeople));
    // A different event with identical content is a different invitation.
    Event other = original;
    other.id = QStringLiteral("other-meeting");
    QVERIFY(differs(other));
  }
};

int main(int argc, char** argv) {
  qputenv("TZ", "America/Los_Angeles");
  ::tzset();
  QCoreApplication application(argc, argv);
  DaemonInvitationTest test;
  return QTest::qExec(&test, argc, argv);
}

#include "test_daemon_invitations.moc"
