#include "calendarpdf.h"

#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QLocale>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <algorithm>

namespace omacalendar {
namespace {

// A fixed light palette: printouts never follow the on-screen theme.
const QColor kText(0x1f, 0x23, 0x28);
const QColor kMuted(0x5f, 0x66, 0x6d);
const QColor kRule(0xc9, 0xcd, 0xd2);
const QColor kShade(0xf1, 0xf3, 0xf5);
const QColor kFallbackEventColor(0x7a, 0xa2, 0xf7);

constexpr qsizetype kMaximumDescriptionLength = 1200;

QPageSize defaultPageSize() {
  return QLocale::system().measurementSystem() == QLocale::ImperialUSSystem
             ? QPageSize(QPageSize::Letter)
             : QPageSize(QPageSize::A4);
}

QFont font(const qreal pointSize, const QFont::Weight weight = QFont::Normal) {
  QFont value;
  value.setPointSizeF(pointSize);
  value.setWeight(weight);
  return value;
}

QColor eventColor(const PrintableEvent& event) {
  return event.color.isValid() ? event.color : kFallbackEventColor;
}

QString rangeText(const PrintOptions& options) {
  const QLocale locale;
  if (options.firstDate == options.lastDate) {
    return locale.toString(options.firstDate, QLocale::LongFormat);
  }
  return QStringLiteral("%1 – %2").arg(
      locale.toString(options.firstDate, QLocale::ShortFormat),
      locale.toString(options.lastDate, QLocale::ShortFormat));
}

// How the event's time reads on one day of the list, with an ellipsis where it
// continues from or into another day.
QString timeText(const PrintableEvent& event, const QDate& day,
                 const QString& timeFormat) {
  if (event.allDay) {
    return QObject::tr("All day");
  }
  const QString from = event.start.date() == day
                           ? event.start.time().toString(timeFormat)
                           : QStringLiteral("…");
  const QString to = event.end.date() == day ? event.end.time().toString(timeFormat)
                                             : QStringLiteral("…");
  return QStringLiteral("%1 – %2").arg(from, to);
}

QList<const PrintableEvent*> eventsOn(const QList<PrintableEvent>& events,
                                      const QDate& day) {
  QList<const PrintableEvent*> result;
  for (const PrintableEvent& event : events) {
    if (event.occursOn(day)) {
      result.append(&event);
    }
  }
  std::stable_sort(result.begin(), result.end(),
                   [](const PrintableEvent* left, const PrintableEvent* right) {
                     if (left->allDay != right->allDay) {
                       return left->allDay;
                     }
                     if (!left->allDay && left->start != right->start) {
                       return left->start < right->start;
                     }
                     return left->title.localeAwareCompare(right->title) < 0;
                   });
  return result;
}

class PdfDocument final {
 public:
  PdfDocument(QPdfWriter* writer, const PrintOptions& options)
      : m_writer(writer), m_options(options) {}

  bool begin() {
    if (!m_painter.begin(m_writer)) {
      return false;
    }
    m_pages = 1;
    startPage();
    return true;
  }

  void end() { m_painter.end(); }

  [[nodiscard]] int pages() const { return m_pages; }
  QPainter& painter() { return m_painter; }
  [[nodiscard]] QRectF area() const { return m_area; }
  [[nodiscard]] qreal top() const { return m_top; }

  void newPage() {
    m_writer->newPage();
    ++m_pages;
    startPage();
  }

 private:
  void startPage() {
    const QRect paint = m_writer->pageLayout().paintRectPixels(m_writer->resolution());
    const QRectF page(0, 0, paint.width(), paint.height());
    m_painter.setPen(kText);
    m_painter.setFont(font(15, QFont::Bold));
    const QFontMetricsF titleMetrics(m_painter.font(), m_writer);
    m_painter.drawText(QPointF(0, titleMetrics.ascent()), m_options.title);
    m_painter.setFont(font(9));
    m_painter.setPen(kMuted);
    const QFontMetricsF smallMetrics(m_painter.font(), m_writer);
    const QString range = rangeText(m_options);
    m_painter.drawText(QPointF(page.width() - smallMetrics.horizontalAdvance(range),
                               titleMetrics.ascent()),
                       range);
    const QString pageNumber = QString::number(m_pages);
    m_painter.drawText(
        QPointF(page.width() - smallMetrics.horizontalAdvance(pageNumber),
                page.height() - smallMetrics.descent()),
        pageNumber);
    const qreal headerBottom = titleMetrics.height() + smallMetrics.height() * 0.6;
    m_painter.setPen(QPen(kRule, 0));
    m_painter.drawLine(QPointF(0, headerBottom), QPointF(page.width(), headerBottom));
    m_top = headerBottom + smallMetrics.height() * 0.8;
    m_area = QRectF(0, m_top, page.width(),
                    page.height() - m_top - smallMetrics.height() * 1.5);
  }

