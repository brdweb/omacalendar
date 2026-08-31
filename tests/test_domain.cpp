// Copyright (c) 2026

#include <QtTest/QtTest>

#include "core/domain.h"

using namespace omacalendar;

class DomainTest final : public QObject {
  Q_OBJECT

 private slots:
  void providerKindConversions() {
    QCOMPARE(providerKindFromString(QStringLiteral("google")), ProviderKind::Google);
    QCOMPARE(providerKindFromString(QStringLiteral("caldav")), ProviderKind::CalDav);
    QCOMPARE(providerKindFromString(QStringLiteral("unknown")), ProviderKind::Unknown);
    QCOMPARE(providerKindToString(ProviderKind::Google), QStringLiteral("google"));
    QCOMPARE(providerKindToString(ProviderKind::CalDav), QStringLiteral("caldav"));
    QCOMPARE(providerKindToString(ProviderKind::Unknown), QStringLiteral("unknown"));
  }

  void outboxConversions() {
    QCOMPARE(outboxOperationFromString(QStringLiteral("create")),
             OutboxOperation::Create);
    QCOMPARE(outboxOperationFromString(QStringLiteral("update")),
             OutboxOperation::Update);
    QCOMPARE(outboxOperationFromString(QStringLiteral("move")), OutboxOperation::Move);
    QCOMPARE(outboxOperationFromString(QStringLiteral("remove")),
             OutboxOperation::Remove);
    QCOMPARE(outboxOperationFromString(QStringLiteral("other")),
             OutboxOperation::Create);
    QCOMPARE(outboxOperationToString(OutboxOperation::Move), QStringLiteral("move"));

    QCOMPARE(outboxStateFromString(QStringLiteral("pending")), OutboxState::Pending);
    QCOMPARE(outboxStateFromString(QStringLiteral("sending")), OutboxState::Sending);
    QCOMPARE(outboxStateFromString(QStringLiteral("retry_wait")),
             OutboxState::RetryWait);
    QCOMPARE(outboxStateFromString(QStringLiteral("blocked")), OutboxState::Blocked);
    QCOMPARE(outboxStateFromString(QStringLiteral("done")), OutboxState::Done);
    QCOMPARE(outboxStateFromString(QStringLiteral("other")), OutboxState::Pending);
  }

  void isoTimestampRoundTrip() {
    const QDateTime valid =
        QDateTime(QDate(2026, 8, 28), QTime(12, 34, 56, 987), QTimeZone::utc());
    const QString encoded = isoUtc(valid);
    QCOMPARE(valid, dateTimeFromIso(encoded));
    QCOMPARE(isoUtc(QDateTime()), QString{});
    QCOMPARE(dateTimeFromIso(QString{}), QDateTime());
  }

  void recurrenceIdentityCanonicalization() {
    const QString zoned = QStringLiteral("TZID=America/New_York:20260904T090000");
    const QString utc = QStringLiteral("2026-09-04T13:00:00.000Z");
    QVERIFY(recurrenceIdentityEqual(zoned, utc, false, TimeKind::Zoned,
                                    QStringLiteral("America/New_York")));
    QVERIFY(recurrenceIdentityEqual(
        QStringLiteral("RANGE=THISANDFUTURE;TZID=America/New_York:20260904T090000"),
        zoned, false, TimeKind::Zoned, QStringLiteral("America/New_York")));
    QVERIFY(recurrenceIdentityEqual(
        QStringLiteral("20260904T090000"), QStringLiteral("2026-09-04T09:00:00"), false,
        TimeKind::Zoned, QStringLiteral("America/New_York")));
    QVERIFY(!recurrenceIdentityEqual(
        QStringLiteral("TZID=America/Los_Angeles:20260904T090000"), utc, false,
        TimeKind::Zoned, QStringLiteral("America/New_York")));

    QVERIFY(recurrenceIdentityEqual(QStringLiteral("20260904"),
                                    QStringLiteral("2026-09-04"), true,
                                    TimeKind::AllDay));
    QVERIFY(recurrenceIdentityEqual(QStringLiteral("VALUE=DATE:20260904"),
                                    QStringLiteral("2026-09-04"), true,
                                    TimeKind::AllDay));
    QVERIFY(recurrenceIdentityEqual(
        QStringLiteral("VALUE=DATE;TZID=America/New_York:20260904"),
        QStringLiteral("2026-09-04"), true, TimeKind::AllDay));
    QVERIFY(!recurrenceIdentityEqual(QStringLiteral("20260904"),
                                     QStringLiteral("20260905"), true,
                                     TimeKind::AllDay));

    QVERIFY(recurrenceIdentityEqual(QStringLiteral("20260904T090000"),
                                    QStringLiteral("2026-09-04T09:00:00.000"), false,
                                    TimeKind::Floating));
    QVERIFY(!recurrenceIdentityEqual(QStringLiteral("20260904T090000"),
                                     QStringLiteral("2026-09-04T09:00:00.000Z"), false,
                                     TimeKind::Floating));

    Event left;
    Event right;
    left.recurrenceId = QStringLiteral("20260904");
    right.recurrenceId = QStringLiteral("2026-09-04");
    left.allDay = right.allDay = true;
    left.timeKind = right.timeKind = TimeKind::AllDay;
    QVERIFY(recurrenceIdentityEqual(left, right));
    right.allDay = false;
    QVERIFY(!recurrenceIdentityEqual(left, right));
  }

