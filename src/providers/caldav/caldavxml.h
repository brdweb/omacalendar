#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

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

  // Component names from supported-calendar-component-set, upper case.
  // Empty when the server did not report the property.
  QStringList supportedComponents;

  bool isCollection = false;
  bool isCalendar = false;
  bool privilegesReported = false;
  bool canWrite = false;
  bool canBind = false;
  bool canUnbind = false;

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
  bool canBind = false;
  bool canUnbind = false;
  QStringList supportedComponents;

  // RFC 4791: a collection that does not report the property accepts any
  // component, so only an explicit list rules VEVENT or VTODO out.
  [[nodiscard]] bool holdsEvents() const {
    return supportedComponents.isEmpty() ||
           supportedComponents.contains(QStringLiteral("VEVENT"));
  }
  // Task lists are only created for collections that explicitly accept
  // VTODO, so ordinary calendars do not each turn into an empty task list.
  [[nodiscard]] bool holdsTasks() const {
    return supportedComponents.contains(QStringLiteral("VTODO"));
  }
};

struct CalDavResource {
  QString href;
  QString etag;
  QString calendarData;
  int statusCode = 0;

  [[nodiscard]] bool deleted() const { return statusCode == 404 || statusCode == 410; }
};

struct CalDavSchedulingCapabilities {
  QString inboxHref;
  QString outboxHref;
  QStringList userAddresses;

  // An address plus an advertised scheduling outbox proves that the account
  // can originate iTIP scheduling messages. Mere attendee data does not.
  [[nodiscard]] bool canSend() const {
    return !outboxHref.isEmpty() && !userAddresses.isEmpty();
  }
};

class CalDavXml final {
 public:
  // Namespace prefixes are deliberately ignored; elements are identified by
  // namespace URI and local name.
  [[nodiscard]] static CalDavMultiStatusResult parseMultiStatus(const QByteArray& xml);

  [[nodiscard]] static QString principalHref(const CalDavMultiStatusResult& result);
  [[nodiscard]] static QString calendarHomeSetHref(
      const CalDavMultiStatusResult& result);
  [[nodiscard]] static CalDavSchedulingCapabilities schedulingCapabilities(
      const CalDavMultiStatusResult& result);
  [[nodiscard]] static QList<CalDavCollection> collections(
      const CalDavMultiStatusResult& result);
  [[nodiscard]] static QList<CalDavResource> resources(
      const CalDavMultiStatusResult& result);
};

}  // namespace omacalendar::caldav
