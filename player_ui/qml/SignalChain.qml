import QtQuick
import PlayerUi

// 信号链面板:从音源到设备的每一个环节。绿色 = 无损直通,琥珀色 = 存在转换 (附原因);独占输出在设置页切换
Rectangle {
    id: panel
    property bool shown: false
    width: 380
    height: col.implicitHeight + 40
    radius: Theme.radiusL
    color: Theme.panel
    border.width: 1
    border.color: Theme.hairline
    opacity: shown ? 1 : 0
    visible: opacity > 0
    scale: shown ? 1 : 0.96
    transformOrigin: Item.BottomRight
    Behavior on opacity { NumberAnimation { duration: Theme.normal } }
    Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }

    Column {
        id: col
        x: 22
        y: 20
        width: parent.width - 44
        spacing: 0

        Row {
            spacing: 10
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 10; height: 10; radius: 5
                color: player.bitPerfect ? Theme.ok : Theme.warn
            }
            Text {
                text: player.bitPerfect ? qsTr("无损直通") : qsTr("输出链路")
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
        }
        Text {
            topPadding: 6
            bottomPadding: 14
            width: parent.width
            wrapMode: Text.WordWrap
            text: player.bitPerfect ? qsTr("送往设备的样本与音源逐位一致。")
                                    : qsTr("以下环节对声音做了转换,琥珀色标出了原因。")
            color: Theme.textTertiary
            font.family: Theme.fontFamily
            font.pixelSize: 12
        }

        Repeater {
            model: player.signalChain
            delegate: Item {
                width: col.width
                height: stageText.implicitHeight + detailText.implicitHeight + 18
                readonly property color tone: modelData.ok ? Theme.ok : Theme.warn
                // 竖向连接线
                Rectangle {
                    x: 5
                    y: 14
                    width: 1
                    height: parent.height - 6
                    color: Theme.hairline
                    visible: index < player.signalChain.length - 1
                }
                Rectangle {
                    x: 1
                    y: 4
                    width: 9; height: 9; radius: 4.5
                    color: "transparent"
                    border.width: 2
                    border.color: tone
                }
                Text {
                    id: stageText
                    x: 24
                    width: parent.width - 24
                    text: modelData.label
                    color: tone
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                Text {
                    id: detailText
                    x: 24
                    anchors.top: stageText.bottom
                    anchors.topMargin: 2
                    width: parent.width - 24
                    wrapMode: Text.WordWrap
                    text: modelData.detail
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                }
            }
        }

    }
}
