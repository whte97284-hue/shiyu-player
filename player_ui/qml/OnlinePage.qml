import QtQuick
import PlayerUi

// 在线页：榜单 / 搜索 / 扫码登录 / 边听边存
//
// 排版对齐本地曲库：标题 + 分段控件 + 搜索框，下面是「左榜单 / 右歌曲」或搜索的四段式。
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
    // 内容整体换一批时压一下透明度再回来：一个动画顶替上百行的逐行动画
    property real contentFade: 1

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
        function onListsChanged() { refreshFade.restart() }
        function onTracksReset() { refreshFade.restart() }
    }
    NumberAnimation {
        id: refreshFade
        target: page
        property: "contentFade"
        from: 0.62
        to: 1
        duration: 120
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
                                hoverEnabled: true
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
                                hoverEnabled: true
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
                    hoverEnabled: true
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
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (!online.loggedIn) { online.beginLogin(); return }
                        accMenu.opened ? accMenu.close() : accMenu.open()
                    }
                }
            }
        }
    }

    // ── 搜索框 ────────────────────────────────────────────────────────
    Rectangle {
        id: searchBox
        x: page.sidePadding
        y: header.y + header.height + 14
        width: Math.min(520, page.width - page.sidePadding * 2)
        height: 40
        radius: 20
        color: Theme.hover
        border.width: 1
        border.color: input.activeFocus ? Theme.accentAlpha(0.6) : Theme.hairline
        Behavior on border.color { ColorAnimation { duration: Theme.fast } }

        Icon {
            id: searchIcon
            anchors.left: parent.left
            anchors.leftMargin: 15
            anchors.verticalCenter: parent.verticalCenter
            name: "search"
            size: 14
            opacity: 0.42
        }
        TextInput {
            id: input
            anchors.left: searchIcon.right
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.rightMargin: online.busy ? 34 : 15
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.text
            font.pixelSize: 13
            selectByMouse: true
            clip: true
            // 一进输入状态就淡出：光标压在提示字上很难看，也分不清自己到底在不在输入
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: opacity > 0.01
                opacity: (input.text.length === 0 && !input.activeFocus) ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                text: qsTr("搜索在线歌曲、歌手或专辑")
                color: Theme.textTertiary
                font.pixelSize: 13
            }
            onAccepted: online.search(text)
        }
        // 请求期间在输入框里转圈：点击到出结果之间不再是"什么都不发生"
        Icon {
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            name: "spinner"
            size: 13
            visible: online.busy
            opacity: 0.55
            RotationAnimator on rotation {
                running: online.busy
                loops: Animation.Infinite
                from: 0
                to: 360
                duration: 900
            }
        }
        MouseArea {
            anchors.fill: parent
            propagateComposedEvents: true
            cursorShape: Qt.IBeamCursor
            onClicked: (m) => { input.forceActiveFocus(); m.accepted = false }
        }
    }

    // 右侧只留一个数字和「返回榜单」，其余信息靠列表本身表达
    Row {
        anchors.right: parent.right
        anchors.rightMargin: page.sidePadding
        anchors.verticalCenter: searchBox.verticalCenter
        spacing: 10

        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: page.hasMeta
            text: qsTr("%1 首").arg(online.tracks.length)
            color: Theme.textTertiary
            font.pixelSize: 11
        }
        Rectangle {
            visible: online.viewMode >= 1
            width: backRow.implicitWidth + 16
            height: 26; radius: 13
            anchors.verticalCenter: parent.verticalCenter
            color: backMouse.containsMouse ? page.dim : "transparent"
            border.width: 1
            border.color: Theme.hairline
            Behavior on color { ColorAnimation { duration: Theme.fast } }
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
                hoverEnabled: true
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
                hoverEnabled: true
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
        opacity: modeFade * page.contentFade
        visible: opacity > 0.01

        ListView {
            id: trackList
            anchors.fill: parent
            clip: true
            spacing: 1
            model: online.tracks
            boundsBehavior: Flickable.StopAtBounds
            SmoothScroll { flickable: trackList }

            delegate: TrackLine {
                required property int index
                required property var modelData
                width: trackList.width
                title: modelData.title
                subtitle: modelData.artist + (modelData.album.length ? " · " + modelData.album : "")
                duration: modelData.duration
                paywalled: modelData.paywalled
                active: online.playing === index
                playing: online.playing === index && player.playing
                pending: online.pending === index
                accent: page.accent
                onPlayed: online.playing === index ? player.toggle() : online.play(index)
                onDownloaded: online.download(index)
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

    // ── 搜索模式：最佳匹配 / 歌手 / 专辑 / 歌曲 ──────────────────────
    //
    // 用 ListView 而不是 Flickable + Repeater：Repeater 会一次性把 60 行全建出来
    // （每行十来个元素 + 图标），创建那一下就是肉眼可见的顿。ListView 只建可见的十来行。
    // 最佳匹配 / 歌手 / 专辑放进 header，跟着一起滚。
    ListView {
        id: searchView
        x: page.sidePadding
        y: divider.y + 14
        width: page.width - page.sidePadding * 2
        height: page.height - y - page.bottomInset
        clip: true
        spacing: 2
        model: online.tracks
        boundsBehavior: Flickable.StopAtBounds
        property real modeFade: online.viewMode >= 1 ? 1 : 0
        Behavior on modeFade { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
        opacity: modeFade * page.contentFade
        visible: opacity > 0.01
        SmoothScroll { flickable: searchView }

        header: Item {
            width: searchView.width
            height: searchHead.height
            Column {
                id: searchHead
                width: parent.width
                spacing: 20

                // 歌手 / 专辑详情头：封面 + 标题 + 副标题 + 统计
                Rectangle {
                    width: parent.width
                    height: 148
                    radius: 14
                    visible: online.isDetail
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: Theme.accentAlpha(0.13) }
                        GradientStop { position: 0.55; color: Theme.accentAlpha(0.04) }
                        GradientStop { position: 1.0; color: Theme.hover }
                    }

                    SoftShadow {
                        target: detailArt
                        spread: 18
                        offsetY: 8
                        strength: 0.5
                    }
                    RoundedImage {
                        id: detailArt
                        x: 20
                        anchors.verticalCenter: parent.verticalCenter
                        width: 104; height: 104
                        radius: online.detailRound ? 52 : 10
                        decodeSize: 256
                        source: online.detailCover
                    }
                    Column {
                        anchors.left: detailArt.right
                        anchors.leftMargin: 20
                        anchors.right: parent.right
                        anchors.rightMargin: 24
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8
                        Text {
                            width: parent.width
                            text: online.detailTitle
                            color: Theme.text
                            font.pixelSize: 24
                            font.weight: Font.Bold
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            visible: text.length > 0
                            text: online.detailSubtitle
                            color: Theme.textSecondary
                            font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: online.detailMeta
                            color: Theme.textTertiary
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }
                }

                // 最佳匹配：换一批结果时轻微上浮淡入，只动这一张卡片
                Rectangle {
                    id: bestCard
                    width: parent.width
                    height: 136
                    radius: 14
                    visible: page.best !== null && online.viewMode === 1
                    opacity: 0
                    transform: Translate { id: bestShift; y: 0 }
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: Theme.accentAlpha(0.13) }
                        GradientStop { position: 0.55; color: Theme.accentAlpha(0.04) }
                        GradientStop { position: 1.0; color: Theme.hover }
                    }
                    ParallelAnimation {
                        id: bestIn
                        NumberAnimation { target: bestCard; property: "opacity"; from: 0; to: 1; duration: Theme.normal; easing.type: Easing.OutCubic }
                        NumberAnimation { target: bestShift; property: "y"; from: 6; to: 0; duration: Theme.normal; easing.type: Easing.OutCubic }
                    }
                    Connections {
                        target: online
                        function onTracksReset() { bestIn.restart() }
                    }
                    Component.onCompleted: bestIn.start()

                    SoftShadow {
                        target: bestCover
                        spread: 18
                        offsetY: 8
                        strength: 0.5
                    }
                    RoundedImage {
                        id: bestCover
                        x: 18
                        anchors.verticalCenter: parent.verticalCenter
                        width: 100; height: 100
                        radius: 10
                        decodeSize: 208
                        source: online.albums.length > 0 ? online.albums[0].cover : ""
                    }
                    Column {
                        anchors.left: bestCover.right
                        anchors.leftMargin: 18
                        anchors.right: bestActions.left
                        anchors.rightMargin: 20
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 7
                        Text {
                            width: parent.width
                            text: page.best ? page.best.title : ""
                            color: Theme.text
                            font.pixelSize: 22
                            font.weight: Font.Bold
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: page.best
                                  ? page.best.artist + (page.best.album.length ? " · " + page.best.album : "")
                                  : ""
                            color: Theme.textSecondary
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        Row {
                            spacing: 8
                            Rectangle {
                                visible: page.best && page.best.paywalled
                                width: 32; height: 16; radius: 4
                                color: page.dim
                                Text {
                                    anchors.centerIn: parent
                                    text: "VIP"
                                    color: Theme.accent
                                    font.pixelSize: 8
                                }
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: page.best ? page.best.duration : ""
                                color: Theme.textTertiary
                                font.pixelSize: 11
                            }
                        }
                    }
                    Row {
                        id: bestActions
                        anchors.right: parent.right
                        anchors.rightMargin: 20
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 10
                        Rectangle {
                            width: 42; height: 42; radius: 21
                            color: page.accent
                            scale: bestPlay.pressed ? 0.92 : (bestPlay.containsMouse ? 1.05 : 1.0)
                            Behavior on color { ColorAnimation { duration: Theme.normal } }
                            Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
                            Icon {
                                anchors.centerIn: parent
                                name: online.playing === 0 && player.playing ? "pause" : "play"
                                size: 14
                                tint: Theme.onAccent
                            }
                            MouseArea {
                                id: bestPlay
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: online.playing === 0 ? player.toggle() : online.play(0)
                            }
                        }
                        Rectangle {
                            width: 42; height: 42; radius: 21
                            color: bestDl.containsMouse ? page.dim : "transparent"
                            scale: bestDl.pressed ? 0.92 : (bestDl.containsMouse ? 1.05 : 1.0)
                            border.width: 1
                            border.color: bestDl.containsMouse ? Theme.accentAlpha(0.4) : Theme.hairline
                            Behavior on color { ColorAnimation { duration: Theme.fast } }
                            Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
                            Icon {
                                anchors.centerIn: parent
                                name: "import"
                                size: 15
                                opacity: 0.6
                                tint: Theme.textSecondary
                            }
                            MouseArea {
                                id: bestDl
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: online.download(0)
                            }
                        }
                    }
                }

                // 歌手
                Column {
                    width: parent.width
                    spacing: 10
                    visible: online.artists.length > 0 && online.viewMode === 1
                    Text {
                        text: qsTr("歌手")
                        color: Theme.textTertiary
                        font.pixelSize: 11
                    }
                    Row {
                        spacing: 10
                        Repeater {
                            model: Math.min(online.artists.length, 8)
                            delegate: Rectangle {
                                required property int index
                                width: 136; height: 46; radius: 23
                                color: artistMouse.containsMouse ? page.faint : "transparent"
                                border.width: 1
                                border.color: Theme.hairline
                                Behavior on color { ColorAnimation { duration: Theme.fast } }
                                Rectangle {
                                    x: 6
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 34; height: 34; radius: 17
                                    color: page.dim
                                    Text {
                                        anchors.centerIn: parent
                                        text: online.artists[index].name.charAt(0)
                                        color: Theme.accent
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                    }
                                }
                                Column {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 48
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 1
                                    Text {
                                        width: parent.width
                                        text: online.artists[index].name
                                        color: Theme.text
                                        font.pixelSize: 12
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        visible: online.artists[index].count > 0
                                        text: qsTr("%1 首").arg(online.artists[index].count)
                                        color: Theme.textTertiary
                                        font.pixelSize: 10
                                    }
                                }
                                MouseArea {
                                    id: artistMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: online.openArtist(online.artists[index].id || "",
                                                                 online.artists[index].name)
                                }
                            }
                        }
                    }
                }

                // 专辑
                Column {
                    width: parent.width
                    spacing: 10
                    visible: online.albums.length > 0 && online.viewMode === 1
                    Text {
                        text: qsTr("专辑")
                        color: Theme.textTertiary
                        font.pixelSize: 11
                    }
                    Row {
                        spacing: 16
                        Repeater {
                            model: Math.min(online.albums.length, 7)
                            delegate: Item {
                                required property int index
                                width: 128
                                height: 182
                                RoundedImage {
                                    width: 128; height: 128
                                    radius: 10
                                    decodeSize: 256
                                    source: online.albums[index].cover
                                }
                                Text {
                                    y: 136
                                    width: parent.width
                                    text: online.albums[index].name
                                    color: Theme.text
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }
                                Text {
                                    y: 154
                                    width: parent.width
                                    text: online.albums[index].artist
                                    color: Theme.textTertiary
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: online.openAlbum(online.albums[index].id || "",
                                                                online.albums[index].name)
                                }
                            }
                        }
                    }
                }

                Text {
                    visible: online.tracks.length > 0
                    text: qsTr("歌曲")
                    color: Theme.textTertiary
                    font.pixelSize: 11
                }
            }
        }

        delegate: TrackLine {
            required property int index
            required property var modelData
            width: searchView.width
            title: modelData.title
            subtitle: modelData.artist + (modelData.album.length ? " · " + modelData.album : "")
            duration: modelData.duration
            paywalled: modelData.paywalled
            active: online.playing === index
            playing: online.playing === index && player.playing
            pending: online.pending === index
            accent: page.accent
            onPlayed: online.playing === index ? player.toggle() : online.play(index)
            onDownloaded: online.download(index)
        }

        footer: Item {
            width: searchView.width
            height: online.canLoadMore ? 54 : 12
            Rectangle {
                visible: online.canLoadMore
                width: 132; height: 32; radius: 16
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 6
                color: moreMouse.containsMouse ? page.dim : "transparent"
                border.width: 1
                border.color: Theme.hairline
                Behavior on color { ColorAnimation { duration: Theme.fast } }
                Text {
                    anchors.centerIn: parent
                    text: qsTr("加载更多")
                    color: Theme.textSecondary
                    font.pixelSize: 11
                }
                MouseArea {
                    id: moreMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: online.loadMore()
                }
            }
        }
    }

    Text {
        anchors.centerIn: searchView
        visible: online.isSearch && online.tracks.length === 0 && !online.busy
        text: online.keyword.length > 0 ? qsTr("没有找到，换个关键词试试") : ""
        color: Theme.textTertiary
        font.pixelSize: 12
    }

    // ── 骨架屏：等网络时给个形状 ────────────────────────────────────
    //
    // 站位必须和 TrackLine 对齐：序号位 / 歌名 / 歌手·专辑 / 时长 / 两个按钮。
    // 之前是"一个方块 + 两条杠"，跟真实行完全不是一套骨架，出现时像另一份界面闪了一下。
    Item {
        id: skeleton
        x: online.viewMode === 0 ? trackArea.x : page.sidePadding
        y: divider.y + 14
        width: online.viewMode === 0 ? trackArea.width : page.width - page.sidePadding * 2
        height: Math.min(page.height - y - page.bottomInset, 8 * 48)
        visible: online.busy && online.tracks.length === 0
        clip: true

        Column {
            width: parent.width
            spacing: 2
            Repeater {
                model: 8
                delegate: Item {
                    required property int index
                    width: skeleton.width
                    height: 46

                    // 序号位（真实行这里是个数字）
                    Rectangle {
                        x: 19
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12; height: 8; radius: 4
                        color: Qt.rgba(1, 1, 1, 0.032)
                    }
                    // 歌名 + 歌手·专辑
                    Column {
                        x: 44
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 7
                        Rectangle {
                            width: Math.min(skeleton.width * 0.32, 150 + (index % 3) * 54)
                            height: 9; radius: 4.5
                            color: Qt.rgba(1, 1, 1, 0.055)
                        }
                        Rectangle {
                            width: Math.min(skeleton.width * 0.2, 88 + (index % 2) * 46)
                            height: 8; radius: 4
                            color: Qt.rgba(1, 1, 1, 0.03)
                        }
                    }
                    // 时长
                    Rectangle {
                        x: parent.width - 132
                        anchors.verticalCenter: parent.verticalCenter
                        width: 26; height: 8; radius: 4
                        color: Qt.rgba(1, 1, 1, 0.028)
                    }
                    // 播放 / 下载两个圆钮
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

        // 整块一起呼吸：8 行只花一个动画，不逐行跑
        SequentialAnimation on opacity {
            running: skeleton.visible
            loops: Animation.Infinite
            NumberAnimation { to: 0.45; duration: 700; easing.type: Easing.InOutSine }
            NumberAnimation { to: 0.9; duration: 700; easing.type: Easing.InOutSine }
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
                        hoverEnabled: true
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
