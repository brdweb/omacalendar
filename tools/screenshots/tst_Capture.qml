import QtCore
import QtQuick
import QtTest
import OmaCalendar
import "../../src/app/qml" as Production

Item {
    id: scene
    width: 1440
    height: 900
    property var appSceneRoot: null

    Production.Main {
        id: appWindow
        width: 1440
        height: 900
        visible: true
    }

    TestCase {
        name: "ReleaseScreenshots"
        when: windowShown

        function initTestCase() {
            scene.appSceneRoot = appWindow.contentItem.parent
            verify(scene.appSceneRoot !== null, "Application scene root found")
        }

        function outputPath(fileName) {
            const cacheUrl = String(StandardPaths.writableLocation(
                                        StandardPaths.GenericCacheLocation))
            const cachePath = cacheUrl.indexOf("file://") === 0
                    ? decodeURIComponent(cacheUrl.slice(7)) : cacheUrl
            return cachePath + "/omacalendar-screenshots/" + fileName
        }

        function capture(window, fileName) {
            // ApplicationWindow.contentItem excludes the 68 px header. Its
            // visual parent is the window's scene root and includes headers,
            // content, footer, overlays, and modal popups.
            compare(window.contentItem.parent, scene.appSceneRoot)
            const image = grabImage(scene.appSceneRoot)
            compare(image.width, 1440)
            compare(image.height, 900)
            image.save(outputPath(fileName))
        }

        function positionWeekTimeline(window) {
            const scroll = findChild(window.contentItem.parent,
                                     "weekTimelineScroll")
            verify(scroll !== null, "Week timeline scroll area found")
            // The normal workday jump places the first hour label exactly on
            // the clip boundary. Give the public overview half an hour of
            // visual breathing room without changing production QML.
            scroll.contentY = Math.max(0, scroll.contentY - 32)
            wait(150)
        }

        function test_capture_release_images() {
            wait(700)

            appWindow.currentView = "month"
            wait(350)
            capture(appWindow, "desktop-month.png")

            appWindow.currentView = "week"
            wait(500)
            positionWeekTimeline(appWindow)
            capture(appWindow, "desktop-week.png")

            appWindow.currentView = "agenda"
            wait(350)
            capture(appWindow, "desktop-agenda.png")

            appWindow.currentView = "week"
            wait(250)
            positionWeekTimeline(appWindow)
            appWindow.openEvent(App.events[5])
            wait(500)
            capture(appWindow, "event-editor.png")
        }
    }
}
