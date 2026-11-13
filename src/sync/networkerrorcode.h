#pragma once

#include <QNetworkReply>
#include <QString>

namespace omacalendar {

// The errorCode for a request that failed before any HTTP status arrived.
// The desktop app turns these into advice on what to check, so a missing
// host, a TLS failure, a timeout and being offline stay distinguishable.
[[nodiscard]] inline QString networkErrorCode(const int networkError) {
  switch (static_cast<QNetworkReply::NetworkError>(networkError)) {
    case QNetworkReply::HostNotFoundError:
      return QStringLiteral("host_not_found");
    case QNetworkReply::ConnectionRefusedError:
      return QStringLiteral("connection_refused");
    case QNetworkReply::SslHandshakeFailedError:
      return QStringLiteral("tls_error");
    // Transfer timeouts abort the reply; aborts for oversized responses are
    // reported before this is consulted.
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
      return QStringLiteral("timeout");
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::TemporaryNetworkFailureError:
      return QStringLiteral("network_unavailable");
    default:
      return QStringLiteral("network_error");
  }
}

}  // namespace omacalendar
