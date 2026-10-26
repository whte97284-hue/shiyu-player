pragma Singleton
import QtQuick

// 设计系统:颜色 / 字体 / 圆角 / 动效时长。强调色由 Main.qml 绑定到当前专辑封面提取色
QtObject {
    id: theme

    // ---- 强调色 (随专辑封面,已在 C++ 侧驯化:限饱和、定亮度、蓝紫品红降饱和) ----
    // 不对 accent 做颜色动画:上百处绑定会在动画期间每帧重算,切歌瞬间掉帧;
    // 背景极光自带 1.6 秒色彩过渡,整体观感依然是平滑变色
    property color accent: "#c8a46e"

    // ---- 暖中性色:暖炭黑底、暖白文字,像胶片与高端音响面板,而非冷蓝科技感 ----
    readonly property color base: "#100f0e"
    readonly property color panel: Qt.rgba(0.082, 0.075, 0.068, 0.96)   // 弹出面板
    readonly property color tooltip: Qt.rgba(0.11, 0.10, 0.09, 0.96)
    readonly property color text: "#f3efe9"
    readonly property color textSecondary: Qt.rgba(0.953, 0.937, 0.914, 0.64)
    readonly property color textTertiary: Qt.rgba(0.953, 0.937, 0.914, 0.40)
    readonly property color hairline: Qt.rgba(1, 0.96, 0.9, 0.08)
    readonly property color hover: Qt.rgba(1, 0.96, 0.9, 0.055)
    readonly property color pressed: Qt.rgba(1, 0.96, 0.9, 0.09)

    // ---- 功能色 (低饱和,与强调色并存不抢戏) ----
    readonly property color perfect: "#d6b77e"   // Hi-Res / DSD 品质标签:香槟金
    // 状态色不另起色相:正常 = 强调色,存在转换 = 暖灰,全应用只有一个彩色
    readonly property color ok: accent
    readonly property color warn: "#a89c8c"
    function okAlpha(a) { return Qt.rgba(ok.r, ok.g, ok.b, a) }
    function warnAlpha(a) { return Qt.rgba(warn.r, warn.g, warn.b, a) }
    function perfectAlpha(a) { return Qt.rgba(perfect.r, perfect.g, perfect.b, a) }

    // 桌面歌词已唱部分的配色 (设置页与歌词窗口共用);0 跟随封面强调色
    readonly property var lyricPresets: [
        { name: qsTr("跟随封面"), c: accent },
        { name: qsTr("冰蓝"), c: "#8fd8ff" },
        { name: qsTr("樱粉"), c: "#ff9cbc" },
        { name: qsTr("薄荷"), c: "#7ee8be" },
        { name: qsTr("柠檬"), c: "#ffe27a" }
    ]

    // 强调色的派生色:用于按钮底色、柔光
    function accentAlpha(a) { return Qt.rgba(accent.r, accent.g, accent.b, a) }
    // 强调色偏亮时,其上的文字与图标改用深色 (驯化后的强调色大多属于此类,深字更显精致)
    readonly property bool accentIsLight: (accent.r * 0.299 + accent.g * 0.587 + accent.b * 0.114) > 0.46
    readonly property color onAccent: accentIsLight ? "#15120e" : "#ffffff"

    readonly property string fontFamily: {
        const preferred = ["Microsoft YaHei UI", "PingFang SC", "HarmonyOS Sans SC", "Noto Sans CJK SC", "Source Han Sans SC", "Noto Sans SC"]
        const all = Qt.fontFamilies()
        for (let i = 0; i < preferred.length; ++i)
            if (all.indexOf(preferred[i]) >= 0) return preferred[i]
        return Qt.application.font.family
    }

    readonly property int radiusS: 8
    readonly property int radiusM: 12
    readonly property int radiusL: 22

    // 「减弱动效」:时长缩短,背景停止流动
    property bool reduced: false
    readonly property int fast: reduced ? 80 : 140
    readonly property int normal: reduced ? 140 : 260
    readonly property int slow: reduced ? 220 : 520

    // 详情页左右留白:窄窗 44、宽窗 64;超宽屏时内容列最宽 1560 居中
    function pagePadding(w) { return Math.max(w > 1400 ? 64 : 44, Math.round((w - 1560) / 2)) }

    // 正在滚动的列表数 (SmoothScroll 维护):滚动期间暂停背景流动、呼吸点、悬停效果,把每一帧都留给滚动
    property int scrollers: 0
    // 停下后再保持 220ms:慢慢一格一格拨滚轮时,每格之间都会短暂停住,不保持的话「滚动中」会逐格开关,
    // 每次开关都让所有可见行的 hoverEnabled 重算、鼠标下那一行高亮淡入又淡出、背景恢复又暂停
    property bool scrollHold: false
    readonly property bool scrolling: scrollers > 0 || scrollHold
    onScrollersChanged: if (scrollers === 0) { scrollHold = true; holdTimer.restart() } else holdTimer.stop()
    property Timer holdTimer: Timer { interval: 220; onTriggered: theme.scrollHold = false }

    readonly property int navWidth: 72
    readonly property int islandHeight: 76

    // 秒 → m:ss;一小时以上 h:mm:ss (CUE 整轨、DJ 串烧)
    function formatTime(s) {
        s = Math.max(0, Math.floor(s))
        const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), r = s % 60
        const ss = (r < 10 ? "0" : "") + r
        return h > 0 ? h + ":" + (m < 10 ? "0" : "") + m + ":" + ss : m + ":" + ss
    }

    // 字节数 → 人类可读
    function formatBytes(b) {
        const u = ["B", "KB", "MB", "GB", "TB"]
        let i = 0
        let v = Number(b)
        while (v >= 1024 && i < u.length - 1) { v /= 1024; ++i }
        return (i === 0 ? v : v.toFixed(v >= 100 ? 0 : 1)) + " " + u[i]
    }
}
