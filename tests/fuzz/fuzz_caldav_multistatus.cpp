// Parses an untrusted CalDAV/WebDAV multistatus response.
#include <QByteArray>
#include <cstddef>
#include <cstdint>

#include "providers/caldav/caldavxml.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const QByteArray xml(reinterpret_cast<const char*>(data),
                       static_cast<qsizetype>(size));
  using omacalendar::caldav::CalDavXml;
  const auto result = CalDavXml::parseMultiStatus(xml);
  (void)CalDavXml::principalHref(result);
  (void)CalDavXml::calendarHomeSetHref(result);
  (void)CalDavXml::schedulingCapabilities(result);
  (void)CalDavXml::collections(result);
  (void)CalDavXml::resources(result);
  return 0;
}