  QPdfWriter* m_writer;
  const PrintOptions& m_options;
  QPainter m_painter;
  QRectF m_area;
  qreal m_top = 0;
  int m_pages = 0;
};

void writeList(PdfDocument* document, QPdfWriter* writer,
               const QList<PrintableEvent>& events, const PrintOptions& options) {
  QPainter& painter = document->painter();
  const QLocale locale;
  const QFont dayFont = font(11, QFont::Bold);
  const QFont titleFont = font(10, QFont::DemiBold);
  const QFont bodyFont = font(10);
  const QFont detailFont = font(8.5);
  const QFontMetricsF dayMetrics(dayFont, writer);
  const QFontMetricsF bodyMetrics(bodyFont, writer);
  const QFontMetricsF detailMetrics(detailFont, writer);
  const qreal timeColumn =
      bodyMetrics.horizontalAdvance(QStringLiteral("00:00 AM – 00:00 AM")) +
      bodyMetrics.height();
  const qreal gap = bodyMetrics.height() * 0.35;
  qreal y = document->area().top();
  bool printedAny = false;

  const auto ensureSpace = [&](const qreal height) {
    if (y + height > document->area().bottom() && y > document->area().top()) {
      document->newPage();
      y = document->area().top();
    }
  };

  for (QDate day = options.firstDate; day <= options.lastDate; day = day.addDays(1)) {
    const QList<const PrintableEvent*> dayEvents = eventsOn(events, day);
    if (dayEvents.isEmpty()) {
      continue;
    }
    printedAny = true;
    // Keep a day heading with at least its first event.
    ensureSpace(dayMetrics.height() + bodyMetrics.height() * 2 + gap);
    painter.setFont(dayFont);
    painter.setPen(kText);
    painter.drawText(QPointF(0, y + dayMetrics.ascent()),
                     locale.toString(day, QStringLiteral("dddd d MMMM yyyy")));
    y += dayMetrics.height() + gap;
    painter.setPen(QPen(kRule, 0));
    painter.drawLine(QPointF(0, y - gap / 2),
                     QPointF(document->area().width(), y - gap / 2));

    for (const PrintableEvent* event : dayEvents) {
      const qreal textLeft = timeColumn;
      const qreal textWidth = document->area().width() - textLeft;
      QStringList details;
      if (options.includeDetails) {
        if (!event->location.trimmed().isEmpty()) {
          details.append(event->location.trimmed());
        }
        if (!event->calendarName.isEmpty()) {
          details.append(event->calendarName);
        }
      }
      QString notes;
      if (options.includeDetails) {
        notes = event->description.trimmed();
        if (notes.size() > kMaximumDescriptionLength) {
          notes = notes.left(kMaximumDescriptionLength).trimmed() + QStringLiteral("…");
        }
      }
      painter.setFont(titleFont);
      const QRectF titleBounds = painter.boundingRect(
          QRectF(textLeft, 0, textWidth, 1e6), Qt::TextWordWrap,
          event->title.isEmpty() ? QObject::tr("(No title)") : event->title);
      painter.setFont(detailFont);
      const QString detailLine = details.join(QStringLiteral(" · "));
      const qreal detailHeight = detailLine.isEmpty() ? 0 : detailMetrics.height();
      const QRectF notesBounds =
          notes.isEmpty() ? QRectF()
                          : painter.boundingRect(QRectF(textLeft, 0, textWidth, 1e6),
                                                 Qt::TextWordWrap, notes);
      const qreal height = std::max(titleBounds.height(), bodyMetrics.height()) +
                           detailHeight + notesBounds.height() + gap;
      ensureSpace(height);

      painter.setFont(bodyFont);
      painter.setPen(kMuted);
      painter.drawText(QPointF(0, y + bodyMetrics.ascent()),
                       timeText(*event, day, options.timeFormat));
      const qreal swatch = bodyMetrics.height() * 0.45;
      painter.fillRect(QRectF(textLeft - swatch * 1.8,
                              y + (bodyMetrics.height() - swatch) / 2, swatch, swatch),
                       eventColor(*event));
      painter.setFont(titleFont);
      painter.setPen(kText);
      painter.drawText(
          QRectF(textLeft, y, textWidth, titleBounds.height()), Qt::TextWordWrap,
          event->title.isEmpty() ? QObject::tr("(No title)") : event->title);
      qreal lineY = y + std::max(titleBounds.height(), bodyMetrics.height());
      painter.setFont(detailFont);
      painter.setPen(kMuted);
      if (!detailLine.isEmpty()) {
        painter.drawText(
            QRectF(textLeft, lineY, textWidth, detailHeight), Qt::TextSingleLine,
            detailMetrics.elidedText(detailLine, Qt::ElideRight, textWidth));
        lineY += detailHeight;
      }
      if (!notes.isEmpty()) {
        painter.drawText(QRectF(textLeft, lineY, textWidth, notesBounds.height()),
                         Qt::TextWordWrap, notes);
      }
      y += height;
    }
    y += gap;
  }

  if (!printedAny) {
    painter.setFont(bodyFont);
    painter.setPen(kMuted);
    painter.drawText(QPointF(0, y + bodyMetrics.ascent()),
                     QObject::tr("No events in this period."));
  }
}

void writeMonth(PdfDocument* document, QPdfWriter* writer, const QDate& month,
                const QList<PrintableEvent>& events, const PrintOptions& options) {
  QPainter& painter = document->painter();
  const QLocale locale;
  const QFont monthFont = font(13, QFont::Bold);
  const QFont headerFont = font(8, QFont::DemiBold);
  const QFont dayFont = font(8.5, QFont::DemiBold);
  const QFont eventFont = font(7);
  const QFontMetricsF monthMetrics(monthFont, writer);
  const QFontMetricsF headerMetrics(headerFont, writer);
  const QFontMetricsF dayMetrics(dayFont, writer);
  const QFontMetricsF eventMetrics(eventFont, writer);

  const QRectF area = document->area();
  painter.setFont(monthFont);
  painter.setPen(kText);
  painter.drawText(QPointF(0, area.top() + monthMetrics.ascent()),
                   locale.toString(month, QStringLiteral("MMMM yyyy")));
  const qreal headerTop = area.top() + monthMetrics.height() * 1.3;

  const int offset = (month.dayOfWeek() - options.firstDayOfWeek + 7) % 7;
  const QDate gridStart = month.addDays(-offset);
  const int days = offset + month.daysInMonth();
  const int rows = (days + 6) / 7;
  const qreal columnWidth = area.width() / 7;
  const qreal gridTop = headerTop + headerMetrics.height() * 1.4;
  const qreal rowHeight = (area.bottom() - gridTop) / rows;

  painter.setFont(headerFont);
  painter.setPen(kMuted);
  for (int column = 0; column < 7; ++column) {
    const int weekday = ((options.firstDayOfWeek - 1 + column) % 7) + 1;
    painter.drawText(
        QRectF(column * columnWidth, headerTop, columnWidth, headerMetrics.height()),
        Qt::AlignHCenter | Qt::AlignVCenter,
        locale.dayName(weekday, QLocale::ShortFormat));
  }

  const qreal padding = eventMetrics.height() * 0.3;
  for (int row = 0; row < rows; ++row) {
    for (int column = 0; column < 7; ++column) {
      const QDate day = gridStart.addDays(row * 7 + column);
      const QRectF cell(column * columnWidth, gridTop + row * rowHeight, columnWidth,
                        rowHeight);
      const bool inMonth = day.month() == month.month();
      if (!inMonth) {
        painter.fillRect(cell, kShade);
      }
      painter.setPen(QPen(kRule, 0));
      painter.drawRect(cell);
      painter.setFont(dayFont);
      painter.setPen(inMonth ? kText : kMuted);
      painter.drawText(
          QPointF(cell.left() + padding, cell.top() + padding + dayMetrics.ascent()),
          QString::number(day.day()));
      if (!inMonth || day < options.firstDate || day > options.lastDate) {
        continue;
      }

      const QList<const PrintableEvent*> dayEvents = eventsOn(events, day);
      const qreal listTop = cell.top() + padding + dayMetrics.height();
      const int capacity =
          std::max(0, static_cast<int>((cell.bottom() - padding - listTop) /
                                       eventMetrics.height()));
      const bool overflow = dayEvents.size() > capacity;
      const int shown =
          overflow ? std::max(0, capacity - 1) : static_cast<int>(dayEvents.size());
      painter.setFont(eventFont);
      const qreal textWidth = cell.width() - padding * 3;
      for (int index = 0; index < shown; ++index) {
        const PrintableEvent* event = dayEvents.at(index);
        const qreal lineTop = listTop + index * eventMetrics.height();
        painter.fillRect(
            QRectF(cell.left() + padding, lineTop + eventMetrics.height() * 0.15,
                   padding * 0.6, eventMetrics.height() * 0.7),
            eventColor(*event));
        QString text =
            event->title.isEmpty() ? QObject::tr("(No title)") : event->title;
        if (!event->allDay && event->start.date() == day) {
          text = event->start.time().toString(options.timeFormat) + QLatin1Char(' ') +
                 text;
        }
        painter.setPen(kText);
        painter.drawText(
            QPointF(cell.left() + padding * 2, lineTop + eventMetrics.ascent()),
            eventMetrics.elidedText(text, Qt::ElideRight, textWidth));
      }
      if (overflow) {
        painter.setPen(kMuted);
        painter.drawText(
            QPointF(cell.left() + padding * 2,
                    listTop + shown * eventMetrics.height() + eventMetrics.ascent()),
            QObject::tr("+%n more", nullptr,
                        static_cast<int>(dayEvents.size()) - shown));
      }
    }
  }
}

}  // namespace

bool PrintableEvent::occursOn(const QDate& day) const {
  if (allDay) {
    return startDate.isValid() && day >= startDate &&
           day < (endDate.isValid() && endDate > startDate ? endDate
                                                           : startDate.addDays(1));
  }
  if (!start.isValid()) {
    return false;
  }
  if (start.date() == day) {
    return true;
  }
  // A timed event that ends exactly at midnight does not reach that day.
  return start.date() < day && end.isValid() &&
         (end.date() > day || (end.date() == day && end.time() > QTime(0, 0)));
}

int writeCalendarPdf(const QString& path, const QList<PrintableEvent>& events,
                     const PrintOptions& options, QString* errorMessage) {
  const auto fail = [errorMessage](const QString& message) {
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    return 0;
  };
  if (!options.firstDate.isValid() || !options.lastDate.isValid() ||
      options.firstDate > options.lastDate) {
    return fail(QObject::tr("A valid date range is required"));
  }
  if (options.firstDate.daysTo(options.lastDate) > 366) {
    return fail(QObject::tr("Print at most one year at a time"));
  }

  // QSaveFile only replaces an existing file once the new one is complete.
  QSaveFile file(path);
  const QString name = QFileInfo(path).fileName();
  if (!file.open(QIODevice::WriteOnly)) {
    return fail(QObject::tr("Could not write %1").arg(name));
  }
  QPdfWriter writer(&file);
  writer.setTitle(options.title);
  writer.setCreator(QStringLiteral("OmaCalendar"));
  writer.setResolution(300);
  const QPageLayout layout(defaultPageSize(),
                           options.layout == PrintLayout::Month ? QPageLayout::Landscape
                                                                : QPageLayout::Portrait,
                           QMarginsF(12, 12, 12, 12), QPageLayout::Millimeter);
  writer.setPageLayout(layout);

  PdfDocument document(&writer, options);
  if (!document.begin()) {
    file.cancelWriting();
    return fail(QObject::tr("Could not write %1").arg(name));
  }
  if (options.layout == PrintLayout::Month) {
    QDate month(options.firstDate.year(), options.firstDate.month(), 1);
    const QDate lastMonth(options.lastDate.year(), options.lastDate.month(), 1);
    bool first = true;
    for (; month <= lastMonth; month = month.addMonths(1)) {
      if (!first) {
        document.newPage();
      }
      first = false;
      writeMonth(&document, &writer, month, events, options);
    }
  } else {
    writeList(&document, &writer, events, options);
  }
  const int pages = document.pages();
  document.end();
  if (file.pos() == 0 || !file.commit()) {
    file.cancelWriting();
    return fail(QObject::tr("Could not write %1").arg(name));
  }
  return pages;
}

}  // namespace omacalendar
