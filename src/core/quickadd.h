#pragma once

#include <QDate>
#include <QString>

namespace omacalendar {

// A draft event read from one line of text such as
// "Lunch with Sam fri 12:30" or "Trip to Denver Oct 3-6". The parser never
// saves anything; the result pre-fills the event editor for confirmation.
struct QuickAddDraft {
  QString title;
  QString location;
  QString recurrenceRule;
  // The first day of the event; invalid when the text named no date.
  QDate date;
  // All-day events end on endDate, exclusive.
  bool allDay = false;
  QDate endDate;
  // Minutes after midnight; negative when the text named no time.
  int startMinute = -1;
  // Zero when the text named neither an end time nor a duration.
  int durationMinutes = 0;
};

// Parses English quick-add text relative to today. Words that do not read as
// a date, time, duration, repeat or "@ location" become the title.
[[nodiscard]] QuickAddDraft parseQuickAdd(const QString& text, const QDate& today);

}  // namespace omacalendar
