import QtQuick
import PlayerUi

// 封面墙:一张可以往四面八方推的封面画布。
//   · 拖动 / 滚轮平移(与专辑墙同一套临界阻尼手感),Ctrl+滚轮或双指捏合缩放(0.6x ~ 1.8x)
//   · 满屏无缝密铺,没有文字、没有任何控件;鼠标贴近边缘才浮出导航与播放条
//   · 进入时从视口中心向外炸开
//   · 左键进专辑,右键出整张专辑的菜单
//
// 为什么不用 GridView:
//   GridView 只能单轴滚动(要么竖要么横),做不了"二维自由平移"。
//   所以这里用 Flickable 当画布,自己做**虚拟化**:只保留视口内(加一圈缓冲)的格子,
//   固定 220 个槽循环使用,平移时只改槽的位置和封面,绝不重建 delegate。
//
// 缩放为什么几乎不花代价:
//   格子尺寸始终按 baseCell 布局,缩放只作用在内层 Item 的 scale 上 ——
//   一个 transform 就搞定,不会触发 220 个格子的重新布局。
//   封面解码尺寸固定(cellSize × 1.25 × dpr),不随缩放重解码,所以缩放全程不闪不卡。
Item {
    id: burst
    signal openAlbum(int index, Item coverItem)
    signal exitRequested
    // 鼠标靠近窗口边缘时提示主界面把控件浮出来
    signal chromeHint(bool show)

    // 基准格子尺寸:布局坐标固定用它,缩放只改内层的 scale
    readonly property int baseCell: 300
    // 列数:在"接近正方形"的范围内挑一个**余数最小**的列数。
    // 画布是规整网格,最后一行不满就会在右下角留下空格子(滚到底能看见),
    // 所以这里搜索一遍,让 cols*rows - count 尽量接近 0(971 张时 36 列只剩 1 格)。
    readonly property int cols: {
        const n = Math.max(1, albumModel.count)
        const target = Math.sqrt(n * 1.7)
        const lo = Math.max(4, Math.round(target * 0.7))
        const hi = Math.max(lo + 1, Math.round(target * 1.35))
        let best = Math.max(4, Math.round(target))
        let bestWaste = Number.MAX_SAFE_INTEGER
        for (let c = lo; c <= hi; ++c) {
            const waste = c * Math.ceil(n / c) - n
            if (waste < bestWaste) { bestWaste = waste; best = c }
        }
        return best
    }
    readonly property int rows: Math.max(1, Math.ceil(albumModel.count / cols))

    // ---- 缩放 ----
    // zoom 是**实际生效**的值(由动画驱动),zoomGoal 是滚轮累加出来的目标值。
    // 直接改 zoom 会一顿一顿地跳,所以滚轮只推目标,动画负责追。
    property real zoom: 1.0
    property real zoomGoal: 1.0
    readonly property real minZoom: 0.6
    readonly property real maxZoom: 1.8
    // 解码倍率:固定 1.25 倍,留一点余量给放大。
    // 不按 zoom 分档重解码 —— 那样每次跨档 200 多个槽同时换纹理,是闪烁和 100ms 级卡顿的来源。
    readonly property real decodeScale: 1.25
    // 缩放的锚点(内容坐标 + 视口坐标):动画每帧用它重算 contentX/Y,
    // 保证鼠标下的那张封面在缩放过程中原地不动,不会"漂"
    property real anchorCx: 0
    property real anchorCy: 0
    property real anchorVx: 0
    property real anchorVy: 0

    // 槽池大小:最小缩放下全屏约 16x9=144 格,留一圈缓冲
    readonly property int poolSize: 220

    // 入场只播一次。总时长 = 延迟上限 700 + 动画 560,留足余量
    property bool intro: true
    Timer { interval: 1800; running: burst.intro; onTriggered: burst.intro = false }

    // 自动化入口(截图验证用):直接驱动画布,不依赖滚轮事件链
    function automationPan(dx, dy) {
        canvas.contentX += dx
        canvas.contentY += dy
        burst.relayout(true)
    }
    function automationZoom(z) { burst.zoomTo(z, canvas.width / 2, canvas.height / 2) }
    function automationOpenSlot(row) {
        const s = pool.itemAt(row % burst.poolSize)
        if (s && s.albumIndex >= 0) burst.openAlbum(s.albumIndex, s.coverItem)
    }
    function scrollToTop() { pan.scrollTo(0, 0) }

    // 以视口坐标 (vx, vy) 为中心缩放到 z
    function zoomTo(z, vx, vy) {
        const target = Math.max(burst.minZoom, Math.min(burst.maxZoom, z))
        if (Math.abs(target - burst.zoomGoal) < 0.0001) return
        anchorVx = vx
        anchorVy = vy
        anchorCx = (canvas.contentX + vx) / Math.max(0.001, burst.zoom)
        anchorCy = (canvas.contentY + vy) / Math.max(0.001, burst.zoom)
        zoomGoal = target
        zoomAnim.restart()
    }

    // 动画驱动 zoom;每帧用锚点回算滚动位置,锚点不漂
    onZoomChanged: {
        canvas.contentX = anchorCx * zoom - anchorVx
        canvas.contentY = anchorCy * zoom - anchorVy
        burst.relayout()
    }
    NumberAnimation {
        id: zoomAnim
        target: burst
        property: "zoom"
        duration: 140
        easing.type: Easing.OutCubic
        // 动画结束后做一次硬钳制:缩小时内容变小,位置可能已经越界
        onStopped: Qt.callLater(() => burst.relayout(true))
    }

    // Esc 退出。用 Shortcut 而不是 Keys.onEscapePressed —— 后者依赖焦点,
    // 焦点被主界面的按键捕获层拿走时就失效了
    Shortcut {
        sequence: "Escape"
        enabled: burst.visible
        onActivated: burst.exitRequested()
    }

    // ---- 边缘感应 ----
    // 鼠标贴近左 / 上 / 下边缘时让主界面把导航和播放条浮出来。
    // 两点注意:
    //  · 必须记住上一次状态再发信号 —— 每次移动都穿透到 Main 改属性,鼠标一动就卡
    //  · 显隐用不同阈值(迟滞),否则贴边时会在阈值上反复 toggle
    property bool chromeShown: false
    readonly property int chromeShowX: 88
    readonly property int chromeHideX: 124
    readonly property int chromeShowTop: 64
    readonly property int chromeHideTop: 96
    readonly property int chromeShowBottom: 132
    readonly property int chromeHideBottom: 168
    HoverHandler {
        onPointChanged: {
            const p = point.position
            const inX = p.x < (burst.chromeShown ? burst.chromeHideX : burst.chromeShowX)
            const inTop = p.y < (burst.chromeShown ? burst.chromeHideTop : burst.chromeShowTop)
            const inBottom = p.y > burst.height - (burst.chromeShown ? burst.chromeHideBottom : burst.chromeShowBottom)
            const want = inX || inTop || inBottom
            if (want !== burst.chromeShown) {
                burst.chromeShown = want
                burst.chromeHint(want)
            }
        }
    }

    Flickable {
        id: canvas
        anchors.fill: parent
        clip: true
        // DragOverBounds:拖过边界时能拉出一点再回弹,不是硬撞停。
        // 真正的"不许滚到空白"由 relayout 的钳制负责(见下),两者不冲突:
        // 拖动越界时 relayout 只夹住**算范围**用的坐标,不回写 contentX/Y,回弹才不会被吃掉
        boundsBehavior: Flickable.DragOverBounds
        flickableDirection: Flickable.HorizontalAndVerticalFlick
        pressDelay: 0
        contentWidth: Math.max(width, inner.width * inner.scale)
        contentHeight: Math.max(height, inner.height * inner.scale)

        // 内容层:布局坐标固定为 baseCell,缩放只改这一层的 scale
        Item {
            id: inner
            width: burst.cols * burst.baseCell
            height: burst.rows * burst.baseCell
            scale: burst.zoom
            transformOrigin: Item.TopLeft

            Repeater {
                id: pool
                model: burst.poolSize
                delegate: slotComp
            }
        }

        SmoothPan { id: pan; flickable: canvas }

        // 平移后重排可见槽(只在可见范围变化时才真的干活)
        onContentXChanged: burst.relayout()
        onContentYChanged: burst.relayout()
        onWidthChanged: burst.relayout(true)
        onHeightChanged: burst.relayout(true)
    }

    // 双指捏合缩放(触摸板 / 触屏)
    PinchHandler {
        id: pinch
        target: null
        property real z0: 1
        property real cx0: 0
        property real cy0: 0
        property real vx0: 0
        property real vy0: 0
        onActiveChanged: {
            if (!active) return
            z0 = burst.zoom
            vx0 = pinch.centroid.position.x
            vy0 = pinch.centroid.position.y
            cx0 = (canvas.contentX + vx0) / Math.max(0.001, burst.zoom)
            cy0 = (canvas.contentY + vy0) / Math.max(0.001, burst.zoom)
        }
        onScaleChanged: {
            if (!active) return
            // 捏合过程中直接跟手(不做动画,否则手指和画面脱节)
            zoomAnim.stop()
            const next = Math.max(burst.minZoom, Math.min(burst.maxZoom, z0 * pinch.scale))
            burst.anchorVx = vx0
            burst.anchorVy = vy0
            burst.anchorCx = cx0
            burst.anchorCy = cy0
            burst.zoomGoal = next
            burst.zoom = next
        }
    }

    // 滚轮:Ctrl 缩放,否则平移(平移交给 SmoothPan)
    WheelHandler {
        acceptedModifiers: Qt.ControlModifier
        onWheel: (e) => {
            const k = e.angleDelta.y > 0 ? 1.12 : 1 / 1.12
            burst.zoomTo(burst.zoomGoal * k, e.point.position.x, e.point.position.y)
        }
    }

    // ---- 虚拟化:把可见范围内的行/列分配给槽 ----
    property int lastC0: -1
    property int lastR0: -1
    property int lastC1: -1
    property int lastR1: -1

    function relayout(force) {
        const c = burst.baseCell
        const z = burst.zoom
        const vw = canvas.width / z
        const vh = canvas.height / z
        const maxVx = Math.max(0, burst.cols * c - vw)
        const maxVy = Math.max(0, burst.rows * c - vh)
        const rawVx = canvas.contentX / z
        const rawVy = canvas.contentY / z
        // 算可见范围时用夹住的值(拖动越界时依然显示边缘内容);
        // 只有强制重排(缩放结束 / 改窗口 / 曲库变化)才把位置写回去,
        // 否则每一帧都会把 DragOverBounds 的回弹吃掉,边界变成硬停
        const vx = Math.max(0, Math.min(maxVx, rawVx))
        const vy = Math.max(0, Math.min(maxVy, rawVy))
        if (force && (rawVx !== vx || rawVy !== vy)) {
            canvas.contentX = vx * z
            canvas.contentY = vy * z
        }
        const c0 = Math.max(0, Math.floor(vx / c) - 1)
        const r0 = Math.max(0, Math.floor(vy / c) - 1)
        const c1 = Math.min(burst.cols - 1, Math.ceil((vx + vw) / c) + 1)
        const r1 = Math.min(burst.rows - 1, Math.ceil((vy + vh) / c) + 1)
        if (!force && c0 === lastC0 && r0 === lastR0 && c1 === lastC1 && r1 === lastR1) return
        lastC0 = c0; lastR0 = r0; lastC1 = c1; lastR1 = r1

        const total = albumModel.count
        const n = pool.count
        // 关键:**同一个网格位置永远用同一个槽**(row % poolSize)。
        // 按"从左上角线性遍历"分配的话,平移一格就让所有槽换成别的专辑、
        // 整屏 source 同时变化 —— 那就是画面一直跳变闪烁的根因。
        const used = new Array(n)
        for (let r = r0; r <= r1; ++r) {
            for (let col = c0; col <= c1; ++col) {
                const row = r * burst.cols + col
                const si = row % n
                const s = pool.itemAt(si)
                if (!s) continue
                used[si] = true
                s.x = col * c
                s.y = r * c
                s.cellSize = c
                if (row >= total) {
                    // 末尾余数格:拿最后一张封面填满,右下角就不会出现空洞。
                    // albumIndex 置 -1,点它不打开专辑
                    if (s.row !== row) {
                        s.row = row
                        s.coverUrl = albumModel.coverAt(total - 1)
                        s.albumIndex = -1
                    }
                } else if (s.row !== row) {
                    s.row = row
                    s.coverUrl = albumModel.coverAt(row)
                    s.albumIndex = albumModel.albumAt(row)
                }
                s.visible = true
            }
        }
        for (let i = 0; i < n; ++i) {
            if (used[i]) continue
            const s = pool.itemAt(i)
            if (s && s.visible) s.visible = false
        }
    }

    // 曲库变化(重扫 / 换排序)后重新分配
    Connections {
        target: albumModel
        function onViewChanged() { burst.relayout(true) }
    }

    Component.onCompleted: Qt.callLater(() => burst.relayout(true))

    // ---- 单个格子 ----
    Component {
        id: slotComp
        Item {
            id: slot
            property int row: -1
            property int albumIndex: -1
            property string coverUrl: ""
            property real cellSize: burst.baseCell
            property bool entered: false
            // 悬停态(带进入延迟):鼠标快速扫过时不放大,避免整屏"抽搐"
            property bool hovered: false
            visible: false
            width: cellSize
            height: cellSize
            // z 必须抬在**槽**这一层:放在内部子项上抬不出父级,
            // 放大后的四条边会被后创建的邻居盖住,看起来像被啃掉一块
            z: mouse.pressed ? 6 : (slot.hovered ? 5 : 1)

            HoverHandler { id: hover }
            Timer {
                id: hoverIn
                interval: 60
                onTriggered: slot.hovered = true
            }
            Timer {
                id: hoverOut
                interval: 90
                onTriggered: slot.hovered = false
            }
            Connections {
                target: hover
                function onHoveredChanged() {
                    if (hover.hovered) { hoverOut.stop(); hoverIn.restart() }
                    else { hoverIn.stop(); hoverOut.restart() }
                }
            }

            // 悬停放大 + 按下回缩,和入场动画(inner)分开,互不打架
            Item {
                id: hoverBox
                anchors.fill: parent
                scale: mouse.pressed ? 0.965 : (slot.hovered ? 1.05 : 1)
                Behavior on scale {
                    ScaleAnimator {
                        duration: mouse.pressed ? 90 : (slot.hovered ? 150 : 220)
                        easing.type: Easing.OutCubic
                    }
                }

                Item {
                    id: inner
                    anchors.fill: parent
                    transformOrigin: Item.Center
                    opacity: 1
                    scale: 1
                    rotation: 0

                    Rectangle {
                        anchors.fill: parent
                        color: Theme.hover
                    }

                    Image {
                        id: img
                        anchors.fill: parent
                        source: slot.coverUrl
                        asynchronous: true
                        // Qt 的图片缓存是 LRU(有上限),开着能让来回平移时命中最近解过的图;
                        // 关掉反而每次换 source 都要重新解码
                        cache: true
                        smooth: true
                        fillMode: Image.PreserveAspectCrop
                        readonly property real pxScale: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
                        readonly property real px: Math.ceil(slot.cellSize * burst.decodeScale * pxScale)
                        sourceSize: Qt.size(px, px)
                        // 这两个属性是给 Main 的"封面飞向详情页"动画用的:
                        // imageIn() 靠 status + decodeSize 认出可飞的图块,靠 radius 算起始圆角。
                        // 纯 Image 没有它们,飞行会直接失败 —— 点击那一瞬就成了硬切,看起来像闪了一下。
                        readonly property int decodeSize: px
                        readonly property real radius: 0
                    }

                    // 按下时压一层暗色,给一点"被按住"的实感
                    Rectangle {
                        anchors.fill: parent
                        color: Theme.pressed
                        opacity: mouse.pressed ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 90 } }
                    }

                    Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                        border.width: 2
                        border.color: Qt.rgba(1, 1, 1, slot.hovered && !mouse.pressed ? 0.7 : 0.0)
                        Behavior on border.color { ColorAnimation { duration: Theme.fast } }
                    }

                    SequentialAnimation {
                        id: enter
                        PauseAnimation { duration: slot.enterDelay }
                        ParallelAnimation {
                            // overshoot 不能太大:页面本身还在 460ms 的缩放淡入,
                            // 两层弹性叠在一起会显得轻浮。1.3 已经够"弹"
                            ScaleAnimator {
                                target: inner
                                from: 0.05; to: 1
                                duration: 560
                                easing.type: Easing.OutBack
                                easing.overshoot: 1.3
                            }
                            RotationAnimator {
                                // 符号按行列散列,相邻格子方向不同 —— 按 row 奇偶取符号会让整行同向,
                                // 一眼看出斜条纹
                                from: ((slot.row * 7 + (slot.row % burst.cols) * 13) % 2 ? -1 : 1) * 8
                                to: 0
                                duration: 560
                                easing.type: Easing.OutBack
                                easing.overshoot: 1.2
                            }
                            OpacityAnimator { target: inner; from: 0; to: 1; duration: 300 }
                        }
                    }
                }

                MouseArea {
                    id: mouse
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: canvas.dragging ? Qt.ClosedHandCursor
                               : (slot.albumIndex >= 0 ? Qt.PointingHandCursor : Qt.ArrowCursor)
                    onClicked: (m) => {
                        if (slot.albumIndex < 0) return
                        if (m.button === Qt.RightButton) {
                            const p = mapToItem(null, m.x, m.y)
                            Nav.trackMenu(-1, p.x, p.y, { albumIndex: slot.albumIndex })
                        } else {
                            burst.openAlbum(slot.albumIndex, img)
                        }
                    }
                }
            }

            // 首次可见且还在入场窗口内:从**视口中心**向外炸开。
            // 不能用整个网格的中心 —— 画布有几十列,那样所有可见格子的距离都一样,
            // 延迟全被封顶,就变成"同时出现",没有扩散感
            readonly property real enterDelay: {
                const c = burst.baseCell
                const cx = canvas.contentX / c + canvas.width / c / 2
                const cy = canvas.contentY / c + canvas.height / c / 2
                const col = slot.row % burst.cols
                const row = Math.floor(slot.row / burst.cols)
                const d = Math.sqrt((row - cy) * (row - cy) + (col - cx) * (col - cx))
                return Math.min(700, d * 80)
            }
            onVisibleChanged: {
                if (!visible || entered || !burst.intro || Theme.reduced) return
                entered = true
                inner.opacity = 0
                inner.scale = 0.05
                Qt.callLater(() => enter.restart())
            }
        }
    }
}