  void jsonRoundTrip() {
    Account inAccount;
    inAccount.id = "acc-1";
    inAccount.provider = ProviderKind::Google;
    inAccount.displayName = "Test";
    inAccount.principal = "user@example.com";
    inAccount.endpoint = "https://example.test/";
    inAccount.enabled = false;
    inAccount.authStatus = "authed";
    inAccount.createdAt = QDateTime::currentDateTimeUtc();
    inAccount.updatedAt = inAccount.createdAt;

    const Account outAccount = accountFromJson(toJson(inAccount));
    QCOMPARE(outAccount.id, inAccount.id);
    QCOMPARE(outAccount.provider, inAccount.provider);
    QCOMPARE(outAccount.displayName, inAccount.displayName);
    QCOMPARE(outAccount.principal, inAccount.principal);
    // Presentation DTOs must never disclose provider endpoints.
    QVERIFY(!toJson(inAccount).contains(QStringLiteral("endpoint")));
    QVERIFY(outAccount.endpoint.isEmpty());
    QCOMPARE(outAccount.enabled, inAccount.enabled);
    QCOMPARE(outAccount.authStatus, inAccount.authStatus);

    Calendar inCalendar;
    inCalendar.id = "cal-1";
    inCalendar.accountId = "acc-1";
    inCalendar.remoteId = "r-1";
    inCalendar.name = "Default";
    inCalendar.color = "#123456";
    inCalendar.timeZone = "UTC";
    inCalendar.readOnly = true;
    inCalendar.enabled = false;
    inCalendar.etag = "etag";
    inCalendar.syncToken = "sync";
    inCalendar.capabilities = QJsonObject{{"cal", true}};
    inCalendar.lastSyncAt = QDateTime::currentDateTimeUtc();

    const Calendar outCalendar = calendarFromJson(toJson(inCalendar));
    QCOMPARE(outCalendar.id, inCalendar.id);
    QCOMPARE(outCalendar.name, inCalendar.name);
    QCOMPARE(outCalendar.timeZone, inCalendar.timeZone);
    QCOMPARE(outCalendar.readOnly, inCalendar.readOnly);
    QCOMPARE(outCalendar.enabled, inCalendar.enabled);
    // Provider resource identities and validators stay daemon-private.
    const QJsonObject calendarDto = toJson(inCalendar);
    QVERIFY(!calendarDto.contains(QStringLiteral("remoteId")));
    QVERIFY(!calendarDto.contains(QStringLiteral("href")));
    QVERIFY(!calendarDto.contains(QStringLiteral("etag")));
    QVERIFY(!calendarDto.contains(QStringLiteral("syncToken")));
    QVERIFY(outCalendar.etag.isEmpty());
    QVERIFY(outCalendar.syncToken.isEmpty());
    QJsonObject expectedCapabilities = inCalendar.capabilities;
    expectedCapabilities.insert(QStringLiteral("canDeleteCalendar"), false);
    QCOMPARE(outCalendar.capabilities, expectedCapabilities);
    QCOMPARE(outCalendar.lastSyncAt, inCalendar.lastSyncAt);

    Calendar ownedSecondary = inCalendar;
    ownedSecondary.capabilities = {
        {QStringLiteral("provider"), QStringLiteral("google")},
        {QStringLiteral("accessRole"), QStringLiteral("owner")},
        {QStringLiteral("primary"), false},
    };
    QVERIFY(canDeleteCalendar(ownedSecondary));
    QVERIFY(toJson(ownedSecondary)
                .value(QStringLiteral("capabilities"))
                .toObject()
                .value(QStringLiteral("canDeleteCalendar"))
                .toBool());

    Event inEvent;
    inEvent.id = "ev-1";
    inEvent.calendarId = "cal-1";
    inEvent.uid = "uid";
    inEvent.summary = "Summary";
    inEvent.description = "Description";
    inEvent.location = "Office";
    inEvent.startUtc = QDateTime(QDate(2026, 1, 1), QTime(9, 0), QTimeZone::utc());
    inEvent.endUtc = QDateTime(QDate(2026, 1, 1), QTime(10, 0), QTimeZone::utc());
    inEvent.startDate = QDate(2026, 1, 1);
    inEvent.endDate = QDate(2026, 1, 2);
    inEvent.startTimeZone = "UTC";
    inEvent.endTimeZone = "UTC";
    inEvent.status = "confirmed";
    inEvent.transparency = "opaque";
    inEvent.allDay = false;
    inEvent.dirty = true;
    inEvent.deleted = false;
    inEvent.sequence = 7;
    inEvent.organizer = QJsonObject{{"cn", "Organizer"}};
    inEvent.attendees = QJsonArray{QJsonObject{{"name", "A"}}};
    inEvent.reminders = QJsonArray{QJsonObject{{"min", 15}}};
    inEvent.createdAt = QDateTime::currentDateTimeUtc();
    inEvent.updatedAt = inEvent.createdAt;

    inEvent.remoteId = QStringLiteral("provider-event-id");
    inEvent.uid = QStringLiteral("private-uid");
    inEvent.etag = QStringLiteral("private-etag");
    inEvent.rawPayload = QStringLiteral("provider payload");
    inEvent.rawFormat = QStringLiteral("json");

    const QJsonObject eventDto = toJson(inEvent);
    QVERIFY(!eventDto.contains(QStringLiteral("remoteId")));
    QVERIFY(!eventDto.contains(QStringLiteral("uid")));
    QVERIFY(!eventDto.contains(QStringLiteral("etag")));
    QVERIFY(!eventDto.contains(QStringLiteral("rawPayload")));
    const Event outEvent = eventFromJson(toStorageJson(inEvent));
    QCOMPARE(outEvent.id, inEvent.id);
    QCOMPARE(outEvent.calendarId, inEvent.calendarId);
    QCOMPARE(outEvent.summary, inEvent.summary);
    QCOMPARE(outEvent.location, inEvent.location);
    QCOMPARE(outEvent.startDate, inEvent.startDate);
    QCOMPARE(outEvent.endDate, inEvent.endDate);
    QCOMPARE(outEvent.status, inEvent.status);
    QCOMPARE(outEvent.sequence, inEvent.sequence);
    QCOMPARE(outEvent.organizer, inEvent.organizer);
    QCOMPARE(outEvent.attendees, inEvent.attendees);
    QCOMPARE(outEvent.reminders, inEvent.reminders);
    QCOMPARE(outEvent.remoteId, inEvent.remoteId);
    QCOMPARE(outEvent.uid, inEvent.uid);
    QCOMPARE(outEvent.etag, inEvent.etag);
    QCOMPARE(outEvent.rawPayload, inEvent.rawPayload);
    QCOMPARE(outEvent.dirty, inEvent.dirty);
    QCOMPARE(outEvent.deleted, inEvent.deleted);
    QCOMPARE(outEvent.createdAt, inEvent.createdAt);
  }
};

QTEST_MAIN(DomainTest)
#include "test_domain.moc"
