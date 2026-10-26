import QtQuick
import PlayerUi

// 岛底边进度线:平时 2px 发光细线 + 光点;鼠标靠近变粗,可点击 / 拖动定位,拖动期间本地预览
Item {
    id: bar
    property real position: 0
    property real duration: 0
    property bool lyricHint: true
    signal seekRequested(real seconds)
    height: 14

    property bool dragging: false
    property real dragValue: 0
    readonly property real ratio: duration > 0 ? Math.min(1, Math.max(0, (dragging ? dragValue : position) / duration)) : 0
    readonly property bool active: mouse.containsMouse || dragging

    function fmt(s) { return Theme.formatTime(s) }

    Rectangle {
        id: track
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 2
        width: parent.width
        height: bar.active ? 4 : 2
        radius: height / 2
        color: Qt.rgba(1, 1, 1, bar.active ? 0.16 : 0.08)
        Behavior on height { NumberAnimation { duration: Theme.fast } }
        Rectangle {
            width: parent.width * bar.ratio
            height: parent.height
            radius: parent.radius
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: Theme.accentAlpha(0.25) }
                GradientStop { position: 1; color: Theme.accent }
            }
        }
    }
    // 进度头部的发光点
    Rectangle {
        width: bar.active ? 12 : 6
        height: width
        radius: width / 2
        color: bar.active ? "#ffffff" : Theme.accent
        x: track.width * bar.ratio - width / 2
        y: track.y + track.height / 2 - height / 2
        visible: bar.duration > 0
        Behavior on width { NumberAnimation { duration: Theme.fast } }
        Rectangle {
            anchors.centerIn: parent
            width: parent.width * 3
            height: width
            radius: width / 2
            color: Theme.accentAlpha(0.22)
            visible: !bar.active
        }
    }
    Rectangle {
        visible: mouse.containsMouse && bar.duration > 0
        x: Math.max(0, Math.min(bar.width - width, mouse.mouseX - width / 2))
        y: -30
        width: Math.min(tip.implicitWidth, 340) + 16
        height: 24
        radius: 7
        color: Theme.tooltip
        // 悬停处的时间,后面跟着那一刻正在唱的歌词 (有同步歌词时)
        readonly property real at: bar.duration * Math.max(0, Math.min(1, mouse.mouseX / bar.width))
        readonly property string line: visible && bar.lyricHint ? player.lyricAt(at) : ""
        Text {
            id: tip
            anchors.centerIn: parent
            width: Math.min(implicitWidth, 340)
            elide: Text.ElideRight
            textFormat: Text.StyledText
            text: bar.fmt(parent.at) + (parent.line.length ? "<font color='#aaa49b'>&nbsp;&nbsp;" + parent.line.replace(/&/g, "&amp;").replace(/</g, "&lt;") + "</font>" : "")
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 11
        }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        anchors.topMargin: -4
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        preventStealing: true
        function valueAt(x) { return bar.duration * Math.max(0, Math.min(1, x / bar.width)) }
        onPressed: (e) => { bar.dragging = true; bar.dragValue = valueAt(e.x) }
        onPositionChanged: (e) => { if (bar.dragging) bar.dragValue = valueAt(e.x) }
        onReleased: (e) => { bar.seekRequested(valueAt(e.x)); bar.dragging = false }
        onCanceled: bar.dragging = false
    }
}
