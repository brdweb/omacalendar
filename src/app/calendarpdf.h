#pragma once

#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QList>
#include <QString>

namespace omacalendar {

// One event as it should appear on paper. Times are wall times in the zone
// the calendar is displayed in.
struct PrintableEvent {
  QString title;
  QString location;
  QString description;
  QString calendarName;
  QColor color;
  bool allDay = false;
  // All-day events: the first day and the exclusive end day.
  QDate startDate;
  QDate endDate;
  // Timed events.
  QDateTime start;
  QDateTime end;

  // Whether any part of the event falls on this day.
  [[nodiscard]] bool occursOn(const QDate& day) const;
};

enum class PrintLayout {
  // Day by day, one line per event, portrait pages.
  List,
  // One landscape month grid per month in the range.
  Month,
};

struct PrintOptions {
  QDate firstDate;
  QDate lastDate;
  PrintLayout layout = PrintLayout::List;
  // Adds location, calendar and notes under each event in the list layout.
  bool includeDetails = false;
  QString title;
  // QTime format for event times, for example "HH:mm" or "h:mm AP".
  QString timeFormat = QStringLiteral("HH:mm");
  // 1 (Monday) to 7 (Sunday).
  int firstDayOfWeek = 1;
};

// Writes the events that fall between firstDate and lastDate (inclusive) as a
// print-ready PDF in a fixed light theme, whatever the app's theme is.
// Returns the number of pages written, or 0 with errorMessage set.
int writeCalendarPdf(const QString& path, const QList<PrintableEvent>& events,
                     const PrintOptions& options, QString* errorMessage = nullptr);

}  // namespace omacalendar
