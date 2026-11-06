import QtQuick
import PlayerUi

// 在线列表的通用歌曲行：序号/律动 · 标题 + VIP · 歌手·专辑 · 时长 · 播放 · 下载
//
// active = 这一行就是当前在线曲目（含暂停态，用来高亮）；playing = 真的在出声
// （用来决定律动不动、按钮显示播放还是暂停）。
// 强调色不写死，由页面传 Theme.accent 进来 —— 跟着当前播放曲目的封面主色走。
Item {
    id: row
    property int index: 0
    property string title: ""
    property string subtitle: ""
    property string duration: ""
    property bool paywalled: false
    property bool active: false
    property bool playing: false
    property bool pending: false     // 正在解析 / 下载：给转圈，别让人以为点了没反应
    property color accent: Theme.accent
    readonly property color dim: Theme.accentAlpha(0.14)

    signal played()
    signal downloaded()

    height: 46

    Rectangle {
        anchors.fill: parent
        radius: 9
        color: row.active ? Theme.accentAlpha(0.09)
                          : (hover.containsMouse ? Theme.hover : "transparent")
        Behavior on color { ColorAnimation { duration: Theme.fast } }
    }

    // 当前曲目：左缘一道强调色细条，和本地列表的选中语言一致
    Rectangle {
        width: 2
        height: row.active ? 18 : 0
        radius: 1
        x: 1
        anchors.verticalCenter: parent.verticalCenter
        color: row.accent
        opacity: row.active ? 1 : 0
        Behavior on height { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
    }

    Item {
        id: idxBox
        width: 30
        height: parent.height
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.verticalCenter: parent.verticalCenter

        Text {
            anchors.centerIn: parent
            visible: !row.active
            text: row.index + 1
            color: Theme.textTertiary
            font.pixelSize: 11
        }
        // 暂停时停在低位，不空转：一眼能看出"这首放着但停住了"
        Row {
            anchors.centerIn: parent
            spacing: 2
            visible: row.active
            Repeater {
                model: 3
                delegate: Rectangle {
                    id: bar
                    required property int index
                    width: 2
                    radius: 1
                    height: 4
                    color: row.accent
                    SequentialAnimation {
                        running: row.playing
                        loops: Animation.Infinite
                        NumberAnimation {
                            target: bar
                            property: "height"
                            to: 13
                            duration: 300 + index * 90
                            easing.type: Easing.InOutSine
                        }
                        NumberAnimation {
                            target: bar
                            property: "height"
                            to: 4
                            duration: 300 + index * 90
                            easing.type: Easing.InOutSine
                        }
                    }
                }
            }
        }
    }

    Column {
        anchors.left: idxBox.right
        anchors.leftMargin: 4
        anchors.right: dur.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        Row {
            spacing: 6
            width: parent.width
            Text {
                text: row.title
                color: row.active ? row.accent : Theme.text
                font.pixelSize: 13
                elide: Text.ElideRight
                width: Math.min(implicitWidth, parent.width - (badge.visible ? 34 : 0))
                Behavior on color { ColorAnimation { duration: Theme.fast } }
            }
            Rectangle {
                id: badge
                visible: row.paywalled
                width: 26; height: 14; radius: 3
                color: row.dim
                Text {
                    anchors.centerIn: parent
                    text: "VIP"
                    color: row.accent
                    font.pixelSize: 8
                }
            }
        }
        Text {
            width: parent.width
            text: row.subtitle
            color: Theme.textTertiary
            font.pixelSize: 11
            elide: Text.ElideRight
        }
    }

    Text {
        id: dur
        anchors.right: actions.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: row.duration
        color: Theme.textTertiary
        font.pixelSize: 11
    }

    Row {
        id: actions
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        Rectangle {
            width: 28; height: 28; radius: 14
            color: row.pending ? row.dim
                               : (playMouse.containsMouse || row.active ? row.accent : "transparent")
            // 按下时收一点，给一个"按到了"的手感；悬停微放大
            scale: playMouse.pressed ? 0.9 : (playMouse.containsMouse ? 1.06 : 1.0)
            Behavior on color { ColorAnimation { duration: Theme.fast } }
            Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
            Icon {
                anchors.centerIn: parent
                visible: !row.pending
                name: row.playing ? "pause" : "play"
                size: 10
                opacity: playMouse.containsMouse || row.active ? 1 : 0.55
                tint: playMouse.containsMouse || row.active ? Theme.onAccent : Theme.textSecondary
            }
            // 解析中：转圈代替播放键，点下去立刻有反馈
            Icon {
                anchors.centerIn: parent
                visible: row.pending
                name: "spinner"
                size: 11
                tint: row.accent
                RotationAnimator on rotation {
                    running: row.pending
                    loops: Animation.Infinite
                    from: 0
                    to: 360
                    duration: 800
                }
            }
            MouseArea {
                id: playMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (!row.pending) row.played()
            }
        }
        Rectangle {
            width: 28; height: 28; radius: 14
            color: dlMouse.containsMouse ? row.dim : "transparent"
            scale: dlMouse.pressed ? 0.9 : (dlMouse.containsMouse ? 1.06 : 1.0)
            Behavior on color { ColorAnimation { duration: Theme.fast } }
            Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
            Icon {
                anchors.centerIn: parent
                name: "import"
                size: 11
                opacity: dlMouse.containsMouse ? 0.9 : 0.42
                tint: dlMouse.containsMouse ? row.accent : Theme.textSecondary
            }
            MouseArea {
                id: dlMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: row.downloaded()
            }
        }
    }

    MouseArea {
        id: hover
        anchors.fill: parent
        anchors.rightMargin: 70
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }
}
