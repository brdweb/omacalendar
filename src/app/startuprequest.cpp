#include "startuprequest.h"

#include <QFileInfo>

namespace omacalendar {

QUrl normalizedLocalIcsUrl(const QUrl& candidate) {
  if (!candidate.isValid() || !candidate.isLocalFile() || candidate.hasQuery() ||
      candidate.hasFragment()) {
    return {};
  }

  const QString host = candidate.host();
  if (!host.isEmpty() &&
      host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) != 0) {
    return {};
  }

  const QFileInfo file(candidate.toLocalFile());
  if (!file.exists() || !file.isFile() || !file.isReadable() ||
      file.suffix().compare(QStringLiteral("ics"), Qt::CaseInsensitive) != 0) {
    return {};
  }

  const QString canonicalPath = file.canonicalFilePath();
  return canonicalPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(canonicalPath);
}

StartupRequest startupRequestFromArguments(const QStringList& arguments) {
  for (qsizetype index = 1; index < arguments.size(); ++index) {
    const QString argument = arguments.at(index);
    const QUrl parsed(argument, QUrl::StrictMode);
    if (parsed.isValid() && parsed.scheme().compare(QStringLiteral("omacalendar"),
                                                    Qt::CaseInsensitive) == 0) {
      return {StartupRequestType::DeepLink, parsed};
    }

    QUrl localCandidate;
    if (parsed.isValid() &&
        parsed.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0) {
      localCandidate = parsed;
    } else if (!argument.trimmed().isEmpty() &&
               !argument.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive) &&
               (!parsed.isValid() || parsed.scheme().isEmpty())) {
      localCandidate = QUrl::fromLocalFile(QFileInfo(argument).absoluteFilePath());
    }

    const QUrl normalized = normalizedLocalIcsUrl(localCandidate);
    if (normalized.isValid()) {
      return {StartupRequestType::IcsImport, normalized};
    }
  }

  return {};
}

}  // namespace omacalendar
