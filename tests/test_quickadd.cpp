#include <QtTest/QtTest>

#include "core/quickadd.h"

using namespace omacalendar;

class QuickAddTest final : public QObject {
  Q_OBJECT

 private slots:
  void parsesEverydayPhrases_data();
  void parsesEverydayPhrases();
};

void QuickAddTest::parsesEverydayPhrases_data() {
  QTest::addColumn<QString>("text");
  QTest::addColumn<QString>("title");
  QTest::addColumn<QDate>("date");
  QTest::addColumn<int>("startMinute");
  QTest::addColumn<int>("duration");
  QTest::addColumn<bool>("allDay");
  QTest::addColumn<QDate>("endDate");
  QTest::addColumn<QString>("rule");
  QTest::addColumn<QString>("location");

  // Relative to Monday 2026-09-28.
  QTest::newRow("weekday and time")
      << "Lunch with Sam fri 12:30" << "Lunch with Sam" << QDate(2026, 10, 2) << 750
      << 0 << false << QDate() << QString() << QString();
  QTest::newRow("repeat with duration")
      << "Standup every weekday 9am 15m" << "Standup" << QDate() << 540 << 15 << false
      << QDate() << "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR" << QString();
  QTest::newRow("all-day span")
      << "Trip to Denver Oct 3-6" << "Trip to Denver" << QDate(2026, 10, 3) << -1 << 0
      << true << QDate(2026, 10, 7) << QString() << QString();
  QTest::newRow("location") << "Dentist tomorrow 3pm @ Main St Clinic" << "Dentist"
                            << QDate(2026, 9, 29) << 900 << 0 << false << QDate()
                            << QString() << "Main St Clinic";
  QTest::newRow("time range") << "Review 2-3:30pm" << "Review" << QDate() << 840 << 90
                              << false << QDate() << QString() << QString();
  QTest::newRow("range across noon")
      << "Call mom 11-1pm" << "Call mom" << QDate() << 660 << 120 << false << QDate()
      << QString() << QString();
  QTest::newRow("past month day means next year")
      << "Party Sep 1" << "Party" << QDate(2027, 9, 1) << -1 << 0 << true
      << QDate(2027, 9, 2) << QString() << QString();
  QTest::newRow("repeating weekday anchors the date")
      << "Gym every monday 7am" << "Gym" << QDate(2026, 9, 28) << 420 << 0 << false
      << QDate() << "FREQ=WEEKLY;BYDAY=MO" << QString();
  QTest::newRow("next weekday and duration")
      << "Retro next monday at noon for 45 min" << "Retro" << QDate(2026, 10, 5) << 720
      << 45 << false << QDate() << QString() << QString();
  QTest::newRow("title only") << "Plain title" << "Plain title" << QDate() << -1 << 0
                              << false << QDate() << QString() << QString();
  QTest::newRow("iso date and 24-hour time")
      << "Launch 2026-12-01 10:00" << "Launch" << QDate(2026, 12, 1) << 600 << 0
      << false << QDate() << QString() << QString();
  QTest::newRow("day before month")
      << "Holiday 24 Dec" << "Holiday" << QDate(2026, 12, 24) << -1 << 0 << true
      << QDate(2026, 12, 25) << QString() << QString();
  QTest::newRow("ambiguous abbreviation stays in the title")
      << "Sun protection talk" << "Sun protection talk" << QDate() << -1 << 0 << false
      << QDate() << QString() << QString();
  QTest::newRow("ambiguous abbreviation after on")
      << "Brunch on sun 11am" << "Brunch" << QDate(2026, 10, 4) << 660 << 0 << false
      << QDate() << QString() << QString();
  QTest::newRow("full weekday name")
      << "Market saturday" << "Market" << QDate(2026, 10, 3) << -1 << 0 << true
      << QDate(2026, 10, 4) << QString() << QString();
  QTest::newRow("numbers that are not times stay in the title")
      << "Read chapter 3" << "Read chapter 3" << QDate() << -1 << 0 << false << QDate()
      << QString() << QString();
  QTest::newRow("words that start like months stay in the title")
      << "Mark 5 papers" << "Mark 5 papers" << QDate() << -1 << 0 << false << QDate()
      << QString() << QString();
  QTest::newRow("tonight") << "Dinner tonight" << "Dinner" << QDate(2026, 9, 28)
                           << 19 * 60 << 0 << false << QDate() << QString()
                           << QString();
}

void QuickAddTest::parsesEverydayPhrases() {
  QFETCH(QString, text);
  QFETCH(QString, title);
  QFETCH(QDate, date);
  QFETCH(int, startMinute);
  QFETCH(int, duration);
  QFETCH(bool, allDay);
  QFETCH(QDate, endDate);
  QFETCH(QString, rule);
  QFETCH(QString, location);

  const QuickAddDraft draft = parseQuickAdd(text, QDate(2026, 9, 28));
  QCOMPARE(draft.title, title);
  QCOMPARE(draft.date, date);
  QCOMPARE(draft.startMinute, startMinute);
  QCOMPARE(draft.durationMinutes, duration);
  QCOMPARE(draft.allDay, allDay);
  QCOMPARE(draft.endDate, endDate);
  QCOMPARE(draft.recurrenceRule, rule);
  QCOMPARE(draft.location, location);
}

QTEST_GUILESS_MAIN(QuickAddTest)
#include "test_quickadd.moc"
