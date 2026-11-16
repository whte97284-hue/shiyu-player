import QtQuick
import PlayerUi

// 在线页：榜单 / 搜索 / 扫码登录 / 边听边存
//
// 排版对齐本地搜索页：最佳匹配卡片 + 歌手带并排，往下是歌单带 / 专辑带 / 歌曲列表；
// 分组之间只用一个「图标 + 数量」的分隔（Marker），不放文字标签。
// 强调色**不写死**：全部取 Theme.accent（由当前播放曲目的封面主色驱动），
// 所以在线内容不再是一块独立的冷蓝色，而是跟着歌一起变色。
//
// 动效原则：只做「位置/透明度/颜色」这三类，时长都在 Theme.fast 一档，
// 不上逐行入场、不做逐帧 JS —— 在线页的列表有 120 行，逐行动画会直接吃掉滚动预算。
Item {
    id: page
    property int bottomInset: 140

    readonly property int sidePadding: width > 1400 ? 56 : 40
    readonly property color accent: Theme.accent
    readonly property color dim: Theme.accentAlpha(0.14)
    readonly property color faint: Theme.accentAlpha(0.08)
    readonly property var srcNames: [qsTr("酷狗"), qsTr("网易云")]
    readonly property var qualNames: ["无损", "320K", "128K"]
    readonly property var best: online.tracks.length > 0 ? online.tracks[0] : null
    readonly property bool hasMeta: online.tracks.length > 0
    readonly property real pxScale: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
    // 搜索结果页键盘选中的歌曲下标
    property int sel: -1
    // 结果换了要回到顶部：新内容比旧的矮/高，不重置就会停在半空
    property bool pinTop: true
    function rewind() {
        if (!pinTop || searchView.contentY <= searchView.originY + 1) return
        searchView.positionViewAtBeginning()
    }
    function moveSel(d) {
        if (online.viewMode < 1) return
        const n = online.tracks.length
        if (n === 0) return
        sel = Math.max(-1, Math.min(n - 1, sel + d))
        if (sel < 0) {
            pinTop = true
            searchView.positionViewAtBeginning()
            return
        }
        pinTop = false
        searchView.positionViewAtIndex(sel, ListView.Contain)
    }
    function fmtCount(n) {
        if (!n) return ""
        if (n >= 100000000) return (n / 100000000).toFixed(1) + qsTr("亿")
        if (n >= 10000) return (n / 10000).toFixed(1) + qsTr("万")
        return String(n)
    }

    // 分组分隔：一个图标 + 数量，代替"歌单/歌手/专辑/歌曲"四个文字标签
    component Marker: Row {
        property string icon
        property int count
        property bool want: false
        visible: want && count > 0
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

    // 横向滚动条左右翻页微调按钮（只在有溢出且悬停时浮现，避免桌面鼠标用户翻不动横向带）
    component StripArrows: Item {
        id: arrows
        property ListView targetView
        property var scroller
        property bool hostHovered: false
        anchors.fill: parent
        z: 3
        readonly property bool showLeft: hostHovered && targetView && !targetView.atXBeginning
        readonly property bool showRight: hostHovered && targetView && !targetView.atXEnd
        Rectangle {
            width: 30; height: 30; radius: 15
            anchors.left: parent.left; anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.panel
            border.width: 1; border.color: leftMouse.containsMouse ? Theme.accentAlpha(0.5) : Theme.hairline
            opacity: arrows.showLeft ? 0.92 : 0
            visible: opacity > 0.01
            scale: leftMouse.pressed ? 0.9 : 1
            Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
            Behavior on scale { ScaleAnimator { duration: Theme.fast; easing.type: Easing.OutCubic } }
            Icon { anchors.centerIn: parent; name: "back"; size: 11; tint: Theme.text }
            MouseArea {
                id: leftMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (arrows.scroller && arrows.targetView) arrows.scroller.scrollTo(arrows.scroller.destination() - Math.max(240, arrows.targetView.width * 0.72))
            }
        }
        Rectangle {
            width: 30; height: 30; radius: 15
            anchors.right: parent.right; anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.panel
            border.width: 1; border.color: rightMouse.containsMouse ? Theme.accentAlpha(0.5) : Theme.hairline
            opacity: arrows.showRight ? 0.92 : 0
            visible: opacity > 0.01
            scale: rightMouse.pressed ? 0.9 : 1
            Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
            Behavior on scale { ScaleAnimator { duration: Theme.fast; easing.type: Easing.OutCubic } }
            Icon { anchors.centerIn: parent; name: "back"; size: 11; tint: Theme.text; rotation: 180 }
            MouseArea {
                id: rightMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: if (arrows.scroller && arrows.targetView) arrows.scroller.scrollTo(arrows.scroller.destination() + Math.max(240, arrows.targetView.width * 0.72))
            }
        }
    }

    opacity: 0
    Component.onCompleted: {
        if (online.lists.length === 0) online.reload()
        intro.start()
    }
    ParallelAnimation {
        id: intro
        NumberAnimation {
            target: page; property: "opacity"
            to: 1; duration: Theme.normal; easing.type: Easing.OutCubic
        }
    }

    Connections {
        target: online
        function onToast(t) { Nav.toast("info", t) }
        function onTracksReset() {
            page.sel = -1
            page.pinTop = true
            Qt.callLater(page.rewind)
            if (online.viewMode >= 1) searchRefresh.restart()
            else browseRefresh.restart()
        }
    }
    ParallelAnimation {
        id: searchRefresh
        OpacityAnimator { target: searchView; from: 0.35; to: 1; duration: Theme.normal; easing.type: Easing.OutCubic }
        NumberAnimation { target: searchShift; property: "y"; from: 10; to: 0; duration: 360; easing.type: Easing.OutQuint }
    }
    OpacityAnimator {
        id: browseRefresh
        target: trackList
        from: 0.45
        to: 1
        duration: Theme.fast
        easing.type: Easing.OutCubic
    }

    // ── 顶部：标题 / 源 / 音质 / 存储方式 / 账号 ──────────────────────
    Item {
        id: header
        x: page.sidePadding
        y: 34
        width: page.width - page.sidePadding * 2
        height: 38

        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 12
            Text {
                text: qsTr("在线曲库")
                color: Theme.text
                font.pixelSize: 26
                font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
            }
            Rectangle {
                width: 1; height: 16; color: Theme.hairline
                anchors.verticalCenter: parent.verticalCenter
            }
            // 源：定宽两段，选中底块滑过去，比逐段变色更像一个控件
            Rectangle {
                width: 132; height: 30; radius: 15
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.hover
                border.width: 1
                border.color: Theme.hairline

                Rectangle {
                    id: srcPill
                    x: 3 + online.source * 66
                    y: 3
                    width: 60; height: 24; radius: 12
                    color: page.dim
                    border.width: 1
                    border.color: Theme.accentAlpha(0.45)
                    Behavior on x { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
                    Behavior on color { ColorAnimation { duration: Theme.normal } }
                    Behavior on border.color { ColorAnimation { duration: Theme.normal } }
                }
                Row {
                    anchors.fill: parent
                    Repeater {
                        model: page.srcNames
                        delegate: Item {
                            required property int index
                            required property string modelData
                            width: 66; height: 30
                            Text {
                                anchors.centerIn: parent
                                text: modelData
                                font.pixelSize: 12
                                color: online.source === index ? Theme.accent
                                       : (srcSegMouse.containsMouse ? Theme.text : Theme.textSecondary)
                                Behavior on color { ColorAnimation { duration: Theme.fast } }
                            }
                            MouseArea {
                                id: srcSegMouse
                                anchors.fill: parent
                                hoverEnabled: !Theme.scrolling
                                cursorShape: Qt.PointingHandCursor
                                onClicked: online.source = index
                            }
                        }
                    }
                }
            }
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            // 音质
            Rectangle {
                width: 138; height: 30; radius: 15
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.hover
                border.width: 1
                border.color: Theme.hairline

                Rectangle {
                    id: qualPill
                    x: 3 + online.quality * 46
                    y: 3
                    width: 40; height: 24; radius: 12
                    color: page.dim
                    border.width: 1
                    border.color: Theme.accentAlpha(0.45)
                    Behavior on x { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
                    Behavior on color { ColorAnimation { duration: Theme.normal } }
                }
                Row {
                    anchors.fill: parent
                    Repeater {
                        model: page.qualNames
                        delegate: Item {
                            required property int index
                            required property string modelData
                            width: 46; height: 30
                            Text {
                                anchors.centerIn: parent
                                text: modelData
                                font.pixelSize: 11
                                color: online.quality === index ? Theme.accent
                                       : (qualSegMouse.containsMouse ? Theme.textSecondary : Theme.textTertiary)
                                Behavior on color { ColorAnimation { duration: Theme.fast } }
                            }
                            MouseArea {
                                id: qualSegMouse
                                anchors.fill: parent
                                hoverEnabled: !Theme.scrolling
                                cursorShape: Qt.PointingHandCursor
                                onClicked: online.quality = index
                            }
                        }
                    }
                }
            }

            // 存储方式：边听边存 / 听完即删（点一下就换，不再挂提示条）
            Rectangle {
                id: saveChip
                width: saveRow.implicitWidth + 20
                height: 30
                radius: 15
                anchors.verticalCenter: parent.verticalCenter
                color: online.autoSave ? page.dim
                                       : (saveMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.085) : Theme.hover)
                border.width: 1
                border.color: online.autoSave ? Theme.accentAlpha(0.45) : Theme.hairline
                Behavior on color { ColorAnimation { duration: Theme.normal } }
                Behavior on border.color { ColorAnimation { duration: Theme.normal } }

                Row {
                    id: saveRow
                    anchors.centerIn: parent
                    spacing: 7
                    Rectangle {
                        width: 24; height: 13; radius: 6.5
                        anchors.verticalCenter: parent.verticalCenter
                        color: online.autoSave ? page.accent : Qt.rgba(1, 1, 1, 0.16)
                        Behavior on color { ColorAnimation { duration: Theme.normal } }
                        Rectangle {
                            width: 9; height: 9; radius: 4.5
                            y: 2
                            x: online.autoSave ? 13 : 2
                            color: online.autoSave ? Theme.onAccent : Theme.textSecondary
                            Behavior on x { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: online.autoSave ? qsTr("边听边存") : qsTr("听完即删")
                        font.pixelSize: 11
                        color: online.autoSave ? Theme.accent : Theme.textSecondary
                        Behavior on color { ColorAnimation { duration: Theme.fast } }
                    }
                }
                MouseArea {
                    id: saveMouse
                    anchors.fill: parent
                    hoverEnabled: !Theme.scrolling
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        online.autoSave = !online.autoSave
                        Nav.toast("info", online.autoSave
                                  ? qsTr("边听边存：下载并入曲库")
                                  : qsTr("听完即删：播完自动清理，不进曲库"))
                    }
                }
            }

            // 账号：登录后是「首字头像 + 昵称」。点开是菜单，退出是菜单里的一项 ——
            // 点一下直接退出太反直觉，也没法看当前登的是谁
            Rectangle {
                id: accChip
                width: accRow.implicitWidth + 20
                height: 30
                radius: 15
                anchors.verticalCenter: parent.verticalCenter
                color: online.loggedIn ? page.dim
                                       : (accMouse.containsMouse ? Theme.hover : "transparent")
                border.width: 1
                border.color: online.loggedIn ? Theme.accentAlpha(0.45) : Theme.hairline
                Behavior on color { ColorAnimation { duration: Theme.normal } }
                Behavior on border.color { ColorAnimation { duration: Theme.normal } }

                Row {
                    id: accRow
                    anchors.centerIn: parent
                    spacing: 6
                    Rectangle {
                        width: 16; height: 16; radius: 8
                        anchors.verticalCenter: parent.verticalCenter
                        color: online.loggedIn ? page.accent : "transparent"
                        border.width: online.loggedIn ? 0 : 1
                        border.color: Theme.textTertiary
                        Behavior on color { ColorAnimation { duration: Theme.normal } }
                        Text {
                            anchors.centerIn: parent
                            text: online.loggedIn && online.nickname.length > 0
                                  ? online.nickname.charAt(0) : ""
                            color: Theme.onAccent
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: online.loggedIn ? online.nickname : qsTr("登录")
                        font.pixelSize: 11
                        color: online.loggedIn ? Theme.accent : Theme.textSecondary
                        Behavior on color { ColorAnimation { duration: Theme.fast } }
                    }
                }
                MouseArea {
                    id: accMouse
                    anchors.fill: parent
                    hoverEnabled: !Theme.scrolling
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (!online.loggedIn) { online.beginLogin(); return }
                        accMenu.opened ? accMenu.close() : accMenu.open()
                    }
                }
            }
        }
    }

    // ── 搜索框（交互语言向本地搜索对齐：强调色聚焦、一键清空、↑↓ 选歌、Esc 退回）────
    Rectangle {
        id: searchBox
        x: page.sidePadding
        y: header.y + header.height + 14
        width: Math.min(540, page.width - page.sidePadding * 2)
        height: 42
        radius: 21
        color: Theme.hover
        border.width: 1
        border.color: input.activeFocus ? Theme.accentAlpha(0.55) : Theme.hairline
        Behavior on border.color { ColorAnimation { duration: Theme.fast } }

        Icon {
            id: searchIcon
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            name: "search"
            size: 15
            tint: input.activeFocus ? Theme.accent : "transparent"
            opacity: input.activeFocus ? 0.95 : 0.45
        }
        TextInput {
            id: input
            anchors.left: searchIcon.right
            anchors.leftMargin: 10
            anchors.right: clearOrSpin.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.text
            selectionColor: Theme.accentAlpha(0.4)
            selectedTextColor: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: 14
            selectByMouse: true
            clip: true
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: opacity > 0.01
                opacity: (input.text.length === 0 && !input.activeFocus) ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                text: qsTr("搜索在线歌曲、歌手或专辑")
                color: Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 13
            }
            onTextChanged: page.sel = -1
            onAccepted: {
                if (online.viewMode >= 1 && page.sel >= 0 && page.sel < online.tracks.length) {
                    online.play(page.sel)
                } else {
                    online.search(text)
                }
            }
            Keys.onDownPressed: page.moveSel(1)
            Keys.onUpPressed: page.moveSel(-1)
            Keys.onEscapePressed: {
                if (text.length > 0) {
                    text = ""
                } else if (online.isDetail) {
                    online.back()
                } else if (online.viewMode === 1) {
                    online.backToLists()
                }
            }
        }
        Item {
            id: clearOrSpin
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 28; height: 28

            Icon {
                anchors.centerIn: parent
                name: "spinner"
                size: 13
                visible: online.busy
                opacity: 0.6
                RotationAnimator on rotation {
                    running: online.busy
                    loops: Animation.Infinite
                    from: 0
                    to: 360
                    duration: 900
                }
            }
            IconButton {
                anchors.centerIn: parent
                width: 28; height: 28
                icon: "close"
                iconSize: 12
                visible: !online.busy && input.text.length > 0
                opacity: visible ? 0.85 : 0
                Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                onClicked: {
                    input.text = ""
                    input.forceActiveFocus()
                    if (online.viewMode === 1) online.backToLists()
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            anchors.rightMargin: 36
            propagateComposedEvents: true
            cursorShape: Qt.IBeamCursor
            onClicked: (m) => { input.forceActiveFocus(); m.accepted = false }
        }
    }

    // 右侧只留返回入口，数量统一由列表内 Marker 表达，不重复报数
    Row {
        anchors.right: parent.right
        anchors.rightMargin: page.sidePadding
        anchors.verticalCenter: searchBox.verticalCenter
        spacing: 10

        Rectangle {
            visible: online.viewMode >= 1
            width: backRow.implicitWidth + 20
            height: 30; radius: 15
            anchors.verticalCenter: parent.verticalCenter
            color: backMouse.pressed ? Theme.pressed : (backMouse.containsMouse ? page.dim : Theme.hover)
            border.width: 1
            border.color: backMouse.containsMouse ? Theme.accentAlpha(0.4) : Theme.hairline
            Behavior on color { ColorAnimation { duration: Theme.fast } }
            Behavior on border.color { ColorAnimation { duration: Theme.fast } }
            Row {
                id: backRow
                anchors.centerIn: parent
                spacing: 5
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "back"
                    size: 10
                    tint: Theme.textSecondary
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: online.isDetail ? qsTr("返回") : qsTr("榜单")
                    color: Theme.textSecondary
                    font.pixelSize: 11
                }
            }
            MouseArea {
                id: backMouse
                anchors.fill: parent
                hoverEnabled: !Theme.scrolling
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (online.isDetail) {
                        online.back()
                    } else {
                        input.text = ""
                        online.backToLists()
                    }
                }
            }
        }
    }

    // 顶栏与内容之间的分隔线：左侧带一点强调色，随歌曲变色
    Rectangle {
        id: divider
        x: page.sidePadding
        y: searchBox.y + searchBox.height + 12
        width: page.width - page.sidePadding * 2
        height: 1
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Theme.accentAlpha(0.22) }
            GradientStop { position: 0.4; color: Theme.hairline }
            GradientStop { position: 1.0; color: "transparent" }
        }
    }

    // ── 浏览模式：左榜单 / 右歌曲 ────────────────────────────────────
    ListView {
        id: listCol
        x: page.sidePadding
        y: divider.y + 14
        width: 210
        height: page.height - y - page.bottomInset
        clip: true
        spacing: 1
        model: online.lists
        boundsBehavior: Flickable.StopAtBounds
        property real modeFade: online.viewMode === 0 ? 1 : 0
        Behavior on modeFade { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
        // 只跟模式走，不跟 contentFade：榜单本身没换，点一下不该整列暗一下再亮
        opacity: modeFade
        visible: opacity > 0.01
        SmoothScroll { flickable: listCol }

        delegate: Rectangle {
            required property int index
            required property var modelData
            width: listCol.width
            height: 36
            radius: 8
            color: listMouse.pressed ? Theme.pressed
                   : (online.listName === modelData.name ? page.faint
                      : (listMouse.containsMouse ? Theme.hover : "transparent"))
            Behavior on color { ColorAnimation { duration: Theme.fast } }

            // 选中标记：竖条从 0 长出来，配合底色一起变化，切换时不再"啪"地跳一下
            Rectangle {
                width: 2
                height: online.listName === modelData.name ? 14 : 0
                radius: 1
                x: 1
                anchors.verticalCenter: parent.verticalCenter
                color: page.accent
                opacity: online.listName === modelData.name ? 1 : 0
                Behavior on height { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
                Behavior on opacity { NumberAnimation { duration: Theme.fast } }
            }
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: modelData.name
                color: online.listName === modelData.name ? Theme.accent : Theme.textSecondary
                font.pixelSize: 12
                elide: Text.ElideRight
                Behavior on color { ColorAnimation { duration: Theme.fast } }
            }
            MouseArea {
                id: listMouse
                anchors.fill: parent
                hoverEnabled: !Theme.scrolling
                cursorShape: Qt.PointingHandCursor
                onClicked: online.openList(index)
            }
        }
    }

    Item {
        id: trackArea
        x: listCol.x + listCol.width + 26
        y: divider.y + 8
        width: page.width - x - page.sidePadding
        height: page.height - y - page.bottomInset
        property real modeFade: online.viewMode === 0 ? 1 : 0
        Behavior on modeFade { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
        opacity: modeFade
        visible: opacity > 0.01

        ListView {
            id: trackList
            anchors.fill: parent
            clip: true
            cacheBuffer: 600
            spacing: 2
            model: online.tracks
            boundsBehavior: Flickable.StopAtBounds
            SmoothScroll { flickable: trackList }

            delegate: TrackLine {
                required property int index
                required property var modelData
                rowIndex: index
                width: trackList.width
                title: modelData.title
                artist: modelData.artist || ""
                album: modelData.album || ""
                showAlbum: true
                duration: modelData.duration
                paywalled: modelData.paywalled
                active: online.playing === index
                playing: online.playing === index && player.playing
                pending: online.pending === index
                accent: page.accent
                fav: playlists.revision >= 0 && playlists.isFavoriteOnline(online.trackMeta(index).onlineKey || "")
                onPlayed: online.playing === index ? player.toggle() : online.play(index)
                onDownloaded: online.download(index)
                onFavorited: playlists.toggleFavoriteOnline(online.trackMeta(index))
                onMenuRequested: (px, py) => {
                    const m = online.trackMeta(index)
                    const p = mapToItem(null, px, py)
                    Nav.trackMenu(-1, p.x, p.y, { isOnline: true, onlineKey: m.onlineKey || "", onlineMeta: m, play: () => (online.playing === index ? player.toggle() : online.play(index)) })
                }
            }
        }

        // 空态：不摆一块空白，直接给下一步（加载中由骨架屏接手）
        Column {
            anchors.centerIn: parent
            spacing: 10
            visible: online.tracks.length === 0 && !online.busy
            Icon {
                anchors.horizontalCenter: parent.horizontalCenter
                name: "wave"
                size: 30
                opacity: 0.18
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("选一个榜单，或搜一首歌")
                color: Theme.textTertiary
                font.pixelSize: 12
            }
        }
    }

    // ── 搜索 / 详情模式：对齐本地 SearchPage 的排版与滚屏体验 ─────────────────
    ListView {
        id: searchView
        x: 0
        y: divider.y + 14
        width: page.width
        height: page.height - y - page.bottomInset
        leftMargin: page.sidePadding
        rightMargin: page.sidePadding
        readonly property real innerWidth: width - page.sidePadding * 2
        cacheBuffer: 900
        transform: Translate { id: searchShift }
        clip: true
        spacing: 2
        model: online.tracks
        boundsBehavior: Flickable.StopAtBounds
        property real modeFade: online.viewMode >= 1 ? 1 : 0
        Behavior on modeFade { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
        opacity: modeFade
        visible: opacity > 0.01
        SmoothScroll { flickable: searchView; onScrolled: page.pinTop = false }
        onMovementStarted: page.pinTop = false

        header: Item {
            width: searchView.innerWidth
            height: searchHead.height

            Column {
                id: searchHead
                width: parent.width
                topPadding: 10
                spacing: 0

                // ── 歌手 / 专辑 / 歌单 详情头 ──────────────────────────
                Rectangle {
                    id: detailCard
                    visible: online.isDetail
                    width: parent.width
                    height: visible ? (online.viewMode === 4 ? 184 : 156) : 0
                    radius: 18
                    color: Theme.hover
                    border.width: 1
                    border.color: Theme.hairline

                    SoftShadow { target: detailArt; spread: 18; offsetY: 10; strength: 0.5 }
                    RoundedImage {
                        id: detailArt
                        x: 22
                        anchors.verticalCenter: parent.verticalCenter
                        width: 108; height: 108
                        radius: online.detailRound ? 54 : 10
                        decodeSize: Math.ceil(108 * Math.max(1.0, Math.min(1.5, page.pxScale)))
                        source: online.detailCover
                    }
                    Column {
                        anchors.left: detailArt.right
                        anchors.leftMargin: 22
                        anchors.right: parent.right
                        anchors.rightMargin: 24
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 5
                        Text {
                            width: parent.width
                            text: online.detailTitle
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 24
                            font.weight: Font.Bold
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            visible: text.length > 0
                            text: online.detailSubtitle
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: online.detailMeta
                            color: Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        Row {
                            visible: online.viewMode === 4
                            spacing: 10
                            topPadding: 4
                            Rectangle {
                                width: 92; height: 30; radius: 15
                                color: page.accent
                                scale: playAllMouse.pressed ? 0.94 : (playAllMouse.containsMouse ? 1.04 : 1.0)
                                Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
                                Text {
                                    anchors.centerIn: parent
                                    text: qsTr("播放全部")
                                    color: Theme.onAccent
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                }
                                MouseArea {
                                    id: playAllMouse
                                    anchors.fill: parent
                                    hoverEnabled: !Theme.scrolling
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: online.playAll()
                                }
                            }
                            Rectangle {
                                width: 92; height: 30; radius: 15
                                color: dlAllMouse.containsMouse ? page.dim : "transparent"
                                border.width: 1
                                border.color: dlAllMouse.containsMouse ? Theme.accentAlpha(0.4) : Theme.hairline
                                Behavior on color { ColorAnimation { duration: Theme.fast } }
                                Text {
                                    anchors.centerIn: parent
                                    text: qsTr("下载全部")
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    id: dlAllMouse
                                    anchors.fill: parent
                                    hoverEnabled: !Theme.scrolling
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: online.downloadAll()
                                }
                            }
                        }
                    }
                }

                Item { width: 1; height: detailCard.visible ? 18 : 0 }

                // 歌单工具条：排序 / 倒序 / 列表内筛选
                Row {
                    width: parent.width
                    height: visible ? 32 : 0
                    spacing: 8
                    visible: online.viewMode === 4

                    Repeater {
                        model: [{ t: qsTr("默认"), k: 0 }, { t: qsTr("歌名"), k: 1 },
                                { t: qsTr("歌手"), k: 2 }, { t: qsTr("时长"), k: 3 }]
                        delegate: Rectangle {
                            required property int index
                            required property var modelData
                            width: 56; height: 30; radius: 15
                            color: online.sortKey === modelData.k ? Theme.accentAlpha(0.16)
                                                                 : (sortMouse.containsMouse ? Theme.hover : "transparent")
                            border.width: 1
                            border.color: online.sortKey === modelData.k ? Theme.accentAlpha(0.5) : Theme.hairline
                            Behavior on color { ColorAnimation { duration: Theme.fast } }
                            Text {
                                anchors.centerIn: parent
                                text: modelData.t
                                color: online.sortKey === modelData.k ? Theme.accent : Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }
                            MouseArea {
                                id: sortMouse
                                anchors.fill: parent
                                hoverEnabled: !Theme.scrolling
                                cursorShape: Qt.PointingHandCursor
                                onClicked: online.setSort(modelData.k, online.sortDesc)
                            }
                        }
                    }

                    Rectangle {
                        width: 68; height: 30; radius: 15
                        color: online.sortDesc ? Theme.accentAlpha(0.16)
                                               : (descMouse.containsMouse ? Theme.hover : "transparent")
                        border.width: 1
                        border.color: online.sortDesc ? Theme.accentAlpha(0.5) : Theme.hairline
                        Behavior on color { ColorAnimation { duration: Theme.fast } }
                        Text {
                            anchors.centerIn: parent
                            text: online.sortDesc ? qsTr("↓ 倒序") : qsTr("↑ 正序")
                            color: online.sortDesc ? Theme.accent : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                        MouseArea {
                            id: descMouse
                            anchors.fill: parent
                            hoverEnabled: !Theme.scrolling
                            cursorShape: Qt.PointingHandCursor
                            onClicked: online.setSort(online.sortKey, !online.sortDesc)
                        }
                    }

                    Rectangle {
                        width: 184; height: 30; radius: 15
                        color: Theme.hover
                        border.width: 1
                        border.color: filterInput.activeFocus ? Theme.accentAlpha(0.5) : Theme.hairline
                        Behavior on border.color { ColorAnimation { duration: Theme.fast } }
                        TextInput {
                            id: filterInput
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: filterClear.visible ? 26 : 12
                            verticalAlignment: TextInput.AlignVCenter
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            clip: true
                            onTextChanged: filterDebounce.restart()
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: filterInput.text.length === 0
                                text: qsTr("筛选曲目")
                                color: Theme.textTertiary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }
                        }
                        IconButton {
                            id: filterClear
                            anchors.right: parent.right
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            width: 22; height: 22
                            icon: "close"
                            iconSize: 10
                            visible: filterInput.text.length > 0
                            onClicked: filterInput.text = ""
                        }
                        Timer {
                            id: filterDebounce
                            interval: 220
                            onTriggered: online.setFilter(filterInput.text)
                        }
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        leftPadding: 4
                        visible: online.filterText.length > 0
                        text: qsTr("%1 / %2").arg(online.tracks.length).arg(online.baseCount)
                        color: Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }
                }

                Item { width: 1; height: online.viewMode === 4 ? 14 : 0 }

                // ── 最佳匹配 + 歌手带（宽屏按 SearchPage 196px 并排，窄屏自动叠放）──
                Item {
                    id: heroBox
                    visible: online.viewMode === 1 && (page.best !== null || online.artists.length > 0)
                    width: parent.width
                    readonly property bool wideHero: width >= 780
                    readonly property int bestW: wideHero
                                                 ? (online.artists.length > 0 ? Math.min(420, Math.round(width * 0.42)) : Math.min(540, width))
                                                 : width
                    readonly property int bestH: wideHero ? 196 : 132
                    readonly property int artistH: wideHero ? 196 : (online.artists.length > 0 ? 54 : 0)
                    height: !visible ? 0 : (wideHero
                                            ? Math.max(page.best ? bestH : 0, online.artists.length > 0 ? artistH : 0)
                                            : (page.best ? bestH : 0) + (page.best && online.artists.length > 0 ? 14 : 0) + artistH)

                    Rectangle {
                        id: bestCard
                        visible: page.best !== null
                        x: 0; y: 0
                        width: heroBox.bestW
                        height: heroBox.bestH
                        radius: 18
                        color: bestMouse.pressed ? Theme.pressed : (bestMouse.containsMouse ? Theme.pressed : Theme.hover)
                        border.width: 1
                        border.color: Theme.hairline
                        Behavior on color { ColorAnimation { duration: Theme.fast } }

                        SoftShadow { target: bestArt; spread: 16; offsetY: 8; strength: 0.5 }
                        RoundedImage {
                            id: bestArt
                            x: 22
                            y: heroBox.wideHero ? 22 : 18
                            width: heroBox.wideHero ? 92 : 96
                            height: heroBox.wideHero ? 92 : 96
                            radius: 10
                            decodeSize: Math.ceil(96 * Math.max(1.0, Math.min(1.5, page.pxScale)))
                            source: online.bestCover || (page.best && page.best.cover ? page.best.cover : "")
                            Icon {
                                anchors.centerIn: parent
                                name: "note"
                                size: 28
                                opacity: 0.28
                                visible: bestArt.status !== Image.Ready
                            }
                        }

                        // 宽屏 196px：与 SearchPage topCard 同构，文字沉底、右下浮现播放+下载
                        Column {
                            x: heroBox.wideHero ? 22 : bestArt.x + bestArt.width + 18
                            anchors.bottom: heroBox.wideHero ? parent.bottom : undefined
                            anchors.bottomMargin: heroBox.wideHero ? 20 : 0
                            anchors.verticalCenter: heroBox.wideHero ? undefined : parent.verticalCenter
                            width: heroBox.wideHero ? (parent.width - 44 - 102) : (parent.width - x - 114)
                            spacing: 4

                            Text {
                                width: parent.width
                                text: page.best
                                      ? (online.keyword.length ? Nav.highlight(page.best.title, online.keyword, Theme.accent) : page.best.title)
                                      : ""
                                textFormat: page.best && online.keyword.length ? Text.StyledText : Text.PlainText
                                color: Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: heroBox.wideHero ? 23 : 20
                                font.weight: Font.Bold
                                elide: Text.ElideRight
                            }
                            Row {
                                width: parent.width
                                spacing: 6
                                Icon {
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: "note"
                                    size: 13
                                    opacity: 0.48
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - 20
                                    readonly property string subLine: page.best
                                        ? (page.best.artist
                                           + (page.best.album.length ? " · " + page.best.album : "")
                                           + (page.best.duration.length ? " · " + page.best.duration : ""))
                                        : ""
                                    text: page.best && online.keyword.length
                                          ? Nav.highlight(subLine, online.keyword, Theme.accent)
                                          : subLine
                                    textFormat: page.best && online.keyword.length ? Text.StyledText : Text.PlainText
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 13
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        // 整卡点击区在操作按钮下方，防止挡住下载键
                        MouseArea {
                            id: bestMouse
                            anchors.fill: parent
                            hoverEnabled: !Theme.scrolling
                            cursorShape: Qt.PointingHandCursor
                            onClicked: online.playing === 0 ? player.toggle() : online.play(0)
                        }

                        Row {
                            id: bestActions
                            anchors.right: parent.right
                            anchors.rightMargin: 18
                            anchors.bottom: heroBox.wideHero ? parent.bottom : undefined
                            anchors.bottomMargin: heroBox.wideHero ? 18 : 0
                            anchors.verticalCenter: heroBox.wideHero ? undefined : parent.verticalCenter
                            spacing: 8
                            readonly property bool reveal: !heroBox.wideHero || bestMouse.containsMouse || bestPlay.containsMouse || bestDl.containsMouse || online.playing === 0 || online.pending === 0

                            Rectangle {
                                width: 34; height: 34; radius: 17
                                anchors.verticalCenter: parent.verticalCenter
                                color: bestDl.containsMouse ? page.dim : Theme.hover
                                border.width: 1
                                border.color: bestDl.containsMouse ? Theme.accentAlpha(0.4) : Theme.hairline
                                opacity: bestActions.reveal ? 1 : 0
                                scale: bestDl.pressed ? 0.9 : (bestActions.reveal ? 1 : 0.85)
                                Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                                Behavior on scale { ScaleAnimator { duration: Theme.fast; easing.type: Easing.OutCubic } }
                                Behavior on color { ColorAnimation { duration: Theme.fast } }
                                Icon {
                                    anchors.centerIn: parent
                                    name: "import"
                                    size: 13
                                    opacity: 0.78
                                    tint: bestDl.containsMouse ? Theme.accent : Theme.textSecondary
                                }
                                MouseArea {
                                    id: bestDl
                                    anchors.fill: parent
                                    hoverEnabled: !Theme.scrolling
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: online.download(0)
                                }
                            }

                            Rectangle {
                                width: heroBox.wideHero ? 48 : 42
                                height: heroBox.wideHero ? 48 : 42
                                radius: width / 2
                                color: online.pending === 0 ? page.dim : page.accent
                                opacity: bestActions.reveal ? 1 : 0
                                scale: bestPlay.pressed ? 0.9 : (bestActions.reveal ? (bestPlay.containsMouse ? 1.06 : 1.0) : 0.8)
                                Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
                                Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutBack } }
                                Behavior on color { ColorAnimation { duration: Theme.normal } }
                                Icon {
                                    anchors.centerIn: parent
                                    visible: online.pending !== 0
                                    name: online.playing === 0 && player.playing ? "pause" : "play"
                                    size: heroBox.wideHero ? 16 : 14
                                    tint: Theme.onAccent
                                }
                                Icon {
                                    anchors.centerIn: parent
                                    visible: online.pending === 0
                                    name: "spinner"
                                    size: 15
                                    tint: page.accent
                                    RotationAnimator on rotation {
                                        running: online.pending === 0
                                        loops: Animation.Infinite
                                        from: 0; to: 360; duration: 800
                                    }
                                }
                                MouseArea {
                                    id: bestPlay
                                    anchors.fill: parent
                                    hoverEnabled: !Theme.scrolling
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: if (online.pending !== 0) (online.playing === 0 ? player.toggle() : online.play(0))
                                }
                            }
                        }
                    }

                    // ── 歌手横向带（宽屏大圆头像与 SearchPage 同构；窄屏退化为胶囊）──
                    Item {
                        id: artistHost
                        visible: online.artists.length > 0
                        x: heroBox.wideHero && bestCard.visible ? bestCard.width + 28 : 0
                        y: heroBox.wideHero ? 0 : (bestCard.visible ? bestCard.height + 14 : 0)
                        width: parent.width - x
                        height: heroBox.artistH

                        HoverHandler { id: artistHover }
                        StripArrows { targetView: artistStrip; scroller: artistScroller; hostHovered: artistHover.hovered }

                        ListView {
                            id: artistStrip
                            anchors.fill: parent
                            orientation: ListView.Horizontal
                            spacing: heroBox.wideHero ? 18 : 10
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            model: online.artists
                            SmoothScroll { id: artistScroller; flickable: artistStrip; horizontal: true; horizontalWheelOnly: true }

                            delegate: Item {
                                required property int index
                                required property var modelData
                                readonly property bool wide: heroBox.wideHero
                                readonly property bool hovered: artistMouse.containsMouse
                                width: wide ? 116 : chipContent.implicitWidth
                                height: wide ? artistStrip.height : 42
                                y: wide ? 0 : 6

                                // 宽屏样式：圆形头像/首字母圆徽 + 居中名字
                                Item {
                                    id: avatarBox
                                    visible: wide
                                    x: 0
                                    y: 20 + (hovered ? -3 : 0)
                                    width: 116; height: 116
                                    Behavior on y { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }

                                    SoftShadow {
                                        target: avatarImg
                                        visible: (modelData.cover || "").length > 0
                                        spread: 14; offsetY: 8; strength: hovered ? 0.85 : 0.4
                                    }
                                    RoundedImage {
                                        id: avatarImg
                                        anchors.fill: parent
                                        visible: (modelData.cover || "").length > 0
                                        radius: 58
                                        decodeSize: Math.ceil(116 * Math.max(1.0, Math.min(1.5, page.pxScale)))
                                        source: modelData.cover || ""
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        visible: (modelData.cover || "").length === 0
                                        radius: 58
                                        color: hovered ? Theme.accentAlpha(0.16) : Theme.accentAlpha(0.09)
                                        border.width: 1
                                        border.color: hovered ? Theme.accentAlpha(0.45) : Theme.hairline
                                        Behavior on color { ColorAnimation { duration: Theme.fast } }
                                        Behavior on border.color { ColorAnimation { duration: Theme.fast } }
                                        Text {
                                            anchors.centerIn: parent
                                            text: modelData.name ? modelData.name.charAt(0) : ""
                                            color: Theme.accent
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 34
                                            font.weight: Font.DemiBold
                                        }
                                    }
                                }
                                Text {
                                    visible: wide
                                    y: 148
                                    width: parent.width
                                    horizontalAlignment: Text.AlignHCenter
                                    text: online.keyword.length ? Nav.highlight(modelData.name, online.keyword, Theme.accent) : modelData.name
                                    textFormat: online.keyword.length ? Text.StyledText : Text.PlainText
                                    elide: Text.ElideRight
                                    color: hovered ? Theme.accent : Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    Behavior on color { ColorAnimation { duration: Theme.fast } }
                                }

                                // 窄屏降级：对称胶囊
                                Rectangle {
                                    visible: !wide
                                    anchors.fill: parent
                                    radius: 21
                                    color: hovered ? page.faint : Theme.hover
                                    border.width: 1
                                    border.color: hovered ? Theme.accentAlpha(0.4) : Theme.hairline
                                    Behavior on color { ColorAnimation { duration: Theme.fast } }
                                    Row {
                                        id: chipContent
                                        anchors.centerIn: parent
                                        spacing: 10
                                        leftPadding: 6
                                        rightPadding: 14
                                        Rectangle {
                                            width: 30; height: 30; radius: 15
                                            anchors.verticalCenter: parent.verticalCenter
                                            color: page.dim
                                            RoundedImage {
                                                anchors.fill: parent
                                                visible: (modelData.cover || "").length > 0
                                                radius: 15
                                                decodeSize: 64
                                                source: modelData.cover || ""
                                            }
                                            Text {
                                                anchors.centerIn: parent
                                                visible: (modelData.cover || "").length === 0
                                                text: modelData.name ? modelData.name.charAt(0) : ""
                                                color: Theme.accent
                                                font.family: Theme.fontFamily
                                                font.pixelSize: 12
                                                font.weight: Font.DemiBold
                                            }
                                        }
                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: Math.min(implicitWidth, 148)
                                            text: online.keyword.length ? Nav.highlight(modelData.name, online.keyword, Theme.accent) : modelData.name
                                            textFormat: online.keyword.length ? Text.StyledText : Text.PlainText
                                            color: hovered ? Theme.accent : Theme.text
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 12
                                            elide: Text.ElideRight
                                        }
                                    }
                                }

                                MouseArea {
                                    id: artistMouse
                                    anchors.fill: parent
                                    hoverEnabled: !Theme.scrolling
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: online.openArtist(modelData.id || "", modelData.name)
                                }
                            }
                        }
                    }
                }

                // ── 歌单带 ────────────────────────────────────────────
                Item { width: 1; height: online.viewMode === 1 && online.playlists.length > 0 ? 26 : 0 }
                Marker {
                    want: online.viewMode === 1 && online.playlists.length > 0
                    icon: "playlist"
                    count: online.playlists.length
                }
                Item { width: 1; height: online.viewMode === 1 && online.playlists.length > 0 ? 10 : 0 }
                Item {
                    id: plHost
                    visible: online.viewMode === 1 && online.playlists.length > 0
                    width: parent.width
                    height: visible ? 228 : 0
                    HoverHandler { id: plHover }
                    StripArrows { targetView: playlistStrip; scroller: plScroller; hostHovered: plHover.hovered }

                    ListView {
                        id: playlistStrip
                        anchors.fill: parent
                        orientation: ListView.Horizontal
                        spacing: 20
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        model: online.playlists
                        SmoothScroll { id: plScroller; flickable: playlistStrip; horizontal: true; horizontalWheelOnly: true }
                        delegate: Item {
                            required property int index
                            required property var modelData
                            width: 156
                            height: 228
                            readonly property bool hovered: plMouse.containsMouse

                            SoftShadow { target: plCoverBox; spread: 16; offsetY: 8; strength: parent.hovered ? 0.85 : 0.45 }
                            Item {
                                id: plCoverBox
                                width: 156
                                height: 156
                                y: parent.hovered ? -3 : 0
                                Behavior on y { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }

                                RoundedImage {
                                    anchors.fill: parent
                                    radius: 8
                                    decodeSize: Math.ceil(156 * Math.max(1.0, Math.min(1.5, page.pxScale)))
                                    source: modelData.cover
                                }
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    height: 46
                                    radius: 8
                                    visible: modelData.playCount > 0
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: "transparent" }
                                        GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.56) }
                                    }
                                }
                                Row {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 10
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 8
                                    spacing: 4
                                    visible: modelData.playCount > 0
                                    Icon {
                                        anchors.verticalCenter: parent.verticalCenter
                                        name: "play"
                                        size: 9
                                        tint: "white"
                                        opacity: 0.88
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: page.fmtCount(modelData.playCount)
                                        color: "white"
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 11
                                    }
                                }
                            }

                            Column {
                                y: 164
                                width: parent.width
                                spacing: 3
                                Text {
                                    width: parent.width
                                    text: online.keyword.length ? Nav.highlight(modelData.name, online.keyword, Theme.accent) : modelData.name
                                    textFormat: online.keyword.length ? Text.StyledText : Text.PlainText
                                    color: plMouse.containsMouse ? Theme.accent : Theme.text
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    lineHeight: 1.2
                                    wrapMode: Text.Wrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                }
                                Text {
                                    width: parent.width
                                    readonly property string plSub: (modelData.creator && modelData.creator.length ? modelData.creator + " · " : "")
                                                                  + qsTr("%1 首").arg(modelData.count)
                                    text: plSub
                                    color: Theme.textTertiary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }
                            MouseArea {
                                id: plMouse
                                anchors.fill: parent
                                hoverEnabled: !Theme.scrolling
                                cursorShape: Qt.PointingHandCursor
                                onClicked: online.openPlaylist(modelData.id || "", modelData.name)
                            }
                        }
                    }
                }

                // ── 专辑带（规格对齐 AlbumStrip.qml：150px 封面 + SoftShadow + 8px 圆角）──
                Item { width: 1; height: online.viewMode === 1 && online.albums.length > 0 ? 26 : 0 }
                Marker {
                    want: online.viewMode === 1 && online.albums.length > 0
                    icon: "album-open"
                    count: online.albums.length
                }
                Item { width: 1; height: online.viewMode === 1 && online.albums.length > 0 ? 10 : 0 }
                Item {
                    id: albHost
                    visible: online.viewMode === 1 && online.albums.length > 0
                    width: parent.width
                    height: visible ? 212 : 0
                    HoverHandler { id: albHover }
                    StripArrows { targetView: albumStrip; scroller: albScroller; hostHovered: albHover.hovered }

                    ListView {
                        id: albumStrip
                        anchors.fill: parent
                        orientation: ListView.Horizontal
                        spacing: 22
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        model: online.albums
                        SmoothScroll { id: albScroller; flickable: albumStrip; horizontal: true; horizontalWheelOnly: true }
                        delegate: Item {
                            required property int index
                            required property var modelData
                            width: 150
                            height: 212
                            readonly property bool hovered: albMouse.containsMouse

                            SoftShadow { target: albArt; spread: 16; offsetY: 8; strength: parent.hovered ? 0.85 : 0.45 }
                            RoundedImage {
                                id: albArt
                                width: 150; height: 150
                                radius: 8
                                y: parent.hovered ? -3 : 0
                                decodeSize: Math.ceil(150 * Math.max(1.0, Math.min(1.5, page.pxScale)))
                                source: modelData.cover
                                Behavior on y { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                            }
                            Text {
                                y: 160
                                width: parent.width
                                text: online.keyword.length ? Nav.highlight(modelData.name, online.keyword, Theme.accent) : modelData.name
                                textFormat: online.keyword.length ? Text.StyledText : Text.PlainText
                                color: parent.hovered ? Theme.accent : Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Text {
                                y: 180
                                width: parent.width
                                text: modelData.artist
                                color: Theme.textTertiary
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                            MouseArea {
                                id: albMouse
                                anchors.fill: parent
                                hoverEnabled: !Theme.scrolling
                                cursorShape: Qt.PointingHandCursor
                                onClicked: online.openAlbum(modelData.id || "", modelData.name)
                            }
                        }
                    }
                }

                // ── 歌曲 ──────────────────────────────────────────────
                Item { width: 1; height: online.tracks.length > 0 ? 24 : 0 }
                Marker {
                    want: online.tracks.length > 0
                    icon: "note"
                    count: online.tracks.length
                }
                Item { width: 1; height: online.tracks.length > 0 ? 6 : 0 }
            }
        }

        delegate: TrackLine {
            required property int index
            required property var modelData
            rowIndex: index
            width: searchView.innerWidth
            title: modelData.title
            artist: modelData.artist || ""
            album: modelData.album || ""
            showAlbum: online.viewMode !== 3
            highlight: online.viewMode === 1 ? online.keyword : online.filterText
            keyFocus: page.sel === index
            duration: modelData.duration
            paywalled: modelData.paywalled
            active: online.playing === index
            playing: online.playing === index && player.playing
            pending: online.pending === index
            accent: page.accent
            fav: playlists.revision >= 0 && playlists.isFavoriteOnline(online.trackMeta(index).onlineKey || "")
            onPlayed: online.playing === index ? player.toggle() : online.play(index)
            onDownloaded: online.download(index)
            onFavorited: playlists.toggleFavoriteOnline(online.trackMeta(index))
            onMenuRequested: (px, py) => {
                const m = online.trackMeta(index)
                const p = mapToItem(null, px, py)
                Nav.trackMenu(-1, p.x, p.y, { isOnline: true, onlineKey: m.onlineKey || "", onlineMeta: m, play: () => (online.playing === index ? player.toggle() : online.play(index)) })
            }
        }

        footer: Item {
            width: searchView.innerWidth
            height: online.canLoadMore ? 56 : 12
            Rectangle {
                visible: online.canLoadMore
                width: 132; height: 32; radius: 16
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 8
                color: moreMouse.pressed ? Theme.pressed : (moreMouse.containsMouse ? page.dim : "transparent")
                border.width: 1
                border.color: moreMouse.containsMouse ? Theme.accentAlpha(0.45) : Theme.hairline
                Behavior on color { ColorAnimation { duration: Theme.fast } }
                Text {
                    anchors.centerIn: parent
                    text: qsTr("加载更多")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                }
                MouseArea {
                    id: moreMouse
                    anchors.fill: parent
                    hoverEnabled: !Theme.scrolling
                    cursorShape: Qt.PointingHandCursor
                    onClicked: online.loadMore()
                }
            }
        }
    }

    // 无结果空态（对齐 SearchPage 的图标 + 提示布局）
    Column {
        anchors.centerIn: searchView
        spacing: 12
        visible: online.isSearch && online.tracks.length === 0 && !online.busy && online.keyword.length > 0
        Icon {
            anchors.horizontalCenter: parent.horizontalCenter
            name: "search"
            size: 48
            opacity: 0.14
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("没有找到与「%1」相关的结果").arg(online.keyword)
            color: Theme.textTertiary
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("可以检查拼写，或切换上方音乐源重试")
            color: Theme.textTertiary
            font.family: Theme.fontFamily
            font.pixelSize: 12
            opacity: 0.75
        }
    }

    // ── 骨架屏：按当前视图形态占位（搜索态模仿 Hero + 歌手圆 + 列表）──────────
    Item {
        id: skeleton
        x: online.viewMode === 0 ? trackArea.x : page.sidePadding
        y: divider.y + 14
        width: online.viewMode === 0 ? trackArea.width : page.width - page.sidePadding * 2
        height: page.height - y - page.bottomInset
        property bool want: online.busy && online.tracks.length === 0
        visible: opacity > 0.01
        opacity: 0
        Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
        Component.onCompleted: opacity = want ? 1 : 0
        onWantChanged: opacity = want ? 1 : 0
        clip: true

        Item {
            id: breath
            anchors.fill: parent
            SequentialAnimation on opacity {
                running: skeleton.want
                onRunningChanged: if (!running) breath.opacity = 1
                loops: Animation.Infinite
                NumberAnimation { to: 0.45; duration: 700; easing.type: Easing.InOutSine }
                NumberAnimation { to: 0.9; duration: 700; easing.type: Easing.InOutSine }
            }

            Column {
                width: parent.width
                topPadding: online.viewMode >= 1 ? 10 : 0
                spacing: 0

                // 搜索/详情顶部卡片骨架
                Row {
                    visible: online.viewMode === 1
                    width: parent.width
                    height: visible ? (skeleton.width >= 780 ? 196 : 132) : 0
                    spacing: 28
                    Rectangle {
                        width: skeleton.width >= 780 ? Math.min(420, Math.round(skeleton.width * 0.42)) : skeleton.width
                        height: parent.height
                        radius: 18
                        color: Qt.rgba(1, 1, 1, 0.04)
                        Rectangle {
                            x: 22; y: 22
                            width: skeleton.width >= 780 ? 92 : 84
                            height: width; radius: 10
                            color: Qt.rgba(1, 1, 1, 0.05)
                        }
                        Rectangle {
                            x: skeleton.width >= 780 ? 22 : 124
                            y: skeleton.width >= 780 ? parent.height - 54 : 38
                            width: Math.min(parent.width * 0.52, 200)
                            height: 14; radius: 7
                            color: Qt.rgba(1, 1, 1, 0.06)
                        }
                        Rectangle {
                            x: skeleton.width >= 780 ? 22 : 124
                            y: skeleton.width >= 780 ? parent.height - 32 : 64
                            width: Math.min(parent.width * 0.36, 130)
                            height: 10; radius: 5
                            color: Qt.rgba(1, 1, 1, 0.035)
                        }
                    }
                    Row {
                        visible: skeleton.width >= 780
                        spacing: 18
                        Repeater {
                            model: 4
                            delegate: Column {
                                width: 116
                                topPadding: 20
                                spacing: 12
                                Rectangle { width: 116; height: 116; radius: 58; color: Qt.rgba(1, 1, 1, 0.04) }
                                Rectangle { anchors.horizontalCenter: parent.horizontalCenter; width: 64; height: 10; radius: 5; color: Qt.rgba(1, 1, 1, 0.04) }
                            }
                        }
                    }
                }
                Rectangle {
                    visible: online.isDetail
                    width: parent.width
                    height: visible ? 156 : 0
                    radius: 18
                    color: Qt.rgba(1, 1, 1, 0.04)
                }
                Item { width: 1; height: online.viewMode >= 1 ? 26 : 0 }

                Column {
                    width: parent.width
                    spacing: 2
                    Repeater {
                        model: online.viewMode >= 1 ? 6 : 8
                        delegate: Item {
                            required property int index
                            width: skeleton.width
                            height: 52

                            Rectangle {
                                x: 19
                                anchors.verticalCenter: parent.verticalCenter
                                width: 12; height: 8; radius: 4
                                color: Qt.rgba(1, 1, 1, 0.032)
                            }
                            Column {
                                x: 46
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 7
                                Rectangle {
                                    width: Math.min(skeleton.width * 0.32, 150 + (index % 3) * 54)
                                    height: 10; radius: 5
                                    color: Qt.rgba(1, 1, 1, 0.055)
                                }
                                Rectangle {
                                    width: Math.min(skeleton.width * 0.2, 88 + (index % 2) * 46)
                                    height: 8; radius: 4
                                    color: Qt.rgba(1, 1, 1, 0.03)
                                }
                            }
                            Rectangle {
                                x: parent.width - 132
                                anchors.verticalCenter: parent.verticalCenter
                                width: 26; height: 8; radius: 4
                                color: Qt.rgba(1, 1, 1, 0.028)
                            }
                            Repeater {
                                model: 2
                                delegate: Rectangle {
                                    required property int index
                                    x: parent.width - 82 + index * 34
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 28; height: 28; radius: 14
                                    color: Qt.rgba(1, 1, 1, 0.026)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ── 状态条 / 进度 ─────────────────────────────────────────────────
    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: page.sidePadding
        anchors.rightMargin: page.sidePadding
        anchors.bottom: parent.bottom
        anchors.bottomMargin: page.bottomInset - 44
        height: 20
        opacity: (online.busy || online.status.length > 0 || online.progress > 0) ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }

        Rectangle {
            id: dot
            width: 7; height: 7; radius: 3.5
            anchors.verticalCenter: parent.verticalCenter
            color: page.accent
            visible: online.busy
            SequentialAnimation on opacity {
                running: online.busy
                loops: Animation.Infinite
                NumberAnimation { to: 0.25; duration: 600 }
                NumberAnimation { to: 1.0; duration: 600 }
            }
        }
        Text {
            anchors.left: dot.right
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: online.status
            color: Theme.textTertiary
            font.pixelSize: 11
        }
        Rectangle {
            visible: online.progress > 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 2
            color: Theme.hairline
            Rectangle {
                width: parent.width * online.progress / 100
                height: parent.height
                color: page.accent
                Behavior on width { NumberAnimation { duration: 160 } }
            }
        }
    }

    // ── 账号菜单 ──────────────────────────────────────────────────────
    Item {
        id: accMenu
        anchors.fill: parent
        z: 45
        visible: false
        property bool opened: false
        opacity: 0
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }

        function open() {
            opened = true
            visible = true
            opacity = 1
            accPanel.scale = 1
        }
        function close() {
            opened = false
            opacity = 0
            accPanel.scale = 0.96
            hideTimer.restart()
        }
        Timer {
            id: hideTimer
            interval: Theme.fast + 20
            onTriggered: if (!accMenu.opened) accMenu.visible = false
        }

        // 点空白处关闭
        MouseArea { anchors.fill: parent; onClicked: accMenu.close() }

        Rectangle {
            id: accPanel
            width: 210
            height: menuCol.height + 20
            radius: 12
            color: Theme.panel
            border.width: 1
            border.color: Theme.hairline
            scale: 0.96
            transformOrigin: Item.TopRight
            Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
            x: Math.max(8, Math.min(page.width - width - 8,
                                    accChip.mapToItem(page, 0, 0).x + accChip.width - width))
            y: accChip.mapToItem(page, 0, 0).y + accChip.height + 8

            Column {
                id: menuCol
                x: 10
                y: 10
                width: parent.width - 20
                spacing: 2

                Item {
                    width: parent.width
                    height: 44
                    Rectangle {
                        id: menuAvatar
                        x: 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: 30; height: 30; radius: 15
                        color: page.accent
                        Text {
                            anchors.centerIn: parent
                            text: online.nickname.length > 0 ? online.nickname.charAt(0) : ""
                            color: Theme.onAccent
                            font.pixelSize: 13
                            font.weight: Font.Bold
                        }
                    }
                    Column {
                        anchors.left: menuAvatar.right
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            width: parent.width
                            text: online.nickname
                            color: Theme.text
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        Text {
                            text: qsTr("酷狗账号")
                            color: Theme.textTertiary
                            font.pixelSize: 10
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Theme.hairline
                }

                Rectangle {
                    width: parent.width
                    height: 34
                    radius: 8
                    color: exitMouse.containsMouse ? Theme.hover : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.fast } }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("退出登录")
                        color: exitMouse.containsMouse ? Theme.text : Theme.textSecondary
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: exitMouse
                        anchors.fill: parent
                        hoverEnabled: !Theme.scrolling
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            accMenu.close()
                            online.logout()
                        }
                    }
                }
            }
        }
    }

    // ── 断网提示 ──────────────────────────────────────────────────────
    // 关掉联网后整页都是"点不动的"——与其让每个按钮默默失败，不如直接说清楚
    Item {
        anchors.fill: parent
        visible: settings.offline
        z: 30
        // 重新打开联网后自动把榜单拉起来，不用切页或重启
        onVisibleChanged: if (!visible && online.lists.length === 0) online.reload()

        MouseArea { anchors.fill: parent }   // 吃掉点击，避免误触下面的列表

        Column {
            anchors.centerIn: parent
            spacing: 10
            Icon {
                anchors.horizontalCenter: parent.horizontalCenter
                name: "cloud"
                size: 34
                opacity: 0.16
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("已关闭联网")
                color: Theme.textSecondary
                font.pixelSize: 15
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("在线曲库、在线歌词、联网补封面都已停用")
                color: Theme.textTertiary
                font.pixelSize: 12
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("在「设置 → 联网」里重新打开")
                color: Theme.textTertiary
                font.pixelSize: 12
            }
        }
    }

    // ── 扫码浮层 ──────────────────────────────────────────────────────
    Item {
        id: qrLayer
        anchors.fill: parent
        z: 50
        opacity: online.loginVisible ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }

        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.6)
            MouseArea { anchors.fill: parent; onClicked: online.cancelLogin() }
        }
        Rectangle {
            width: 300; height: 372; radius: 16
            anchors.centerIn: parent
            color: Theme.panel
            border.width: 1
            border.color: Theme.hairline
            scale: online.loginVisible ? 1 : 0.96
            Behavior on scale { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }

            Rectangle {
                anchors.top: parent.top
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.topMargin: -1
                width: 90; height: 2; radius: 1
                color: page.accent
                opacity: 0.7
            }

            Column {
                anchors.centerIn: parent
                spacing: 14
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("扫码登录酷狗")
                    color: Theme.text
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
                Rectangle {
                    width: 200; height: 200; radius: 8
                    color: "#ffffff"
                    anchors.horizontalCenter: parent.horizontalCenter
                    Image {
                        anchors.fill: parent
                        anchors.margins: 8
                        source: online.qrImage
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                    }
                }
                Text {
                    width: 240
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: online.loginHint
                    color: Theme.textTertiary
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 10
                    Rectangle {
                        width: 78; height: 28; radius: 14
                        color: page.dim
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("重新生成")
                            color: Theme.accent
                            font.pixelSize: 11
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { online.cancelLogin(); online.beginLogin() }
                        }
                    }
                    Rectangle {
                        width: 60; height: 28; radius: 14
                        color: "transparent"
                        border.width: 1
                        border.color: Theme.hairline
                        Text {
                            anchors.centerIn: parent
                            text: qsTr("取消")
                            color: Theme.textSecondary
                            font.pixelSize: 11
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: online.cancelLogin()
                        }
                    }
                }
            }
        }
    }
}
