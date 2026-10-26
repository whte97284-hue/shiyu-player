import QtQuick
import PlayerUi

// 通用曲目行 (专辑 / 歌曲 / 歌手 / 歌单 / 搜索共用)
//  左:序号或小封面 (正在播放时为律动图标) · 标题 + 歌手 · 专辑列 (宽屏) · 收藏 · 品质 · 时长 · 更多
//  单击播放;右键或「更多」弹出菜单;悬停才显示收藏与更多,平时保持安静
Item {
    id: row
    property real trackId: -1
    property int number
    property string title
    property string artist
    property string albumArtist
    property string album
    property string quality
    property string duration
    property string cover               // 非空时以小封面代替序号
    // 专辑列 / 歌手行做成链接 (点了打开专辑 / 歌手);专辑页自己不设
    property int albumIndex: -1
    property bool artistLink: albumIndex >= 0
    property bool showAlbum: false
    property bool showQuality: true     // 整张专辑音质相同时由专辑页关掉,不逐行重复
    property bool keyFocus: false       // 键盘选中 (上下键)
    property string highlight           // 搜索词高亮
    property var menuContext: ({})
    readonly property bool current: player.hasTrack && player.trackId === trackId
    signal activated()
    // 右键菜单的「播放」与双击同一做法 (在歌单 / 歌手 / 搜索里播放时保留所在列表作为队列)
    function menuCtx() { return Object.assign({ play: () => row.activated() }, row.menuContext) }
    height: cover.length ? 60 : 54

    readonly property bool hovered: mouse.containsMouse || heart.hovered || more.hovered || albumLink.containsMouse || artistArea.containsMouse
    // 歌手链接:第一次悬停 / 按下时才查歌手下标 (-2 = 还没查;重扫后下标会变,按曲库代数重查),找不到的不显示成链接
    property int artistIdx: -2
    property int artistGen: -1
    onArtistChanged: artistIdx = -2
    function resolveArtist() {
        if (artistIdx !== -2 && artistGen === library.generation) return
        artistIdx = library.artistIndexOf(artist)
        artistGen = library.generation
    }
    readonly property bool fav: playlists.revision >= 0 && playlists.isFavorite(trackId)
    readonly property bool wide: width > 760

    // 当前曲目底色 + 悬停底色 (透明度动画在渲染线程执行,快速划过整列也不掉帧);当前行悬停时也略微提亮
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusM
        visible: row.current
        color: Theme.accentAlpha(0.10)
    }
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusM
        color: row.current ? Theme.accentAlpha(0.06) : Theme.hover
        opacity: row.hovered ? 1 : 0
        Behavior on opacity { OpacityAnimator { duration: 120 } }
    }
    // 键盘选中:细描边
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusM
        visible: row.keyFocus
        color: "transparent"
        border.width: 1
        border.color: Theme.accentAlpha(0.5)
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        // 滚动时行从鼠标下掠过,不逐行触发悬停高亮 (浏览器滚动时屏蔽 hover 的同一做法)
        hoverEnabled: !Theme.scrolling
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.PointingHandCursor
        onClicked: (e) => {
            if (e.button === Qt.RightButton) {
                const p = mapToItem(null, e.x, e.y)
                Nav.trackMenu(row.trackId, p.x, p.y, row.menuCtx())
            } else {
                row.activated()
            }
        }
    }

    // ---- 左:序号 / 封面 ----
    Item {
        id: lead
        x: 12
        width: row.cover.length ? 40 : 32
        height: parent.height
        // 序号:当前曲目暂停时也显示序号 (强调色),不再画三个像省略号的小点
        Text {
            anchors.centerIn: parent
            visible: !row.cover.length && !row.hovered && (!row.current || !player.playing)
            text: row.number
            color: row.current ? Theme.accent : Theme.textTertiary
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.weight: row.current ? Font.DemiBold : Font.Normal
        }
        RoundedImage {
            visible: row.cover.length > 0
            anchors.centerIn: parent
            width: 40; height: 40
            radius: 6
            source: row.cover
            decodeSize: 80
        }
        Rectangle {
            visible: row.cover.length > 0 && (row.current || row.hovered)
            anchors.centerIn: parent
            width: 40; height: 40; radius: 6
            color: Qt.rgba(0, 0, 0, 0.45)
        }
        // 悬停:当前曲目显示暂停 / 继续,其他显示播放
        Icon {
            anchors.centerIn: parent
            visible: row.hovered
            name: row.current && player.playing ? "pause" : "play"
            size: 14
        }
        Loader {
            anchors.centerIn: parent
            active: row.current && !row.hovered && (player.playing || row.cover.length > 0)
            sourceComponent: PlayingBars { maxHeight: 14 }
        }
        // 点序号 / 封面位置:当前曲目是暂停 / 继续 (不从头重放),其他曲目照常播放
        MouseArea {
            anchors.fill: parent
            enabled: row.hovered
            cursorShape: Qt.PointingHandCursor
            onClicked: row.current ? player.toggle() : row.activated()
        }
    }

    // ---- 右:收藏 · 品质 · 时长 · 更多 ----
    Row {
        id: meta
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6
        IconButton {
            id: heart
            anchors.verticalCenter: parent.verticalCenter
            width: 32; height: 32
            icon: row.fav ? "heart-filled" : "heart"
            iconTint: row.fav ? Theme.accent : "transparent"
            popOnChange: true
            iconSize: 15
            restOpacity: row.fav ? 0.95 : 0.6
            opacity: row.fav || row.hovered ? 1 : 0
            Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
            onClicked: playlists.toggleFavorite(row.trackId)
        }
        // 品质标签放进定宽槽位,专辑列因此上下对齐
        Item {
            anchors.verticalCenter: parent.verticalCenter
            visible: row.wide && row.showQuality
            width: 64
            height: 20
            QualityBadge {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: row.quality
                opacity: row.hovered || row.current ? 1 : 0.55
            }
        }
        Text {
            width: 52
            horizontalAlignment: Text.AlignRight
            anchors.verticalCenter: parent.verticalCenter
            text: row.duration
            color: Theme.textTertiary
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
        IconButton {
            id: more
            anchors.verticalCenter: parent.verticalCenter
            width: 32; height: 32
            icon: "more"
            iconSize: 16
            opacity: row.hovered ? 1 : 0
            Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
            onClicked: {
                const p = mapToItem(null, width / 2, height)
                Nav.trackMenu(row.trackId, p.x, p.y, row.menuCtx())
            }
        }
    }

    // ---- 中:标题 / 歌手 / 专辑 ----
    Item {
        anchors.left: lead.right
        anchors.leftMargin: 14
        anchors.right: meta.left
        anchors.rightMargin: 12
        height: parent.height

        Column {
            anchors.left: parent.left
            anchors.right: albumText.visible ? albumText.left : parent.right
            anchors.rightMargin: albumText.visible ? 20 : 0
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text {
                width: parent.width
                text: row.highlight.length ? Nav.highlight(row.title, row.highlight, Theme.accent) : row.title
                textFormat: row.highlight.length ? Text.StyledText : Text.PlainText
                elide: Text.ElideRight
                color: row.current ? Theme.accent : Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 14
                font.weight: row.current ? Font.DemiBold : Font.Normal
            }
            Text {
                id: artistText
                readonly property bool linked: row.artistLink && artistArea.containsMouse && row.artistIdx >= 0
                width: parent.width
                visible: text.length > 0
                text: row.artist !== row.albumArtist ? row.artist : ""
                elide: Text.ElideRight
                color: linked ? Theme.textSecondary : Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.underline: linked
                // 只盖住文字本身,点行的其余地方照常播放;找不到歌手时点击落到行上
                MouseArea {
                    id: artistArea
                    width: Math.min(artistText.implicitWidth, artistText.width)
                    height: parent.height
                    enabled: row.artistLink
                    hoverEnabled: !Theme.scrolling
                    cursorShape: Qt.PointingHandCursor
                    onContainsMouseChanged: if (containsMouse) row.resolveArtist()
                    onPressed: (e) => { row.resolveArtist(); e.accepted = row.artistIdx >= 0 }
                    onClicked: Nav.openArtist(row.artistIdx)
                }
            }
        }
        Text {
            id: albumText
            visible: row.showAlbum && row.wide && row.album.length > 0
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(parent.width * 0.36, 300)
            text: row.album
            elide: Text.ElideRight
            color: albumLink.containsMouse ? Theme.textSecondary : Theme.textTertiary
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.underline: albumLink.containsMouse
            MouseArea {
                id: albumLink
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(albumText.implicitWidth, albumText.width)
                height: parent.height + 8
                enabled: row.albumIndex >= 0 && albumText.visible
                hoverEnabled: !Theme.scrolling
                cursorShape: Qt.PointingHandCursor
                onClicked: Nav.openAlbum(row.albumIndex)
            }
        }
    }
}
