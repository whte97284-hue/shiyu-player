import QtQuick
import PlayerUi

// 专辑封面面板 —— 从专辑墙横幅的「封面」按钮弹出
//
// 为什么按「专辑」补:曲库里没有「专辑封面」这个独立实体。
// 专辑封面借用该专辑里第一首有封面的曲目 (library_service.cpp),一首都没有就用第一首占位。
// 所以一张专辑常常只有一两首带图 —— 专辑墙看着有封面,其余曲目拷到别的播放器就没图了。
// 这里一次把整张专辑的每一首都补齐。
//
// 无字化:只用明暗 / 边框 / 勾号表达状态,面板里不出现任何文字。
Item {
    id: root
    property bool shown: false
    // 弹出锚点:面板右上角对齐这个点 (root 坐标系)
    property real anchorRight: width - 40
    property real anchorBottom: height - 40
    anchors.fill: parent
    visible: pop.opacity > 0
    z: 6

    property var albums: []
    property var picked: ({})   // albumIndex -> true
    // 底部播放栏高度:面板不能压到它下面,否则底部按钮点不到
    property int bottomInset: 140
    readonly property bool busy: covers.busy
    readonly property int selCount: {
        let n = 0
        for (const k in picked) if (picked[k]) n++
        return n
    }

    function reload() {
        const next = covers.albumStates()
        // 内容没变就别换引用:albums 一换,GridView 会把整个 delegate 池重建,
        // 每个新格子都从 opacity 0 重播入场动画 —— 频繁调用时会一直看不到格子。
        if (JSON.stringify(next) === JSON.stringify(albums)) return
        albums = next
    }
    function openAt(item) {
        if (item) {
            const p = item.mapToItem(root, 0, item.height)
            anchorRight = p.x + item.width
            anchorBottom = p.y + 12
        }
        reload()
        picked = ({})
        shown = true
        covers.checkStale()   // 后台核对"快照说没封面但磁盘已有",有则提示重建
    }
    function close() { shown = false }
    function toggle(i, missing) {
        if (missing <= 0 || busy) return
        const p = Object.assign({}, picked)
        if (p[i]) delete p[i]
        else p[i] = true
        picked = p
    }
    function run() {
        if (busy) { covers.cancel(); return }
        const ids = []
        for (const k in picked) if (picked[k]) ids.push(Number(k))
        if (ids.length > 0) covers.fetchAlbums(ids)
        else covers.fetchAlbums([])   // 空 = 全部
    }
    function runAll() { if (busy) covers.cancel(); else covers.fetchAlbums([]) }

    // 补完封面会重扫曲库:版本号变了就重新取一次状态
    // 但补封面时每写一首都会触发一次重扫,运行中刷新会让格子反复重建(面板看起来是黑的),
    // 所以运行期间不刷新,补完由下面的 onFinished 统一刷。
    Connections {
        target: library
        function onLibraryChanged() { if (root.shown && !root.busy) root.reload() }
    }
    Connections {
        target: covers
        function onFinished(ok, failed) { root.reload() }
    }

    // 点面板外收起
    MouseArea {
        anchors.fill: parent
        enabled: root.shown
        acceptedButtons: Qt.AllButtons
        onPressed: root.close()
        onWheel: (w) => w.accepted = true
    }

    // 面板连同投影一起从按钮处滑出:向下平移 + 轻微放大 + 淡入
    Item {
        id: pop
        readonly property int pad: 18
        // 跑任务时头部要放"当前专辑 + 计数 + 剩余时间",多占 36px
        readonly property int headH: root.busy ? 84 : 46
        readonly property int footH: 58
        readonly property int cols: 4
        readonly property int gap: 10
        width: Math.min(520, root.width - 40)
        readonly property real cell: (width - pad * 2 - gap * (cols - 1)) / cols
        readonly property int rows: Math.max(1, Math.ceil(root.albums.length / cols))
        // 高度跟着行数走:专辑少时不留大片空白,多了才滚动
        readonly property real contentH: headH + rows * cell + (rows - 1) * gap + footH + 8
        // 安全区:顶部留 20,底部让开播放栏 —— 保证底部按钮始终可点
        readonly property int safeTop: 20
        readonly property real safeBottom: Math.max(safeTop + 240, root.height - root.bottomInset - 8)
        readonly property real maxH: safeBottom - safeTop
        height: Math.min(contentH, maxH)
        x: Math.max(20, Math.min(root.width - width - 20, root.anchorRight - width))
        y: Math.max(pop.safeTop, Math.min(pop.safeBottom - height, root.anchorBottom))
        transformOrigin: Item.TopRight
        opacity: root.shown ? 1 : 0
        scale: root.shown ? 1 : 0.97
        Behavior on opacity { OpacityAnimator { duration: root.shown ? 240 : 140; easing.type: Easing.OutCubic } }
        Behavior on scale { ScaleAnimator { duration: root.shown ? 480 : 160; easing.type: root.shown ? Easing.OutQuint : Easing.InCubic } }
        transform: Translate {
            y: root.shown ? 0 : -14
            Behavior on y { NumberAnimation { duration: root.shown ? 520 : 160; easing.type: root.shown ? Easing.OutQuint : Easing.InCubic } }
        }
        SoftShadow { target: card; strength: 0.85; spread: 30; offsetY: 16 }

        Rectangle {
            id: card
            anchors.fill: parent
            radius: Theme.radiusL
            color: Theme.panel
            border.width: 1
            border.color: Qt.rgba(1, 0.96, 0.9, 0.08)
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }
            Keys.onEscapePressed: root.close()

            // ---- 标题 ----
            Icon {
                id: heading
                x: pop.pad
                y: 16
                name: "album-open"
                size: 18
                opacity: 0.9
            }

            // ---- 运行中的进度信息:当前专辑 / 计数 / 剩余时间 / 并发数 ----
            Item {
                id: progressInfo
                x: pop.pad
                y: 44
                width: card.width - pop.pad * 2
                height: 34
                // 收起时整体上移淡出,不占位
                opacity: root.busy ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                transform: Translate {
                    y: root.busy ? 0 : -6
                    Behavior on y { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                }

                // 当前处理到哪张(长标题走马灯式省略)
                Text {
                    id: curTitle
                    anchors.left: parent.left
                    anchors.right: counter.left
                    anchors.rightMargin: 10
                    anchors.top: parent.top
                    height: 16
                    text: covers.currentTitle
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    elide: Text.ElideMiddle
                    opacity: 0.85
                    // 换标题时轻微闪一下,提示"在动"
                    onTextChanged: if (text.length) titlePulse.restart()
                    SequentialAnimation {
                        id: titlePulse
                        NumberAnimation { target: curTitle; property: "opacity"; to: 0.3; duration: 80 }
                        NumberAnimation { target: curTitle; property: "opacity"; to: 0.85; duration: 220; easing.type: Easing.OutCubic }
                    }
                }

                // 右侧计数:12/48 · 剩 15s · 4 线程
                Text {
                    id: counter
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 16
                    text: {
                        const d = covers.done, t = covers.total
                        let s = d + " / " + t
                        const eta = covers.etaSec
                        if (eta >= 0) s += "  ·  " + (eta >= 60 ? Math.ceil(eta / 60) + "分" : eta + "秒")
                        if (covers.threadCount > 1) s += "  ·  " + covers.threadCount + "线程"
                        return s
                    }
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    opacity: 0.5
                }

                // 细进度条
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 3
                    radius: 1.5
                    color: Theme.accentAlpha(0.14)

                    Rectangle {
                        id: progFill
                        height: parent.height
                        radius: 1.5
                        color: Theme.accent
                        width: parent.width * Math.max(0, Math.min(1, covers.progress))
                        // 进度推进要顺滑,但不能拖太久(否则看起来比实际慢)
                        Behavior on width { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }

                        // 流光:一道浅色高光在填充区里循环扫过
                        Rectangle {
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 46
                            radius: 1.5
                            visible: root.busy && progFill.width > 60
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 0.0) }
                                GradientStop { position: 0.5; color: Qt.rgba(1, 1, 1, 0.38) }
                                GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 0.0) }
                            }
                            SequentialAnimation on x {
                                running: root.busy && progFill.width > 60
                                loops: Animation.Infinite
                                NumberAnimation { from: -46; to: progFill.width; duration: 1100; easing.type: Easing.InOutQuad }
                                PauseAnimation { duration: 260 }
                            }
                        }
                    }
                }
            }
            IconButton {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.top: parent.top
                anchors.topMargin: 8
                width: 32; height: 32
                icon: "close"
                iconSize: 16
                onClicked: root.close()
            }

            // ---- 专辑网格 ----
            GridView {
                id: grid
                x: pop.pad
                y: pop.headH
                width: card.width - pop.pad * 2
                height: Math.min(card.height - pop.headH - pop.footH,
                                 pop.rows * pop.cell + (pop.rows - 1) * pop.gap)
                clip: true
                cellWidth: pop.cell
                cellHeight: pop.cell
                model: root.albums
                boundsBehavior: Flickable.StopAtBounds
                // 滚动时不再重复创建格子(复用已有 delegate),滚动更顺
                reuseItems: true

                delegate: Item {
                    id: cellItem
                    width: grid.cellWidth
                    height: grid.cellHeight
                    // 入场:按位置错开淡入(前 16 个错开 22ms,再往后的同批,避免列表长时等太久)
                    // 初值给 1:reuseItems 复用 delegate 时不会再触发 onCompleted,不能留 0
                    opacity: 1
                    Component.onCompleted: { cellItem.opacity = 0; enterAnim.start() }
                    SequentialAnimation {
                        id: enterAnim
                        PauseAnimation { duration: Math.min(index, 16) * 22 }
                        NumberAnimation { target: cellItem; property: "opacity"; to: 1; duration: 240; easing.type: Easing.OutCubic }
                    }
                    readonly property int albumIndex: modelData.index
                    readonly property int missing: modelData.missing
                    readonly property bool on: !!root.picked[albumIndex]
                    readonly property bool dead: modelData.fixable === 0   // 这张补不了(缺的是 WAV/DSF 等)

                    RoundedImage {
                        anchors.fill: parent
                        anchors.margins: 3
                        source: modelData.cover
                        radius: Theme.radiusS
                        decodeSize: 128          // 格子只有 ~110px,解大了白费
                        fadeIn: false
                        opacity: cellItem.dead ? 0.3 : 1
                    }

                    // 缺封面的圈一圈,悬停变亮;选中换成强调色
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 3
                        radius: Theme.radiusS
                        color: "transparent"
                        border.width: cellItem.on ? 2 : 1
                        border.color: cellItem.on ? Theme.accent
                                    : cellItem.dead ? Qt.rgba(0.6, 0.6, 0.6, 0.3)
                                    : (hover.hovered ? Qt.rgba(1, 1, 1, 0.55) : Qt.rgba(1, 1, 1, 0.22))
                        Behavior on border.color { ColorAnimation { duration: Theme.fast } }
                    }

                    // 缺几首:右下角小点(无字提示,1 个点 = 全部缺,2 个 = 部分)
                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: 9
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 9
                        spacing: 3
                        Repeater {
                            model: cellItem.dead ? 0 : (cellItem.missing >= 2 ? 2 : 1)
                            Rectangle { width: 5; height: 5; radius: 2.5; color: Theme.accent }
                        }
                        // 补不了的:一根灰横杠
                        Rectangle {
                            visible: cellItem.dead
                            width: 10; height: 2; radius: 1
                            color: Qt.rgba(1, 1, 1, 0.35)
                        }
                    }

                    // 选中:右上角勾
                    Rectangle {
                        width: 20; height: 20; radius: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.top: parent.top
                        anchors.topMargin: 8
                        color: Theme.accent
                        visible: cellItem.on
                        scale: cellItem.on ? 1 : 0
                        Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutBack } }
                        Icon {
                            anchors.centerIn: parent
                            name: "check"
                            size: 12
                            tint: Theme.onAccent
                        }
                    }

                    HoverHandler { id: hover; enabled: !root.busy && !cellItem.dead }
                    // 悬停轻微放大,给一点"能点"的反馈
                    scale: (hover.hovered && !cellItem.on) ? 1.035 : 1
                    Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                    MouseArea {
                        anchors.fill: parent
                        enabled: !root.busy && !cellItem.dead
                        hoverEnabled: true
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: root.toggle(cellItem.albumIndex, cellItem.missing)
                    }
                }
            }

            // ---- 滚动条:细条,可拖动;只在内容超出时出现 ----
            // 用单个 MouseArea 处理「拖滑块」和「点轨道翻页」:
            // 拆成两个热区时,滑块一移动热区也跟着跑,抓取点会漂 —— 用绝对坐标最稳。
            Item {
                id: scrollTrack
                readonly property bool needed: grid.contentHeight > grid.height + 1
                anchors.left: grid.right
                anchors.leftMargin: 4
                y: grid.y
                width: 8
                height: grid.height
                visible: needed
                opacity: needed ? (sbArea.hovered || sbArea.dragging ? 1 : 0.5) : 0
                Behavior on opacity { NumberAnimation { duration: Theme.fast } }

                Rectangle {
                    anchors.centerIn: parent
                    width: 3
                    height: parent.height
                    radius: 1.5
                    color: Qt.rgba(1, 1, 1, 0.06)
                }

                Rectangle {
                    id: thumb
                    readonly property real trackH: scrollTrack.height
                    readonly property real range: Math.max(1, grid.contentHeight - grid.height)
                    readonly property real thumbH: Math.max(28, trackH * grid.height / Math.max(1, grid.contentHeight))
                    readonly property real maxY: Math.max(0, trackH - thumbH)
                    readonly property real ratio: Math.max(0, Math.min(1, (grid.contentY - grid.originY) / range))
                    width: sbArea.hovered || sbArea.dragging ? 5 : 3
                    height: thumbH
                    radius: width / 2
                    x: (parent.width - width) / 2
                    y: ratio * maxY
                    color: sbArea.dragging ? Qt.rgba(1, 1, 1, 0.7)
                         : sbArea.hovered ? Qt.rgba(1, 1, 1, 0.5) : Qt.rgba(1, 1, 1, 0.28)
                    Behavior on color { ColorAnimation { duration: Theme.fast } }
                    Behavior on width { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
                    // 拖动时 y 由 contentY 直接驱动,不能再插值(否则手感发飘)
                    Behavior on y {
                        enabled: !sbArea.dragging
                        NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
                    }
                }

                MouseArea {
                    id: sbArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    property bool dragging: false
                    property real grabDy: 0

                    onPressed: (m) => {
                        if (m.y >= thumb.y && m.y <= thumb.y + thumb.height) {
                            dragging = true
                            grabDy = m.y - thumb.y
                        } else {
                            // 点轨道空白:朝那一侧翻一屏
                            const dir = m.y < thumb.y ? -1 : 1
                            const step = grid.height * 0.85 * dir
                            grid.contentY = Math.max(grid.originY,
                                Math.min(grid.originY + grid.contentHeight - grid.height,
                                         grid.contentY + step))
                        }
                    }
                    onPositionChanged: (m) => {
                        if (!dragging || thumb.maxY <= 0) return
                        const ty = Math.max(0, Math.min(thumb.maxY, m.y - grabDy))
                        grid.contentY = grid.originY + (ty / thumb.maxY) * thumb.range
                    }
                    onReleased: dragging = false
                    onCanceled: dragging = false
                }
            }

            // 没有缺封面的专辑:居中一个勾,不打扰
            Icon {
                anchors.centerIn: grid
                name: "check"
                size: 34
                opacity: root.albums.length === 0 ? 0.18 : 0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: Theme.normal } }
            }

            // ---- 底部条:固定在卡片底边,永远可见可点 ----
            Rectangle {
                id: foot
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: pop.footH
                color: "transparent"
                // 上方一条分隔线,让底部条和网格分开
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 1
                    color: Theme.hairline
                    visible: grid.height > 0
                }

                Capsule {
                    id: errCap
                    anchors.left: parent.left
                    anchors.leftMargin: pop.pad
                    anchors.verticalCenter: parent.verticalCenter
                    visible: (covers.failures.length > 0 || covers.skips.length > 0) && !root.busy
                    text: covers.failures.length > 0
                          ? qsTr("失败 %1 · 跳过 %2").arg(covers.failures.length).arg(covers.skips.length)
                          : qsTr("跳过 %1").arg(covers.skips.length)
                    onClicked: covers.openFailureLog()
                }

                // 需要重建索引:磁盘上已有封面但曲库还没认到 —— 给一个直接入口
                Capsule {
                    id: rebuildCap
                    anchors.left: errCap.visible ? errCap.right : parent.left
                    anchors.leftMargin: errCap.visible ? 8 : pop.pad
                    anchors.verticalCenter: parent.verticalCenter
                    visible: covers.needRebuild && !root.busy
                    accent: true
                    pad: 12
                    text: qsTr("重建索引")
                    onClicked: settings.rebuildLibrary()
                }
            }
            IconButton {
                id: selBtn
                anchors.right: allCap.left
                anchors.rightMargin: 10
                anchors.verticalCenter: foot.verticalCenter
                visible: root.selCount > 0 && !root.busy
                width: 44; height: 44
                filled: true
                icon: "import"
                iconSize: 18
                tip: qsTr("补齐选中的 %1 张").arg(root.selCount)
                onClicked: root.run()
            }
            Capsule {
                id: allCap
                anchors.right: parent.right
                anchors.rightMargin: pop.pad
                anchors.verticalCenter: foot.verticalCenter
                accent: true
                text: root.busy ? qsTr("取消") : qsTr("全部补全")
                onClicked: root.runAll()
            }

        }
    }
}
