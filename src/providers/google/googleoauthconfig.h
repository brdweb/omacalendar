#pragma once

#include <QString>
#include <QtGlobal>

#include "providers/google/googleoauthdeployment.h"

namespace omacalendar::google {

// OAuth application credentials are deployment configuration, never source
// material. Official release binaries receive them through a generated build
// header; development builds can use runtime overrides or the credentials JSON
// picker. User refresh tokens remain in Secret Service.
inline QString defaultOAuthClientId() {
  if (qEnvironmentVariableIsSet("OMACALENDAR_GOOGLE_CLIENT_ID")) {
    return qEnvironmentVariable("OMACALENDAR_GOOGLE_CLIENT_ID").trimmed();
  }
  return QString::fromUtf8(deployment::kClientId).trimmed();
}

inline QString defaultOAuthClientSecret() {
  // Setting either runtime variable selects a complete runtime override. This
  // prevents a custom client ID from being paired with the packaged client's
  // shared key when the custom desktop client legitimately uses no secret.
  if (qEnvironmentVariableIsSet("OMACALENDAR_GOOGLE_CLIENT_ID") ||
      qEnvironmentVariableIsSet("OMACALENDAR_GOOGLE_CLIENT_SECRET")) {
    return qEnvironmentVariable("OMACALENDAR_GOOGLE_CLIENT_SECRET");
  }
  return QString::fromUtf8(deployment::kClientSecret);
}

}  // namespace omacalendar::google
