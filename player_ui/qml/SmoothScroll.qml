import QtQuick

// 平滑滚轮:鼠标滚轮每一格以缓出曲线滑动,连续滚动时目标累加 (不会一顿一顿);
// 触控板 (像素增量) 直接跟手。挂在 Flickable 内部:SmoothScroll { flickable: list }
WheelHandler {
    id: h
    property Flickable flickable: parent
    property bool horizontal: false
    property real step: 120          // 每格滚动像素
    property int duration: 340
    signal scrolled()
    target: null
    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
    // 横向条内容不够滚时不拦截滚轮,交给外层页面纵向滚动
    enabled: !horizontal || maxPos > minPos + 1

    readonly property real minPos: horizontal ? flickable.originX - flickable.leftMargin : flickable.originY - flickable.topMargin
    readonly property real maxPos: Math.max(minPos, horizontal
        ? flickable.originX + flickable.contentWidth - flickable.width + flickable.rightMargin
        : flickable.originY + flickable.contentHeight - flickable.height + flickable.bottomMargin)
    property real goal: 0

    function clamp(v) { return Math.max(minPos, Math.min(maxPos, v)) }
    function current() { return horizontal ? flickable.contentX : flickable.contentY }
    function apply(v) { if (horizontal) flickable.contentX = v; else flickable.contentY = v }

    onWheel: (e) => {
        if (!flickable) return
        h.scrolled()
        const pixel = horizontal ? (e.pixelDelta.x || e.pixelDelta.y) : e.pixelDelta.y
        const angle = horizontal ? (e.angleDelta.x || e.angleDelta.y) : e.angleDelta.y
        if (pixel !== 0 && e.device.type === PointerDevice.TouchPad) {
            anim.stop()
            apply(clamp(current() - pixel))
            return
        }
        if (angle === 0) return
        if (!anim.running) { pos = current(); goal = pos; vel = 0 }
        goal = clamp(goal - angle / 120 * step)
        anim.start()
    }

    // 逐帧用临界阻尼弹簧追随目标 (速度连续):
    //  · 连续滚动时只是把目标往前推,当前速度会平滑地加上去,不会每格「猛冲一下再刹住」。
    //    原来的一阶指数追随在每格到来时速度瞬间跳到最大再迅速衰减,连续滚动时速度呈锯齿状一快一慢,
    //    帧率计不掉帧,眼睛看起来却像低帧率;
    //  · 按显示器帧长计算,60 / 144Hz 下速度一致;
    //  · 运动途中不再逐帧对齐像素 (速度不是整数像素 / 帧时,取整会让每帧位移 1、2、1、2 地交替,也是一种抖),
    //    停稳时才对齐到物理像素,静止的文字和封面依然清晰
    property real pos: 0
    property real vel: 0
    readonly property real smoothTime: duration / 2600   // 340ms 约 0.13s,约 0.4s 基本停稳
    // 显示器一帧的时长。frameTime 是界面线程「被叫醒」的时刻差,会被同一帧里其他工作推迟或提前几毫秒,
    // 而画面总是按刷新节拍显示:直接用它推进,每帧位移忽大忽小,看起来就是细碎的抖动。
    // 所以把间隔对齐到整数帧 (掉帧时按掉了几帧算),和 Qt 自己的 vsync 动画驱动、浏览器的平滑滚动同一做法
    property real vsync: 1 / 60
    // 判定掉帧要留足余量:Qt 的动画驱动在 144Hz 下容差只有 8.7ms,几乎一直处在计时器模式,frameTime 是
    // 界面线程被叫醒的挂钟时刻差,抖动 ±3~4ms 很常见。按 1.5 帧四舍五入的话,一帧 10.5ms 就会被当成掉了一帧、
    // 一次走两步 —— 144Hz 下这正是「同样 144fps,滚起来却一顿一顿」的来源 (帧诊断日志实测:出帧稳定 6.9ms,
    // 驱动在两种模式间切了上百次)。真正掉帧时间隔接近两帧 (13.9ms),所以 1.75 帧以下一律算一帧
    function frameStep(ft) {
        const r = ft / vsync
        const n = r < 1.75 ? 1 : Math.round(r)
        return Math.min(0.1, n * vsync)
    }
    // 滚动中 (滚轮动画或拖动 / 惯性) 时登记到 Theme.scrolling (不叫 active:那是 WheelHandler 自带的属性)
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
            const g = h.clamp(h.goal)
            // 临界阻尼 SmoothDamp (Game Programming Gems 4, 1.10)
            const dt = h.frameStep(frameTime)
            const w = 2 / h.smoothTime
            const x = w * dt
            const e = 1 / (1 + x + 0.48 * x * x + 0.235 * x * x * x)
            const c = h.pos - g
            const t = (h.vel + w * c) * dt
            h.vel = (h.vel - w * t) * e
            h.pos = g + (c + t) * e
            // 到边界时不越过去回弹
            if ((h.pos - h.minPos) * (h.pos - h.maxPos) > 0) { h.pos = h.clamp(h.pos); h.vel = 0 }
            if (Math.abs(g - h.pos) < 0.3 && Math.abs(h.vel) < 12) {
                // 注意: QQuickWindow / QWindow 在 QML 里 **没有** devicePixelRatio 属性,
                // 用 Window.window.devicePixelRatio 会取到 undefined -> contentY = NaN -> 完全滚不动。
                // 用附加属性 Screen.devicePixelRatio (QtQuick 自带, 无需额外 import)。
                const dpr = h.flickable.Screen.devicePixelRatio > 0 ? h.flickable.Screen.devicePixelRatio : 1
                h.pos = g; h.vel = 0
                h.apply(Math.round(g * dpr) / dpr)
                stop()
                return
            }
            h.apply(h.pos)
        }
    }
    // 用户拖动或代码跳转时停止动画
    property Connections stopOnDrag: Connections {
        target: h.flickable
        function onMovementStarted() { anim.stop() }
    }
    function stop() { anim.stop() }
    // 代码驱动的平滑滚动 (回到顶部、翻页):与滚轮共用同一条追随曲线
    function destination() { return anim.running ? goal : current() }
    function scrollTo(v) {
        if (!flickable) return
        if (!anim.running) { pos = current(); vel = 0 }
        goal = clamp(v)
        anim.start()
    }
}
