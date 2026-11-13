// Maps an untrusted Google Calendar event resource and writes it back.
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstddef>
#include <cstdint>

#include "providers/google/googlemapper.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const QByteArray json(reinterpret_cast<const char*>(data),
                        static_cast<qsizetype>(size));
  const QJsonDocument document = QJsonDocument::fromJson(json);
  if (!document.isObject()) {
    return 0;
  }
  using omacalendar::google::GoogleMapper;
  const omacalendar::Event event =
      GoogleMapper::eventFromJson(document.object(), QStringLiteral("calendar"));
  (void)GoogleMapper::eventToJson(event);
  (void)GoogleMapper::calendarFromJson(document.object(), QStringLiteral("account"));
  return 0;
}
