import QtQuick
import PlayerUi

// 歌单封面:4 张以上专辑拼成 2×2,否则取第一张;空歌单为强调色底 + 图标
Item {
    id: mosaic
    property var covers: []
    property bool favorite: false
    property real radius: 10
    property int decodeSize: 300

    Rectangle {
        anchors.fill: parent
        visible: mosaic.covers.length === 0 || mosaic.favorite
        radius: mosaic.radius
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.accentAlpha(mosaic.favorite ? 0.55 : 0.18) }
            GradientStop { position: 1; color: Theme.accentAlpha(mosaic.favorite ? 0.18 : 0.06) }
        }
        border.width: 1
        border.color: Theme.hairline
        Icon {
            anchors.centerIn: parent
            name: mosaic.favorite ? "heart-filled" : "playlist"
            size: Math.round(mosaic.width * 0.28)
            opacity: mosaic.favorite ? 0.92 : 0.4
        }
    }
    RoundedImage {
        anchors.fill: parent
        visible: !mosaic.favorite && mosaic.covers.length > 0 && mosaic.covers.length < 4
        radius: mosaic.radius
        source: visible ? mosaic.covers[0] : ""
        decodeSize: mosaic.decodeSize
    }
    // 2×2:只圆外侧的角,中间留 2px 细缝,整体读作一张圆角方图
    Rectangle {
        anchors.fill: parent
        visible: !mosaic.favorite && mosaic.covers.length >= 4
        radius: mosaic.radius
        color: Qt.rgba(1, 1, 1, 0.05)
    }
    Grid {
        anchors.fill: parent
        visible: !mosaic.favorite && mosaic.covers.length >= 4
        columns: 2
        spacing: 2
        Repeater {
            model: parent.visible ? 4 : 0
            RoundedImage {
                width: (mosaic.width - 2) / 2
                height: width
                radius: mosaic.radius
                corners: [Qt.vector4d(1, 0, 0, 0), Qt.vector4d(0, 1, 0, 0), Qt.vector4d(0, 0, 0, 1), Qt.vector4d(0, 0, 1, 0)][index]
                outline: false
                placeholder: false
                source: mosaic.covers[index]
                decodeSize: mosaic.decodeSize / 2
            }
        }
    }
    Rectangle {
        anchors.fill: parent
        visible: !mosaic.favorite && mosaic.covers.length >= 4
        radius: mosaic.radius
        color: "transparent"
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.07)
    }
}
