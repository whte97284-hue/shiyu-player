import QtQuick
import PlayerUi

// 搜索页:大号输入框常驻顶部;结果按「最佳匹配 + 歌手 / 专辑带 / 歌曲列表」组织
//  · 拼音 / 首字母 / 简繁 / 假名罗马音都能搜;匹配到的文字以强调色标出
//  · 回车直接打开 / 播放最佳匹配;↓ 进入歌曲列表,回车播放所选
Item {
    id: page
    property int bottomInset: 140
    signal back()
    readonly property int sidePadding: width > 1400 ? 56 : 40
    readonly property string q: searchModel.query.trim()
    readonly property var best: searchModel.top
    readonly property bool hasTop: best && best.kind !== undefined
    readonly property bool empty: q.length > 0 && !hasTop && searchModel.count === 0
    // 歌手 / 专辑带不重复最佳匹配卡片里已经大大显示的那一位 / 那一张
    readonly property var stripArtists: searchModel.artists.filter(a => !(hasTop && best.kind === "artist" && a.index === best.index))
    readonly property var stripAlbums: searchModel.albums.filter(a => !(hasTop && best.kind === "album" && a.index === best.index))

    function focusWith(t) {
        // 从侧栏 / 快捷键回到搜索页时保留上次的搜索,只有带着新词进来才替换
        if (t.length) {
            input.text = t
            searchModel.query = t
        }
        input.forceActiveFocus()
        input.cursorPosition = input.text.length
    }
    function openTop() {
        if (!hasTop) return
        if (best.kind === "artist") Nav.openArtist(best.index)
        else if (best.kind === "album") Nav.openAlbum(best.index)
        else Nav.picked(player.pickTracks(searchModel.trackIds(), 0))
    }

    Timer { id: debounce; interval: 90; onTriggered: searchModel.query = input.text }

    // ---------------- 输入框 ----------------
    Item {
        id: field
        x: page.sidePadding
        y: 54
        // 与结果列表同宽,下划线和歌曲行右端对齐
        width: page.width - page.sidePadding * 2
        height: 56
        Icon {
            id: glass
            anchors.verticalCenter: parent.verticalCenter
            name: "search"
            size: 24
            tint: input.activeFocus ? Theme.accent : "transparent"
            opacity: input.activeFocus ? 1 : 0.5
        }
        TextInput {
            id: input
            anchors.left: glass.right
            anchors.leftMargin: 16
            anchors.right: clear.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.text
            selectionColor: Theme.accentAlpha(0.4)
            selectedTextColor: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 28
            font.weight: Font.DemiBold
            clip: true
            onTextChanged: debounce.restart()
            Keys.onReturnPressed: page.sel >= 0 ? Nav.picked(player.pickTracks(searchModel.trackIds(), page.sel)) : page.openTop()
            Keys.onEnterPressed: page.sel >= 0 ? Nav.picked(player.pickTracks(searchModel.trackIds(), page.sel)) : page.openTop()
            Keys.onDownPressed: page.moveSel(1)
            Keys.onUpPressed: page.moveSel(-1)
            Keys.onEscapePressed: text.length ? (text = "") : page.back()
        }
        IconButton {
            id: clear
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 36; height: 36
            icon: "close"
            iconSize: 15
            opacity: input.text.length ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.fast } }
            onClicked: { input.text = ""; input.forceActiveFocus() }
        }
        // 下划线:聚焦时从中间向两侧展开为强调色
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.hairline }
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            height: 2
            radius: 1
            width: parent.width
            color: Theme.accent
            scale: input.activeFocus ? 1 : 0
            Behavior on scale { ScaleAnimator { duration: Theme.slow; easing.type: Easing.OutQuint } }
        }
    }

    // ---------------- 结果 ----------------
    // 单个可滚动列:最佳匹配 / 歌手 / 专辑 / 歌曲。键盘焦点始终留在输入框:↑↓ 选择歌曲,回车播放
    property int sel: -1
    // 新搜索停在顶部,直到用户自己滚动 / 选歌:头部 (最佳匹配 / 歌手 / 专辑带) 随结果换高度后要重新定位,
    // 不能写 contentY = 0 (有 header 的 ListView 顶端是 originY),否则头部整个留在视口上方
    property bool pinTop: false
    onQChanged: { sel = -1; pinTop = true; if (q.length) refresh.restart() }
    function rewind() { if (pinTop) results.positionViewAtBeginning() }
    Connections {
        target: searchModel
        function onResultsChanged() { Qt.callLater(page.rewind) }
    }
    // 结果刷新:新结果从略下方轻轻浮上并淡入,而不是整页瞬间替换
    ParallelAnimation {
        id: refresh
        OpacityAnimator { target: results; from: 0.35; to: 1; duration: 260; easing.type: Easing.OutCubic }
        NumberAnimation { target: colShift; property: "y"; from: 10; to: 0; duration: 380; easing.type: Easing.OutQuint }
    }
    function moveSel(d) {
        const n = searchModel.count
        if (n === 0) return
        sel = Math.max(-1, Math.min(n - 1, sel + d))
        if (sel < 0) { pinTop = true; results.positionViewAtBeginning(); return }
        pinTop = false
        results.positionViewAtIndex(sel, ListView.Contain)
        const r = results.itemAtIndex(sel)
        if (!r) return
        // 别让选中行藏到底部播放条下面 (不超出列表两端)
        const over = r.y + r.height - (results.contentY + results.height - page.bottomInset)
        if (over > 0) results.contentY = Math.max(results.originY, Math.min(results.contentY + over, results.originY + results.contentHeight - results.height))
    }

    // 结果用 ListView 虚拟化:歌曲再多也只创建可见的几十行,滚动不卡;
    // 最佳匹配 / 歌手 / 专辑带作为 header 跟着一起滚
    ListView {
        id: results
        model: searchModel
        cacheBuffer: 900
        transform: Translate { id: colShift }
        anchors.top: field.bottom
        anchors.topMargin: 8
        anchors.bottom: parent.bottom
        // 列表铺满整页宽度 (两侧留白用边距),鼠标停在左右空白处也能滚动
        width: page.width
        leftMargin: page.sidePadding
        rightMargin: page.sidePadding
        readonly property real innerWidth: width - page.sidePadding * 2
        clip: true
        footer: Item { width: 1; height: page.bottomInset }
        boundsBehavior: Flickable.StopAtBounds
        opacity: page.q.length ? 1 : 0
        Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
        SmoothScroll { flickable: results; onScrolled: page.pinTop = false }
        onMovementStarted: page.pinTop = false

        header: Column {
            id: col
            width: results.innerWidth
            topPadding: 24
            spacing: 0
            onHeightChanged: Qt.callLater(page.rewind)

            // 最佳匹配 + 歌手
            Row {
                visible: page.hasTop || page.stripArtists.length > 0
                width: parent.width
                height: visible ? 196 : 0
                spacing: 28

                // 最佳匹配卡片
                Rectangle {
                    id: topCard
                    visible: page.hasTop
                    width: Math.min(420, parent.width * 0.42)
                    height: 196
                    radius: 18
                    color: topMouse.containsMouse ? Theme.pressed : Theme.hover
                    border.width: 1
                    border.color: Theme.hairline
                    Behavior on color { ColorAnimation { duration: Theme.fast } }
                    RoundedImage {
                        id: topArt
                        x: 22; y: 22
                        width: 92; height: 92
                        radius: page.best && page.best.kind === "artist" ? 46 : 10
                        source: page.best && page.best.cover ? page.best.cover : ""
                        decodeSize: 208
                    }
                    Column {
                        x: 22
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 20
                        width: parent.width - 44 - 64
                        spacing: 4
                        Text {
                            width: parent.width
                            text: page.best ? Nav.highlight(page.best.title || page.best.name || "", page.q, Theme.accent) : ""
                            textFormat: Text.StyledText
                            elide: Text.ElideRight
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 24
                            font.weight: Font.Bold
                        }
                        Row {
                            width: parent.width
                            spacing: 8
                            Icon {
                                anchors.verticalCenter: parent.verticalCenter
                                name: !page.best ? "" : page.best.kind === "artist" ? "artist" : page.best.kind === "album" ? "album-open" : "note"
                                size: 14
                                opacity: 0.5
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 22
                                elide: Text.ElideRight
                                text: !page.best ? "" : (page.best.kind === "artist" ? page.best.summary : (page.best.subtitle || page.best.artist || ""))
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                            }
                        }
                    }
                    MouseArea {
                        id: topMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.openTop()
                    }
                    IconButton {
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 18
                        width: 50; height: 50
                        filled: true
                        icon: "play"
                        iconSize: 20
                        opacity: topMouse.containsMouse || hovered ? 1 : 0
                        scale: topMouse.containsMouse || hovered ? 1 : 0.8
                        Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
                        Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutBack } }
                        onClicked: {
                            const t = page.best
                            if (t.kind === "album") Nav.picked(player.pickAlbum(t.index, 0, true))
                            // 不写 artistTracks:那是歌手详情页正在显示的列表
                            else if (t.kind === "artist") { player.shuffle = false; Nav.picked(player.pickTracks(library.artistTrackIds(t.index), 0, "artist:" + t.name, true)) }
                            else Nav.picked(player.pickTracks(searchModel.trackIds(), 0))
                        }
                    }
                }

                // 歌手
                ListView {
                    id: artistStrip
                    width: parent.width - (topCard.visible ? topCard.width + 28 : 0)
                    height: 196
                    orientation: ListView.Horizontal
                    spacing: 18
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: page.stripArtists
                    SmoothScroll { flickable: artistStrip; horizontal: true }
                    delegate: Item {
                        width: 128
                        height: artistStrip.height
                        readonly property bool hovered: am.containsMouse
                        RoundedImage {
                            y: 22 + (parent.hovered ? -3 : 0)
                            width: 128; height: 128
                            radius: 64
                            source: modelData.cover
                            decodeSize: 256
                            Behavior on y { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                        }
                        Text {
                            y: 160
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: Nav.highlight(modelData.name, page.q, Theme.accent)
                            textFormat: Text.StyledText
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }
                        MouseArea {
                            id: am
                            anchors.fill: parent
                            hoverEnabled: !Theme.scrolling
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Nav.openArtist(modelData.index)
                        }
                    }
                }
            }

            // 专辑带
            Item { width: 1; height: page.stripAlbums.length ? 30 : 0 }
            Marker { visible: page.stripAlbums.length > 0; icon: "album-open"; count: page.stripAlbums.length }
            Item { width: 1; height: page.stripAlbums.length ? 12 : 0 }
            AlbumStrip {
                width: parent.width
                model: page.stripAlbums
                showArtist: true
                query: page.q
                onOpen: (index) => Nav.openAlbum(index)
            }

            Item { width: 1; height: searchModel.count ? 26 : 0 }
            // 列表最多列 500 首,数字写匹配总数
            Marker { visible: searchModel.count > 0; icon: "note"; count: searchModel.total }
            Item { width: 1; height: 6 }
        }
        delegate: TrackRow {
            width: results.innerWidth
            trackId: model.trackId
            number: index + 1
            title: model.title
            artist: model.artist
            album: model.album
            quality: model.quality
            duration: model.duration
            cover: model.cover
            albumIndex: model.albumIndex
            showAlbum: true
            highlight: page.q
            keyFocus: page.sel === index
            onActivated: Nav.picked(player.pickTracks(searchModel.trackIds(), index, "", false, Nav.pickNow))
        }
    }

    // 空查询:只留一个淡淡的图标;无结果时图标下写明没找到,并提示可以怎么搜
    Icon {
        id: emptyIcon
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -20
        name: "search"
        size: 64
        opacity: page.q.length === 0 ? 0.06 : (page.empty ? 0.14 : 0)
        Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
    }
    Column {
        anchors.top: emptyIcon.bottom
        anchors.topMargin: 18
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(560, page.width - page.sidePadding * 2)
        spacing: 6
        opacity: page.empty ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideMiddle
            text: qsTr("没有找到与「%1」相关的结果").arg(page.q)
            color: Theme.textTertiary
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("可以试试拼音或首字母,比如 zjl")
            color: Theme.textTertiary
            font.family: Theme.fontFamily
            font.pixelSize: 12
            opacity: 0.8
        }
    }

    component Marker: Row {
        property string icon
        property int count
        height: visible ? 28 : 0
        spacing: 8
        leftPadding: 2
        Icon { anchors.verticalCenter: parent.verticalCenter; name: parent.icon; size: 15; opacity: 0.5 }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: parent.count.toLocaleString(Qt.locale(), "f", 0)
            color: Theme.textTertiary
            font.family: Theme.fontFamily
            font.pixelSize: 12
        }
    }
}
