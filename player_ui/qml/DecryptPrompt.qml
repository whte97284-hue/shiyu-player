import QtQuick
import PlayerUi

// 扫描发现需要转换的文件:播放岛上方滑出一条细胶囊 —— 数量 · 格式 · [稍后] [转换]
// 选「稍后」文件留在转换页的列表里待命;不遮挡、不打断
Rectangle {
    id: prompt
    property bool shown: false
    function open() { shown = true }
    function close() { shown = false }

    readonly property var info: decrypt.pending
    width: row.implicitWidth + 12 + 18
    height: 46
    radius: 23
    color: Theme.panel
    border.width: 1
    border.color: Qt.rgba(1, 0.96, 0.9, 0.09)
    opacity: shown ? 1 : 0
    visible: opacity > 0
    transform: Translate {
        y: prompt.shown ? 0 : 16
        Behavior on y { NumberAnimation { duration: 420; easing.type: Easing.OutQuint } }
    }
    Behavior on opacity { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }

    Connections {
        target: decrypt
        function onPendingChanged() { if ((decrypt.pending.total || 0) === 0) prompt.close() }
    }

    Row {
        id: row
        x: 18
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("%1 首需要转换").arg((prompt.info.total || 0).toLocaleString(Qt.locale(), "f", 0))
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: (prompt.info.kinds || []).map(x => x.id.toUpperCase() + " " + x.count).join("  ·  ")
            color: Theme.textTertiary
            font.family: Theme.fontFamily
            font.pixelSize: 12
            rightPadding: 6
        }
        Capsule {
            anchors.verticalCenter: parent.verticalCenter
            height: 32
            text: qsTr("稍后")
            onClicked: { decrypt.dismissPending(); prompt.close() }
        }
        Capsule {
            anchors.verticalCenter: parent.verticalCenter
            height: 32
            filled: true
            text: qsTr("转换")
            onClicked: { decrypt.decryptPending(); prompt.close() }
        }
    }
}
