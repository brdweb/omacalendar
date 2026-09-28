#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "app/calendarpdf.h"

using namespace omacalendar;

namespace {

PrintableEvent timed(const QString& title, const QDateTime& start, const int minutes) {
  PrintableEvent event;
  event.title = title;
  event.location = QStringLiteral("Room 4");
  event.description = QStringLiteral("Bring the quarterly numbers.");
  event.calendarName = QStringLiteral("Work");
  event.color = QColor(QStringLiteral("#7aa2f7"));
  event.start = start;
  event.end = start.addSecs(minutes * 60);
  return event;
}

PrintableEvent allDay(const QString& title, const QDate& first,
                      const QDate& exclusiveEnd) {
  PrintableEvent event;
  event.title = title;
  event.allDay = true;
  event.startDate = first;
  event.endDate = exclusiveEnd;
  return event;
}

// QPdfWriter emits one page object per page.
int pageObjects(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return -1;
  }
  const QByteArray data = file.readAll();
  if (!data.startsWith("%PDF-")) {
    return -1;
  }
  return static_cast<int>(QString::fromLatin1(data).count(
      QRegularExpression(QStringLiteral("/Type\\s*/Page\\b"))));
}

}  // namespace

class CalendarPdfTest : public QObject {
  Q_OBJECT

 private slots:
  void eventsFallOnTheRightDays() {
    const PrintableEvent trip =
        allDay(QStringLiteral("Trip"), QDate(2026, 10, 3), QDate(2026, 10, 6));
    QVERIFY(!trip.occursOn(QDate(2026, 10, 2)));
    QVERIFY(trip.occursOn(QDate(2026, 10, 3)));
    QVERIFY(trip.occursOn(QDate(2026, 10, 5)));
    QVERIFY(!trip.occursOn(QDate(2026, 10, 6)));

    const PrintableEvent overnight = timed(
        QStringLiteral("Overnight"), QDateTime(QDate(2026, 10, 3), QTime(22, 0)), 240);
    QVERIFY(overnight.occursOn(QDate(2026, 10, 3)));
    QVERIFY(overnight.occursOn(QDate(2026, 10, 4)));
    const PrintableEvent lateShift =
        timed(QStringLiteral("Late"), QDateTime(QDate(2026, 10, 3), QTime(22, 0)), 120);
    // It ends exactly at midnight, so it does not reach the next day.
    QVERIFY(!lateShift.occursOn(QDate(2026, 10, 4)));
  }

  void listLayoutPaginates() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    PrintOptions options;
    options.firstDate = QDate(2026, 9, 28);
    options.lastDate = QDate(2026, 10, 4);
    options.includeDetails = true;
    options.title = QStringLiteral("Work");

    const QList<PrintableEvent> few{
        timed(QStringLiteral("Design review"),
              QDateTime(QDate(2026, 9, 28), QTime(9, 0)), 60),
        allDay(QStringLiteral("Offsite"), QDate(2026, 9, 30), QDate(2026, 10, 2)),
        timed(QStringLiteral("Outside the range"),
              QDateTime(QDate(2026, 10, 9), QTime(9, 0)), 60),
    };
    const QString small = directory.filePath(QStringLiteral("small.pdf"));
    QString error;
    const int smallPages = writeCalendarPdf(small, few, options, &error);
    QVERIFY2(smallPages == 1, qPrintable(error));
    QCOMPARE(pageObjects(small), 1);

    QList<PrintableEvent> many;
    for (int index = 0; index < 150; ++index) {
      many.append(timed(QStringLiteral("Slot %1").arg(index),
                        QDateTime(QDate(2026, 9, 29), QTime(8, 0)).addSecs(index * 300),
                        30));
    }
    const QString large = directory.filePath(QStringLiteral("large.pdf"));
    const int largePages = writeCalendarPdf(large, many, options, &error);
    QVERIFY2(largePages > 1, qPrintable(error));
    QCOMPARE(pageObjects(large), largePages);

    const QString empty = directory.filePath(QStringLiteral("empty.pdf"));
    QCOMPARE(writeCalendarPdf(empty, {}, options, &error), 1);
  }

  void monthLayoutPrintsOnePagePerMonth() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    PrintOptions options;
    options.layout = PrintLayout::Month;
    options.firstDate = QDate(2026, 9, 1);
    options.lastDate = QDate(2026, 10, 31);
    options.firstDayOfWeek = 7;
    QList<PrintableEvent> events;
    // More events than a cell can hold, so the "+N more" line is drawn.
    for (int index = 0; index < 12; ++index) {
      events.append(
          timed(QStringLiteral("Busy %1").arg(index),
                QDateTime(QDate(2026, 9, 15), QTime(8, 0)).addSecs(index * 1800), 30));
    }
    events.append(
        allDay(QStringLiteral("Holiday"), QDate(2026, 10, 12), QDate(2026, 10, 13)));
    const QString path = directory.filePath(QStringLiteral("months.pdf"));
    QString error;
    QCOMPARE(writeCalendarPdf(path, events, options, &error), 2);
    QCOMPARE(pageObjects(path), 2);
  }

  void replacesAnExistingFileOnlyOnSuccess() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("existing.pdf"));
    QFile existing(path);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    existing.write("previous export");
    existing.close();

    PrintOptions options;
    options.firstDate = QDate(2026, 10, 2);
    options.lastDate = QDate(2026, 10, 1);
    QString error;
    QCOMPARE(writeCalendarPdf(path, {}, options, &error), 0);
    QFile unchanged(path);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), QByteArray("previous export"));
    unchanged.close();

    options.lastDate = QDate(2026, 10, 3);
    QCOMPARE(writeCalendarPdf(path, {}, options, &error), 1);
    QCOMPARE(pageObjects(path), 1);
    QVERIFY(!QFile::exists(path + QStringLiteral(".part")));

    // A destination that cannot be created fails without writing anything.
    QCOMPARE(writeCalendarPdf(directory.filePath(QStringLiteral("missing/out.pdf")), {},
                              options, &error),
             0);
    QVERIFY(!error.isEmpty());
  }

  void rejectsInvalidRanges() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    PrintOptions options;
    options.firstDate = QDate(2026, 10, 2);
    options.lastDate = QDate(2026, 10, 1);
    QString error;
    QCOMPARE(writeCalendarPdf(directory.filePath(QStringLiteral("bad.pdf")), {},
                              options, &error),
             0);
    QVERIFY(!error.isEmpty());
    options.firstDate = QDate(2025, 1, 1);
    options.lastDate = QDate(2026, 6, 1);
    QCOMPARE(writeCalendarPdf(directory.filePath(QStringLiteral("long.pdf")), {},
                              options, &error),
             0);
  }
};

QTEST_MAIN(CalendarPdfTest)
#include "test_calendarpdf.moc"
