import QtQuick
import PlayerUi

// 胶囊:全应用统一的容器形态。浅底 + 细描边;accent = 选中 / 主要;filled = 实心主按钮
// 悬停提亮、按下轻收,过渡都在渲染线程
Rectangle {
    id: cap
    property string text
    property bool accent: false
    property bool filled: false
    property bool interactive: true
    property int pad: 14
    property int trailing: 0   // 右侧额外留白 (放下拉箭头等),文字随之左移
    readonly property bool hovered: area.containsMouse
    readonly property alias pressed: area.pressed
    signal clicked()

    implicitWidth: label.implicitWidth + pad * 2 + trailing
    implicitHeight: 32
    width: implicitWidth
    height: implicitHeight
    radius: height / 2
    // 未选中:极淡暖色底、无描边;强调态:淡强调色底;实心:强调色
    color: filled ? Theme.accent : accent ? Theme.accentAlpha(0.16) : Qt.rgba(1, 0.96, 0.9, 0.05)
    border.width: 0
    scale: area.pressed ? 0.96 : 1
    Behavior on scale { ScaleAnimator { duration: Theme.fast; easing.type: Easing.OutCubic } }

    // 悬停高光层 (不改底色,避免逐帧颜色插值)
    Rectangle {
        anchors.fill: parent
        radius: parent.radius
        color: cap.filled ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(1, 0.96, 0.9, 0.05)
        opacity: cap.interactive && area.containsMouse ? 1 : 0
        Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
    }
    Text {
        id: label
        anchors.centerIn: parent
        anchors.horizontalCenterOffset: -cap.trailing / 2
        text: cap.text
        color: cap.filled ? Theme.onAccent : cap.accent ? Theme.accent : Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: 13
        font.weight: cap.filled || cap.accent ? Font.DemiBold : Font.Normal
    }
    MouseArea {
        id: area
        anchors.fill: parent
        enabled: cap.interactive
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: cap.clicked()
    }
}
