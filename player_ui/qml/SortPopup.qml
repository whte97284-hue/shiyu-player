import QtQuick
import PlayerUi

// 排序小菜单:贴着「按…」胶囊右下角展开,当前项强调色加勾;点外面或滚轮收起
// 铺满所在页面 (放在列表上层,不被列表裁切)
Item {
    id: pop
    property var options: []
    property int current: 0
    property bool shown: false
    signal picked(int index)
    anchors.fill: parent
    z: 10
    visible: panel.opacity > 0

    function open(anchor) {
        const p = anchor.mapToItem(pop, anchor.width, anchor.height + 6)
        panel.x = Math.max(8, Math.min(p.x - panel.width, pop.width - panel.width - 8))
        panel.y = Math.min(p.y, pop.height - panel.height - 8)
        shown = true
    }
    MouseArea {
        anchors.fill: parent
        enabled: pop.shown
        acceptedButtons: Qt.AllButtons
        onPressed: pop.shown = false
        onWheel: (w) => { pop.shown = false; w.accepted = true }
    }
    Rectangle {
        id: panel
        width: 176
        height: col.implicitHeight + 12
        radius: Theme.radiusM
        color: Theme.panel
        border.width: 1
        border.color: Qt.rgba(1, 0.96, 0.9, 0.09)
        opacity: pop.shown ? 1 : 0
        scale: pop.shown ? 1 : 0.96
        transformOrigin: Item.TopRight
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        Behavior on scale { ScaleAnimator { duration: Theme.fast; easing.type: Easing.OutCubic } }
        Column {
            id: col
            x: 6
            y: 6
            width: parent.width - 12
            Repeater {
                model: pop.options
                Rectangle {
                    readonly property bool on: pop.current === index
                    width: col.width
                    height: 34
                    radius: 8
                    color: rowMouse.containsMouse ? Theme.hover : "transparent"
                    Text {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData
                        color: parent.on ? Theme.accent : Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        font.weight: parent.on ? Font.DemiBold : Font.Normal
                    }
                    Icon {
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        visible: parent.on
                        name: "check"
                        size: 14
                        tint: Theme.accent
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            pop.shown = false
                            pop.picked(index)
                        }
                    }
                }
            }
        }
    }
}
