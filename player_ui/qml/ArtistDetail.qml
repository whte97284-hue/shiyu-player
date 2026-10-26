import QtQuick
import PlayerUi

// 歌手详情:圆形大头像 + 名字 + 播放 / 随机 / 更多;横向专辑带;全部曲目
// 头部尺寸与专辑详情一致;滚过头部后顶端浮出精简标题栏
Item {
    id: page
    // 内容滚到窗口按钮下面了:Main 据此在顶端铺一条磨砂条 (精简标题栏出现后由它自己盖住顶端)
    readonly property bool scrolled: list.contentY - list.originY > 8 && !bar.shown
    objectName: "artistDetailPage"
    property int bottomInset: 140
    property int artistIndex: -1
    signal back()
    readonly property int sidePadding: Theme.pagePadding(width)
    readonly property Item heroAvatar: list.headerItem ? list.headerItem.avatarItem : null
    readonly property var info: library.generation >= 0 ? library.artistInfo(artistIndex) : ({})
    readonly property var albums: library.generation >= 0 ? library.artistAlbums(artistIndex) : []
    // 当前播放队列就是这位歌手 (从歌手页、歌手墙或搜索点的播放):播放键是暂停 / 继续
    readonly property string queueTag: "artist:" + (info.name || "")
    readonly property bool isCurrent: !!info.name && player.queueTag === queueTag

    onArtistIndexChanged: {
        artistTracks.artistIndex = artistIndex
        list.userScrolled = false
        Qt.callLater(list.positionViewAtBeginning)
    }

    function play(shuffled) {
        if (artistTracks.count === 0) return
        player.shuffle = shuffled
        shuffled ? player.playTracks(artistTracks.trackIds(), Math.floor(Math.random() * artistTracks.count), queueTag)
                 : Nav.picked(player.pickTracks(artistTracks.trackIds(), 0, queueTag, true))
    }
    function playOrToggle() { if (isCurrent) player.toggle(); else play(false) }
    function keyNav(e) {
        return Nav.listKeys(list, e, isCurrent ? artistTracks.rowOfTrack(player.trackId) : -1, bottomInset - 20,
                            (i) => Nav.picked(player.pickTracks(artistTracks.trackIds(), i, queueTag)),
                            (i, x, y) => { const it = list.currentItem; if (it) Nav.trackMenu(it.trackId, x, y, it.menuCtx()) })
    }
    Connections {
        target: artistTracks
        function onContentChanged() { list.currentIndex = -1 }
    }
    function openMenu(item, px, py) {
        const p = item.mapToItem(null, px, py)
        Nav.trackMenu(-1, p.x, p.y, { ids: artistTracks.trackIds(), kind: "artist", title: info.name || "",
                                      artistIndex: artistIndex, tag: queueTag, fromDetail: true })
    }
    // 截图验证用:滚到指定位置 (< 0 滚到底)
    function automationScroll(y) {
        list.userScrolled = true
        const maxY = list.originY + list.contentHeight + list.bottomMargin - list.height
        list.contentY = y < 0 ? maxY : Math.min(maxY, list.originY + y)
    }
    function automationMenu() { if (list.headerItem) list.headerItem.openMore() }

    // ---- 头部底色:代表专辑的封面模糊铺在头部后面,往下渐隐,随列表一起滚走 ----
    Item {
        id: wash
        width: page.width
        height: list.headerItem ? list.headerItem.heroHeight + 180 : 0
        y: list.headerItem ? list.headerItem.y - list.contentY : 0
        visible: y + height > 0 && !!page.info.cover
        opacity: page.isCurrent ? 0.38 : 0.72
        Behavior on opacity { NumberAnimation { duration: Theme.slow } }
        layer.enabled: true
        layer.effect: ShaderEffect {
            property real fadeTop: 0.001
            property real fadeBottom: 0.75
            fragmentShader: "qrc:/shaders/fade.frag.qsb"
        }
        Backdrop {
            anchors.fill: parent
            source: page.info.cover || ""
            solid: false
            dim: 0.55
            spread: 0.2
            sampleSize: 32
        }
    }

    // 鼠标真的移动了才取消键盘选中 (键盘翻动列表时行从静止的鼠标下经过,不算)
    HoverHandler {
        property point last
        property bool seen: false
        onPointChanged: {
            const p = point.scenePosition
            if (!seen || Math.abs(p.x - last.x) + Math.abs(p.y - last.y) < 3) { seen = true; last = p; return }
            last = p
            if (list.currentIndex >= 0) list.currentIndex = -1
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.leftMargin: page.sidePadding
        anchors.rightMargin: page.sidePadding
        model: artistTracks
        currentIndex: -1
        keyNavigationEnabled: false
        highlightFollowsCurrentItem: false
        boundsBehavior: Flickable.StopAtBounds
        bottomMargin: page.bottomInset
        cacheBuffer: 600
        reuseItems: true
        SmoothScroll { flickable: list; onScrolled: list.userScrolled = true }
        // 头部内容 (专辑带、封面) 异步就绪后高度会变:用户还没滚动时始终停在顶部
        property bool userScrolled: false
        onMovementStarted: userScrolled = true
        onCountChanged: if (!userScrolled) Qt.callLater(positionViewAtBeginning)

        header: Column {
            id: head
            readonly property Item avatarItem: avatarImg
            // 大标题区 (返回键 + 头像行) 的高度,精简栏与底色按它计算
            readonly property real heroHeight: 102 + avatar.height + 46
            function openMore() { page.openMenu(moreBtn, 0, moreBtn.height + 6) }
            onHeightChanged: if (!list.userScrolled) Qt.callLater(list.positionViewAtBeginning)
            width: list.width
            Item { width: 1; height: 44 }
            IconButton { x: -8; icon: "back"; iconSize: 20; tip: qsTr("返回"); onClicked: page.back() }
            Item { width: 1; height: 18 }

            Row {
                spacing: 40
                Item {
                    id: avatar
                    // 与专辑封面同样随窗口宽度和高度变化
                    width: Math.round(Math.max(170, Math.min(280, list.width * 0.2, page.height * 0.32)))
                    height: width
                    SoftShadow { target: avatarImg; spread: 30; offsetY: 18 }
                    RoundedImage {
                        id: avatarImg
                        anchors.fill: parent
                        radius: width / 2
                        source: page.info.cover || ""
                        // 按物理像素解码(见 AlbumCard.qml 的说明)
                        readonly property real pxScale: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
                        decodeSize: Math.ceil(avatar.width * Math.max(1.0, Math.min(1.5, pxScale)))
                    }
                }
                Column {
                    anchors.bottom: avatar.bottom
                    spacing: 12
                    // 接近顶部精简栏时渐隐,两处名字不同时出现
                    opacity: Math.max(0, Math.min(1, (bar.heroBottom - bar.height - 20) / 120))
                    Text {
                        width: list.width - avatar.width - 40
                        text: page.info.name || ""
                        elide: Text.ElideRight
                        color: Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: list.width >= 1500 ? 52 : list.width >= 860 ? 44 : 34
                        font.weight: Font.Bold
                        font.letterSpacing: list.width >= 1500 ? -0.8 : -0.5
                    }
                    Text {
                        text: page.info.summary || ""
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                    }
                    Item { width: 1; height: 6 }
                    Row {
                        spacing: 12
                        IconButton {
                            width: 52; height: 52
                            filled: true
                            morphOnChange: true
                            icon: page.isCurrent && player.playing ? "pause" : "play"
                            iconSize: 20
                            tip: page.isCurrent ? (player.playing ? qsTr("暂停") : qsTr("继续播放")) : qsTr("播放")
                            onClicked: page.playOrToggle()
                        }
                        IconButton {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 42; height: 42; outlined: true; icon: "shuffle"; iconSize: 17
                            tip: qsTr("随机播放")
                            onClicked: page.play(true)
                        }
                        IconButton {
                            id: moreBtn
                            anchors.verticalCenter: parent.verticalCenter
                            width: 42; height: 42; outlined: true; icon: "more"; iconSize: 17
                            tip: qsTr("更多")
                            onClicked: head.openMore()
                        }
                    }
                }
            }

            // 专辑带:标题 + 数量,封面与下方曲目行的封面左缘对齐 (曲目行从 x 12 开始)
            Item { width: 1; height: page.albums.length ? 40 : 30 }
            Item {
                width: list.width
                height: 30
                visible: page.albums.length > 0
                Text {
                    id: albumsTitle
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("专辑")
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
                Text {
                    anchors.left: albumsTitle.right
                    anchors.leftMargin: 10
                    anchors.baseline: albumsTitle.baseline
                    text: page.albums.length
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                }
            }
            Item { width: 1; height: page.albums.length ? 14 : 0 }
            AlbumStrip {
                x: 12
                width: list.width - 12
                model: page.albums
                onOpen: (index) => Nav.openAlbum(index)
            }
            Item { width: 1; height: 18 }
            Rectangle { width: list.width; height: 1; color: Theme.hairline }
            Item { width: 1; height: 10 }
        }

        delegate: TrackRow {
            width: list.width
            trackId: model.trackId
            number: model.number
            title: model.title
            artist: model.artist
            albumArtist: page.info.name || ""
            album: model.album
            albumIndex: model.albumIndex
            quality: model.quality
            duration: model.duration
            cover: model.cover
            showAlbum: true
            keyFocus: ListView.isCurrentItem
            onActivated: Nav.picked(player.pickTracks(artistTracks.trackIds(), index, page.queueTag, false, Nav.pickNow))
        }
    }

    // ---- 精简标题栏 ----
    DetailBar {
        id: bar
        view: list
        heroHeight: list.headerItem ? list.headerItem.heroHeight : -1
        sidePadding: page.sidePadding
        title: page.info.name || ""
        subtitle: page.info.summary || ""
        cover: page.info.cover || ""
        round: true
        current: page.isCurrent
        onBack: page.back()
        onMore: (anchor) => page.openMenu(anchor, 0, anchor.height + 6)
        onShuffle: page.play(true)
        onPlay: page.playOrToggle()
    }
}
