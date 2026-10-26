import QtQuick
import PlayerUi

// 隐形标题栏:整条可拖动,双击最大化/还原;右上角极简窗口按钮
Item {
    id: bar
    property var window
    height: 44

    MouseArea {
        // 必须让出右上角按钮区:这块铺满的 MouseArea 一旦收到 pressed 就会调 startSystemMove(),
        // 系统接管鼠标后按钮的 clicked 就被吃掉了 —— 表现就是"按两次才生效"。
        anchors.fill: parent
        anchors.rightMargin: btnRow.width
        acceptedButtons: Qt.LeftButton
        onPressed: bar.window.startSystemMove()
        onDoubleClicked: bar.toggleMaximize()
    }

    // 自己维护最大化状态。
    // 不能用 window.visibility 做实时判断:它在"最大化 -> 还原"时更新滞后,
    // 第一次点击读到的还是旧值,导致要按两次。
    // 也不能读 window.windowState:无边框窗口上它恒为 WindowMaximized。
    // 注意 visibility 在 QML 里是**只读属性**,直接赋值会抛运行时错误(按钮会彻底点不动)。
    property bool maxed: false

    Connections {
        target: bar.window
        ignoreUnknownSignals: true
        function onVisibilityChanged() {
            // 只在两种明确状态下同步;最小化 / 全屏等中间状态不碰
            if (bar.window.visibility === Window.Maximized) bar.maxed = true
            else if (bar.window.visibility === Window.Windowed) bar.maxed = false
        }
    }

    function toggleMaximize() {
        if (!window) return
        if (bar.maxed) {
            window.showNormal()
            bar.maxed = false
        } else {
            window.showMaximized()
            bar.maxed = true
        }
    }

    Row {
        id: btnRow
        // 显式给宽度:拖拽区用 rightMargin: btnRow.width 让位,
        // 如果这里依赖 implicitWidth,布局重算的瞬间可能是 0,拖拽区又会盖回按钮上
        width: 46 * 3
        anchors.right: parent.right
        anchors.top: parent.top
        Repeater {
            model: [
                { icon: "minimize", action: "min" },
                { icon: bar.maxed ? "restore" : "maximize", action: "max" },
                { icon: "close", action: "close" }
            ]
            delegate: Item {
                width: 46
                height: 34
                Rectangle {
                    anchors.fill: parent
                    color: modelData.action === "close" ? "#e5484d" : Theme.hover
                    opacity: hover.containsMouse ? 1 : 0
                    Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                }
                Icon {
                    anchors.centerIn: parent
                    name: modelData.icon
                    size: 14
                    opacity: hover.containsMouse ? 1 : 0.62
                }
                MouseArea {
                    id: hover
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        if (modelData.action === "min") bar.window.showMinimized()
                        else if (modelData.action === "max") bar.toggleMaximize()
                        else bar.window.close()
                    }
                }
            }
        }
    }
}
