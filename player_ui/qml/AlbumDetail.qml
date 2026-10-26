import QtQuick
import PlayerUi

// 专辑详情:大封面 + 信息区 (随列表一起滚动) + 曲目列表;多碟专辑按碟分组;
// 页尾是专辑资料与「更多这位歌手的专辑」。滚过头部后顶端浮出一条精简标题栏 (返回 · 封面 · 标题 · 播放)
Item {
    id: page
    property int bottomInset: 140
    readonly property Item heroCover: list.headerItem ? list.headerItem.coverItem : null
    readonly property bool isCurrentAlbum: player.hasTrack && player.albumIndex === albumTracks.albumIndex
    // 封面正飞回专辑墙:唱片随之隐去,不让它在封面离开后单独露出来
    property bool heroAway: false
    signal back()

    function heroCoverRect() {
        if (!heroCover) return Qt.rect(0, 0, 0, 0)
        const p = heroCover.mapToItem(page, 0, 0)
        return Qt.rect(p.x, p.y, heroCover.width, heroCover.height)
    }
    function resetScroll() { list.currentIndex = -1; list.moved = false; list.positionViewAtBeginning() }
    // 截图验证用:滚到指定位置 (< 0 滚到底)
    function automationScroll(y) {
        list.moved = true
        const maxY = list.originY + list.contentHeight + list.bottomMargin - list.height
        list.contentY = y < 0 ? maxY : Math.min(maxY, list.originY + y)
    }

    // 内容列最宽 1560:超宽屏上标题和时长不会隔着整块屏幕
    readonly property int sidePadding: Theme.pagePadding(width)
    readonly property int artistIdx: library.generation >= 0 && albumTracks.artist.length ? library.artistIndexOf(albumTracks.artist) : -1
    readonly property var moreAlbums: artistIdx >= 0 ? library.artistAlbums(artistIdx).filter(a => a.index !== albumTracks.albumIndex) : []
    readonly property var details: albumTracks.details

    // 按专辑顺序播放 (关掉随机,和歌手 / 歌单页的「播放」一致);正在放这张时就是暂停 / 继续
    function playInOrder() {
        if (page.isCurrentAlbum) { player.toggle(); return }
        player.shuffle = false
        Nav.picked(player.pickAlbum(albumTracks.albumIndex, 0, true))
    }
    function playShuffled() {
        player.shuffle = true
        player.playAlbum(albumTracks.albumIndex, Math.floor(Math.random() * Math.max(1, list.count)))
    }
    function openMenu(item, px, py) {
        const p = item.mapToItem(null, px, py)
        Nav.trackMenu(-1, p.x, p.y, { albumIndex: albumTracks.albumIndex, fromDetail: true })
    }

    // 键盘:上下选择曲目、回车播放、Home / End、菜单键 (由 Main 的按键捕获转发过来)
    function keyNav(e) {
        if (list.count === 0) return false
        let i = list.currentIndex
        if (e.key === Qt.Key_Down || e.key === Qt.Key_Up) {
            if (i < 0) {
                const playing = page.isCurrentAlbum ? albumTracks.trackIds.indexOf(player.trackId) : -1
                i = playing >= 0 ? playing : (e.key === Qt.Key_Down ? 0 : list.count - 1)
            } else {
                i = Math.max(0, Math.min(list.count - 1, i + (e.key === Qt.Key_Down ? 1 : -1)))
            }
        } else if (e.key === Qt.Key_Home) i = 0
        else if (e.key === Qt.Key_End) i = list.count - 1
        else if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter) && i >= 0) {
            Nav.picked(player.pickAlbum(albumTracks.albumIndex, i))
            return true
        } else if (e.key === Qt.Key_Menu && i >= 0 && list.currentItem) {
            const p = list.currentItem.mapToItem(null, 60, list.currentItem.height)
            Nav.trackMenu(albumTracks.trackIds[i], p.x, p.y, { albumIndex: albumTracks.albumIndex })
            return true
        } else return false
        list.currentIndex = i
        list.positionViewAtIndex(i, ListView.Contain)
        return true
    }

    // 换专辑时:唱片不要把上一张的滑出动画再演一遍,内容轻轻浮现而不是硬切
    property bool slideReady: false
    Connections {
        target: albumTracks
        function onAlbumChanged() {
            page.slideReady = false
            list.currentIndex = -1
            Qt.callLater(() => page.slideReady = true)
            if (page.visible && !Theme.reduced) swap.restart()
        }
    }
    ParallelAnimation {
        id: swap
        NumberAnimation { target: list; property: "opacity"; from: 0.35; to: 1; duration: Theme.normal; easing.type: Easing.OutCubic }
        NumberAnimation { target: listShift; property: "y"; from: 10; to: 0; duration: Theme.normal; easing.type: Easing.OutCubic }
    }

    // ---- 头部底色:这张专辑自己的封面模糊铺在头部后面,往下渐隐 ----
    // 只在换封面时渲染一次 (Backdrop 自带图层),滚动时只是平移,不逐帧重绘
    Item {
        id: wash
        width: page.width
        height: list.headerItem ? list.headerItem.height + 180 : 0
        y: list.headerItem ? list.headerItem.y - list.contentY : 0
        visible: y + height > 0 && albumTracks.cover.length > 0
        opacity: page.isCurrentAlbum ? 0.38 : 0.72
        Behavior on opacity { NumberAnimation { duration: Theme.slow } }
        layer.enabled: true
        layer.effect: ShaderEffect {
            property real fadeTop: 0.001
            property real fadeBottom: 0.75
            fragmentShader: "qrc:/shaders/fade.frag.qsb"
        }
        Backdrop {
            anchors.fill: parent
            source: albumTracks.cover
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
        model: albumTracks
        boundsBehavior: Flickable.StopAtBounds
        bottomMargin: page.bottomInset
        cacheBuffer: 800
        reuseItems: true
        spacing: 2
        currentIndex: -1
        keyNavigationEnabled: false
        highlightFollowsCurrentItem: false
        transform: Translate { id: listShift }
        SmoothScroll { flickable: list; onScrolled: list.moved = true }
        // 用户还没滚动时,头部高度变化 (窗口缩放让封面变大) 后始终停在顶部
        property bool moved: false
        onMovementStarted: moved = true

        section.property: albumTracks.multiDisc ? "disc" : ""
        section.delegate: Item {
            id: discHead
            width: list.width
            height: 52
            readonly property var info: albumTracks.discs.find(d => d.disc === Number(section)) || ({})
            readonly property bool hovered: discMouse.containsMouse || discPlay.hovered
            MouseArea { id: discMouse; anchors.fill: parent; hoverEnabled: !Theme.scrolling }
            // 碟片图标对齐序号列,文字对齐标题列
            Icon {
                x: 20
                anchors.verticalCenter: discLabel.verticalCenter
                name: "disc"
                size: 16
                tint: Theme.accent
            }
            Text {
                id: discLabel
                x: 58
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 13
                text: qsTr("第 %1 张碟").arg(section)
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
            Text {
                id: discMeta
                anchors.left: discLabel.right
                anchors.leftMargin: 10
                anchors.baseline: discLabel.baseline
                text: discHead.info.count !== undefined ? qsTr("%1 首  ·  %2").arg(discHead.info.count).arg(discHead.info.duration) : ""
                color: Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
            Rectangle {
                anchors.left: discMeta.right
                anchors.leftMargin: 14
                anchors.right: discPlay.left
                anchors.rightMargin: 10
                anchors.verticalCenter: discLabel.verticalCenter
                height: 1
                color: Theme.hairline
            }
            IconButton {
                id: discPlay
                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: discLabel.verticalCenter
                width: 30; height: 30
                icon: "play"
                iconSize: 12
                outlined: true
                tip: qsTr("播放这张碟")
                opacity: discHead.hovered ? 1 : 0.0
                Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                onClicked: {
                    player.shuffle = false
                    Nav.picked(player.pickAlbum(albumTracks.albumIndex, discHead.info.firstRow || 0, true))
                }
            }
        }

        header: Item {
            id: hero
            width: list.width
            readonly property Item coverItem: heroCoverItem
            // 封面随窗口变大 (宽度与高度都考虑),小窗口不至于让头部吃掉大半屏
            readonly property real coverSize: Math.round(Math.max(180, Math.min(340, list.width * 0.24, page.height * 0.36)))
            height: 102 + coverSize + 46
            onHeightChanged: if (!list.moved) Qt.callLater(list.positionViewAtBeginning)
            // 宽窗口:头部右侧放一栏专辑资料,不让大片空白
            readonly property bool wideSpecs: list.width >= 1180

            IconButton {
                y: 44
                x: -8
                icon: "back"
                iconSize: 20
                tip: qsTr("返回")
                onClicked: page.back()
            }

            // 唱片平时从封面后探出一截,播放这张专辑时滑出更多并转动
            Vinyl {
                width: hero.coverSize * 0.94
                height: width
                y: heroCoverItem.y + (hero.coverSize - height) / 2
                x: page.isCurrentAlbum ? hero.coverSize * 0.42 : hero.coverSize * 0.16
                Behavior on x {
                    enabled: page.slideReady
                    NumberAnimation { duration: Theme.reduced ? Theme.normal : 900; easing.type: Easing.OutCubic }
                }
                cover: albumTracks.cover
                opacity: page.heroAway ? 0 : 1
                Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                spinning: page.isCurrentAlbum && player.playing
                labelSize: 256
            }
            SoftShadow { target: heroCoverItem; spread: 36; offsetY: 20 }
            RoundedImage {
                id: heroCoverItem
                y: 102
                width: hero.coverSize
                height: hero.coverSize
                radius: 10
                source: albumTracks.cover
                decodeSize: Math.ceil(hero.coverSize * Screen.devicePixelRatio)
                // 左键看大图,右键弹出专辑菜单
                MouseArea {
                    id: coverMouse
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: Qt.PointingHandCursor
                    onClicked: (e) => {
                        if (e.button === Qt.RightButton) page.openMenu(coverMouse, e.x, e.y)
                        else Nav.viewCover(albumTracks.cover, albumTracks.title, albumTracks.artist)
                    }
                }
            }

            Column {
                id: info
                // 文字起点固定 (给唱片留出滑出的位置):开始播放时按钮不会从鼠标下面跑掉
                anchors.left: heroCoverItem.right
                anchors.leftMargin: hero.coverSize * 0.36 + 40
                anchors.right: hero.wideSpecs ? specs.left : parent.right
                anchors.rightMargin: hero.wideSpecs ? 48 : 0
                anchors.bottom: heroCoverItem.bottom
                spacing: 10
                // 接近顶部精简栏时渐隐,两处标题不同时出现
                opacity: Math.max(0, Math.min(1, (sticky.heroBottom - sticky.height - 20) / 120))

                Text {
                    id: titleText
                    width: parent.width
                    text: albumTracks.title
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    color: Theme.text
                    font.family: Theme.fontFamily
                    // 字号只看列表宽度 (不看自己的宽度),播放与否都不会跳
                    font.pixelSize: list.width >= 1500 ? 52 : list.width >= 860 ? 44 : 34
                    font.weight: Font.Bold
                    font.letterSpacing: list.width >= 1500 ? -0.8 : -0.5
                    lineHeight: 1.05
                }
                Text {
                    width: Math.min(implicitWidth, parent.width)
                    elide: Text.ElideRight
                    text: albumTracks.artist
                    color: Theme.accent
                    font.family: Theme.fontFamily
                    font.pixelSize: 18
                    font.weight: Font.Medium
                    font.underline: artistMouse.containsMouse && artistMouse.enabled
                    MouseArea {
                        id: artistMouse
                        anchors.fill: parent
                        // 群星 / 找不到对应歌手时不可点
                        enabled: page.artistIdx >= 0
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: Nav.openArtist(page.artistIdx)
                    }
                }
                Text {
                    width: parent.width
                    elide: Text.ElideRight
                    text: albumTracks.summary
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                }
                // 音质:一个档次标签 + 具体格式写成文字
                Row {
                    spacing: 10
                    visible: albumTracks.badges.length > 0 || albumTracks.formatText.length > 0
                    Repeater {
                        model: albumTracks.badges
                        QualityBadge { text: modelData; fontSize: 11; anchors.verticalCenter: parent.verticalCenter }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: albumTracks.formatText
                        color: Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        font.letterSpacing: 0.2
                    }
                }
                Item { width: 1; height: 6 }
                Row {
                    spacing: 12
                    IconButton {
                        width: 52; height: 52
                        filled: true
                        morphOnChange: true
                        icon: page.isCurrentAlbum && player.playing ? "pause" : "play"
                        iconSize: 20
                        tip: page.isCurrentAlbum ? (player.playing ? qsTr("暂停") : qsTr("继续播放")) : qsTr("按专辑顺序播放")
                        onClicked: page.playInOrder()
                    }
                    IconButton {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 42; height: 42
                        outlined: true
                        icon: "shuffle"
                        iconSize: 17
                        tip: qsTr("随机播放")
                        onClicked: page.playShuffled()
                    }
                    IconButton {
                        id: moreBtn
                        anchors.verticalCenter: parent.verticalCenter
                        width: 42; height: 42
                        outlined: true
                        icon: "more"
                        iconSize: 17
                        tip: qsTr("更多")
                        onClicked: page.openMenu(moreBtn, 0, height + 6)
                    }
                    // 正在播放这张专辑时,按钮旁浮现律动频谱
                    Item {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 132
                        height: 30
                        visible: info.width > 420
                        Spectrum {
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.bottom: parent.bottom
                            width: 120
                            height: 30
                            bars: 18
                            gap: 2.5
                            opacity: page.isCurrentAlbum && player.playing ? 0.85 : 0
                            visible: opacity > 0
                            Behavior on opacity { NumberAnimation { duration: Theme.slow } }
                        }
                    }
                }
            }

            // ---- 宽窗口右侧:专辑资料 ----
            Column {
                id: specs
                visible: hero.wideSpecs
                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.bottom: heroCoverItem.bottom
                anchors.bottomMargin: 4
                width: 260
                spacing: 0
                opacity: info.opacity
                Text {
                    text: qsTr("专辑资料")
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1
                    bottomPadding: 10
                }
                Rectangle { width: parent.width; height: 1; color: Theme.hairline }
                Repeater {
                    model: {
                        const d = page.details
                        const rows = [
                            { k: qsTr("格式"), v: albumTracks.codecText },
                            { k: qsTr("规格"), v: albumTracks.specText },
                            { k: qsTr("大小"), v: d.size > 0 ? Theme.formatBytes(d.size) : "" },
                            { k: qsTr("碟数"), v: d.discCount > 1 ? qsTr("%1 张").arg(d.discCount) : "" },
                            { k: qsTr("专辑增益"), v: d.albumGain || "" },
                            { k: qsTr("位置"), v: d.folder || "", folder: true }
                        ]
                        return rows.filter(r => r.v.length > 0)
                    }
                    delegate: Item {
                        width: specs.width
                        height: 34
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 64
                            text: modelData.k
                            color: Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                        Text {
                            x: 70
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 70
                            horizontalAlignment: Text.AlignRight
                            elide: modelData.folder ? Text.ElideMiddle : Text.ElideRight
                            text: modelData.v
                            color: modelData.folder && specMouse.containsMouse ? Theme.text : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            font.underline: modelData.folder === true && specMouse.containsMouse
                            MouseArea {
                                id: specMouse
                                anchors.fill: parent
                                enabled: modelData.folder === true
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: if (!library.revealTrack(page.details.firstTrackId)) Nav.toast("folder", qsTr("找不到这张专辑所在的文件夹"))
                            }
                        }
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.hairline; opacity: 0.6 }
                    }
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 14
                width: parent.width
                height: 1
                color: Theme.hairline
                visible: !albumTracks.multiDisc
            }
        }

        delegate: TrackRow {
            width: list.width
            trackId: model.trackId
            number: model.number
            title: model.title
            artist: model.artist
            albumArtist: albumTracks.artist
            quality: model.quality
            // 每首音质都一样时不再逐行重复,只在头部说一次;混着不同音质时才逐行标出
            showQuality: albumTracks.commonQuality.length === 0
            duration: model.duration
            keyFocus: ListView.isCurrentItem
            menuContext: ({ albumIndex: albumTracks.albumIndex })
            onActivated: Nav.picked(player.pickAlbum(albumTracks.albumIndex, index, false, Nav.pickNow))
        }

        // ---- 页尾:专辑资料 + 这位歌手的其他专辑 ----
        footer: Column {
            width: list.width
            visible: albumTracks.albumIndex >= 0
            Item { width: 1; height: 26 }
            Rectangle { width: parent.width; height: 1; color: Theme.hairline }
            Item { width: 1; height: 16 }
            Text {
                x: 12
                width: parent.width - 24
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 12
                lineHeight: 1.3
                text: {
                    const d = page.details
                    const parts = []
                    if (albumTracks.formatText.length) parts.push(albumTracks.formatText)
                    if (d.size > 0) parts.push(Theme.formatBytes(d.size))
                    if (d.discCount > 1) parts.push(qsTr("%1 张碟").arg(d.discCount))
                    if (d.albumGain) parts.push(qsTr("专辑增益 %1").arg(d.albumGain))
                    return parts.join("  ·  ")
                }
            }
            Item { width: 1; height: 8 }
            // 所在文件夹:点一下在资源管理器里打开 (同一首歌下了好几份时,能看出这张来自哪里)
            Item {
                x: 12
                width: parent.width - 24
                height: 20
                visible: !!page.details.folder
                Icon { id: folderIcon; anchors.verticalCenter: parent.verticalCenter; name: "folder-open"; size: 13; opacity: 0.55 }
                Text {
                    anchors.left: folderIcon.right
                    anchors.leftMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, parent.width - 20)
                    elide: Text.ElideMiddle
                    text: (page.details.folder || "") + (page.details.folderCount > 1 ? qsTr("  等 %1 个文件夹").arg(page.details.folderCount) : "")
                    color: folderMouse.containsMouse ? Theme.textSecondary : Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.underline: folderMouse.containsMouse
                    MouseArea {
                        id: folderMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (!library.revealTrack(page.details.firstTrackId)) Nav.toast("folder", qsTr("找不到这张专辑所在的文件夹"))
                    }
                }
            }

            Item { width: 1; height: page.moreAlbums.length > 0 ? 40 : 8 }
            Item {
                width: parent.width
                height: 30
                visible: page.moreAlbums.length > 0
                Text {
                    id: moreTitle
                    x: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, parent.width - 120)
                    elide: Text.ElideRight
                    text: qsTr("更多 %1 的专辑").arg(albumTracks.artist)
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
                Capsule {
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    height: 28
                    text: qsTr("全部")
                    onClicked: Nav.openArtist(page.artistIdx)
                }
            }
            Item { width: 1; height: page.moreAlbums.length > 0 ? 14 : 0 }
            AlbumStrip {
                x: 12
                width: parent.width - 12
                model: page.moreAlbums
                onOpen: (index) => Nav.openAlbum(index)
            }
        }
    }

    // ---- 精简标题栏:头部滚出视野后从顶端浮出 ----
    DetailBar {
        id: sticky
        view: list
        sidePadding: page.sidePadding
        title: albumTracks.title
        subtitle: albumTracks.artist
        cover: albumTracks.cover
        current: page.isCurrentAlbum
        onBack: page.back()
        onMore: (anchor) => page.openMenu(anchor, 0, anchor.height + 6)
        onShuffle: page.playShuffled()
        onPlay: page.playInOrder()
    }
}
