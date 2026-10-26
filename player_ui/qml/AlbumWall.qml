import QtQuick
import PlayerUi

// 专辑墙:列数随宽度自适应,卡片宽度精确铺满;代理复用,缓冲区预加载封面
// 顶部横幅带筛选 / 排序工具栏;右侧字母索引;曲库重扫后停在原来那张专辑处;Ctrl + 滚轮调封面大小
Item {
    id: wall
    // 内容滚到窗口按钮下面了:Main 据此在顶端铺一条磨砂条
    readonly property bool scrolled: grid.contentY - grid.originY > 8
    signal openAlbum(int index, Item coverItem)
    property int bottomInset: 140

    readonly property int gap: 28
    // 网格从窗口按钮下方开始:滚动的封面和标题不会压在最小化 / 关闭按钮下面
    readonly property int topClip: 40
    // 与详情页同一套页边距;超宽屏仍让网格多用一些宽度
    readonly property int sidePadding: Math.max(width > 1400 ? 64 : 44, Math.round((width - 2200) / 2))
    readonly property int minCell: settings.albumCell
    readonly property int columns: Math.max(2, Math.floor((width - 2 * sidePadding + gap) / (minCell + gap)))
    readonly property real cellW: (width - 2 * sidePadding + gap) / columns

    // 曲库首次出现、换排序 / 筛选、回到专辑墙时,新建的卡片依次浮现;重扫和之后滚动复用的卡片不再播放
    property bool entering: false
    Timer { id: enterWindow; interval: 900; onTriggered: wall.entering = false }
    function startEntering() {
        if (Theme.reduced) return
        entering = true
        enterWindow.restart()
    }
    onVisibleChanged: {
        if (!visible) return
        albumModel.refreshRecent()
        if (grid.count > 0 && !grid.userScrolled) startEntering()
    }

    // ---- 模型重置 (重扫 / 换排序 / 换筛选) 前后保持位置 ----
    // 重扫:回到原来顶上那张专辑 (按专辑键找,下标每次扫描都会变);换排序 / 筛选:位置不动 (工具栏留在原处)
    property int lastSort: albumModel.sortMode
    property int lastFilter: albumModel.filter
    property string anchorKey
    property real anchorDy: 0
    property real savedY: 0
    property real headRel: 0
    Connections {
        target: albumModel
        function onModelAboutToBeReset() {
            const viewChange = albumModel.sortMode !== wall.lastSort || albumModel.filter !== wall.lastFilter
            wall.lastSort = albumModel.sortMode
            wall.lastFilter = albumModel.filter
            if (grid.count === 0 || viewChange) wall.startEntering()
            wall.savedY = grid.contentY
            wall.anchorKey = ""
            const head = grid.headerItem ? grid.headerItem.height : 0
            if (viewChange || grid.contentY - grid.originY < head) return
            const i = grid.indexAt(grid.cellWidth / 2, grid.contentY + grid.cellHeight / 2)
            const it = i >= 0 ? grid.itemAtIndex(i) : null
            if (!it) return
            wall.anchorKey = it.albumKey
            wall.anchorDy = grid.contentY - it.y
        }
        function onModelReset() {
            smooth.stop()
            Qt.callLater(wall.restorePosition)
        }
    }
    function clampY(y) {
        return Math.max(grid.originY, Math.min(y, grid.originY + grid.contentHeight - grid.height + grid.bottomMargin))
    }
    function restorePosition() {
        if (!grid.userScrolled) { grid.positionViewAtBeginning(); return }
        const r = albumModel.rowOfKey(anchorKey)
        if (r >= 0) {
            grid.positionViewAtIndex(r, GridView.Beginning)
            const it = grid.itemAtIndex(r)
            if (it) { grid.contentY = clampY(it.y + anchorDy); return }
        }
        grid.contentY = clampY(savedY)
    }

    // ---- 跳转 ----
    function scrollToTop() { scrollToY(grid.originY) }
    function scrollToEnd() { scrollToY(grid.originY + grid.contentHeight - grid.height + grid.bottomMargin) }
    function pageBy(dir) { scrollToY(smooth.destination() + dir * Math.max(200, grid.height - wall.bottomInset - 80)) }
    // 远距离先瞬间跳到目标附近再滑过去:不必把途经的几百张封面都加载一遍
    function scrollToY(y) {
        const goal = clampY(y)
        const far = grid.height * 6
        if (Math.abs(goal - grid.contentY) > far) {
            smooth.stop()
            grid.contentY = goal + (goal > grid.contentY ? -1 : 1) * grid.height * 1.5
        }
        smooth.scrollTo(goal)
        grid.userScrolled = true
    }

    // 封面在网格视口里露出了多少 (0..1);完全滚出视野的不当作飞行落点
    function shownPart(item) {
        const p = item.mapToItem(grid, 0, 0)
        const top = Math.max(0, p.y), bottom = Math.min(grid.height, p.y + item.height)
        return item.height > 0 ? Math.max(0, bottom - top) / item.height : 0
    }
    // 关闭专辑时封面飞回的位置:优先网格里露出来的那张卡片,其次是顶部横幅;都不在视野内就不飞
    // (横幅显示的是刚打开过的专辑,滚下去之后它已在视野之外,飞向它看起来像往上跳走)
    function coverOf(index, preferred) {
        // 优先飞回打开时点的那一张 (它所在的格子 / 横幅仍是这张专辑,且露在视野里)
        if (preferred && shownPart(preferred) > 0.3) {
            let p = preferred
            while (p && p.albumIndex === undefined) p = p.parent
            if (p && p.albumIndex === index) return preferred
        }
        // 专辑详情盖在上面时专辑墙 visible 为 false,不能用 visible 判断;复用池里的旧卡片 itemAtIndex 取不到
        for (let i = 0; i < grid.contentItem.children.length; ++i) {
            const c = grid.contentItem.children[i]
            if (c.albumIndex === index && c.coverItem && grid.itemAtIndex(c.row) === c && shownPart(c.coverItem) > 0.3)
                return c.coverItem
        }
        const h = grid.headerItem
        if (h && h.albumIndex === index && h.coverItem && shownPart(h.coverItem) > 0.3) return h.coverItem
        return null
    }

    Component {
        id: riseAnim
        SequentialAnimation {
            id: rise
            property Item cell
            property Translate lift
            property int delay
            ScriptAction { script: { rise.cell.opacity = 0; rise.lift.y = 22 } }
            PauseAnimation { duration: rise.delay }
            ParallelAnimation {
                OpacityAnimator { target: rise.cell; to: 1; duration: 420; easing.type: Easing.OutCubic }
                NumberAnimation { target: rise.lift; property: "y"; to: 0; duration: 560; easing.type: Easing.OutQuint }
            }
        }
    }

    GridView {
        id: grid
        anchors.fill: parent
        anchors.topMargin: wall.topClip
        anchors.leftMargin: wall.sidePadding - wall.gap / 2
        anchors.rightMargin: wall.sidePadding - wall.gap / 2
        clip: true
        model: albumModel
        cellWidth: wall.cellW
        cellHeight: wall.cellW - wall.gap + 84
        cacheBuffer: 600
        property bool userScrolled: false
        onMovementStarted: userScrolled = true
        SmoothScroll {
            id: smooth
            flickable: grid
            acceptedModifiers: Qt.NoModifier
            onScrolled: grid.userScrolled = true
        }
        // Ctrl + 滚轮:封面变大 / 变小 (记住设置),顶上那张专辑保持在原处
        property real zoomAcc: 0
        WheelHandler {
            target: null
            acceptedModifiers: Qt.ControlModifier
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (e) => {
                grid.zoomAcc += e.angleDelta.y
                if (Math.abs(grid.zoomAcc) < 120) return
                const dir = grid.zoomAcc > 0 ? 1 : -1
                grid.zoomAcc = 0
                const head = grid.headerItem ? grid.headerItem.height : 0
                const top = grid.contentY - grid.originY < head ? -1 : grid.indexAt(grid.cellWidth / 2, grid.contentY + grid.cellHeight / 2)
                settings.albumCell = settings.albumCell + dir * 24
                if (top >= 0) Qt.callLater(() => grid.positionViewAtIndex(top, GridView.Beginning))
            }
        }
        reuseItems: true
        boundsBehavior: Flickable.StopAtBounds
        maximumFlickVelocity: 5200
        flickDeceleration: 2600
        topMargin: 0
        bottomMargin: wall.bottomInset

        header: Item {
            width: grid.width
            height: heroItem.height
            readonly property int albumIndex: heroItem.albumIndex
            readonly property Item coverItem: heroItem.coverItem
            readonly property Item sortAnchor: heroItem.sortAnchor
            HeroBanner {
                id: heroItem
                x: wall.gap / 2
                width: parent.width - wall.gap
                availableHeight: grid.height - wall.bottomInset
                sortOpen: sortPop.shown
                onOpenAlbum: (index, coverItem) => wall.openAlbum(index, coverItem)
                onSortMenu: (anchor) => sortPop.open(anchor)
                onCoverPanel: (anchor) => coverSheet.openAt(anchor)
            }
            // 横幅高度变化 (扫描完成、窗口缩放):用户尚未滚动时停在顶部;已滚动但视口还在横幅范围内时
            // 保持视口相对横幅的位置 (GridView 有时自己挪 contentY、有时不挪,所以按记下的相对位置重设)
            property real lastH: 0
            onHeightChanged: {
                const old = lastH
                lastH = height
                if (!grid.userScrolled) Qt.callLater(grid.positionViewAtBeginning)
                else if (wall.headRel < old) grid.contentY = grid.originY + Math.min(wall.headRel, height)
            }
        }

        // 筛选后一张都没有时的提示
        footer: Item {
            width: grid.width
            height: albumModel.count === 0 && library.albumCount > 0 ? 160 : 0
            visible: height > 0
            Column {
                anchors.centerIn: parent
                spacing: 14
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("没有符合条件的专辑")
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                }
                Capsule {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("显示全部")
                    onClicked: albumModel.filter = 0
                }
            }
        }

        delegate: Item {
            id: cell
            width: grid.cellWidth
            height: grid.cellHeight
            readonly property int row: index
            readonly property int albumIndex: model.albumIndex
            readonly property string albumKey: model.albumKey
            readonly property string letter: model.letter
            readonly property Item coverItem: cardItem.coverItem
            // 入场浮起只在刚进入专辑墙时需要:按需创建动画,滚动时新建的格子不带这一套
            property var rising: null
            Component.onCompleted: if (wall.entering) {
                rising = riseAnim.createObject(cell, { cell: cell, lift: lift,
                    delay: Math.max(0, Math.min(420, (index % wall.columns) * 45 + Math.floor(index / wall.columns) * 70)) })
                rising.start()
            }
            // Qt 6.9 起 GridView 真正复用格子:收进复用池时把没放完的入场动画收尾,复用出来的格子直接是停稳的样子
            GridView.onPooled: {
                if (rising) { rising.stop(); rising.destroy(); rising = null }
                cell.opacity = 1
                lift.y = 0
            }
            GridView.onReused: cardItem.settle()
            transform: Translate { id: lift }
            AlbumCard {
                id: cardItem
                x: wall.gap / 2
                width: cell.width - wall.gap
                height: cell.height
                title: model.title
                artist: model.artist
                detail: albumModel.sortMode === 1 || albumModel.sortMode === 2 ? model.year : ""
                badge: model.badge
                cover: model.cover
                albumIndex: model.albumIndex
                onOpen: wall.openAlbum(model.albumIndex, cardItem.coverItem)
                onPlay: {
                    player.shuffle = false
                    Nav.picked(player.pickAlbum(model.albumIndex, 0, true))
                }
            }
        }

        // 视口顶端相对横幅顶端的位置;横幅正在变高变矮时 (GridView 内部调整) 不记
        onContentYChanged: {
            syncLetter.restart()
            if (headerItem && headerItem.height === headerItem.lastH) wall.headRel = contentY - originY
        }
        Timer {
            id: syncLetter
            interval: 60
            onTriggered: {
                const i = grid.indexAt(grid.cellWidth / 2, grid.contentY + grid.cellHeight / 2)
                const it = i >= 0 ? grid.itemAtIndex(i) : null
                if (it) rail.current = it.letter
                else if (albumModel.letters.length) rail.current = albumModel.letters[0]
            }
        }
    }

    // 字母索引 (按年份排序时是年代),放在右侧页边里,不占卡片宽度
    LetterRail {
        id: rail
        anchors.right: parent.right
        anchors.rightMargin: Math.max(4, Math.round((wall.sidePadding - width) / 2))
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: (wall.topClip - wall.bottomInset) / 2
        width: albumModel.sortMode === 2 ? 30 : 22
        letters: albumModel.letters
        onJump: (l) => {
            const i = albumModel.indexOfLetter(l)
            if (i < 0) return
            smooth.stop()
            grid.positionViewAtIndex(i, GridView.Beginning)
            grid.userScrolled = true
        }
    }

    // 回到顶部:滚出两屏以上时出现在右下角
    Rectangle {
        id: topBtn
        readonly property bool shown: grid.contentY - grid.originY > grid.height * 2
        anchors.right: parent.right
        anchors.rightMargin: wall.sidePadding
        anchors.bottom: parent.bottom
        anchors.bottomMargin: wall.bottomInset + 12
        width: topRow.implicitWidth + 28
        height: 36
        radius: 18
        color: Theme.panel
        border.width: 1
        border.color: Qt.rgba(1, 0.96, 0.9, 0.09)
        opacity: shown ? 1 : 0
        scale: shown ? (topMouse.pressed ? 0.96 : 1) : 0.9
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Theme.normal } }
        Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
        Row {
            id: topRow
            anchors.centerIn: parent
            spacing: 6
            Icon { anchors.verticalCenter: parent.verticalCenter; name: "chevron"; size: 14; rotation: -90; opacity: topMouse.containsMouse ? 1 : 0.7 }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("回到顶部")
                color: topMouse.containsMouse ? Theme.text : Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 13
            }
        }
        MouseArea {
            id: topMouse
            anchors.fill: parent
            enabled: topBtn.shown
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: wall.scrollToTop()
        }
    }

    // ---- 排序菜单 ----
    SortPopup {
        id: sortPop
        options: [qsTr("按标题"), qsTr("按歌手"), qsTr("按年份"), qsTr("最近添加"), qsTr("最近播放")]
        current: albumModel.sortMode
        onPicked: (i) => albumModel.sortMode = i
    }

    // ---- 专辑封面面板 ----
    CoverPanel {
        id: coverSheet
        bottomInset: wall.bottomInset + 8
    }

    // 截图自动化
    function automationScroll(y) { smooth.stop(); grid.userScrolled = true; grid.contentY = clampY(grid.originY + y) }
    function automationSortMenu(on) {
        if (!on) { sortPop.shown = false; return }
        const h = grid.headerItem
        if (h) sortPop.open(h.sortAnchor)
    }
    // 封面面板:on = 打开 / 关;run = 点「导入」
    function automationCoverPanel(on) { if (on) coverSheet.openAt(null); else coverSheet.close() }
    function automationCoverRun() { coverSheet.run() }
    function automationCoverPick(i) { coverSheet.toggle(Number(i), 1) }
    function automationCoverRunAll() { coverSheet.runAll() }
    function automationCoverLog() { covers.openFailureLog() }
}
