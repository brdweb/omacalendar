#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "app/themebridge.h"

using namespace omacalendar;

namespace {

bool writeColors(const QString& path, const QByteArray& colors) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         file.write(colors) == colors.size();
}

}  // namespace

class ThemeBridgeTest final : public QObject {
  Q_OBJECT

 private slots:
  void cleanup() { qunsetenv("OMACALENDAR_OMARCHY_THEME_DIR"); }

  void loadsAndReloadsOmarchyPalette() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString colorsPath = directory.filePath(QStringLiteral("colors.toml"));
    QVERIFY(writeColors(colorsPath, R"TOML(
background = "#20242c"
dark_background = "#12151a"
lighter_background = "#303641"
selection = "#414958"
foreground = "#e9edf4"
light_foreground = "#aab2c0"
accent = "#f2c879"
red = "#ef6b73"
green = "#8fcf8f"
yellow = "#e5b85c"
cyan = "#72c7d6"
)TOML"));
    qputenv("OMACALENDAR_OMARCHY_THEME_DIR", directory.path().toUtf8());

    ThemeBridge bridge;
    QCOMPARE(bridge.sourceName(), QStringLiteral("Omarchy"));
    QCOMPARE(bridge.text(), QColor(QStringLiteral("#e9edf4")));
    QCOMPARE(bridge.accent(), QColor(QStringLiteral("#f2c879")));
    QCOMPARE(bridge.onAccent(), QColor(QStringLiteral("#12151a")));
    QCOMPARE(bridge.warning(), QColor(QStringLiteral("#e5b85c")));
    QCOMPARE(bridge.info(), QColor(QStringLiteral("#72c7d6")));

    QSignalSpy changed(&bridge, &ThemeBridge::themeChanged);
    QVERIFY(writeColors(colorsPath, R"TOML(
background = "#f4f1eb"
dark_background = "#ffffff"
lighter_background = "#e8e3da"
selection = "#d8d0c4"
foreground = "#22252b"
light_foreground = "#5f6570"
accent = "#334f8d"
red = "#a73540"
green = "#287a48"
yellow = "#8a6500"
cyan = "#176f80"
)TOML"));
    QTRY_VERIFY_WITH_TIMEOUT(changed.count() > 0, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(bridge.accent(), QColor(QStringLiteral("#334f8d")), 2000);
    QCOMPARE(bridge.onAccent(), QColor(QStringLiteral("#ffffff")));
    QCOMPARE(bridge.text(), QColor(QStringLiteral("#22252b")));
  }
};

QTEST_GUILESS_MAIN(ThemeBridgeTest)
#include "test_themebridge.moc"
