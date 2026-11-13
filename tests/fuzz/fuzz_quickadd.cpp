// Parses untrusted quick-add text (typed by the user or passed to
// omacalendarctl add).
#include <QByteArray>
#include <QDate>
#include <QString>
#include <cstddef>
#include <cstdint>

#include "core/quickadd.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > 4096) {
    return 0;
  }
  const QString text = QString::fromUtf8(reinterpret_cast<const char*>(data),
                                         static_cast<qsizetype>(size));
  (void)omacalendar::parseQuickAdd(text, QDate(2026, 11, 13));
  return 0;
}
