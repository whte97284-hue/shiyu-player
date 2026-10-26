import QtQuick
import PlayerUi

// 横向专辑带 (歌手详情的专辑、专辑详情页尾「更多这位歌手的专辑」、搜索结果共用)
// model: library.artistAlbums() 返回的列表 [{ index, title, artist, year, cover, badge }]
ListView {
    id: strip
    property int coverSize: 150
    // 第二行写歌手而不是年份 (搜索结果里专辑来自不同歌手);query 为要高亮的搜索词
    property bool showArtist: false
    property string query
    signal open(int index, Item coverItem)
    width: parent ? parent.width : 0
    height: count > 0 ? coverSize + 64 : 0
    visible: count > 0
    orientation: ListView.Horizontal
    spacing: 22
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    SmoothScroll { flickable: strip; horizontal: true }
    delegate: Item {
        width: strip.coverSize
        height: strip.height
        readonly property bool hovered: am.containsMouse
        SoftShadow { target: ac; spread: 18; offsetY: 10; strength: parent.hovered ? 0.9 : 0.5 }
        RoundedImage {
            id: ac
            width: strip.coverSize; height: strip.coverSize
            radius: 8
            y: parent.hovered ? -3 : 0
            Behavior on y { YAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
            source: modelData.cover
            // 按物理像素解码(见 AlbumCard.qml 的说明):固定 1.5 倍在高 DPI 屏上会过度解码
            readonly property real pxScale: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
            decodeSize: Math.ceil(strip.coverSize * Math.max(1.0, Math.min(1.5, pxScale)))
        }
        Text {
            y: strip.coverSize + 10
            width: parent.width
            text: strip.query.length ? Nav.highlight(modelData.title, strip.query, Theme.accent) : modelData.title
            textFormat: strip.query.length ? Text.StyledText : Text.PlainText
            elide: Text.ElideRight
            color: parent.hovered ? Theme.accent : Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }
        Row {
            y: strip.coverSize + 30
            width: parent.width
            spacing: 6
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - (stripBadge.text.length ? stripBadge.width + 6 : 0))
                visible: text.length > 0
                text: (strip.showArtist ? modelData.artist : modelData.year) || ""
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 11
            }
            QualityBadge { id: stripBadge; anchors.verticalCenter: parent.verticalCenter; text: modelData.badge || "" }
        }
        MouseArea {
            id: am
            anchors.fill: parent
            hoverEnabled: !Theme.scrolling
            cursorShape: Qt.PointingHandCursor
            onClicked: strip.open(modelData.index, ac)
        }
    }
}
