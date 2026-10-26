import QtQuick
import PlayerUi

// 封面大图:专辑详情点封面打开,点任意处或 Esc 关闭
Item {
    id: viewer
    anchors.fill: parent
    visible: dim.opacity > 0
    property bool shown: false
    property string source
    property string title
    property string subtitle

    function open(src, t, s) {
        if (!src || !src.length) return
        source = src
        title = t || ""
        subtitle = s || ""
        shown = true
    }
    function close() { shown = false }

    Rectangle {
        id: dim
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.8)
        opacity: viewer.shown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
    }
    MouseArea {
        anchors.fill: parent
        enabled: viewer.shown
        acceptedButtons: Qt.AllButtons
        onClicked: viewer.close()
        onWheel: (w) => w.accepted = true
    }

    Item {
        id: stage
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -24
        readonly property real side: Math.max(120, Math.min(viewer.height - 200, viewer.width - Theme.navWidth - 120))
        width: side
        height: side + 52
        opacity: viewer.shown ? 1 : 0
        scale: viewer.shown ? 1 : 0.96
        Behavior on opacity { OpacityAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
        Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }

        SoftShadow { target: big; spread: 48; offsetY: 24 }
        RoundedImage {
            id: big
            width: stage.side
            height: stage.side
            radius: 12
            // 大图按需加载:关闭后不常驻 1600px 的纹理
            source: viewer.visible ? viewer.source : ""
            decodeSize: 1600
        }
        Column {
            y: stage.side + 16
            width: stage.side
            spacing: 3
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: viewer.title
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: viewer.subtitle
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
        }
    }
}
