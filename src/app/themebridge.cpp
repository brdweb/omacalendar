#include "themebridge.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

namespace omacalendar {
namespace {

QColor colorValue(const QString& text, const QString& key, const QColor& fallback) {
  const QRegularExpression expression(
      QStringLiteral("(?m)^\\s*%1\\s*=\\s*[\\\"'](#[0-9A-Fa-f]{6,8})[\\\"']")
          .arg(QRegularExpression::escape(key)));
  const QRegularExpressionMatch match = expression.match(text);
  if (!match.hasMatch()) {
    return fallback;
  }
  const QColor candidate(match.captured(1));
  return candidate.isValid() ? candidate : fallback;
}

QString readText(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return {};
  }
  return QString::fromUtf8(file.readAll());
}

QString stateHome() {
  const QString configured = qEnvironmentVariable("XDG_STATE_HOME");
  if (!configured.isEmpty()) {
    return configured;
  }
  return QDir::home().filePath(QStringLiteral(".local/state"));
}

double relativeLuminance(const QColor& color) {
  const auto channel = [](const double value) {
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) +
         0.0722 * channel(color.blueF());
}

double contrastRatio(const QColor& first, const QColor& second) {
  const double firstLuminance = relativeLuminance(first);
  const double secondLuminance = relativeLuminance(second);
  const double lighter = std::max(firstLuminance, secondLuminance);
  const double darker = std::min(firstLuminance, secondLuminance);
  return (lighter + 0.05) / (darker + 0.05);
}

QColor bestContrast(const QColor& background, const QColor& first,
                    const QColor& second) {
  return contrastRatio(background, first) >= contrastRatio(background, second) ? first
                                                                               : second;
}

}  // namespace

ThemeBridge::ThemeBridge(QObject* parent) : QObject(parent) {
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &ThemeBridge::reload);
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
          &ThemeBridge::reload);
  reload();
}

QColor ThemeBridge::background() const { return m_background; }
QColor ThemeBridge::darkBackground() const { return m_darkBackground; }
QColor ThemeBridge::surface() const { return m_surface; }
QColor ThemeBridge::surfaceAlt() const { return m_surfaceAlt; }
QColor ThemeBridge::text() const { return m_text; }
QColor ThemeBridge::mutedText() const { return m_mutedText; }
QColor ThemeBridge::accent() const { return m_accent; }
QColor ThemeBridge::onAccent() const { return m_onAccent; }
QColor ThemeBridge::danger() const { return m_danger; }
QColor ThemeBridge::success() const { return m_success; }
QColor ThemeBridge::warning() const { return m_warning; }
QColor ThemeBridge::info() const { return m_info; }
int ThemeBridge::baseFontSize() const { return m_baseFontSize; }
QString ThemeBridge::sourceName() const { return m_sourceName; }

void ThemeBridge::watch(const QString& path) {
  if (!path.isEmpty() && QFile::exists(path) && !m_watcher.files().contains(path) &&
      !m_watcher.directories().contains(path)) {
    m_watcher.addPath(path);
  }
}

void ThemeBridge::reload() {
  const QString configuredThemeDirectory =
      qEnvironmentVariable("OMACALENDAR_OMARCHY_THEME_DIR");
  const QString currentDirectory =
      QDir(stateHome()).filePath(QStringLiteral("omarchy/current"));
  const QString themeDirectory =
      configuredThemeDirectory.isEmpty()
          ? QDir(currentDirectory).filePath(QStringLiteral("theme"))
          : configuredThemeDirectory;
  const QString colorsPath =
      QDir(themeDirectory).filePath(QStringLiteral("colors.toml"));
  const QString themeShellPath =
      QDir(themeDirectory).filePath(QStringLiteral("shell.toml"));
  const QString userShellPath =
      QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
          .filePath(QStringLiteral("omarchy/shell.toml"));
  const QString colors = readText(colorsPath);

  if (!colors.isEmpty()) {
    m_background = colorValue(colors, QStringLiteral("background"), m_background);
    m_darkBackground =
        colorValue(colors, QStringLiteral("dark_background"), m_background);
    m_surface = colorValue(colors, QStringLiteral("lighter_background"), m_surface);
    m_surfaceAlt = colorValue(colors, QStringLiteral("selection"), m_surfaceAlt);
    m_text = colorValue(colors, QStringLiteral("foreground"), m_text);
    m_mutedText = colorValue(colors, QStringLiteral("light_foreground"), m_mutedText);
    m_accent = colorValue(colors, QStringLiteral("accent"), m_accent);
    m_danger = colorValue(colors, QStringLiteral("red"), m_danger);
    m_success = colorValue(colors, QStringLiteral("green"), m_success);
    m_warning = colorValue(colors, QStringLiteral("yellow"), m_warning);
    m_info = colorValue(colors, QStringLiteral("cyan"), m_info);
    m_onAccent = bestContrast(m_accent, m_darkBackground, m_text);
    m_sourceName = QStringLiteral("Omarchy");
  } else {
    m_sourceName = QStringLiteral("fallback");
  }

  const QString shell =
      readText(themeShellPath) + QLatin1Char('\n') + readText(userShellPath);
  const QRegularExpression fontExpression(
      QStringLiteral("(?ms)^\\s*\\[font\\].*?^\\s*base-size\\s*=\\s*(\\d+)"));
  const QRegularExpressionMatch fontMatch = fontExpression.match(shell);
  if (fontMatch.hasMatch()) {
    m_baseFontSize = qBound(10, fontMatch.captured(1).toInt(), 28);
  }

  const QStringList watchedFiles = m_watcher.files();
  if (!watchedFiles.isEmpty()) {
    m_watcher.removePaths(watchedFiles);
  }
  const QStringList watchedDirectories = m_watcher.directories();
  if (!watchedDirectories.isEmpty()) {
    m_watcher.removePaths(watchedDirectories);
  }
  if (configuredThemeDirectory.isEmpty()) {
    watch(currentDirectory);
  }
  watch(themeDirectory);
  watch(colorsPath);
  watch(themeShellPath);
  watch(userShellPath);
  watch(QFileInfo(userShellPath).absolutePath());
  emit themeChanged();
}

}  // namespace omacalendar
