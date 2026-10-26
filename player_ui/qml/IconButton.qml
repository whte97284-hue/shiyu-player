import QtQuick
import PlayerUi

// 圆形图标按钮:悬停出现底色,按下轻微缩小 (渲染线程动画)
Item {
    id: btn
    property string icon
    property int iconSize: 20
    property real restOpacity: 0.78
    property color hoverColor: Theme.hover
    property bool filled: false
    property bool outlined: false   // 次要按钮:细描边 + 极淡底色
    property color fillColor: Theme.accent
    property string tip
    property color iconTint: "transparent"
    readonly property alias hovered: mouse.containsMouse
    // 图标切换时弹跳一下;收藏键不用它 (切歌时状态变化也会弹),而是在点击时调用 popNow()
    property bool popOnChange: false
    function popNow() { if (!Theme.reduced) pop.restart() }
    // 图标切换时旋转交叉淡化 (播放 / 暂停)
    property bool morphOnChange: false
    // 点击时图标朝这个方向轻推一下 (-1 上一首,1 下一首,0 不推)
    property int nudge: 0
    onTipChanged: if (mouse.containsMouse) Nav.hint(btn, tip)
    onIconChanged: {
        if (popOnChange) pop.restart()
        if (morphOnChange && !Theme.reduced) morph.restart()
    }
    signal clicked()

    width: 40
    height: 40

    // 普通按钮:悬停底色从中心「涨」出来,而不是整块突然出现
    Rectangle {
        id: bg
        anchors.fill: parent
        radius: width / 2
        visible: !btn.filled
        color: btn.outlined ? (mouse.containsMouse ? Theme.pressed : Theme.hover) : btn.hoverColor
        border.width: btn.outlined ? 1 : 0
        border.color: Theme.hairline
        opacity: btn.outlined || mouse.containsMouse ? 1 : 0
        scale: btn.outlined || mouse.containsMouse ? 1 : 0.6
        Behavior on opacity { OpacityAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
        Behavior on scale { ScaleAnimator { duration: 320; easing.type: Easing.OutQuint } }
    }
    // 填充按钮:强调色底 + 上半部一层很淡的高光 (像有一点厚度),悬停时整体提亮
    Rectangle {
        anchors.fill: parent
        radius: width / 2
        visible: btn.filled
        color: btn.fillColor
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 0.22) }
                GradientStop { position: 0.5; color: Qt.rgba(1, 1, 1, 0.0) }
                GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.10) }
            }
        }
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "white"
            opacity: mouse.containsMouse ? 0.10 : 0
            Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
        }
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.14)
    }
    // 点击涟漪:一圈强调色从按钮边缘向外扩散并淡去
    Rectangle {
        id: ripple
        anchors.centerIn: parent
        width: parent.width
        height: parent.height
        radius: width / 2
        color: "transparent"
        border.width: 1.5
        border.color: btn.filled ? btn.fillColor : Theme.accentAlpha(0.7)
        opacity: 0
        ParallelAnimation {
            id: rippleAnim
            ScaleAnimator { target: ripple; from: 0.8; to: 1.45; duration: 520; easing.type: Easing.OutCubic }
            OpacityAnimator { target: ripple; from: 0.6; to: 0; duration: 520; easing.type: Easing.OutCubic }
        }
    }
    Icon {
        id: glyph
        anchors.centerIn: parent
        // 填充按钮在浅色强调色上自动切换为深色图标,保证对比度
        name: btn.filled && Theme.accentIsLight ? btn.icon + "-dark" : btn.icon
        size: btn.iconSize
        tint: btn.iconTint
        transform: Translate { id: glyphShift }
        SequentialAnimation {
            id: nudgeAnim
            NumberAnimation { target: glyphShift; property: "x"; to: btn.nudge * 4; duration: 90; easing.type: Easing.OutCubic }
            NumberAnimation { target: glyphShift; property: "x"; to: 0; duration: 420; easing.type: Easing.OutBack; easing.overshoot: 2.2 }
        }
        SequentialAnimation {
            id: morph
            ParallelAnimation {
                ScaleAnimator { target: glyph; from: 0.55; to: 1; duration: 380; easing.type: Easing.OutBack; easing.overshoot: 1.6 }
                RotationAnimator { target: glyph; from: -90; to: 0; duration: 420; easing.type: Easing.OutQuint }
                OpacityAnimator { target: glyph; from: 0; to: 1; duration: 200; easing.type: Easing.OutCubic }
            }
        }
        SequentialAnimation {
            id: pop
            ScaleAnimator { target: glyph; from: 0.6; to: 1.28; duration: 160; easing.type: Easing.OutCubic }
            ScaleAnimator { target: glyph; to: 1; duration: 260; easing.type: Easing.OutBack }
        }
        opacity: btn.filled ? 1 : (mouse.containsMouse ? 1 : btn.restOpacity)
        Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
    }
    // 按下快速收缩,松开带一点回弹地弹回
    scale: mouse.pressed ? 0.88 : 1
    Behavior on scale { ScaleAnimator { duration: mouse.pressed ? 90 : 360; easing.type: mouse.pressed ? Easing.OutCubic : Easing.OutBack; easing.overshoot: 2.0 } }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onContainsMouseChanged: {
            if (!btn.tip.length) return
            if (containsMouse) Nav.hint(btn, btn.tip)
            else Nav.unhint(btn)
        }
        onPressed: if (btn.tip.length) Nav.unhint(btn)
        onClicked: {
            if (!Theme.reduced) {
                rippleAnim.restart()
                if (btn.nudge !== 0) nudgeAnim.restart()
            }
            btn.clicked()
        }
    }
}
