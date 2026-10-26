import QtQuick
import PlayerUi

// 进度条:平时 3px 细线,悬停加粗并出现拖动点与时间提示;拖动期间本地预览,松手才 seek
Item {
    id: bar
    property real position: 0
    property real duration: 0
    property bool showTip: true
    property bool lyricHint: true
    signal seekRequested(real seconds)
    height: 24

    property bool dragging: false
    property real dragValue: 0
    readonly property real ratio: duration > 0 ? Math.min(1, Math.max(0, (dragging ? dragValue : position) / duration)) : 0
    readonly property bool active: mouse.containsMouse || dragging

    function fmt(s) { return Theme.formatTime(s) }

    Rectangle {
        id: track
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        height: bar.active ? 6 : 3
        radius: height / 2
        color: Qt.rgba(1, 1, 1, 0.14)
        Behavior on height { NumberAnimation { duration: Theme.fast } }
        Rectangle {
            width: parent.width * bar.ratio
            height: parent.height
            radius: parent.radius
            color: Theme.accent
        }
    }
    Rectangle {
        width: 14; height: 14; radius: 7
        color: "#fff"
        x: track.width * bar.ratio - width / 2
        anchors.verticalCenter: parent.verticalCenter
        scale: bar.active ? 1 : 0
        Behavior on scale { ScaleAnimator { duration: Theme.fast; easing.type: Easing.OutBack } }
    }
    // 悬停时间提示
    Rectangle {
        visible: bar.showTip && mouse.containsMouse && bar.duration > 0
        x: Math.max(0, Math.min(bar.width - width, mouse.mouseX - width / 2))
        y: -32
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
        anchors.topMargin: -6
        anchors.bottomMargin: -6
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
