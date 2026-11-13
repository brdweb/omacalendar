#pragma once

#include <QStringList>
#include <QUrl>

namespace omacalendar {

enum class StartupRequestType {
  None,
  DeepLink,
  IcsImport,
  // A webcal:// or webcals:// link. It only prefills the subscription form;
  // nothing is subscribed until the person confirms.
  IcsSubscribe,
};

struct StartupRequest {
  StartupRequestType type = StartupRequestType::None;
  QUrl url;
};

// Parses QCoreApplication::arguments(), including the executable at index zero.
// Only OmaCalendar deep links, webcal(s) subscription links and readable,
// regular local .ics files are routed. Plain http(s) URLs are never claimed.
[[nodiscard]] StartupRequest startupRequestFromArguments(const QStringList& arguments);

// Returns a canonical local file URL when the candidate is safe to present to the
// import workflow. Remote URLs, non-files, unreadable files, and other extensions
// are rejected.
[[nodiscard]] QUrl normalizedLocalIcsUrl(const QUrl& candidate);

// Returns the link unchanged when it is a valid webcal:// or webcals:// URL with
// a host and no credentials; otherwise an invalid URL.
[[nodiscard]] QUrl normalizedWebcalUrl(const QUrl& candidate);

}  // namespace omacalendar
