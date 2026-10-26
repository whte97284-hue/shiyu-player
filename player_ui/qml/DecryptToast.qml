import QtQuick
import PlayerUi

// 后台解密进度胶囊:迷你进度环 + 已完成 / 总数;悬停出现 × 可取消;结束后显示结果片刻
Rectangle {
    id: pill
    property bool shown: decrypt.busy || hold.running
    width: row.implicitWidth + 30
    height: 38
    radius: 19
    color: Theme.panel
    border.width: 1
    border.color: Theme.hairline
    opacity: shown ? 1 : 0
    visible: opacity > 0
    scale: shown ? 1 : 0.9
    Behavior on opacity { NumberAnimation { duration: Theme.normal } }
    Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutBack } }

    Timer { id: hold; interval: 2600 }
    Connections { target: decrypt; function onFinished(ok, failed) { hold.restart() } }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 10
        Item {
            anchors.verticalCenter: parent.verticalCenter
            width: decrypt.busy ? 18 : 0
            height: 18
            visible: decrypt.busy
            ShaderEffect {
                anchors.fill: parent
                visible: decrypt.busy
                property color color: Theme.accent
                property color trackColor: Theme.accentAlpha(0.22)
                property real side: width
                property real arcWidth: 2.5
                property real trackWidth: 1.5
                property real start: 0
                property real span: Math.max(0.03, decrypt.progress)
                Behavior on span { NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }
                fragmentShader: "qrc:/shaders/ring.frag.qsb"
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: decrypt.busy ? qsTr("转换 %1 / %2").arg(decrypt.done).arg(decrypt.total)
                               : qsTr("已转换 %1").arg(decrypt.okCount) + (decrypt.failCount > 0 ? qsTr("  ·  失败 %1").arg(decrypt.failCount) : "")
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
        Capsule {
            anchors.verticalCenter: parent.verticalCenter
            visible: decrypt.busy
            height: 24
            pad: 9
            text: qsTr("取消")
            color: "transparent"
            border.width: 0
            onClicked: decrypt.cancel()
        }
    }
    MouseArea { id: hover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
}
