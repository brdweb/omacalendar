#pragma once

#include <QString>

namespace omacalendar::paths {

bool isFlatpak();
QString dataDirectory();
QString configDirectory();
QString cacheDirectory();
QString runtimeDirectory();
QString databaseFile();
QString socketFile();
bool ensureDirectories(QString* errorMessage = nullptr);

}  // namespace omacalendar::paths
