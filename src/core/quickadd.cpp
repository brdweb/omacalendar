#include "core/quickadd.h"

#include <QRegularExpression>
#include <QStringList>
#include <array>

namespace omacalendar {
namespace {

constexpr std::array<const char*, 12> kMonths = {
    "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
constexpr std::array<const char*, 7> kWeekdays = {"mon", "tue", "wed", "thu",
                                                  "fri", "sat", "sun"};
constexpr std::array<const char*, 7> kRuleDays = {"MO", "TU", "WE", "TH",
                                                  "FR", "SA", "SU"};

const QString kMonthPattern = QStringLiteral(
    "(jan(?:uary)?|feb(?:ruary)?|mar(?:ch)?|apr(?:il)?|may|june?|july?|"
    "aug(?:ust)?|sep(?:t(?:ember)?)?|oct(?:ober)?|nov(?:ember)?|dec(?:ember)?)\\.?");
const QString kWeekdayPattern = QStringLiteral(
    "(mon(?:day)?|tue(?:s(?:day)?)?|wed(?:nesday)?|thu(?:r(?:s(?:day)?)?)?|"
    "fri(?:day)?|sat(?:urday)?|sun(?:day)?)");

QRegularExpression pattern(const QString& source) {
  return QRegularExpression(source, QRegularExpression::CaseInsensitiveOption);
}

int monthNumber(const QString& name) {
  const QString prefix = name.left(3).toLower();
  for (std::size_t index = 0; index < kMonths.size(); ++index) {
    if (prefix == QLatin1String(kMonths.at(index))) {
      return static_cast<int>(index) + 1;
    }
  }
  return 0;
}

// Monday is 1, as in QDate::dayOfWeek.
int weekdayNumber(const QString& name) {
  const QString prefix = name.left(3).toLower();
  for (std::size_t index = 0; index < kWeekdays.size(); ++index) {
    if (prefix == QLatin1String(kWeekdays.at(index))) {
      return static_cast<int>(index) + 1;
    }
  }
  return 0;
}

// The next date falling on weekday, counting today.
QDate upcoming(const QDate& today, const int weekday) {
  return today.addDays((weekday - today.dayOfWeek() + 7) % 7);
}

// A month and day without a year means the next time it comes round.
QDate nextMonthDay(const QDate& today, const int month, const int day) {
  QDate date(today.year(), month, day);
  if (date.isValid() && date < today) {
    date = QDate(today.year() + 1, month, day);
  }
  return date;
}

// Hour and minute as minutes after midnight, or -1 when out of range.
int clockMinutes(int hour, const int minute, const QString& meridiem) {
  const QString suffix = meridiem.toLower();
  if (minute < 0 || minute > 59) {
    return -1;
  }
  if (!suffix.isEmpty()) {
    if (hour < 1 || hour > 12) {
      return -1;
    }
    hour %= 12;
    if (suffix == QStringLiteral("pm")) {
      hour += 12;
    }
  } else if (hour > 23) {
    return -1;
  }
  return hour * 60 + minute;
}

// Removes the first match of re from text, returning the match (invalid when
// nothing matched).
QRegularExpressionMatch take(QString* text, const QRegularExpression& re) {
  const QRegularExpressionMatch match = re.match(*text);
  if (match.hasMatch()) {
    text->replace(match.capturedStart(), match.capturedLength(), QStringLiteral(" "));
  }
  return match;
}

int durationMinutes(const QString& amount, const QString& unit) {
  const double value = amount.toDouble();
  const bool hours = unit.startsWith(QLatin1Char('h'), Qt::CaseInsensitive);
  return static_cast<int>(value * (hours ? 60.0 : 1.0) + 0.5);
}

}  // namespace

QuickAddDraft parseQuickAdd(const QString& input, const QDate& today) {
  QuickAddDraft draft;
  QString text = QLatin1Char(' ') + input.simplified() + QLatin1Char(' ');

  // "@ place" runs to the end of the line.
  if (const auto match = take(&text, pattern(QStringLiteral("\\s@\\s*(.+)$")));
      match.hasMatch()) {
    draft.location = match.captured(1).trimmed();
  }

  int repeatWeekday = 0;
  if (const auto match =
          take(&text,
               pattern(QStringLiteral("\\bevery\\s+(other\\s+)?(weekday|day|week|month|"
                                      "year|") +
                       kWeekdayPattern.mid(1, kWeekdayPattern.size() - 2) +
                       QStringLiteral(")s?\\b")));
      match.hasMatch()) {
    const QString unit = match.captured(2).toLower();
    const QString interval =
        match.captured(1).isEmpty() ? QString() : QStringLiteral(";INTERVAL=2");
    if (unit == QStringLiteral("weekday")) {
      draft.recurrenceRule = QStringLiteral("FREQ=WEEKLY") + interval +
                             QStringLiteral(";BYDAY=MO,TU,WE,TH,FR");
    } else if (unit == QStringLiteral("day")) {
      draft.recurrenceRule = QStringLiteral("FREQ=DAILY") + interval;
    } else if (unit == QStringLiteral("week")) {
      draft.recurrenceRule = QStringLiteral("FREQ=WEEKLY") + interval;
    } else if (unit == QStringLiteral("month")) {
      draft.recurrenceRule = QStringLiteral("FREQ=MONTHLY") + interval;
    } else if (unit == QStringLiteral("year")) {
      draft.recurrenceRule = QStringLiteral("FREQ=YEARLY") + interval;
    } else {
      repeatWeekday = weekdayNumber(unit);
      draft.recurrenceRule =
          QStringLiteral("FREQ=WEEKLY") + interval + QStringLiteral(";BYDAY=") +
          QLatin1String(kRuleDays.at(static_cast<std::size_t>(repeatWeekday - 1)));
    }
  } else if (const auto keyword = take(
                 &text, pattern(QStringLiteral("\\b(daily|weekly|monthly|yearly)\\b")));
             keyword.hasMatch()) {
    draft.recurrenceRule = QStringLiteral("FREQ=") + keyword.captured(1).toUpper();
  }

  // Durations: "for 45 minutes", "15m", "1.5h".
  if (const auto match =
          take(&text, pattern(QStringLiteral("\\b(?:for\\s+)?(\\d+(?:\\.\\d+)?)\\s*(h|"
                                             "hrs?|hours?|m|mins?|minutes?)\\b")));
      match.hasMatch()) {
    draft.durationMinutes = durationMinutes(match.captured(1), match.captured(2));
  }

  // Time ranges need a meridiem or a colon so "Oct 3-6" stays a date range;
  // the text is only consumed once the range is known to be a time.
  {
    static const QRegularExpression range = pattern(
        QStringLiteral("\\b(?:from\\s+|at\\s+)?(\\d{1,2})(?::(\\d{2}))?\\s*(am|pm)?"
                       "\\s*(?:-|–|to)\\s*(\\d{1,2})(?::(\\d{2}))?\\s*(am|pm)?\\b"));
    const QRegularExpressionMatch match = range.match(text);
    const bool looksLikeTime =
        match.hasMatch() &&
        (!match.captured(2).isEmpty() || !match.captured(3).isEmpty() ||
         !match.captured(5).isEmpty() || !match.captured(6).isEmpty());
    if (looksLikeTime) {
      const QString endMeridiem = match.captured(6);
      const QString startMeridiem =
          match.captured(3).isEmpty() ? endMeridiem : match.captured(3);
      int start = clockMinutes(match.captured(1).toInt(), match.captured(2).toInt(),
                               startMeridiem);
      const int end = clockMinutes(match.captured(4).toInt(), match.captured(5).toInt(),
                                   endMeridiem);
      // "11-1pm" means 11am to 1pm, not 11pm.
      if (match.captured(3).isEmpty() && start >= end && start >= 12 * 60) {
        start -= 12 * 60;
      }
      if (start >= 0 && end > start) {
        draft.startMinute = start;
        draft.durationMinutes = end - start;
        text.replace(match.capturedStart(), match.capturedLength(),
                     QStringLiteral(" "));
      }
    }
  }
  if (draft.startMinute < 0) {
    if (const auto match = take(
            &text,
            pattern(QStringLiteral("\\b(?:at\\s+)?(\\d{1,2}):(\\d{2})\\s*(am|pm)?\\b|"
                                   "\\b(?:at\\s+)?(\\d{1,2})\\s*(am|pm)\\b|\\b(?:at\\s+"
                                   ")?(noon|midnight)\\b")));
        match.hasMatch()) {
      if (!match.captured(6).isEmpty()) {
        draft.startMinute =
            match.captured(6).toLower() == QStringLiteral("noon") ? 12 * 60 : 0;
      } else if (!match.captured(1).isEmpty()) {
        draft.startMinute = clockMinutes(match.captured(1).toInt(),
                                         match.captured(2).toInt(), match.captured(3));
      } else {
        draft.startMinute =
            clockMinutes(match.captured(4).toInt(), 0, match.captured(5));
      }
    }
  }

  // Dates, most specific first.
  if (const auto match =
          take(&text,
               pattern(QStringLiteral("\\b(?:on\\s+|from\\s+)?") + kMonthPattern +
                       QStringLiteral("\\s+(\\d{1,2})(?:st|nd|rd|th)?\\s*(?:-|–|to)\\s*"
                                      "(\\d{1,2})(?:st|nd|rd|th)?\\b")));
      match.hasMatch()) {
    const int month = monthNumber(match.captured(1));
    const QDate first = nextMonthDay(today, month, match.captured(2).toInt());
    const QDate last(first.year(), month, match.captured(3).toInt());
    if (first.isValid() && last.isValid() && last >= first) {
      draft.date = first;
      draft.allDay = true;
      draft.endDate = last.addDays(1);
    }
  } else if (const auto iso =
                 take(&text, pattern(QStringLiteral(
                                 "\\b(?:on\\s+)?(\\d{4})-(\\d{2})-(\\d{2})\\b")));
             iso.hasMatch()) {
    draft.date = QDate(iso.captured(1).toInt(), iso.captured(2).toInt(),
                       iso.captured(3).toInt());
  } else if (const auto monthDay =
                 take(&text, pattern(QStringLiteral("\\b(?:on\\s+)?") + kMonthPattern +
                                     QStringLiteral("\\s+(\\d{1,2})(?:st|nd|rd|th)?"
                                                    "(?:,?\\s+(\\d{4}))?\\b")));
             monthDay.hasMatch()) {
    const int month = monthNumber(monthDay.captured(1));
    const int day = monthDay.captured(2).toInt();
    draft.date = monthDay.captured(3).isEmpty()
                     ? nextMonthDay(today, month, day)
                     : QDate(monthDay.captured(3).toInt(), month, day);
  } else if (const auto dayMonth = take(
                 &text, pattern(QStringLiteral(
                                    "\\b(?:on\\s+)?(\\d{1,2})(?:st|nd|rd|th)?\\s+") +
                                kMonthPattern + QStringLiteral("\\b")));
             dayMonth.hasMatch()) {
    draft.date = nextMonthDay(today, monthNumber(dayMonth.captured(2)),
                              dayMonth.captured(1).toInt());
  } else if (const auto relative =
                 take(&text, pattern(QStringLiteral("\\b(today|tonight|tomorrow)\\b")));
             relative.hasMatch()) {
    const QString word = relative.captured(1).toLower();
    draft.date = word == QStringLiteral("tomorrow") ? today.addDays(1) : today;
    if (word == QStringLiteral("tonight") && draft.startMinute < 0) {
      draft.startMinute = 19 * 60;
    }
  } else {
    // "sun", "sat", "mon" and "wed" are also ordinary words, so on their own
    // they only count as days after "on" or "next".
    static const QRegularExpression weekdayPattern =
        pattern(QStringLiteral("\\b(on\\s+)?(next\\s+)?") + kWeekdayPattern +
                QStringLiteral("\\b"));
    static const QStringList ambiguous = {QStringLiteral("sun"), QStringLiteral("sat"),
                                          QStringLiteral("mon"), QStringLiteral("wed")};
    QRegularExpressionMatchIterator matches = weekdayPattern.globalMatch(text);
    while (matches.hasNext()) {
      const QRegularExpressionMatch weekday = matches.next();
      const bool qualified =
          !weekday.captured(1).isEmpty() || !weekday.captured(2).isEmpty();
      if (!qualified && ambiguous.contains(weekday.captured(3).toLower())) {
        continue;
      }
      draft.date = upcoming(today, weekdayNumber(weekday.captured(3)));
      // "next friday" said on a Friday means a week today; on any other day it
      // is the coming Friday.
      if (!weekday.captured(2).isEmpty() && draft.date == today) {
        draft.date = draft.date.addDays(7);
      }
      text.replace(weekday.capturedStart(), weekday.capturedLength(),
                   QStringLiteral(" "));
      break;
    }
  }
  if (!draft.date.isValid() && repeatWeekday > 0) {
    draft.date = upcoming(today, repeatWeekday);
  }

  if (draft.date.isValid() && draft.startMinute < 0 && !draft.allDay) {
    draft.allDay = true;
    draft.endDate = draft.date.addDays(1);
  }
  if (draft.allDay && draft.startMinute >= 0) {
    // A time turns a single named date back into a timed event.
    if (draft.endDate == draft.date.addDays(1)) {
      draft.allDay = false;
      draft.endDate = QDate();
    }
  }

  static const QRegularExpression connectors(
      QStringLiteral("^(?:\\s*\\b(?:on|at|from|for|every)\\b\\s*|[\\s,;]+)+|"
                     "(?:\\s*\\b(?:on|at|from|for|every)\\b\\s*|[\\s,;]+)+$"),
      QRegularExpression::CaseInsensitiveOption);
  draft.title = text.simplified().remove(connectors).simplified();
  return draft;
}

}  // namespace omacalendar
