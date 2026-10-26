import QtQuick
import QtQuick.Window
import QtQuick.Dialogs
import PlayerUi

Window {
    id: root
    width: 1110
    height: 780
    minimumWidth: 900
    minimumHeight: 600
    visible: true
    title: player.hasTrack ? player.title + " — " + player.artist : "拾羽"
    color: Theme.base
    flags: Qt.Window | Qt.FramelessWindowHint

    Component.onCompleted: windowHelper.applyNativeFrame(root)

    // 强调色跟随当前专辑
    // 强调色:跟随封面,或设置页里选的固定色
    Binding { target: Theme; property: "accent"; value: settings.accent >= 0 ? settings.accentPresets[settings.accent] : player.accent }
    Binding { target: Theme; property: "reduced"; value: settings.reducedMotion }

    // 页面加载器:后台异步预创建 (不卡当前帧),之后常驻;进场稍慢带轻微放大,退场更快,两页交叠时间短
    component PageLoader: Loader {
        property string key
        property bool allowEmpty: false
        readonly property bool current: root.view === key && (allowEmpty || !pages.libraryEmpty)
        property bool used: false
        readonly property bool scrolled: current && item !== null && item.scrolled === true
        onCurrentChanged: if (current) used = true
        // 启动后空闲时把所有页面都建好。
        // 只靠 used 的话,页面是在"第一次切过去"的那一刻才实例化的 ——
        // 全屏下这一步有 200ms 级的尖峰(实测切歌手页 max 220ms),用户感知就是"切页卡一下"。
        // 启动后 900ms 逐个预建,代价挪到用户还没开始操作的时候。
        active: used || root.preload
        asynchronous: !current
        anchors.fill: parent
        opacity: current && status === Loader.Ready ? 1 : 0
        visible: opacity > 0
        scale: current ? 1 : 0.985
        // 过渡期间把整页渲染到一张纹理上,之后 opacity / scale 变化只是合成纹理。
        // 否则 4K 全屏下两个复杂页面要同时重绘,切换动画的每一帧都是双倍成本
        layer.enabled: opacity > 0.02 && opacity < 0.98
        Behavior on opacity { NumberAnimation { duration: current ? 300 : 90; easing.type: Easing.OutCubic } }
        Behavior on scale { ScaleAnimator { duration: current ? 460 : 150; easing.type: Easing.OutQuint } }
    }

    // 导航状态:section = 侧栏分区;detail = 分区内打开的详情 (album / artist / playlist)
    property string section: "albums"
    property string detailKind: ""
    property var detailRef: -1
    readonly property string view: detailKind.length ? detailKind : section
    // 封面墙是"无 UI"的沉浸页:进去就把导航、播放条、窗口按钮全隐掉,
    // 鼠标贴近边缘时再浮出来(否则用户没有入口切走)
    readonly property bool burstMode: view === "burst"
    property bool burstChrome: false
    onBurstModeChanged: burstChrome = false
    // 启动后空闲时预建所有页面(见 PageLoader)
    property bool preload: false
    Timer {
        interval: 900
        running: true
        onTriggered: root.preload = true
    }
    readonly property bool detailShown: detailKind === "album"
    property var history: []
    // 详情页各自记住最后打开的对象,切到别的详情时不跟着重载
    property int lastArtist: -1
    property string lastArtistName: ""
    property string lastPlaylist: ""
    // 离开搜索页时把键盘焦点从 (已隐藏的) 搜索框收回:空格、方向键、直接打字照常可用
    // 关掉主窗口就退出整个程序:桌面歌词是另一个顶层窗口,否则它会留在桌面上、进程也不退出,
    // 再打开软件就会出现两个桌面歌词
    onClosing: Qt.quit()
    onViewChanged: { trackMenu.close(); if (view !== "search") typeCatcher.forceActiveFocus() }
    // 焦点落空 (编辑框 / 弹出卡片隐去) 时回到按键捕获
    onActiveFocusItemChanged: if (activeFocusItem === null || activeFocusItem === contentItem) Qt.callLater(root.restoreKeys)
    function restoreKeys() { if (root.active && (root.activeFocusItem === null || root.activeFocusItem === contentItem)) typeCatcher.forceActiveFocus() }
    onDetailRefChanged: syncDetail()
    onDetailKindChanged: syncDetail()
    // 换详情类型时 detailKind 先变、detailRef 后变:类型对不上的那一刻跳过 (歌手是下标,歌单是 id 字符串)
    function syncDetail() {
        if (detailKind === "artist" && typeof detailRef === "number") {
            lastArtist = detailRef
            lastArtistName = library.artistInfo(detailRef).name || ""
        }
        else if (detailKind === "playlist" && typeof detailRef === "string") lastPlaylist = detailRef
    }

    // 重新扫描后专辑 / 歌手的下标都会变:打开着的详情页跟着换到新下标 (专辑由曲目列表模型找回),
    // 找不到了就退回分区页;历史里的旧下标已经无效,直接丢掉,免得「返回」打开别的专辑
    Connections {
        target: library
        function onLibraryChanged() {
            const h = root.history.filter(st => st.kind !== "album" && st.kind !== "artist")
            if (h.length !== root.history.length) root.history = h
            if (root.lastArtistName.length) {
                const ai = library.artistIndexOf(root.lastArtistName)
                if (root.detailKind === "artist") {
                    if (ai >= 0) root.detailRef = ai
                    else root.detailKind = ""
                }
                root.lastArtist = ai
            }
            if (root.detailKind === "album") {
                if (albumTracks.albumIndex >= 0) root.detailRef = albumTracks.albumIndex
                else { root.detailKind = ""; Nav.toast("albums", qsTr("这张专辑已不在曲库中")) }
            }
        }
    }

    function snapshotState() { return { section: section, kind: detailKind, ref: detailRef } }
    function pushHistory() {
        const h = history.slice()
        h.push(snapshotState())
        if (h.length > 40) h.shift()
        history = h
    }
    function applyState(st) {
        section = st.section
        detailKind = st.kind
        detailRef = st.ref
        if (st.kind === "album") albumTracks.albumIndex = st.ref
    }
    function go(sec) {
        if (section === sec && !detailKind.length) {
            // 已在专辑墙时再点「专辑」:回到顶部
            if (sec === "albums") wall.scrollToTop()
            else if (sec === "burst" && burstLoader.item) burstLoader.item.scrollToTop()
            return
        }
        pushHistory()
        section = sec
        detailKind = ""
    }
    function openDetail(kind, ref) {
        if (detailKind === kind && detailRef === ref) return
        pushHistory()
        detailKind = kind
        detailRef = ref
    }
    function back() {
        if (!history.length) {
            if (detailKind.length) { closeAlbumFlight(); detailKind = "" }
            return
        }
        const h = history.slice()
        const st = h.pop()
        history = h
        if (detailKind === "album" && st.kind === "") closeAlbumFlight()
        if (detailKind === "artist" && st.kind === "" && st.section === "artists") closeArtistFlight()
        applyState(st)
    }
    function openSearch(text) {
        if (section !== "search" || detailKind.length) { pushHistory(); section = "search"; detailKind = "" }
        Qt.callLater(function() { if (searchLoader.item) searchLoader.item.focusWith(text) })
    }
    function openArtist(i, fromItem) {
        if (i < 0) return
        openDetail("artist", i)
        if (!fromItem) return
        Qt.callLater(function() {
            const hero = artistLoader.item ? artistLoader.item.heroAvatar : null
            if (hero) fly(fromItem, null, hero, artistLoader)
        })
    }
    function openPlaylist(id) { if (id && id.length) openDetail("playlist", id) }
    function closeOverlays() { nowPlaying.shown = false; queuePanel.shown = false; chain.shown = false }
    // 静音 / 恢复:记住静音前的音量 (播放条与沉浸页共用)
    property real lastVolume: 0.85
    function toggleMute() {
        if (player.volume > 0.01) { lastVolume = player.volume; player.volume = 0 }
        else player.volume = Math.max(0.05, lastVolume)
    }

    Connections {
        target: Nav
        // 从沉浸页 / 队列抽屉里跳转:先收起它们,新页面才看得见
        function onOpenAlbum(i) { if (i >= 0) { root.closeOverlays(); root.openAlbum(i, null) } }
        function onOpenArtist(i) { if (i >= 0) { root.closeOverlays(); root.openArtist(i, null) } }
        function onOpenArtistFrom(i, item) { root.closeOverlays(); root.openArtist(i, item) }
        function onOpenPlaylist(id) { root.closeOverlays(); root.openPlaylist(id) }
        function onOpenImport() { root.closeOverlays(); playlistImport.open() }
        function onToggleMute() { root.toggleMute() }
        function onHintRight(item, text) { hoverTip.show(item, text, "right") }
        function onTrackMenu(id, x, y, ctx) { trackMenu.open(id, x, y, ctx) }
        function onViewCover(src, t, s) { coverViewer.open(src, t, s) }
        function onHint(item, text) { hoverTip.show(item, text) }
        function onUnhint(item) { hoverTip.hide(item) }
        function onToast(icon, text) { toast.show(icon, text) }
        function onToastAction(text, actionText, fn) { toast.showAction(text, actionText, fn) }
    }

    // ======================= 场景层 (同时作为毛玻璃的采样源) =======================
    Item {
        id: scene
        anchors.fill: parent

        Aurora {
            anchors.fill: parent
            // 封面背景完全盖住时停止绘制 (省 GPU)
            visible: !nowPlaying.covering && homeBackdrop.opacity < 1
        }
        // 播放时首页背景与播放页同一套:当前封面模糊铺满、压暗,换歌时交叉淡化;再叠一层颗粒防色带
        Backdrop {
            id: homeBackdrop
            anchors.fill: parent
            // 播放页也用这层作背景,盖住时不能停
            visible: opacity > 0
            opacity: player.hasTrack ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 900; easing.type: Easing.InOutQuad } }
            source: player.cover
            solid: false
        }
        ShaderEffect {
            anchors.fill: parent
            visible: homeBackdrop.visible
            opacity: homeBackdrop.opacity
            property size size: Qt.size(width, height)
            property real strength: 0.022
            fragmentShader: "qrc:/shaders/grain.frag.qsb"
        }

        Item {
            id: pages
            anchors.fill: parent
            visible: !nowPlaying.covering
            // 打开播放页时首页内容跟着淡出 (背景不动),收起时再淡回来
            opacity: nowPlaying.shown ? 0 : 1
            Behavior on opacity { OpacityAnimator { duration: nowPlaying.shown ? Theme.normal : 420; easing.type: Easing.OutCubic } }
            anchors.leftMargin: Theme.navWidth
            readonly property int inset: island.height + 28
            readonly property bool libraryEmpty: library.albumCount === 0

            AlbumWall {
                id: wall
                anchors.fill: parent
                bottomInset: pages.inset
                readonly property bool active: root.view === "albums" && !pages.libraryEmpty
                opacity: active ? 1 : 0
                visible: opacity > 0
                scale: active ? 1 : 0.985
                Behavior on opacity { NumberAnimation { duration: wall.active ? 300 : 150; easing.type: Easing.OutCubic } }
                Behavior on scale { ScaleAnimator { duration: wall.active ? 460 : 150; easing.type: Easing.OutQuint } }
                onOpenAlbum: (index, coverItem) => root.openAlbum(index, coverItem)
            }

            AlbumDetail {
                id: detail
                anchors.fill: parent
                bottomInset: pages.inset
                readonly property bool active: root.view === "album"
                opacity: active ? 1 : 0
                visible: opacity > 0
                scale: active ? 1 : 1.015
                Behavior on opacity { NumberAnimation { duration: detail.active ? 300 : 150; easing.type: Easing.OutCubic } }
                Behavior on scale { ScaleAnimator { duration: detail.active ? 460 : 150; easing.type: Easing.OutQuint } }
                onBack: root.back()
            }

            // 其余页面首次进入时才创建,之后常驻 (切换不重建,滚动位置保留)
            PageLoader {
                id: burstLoader
                key: "burst"
                sourceComponent: CoverBurst {
                    onOpenAlbum: (index, coverItem) => root.openAlbum(index, coverItem)
                    onExitRequested: root.go("albums")
                    onChromeHint: (show) => root.burstChrome = show
                }
            }
            PageLoader { id: artistsLoader; key: "artists"; sourceComponent: ArtistsPage { bottomInset: pages.inset } }
            PageLoader { id: songsLoader; key: "songs"; sourceComponent: SongsPage { bottomInset: pages.inset } }
            PageLoader { id: playlistsLoader; key: "playlists"; sourceComponent: PlaylistsPage { bottomInset: pages.inset } }
            PageLoader {
                id: searchLoader
                key: "search"
                allowEmpty: true
                sourceComponent: SearchPage { bottomInset: pages.inset; onBack: root.back() }
            }
            PageLoader { id: settingsLoader; key: "settings"; allowEmpty: true; sourceComponent: SettingsPage { bottomInset: pages.inset } }
            PageLoader { id: toolsLoader; key: "tools"; allowEmpty: true; sourceComponent: ToolsPage { bottomInset: pages.inset } }
            PageLoader { id: organizeLoader; key: "organize"; sourceComponent: OrganizePage { bottomInset: pages.inset; onBack: root.back() } }
            PageLoader { id: dedupeLoader; key: "dedupe"; sourceComponent: DedupePage { bottomInset: pages.inset; onBack: root.back() } }
            PageLoader {
                id: artistLoader
                key: "artist"
                sourceComponent: ArtistDetail { bottomInset: pages.inset; artistIndex: root.lastArtist; onBack: root.back() }
            }
            PageLoader {
                id: playlistLoader
                key: "playlist"
                allowEmpty: true
                sourceComponent: PlaylistDetail { bottomInset: pages.inset; playlistId: root.lastPlaylist; onBack: root.back() }
            }

            // 曲库就绪后在空闲时依次预创建其余页面,第一次切换也不会卡顿
            Timer {
                id: preload
                interval: 350
                repeat: true
                running: !pages.libraryEmpty && !library.scanning
                property int step: 0
                onTriggered: {
                    const list = [artistsLoader, songsLoader, playlistsLoader, searchLoader, artistLoader, playlistLoader]
                    if (step >= list.length) { stop(); return }
                    list[step++].used = true
                }
            }

            // 曲库为空:引导选择文件夹;首次导入时显示识别进度 (歌单与搜索页除外)
            Onboarding {
                anchors.fill: parent
                visible: opacity > 0
                opacity: pages.libraryEmpty && ["playlist", "playlists", "search", "tools", "settings"].indexOf(root.view) < 0 ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: Theme.slow; easing.type: Easing.OutCubic } }
                onPickFolder: folderDialog.open()
            }
        }

        NowPlaying {
            id: nowPlaying
            anchors.fill: parent
            z: 5
            onCloseRequested: shown = false
            chainOpen: chain.shown
            onChainClicked: chain.shown = !chain.shown
            onShownChanged: { if (shown) queuePanel.shown = false; Qt.callLater(root.flyNowPlaying, shown) }
        }

        SideNav {
            id: nav
            visible: !nowPlaying.covering
            // 封面墙(无 UI 模式)里侧栏淡出。注意这里必须走 opacity 而不是 visible ——
            // 直接切 visible 是瞬时的,从封面墙点进专辑时侧栏会"啪"地冒出来,看着像闪了一下
            opacity: nowPlaying.shown ? 0 : (root.burstMode && !root.burstChrome ? 0 : 1)
            enabled: opacity > 0.05
            // 打开播放页时首页内容跟着淡出 (背景不动),收起时再淡回来
            Behavior on opacity { OpacityAnimator { duration: nowPlaying.shown ? Theme.normal : 420; easing.type: Easing.OutCubic } }
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.bottomMargin: island.height
            // 整理曲库 / 查找重复从「音乐文件夹」面板进入:侧栏标在音乐文件夹上
            current: ["organize", "dedupe"].indexOf(root.section) >= 0 ? "folder" : root.section
            onNavigate: (target) => {
                if (target === "folder") library.folders.length ? libraryPanel.open() : folderDialog.open()
                else if (target === "search") root.openSearch("")
                else root.go(target)
            }
        }
    }

    // ======================= 共享元素:飞行的封面 =======================
    // 两层:底层用来源图块已解码好的那张 (缓存命中,首帧就有画面),上层是落点尺寸的清晰版,就绪后直接盖上 (同一张图,只是更清晰)
    Item {
        id: flying
        visible: false
        transformOrigin: Item.TopLeft
        z: 50
        property real radius: 16
        property string loSource
        property int loDecode: 640
        property string hiSource
        property int hiDecode: 640
        // 副本自带的投影 (落点尺寸坐标):飞向大封面时随落地渐浓,交接时与落点自己的投影无缝衔接
        property real shadow: 0
        property real shadowSpread: 44
        property real shadowOffset: 24
        BorderImage {
            x: -flying.shadowSpread
            y: -flying.shadowSpread + flying.shadowOffset
            width: flying.width + flying.shadowSpread * 2
            height: flying.height + flying.shadowSpread * 2
            visible: flying.shadow > 0.01
            opacity: flying.shadow
            source: "qrc:/images/shadow.png"
            border { left: 64; top: 64; right: 64; bottom: 64 }
            smooth: true
        }

        // 起落点是旋转中的黑胶标签时,副本带着标签此刻的角度起飞 / 落地,不会一下子摆正
        Item {
            id: flyingSpin
            anchors.fill: parent
            RoundedImage {
                anchors.fill: parent
                radius: flying.radius
                source: flying.loSource
                decodeSize: flying.loDecode
                fadeIn: false
                placeholder: false
                outline: false
            }
            RoundedImage {
                id: flyingHi
                anchors.fill: parent
                radius: flying.radius
                source: flying.hiSource
                decodeSize: flying.hiDecode
                fadeIn: false
                placeholder: false
            }
        }

        ParallelAnimation {
            id: flight
            property real toX
            property real toY
            property real toScale: 1
            property real toRadius: 10
            property real toAngle: 0
            property Item landing: null // 落点:飞行期间隐藏,落地后显示
            property Item origin: null  // 起点:飞行期间隐藏,避免同一张封面出现两份
            property Item waitFor: null // 落地前要等它解码完成的图片
            property int duration: 460
            property real toShadow: 0
            property int shadowEasing: Easing.InQuad
            property Item landingShadow: null // 落点的投影:交接时直接到位,不再从零淡入
            XAnimator { target: flying; to: flight.toX; duration: flight.duration; easing.type: Easing.OutQuint }
            YAnimator { target: flying; to: flight.toY; duration: flight.duration; easing.type: Easing.OutQuint }
            ScaleAnimator { target: flying; to: flight.toScale; duration: flight.duration; easing.type: Easing.OutQuint }
            // 缩放会连带缩小圆角:同步补偿,让屏幕上的圆角从起点平滑过渡到落点
            NumberAnimation { target: flying; property: "radius"; to: flight.toRadius; duration: flight.duration; easing.type: Easing.OutQuint }
            RotationAnimator { target: flyingSpin; to: flight.toAngle; duration: flight.duration; easing.type: Easing.OutQuint }
            NumberAnimation { target: flying; property: "shadow"; to: flight.toShadow; duration: flight.duration; easing.type: flight.shadowEasing }
            onFinished: handoff.start()
        }
        // 落地交接:等落点图片解码完成再显示它,随后再撤掉飞行副本 (落点自己的淡入在副本下面完成,看不出切换)
        Timer {
            id: handoff
            interval: 16
            repeat: true
            property int waited: 0
            onRunningChanged: if (running) waited = 0
            onTriggered: {
                waited += interval
                const img = flight.waitFor
                if (img && img.status !== Image.Ready && img.status !== Image.Error && waited < 1500) return
                stop()
                const hidden = flight.landing
                if (flight.landingShadow) flight.landingShadow.snap = true
                flight.landingShadow = null
                if (flight.landing) flight.landing.opacity = 1
                if (flight.origin) flight.origin.opacity = 1
                if (detail.heroCover) detail.heroCover.opacity = 1
                detail.heroAway = false
                flight.landing = null
                flight.origin = null
                flight.waitFor = null
                // 落点一直可见 (飞回黑胶标签):副本在它正上方,立即撤掉;否则等落点自己的淡入完成
                if (hidden) reveal.restart()
                else flying.visible = false
            }
        }
        Timer { id: reveal; interval: Theme.normal + 40; onTriggered: flying.visible = false }
        // 飞行途中落点被挪走了 (返回后立刻滚动、窗口改变大小导致重新排版):副本飞向的已是旧位置,
        // 看起来像封面被带着滚走或飞走。这时放弃这段飞行,副本就地淡出,落点在它真正的位置直接显示
        Timer {
            id: flightGuard
            interval: 16
            repeat: true
            running: flight.running || handoff.running
            property Item item: null
            property Item page: null
            property rect at
            onTriggered: {
                if (!item) return
                const r = root.settledRect(item, page)
                if (Math.abs(r.x - at.x) > 3 || Math.abs(r.y - at.y) > 3 || Math.abs(r.width - at.width) > 3)
                    root.abortFlight()
            }
        }
        OpacityAnimator {
            id: flyFade
            target: flying
            from: 1
            to: 0
            duration: 140
            easing.type: Easing.OutCubic
            onFinished: { flying.visible = false; flying.opacity = 1 }
        }
    }
    function abortFlight() {
        flightGuard.item = null
        flight.stop()
        handoff.stop()
        reveal.stop()
        if (flight.landingShadow) flight.landingShadow.snap = true
        flight.landingShadow = null
        if (flight.landing) flight.landing.opacity = 1
        if (flight.origin) flight.origin.opacity = 1
        if (detail.heroCover) detail.heroCover.opacity = 1
        detail.heroAway = false
        flight.landing = null
        flight.origin = null
        flight.waitFor = null
        if (flying.visible) flyFade.restart()
    }
    // 图块露在页面区域里的比例:起点或落点大半在视野外 (详情页滚下去了、窗口小了) 时不飞,
    // 否则副本会从屏幕外飞进来或飞到屏幕外去
    function shownOnPage(item) {
        if (!item || item.width <= 0 || item.height <= 0) return 0
        const r = settledRect(item, null)
        const a = pages.mapToItem(root.contentItem, 0, 0)
        const w = Math.max(0, Math.min(r.x + r.width, a.x + pages.width) - Math.max(r.x, a.x))
        const h = Math.max(0, Math.min(r.y + r.height, a.y + pages.height) - Math.max(r.y, a.y))
        return w * h / (r.width * r.height)
    }

    // 图块里真正显示封面的 RoundedImage (来源可能是卡片容器或交叉淡化封面)
    function imageIn(item) {
        if (!item) return null
        if (item.status !== undefined && item.decodeSize !== undefined) return item
        let best = null
        for (let k = 0; k < item.children.length; ++k) {
            const c = imageIn(item.children[k])
            if (c && (!best || (c.opacity > best.opacity) || (c.status === Image.Ready && best.status !== Image.Ready))) best = c
        }
        return best
    }
    // 图块在屏幕上的矩形。page 正处于切页缩放 (0.985 / 1.015 → 1) 时,按它的缩放中心换算成停稳后的位置与尺寸,
    // 否则飞行落在缩放中途的位置,页面停稳后封面会再跳一下
    function settledRect(item, page) {
        const p = item.mapToItem(root.contentItem, 0, 0)
        const q = item.mapToItem(root.contentItem, item.width, item.height)
        const s = page && page.scale > 0 ? page.scale : 1
        if (s === 1) return Qt.rect(p.x, p.y, q.x - p.x, q.y - p.y)
        const c = page.mapToItem(root.contentItem, page.width / 2, page.height / 2)
        return Qt.rect(c.x + (p.x - c.x) / s, c.y + (p.y - c.y) / s, (q.x - p.x) / s, (q.y - p.y) / s)
    }
    // opts (可选):fromRect / toRect 覆盖屏幕矩形,fromRadius / toRadius 覆盖屏幕圆角,
    // hideFrom / hideTo = false 时飞行期间不隐藏起点 / 落点,duration 飞行时长,
    // guard = true 时落点途中被挪走 (滚动、重新排版) 就放弃飞行
    function fly(fromItem, fromPage, toItem, toPage, opts) {
        opts = opts || {}
        // 上一段飞行还没收尾:先把它藏起的图块还原
        flight.stop()
        handoff.stop()
        reveal.stop()
        flyFade.stop()
        flying.opacity = 1
        flightGuard.item = null
        if (flight.landing) flight.landing.opacity = 1
        if (flight.origin) flight.origin.opacity = 1
        flight.landing = null
        flight.origin = null
        flight.landingShadow = null

        const fromImg = imageIn(fromItem), toImg = imageIn(toItem)
        if (!fromImg || !toImg || fromItem.width <= 0 || toItem.width <= 0) return false
        const a = opts.fromRect || settledRect(fromItem, fromPage)
        const b = opts.toRect || settledRect(toItem, toPage)
        if (a.width <= 0 || b.width <= 0) return false
        const fromRadius = opts.fromRadius !== undefined ? opts.fromRadius
                         : (fromItem.radius !== undefined ? fromItem.radius : fromImg.radius) * a.width / fromItem.width
        const toRadius = opts.toRadius !== undefined ? opts.toRadius
                       : (toItem.radius !== undefined ? toItem.radius : toImg.radius) * b.width / toItem.width

        const fromReady = fromImg.status === Image.Ready
        flying.loSource = fromReady ? fromImg.source : toImg.source
        flying.loDecode = fromReady ? fromImg.decodeSize : toImg.decodeSize
        // 落点更清晰时才叠加上层 (飞向小图时不要用低分辨率版本盖住清晰的起点图)
        flying.hiSource = toImg.decodeSize > flying.loDecode ? toImg.source : ""
        flying.hiDecode = toImg.decodeSize
        flying.width = b.width
        flying.height = b.height
        flying.x = a.x
        flying.y = a.y
        flying.scale = a.width / b.width
        flying.radius = fromRadius / flying.scale
        // 角度取最近的等价值,免得多转一圈
        const norm = (d) => { d = d % 360; return d > 180 ? d - 360 : (d < -180 ? d + 360 : d) }
        const fromAngle = norm(opts.fromAngle || 0)
        flyingSpin.rotation = fromAngle
        flight.toAngle = fromAngle + norm((opts.toAngle || 0) - fromAngle)
        flight.duration = opts.duration || 460
        flight.waitFor = toImg
        // 副本投影:opts.shadow = { from, to, spread, offsetY, shadowItem (落点投影) }
        const sh = opts.shadow || { from: 0, to: 0 }
        flying.shadow = sh.from || 0
        flight.toShadow = sh.to || 0
        flying.shadowSpread = sh.spread || 44
        flying.shadowOffset = sh.offsetY || 0
        flight.shadowEasing = (sh.to || 0) >= (sh.from || 0) ? Easing.InQuad : Easing.OutCubic
        flight.landingShadow = sh.shadowItem || null
        flight.landing = opts.hideTo === false ? null : toItem
        flight.origin = opts.hideFrom === false ? null : fromItem
        if (flight.landing) flight.landing.opacity = 0
        if (flight.origin) flight.origin.opacity = 0
        flying.visible = true
        flight.toX = b.x
        flight.toY = b.y
        flight.toScale = 1
        flight.toRadius = toRadius
        // 返回时飞回列表里的图块:盯住它 (opts.guard)
        if (opts.guard && !opts.toRect) { flightGuard.item = toItem; flightGuard.page = toPage || null; flightGuard.at = b }
        flight.restart()
        return true
    }
    // 播放岛迷你黑胶的中心标签 (封面所在的小圆) 在屏幕上的矩形
    function discLabelRect() {
        const disc = island.disc
        const r = settledRect(disc, null)
        r.y -= islandShift.y // 岛正在进出场位移时,按停稳后的位置算
        const s = r.width * disc.labelRatio
        return Qt.rect(r.x + (r.width - s) / 2, r.y + (r.height - s) / 2, s, s)
    }
    // 展开沉浸页:封面从黑胶标签里「升起」放大成大封面;收起时缩回标签
    function flyNowPlaying(open) {
        if (!player.hasTrack) return
        const big = nowPlaying.coverItem
        if (!big) return
        const disc = island.disc
        const label = discLabelRect()
        // 投影只在大封面这一端:打开时随落地渐浓并和落点投影无缝交接;收起时起飞即开始变淡
        const s = nowPlaying.coverShadow
        const shadow = s ? { spread: s.spread, offsetY: s.offsetY, shadowItem: open ? s : null,
                             from: open ? 0 : s.strength, to: open ? s.strength : 0 } : null
        // 减弱动效:飞行照常 (这是界面的空间线索),只是更短
        if (open) {
            fly(disc, null, big, null, { fromRect: label, fromRadius: label.width / 2, fromAngle: disc.angleAfter(0),
                                         hideFrom: false, duration: Theme.reduced ? 360 : 620, shadow: shadow })
        } else {
            const d = Theme.reduced ? 320 : 520
            fly(big, null, disc, null, { toRect: label, toRadius: label.width / 2, toAngle: disc.angleAfter(d + 16),
                                         hideTo: false, duration: d, shadow: shadow })
        }
    }

    // 打开专辑时点的那张封面 (卡片或顶部横幅);关闭时若它仍在视野里就飞回它
    property Item albumFrom: null
    function openAlbum(index, coverItem) {
        if (index < 0) return
        // 已经在看这张专辑 (例如页尾或菜单里点了它自己):不重置列表
        if (detailKind === "album" && detailRef === index && albumTracks.albumIndex === index) return
        albumTracks.albumIndex = index
        detail.resetScroll()
        const fromItem = coverItem
        albumFrom = coverItem || null
        openDetail("album", index)
        // 从专辑墙打开时封面飞入详情页;其他入口直接淡入
        if (!fromItem) return
        Qt.callLater(function() {
            if (detail.heroCover) fly(fromItem, null, detail.heroCover, detail)
        })
    }

    // 返回专辑墙时封面飞回对应卡片 (专辑墙此刻仍在回弹缩放,落点按停稳后的位置算)
    function closeAlbumFlight() {
        if (detailKind !== "album" || section !== "albums") return
        const hero = detail.heroCover
        const card = wall.coverOf(albumTracks.albumIndex, albumFrom)
        if (!hero || !card || shownOnPage(hero) < 0.3) return
        if (fly(hero, null, card, wall, { guard: true })) detail.heroAway = true
    }
    function closeArtistFlight() {
        const page = artistLoader.item, grid = artistsLoader.item
        if (!page || !grid) return
        const hero = page.heroAvatar
        const cell = grid.avatarOf(root.lastArtist)
        if (hero && cell && shownOnPage(hero) >= 0.3 && shownOnPage(cell) >= 0.3) fly(hero, null, cell, artistsLoader, { guard: true })
    }
    function closeAlbum() { if (detailKind === "album") back() }

    // 顶部渐暗:内容滚到窗口顶端时,保证窗口按钮在浅色封面上依然可见。
    // 内容滚到窗口按钮下面时渐暗加深加长,文字柔和地隐进顶端,不和最小化 / 关闭按钮叠在一起
    // (不用整条实色或磨砂条:顶端会多出一条生硬的黑带)。专辑详情与沉浸页有自己的顶栏
    readonly property bool topCovered: !nowPlaying.shown && root.view !== "album"
        && (root.view === "albums" ? wall.active && wall.scrolled
            : [artistsLoader, songsLoader, playlistsLoader, settingsLoader, artistLoader, playlistLoader].some(l => l.scrolled))
    Rectangle {
        id: topShade
        anchors.left: parent.left
        anchors.right: parent.right
        z: 9
        property real deep: root.topCovered ? 1 : 0
        Behavior on deep { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
        height: 56 + 24 * deep
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.38 + 0.2 * topShade.deep) }
            GradientStop { position: 0.45; color: Qt.rgba(0, 0, 0, 0.12 + 0.2 * topShade.deep) }
            GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0) }
        }
    }

    // ======================= 隐形标题栏 =======================
    TitleBar {
        // 沉浸播放页里窗口按钮与页内控件一起随鼠标隐现 (拖动区仍然有效)
        // 封面墙同理:进去先隐掉,鼠标贴近顶部再浮出
        opacity: nowPlaying.shown ? nowPlaying.chrome : (root.burstMode && !root.burstChrome ? 0 : 1)
        anchors.left: parent.left
        anchors.leftMargin: Theme.navWidth
        anchors.right: parent.right
        anchors.top: parent.top
        window: root
        z: 10
    }

    // ======================= 播放队列抽屉 =======================
    QueuePanel {
        id: queuePanel
        z: 19
        width: Math.min(400, Math.max(320, root.width * 0.34))
        x: root.width - width
        // 从窗口按钮下方开始,不挡最小化 / 关闭
        y: 46
        height: island.y - y
        shown: false
        onCloseRequested: shown = false
    }

    // ======================= 底部播放条 =======================
    PlayIsland {
        id: island
        width: root.width
        x: 0
        y: root.height - height
        z: 20
        queueOpen: queuePanel.shown
        window: root.contentItem
        onQueueClicked: queuePanel.shown = !queuePanel.shown
        // 沉浸页有自己的控制区,岛下沉隐去;封面墙(无 UI 模式)也隐去
        readonly property bool away: nowPlaying.shown || (root.burstMode && !root.burstChrome)
                                    || (library.albumCount === 0 && !player.hasTrack)
        opacity: away ? 0 : 1
        visible: opacity > 0
        // 进出场:淡出时下沉,回来时等沉浸页淡出一点后再从下方浮上来
        Behavior on opacity { SequentialAnimation {
            PauseAnimation { duration: island.away ? 0 : 140 }
            NumberAnimation { duration: island.away ? 180 : 420; easing.type: Easing.OutCubic } } }
        transform: Translate {
            id: islandShift
            y: island.away ? island.height + 12 : 0
            Behavior on y { SequentialAnimation {
                PauseAnimation { duration: island.away ? 0 : 140 }
                NumberAnimation { duration: island.away ? 200 : 620; easing.type: Easing.OutQuint } } }
        }
        onChainClicked: chain.shown = !chain.shown
        onExpandRequested: nowPlaying.shown = true
    }

    SignalChain {
        id: chain
        z: 30
        // 沉浸页中从左栏信号链行展开,其余情况从播放岛上方展开
        x: nowPlaying.shown ? Math.max(24, (root.width - width) / 2) : Math.min(root.width - width - 24, island.x + island.width - width - 10)
        y: nowPlaying.shown ? (root.height - height) / 2 : island.y - height - 12
    }
    MouseArea {
        anchors.fill: parent
        z: 29
        visible: chain.shown
        onClicked: chain.shown = false
    }

    ScanToast {
        // 首次导入时由引导页展示完整进度,不重复显示
        enabled: library.albumCount > 0
        // 沉浸页顶部正中是音质胶囊、右栏顶上是「接下来播放」:扫描提示改到右栏底部,轻提示的上方
        x: nowPlaying.shown ? Math.round(nowPlaying.sideX + (nowPlaying.sideW - width) / 2) : Math.round((root.width - width) / 2)
        y: nowPlaying.shown ? root.height - height - 80 : 18
        z: 25
    }

    // 播放失败:用同一个轻提示 (句首警示色圆点),不另起一套配色和位置
    Connections { target: player; function onErrorChanged() { if (player.errorText.length) toast.showWarn(player.errorText) } }

    DecryptToast {
        z: 25
        x: nowPlaying.shown ? Math.round(nowPlaying.sideX + (nowPlaying.sideW - width) / 2) : Math.round((root.width - width) / 2)
        y: nowPlaying.shown ? root.height - height - 80 - (library.scanShown ? 46 : 0) : 18 + (library.scanShown ? 46 : 0)
        Behavior on y { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
    }

    DecryptPrompt {
        id: decryptPrompt
        z: 24
        anchors.horizontalCenter: island.horizontalCenter
        y: island.y - height - 14
    }
    Connections {
        target: decrypt
        function onPromptRequested() { decryptPrompt.open() }
    }

    TrackMenu {
        id: trackMenu
        z: 45
        onInfoRequested: (id) => trackInfo.open(id)
    }
    TrackInfo {
        id: trackInfo
        z: 44
        onShownChanged: if (!shown) typeCatcher.forceActiveFocus()
    }
    PlaylistImport {
        id: playlistImport
        z: 44
    }
    CoverViewer {
        id: coverViewer
        z: 45
    }

    HoverTip {
        id: hoverTip
    }

    Toast {
        id: toast
        z: 46
        // 沉浸页里播放岛隐去:提示放在右栏 (歌词区) 底部正中,不压左栏的音量条;转换提示条在时叠在它上面
        x: nowPlaying.shown ? Math.round(nowPlaying.sideX + (nowPlaying.sideW - width) / 2) : Math.round((root.width - width) / 2)
        y: decryptPrompt.shown && !nowPlaying.shown ? decryptPrompt.y - height - 8
           : nowPlaying.shown ? root.height - height - 28 : island.y - height - 16
    }

    LibraryPanel {
        id: libraryPanel
        z: 40
        onPickFolder: folderDialog.open()
        onOpenTool: (key) => root.go(key)
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("选择音乐文件夹")
        onAccepted: {
            library.addFolderUrls([selectedFolder])
            root.go("albums")
        }
    }

    // ======================= 拖放添加文件夹 =======================
    DropArea {
        id: drop
        anchors.fill: parent
        z: 60
        keys: ["text/uri-list"]
        enabled: root.view !== "tools"
        onDropped: (d) => {
            if (!d.hasUrls) return
            // 文件夹 = 加入音乐文件夹;普通音频 = 单独加入曲库;加密音频 = 转换后加入曲库
            const enc = [], other = []
            for (let i = 0; i < d.urls.length; ++i) {
                const u = String(d.urls[i])
                if (/\.(kgm|kgma|vpr|ncm|kgg)$/i.test(u)) enc.push(d.urls[i])
                else other.push(d.urls[i])
            }
            if (enc.length) decrypt.decryptUrls(enc)
            if (other.length) library.addFolderUrls(other)
            d.acceptProposedAction()
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 14
            radius: Theme.radiusL
            color: Theme.accentAlpha(0.07)
            border.width: 1.5
            border.color: Theme.accentAlpha(0.6)
            opacity: drop.containsDrag ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.fast } }
            Icon { anchors.centerIn: parent; name: "folder"; size: 44; opacity: 0.9 }
        }
    }

    // ======================= 键盘 / 鼠标侧键 =======================
    readonly property bool typing: activeFocusItem && activeFocusItem.hasOwnProperty("cursorPosition")
    Shortcut { sequence: "Ctrl+D"; onActivated: settings.desktopLyrics = !settings.desktopLyrics }
    Shortcut { sequence: "Ctrl+K"; onActivated: root.openSearch("") }
    Shortcut { sequence: "Ctrl+F"; onActivated: root.openSearch("") }
    Shortcut { sequence: "Space"; enabled: !root.typing && !trackMenu.shown; onActivated: player.toggle() }
    Shortcut { sequence: "Ctrl+Right"; onActivated: player.next() }
    Shortcut { sequence: "Ctrl+Left"; onActivated: player.previous() }
    Shortcut { sequence: "Alt+Left"; onActivated: root.back() }
    // 专辑墙翻页 / 到顶 / 到底
    readonly property bool wallKeys: root.view === "albums" && !root.typing && !trackMenu.shown && !nowPlaying.shown
                                     && !trackInfo.shown && !libraryPanel.shown && !coverViewer.shown
    Shortcut { sequence: "PgDown"; enabled: root.wallKeys; onActivated: wall.pageBy(1) }
    Shortcut { sequence: "PgUp"; enabled: root.wallKeys; onActivated: wall.pageBy(-1) }
    Shortcut { sequence: "Home"; enabled: root.wallKeys; onActivated: wall.scrollToTop() }
    Shortcut { sequence: "End"; enabled: root.wallKeys; onActivated: wall.scrollToEnd() }
    // 方向键:快进快退 5 秒;Ctrl+上下:音量 ±5%;键盘多媒体键 (窗口在前台时)
    Shortcut { sequence: "Right"; enabled: !root.typing && !trackMenu.shown; onActivated: player.seekBy(5) }
    Shortcut { sequence: "Left"; enabled: !root.typing && !trackMenu.shown; onActivated: player.seekBy(-5) }
    Shortcut { sequence: "Shift+Right"; enabled: !root.typing; onActivated: player.seekBy(30) }
    Shortcut { sequence: "Shift+Left"; enabled: !root.typing; onActivated: player.seekBy(-30) }
    // 收藏 / 静音 / 播放页 / 播放队列 / 歌曲信息
    Shortcut {
        sequence: "Ctrl+L"
        enabled: player.hasTrack
        onActivated: {
            playlists.toggleFavorite(player.trackId)
            Nav.toast("heart", playlists.isFavorite(player.trackId) ? qsTr("已收藏") : qsTr("已取消收藏"))
        }
    }
    Shortcut { sequence: "Ctrl+M"; onActivated: root.toggleMute() }
    Shortcut { sequence: "Ctrl+P"; enabled: player.hasTrack; onActivated: { trackMenu.close(); nowPlaying.shown = !nowPlaying.shown } }
    Shortcut { sequence: "Ctrl+Q"; enabled: !nowPlaying.shown; onActivated: { trackMenu.close(); queuePanel.shown = !queuePanel.shown } }
    Shortcut { sequence: "Ctrl+I"; enabled: player.hasTrack; onActivated: { trackMenu.close(); trackInfo.shown ? trackInfo.close() : trackInfo.open(player.trackId) } }
    Shortcut { sequence: "Ctrl+Up"; onActivated: player.changeVolume(0.05) }
    Shortcut { sequence: "Ctrl+Down"; onActivated: player.changeVolume(-0.05) }
    Shortcut { sequence: "Media Play"; onActivated: player.toggle() }
    Shortcut { sequence: "Toggle Media Play/Pause"; onActivated: player.toggle() }
    Shortcut { sequence: "Media Next"; onActivated: player.next() }
    Shortcut { sequence: "Media Previous"; onActivated: player.previous() }
    Shortcut { sequence: "Media Stop"; onActivated: if (player.playing) player.toggle() }
    Connections {
        target: player
        function onSleepFired() { Nav.toast("check", qsTr("定时关闭:已暂停播放")) }
    }
    Shortcut {
        sequence: "Escape"
        enabled: !root.typing
        onActivated: {
            if (trackMenu.shown) trackMenu.close()
            else if (playlistImport.shown) playlistImport.close()
            else if (coverViewer.shown) coverViewer.close()
            else if (trackInfo.shown) trackInfo.close()
            else if (libraryPanel.shown) libraryPanel.close()
            else if (decryptPrompt.shown) decryptPrompt.close()
            else if (chain.shown) chain.shown = false
            else if (queuePanel.shown) queuePanel.shown = false
            else if (nowPlaying.shown) nowPlaying.shown = false
            else root.back()
        }
    }
    MouseArea {
        anchors.fill: parent
        z: -1
        acceptedButtons: Qt.BackButton
        onClicked: root.back()
    }

    // 在任意位置直接打字即进入搜索
    Item {
        id: typeCatcher
        focus: !root.typing
        Keys.onPressed: (e) => {
            // 右键菜单开着:方向键 / 回车操作菜单
            if (trackMenu.shown) { if (trackMenu.keyNav(e)) e.accepted = true; return }
            // 沉浸页 / 歌曲信息 / 音乐文件夹面板盖住了页面:按键不再作用到下面看不见的页面
            if (nowPlaying.shown) {
                if (e.key === Qt.Key_Up || e.key === Qt.Key_Down) { player.changeVolume(e.key === Qt.Key_Up ? 0.05 : -0.05); e.accepted = true }
                return
            }
            if (trackInfo.shown || libraryPanel.shown || coverViewer.shown || playlistImport.shown) return
            if (e.modifiers & (Qt.ControlModifier | Qt.AltModifier)) return
            // 专辑详情:上下键选曲、回车播放
            // 专辑 / 歌曲 / 歌手 / 歌单的曲目列表:上下键选曲、回车播放
            const listPage = root.view === "album" ? detail : root.view === "songs" ? songsLoader.item
                           : root.view === "artist" ? artistLoader.item : root.view === "playlist" ? playlistLoader.item : null
            if (listPage && typeof listPage.keyNav === "function" && listPage.keyNav(e)) { e.accepted = true; return }
            if (e.text.length === 1 && /\S/.test(e.text)) {
                root.openSearch(e.text)
                e.accepted = true
            }
        }
    }

    // ======================= 无边框缩放手柄 =======================
    Repeater {
        model: [
            { e: Qt.LeftEdge, c: Qt.SizeHorCursor }, { e: Qt.RightEdge, c: Qt.SizeHorCursor },
            { e: Qt.TopEdge, c: Qt.SizeVerCursor }, { e: Qt.BottomEdge, c: Qt.SizeVerCursor },
            { e: Qt.LeftEdge | Qt.TopEdge, c: Qt.SizeFDiagCursor }, { e: Qt.RightEdge | Qt.BottomEdge, c: Qt.SizeFDiagCursor },
            { e: Qt.RightEdge | Qt.TopEdge, c: Qt.SizeBDiagCursor }, { e: Qt.LeftEdge | Qt.BottomEdge, c: Qt.SizeBDiagCursor }
        ]
        delegate: MouseArea {
            readonly property int edges: modelData.e
            readonly property int grip: 6
            z: 100
            enabled: root.visibility !== Window.Maximized
            cursorShape: modelData.c
            x: (edges & Qt.RightEdge) ? root.width - grip : 0
            y: (edges & Qt.BottomEdge) ? root.height - grip : 0
            width: (edges & (Qt.LeftEdge | Qt.RightEdge)) && !(edges & (Qt.TopEdge | Qt.BottomEdge)) ? grip
                   : ((edges & (Qt.TopEdge | Qt.BottomEdge)) && !(edges & (Qt.LeftEdge | Qt.RightEdge)) ? root.width : grip * 2)
            height: (edges & (Qt.TopEdge | Qt.BottomEdge)) && !(edges & (Qt.LeftEdge | Qt.RightEdge)) ? grip
                    : ((edges & (Qt.LeftEdge | Qt.RightEdge)) && !(edges & (Qt.TopEdge | Qt.BottomEdge)) ? root.height : grip * 2)
            onPressed: root.startSystemResize(edges)
        }
    }

    // ======================= 自动化 (离屏截图验证用) =======================
    function automationOpenAlbum(i) { root.openAlbum(Number(i), wall.coverOf(Number(i))) }
    function automationBack() { root.back() }
    function automationScrollEnd() { wall.scrollToEnd() }
    function automationSearch(q) { root.openSearch(String(q)) }
    function automationCloseSearch() { root.back() }
    function automationShowChain() { chain.shown = true }
    function automationNowPlaying(on) { nowPlaying.shown = !!on }
    function automationPlayTitle(t) {
        for (let i = 0; i < library.albumCount; ++i)
            if (library.albumInfo(i).title === String(t)) { player.playAlbum(i, 0); return }
    }
    function automationHideChain() { chain.shown = false }
    function automationOpenAlbumTitle(t) {
        for (let i = 0; i < library.albumCount; ++i)
            if (library.albumInfo(i).title === String(t)) { root.openAlbum(i, wall.coverOf(i)); return }
    }
    function automationDetailScroll(y) { detail.automationScroll(Number(y)) }
    function automationAlbumMenu() { trackMenu.open(-1, root.width - 420, 330, { albumIndex: albumTracks.albumIndex, fromDetail: true }) }
    function automationWallSort(m) { albumModel.sortMode = m }
    function automationWallFilter(f) { albumModel.filter = f }
    function automationWallScroll(y) { wall.automationScroll(y) }
    function automationWallSortMenu(on) { wall.automationSortMenu(on) }
    function automationCoverPanel(on) { wall.automationCoverPanel(on) }
    function automationCoverRun() { wall.automationCoverRun() }
    function automationCoverPick(i) { wall.automationCoverPick(i) }
    function automationCoverRunAll() { wall.automationCoverRunAll() }
    function automationCoverLog() { wall.automationCoverLog() }
    function automationAlbumCell(c) { settings.albumCell = c }
    function automationWallMenu(row) { Nav.trackMenu(-1, 520, 420, { albumIndex: albumModel.albumAt(Number(row)) }) }
    function automationViewCover(on) { if (on) coverViewer.open(albumTracks.cover, albumTracks.title, albumTracks.artist); else coverViewer.close() }
    function automationCloseMenu() { trackMenu.close() }
    function automationQueue(on) { queuePanel.shown = !!on }
    function automationQueueScroll(row) { queuePanel.automationScroll(row) }
    function automationQueueMenu(row) { queuePanel.automationMenu(row) }
    function automationClearQueue() { queuePanel.clearWithUndo() }
    function automationNowTab(t) { nowPlaying.automationTab(t) }
    function automationImport(on) { on ? playlistImport.open() : playlistImport.close() }
    function automationImportLink(t) { playlistImport.automationLink(String(t), false) }
    function automationImportGo(t) { playlistImport.automationLink(String(t), true) }
    function automationImportFilter(f) { playlistImport.filter = Number(f) }
    function automationImportSave() { playlistImport.save() }
    function automationImportScroll(y) { playlistImport.automationScroll(y) }
    function automationLyricPanel(on) { nowPlaying.automationLyricPanel(!!on) }
    function automationEqPanel(on) { nowPlaying.automationEqPanel(!!on) }
    function automationEqHover(i) { nowPlaying.automationEqHover(Number(i)) }
    function automationLyricPick(i) { nowPlaying.automationLyricPick(Number(i)) }
    function automationLyricHover(i) { nowPlaying.automationLyricHover(Number(i)) }
    function automationLyricSave() { nowPlaying.automationLyricSave() }
    function automationLyricOnlyTranslated(on) { nowPlaying.automationLyricOnlyTranslated(!!on) }
    function automationPlayTrackTitle(t) {
        for (let i = 0; i < library.albumCount; ++i) {
            const ids = library.albumTrackIds(i)
            for (let j = 0; j < ids.length; ++j)
                if (library.trackInfo(ids[j]).title === String(t)) { player.playTrack(ids[j]); return }
        }
    }
    function automationSleep(m) { player.setSleepTimer(Number(m)) }
    function automationSongsSort(m) { songsModel.sortMode = Number(m) }
    function automationDecryptPrompt(on) { on ? decryptPrompt.open() : decryptPrompt.close() }
    function automationLibraryPanel(on) { on ? libraryPanel.open() : libraryPanel.close() }
    function automationGo(sec) { root.go(String(sec)) }
    // 桌面歌词开关(截图验证用)
    function automationDesktopLyrics(on) { settings.desktopLyrics = !!on }
    // 封面墙:直接驱动画布(验证平移 / 缩放用,不依赖滚轮事件)
    function automationBurstPan(v) { if (burstLoader.item) burstLoader.item.automationPan(Number(v[0]), Number(v[1])) }
    function automationBurstZoom(z) { if (burstLoader.item) burstLoader.item.automationZoom(Number(z)) }
    function automationBurstOpen(row) { if (burstLoader.item) burstLoader.item.automationOpenSlot(Number(row)) }
    function automationOpenArtist(name) { root.openArtist(library.artistIndexOf(String(name))) }
    function automationOpenPlaylist(id) { root.openPlaylist(String(id)) }
    function automationTrackInfo(show) { trackMenu.close(); if (show) trackInfo.open(player.trackId); else trackInfo.close() }
    function automationTrackMenu(x, y) { trackMenu.open(player.trackId, Number(x), Number(y), {}) }

    // ======================= 帧诊断 (Ctrl+Shift+P 开关) =======================
    // 出帧节拍、掉帧、界面线程阻塞、显卡后端;开着时每秒一行写进日志,附上当时的状态 (见 frame_diag.h)
    Shortcut { sequence: "Ctrl+Shift+P"; onActivated: frameDiag.enabled = !frameDiag.enabled }
    Binding {
        target: frameDiag
        property: "state"
        when: frameDiag.enabled
        value: ({
            scrolling: Theme.scrolling,
            playing: player.playing,
            reduced: Theme.reduced,
            desktopLyrics: settings.desktopLyrics,
            nowPlaying: nowPlaying.shown,
            page: root.detailKind.length ? root.detailKind : root.section,
            bg: ["flow", "still", "solid"][settings.background] || "",
            appInactive: Qt.application.state !== Qt.ApplicationActive
        })
    }
    Rectangle {
        id: diag
        visible: frameDiag.enabled
        z: 10000
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 48
        anchors.rightMargin: 16
        width: 340
        height: diagCol.implicitHeight + 24
        radius: 12
        color: Qt.rgba(0, 0, 0, 0.78)
        readonly property var st: frameDiag.stats
        readonly property real frame: st.frame || 16.7
        Column {
            id: diagCol
            x: 14
            y: 12
            width: parent.width - 28
            spacing: 6
            Text {
                text: (diag.st.fps || 0) + " fps   中位 " + (diag.st.p50 || 0).toFixed(1) + "   最慢 " + Math.round(diag.st.max || 0)
                      + " ms   掉帧 " + (diag.st.janks || 0)
                color: "white"
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
            // 最近 120 帧的间隔:细线标出一帧的时长,超过 1.5 帧的柱子标强调色
            Item {
                width: parent.width
                height: 44
                Canvas {
                    id: bars
                    anchors.fill: parent
                    Connections { target: frameDiag; function onStatsChanged() { if (diag.visible) bars.requestPaint() } }
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        const v = frameDiag.recent, w = width / 120
                        for (let i = 0; i < v.length; ++i) {
                            const h = Math.min(height, v[i] / (diag.frame * 3) * height)
                            ctx.fillStyle = v[i] > diag.frame * 1.5 ? Theme.accent : Qt.rgba(1, 1, 1, 0.35)
                            ctx.fillRect(i * w, height - h, Math.max(1, w - 1), h)
                        }
                    }
                }
                Rectangle { y: 44 - 44 / 3; width: parent.width; height: 1; color: Qt.rgba(1, 1, 1, 0.25) }
            }
            Text {
                width: parent.width
                text: "屏幕 " + Math.round(diag.st.hz || 0) + " Hz"
                      + (Math.abs((diag.st.primaryHz || 0) - (diag.st.hz || 0)) > 1 ? " (主屏 " + Math.round(diag.st.primaryHz) + " Hz)" : "")
                      + "  ·  实测 " + (diag.st.p50 > 0 ? Math.round(1000 / diag.st.p50) : 0)
                      + " Hz  ·  录制 " + (diag.st.render || 0).toFixed(1) + " ms  ·  界面线程阻塞 " + Math.round(diag.st.stall || 0) + " ms"
                      + "\n界面线程动画驱动 " + (diag.st.timerMode ? "计时器模式" : "垂直同步") + " (切换 " + (diag.st.switches || 0) + " 次)"
                      + "\n渲染线程动画驱动 " + (diag.st.rtTimerMode ? "计时器模式" : "垂直同步") + " (切换 " + (diag.st.rtSwitches || 0) + " 次)"
                color: Qt.rgba(1, 1, 1, 0.7)
                font.family: Theme.fontFamily
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                text: frameDiag.graphics
                color: Qt.rgba(1, 1, 1, 0.5)
                font.family: Theme.fontFamily
                font.pixelSize: 11
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 2
                elide: Text.ElideRight
            }
            // 启动参数:点选后重启生效,用来对比哪种组合不卡
            Row {
                spacing: 10
                Text { text: "图形接口"; color: Qt.rgba(1, 1, 1, 0.5); font.family: Theme.fontFamily; font.pixelSize: 11 }
                Repeater {
                    model: [{ t: frameDiag.defaultRhi.length ? "默认 (" + frameDiag.defaultRhi.toUpperCase() + ")" : "默认", v: "" }, { t: "D3D11", v: "d3d11" }, { t: "D3D12", v: "d3d12" }, { t: "OpenGL", v: "opengl" }, { t: "Vulkan", v: "vulkan" }]
                    Text {
                        text: modelData.t
                        color: frameDiag.rhiChoice === modelData.v ? Theme.accent : Qt.rgba(1, 1, 1, 0.7)
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.weight: frameDiag.rhiChoice === modelData.v ? Font.DemiBold : Font.Normal
                        MouseArea { anchors.fill: parent; anchors.margins: -4; cursorShape: Qt.PointingHandCursor; onClicked: frameDiag.rhiChoice = modelData.v }
                    }
                }
            }
            Row {
                spacing: 10
                Text { text: "渲染循环"; color: Qt.rgba(1, 1, 1, 0.5); font.family: Theme.fontFamily; font.pixelSize: 11 }
                Repeater {
                    model: [{ t: "默认", v: "" }, { t: "threaded", v: "threaded" }, { t: "basic", v: "basic" }]
                    Text {
                        text: modelData.t
                        color: frameDiag.renderLoop === modelData.v ? Theme.accent : Qt.rgba(1, 1, 1, 0.7)
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.weight: frameDiag.renderLoop === modelData.v ? Font.DemiBold : Font.Normal
                        MouseArea { anchors.fill: parent; anchors.margins: -4; cursorShape: Qt.PointingHandCursor; onClicked: frameDiag.renderLoop = modelData.v }
                    }
                }
            }
            Row {
                spacing: 10
                Text { text: "动画驱动"; color: Qt.rgba(1, 1, 1, 0.5); font.family: Theme.fontFamily; font.pixelSize: 11 }
                Repeater {
                    model: [{ t: "默认", v: false }, { t: "简单 (只按挂钟)", v: true }]
                    Text {
                        text: modelData.t
                        color: frameDiag.simpleDriver === modelData.v ? Theme.accent : Qt.rgba(1, 1, 1, 0.7)
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.weight: frameDiag.simpleDriver === modelData.v ? Font.DemiBold : Font.Normal
                        MouseArea { anchors.fill: parent; anchors.margins: -4; cursorShape: Qt.PointingHandCursor; onClicked: frameDiag.simpleDriver = modelData.v }
                    }
                }
                Text { text: "· 重启后生效"; color: Qt.rgba(1, 1, 1, 0.4); font.family: Theme.fontFamily; font.pixelSize: 11 }
            }
            Text {
                width: parent.width
                text: "日志 " + frameDiag.logPath
                color: Qt.rgba(1, 1, 1, 0.4)
                font.family: Theme.fontFamily
                font.pixelSize: 10
                wrapMode: Text.WrapAnywhere
            }
        }
    }

    // 桌面歌词:单独的置顶透明窗口,开启时才创建
    Loader {
        active: settings.desktopLyrics
        sourceComponent: DesktopLyrics { visible: true }
    }
}
