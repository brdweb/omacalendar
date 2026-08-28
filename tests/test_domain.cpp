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
    QCOMPARE(outboxOperationFromString(QStringLiteral("remove")),
             OutboxOperation::Remove);
    QCOMPARE(outboxOperationFromString(QStringLiteral("other")),
             OutboxOperation::Create);

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
    QCOMPARE(outAccount.endpoint, inAccount.endpoint);
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
    QCOMPARE(outCalendar.etag, inCalendar.etag);
    QCOMPARE(outCalendar.syncToken, inCalendar.syncToken);
    QCOMPARE(outCalendar.capabilities, inCalendar.capabilities);
    QCOMPARE(outCalendar.lastSyncAt, inCalendar.lastSyncAt);

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

    const Event outEvent = eventFromJson(toJson(inEvent));
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
    QCOMPARE(outEvent.dirty, inEvent.dirty);
    QCOMPARE(outEvent.deleted, inEvent.deleted);
    QCOMPARE(outEvent.createdAt, inEvent.createdAt);
  }
};

QTEST_MAIN(DomainTest)
#include "test_domain.moc"
