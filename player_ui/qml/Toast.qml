import QtQuick
import PlayerUi

// 轻提示:一句话,1.8 秒后淡出。带操作时 (如「撤销」) 右侧多一枚胶囊按钮、停留 4 秒,悬停时不消失;
// warn = 出错提示 (播放失败等),句首一个警示色圆点,停留 3.5 秒
Rectangle {
    id: toast
    property string icon
    property string text
    property bool warn: false
    property string actionText
    property var action: null
    readonly property bool shown: hold.running || (hover.hovered && actionText.length > 0 && hold.wasShown)

    function show(i, t) { present(i, t, false, "", null) }
    function showWarn(t) { present("", t, true, "", null) }
    function showAction(t, label, fn) { present("", t, false, label, fn) }
    function present(i, t, w, label, fn) {
        icon = i || ""
        text = t
        warn = w
        actionText = label || ""
        action = fn || null
        hold.interval = actionText.length ? 4000 : w ? 3500 : 1800
        hold.wasShown = true
        hold.restart()
    }
    function dismiss() { hold.stop(); hold.wasShown = false }

    width: row.implicitWidth + (actionText.length ? 24 : 36)
    height: 40
    radius: 20
    color: Theme.panel
    border.width: 1
    border.color: Qt.rgba(1, 0.96, 0.9, 0.09)
    opacity: shown ? 1 : 0
    visible: opacity > 0
    scale: shown ? 1 : 0.94
    Behavior on opacity { NumberAnimation { duration: Theme.normal } }
    Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
    Behavior on width { enabled: toast.shown; NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
    Timer {
        id: hold
        interval: 1800
        property bool wasShown: false
        // 悬停在带操作的提示上时计时暂停,移开后重新计时
        onTriggered: if (!(hover.hovered && toast.actionText.length)) wasShown = false
    }
    HoverHandler {
        id: hover
        enabled: toast.actionText.length > 0
        onHoveredChanged: if (!hovered && toast.actionText.length && hold.wasShown) hold.restart()
    }

    Row {
        id: row
        x: 18
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10
        Rectangle {
            visible: toast.warn
            anchors.verticalCenter: parent.verticalCenter
            width: 6; height: 6; radius: 3
            color: Theme.warn
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, 520)
            elide: Text.ElideRight
            text: toast.text
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
        Capsule {
            visible: toast.actionText.length > 0
            anchors.verticalCenter: parent.verticalCenter
            height: 28
            pad: 12
            accent: true
            text: toast.actionText
            onClicked: {
                const fn = toast.action
                toast.dismiss()
                if (typeof fn === "function") fn()
            }
        }
    }
}
