import QtQuick
import PlayerUi

// 沉浸播放页 v4:整窗铺满模糊封面,左边封面与控制,右边歌词占满整个高度,没有用不到的空白
//  左栏:封面 → 歌名歌手 (居中) → 进度 (时间在两侧) → 控制 → 音量
//  右栏:歌词居中排版,当前句停在视口偏上处、用强调色随演唱点亮,译文跟在原句下面;无歌词时为「接下来播放」
//  顶栏:左上角收起;右下角一排同样式的图标按钮:歌词来源、音质、歌词 / 队列切换
//  大屏时封面、字号、按钮按窗口高度等比放大 (k,最多 1.35 倍)
Item {
    id: np
    property bool shown: false
    signal closeRequested()
    signal chainClicked()
    property bool chainOpen: false   // 信号链详情是否展开 (音质按钮据此显示选中)
    // 共享元素飞行用:大封面。封面本身不参与进场位移,飞行落点在整个过程中保持不动
    readonly property Item coverItem: cover
    // 大封面的投影:飞行副本按它的参数带着投影落地
    readonly property Item coverShadow: coverShadow
    function automationTab(t) { side.tab = String(t) }
    function automationLyricPanel(on) { if (on) { side.tab = "lyrics"; lyricPanel.open() } else lyricPanel.close() }
    function automationEqPanel(on) { if (on) eqPanel.open(); else eqPanel.close() }
    function automationEqHover(i) { eqPanel.hoverBand = i }
    function automationLyricPick(i) { lyricPanel.pick(i) }
    function automationLyricHover(i) { lyricPanel.hoverRow = i }
    function automationLyricSave() { lyricPanel.save() }
    function automationLyricOnlyTranslated(on) { lyricPanel.onlyTranslated = on }
    function openMenu(item, px, py) {
        const p = item.mapToItem(null, px, py)
        Nav.trackMenu(player.trackId, p.x, p.y, {})
    }

    // 进场错峰:封面下方各行依次上浮淡入 (order 越大越晚),退场同时下沉
    component Rise: Item {
        property int order: 0
        width: parent ? parent.width : 0
        height: childrenRect.height
        opacity: np.shown ? 1 : 0
        Behavior on opacity { SequentialAnimation { PauseAnimation { duration: np.shown ? 140 + order * 50 : 0 } OpacityAnimator { duration: np.shown ? 420 : 160; easing.type: Easing.OutCubic } } }
        transform: Translate {
            y: np.shown ? 0 : 18 + order * 6
            Behavior on y { SequentialAnimation { PauseAnimation { duration: np.shown ? 140 + order * 50 : 0 } NumberAnimation { duration: np.shown ? 680 : 200; easing.type: Easing.OutQuint } } }
        }
    }

    opacity: shown ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { OpacityAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
    // 完全淡入后为 true:此时底下的曲库页面与极光背景被整屏盖住,可以停止绘制它们
    property bool covering: false
    onShownChanged: if (shown) { coverDelay.restart(); introduceChrome() } else { coverDelay.stop(); covering = false }
    Timer { id: coverDelay; interval: Theme.normal + 40; onTriggered: np.covering = true }

    // ---------------- 背景 ----------------
    // 背景直接沿用首页那层模糊封面 (同一张图、同一参数,就在播放页下面),播放页自己不再画一份:
    // 打开 / 收起时背景纹丝不动,只有内容淡入淡出;也不会因为自己的图层还没渲染好而闪黑
    Rectangle { anchors.fill: parent; color: Theme.base; visible: !player.hasTrack }
    MouseArea { anchors.fill: parent; hoverEnabled: true; onWheel: (w) => w.accepted = true }

    // ---------------- 版面 ----------------
    readonly property real padX: Math.max(40, width * 0.045)
    readonly property real leftW: Math.round(width * 0.46)
    // 大屏放大系数:820 高以下为 1
    readonly property real k: Math.max(1, Math.min(1.35, height / 820))
    // 封面下方固定内容的总高度 (歌名、歌手、进度、控制、音量与间距)
    readonly property real belowCover: Math.round(224 * k)
    readonly property real coverSize: Math.round(Math.max(200, Math.min(leftW - padX - 24, height - belowCover - 120, 760)))
    readonly property real colX: Math.round(padX + (leftW - padX - coverSize) / 2)
    readonly property real colY: Math.round((height - coverSize - belowCover) / 2 + 8)
    readonly property real sideX: leftW + Math.max(16, width * 0.02)
    readonly property real sideW: width - padX - sideX

    // ---------------- 界面控件随鼠标隐现 ----------------
    // 收起按钮、右下角按钮组 (以及窗口按钮) 平时淡出,不和封面、歌词抢眼;鼠标一动就淡入,
    // 停下约 2.5 秒后再淡出。指着控件、面板或信号链开着时保持显示
    property bool chromeAwake: true
    readonly property bool chromeShown: chromeAwake || lyricPanel.shown || eqPanel.shown || chainOpen || tabsHover.hovered || backHover.hovered
    property real chrome: chromeShown ? 1 : 0
    Behavior on chrome { NumberAnimation { duration: np.chromeShown ? 220 : 700; easing.type: Easing.OutCubic } }
    function wake() { chromeAwake = true; chromeIdle.interval = 2500; chromeIdle.restart() }
    // 进入播放页:控件随页面淡入后先停留约一秒再淡出,让人知道它们在那里
    function introduceChrome() { chromeAwake = true; chromeIdle.interval = 1700; chromeIdle.restart() }
    Timer { id: chromeIdle; interval: 2500; onTriggered: np.chromeAwake = false }
    // 只认鼠标真的移动:歌词在静止的光标下滚动也会产生悬停事件,不能因此一直显示
    HoverHandler {
        id: npHover
        property point last: Qt.point(-1, -1)
        onPointChanged: {
            const p = point.scenePosition
            if (Math.abs(p.x - last.x) + Math.abs(p.y - last.y) < 3) return
            last = p
            np.wake()
        }
    }

    // ---------------- 顶栏 ----------------
    // 收起:放进窗口顶栏那一行,与右上角窗口按钮同一条中线 (y = 17),离封面留出空当
    // (标题栏的拖动区从左侧导航宽度之后才开始,这里点得到)
    IconButton {
        x: 10
        opacity: np.chrome
        visible: opacity > 0.01
        HoverHandler { id: backHover }
        y: -3
        icon: "back"
        iconSize: 16
        rotation: -90
        tip: qsTr("收起  Esc")
        onClicked: np.closeRequested()
    }

    // 右下角按钮组:竖排贴着右缘,不伸进歌词栏;四个按钮同尺寸、等间距
    Column {
        id: tabs
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 22
        spacing: 4
        z: 2
        property real entry: np.shown ? 1 : 0
        Behavior on entry { SequentialAnimation { PauseAnimation { duration: np.shown ? 260 : 0 } NumberAnimation { duration: 360 } } }
        opacity: entry * np.chrome
        visible: opacity > 0.01
        HoverHandler { id: tabsHover }
        // 四个按钮同一种样式:图标 19px、40×40 点击区、强调色表示「开着 / 选中」,右侧短竖线标出当前项
        //  歌词来源 (图标随来源变化,点开来源面板) · 音质 (悬停看完整信号链,点开详情;直通时强调色) · 歌词 · 队列
        Repeater {
            model: [
                { key: "source", tip: qsTr("歌词来源") },
                { key: "quality", tip: qsTr("音质") },
                { key: "eq", icon: "eq", tip: qsTr("均衡器") },
                { key: "lyrics", icon: "lyrics", tip: qsTr("歌词") },
                { key: "queue", icon: "queue", tip: qsTr("播放队列") }
            ]
            Item {
                id: tabBtn
                readonly property string key: modelData.key
                width: 40
                height: 40
                visible: player.hasTrack || key === "lyrics" || key === "queue" || key === "eq"
                readonly property bool active: key === "source" ? lyricPanel.shown : key === "quality" ? np.chainOpen
                                             : key === "eq" ? eqPanel.shown : side.tab === key
                readonly property bool lit: active || (key === "quality" && player.bitPerfect)
                                            || (key === "eq" && player.eqEnabled && player.eqPreset !== "flat")
                readonly property string icon: key === "source" ? Nav.lyricIcon(player.lyricSource.kind) : key === "quality" ? "hires" : modelData.icon
                readonly property string tip: key === "source"
                    ? (player.lyricSource.label ? qsTr("歌词来源 · %1").arg(player.lyricSource.label) : qsTr("搜索歌词"))
                    : key === "quality" ? (player.chainSummary || player.quality || qsTr("音质"))
                    : key === "eq" ? (player.eqEnabled ? qsTr("均衡器 · 已开启") : qsTr("均衡器"))
                    : modelData.tip
                onTipChanged: if (tm.containsMouse) Nav.hint(tabBtn, tip)
                Icon {
                    anchors.centerIn: parent
                    name: tabBtn.icon
                    size: 19
                    tint: tabBtn.lit ? Theme.accent : "transparent"
                    opacity: tabBtn.lit || tm.containsMouse ? 1 : 0.5
                    scale: tm.pressed ? 0.9 : 1
                    Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                    Behavior on scale { ScaleAnimator { duration: Theme.fast; easing.type: Easing.OutCubic } }
                }
                Rectangle {
                    anchors.right: parent.right
                    anchors.rightMargin: -2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 2
                    height: 16
                    radius: 1
                    color: Theme.accent
                    opacity: tabBtn.active ? 1 : 0
                    scale: tabBtn.active ? 1 : 0.3
                    Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
                    Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
                }
                MouseArea {
                    id: tm
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onContainsMouseChanged: containsMouse ? Nav.hint(parent, tabBtn.tip) : Nav.unhint(parent)
                    onClicked: {
                        if (tabBtn.key === "eq") {
                            if (eqPanel.shown) { eqPanel.close(); return }
                            lyricPanel.close()
                            eqPanel.open()
                        } else if (tabBtn.key === "source") {
                            if (lyricPanel.shown) { lyricPanel.close(); return }
                            eqPanel.close()
                            side.tab = "lyrics"
                            lyricPanel.open()
                        } else if (tabBtn.key === "quality") {
                            np.chainClicked()
                        } else {
                            side.tab = tabBtn.key
                        }
                    }
                }
            }
        }
    }

    // 歌词来源面板:右下角对齐来源胶囊,从那里弹出
    LyricSourcePanel {
        id: lyricPanel
        // 从竖排按钮左侧展开,底边与按钮组对齐
        anchorRight: tabs.x - 10
        anchorBottom: tabs.y + tabs.height
        Connections { target: np; function onShownChanged() { if (!np.shown) lyricPanel.close() } }
    }

    // 均衡器面板:同样从按钮组左侧展开
    EqualizerPanel {
        id: eqPanel
        objectName: "eqPanel"
        anchorRight: tabs.x - 10
        anchorBottom: tabs.y + tabs.height
        Connections { target: np; function onShownChanged() { if (!np.shown) eqPanel.close() } }
    }

    component MetaLink: Text {
        id: link
        property real maxWidth: 200
        property bool linked: true
        signal activated()
        width: Math.min(implicitWidth, maxWidth)
        elide: Text.ElideRight
        color: linkMouse.containsMouse && linked ? Theme.text : Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Math.round(14 * np.k)
        font.underline: linkMouse.containsMouse && linked
        MouseArea {
            id: linkMouse
            anchors.fill: parent
            enabled: link.linked
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: link.activated()
        }
    }

    // ---------------- 左栏:封面与控制 ----------------
    Item {
        id: card
        x: np.colX
        y: np.colY
        width: np.coverSize
        height: np.coverSize + np.belowCover

        // 封面舞台:暂停时整张封面连同投影微微后退 (0.9),继续播放时带一点回弹地浮回来
        Item {
            id: coverStage
            width: np.coverSize
            height: np.coverSize
            scale: player.playing || !player.hasTrack ? 1 : 0.9
            Behavior on scale { ScaleAnimator { duration: player.playing ? 680 : 520; easing.type: player.playing ? Easing.OutBack : Easing.OutQuint; easing.overshoot: 1.2 } }
            SoftShadow { id: coverShadow; target: cover; spread: 44; offsetY: player.playing ? 24 : 12; strength: player.playing ? 0.9 : 0.6
                Behavior on offsetY { NumberAnimation { duration: 600; easing.type: Easing.OutCubic } }
                Behavior on strength { NumberAnimation { duration: 600; easing.type: Easing.OutCubic } } }
            CrossfadeCover {
                id: cover
                width: np.coverSize
                height: np.coverSize
                radius: 12
                source: player.cover
            }
            // 右键封面:这首歌的菜单
            MouseArea {
                anchors.fill: cover
                acceptedButtons: Qt.RightButton
                enabled: player.hasTrack
                onClicked: (e) => np.openMenu(cover, e.x, e.y)
            }
        }

        Column {
            y: np.coverSize + 26
            width: parent.width
            spacing: 0

            Rise { order: 0; Column { id: titleBlock; width: parent.width
            // 切歌:新歌名与歌手从右侧轻轻滑入
            transform: Translate { id: titleShift }
            Connections {
                target: player
                function onTrackChanged() { if (np.shown) titleIn.restart() }
            }
            ParallelAnimation {
                id: titleIn
                NumberAnimation { target: titleShift; property: "x"; from: 18; to: 0; duration: 620; easing.type: Easing.OutQuint }
                OpacityAnimator { target: titleBlock; from: 0; to: 1; duration: 420; easing.type: Easing.OutCubic }
            }
            Item {
                width: parent.width
                height: titleText.implicitHeight
                Text {
                    id: titleText
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width - 84
                    horizontalAlignment: Text.AlignHCenter
                    text: player.title
                    elide: Text.ElideRight
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: Math.round(22 * np.k)
                    font.weight: Font.Bold
                }
                // 左「更多」、右「收藏」对称,歌名真正居中
                IconButton {
                    id: npMore
                    anchors.left: parent.left
                    anchors.leftMargin: -8
                    anchors.verticalCenter: titleText.verticalCenter
                    width: 36; height: 36
                    icon: "more"
                    iconSize: 18
                    restOpacity: 0.6
                    tip: qsTr("更多")
                    onClicked: np.openMenu(npMore, npMore.width / 2, npMore.height)
                }
                IconButton {
                    readonly property bool fav: playlists.revision >= 0 && player.hasTrack && playlists.isFavorite(player.trackId)
                    anchors.right: parent.right
                    anchors.rightMargin: -8
                    anchors.verticalCenter: titleText.verticalCenter
                    width: 36; height: 36
                    icon: fav ? "heart-filled" : "heart"
                    iconTint: fav ? Theme.accent : "transparent"
                    iconSize: 18
                    tip: (fav ? qsTr("取消收藏") : qsTr("收藏")) + "  Ctrl+L"
                    onClicked: { playlists.toggleFavorite(player.trackId); popNow() }
                }
            }
            // 歌手 · 专辑:各自可点,收起播放页并打开歌手 / 专辑
            Item {
                width: parent.width
                height: metaRow.height + 3
                Row {
                    id: metaRow
                    y: 3
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 0
                    MetaLink {
                        text: player.artist
                        maxWidth: (titleBlock.width - sepText.width) * (player.album.length ? 0.45 : 1)
                        onActivated: {
                            const i = library.artistOfTrack(player.trackId)
                            if (i >= 0) Nav.openArtist(i)
                        }
                        linked: library.generation >= 0 && player.hasTrack && library.artistOfTrack(player.trackId) >= 0
                    }
                    Text {
                        id: sepText
                        visible: player.album.length > 0
                        text: "  ·  "
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Math.round(14 * np.k)
                    }
                    MetaLink {
                        visible: player.album.length > 0
                        text: player.album
                        maxWidth: (titleBlock.width - sepText.width) * 0.55
                        linked: player.albumIndex >= 0
                        onActivated: Nav.openAlbum(player.albumIndex)
                    }
                }
            }
            }}

            Rise { order: 1; Column { width: parent.width
            Item { width: 1; height: 16 }
            // 进度:时间分列两侧,中间是进度条
            Item {
                width: parent.width
                height: 24
                Text {
                    id: posText
                    width: 40
                    anchors.verticalCenter: parent.verticalCenter
                    text: seek.fmt(seek.dragging ? seek.dragValue : player.position)
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                }
                SeekBar {
                    id: seek
                    anchors.left: posText.right
                    anchors.right: remText.left
                    anchors.verticalCenter: parent.verticalCenter
                    position: player.position
                    duration: player.duration
                    onSeekRequested: (s) => player.seek(s)
                }
                Text {
                    id: remText
                    width: 40
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    horizontalAlignment: Text.AlignRight
                    text: "-" + seek.fmt(Math.max(0, player.duration - (seek.dragging ? seek.dragValue : player.position)))
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                }
            }
            }}

            Rise { order: 2; Column { width: parent.width
            Item { width: 1; height: 10 }
            Item {
                width: parent.width
                height: Math.round(62 * np.k)
                // 随机与单曲循环互斥 (与播放条的四态按钮一致:随机时循环整个列表)
                IconButton {
                    anchors.left: parent.left
                    anchors.leftMargin: -6
                    anchors.verticalCenter: parent.verticalCenter
                    width: 38; height: 38
                    icon: "shuffle"
                    iconSize: 16
                    iconTint: player.shuffle ? Theme.accent : "transparent"
                    restOpacity: player.shuffle ? 1 : 0.5
                    tip: player.shuffle ? qsTr("关闭随机播放") : qsTr("随机播放")
                    onClicked: {
                        player.shuffle = !player.shuffle
                        if (player.shuffle && player.repeatMode === 2) player.repeatMode = 1
                    }
                }
                IconButton {
                    anchors.right: parent.right
                    anchors.rightMargin: -6
                    anchors.verticalCenter: parent.verticalCenter
                    width: 38; height: 38
                    icon: player.repeatMode === 2 ? "repeat-one" : "repeat"
                    iconSize: 16
                    iconTint: player.repeatMode > 0 ? Theme.accent : "transparent"
                    restOpacity: player.repeatMode > 0 ? 1 : 0.5
                    tip: player.repeatMode === 2 ? qsTr("单曲循环") : player.repeatMode === 1 ? qsTr("列表循环") : qsTr("不循环")
                    onClicked: {
                        const m = (player.repeatMode + 1) % 3
                        if (m === 2) player.shuffle = false
                        player.repeatMode = m
                    }
                }
                Row {
                    anchors.centerIn: parent
                    spacing: Math.max(14, Math.min(30 * np.k, np.coverSize * 0.06))
                    IconButton {
                        icon: "prev"; nudge: -1
                        iconSize: Math.round(21 * np.k)
                        width: Math.round(46 * np.k); height: width
                        anchors.verticalCenter: parent.verticalCenter
                        tip: qsTr("上一首  Ctrl+←")
                        onClicked: player.previous()
                    }
                    IconButton {
                        icon: player.playing ? "pause" : "play"
                        morphOnChange: true
                        iconSize: Math.round(24 * np.k)
                        width: Math.round(60 * np.k); height: width
                        filled: true
                        tip: (player.playing ? qsTr("暂停") : qsTr("播放")) + "  空格"
                        onClicked: player.toggle()
                    }
                    IconButton {
                        icon: "next"; nudge: 1
                        iconSize: Math.round(21 * np.k)
                        width: Math.round(46 * np.k); height: width
                        anchors.verticalCenter: parent.verticalCenter
                        tip: qsTr("下一首  Ctrl+→")
                        onClicked: player.next()
                    }
                }
            }
            }}

            Rise { order: 3; Column { width: parent.width
            Item { width: 1; height: 6 }
            // 音量:左键静音 / 恢复,右键开到最大;整行支持滚轮
            Item {
                width: parent.width
                height: 32
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.NoButton
                    property real acc: 0
                    onWheel: (w) => {
                        acc += w.angleDelta.y
                        const steps = Math.trunc(acc / 120)
                        if (steps !== 0) { acc -= steps * 120; player.changeVolume(steps * 0.02) }
                        w.accepted = true
                    }
                }
                IconButton {
                    id: muteBtn
                    x: -9
                    anchors.verticalCenter: parent.verticalCenter
                    width: 32; height: 32
                    icon: player.volume < 0.01 ? "mute" : "volume"
                    iconSize: 14
                    restOpacity: 0.45
                    tip: (player.volume < 0.01 ? qsTr("取消静音") : qsTr("静音")) + "  Ctrl+M"
                    onClicked: Nav.toggleMute()
                }
                SeekBar {
                    id: volBar
                    anchors.left: muteBtn.right
                    anchors.leftMargin: 3
                    anchors.right: volText.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    showTip: false
                    position: player.volume
                    duration: 1
                    onSeekRequested: (v) => player.volume = v
                    onDragValueChanged: if (dragging) player.volume = dragValue
                }
                Text {
                    id: volText
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 32
                    horizontalAlignment: Text.AlignRight
                    text: Math.round(player.volume * 100)
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                }
            }
            }}
        }
    }

    // ---------------- 右栏:歌词 / 接下来播放 ----------------
    Item {
        id: side
        opacity: np.shown ? 1 : 0
        Behavior on opacity { SequentialAnimation { PauseAnimation { duration: np.shown ? 120 : 0 } OpacityAnimator { duration: 420; easing.type: Easing.OutCubic } } }
        transform: Translate {
            y: np.shown ? 0 : 40
            Behavior on y { SequentialAnimation { PauseAnimation { duration: np.shown ? 120 : 0 } NumberAnimation { duration: 640; easing.type: Easing.OutQuint } } }
        }
        x: np.sideX
        y: 0
        width: np.sideW
        height: np.height

        readonly property bool hasLyrics: player.lyrics.length > 0
        property string tab: hasLyrics ? "lyrics" : "queue"
        onHasLyricsChanged: tab = hasLyrics ? "lyrics" : "queue"

        // 歌词:占满整个高度,上下边缘渐隐。当前句停在视口约 42% 处;换句时整列沿一条缓入缓出曲线平滑滑动,
        // 正在滑动时再换句会从当前位置接着滑,不会跳;手动滚动后暂停跟随,片刻后平滑回到当前句
        ListView {
            id: lyricView
            anchors.fill: parent
            visible: side.tab === "lyrics"
            model: player.lyrics
            spacing: 4
            readonly property real anchorRatio: 0.42
            // 首句停在顶部渐隐带下方、末句停在底部渐隐带上方:开头和结尾不再把歌词推出视野
            topMargin: height * 0.2
            bottomMargin: height * 0.28
            // 一首歌的歌词全部实例化:每行位置都是实测的,不会因估算行高在滚动中途跳动
            cacheBuffer: 100000
            boundsBehavior: Flickable.StopAtBounds
            readonly property int fontSize: Math.round(Math.max(20, Math.min(36, np.width / 46)))
            property bool following: true

            layer.enabled: true
            layer.effect: ShaderEffect {
                property real fadeTop: 0.16
                property real fadeBottom: 0.22
                fragmentShader: "qrc:/shaders/fade.frag.qsb"
            }

            // 让第 idx 句的中心停在视口锚点处的 contentY
            function targetFor(idx) {
                const it = itemAtIndex(idx)
                return it ? it.y + it.height / 2 - height * anchorRatio : NaN
            }
            function follow(animate) {
                if (!player.lyricsSynced || !following || !visible || count === 0) return
                const idx = player.lyricIndex
                let t = idx < 0 ? -topMargin : targetFor(idx)
                if (isNaN(t)) { positionViewAtIndex(idx, ListView.Center); t = targetFor(idx) }
                if (isNaN(t)) return
                // 夹在可滚动范围内:前几句时列表停在顶端,当前句自己往下走;末尾同理
                t = Math.max(originY - topMargin, Math.min(t, originY + contentHeight + bottomMargin - height))
                settle.stop()
                if (!animate || Math.abs(contentY - t) < 0.5) { contentY = t; return }
                // 跨越很远 (拖动进度条) 时稍慢一点,相邻换句约 0.62 秒
                settle.duration = Math.abs(contentY - t) > height * 0.6 ? 820 : 620
                settle.to = t
                settle.start()
            }
            function pauseFollow() {
                following = false
                settle.stop()
                resume.restart()
            }
            Timer {
                id: resume
                interval: 2600
                onTriggered: { lyricView.following = true; lyricView.follow(true) }
            }
            NumberAnimation {
                id: settle
                target: lyricView
                property: "contentY"
                easing.type: Easing.BezierSpline
                easing.bezierCurve: [0.3, 0.0, 0.15, 1.0, 1.0, 1.0]
            }
            SmoothScroll { flickable: lyricView; onScrolled: lyricView.pauseFollow() }
            onMovementStarted: pauseFollow()
            onVisibleChanged: if (visible) Qt.callLater(follow, false)
            onCountChanged: { following = true; Qt.callLater(follow, false) }
            onHeightChanged: Qt.callLater(follow, false)
            Connections {
                target: player
                function onLyricIndexChanged() { lyricView.follow(true) }
            }
            Connections {
                target: np
                function onShownChanged() { if (np.shown) { lyricView.following = true; Qt.callLater(lyricView.follow, false) } }
            }

            delegate: Item {
                id: row
                width: lyricView.width
                height: line.implicitHeight + (sub.visible ? sub.implicitHeight + 2 : 0) + 18
                readonly property int dist: Math.abs(index - player.lyricIndex)
                readonly property bool current: player.lyricsSynced && index === player.lyricIndex
                readonly property bool sung: player.lyricsSynced && index < player.lyricIndex
                readonly property string translation: modelData.translation || ""
                opacity: !player.lyricsSynced ? 0.75 : row.current ? 1 : Math.max(0.14, (row.sung ? 0.42 : 0.5) - (row.dist - 1) * 0.07)
                Behavior on opacity { OpacityAnimator { duration: 480; easing.type: Easing.OutCubic } }

                Item {
                    id: body
                    width: parent.width
                    height: parent.height
                    // 当前句放大到 1,其余略小;以中心为原点,居中排版下左右对称地缩放
                    scale: row.current || !player.lyricsSynced ? 1 : 0.9
                    Behavior on scale { ScaleAnimator { duration: 620; easing.type: Easing.OutQuint } }

                    // 单行宽度用 TextMetrics 量 (换行文字的 implicitWidth 依赖自身宽度,直接用会形成绑定环)
                    TextMetrics { id: lineMetrics; font: line.font; text: line.text }
                    Text {
                        id: line
                        // 单行时宽度贴合文字,逐字点亮的进度才能对准文字本身;换行时占满整栏
                        width: Math.min(row.width - 24, Math.ceil(lineMetrics.advanceWidth) + 2)
                        x: (row.width - width) / 2
                        y: 9
                        text: modelData.text
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        color: row.current ? Qt.lighter(Theme.accent, 1.12) : Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: player.lyricsSynced ? lyricView.fontSize : 18
                        font.weight: player.lyricsSynced ? Font.Bold : Font.Medium
                        lineHeight: 1.18
                        Behavior on color { ColorAnimation { duration: 360 } }

                        // 当前句:未唱部分压暗,随演唱进度从左到右点亮 (交界处约 28px 柔和过渡)
                        layer.enabled: row.current
                        layer.effect: ShaderEffect {
                            readonly property real start: modelData.time
                            readonly property real span: modelData.duration > 0 ? modelData.duration : 3
                            property real progress: row.current ? Math.max(0, Math.min(1, (player.position + 0.12 - start) / span)) : 0
                            property real lines: Math.max(1, line.lineCount)
                            property real soft: 14 / Math.max(1, line.width)
                            property real dim: 0.5
                            fragmentShader: "qrc:/shaders/wipe.frag.qsb"
                        }
                    }
                    Text {
                        id: sub
                        visible: row.translation.length > 0
                        anchors.top: line.bottom
                        anchors.topMargin: 2
                        x: 12
                        width: row.width - 24
                        text: row.translation
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Math.round(lyricView.fontSize * 0.6)
                        font.weight: Font.Medium
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: modelData.time >= 0
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { lyricView.following = true; resume.stop(); player.seek(modelData.time) }
                }
            }
        }
        Column {
            anchors.centerIn: parent
            visible: side.tab === "lyrics" && !side.hasLyrics
            spacing: 12
            Icon { anchors.horizontalCenter: parent.horizontalCenter; name: "lyrics"; size: 36; opacity: 0.15 }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("没有找到歌词")
                color: Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 14
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("同名 .lrc · 内嵌歌词 · 酷狗歌词库 · 在线")
                color: Theme.textTertiary
                opacity: 0.7
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
            Item { width: 1; height: 4 }
            Capsule {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("搜索歌词")
                filled: true
                pad: 20
                onClicked: lyricPanel.open()
            }
        }

        // 接下来播放:标题 + 剩余
        Row {
            visible: side.tab === "queue"
            // 在顶部窗口拖动条与收起按钮的高度以下
            y: 92
            x: 10
            spacing: 10
            Text {
                anchors.baseline: queueCount.baseline
                text: qsTr("接下来播放")
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 18
                font.weight: Font.Bold
            }
            Text {
                id: queueCount
                text: qsTr("%1 首 · 剩余 %2").arg(Math.max(0, player.queueLength - player.queuePosition - 1)).arg(player.queueRemaining)
                color: Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
        }

        // 接下来播放
        ListView {
            id: queueView
            y: 128
            width: parent.width
            height: parent.height - y - 32
            clip: true
            visible: side.tab === "queue"
            model: player.queueModel
            boundsBehavior: Flickable.StopAtBounds
            spacing: 2
            cacheBuffer: 400
            reuseItems: true
            SmoothScroll { flickable: queueView }
            onVisibleChanged: if (visible) positionViewAtIndex(Math.max(0, player.queuePosition - 1), ListView.Beginning)
            delegate: Item {
                width: queueView.width
                height: 54
                readonly property bool current: model.queueIndex === player.queuePosition
                readonly property bool played: model.queueIndex < player.queuePosition
                readonly property bool hovered: rowMouse.containsMouse || rm.hovered
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusM
                    color: current ? Theme.accentAlpha(0.12) : "transparent"
                }
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusM
                    color: Theme.hover
                    opacity: hovered && !current ? 1 : 0
                    Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    // 滚动时行从鼠标下掠过,不逐行闪高亮
                    hoverEnabled: !Theme.scrolling
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: Qt.PointingHandCursor
                    onClicked: (e) => {
                        if (e.button === Qt.RightButton) {
                            const p = mapToItem(null, e.x, e.y)
                            Nav.trackMenu(model.trackId, p.x, p.y, { queueIndex: model.queueIndex })
                        } else if (!current) player.playQueueIndex(model.queueIndex)
                    }
                }
                RoundedImage {
                    id: qc
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 38; height: 38
                    radius: 6
                    source: model.cover
                    decodeSize: 76
                    opacity: played && !current ? 0.5 : 1
                }
                Loader {
                    anchors.centerIn: qc
                    active: current
                    sourceComponent: Rectangle {
                        width: 38; height: 38; radius: 6
                        color: Qt.rgba(0, 0, 0, 0.5)
                        PlayingBars { anchors.centerIn: parent; maxHeight: 13 }
                    }
                }
                Column {
                    anchors.left: qc.right
                    anchors.leftMargin: 14
                    anchors.right: dur.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Text {
                        width: parent.width
                        text: model.title
                        elide: Text.ElideRight
                        color: current ? Theme.accent : (played ? Theme.textTertiary : Theme.text)
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: current ? Font.DemiBold : Font.Normal
                    }
                    Text {
                        width: parent.width
                        text: model.artist
                        elide: Text.ElideRight
                        color: Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }
                }
                Text {
                    id: dur
                    anchors.right: rm.left
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    text: model.duration
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                }
                IconButton {
                    id: rm
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 30; height: 30
                    icon: "close"
                    iconSize: 12
                    enabled: !current
                    opacity: hovered && !current ? 1 : 0
                    tip: qsTr("从播放队列移除")
                    onClicked: player.removeFromQueue(model.queueIndex)
                }
            }
        }
    }
}
