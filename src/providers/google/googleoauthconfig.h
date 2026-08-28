#pragma once

#include <QString>

namespace omacalendar::google {

// Installed desktop clients are public OAuth clients: distributed binaries
// cannot keep application credentials confidential. Google requires the
// generated desktop key for token exchange, while PKCE protects each user's
// authorization code. User refresh tokens remain in Secret Service.
inline QString defaultOAuthClientId() {
  return QStringLiteral(
      "1099196962170-nd1issemjdkai0c1ag1fel7lflrioqdr.apps.googleusercontent.com");
}

inline QString defaultOAuthClientSecret() {
  return QStringLiteral("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");
}

}  // namespace omacalendar::google
