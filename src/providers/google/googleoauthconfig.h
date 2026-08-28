#pragma once

#include <QString>

namespace omacalendar::google {

// OAuth client IDs identify an installed application and are public by design.
// Desktop clients authenticate users with Authorization Code + PKCE; no client
// secret is embedded in OmaCalendar.
inline QString defaultOAuthClientId() {
  return QStringLiteral(
      "1099196962170-nd1issemjdkai0c1ag1fel7lflrioqdr.apps.googleusercontent.com");
}

}  // namespace omacalendar::google
