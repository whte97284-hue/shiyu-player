import QtQuick
import PlayerUi

// 图标按钮的文字提示:悬停 500ms 后浮在按钮上方 (上方放不下时在下方;侧栏图标浮在右侧),移开、按下或页面滚动即收起
// 由 IconButton 经 Nav.hint / Nav.unhint 驱动,全窗口共用一个实例
// 刚显示过提示的短时间内移到相邻按钮,新提示立即出现 (不再重新等待)
Rectangle {
    id: tip
    property Item target: null
    property string text
    property bool shown: false
    property string side: "above"
    property bool warm: false
    z: 200
    width: Math.min(280, label.implicitWidth + 20)
    height: 28
    radius: 7
    color: Theme.tooltip
    border.width: 1
    border.color: Qt.rgba(1, 0.96, 0.9, 0.08)
    opacity: shown ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: Theme.fast } }

    function show(item, t, where) {
        if (!item || !t || !t.length) return
        const same = item === target
        target = item
        text = t
        side = where || "above"
        if (same && shown) { place(); return }
        cool.stop()
        if (warm) { delay.stop(); place(); shown = true; return }
        shown = false
        delay.restart()
    }
    function hide(item) {
        if (item && item !== target) return
        delay.stop()
        if (shown) cool.restart()
        shown = false
    }
    function place() {
        if (!target || !parent) return
        const p = target.mapToItem(parent, 0, 0)
        let px, py
        if (side === "right") {
            px = p.x + target.width + 8
            py = p.y + (target.height - height) / 2
        } else {
            px = p.x + (target.width - width) / 2
            py = p.y - height - 8
            if (py < 8) py = p.y + target.height + 8
        }
        x = Math.round(Math.max(8, Math.min(parent.width - width - 8, px)))
        y = Math.round(Math.max(8, Math.min(parent.height - height - 8, py)))
    }
    onTargetChanged: if (!target) hide(null)
    onWidthChanged: if (shown) place()

    onShownChanged: if (shown) warm = true
    Timer {
        id: delay
        interval: 500
        onTriggered: {
            if (!tip.target || !tip.target.visible) return
            tip.place()
            tip.shown = true
        }
    }
    Timer { id: cool; interval: 300; onTriggered: tip.warm = false }
    // 滚动时按钮跟着内容移动,提示留在原地会错位:直接收起
    Connections {
        target: Theme
        function onScrollingChanged() { if (Theme.scrolling) { tip.hide(null); tip.warm = false } }
    }

    Text {
        id: label
        anchors.centerIn: parent
        width: Math.min(implicitWidth, 260)
        elide: Text.ElideRight
        text: tip.text
        color: Theme.text
        font.family: Theme.fontFamily
        font.pixelSize: 12
    }
}
