#pragma once

#include <QString>
#include <QtGlobal>

namespace omacalendar::google {

// OAuth application credentials are deployment configuration, never source
// material. Official packages can inject them into the launch environment;
// development builds can instead use the desktop-credentials JSON picker.
// User refresh tokens and any optional client secret remain in Secret Service.
inline QString defaultOAuthClientId() {
  return qEnvironmentVariable("OMACALENDAR_GOOGLE_CLIENT_ID").trimmed();
}

inline QString defaultOAuthClientSecret() {
  return qEnvironmentVariable("OMACALENDAR_GOOGLE_CLIENT_SECRET");
}

}  // namespace omacalendar::google
