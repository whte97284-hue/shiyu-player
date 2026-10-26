import QtQuick
import PlayerUi

// 歌单详情:封面拼图 + 可编辑名称 + 播放 / 随机 / 改名 / 更多 (删除在「更多」里);曲目可右键移除、拖动排序
// 头部尺寸与专辑详情一致;滚过头部后顶端浮出精简标题栏
Item {
    id: page
    // 内容滚到窗口按钮下面了:Main 据此在顶端铺一条磨砂条 (精简标题栏出现后由它自己盖住顶端)
    readonly property bool scrolled: list.contentY - list.originY > 8 && !bar.shown
    objectName: "playlistDetailPage"
    property int bottomInset: 140
    property string playlistId
    signal back()
    readonly property int sidePadding: Theme.pagePadding(width)
    readonly property var info: playlists.revision >= 0 && playlistId.length ? playlists.info(playlistId) : ({})
    // 当前播放队列就是这个歌单:播放键是暂停 / 继续
    readonly property string queueTag: "playlist:" + playlistId
    readonly property bool isCurrent: playlistId.length > 0 && player.queueTag === queueTag

    onPlaylistIdChanged: {
        playlistTracks.playlistId = playlistId
        list.userScrolled = false
        Qt.callLater(list.positionViewAtBeginning)
        if (Nav.renameNext === playlistId) {
            Nav.renameNext = ""
            renameSoon.restart()
        }
    }
    // 「更多 → 重命名」时歌单页可能已经打开着这个歌单 (playlistId 不变)
    Connections {
        target: Nav
        function onRenameNextChanged() {
            if (!Nav.renameNext.length || Nav.renameNext !== page.playlistId) return
            Nav.renameNext = ""
            renameSoon.restart()
        }
    }
    // 歌单被删除 (「更多 → 删除歌单」) 时退回上一页
    onInfoChanged: if (playlistId.length && info.id === undefined && visible) back()
    // 等页面淡入后再进入编辑,光标不会在过渡中闪烁
    Timer { id: renameSoon; interval: 320; onTriggered: page.startRename() }
    function startRename() {
        if (info.builtin) return
        list.positionViewAtBeginning()
        if (list.headerItem) list.headerItem.beginEdit()
    }

    function play(shuffled) {
        if (playlistTracks.count === 0) return
        player.shuffle = shuffled
        shuffled ? player.playTracks(playlistTracks.trackIds(), Math.floor(Math.random() * playlistTracks.count), queueTag)
                 : Nav.picked(player.pickTracks(playlistTracks.trackIds(), 0, queueTag, true))
    }
    function playOrToggle() { if (isCurrent) player.toggle(); else play(false) }
    function keyNav(e) {
        if (list.dragFrom >= 0) return false
        return Nav.listKeys(list, e, isCurrent ? playlistTracks.rowOfTrack(player.trackId) : -1, bottomInset - 20,
                            (i) => Nav.picked(player.pickTracks(playlistTracks.trackIds(), i, queueTag)),
                            (i, x, y) => { const it = list.currentItem; if (it) Nav.trackMenu(it.row.trackId, x, y, it.row.menuCtx()) })
    }
    Connections {
        target: playlistTracks
        function onContentChanged() { if (list.dragFrom < 0) list.currentIndex = -1 }
    }
    function openMenu(item, px, py) {
        const p = item.mapToItem(null, px, py)
        Nav.trackMenu(-1, p.x, p.y, { ids: playlistTracks.trackIds(), kind: "playlist", playlistId: playlistId,
                                      title: info.name || "", tag: queueTag, fromDetail: true })
    }
    // 截图验证用:滚到指定位置 (< 0 滚到底);打开「更多」菜单
    function automationScroll(y) {
        list.userScrolled = true
        const maxY = list.originY + list.contentHeight + list.bottomMargin - list.height
        list.contentY = y < 0 ? maxY : Math.min(maxY, list.originY + y)
    }
    function automationMenu() { if (list.headerItem) list.headerItem.openMore() }

    // ---- 头部底色:歌单第一张封面模糊铺在头部后面,往下渐隐,随列表一起滚走 ----
    readonly property string firstCover: page.info.covers && page.info.covers.length ? page.info.covers[0] : ""
    Item {
        id: wash
        width: page.width
        height: list.headerItem ? list.headerItem.height + 180 : 0
        y: list.headerItem ? list.headerItem.y - list.contentY : 0
        visible: y + height > 0 && page.firstCover.length > 0
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
            source: page.firstCover
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
            if (list.currentIndex >= 0 && list.dragFrom < 0) list.currentIndex = -1
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.leftMargin: page.sidePadding
        anchors.rightMargin: page.sidePadding
        model: playlistTracks
        boundsBehavior: Flickable.StopAtBounds
        bottomMargin: page.bottomInset
        cacheBuffer: 600
        reuseItems: true
        // 拖动排序时把被拖的行设为当前项:滚出视野也不会被回收 (按住的把手一直有效)
        currentIndex: -1
        keyNavigationEnabled: false
        highlightFollowsCurrentItem: false
        SmoothScroll { flickable: list; onScrolled: list.userScrolled = true }
        // 头部内容 (专辑带、封面) 异步就绪后高度会变:用户还没滚动时始终停在顶部
        property bool userScrolled: false
        onMovementStarted: userScrolled = true
        onCountChanged: if (!userScrolled) Qt.callLater(positionViewAtBeginning)

        header: Column {
            onHeightChanged: if (!list.userScrolled) Qt.callLater(list.positionViewAtBeginning)
            id: head
            width: list.width
            // 大标题区 (返回键 + 封面行) 的高度,精简栏按它计算
            readonly property real heroHeight: 102 + art.height + 46
            function beginEdit() { nameEdit.text = page.info.name || ""; nameEdit.visible = true; nameEdit.forceActiveFocus(); nameEdit.selectAll() }
            function openMore() { page.openMenu(moreBtn, 0, moreBtn.height + 6) }

            Item { width: 1; height: 44 }
            IconButton { x: -8; icon: "back"; iconSize: 20; tip: qsTr("返回"); onClicked: page.back() }
            Item { width: 1; height: 18 }
            Row {
                spacing: 40
                Item {
                    id: art
                    // 与专辑封面同样随窗口宽度和高度变化
                    width: Math.round(Math.max(170, Math.min(280, list.width * 0.2, page.height * 0.32)))
                    height: width
                    SoftShadow { target: mosaic; spread: 32; offsetY: 20 }
                    Mosaic {
                        id: mosaic
                        anchors.fill: parent
                        covers: page.info.covers || []
                        favorite: !!page.info.builtin
                        decodeSize: Math.ceil(art.width * 2)
                    }
                }
                Column {
                    anchors.bottom: art.bottom
                    spacing: 12
                    width: list.width - art.width - 40
                    // 接近顶部精简栏时渐隐,两处名字不同时出现
                    opacity: Math.max(0, Math.min(1, (bar.heroBottom - bar.height - 20) / 120))

                    Item {
                        width: parent.width
                        height: nameText.implicitHeight
                        Text {
                            id: nameText
                            width: parent.width
                            visible: !nameEdit.visible
                            text: page.info.name || ""
                            elide: Text.ElideRight
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: list.width >= 1500 ? 52 : list.width >= 860 ? 44 : 34
                            font.weight: Font.Bold
                            font.letterSpacing: list.width >= 1500 ? -0.8 : -0.5
                            MouseArea {
                                anchors.fill: parent
                                enabled: !page.info.builtin
                                cursorShape: Qt.IBeamCursor
                                onDoubleClicked: head.beginEdit()
                            }
                        }
                        TextInput {
                            id: nameEdit
                            visible: false
                            width: parent.width
                            color: Theme.text
                            selectionColor: Theme.accentAlpha(0.4)
                            font: nameText.font
                            maximumLength: 60
                            onAccepted: { playlists.rename(page.playlistId, text); visible = false }
                            onActiveFocusChanged: if (!activeFocus && visible) { playlists.rename(page.playlistId, text); visible = false }
                            Keys.onEscapePressed: visible = false
                            Rectangle { anchors.top: parent.bottom; width: parent.width; height: 2; color: Theme.accent }
                        }
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
                            visible: !page.info.builtin
                            anchors.verticalCenter: parent.verticalCenter
                            width: 42; height: 42; outlined: true; icon: "edit"; iconSize: 16
                            tip: qsTr("重命名")
                            onClicked: head.beginEdit()
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
            Item { width: 1; height: 30 }
            Rectangle { width: list.width; height: 1; color: Theme.hairline }
            Item { width: 1; height: 10 }
        }

        // 拖动排序:悬停时行左侧出现把手,按住把手上下拖,强调色细线标出落点
        property int dragFrom: -1
        property int dropAt: -1
        // 拖到上下边缘时持续自动滚动 (指针停在边缘不动也继续滚):方向、深入边缘的程度、最后的指针位置 (列表坐标)
        property int edgeDir: 0
        property real edgeDepth: 0
        property real dragY: 0
        function updateDrop() {
            let i = indexAt(40, contentY + dragY)
            if (i < 0) i = contentY + dragY < originY + (headerItem ? headerItem.height : 0) ? 0 : count - 1
            dropAt = i
        }
        function endDrag() {
            dragFrom = -1
            dropAt = -1
            edgeDir = 0
            currentIndex = -1
            ghost.visible = false
        }
        Timer {
            interval: 16
            repeat: true
            running: list.dragFrom >= 0 && list.edgeDir !== 0
            onTriggered: {
                const step = 4 + 14 * Math.min(1, list.edgeDepth / 60)
                const maxY = list.contentHeight + list.originY - list.height + list.bottomMargin
                list.contentY = list.edgeDir < 0 ? Math.max(list.originY, list.contentY - step)
                                                 : Math.max(list.originY, Math.min(maxY, list.contentY + step))
                list.updateDrop()
            }
        }
        delegate: Item {
            id: rowWrap
            readonly property Item row: trackRow
            width: list.width
            height: trackRow.height
            opacity: list.dragFrom === index ? 0.35 : 1
            Behavior on opacity { OpacityAnimator { duration: Theme.fast } }

            TrackRow {
                id: trackRow
                width: parent.width
                trackId: model.trackId
                number: model.number
                title: model.title
                artist: model.artist
                album: model.album
                albumIndex: model.albumIndex
                quality: model.quality
                duration: model.duration
                cover: model.cover
                showAlbum: true
                keyFocus: rowWrap.ListView.isCurrentItem && list.dragFrom < 0
                menuContext: ({ playlistId: page.playlistId })
                onActivated: Nav.picked(player.pickTracks(playlistTracks.trackIds(), index, page.queueTag, false, Nav.pickNow))
            }
            Icon {
                x: -26
                anchors.verticalCenter: parent.verticalCenter
                name: "grip"
                size: 16
                opacity: grip.containsMouse || grip.pressed ? 0.9 : (trackRow.hovered ? 0.4 : 0)
                Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
            }
            MouseArea {
                id: grip
                x: -34
                width: 30
                height: parent.height
                hoverEnabled: true
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                preventStealing: true
                onPressed: (e) => {
                    list.currentIndex = index
                    list.dragFrom = index
                    list.dropAt = index
                    ghost.title = model.title
                    ghost.cover = model.cover
                    move(e)
                }
                onPositionChanged: (e) => { if (pressed) move(e) }
                onReleased: {
                    const from = list.dragFrom, to = list.dropAt
                    list.endDrag()
                    if (from >= 0 && to >= 0 && to !== from) playlists.move(page.playlistId, from, to)
                }
                onCanceled: list.endDrag()
                function move(e) {
                    const lv = mapToItem(list, e.x, e.y)
                    list.dragY = lv.y
                    list.updateDrop()
                    const v = mapToItem(page, e.x, e.y)
                    ghost.y = v.y - ghost.height / 2
                    ghost.visible = true
                    // 进入上下边缘区后由计时器持续滚动,越靠边越快
                    const bottomEdge = list.height - page.bottomInset - 20
                    if (lv.y < 60) { list.edgeDir = -1; list.edgeDepth = 60 - lv.y }
                    else if (lv.y > bottomEdge) { list.edgeDir = 1; list.edgeDepth = lv.y - bottomEdge }
                    else list.edgeDir = 0
                }
            }
            // 落点指示线
            Rectangle {
                visible: list.dropAt === index && list.dragFrom >= 0 && list.dropAt !== list.dragFrom
                y: list.dropAt > list.dragFrom ? parent.height - 1 : -1
                width: parent.width
                height: 2
                radius: 1
                color: Theme.accent
            }
        }
    }

    // 拖动中跟随指针的行影
    Rectangle {
        id: ghost
        property string title
        property string cover
        visible: false
        x: page.sidePadding + 8
        width: Math.min(420, page.width - page.sidePadding * 2)
        height: 52
        radius: Theme.radiusM
        color: Theme.panel
        border.width: 1
        border.color: Theme.accentAlpha(0.5)
        z: 10
        RoundedImage { x: 8; anchors.verticalCenter: parent.verticalCenter; width: 36; height: 36; radius: 6; source: ghost.cover; decodeSize: 72 }
        Text {
            x: 56
            width: parent.width - 70
            anchors.verticalCenter: parent.verticalCenter
            text: ghost.title
            elide: Text.ElideRight
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 14
        }
    }

    // 空歌单:淡淡的图标 + 一行怎么添加歌曲的提示
    Column {
        visible: playlistTracks.count === 0
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.62
        spacing: 14
        Icon {
            anchors.horizontalCenter: parent.horizontalCenter
            name: page.info.builtin ? "heart" : "note"
            size: 40
            opacity: 0.18
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: page.info.builtin ? qsTr("点歌曲行上的 ♡ 收藏") : qsTr("在任意歌曲上右键 → 添加到歌单")
            color: Theme.textTertiary
            font.family: Theme.fontFamily
            font.pixelSize: 13
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
        cover: page.firstCover
        current: page.isCurrent
        onBack: page.back()
        onMore: (anchor) => page.openMenu(anchor, 0, anchor.height + 6)
        onShuffle: page.play(true)
        onPlay: page.playOrToggle()
    }
}
