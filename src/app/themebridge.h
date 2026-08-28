#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QObject>

namespace omacalendar {

class ThemeBridge final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QColor background READ background NOTIFY themeChanged)
  Q_PROPERTY(QColor darkBackground READ darkBackground NOTIFY themeChanged)
  Q_PROPERTY(QColor surface READ surface NOTIFY themeChanged)
  Q_PROPERTY(QColor surfaceAlt READ surfaceAlt NOTIFY themeChanged)
  Q_PROPERTY(QColor text READ text NOTIFY themeChanged)
  Q_PROPERTY(QColor mutedText READ mutedText NOTIFY themeChanged)
  Q_PROPERTY(QColor accent READ accent NOTIFY themeChanged)
  Q_PROPERTY(QColor danger READ danger NOTIFY themeChanged)
  Q_PROPERTY(QColor success READ success NOTIFY themeChanged)
  Q_PROPERTY(int baseFontSize READ baseFontSize NOTIFY themeChanged)
  Q_PROPERTY(QString sourceName READ sourceName NOTIFY themeChanged)

 public:
  explicit ThemeBridge(QObject* parent = nullptr);

  [[nodiscard]] QColor background() const;
  [[nodiscard]] QColor darkBackground() const;
  [[nodiscard]] QColor surface() const;
  [[nodiscard]] QColor surfaceAlt() const;
  [[nodiscard]] QColor text() const;
  [[nodiscard]] QColor mutedText() const;
  [[nodiscard]] QColor accent() const;
  [[nodiscard]] QColor danger() const;
  [[nodiscard]] QColor success() const;
  [[nodiscard]] int baseFontSize() const;
  [[nodiscard]] QString sourceName() const;

 public slots:
  void reload();

 signals:
  void themeChanged();

 private:
  void watch(const QString& path);

  QFileSystemWatcher m_watcher;
  QColor m_background{QStringLiteral("#111318")};
  QColor m_darkBackground{QStringLiteral("#0b0d11")};
  QColor m_surface{QStringLiteral("#191c22")};
  QColor m_surfaceAlt{QStringLiteral("#222630")};
  QColor m_text{QStringLiteral("#eef0f4")};
  QColor m_mutedText{QStringLiteral("#89909d")};
  QColor m_accent{QStringLiteral("#7aa2f7")};
  QColor m_danger{QStringLiteral("#f7768e")};
  QColor m_success{QStringLiteral("#9ece6a")};
  int m_baseFontSize = 14;
  QString m_sourceName = QStringLiteral("fallback");
};

}  // namespace omacalendar
