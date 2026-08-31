#pragma once

#include <QStringList>
#include <QUrl>

namespace omacalendar {

enum class StartupRequestType {
  None,
  DeepLink,
  IcsImport,
};

struct StartupRequest {
  StartupRequestType type = StartupRequestType::None;
  QUrl url;
};

// Parses QCoreApplication::arguments(), including the executable at index zero.
// Only OmaCalendar deep links and readable, regular local .ics files are routed.
[[nodiscard]] StartupRequest startupRequestFromArguments(const QStringList& arguments);

// Returns a canonical local file URL when the candidate is safe to present to the
// import workflow. Remote URLs, non-files, unreadable files, and other extensions
// are rejected.
[[nodiscard]] QUrl normalizedLocalIcsUrl(const QUrl& candidate);

}  // namespace omacalendar
