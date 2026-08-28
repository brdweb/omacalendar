#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace omacalendar::caldav {

struct CalDavXmlError {
  QString code;
  QString message;
  qsizetype line = 0;
  qsizetype column = 0;

  [[nodiscard]] bool isEmpty() const { return code.isEmpty(); }
};

// One DAV:response. Fields from successful propstat blocks are merged while
// the response status is retained for sync deletions (for example, 404).
struct CalDavResponse {
  QString href;
  QString statusLine;
  int statusCode = 0;

  QString principalHref;
  QString calendarHomeSetHref;

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

  [[nodiscard]] bool isSuccess() const;
  [[nodiscard]] bool readOnly() const;
};

struct CalDavMultiStatusResult {
  QList<CalDavResponse> responses;
  QString syncToken;
  CalDavXmlError error;

  [[nodiscard]] bool ok() const { return error.isEmpty(); }
};

struct CalDavCollection {
  QString href;
  QString displayName;
  QString description;
  QString color;
  QString ctag;
  QString syncToken;
  bool readOnly = false;
};

struct CalDavResource {
  QString href;
  QString etag;
  QString calendarData;
  int statusCode = 0;

  [[nodiscard]] bool deleted() const { return statusCode == 404 || statusCode == 410; }
};

class CalDavXml final {
 public:
  // Namespace prefixes are deliberately ignored; elements are identified by
  // namespace URI and local name.
  [[nodiscard]] static CalDavMultiStatusResult parseMultiStatus(const QByteArray& xml);

  [[nodiscard]] static QString principalHref(const CalDavMultiStatusResult& result);
  [[nodiscard]] static QString calendarHomeSetHref(
      const CalDavMultiStatusResult& result);
  [[nodiscard]] static QList<CalDavCollection> collections(
      const CalDavMultiStatusResult& result);
  [[nodiscard]] static QList<CalDavResource> resources(
      const CalDavMultiStatusResult& result);
};

}  // namespace omacalendar::caldav
