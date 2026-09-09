#include "core/paths.h"

#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace omacalendar::paths {
namespace {

QString appSubdirectory(const QStandardPaths::StandardLocation location) {
  return QDir(QStandardPaths::writableLocation(location))
      .filePath(QStringLiteral("omacalendar"));
}

}  // namespace

bool isFlatpak() {
  return qEnvironmentVariable("FLATPAK_ID") ==
         QStringLiteral("org.omacalendar.OmaCalendar");
}

QString dataDirectory() { return appSubdirectory(QStandardPaths::GenericDataLocation); }

QString configDirectory() {
  return appSubdirectory(QStandardPaths::GenericConfigLocation);
}

QString cacheDirectory() {
  return appSubdirectory(QStandardPaths::GenericCacheLocation);
}

QString runtimeDirectory() {
  const QString xdgRuntime = qEnvironmentVariable("XDG_RUNTIME_DIR");
  if (!xdgRuntime.isEmpty()) {
    // Flatpak shares this app-specific directory between sandbox instances.
    // Keep XDG_RUNTIME_DIR itself intact for Wayland and desktop portals, and
    // never share the native daemon or desktop activation endpoints.
    return QDir(xdgRuntime)
        .filePath(isFlatpak()
                      ? QStringLiteral("app/org.omacalendar.OmaCalendar/omacalendar")
                      : QStringLiteral("omacalendar"));
  }
  return QDir(QDir::tempPath())
      .filePath((isFlatpak() ? QStringLiteral("omacalendar-flatpak-%1")
                             : QStringLiteral("omacalendar-%1"))
                    .arg(QString::number(getuid())));
}

QString databaseFile() {
  return QDir(dataDirectory()).filePath(QStringLiteral("calendar.sqlite3"));
}

QString socketFile() {
  return QDir(runtimeDirectory()).filePath(QStringLiteral("daemon.sock"));
}

bool ensureDirectories(QString* errorMessage) {
  const QStringList directories = {dataDirectory(), configDirectory(), cacheDirectory(),
                                   runtimeDirectory()};
  for (const QString& directory : directories) {
    QDir dir;
    if (!dir.mkpath(directory)) {
      if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("Could not create %1").arg(directory);
      }
      return false;
    }
    if (!QFile::setPermissions(directory, QFileDevice::ReadOwner |
                                              QFileDevice::WriteOwner |
                                              QFileDevice::ExeOwner)) {
      if (errorMessage != nullptr) {
        *errorMessage =
            QStringLiteral("Could not restrict permissions on %1").arg(directory);
      }
      return false;
    }
  }
  return true;
}

}  // namespace omacalendar::paths
