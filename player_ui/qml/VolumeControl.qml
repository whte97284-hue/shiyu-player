import QtQuick
import PlayerUi

// 音量:图标点击静音/恢复;悬停时向左展开滑条;滚轮微调,调整时上方浮出百分比
Item {
    id: vol
    width: row.width
    height: 34
    // pinned:滑条常驻 (播放岛宽度足够时);否则悬停时向左展开
    property bool pinned: false
    readonly property bool open: pinned || hover.containsMouse || slider.dragging

    Row {
        id: row
        height: parent.height
        spacing: 4
        layoutDirection: Qt.RightToLeft
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            icon: player.volume < 0.01 ? "mute" : "volume"
            iconSize: 17
            width: 34; height: 34
            tip: (player.volume < 0.01 ? qsTr("取消静音") : qsTr("静音")) + "  Ctrl+M"
            onClicked: Nav.toggleMute()
        }
        Item {
            width: vol.open ? 84 : 0
            height: parent.height
            clip: true
            Behavior on width { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
            SeekBar {
                id: slider
                anchors.verticalCenter: parent.verticalCenter
                x: 4
                width: 76
                showTip: false
                position: player.volume
                duration: 1
                onSeekRequested: (v) => player.volume = v
                onDragValueChanged: if (dragging) player.volume = dragValue
            }
        }
    }
    MouseArea {
        id: hover
        anchors.fill: parent
        anchors.margins: -6
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        // 滚轮:每格 ±2%
        property real acc: 0
        onWheel: (w) => {
            acc += w.angleDelta.y
            const steps = Math.trunc(acc / 120)
            if (steps !== 0) { acc -= steps * 120; player.changeVolume(steps * 0.02) }
            w.accepted = true
        }
    }

    // 音量值:滚轮 / 拖动 / 快捷键改变音量后浮出约 0.9 秒
    Connections {
        target: player
        function onVolumeChanged() { if (vol.visible) { valueHold.restart() } }
    }
    Timer { id: valueHold; interval: 900 }
    Rectangle {
        readonly property bool shown: valueHold.running || slider.dragging
        x: vol.open ? 4 + 76 * player.volume - width / 2 + (row.width - 84 - 34 - 4) : (vol.width - width) / 2
        y: -height - 6
        width: valueText.implicitWidth + 14
        height: 22
        radius: 6
        color: Theme.tooltip
        border.width: 1
        border.color: Qt.rgba(1, 0.96, 0.9, 0.08)
        opacity: shown ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        Text {
            id: valueText
            anchors.centerIn: parent
            text: Math.round(player.volume * 100) + "%"
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 11
        }
    }
}
