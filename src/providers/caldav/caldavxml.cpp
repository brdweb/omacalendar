#include "providers/caldav/caldavxml.h"

#include <QXmlStreamReader>

namespace omacalendar::caldav {
namespace {

constexpr qsizetype kMaximumXmlBytes = 16 * 1024 * 1024;
constexpr QLatin1StringView kDavNamespace("DAV:");
constexpr QLatin1StringView kCalDavNamespace("urn:ietf:params:xml:ns:caldav");
constexpr QLatin1StringView kAppleIcalNamespace("http://apple.com/ns/ical/");
constexpr QLatin1StringView kCalendarServerNamespace("http://calendarserver.org/ns/");

bool isElement(const QXmlStreamReader& reader, QLatin1StringView ns,
               QLatin1StringView name) {
  return reader.namespaceUri() == ns && reader.name() == name;
}

int statusCode(const QString& statusLine) {
  const QStringList parts = statusLine.simplified().split(QLatin1Char(' '));
  for (const QString& part : parts) {
    if (part.size() != 3) {
      continue;
    }
    bool valid = false;
    const int candidate = part.toInt(&valid);
    if (valid && candidate >= 100 && candidate <= 599) {
      return candidate;
    }
  }
  return 0;
}

bool successfulStatus(int code) { return code == 0 || (code >= 200 && code < 300); }

QString readNestedHref(QXmlStreamReader& reader) {
  QString href;
  while (reader.readNextStartElement()) {
    if (isElement(reader, kDavNamespace, QLatin1StringView("href"))) {
      const QString candidate = reader.readElementText().trimmed();
      if (href.isEmpty()) {
        href = candidate;
      }
    } else {
      reader.skipCurrentElement();
    }
  }
  return href;
}

QStringList readNestedHrefs(QXmlStreamReader& reader) {
  QStringList hrefs;
  while (reader.readNextStartElement()) {
    if (isElement(reader, kDavNamespace, QLatin1StringView("href"))) {
      const QString href = reader.readElementText().trimmed();
      if (!href.isEmpty()) {
        hrefs.append(href);
      }
    } else {
      reader.skipCurrentElement();
    }
  }
  hrefs.removeDuplicates();
  return hrefs;
}

struct PropertyValues {
  QString principalHref;
  QString calendarHomeSetHref;
  QString scheduleInboxHref;
  QString scheduleOutboxHref;
  QStringList calendarUserAddresses;
  QString displayName;
  QString description;
  QString color;
  QString ctag;
  QString syncToken;
  QString etag;
  QString calendarData;
  bool isCollection = false;
  bool isCalendar = false;
  bool privilegesReported = false;
  bool canWrite = false;
  bool canBind = false;
  bool canUnbind = false;
};

void parseResourceType(QXmlStreamReader& reader, PropertyValues* properties) {
  while (reader.readNextStartElement()) {
    if (isElement(reader, kDavNamespace, QLatin1StringView("collection"))) {
      properties->isCollection = true;
    } else if (isElement(reader, kCalDavNamespace, QLatin1StringView("calendar"))) {
      properties->isCalendar = true;
    }
    reader.skipCurrentElement();
  }
}

void parsePrivilegeSet(QXmlStreamReader& reader, PropertyValues* properties) {
  properties->privilegesReported = true;
  int depth = 1;
  while (depth > 0 && !reader.atEnd()) {
    const QXmlStreamReader::TokenType token = reader.readNext();
    if (token == QXmlStreamReader::StartElement) {
      ++depth;
      if (reader.namespaceUri() == kDavNamespace &&
          (reader.name() == QLatin1StringView("write") ||
           reader.name() == QLatin1StringView("write-content") ||
           reader.name() == QLatin1StringView("write-properties") ||
           reader.name() == QLatin1StringView("bind") ||
           reader.name() == QLatin1StringView("unbind") ||
           reader.name() == QLatin1StringView("all"))) {
        properties->canWrite = true;
      }
      if (reader.namespaceUri() == kDavNamespace &&
          (reader.name() == QLatin1StringView("bind") ||
           reader.name() == QLatin1StringView("all"))) {
        properties->canBind = true;
      }
      if (reader.namespaceUri() == kDavNamespace &&
          (reader.name() == QLatin1StringView("unbind") ||
           reader.name() == QLatin1StringView("all"))) {
        properties->canUnbind = true;
      }
    } else if (token == QXmlStreamReader::EndElement) {
      --depth;
    }
  }
}

QString simpleText(QXmlStreamReader& reader) {
  return reader.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
}

void parseProperties(QXmlStreamReader& reader, PropertyValues* properties) {
  while (reader.readNextStartElement()) {
    if (isElement(reader, kDavNamespace, QLatin1StringView("current-user-principal")) ||
        isElement(reader, kDavNamespace, QLatin1StringView("principal-URL"))) {
      properties->principalHref = readNestedHref(reader);
    } else if (isElement(reader, kCalDavNamespace,
                         QLatin1StringView("calendar-home-set"))) {
      properties->calendarHomeSetHref = readNestedHref(reader);
    } else if (isElement(reader, kCalDavNamespace,
                         QLatin1StringView("schedule-inbox-URL"))) {
      properties->scheduleInboxHref = readNestedHref(reader);
    } else if (isElement(reader, kCalDavNamespace,
                         QLatin1StringView("schedule-outbox-URL"))) {
      properties->scheduleOutboxHref = readNestedHref(reader);
    } else if (isElement(reader, kCalDavNamespace,
                         QLatin1StringView("calendar-user-address-set"))) {
      properties->calendarUserAddresses = readNestedHrefs(reader);
    } else if (isElement(reader, kDavNamespace, QLatin1StringView("displayname"))) {
      properties->displayName = simpleText(reader);
    } else if (isElement(reader, kCalDavNamespace,
                         QLatin1StringView("calendar-description"))) {
      properties->description = simpleText(reader);
    } else if (isElement(reader, kAppleIcalNamespace,
                         QLatin1StringView("calendar-color"))) {
      properties->color = simpleText(reader);
    } else if (isElement(reader, kCalendarServerNamespace,
                         QLatin1StringView("getctag"))) {
      properties->ctag = simpleText(reader);
    } else if (isElement(reader, kDavNamespace, QLatin1StringView("sync-token"))) {
      properties->syncToken = simpleText(reader);
    } else if (isElement(reader, kDavNamespace, QLatin1StringView("getetag"))) {
      properties->etag = simpleText(reader);
    } else if (isElement(reader, kCalDavNamespace,
                         QLatin1StringView("calendar-data"))) {
      // Calendar data is character content, not DAV metadata. Do not simplify
      // its whitespace because line folding is meaningful to iCalendar.
      properties->calendarData =
          reader.readElementText(QXmlStreamReader::IncludeChildElements);
    } else if (isElement(reader, kDavNamespace, QLatin1StringView("resourcetype"))) {
      parseResourceType(reader, properties);
    } else if (isElement(reader, kDavNamespace,
                         QLatin1StringView("current-user-privilege-set"))) {
      parsePrivilegeSet(reader, properties);
    } else {
      reader.skipCurrentElement();
    }
  }
}

struct PropStat {
  PropertyValues properties;
  QString statusLine;
  int statusCode = 0;
};

PropStat parsePropStat(QXmlStreamReader& reader) {
  PropStat result;
  while (reader.readNextStartElement()) {
    if (isElement(reader, kDavNamespace, QLatin1StringView("prop"))) {
      parseProperties(reader, &result.properties);
    } else if (isElement(reader, kDavNamespace, QLatin1StringView("status"))) {
      result.statusLine = simpleText(reader);
      result.statusCode = statusCode(result.statusLine);
    } else {
      reader.skipCurrentElement();
    }
  }
  return result;
}

void mergeProperties(CalDavResponse* response, const PropertyValues& properties) {
  if (!properties.principalHref.isEmpty()) {
    response->principalHref = properties.principalHref;
  }
  if (!properties.calendarHomeSetHref.isEmpty()) {
    response->calendarHomeSetHref = properties.calendarHomeSetHref;
  }
  if (!properties.scheduleInboxHref.isEmpty()) {
    response->scheduleInboxHref = properties.scheduleInboxHref;
  }
  if (!properties.scheduleOutboxHref.isEmpty()) {
    response->scheduleOutboxHref = properties.scheduleOutboxHref;
  }
  if (!properties.calendarUserAddresses.isEmpty()) {
    response->calendarUserAddresses = properties.calendarUserAddresses;
  }
  if (!properties.displayName.isNull()) {
    response->displayName = properties.displayName;
  }
  if (!properties.description.isNull()) {
    response->description = properties.description;
  }
  if (!properties.color.isNull()) {
    response->color = properties.color;
  }
  if (!properties.ctag.isNull()) {
    response->ctag = properties.ctag;
  }
  if (!properties.syncToken.isNull()) {
    response->syncToken = properties.syncToken;
  }
  if (!properties.etag.isNull()) {
    response->etag = properties.etag;
  }
  if (!properties.calendarData.isNull()) {
    response->calendarData = properties.calendarData;
  }
  response->isCollection = response->isCollection || properties.isCollection;
  response->isCalendar = response->isCalendar || properties.isCalendar;
  if (properties.privilegesReported) {
    response->privilegesReported = true;
    response->canWrite = response->canWrite || properties.canWrite;
    response->canBind = response->canBind || properties.canBind;
    response->canUnbind = response->canUnbind || properties.canUnbind;
  }
}

CalDavResponse parseResponse(QXmlStreamReader& reader) {
  CalDavResponse response;
  int firstPropStatStatus = 0;
  QString firstPropStatStatusLine;
  int successfulPropStatStatus = 0;
  QString successfulPropStatStatusLine;
  bool hasSuccessfulPropStat = false;

  while (reader.readNextStartElement()) {
    if (isElement(reader, kDavNamespace, QLatin1StringView("href"))) {
      response.href = simpleText(reader);
    } else if (isElement(reader, kDavNamespace, QLatin1StringView("status"))) {
      response.statusLine = simpleText(reader);
      response.statusCode = statusCode(response.statusLine);
    } else if (isElement(reader, kDavNamespace, QLatin1StringView("propstat"))) {
      const PropStat propStat = parsePropStat(reader);
      if (firstPropStatStatus == 0) {
        firstPropStatStatus = propStat.statusCode;
        firstPropStatStatusLine = propStat.statusLine;
      }
      if (successfulStatus(propStat.statusCode)) {
        if (!hasSuccessfulPropStat) {
          successfulPropStatStatus = propStat.statusCode;
          successfulPropStatStatusLine = propStat.statusLine;
          hasSuccessfulPropStat = true;
        }
        mergeProperties(&response, propStat.properties);
      }
    } else {
      reader.skipCurrentElement();
    }
  }

  if (response.statusCode == 0) {
    if (hasSuccessfulPropStat) {
      response.statusCode = successfulPropStatStatus;
      response.statusLine = successfulPropStatStatusLine;
    } else if (firstPropStatStatus != 0) {
      response.statusCode = firstPropStatStatus;
      response.statusLine = firstPropStatStatusLine;
    }
  }
  return response;
}

}  // namespace

bool CalDavResponse::isSuccess() const { return successfulStatus(statusCode); }

bool CalDavResponse::readOnly() const { return privilegesReported && !canWrite; }

CalDavMultiStatusResult CalDavXml::parseMultiStatus(const QByteArray& xml) {
  CalDavMultiStatusResult result;
  if (xml.isEmpty()) {
    result.error = {QStringLiteral("empty_document"),
                    QStringLiteral("The DAV response was empty")};
    return result;
  }
  if (xml.size() > kMaximumXmlBytes) {
    result.error = {QStringLiteral("document_too_large"),
                    QStringLiteral("The DAV XML response exceeds the 16 MiB limit")};
    return result;
  }

  QXmlStreamReader reader(xml);
  bool foundRoot = false;
  while (!reader.atEnd()) {
    const QXmlStreamReader::TokenType token = reader.readNext();
    if (token == QXmlStreamReader::DTD) {
      result.error = {QStringLiteral("doctype_not_allowed"),
                      QStringLiteral("DAV XML responses containing a DTD are rejected"),
                      reader.lineNumber(), reader.columnNumber()};
      return result;
    }
    if (token != QXmlStreamReader::StartElement) {
      continue;
    }
    if (!isElement(reader, kDavNamespace, QLatin1StringView("multistatus"))) {
      result.error = {QStringLiteral("invalid_root"),
                      QStringLiteral("Expected a DAV:multistatus document"),
                      reader.lineNumber(), reader.columnNumber()};
      return result;
    }

    foundRoot = true;
    while (reader.readNextStartElement()) {
      if (isElement(reader, kDavNamespace, QLatin1StringView("response"))) {
        result.responses.append(parseResponse(reader));
      } else if (isElement(reader, kDavNamespace, QLatin1StringView("sync-token"))) {
        result.syncToken = simpleText(reader);
      } else {
        reader.skipCurrentElement();
      }
    }
    // Consume the remainder so malformed trailing content is reported rather
    // than silently accepting only the first document element.
    while (!reader.atEnd()) {
      reader.readNext();
    }
    break;
  }

  if (reader.hasError()) {
    result.responses.clear();
    result.syncToken.clear();
    result.error = {QStringLiteral("malformed_xml"), reader.errorString(),
                    reader.lineNumber(), reader.columnNumber()};
  } else if (!foundRoot) {
    result.error = {QStringLiteral("invalid_root"),
                    QStringLiteral("Expected a DAV:multistatus document")};
  }
  return result;
}

QString CalDavXml::principalHref(const CalDavMultiStatusResult& result) {
  for (const CalDavResponse& response : result.responses) {
    if (response.isSuccess() && !response.principalHref.isEmpty()) {
      return response.principalHref;
    }
  }
  return {};
}

QString CalDavXml::calendarHomeSetHref(const CalDavMultiStatusResult& result) {
  for (const CalDavResponse& response : result.responses) {
    if (response.isSuccess() && !response.calendarHomeSetHref.isEmpty()) {
      return response.calendarHomeSetHref;
    }
  }
  return {};
}

CalDavSchedulingCapabilities CalDavXml::schedulingCapabilities(
    const CalDavMultiStatusResult& result) {
  CalDavSchedulingCapabilities capabilities;
  for (const CalDavResponse& response : result.responses) {
    if (!response.isSuccess()) {
      continue;
    }
    if (capabilities.inboxHref.isEmpty()) {
      capabilities.inboxHref = response.scheduleInboxHref;
    }
    if (capabilities.outboxHref.isEmpty()) {
      capabilities.outboxHref = response.scheduleOutboxHref;
    }
    capabilities.userAddresses.append(response.calendarUserAddresses);
  }
  capabilities.userAddresses.removeDuplicates();
  return capabilities;
}

QList<CalDavCollection> CalDavXml::collections(const CalDavMultiStatusResult& result) {
  QList<CalDavCollection> output;
  for (const CalDavResponse& response : result.responses) {
    if (!response.isSuccess() || !response.isCalendar || response.href.isEmpty()) {
      continue;
    }
    output.append({response.href, response.displayName, response.description,
                   response.color, response.ctag, response.syncToken,
                   response.readOnly(), response.canBind, response.canUnbind});
  }
  return output;
}

QList<CalDavResource> CalDavXml::resources(const CalDavMultiStatusResult& result) {
  QList<CalDavResource> output;
  for (const CalDavResponse& response : result.responses) {
    if (response.href.isEmpty()) {
      continue;
    }
    if (response.statusCode != 404 && response.statusCode != 410 &&
        !response.isSuccess()) {
      continue;
    }
    if (response.statusCode != 404 && response.statusCode != 410 &&
        response.etag.isEmpty() && response.calendarData.isEmpty()) {
      continue;
    }
    output.append(
        {response.href, response.etag, response.calendarData, response.statusCode});
  }
  return output;
}

}  // namespace omacalendar::caldav
