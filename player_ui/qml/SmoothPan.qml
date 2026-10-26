import QtQuick

// 平滑二维平移:滚轮 / 触控板驱动 Flickable 的 contentX 与 contentY。
//
// 这是 SmoothScroll.qml 的二维孪生版 —— 沿用同一套临界阻尼弹簧和帧长对齐逻辑,
// 让封面墙和专辑墙的滚动手感完全一致(同一个软件里两种滚动手感会很明显)。
// 挂在 Flickable 内部:SmoothPan { flickable: canvas }
//
// 与一维版的差别:
//  · X / Y 各自维护一套 goal / pos / vel,但共用同一个 FrameAnimation
//  · 触控板像素增量两轴同时跟手
//  · 滚轮两轴同时累加(普通鼠标只有 y;按住 Shift 时系统会给 x)
WheelHandler {
    id: h
    property Flickable flickable: parent
    property real step: 120          // 每格滚动像素
    property int duration: 340
    signal scrolled()
    target: null
    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

    readonly property real minX: flickable ? flickable.originX - flickable.leftMargin : 0
    readonly property real maxX: flickable ? Math.max(minX, flickable.originX + flickable.contentWidth
                                                      - flickable.width + flickable.rightMargin) : 0
    readonly property real minY: flickable ? flickable.originY - flickable.topMargin : 0
    readonly property real maxY: flickable ? Math.max(minY, flickable.originY + flickable.contentHeight
                                                      - flickable.height + flickable.bottomMargin) : 0

    property real goalX: 0
    property real goalY: 0
    property real posX: 0
    property real posY: 0
    property real velX: 0
    property real velY: 0
    readonly property real smoothTime: duration / 2600
    property real vsync: 1 / 60

    function clampX(v) { return Math.max(minX, Math.min(maxX, v)) }
    function clampY(v) { return Math.max(minY, Math.min(maxY, v)) }

    // 与 SmoothScroll 相同:把帧间隔对齐到整数帧,避免高刷下位移忽大忽小
    function frameStep(ft) {
        const r = ft / vsync
        const n = r < 1.75 ? 1 : Math.round(r)
        return Math.min(0.1, n * vsync)
    }

    onWheel: (e) => {
        if (!flickable) return
        h.scrolled()
        const px = e.pixelDelta.x
        const py = e.pixelDelta.y
        // 触控板给的是像素增量:直接跟手,不走弹簧(否则手指移动和画面不同步)
        if ((px !== 0 || py !== 0) && e.device.type === PointerDevice.TouchPad) {
            anim.stop()
            flickable.contentX = clampX(flickable.contentX - px)
            flickable.contentY = clampY(flickable.contentY - py)
            return
        }
        const ax = e.angleDelta.x
        const ay = e.angleDelta.y
        if (ax === 0 && ay === 0) return
        if (!anim.running) {
            posX = flickable.contentX; posY = flickable.contentY
            goalX = posX; goalY = posY
            velX = 0; velY = 0
        }
        goalX = clampX(goalX - ax / 120 * step)
        goalY = clampY(goalY - ay / 120 * step)
        anim.start()
    }

    // 滚动中(滚轮动画或拖动 / 惯性)登记到 Theme.scrolling,页面据此暂停悬停动画
    readonly property bool engaged: anim.running || (flickable ? flickable.moving : false)
    property bool counted: false
    onEngagedChanged: {
        if (engaged === counted) return
        counted = engaged
        Theme.scrollers += engaged ? 1 : -1
    }
    Component.onDestruction: if (counted) Theme.scrollers -= 1

    property FrameAnimation anim: FrameAnimation {
        onRunningChanged: if (running) {
            const w = h.flickable ? h.flickable.Window.window : null
            h.vsync = 1 / (w ? windowHelper.refreshRate(w) : 60)
        }
        onTriggered: {
            const dt = h.frameStep(frameTime)
            const w = 2 / h.smoothTime
            const x = w * dt
            const e = 1 / (1 + x + 0.48 * x * x + 0.235 * x * x * x)

            const gx = h.clampX(h.goalX)
            const cx = h.posX - gx
            const tx = (h.velX + w * cx) * dt
            h.velX = (h.velX - w * tx) * e
            h.posX = gx + (cx + tx) * e

            const gy = h.clampY(h.goalY)
            const cy = h.posY - gy
            const ty = (h.velY + w * cy) * dt
            h.velY = (h.velY - w * ty) * e
            h.posY = gy + (cy + ty) * e

            // 到边界不越过去
            if ((h.posX - h.minX) * (h.posX - h.maxX) > 0) { h.posX = h.clampX(h.posX); h.velX = 0 }
            if ((h.posY - h.minY) * (h.posY - h.maxY) > 0) { h.posY = h.clampY(h.posY); h.velY = 0 }

            const nearX = Math.abs(gx - h.posX) < 0.3 && Math.abs(h.velX) < 12
            const nearY = Math.abs(gy - h.posY) < 0.3 && Math.abs(h.velY) < 12
            if (nearX && nearY) {
                // 停稳时对齐到物理像素,静止的封面边缘依然清晰
                const dpr = h.flickable.Screen.devicePixelRatio > 0 ? h.flickable.Screen.devicePixelRatio : 1
                h.posX = gx; h.posY = gy; h.velX = 0; h.velY = 0
                h.flickable.contentX = Math.round(gx * dpr) / dpr
                h.flickable.contentY = Math.round(gy * dpr) / dpr
                stop()
                return
            }
            h.flickable.contentX = h.posX
            h.flickable.contentY = h.posY
        }
    }

    // 用户拖动或代码跳转时停止弹簧动画,避免和手指打架
    property Connections stopOnDrag: Connections {
        target: h.flickable
        function onMovementStarted() { anim.stop() }
    }

    function stop() { anim.stop() }
    function destinationY() { return anim.running ? goalY : (flickable ? flickable.contentY : 0) }
    // 代码驱动的平滑滚动(回到起点、定位到某张)
    function scrollTo(x, y) {
        if (!flickable) return
        if (!anim.running) {
            posX = flickable.contentX; posY = flickable.contentY
            velX = 0; velY = 0
        }
        goalX = clampX(x); goalY = clampY(y)
        anim.start()
    }
}
