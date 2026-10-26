import QtQuick
import QtQuick.Window
import PlayerUi

// 桌面歌词:置顶、透明、无边框的独立窗口。
//  · 双行交替 (酷狗式:上一行靠左、下一行靠右),当前句逐字高亮;有翻译时每句原文下面带一行小字翻译
//  · 悬停显示半透明底板与工具条:上一首 / 播放 / 下一首 · 字号 · 配色 · 单双行 · 锁定 · 关闭
//  · 锁定后鼠标穿透 (不挡桌面操作);鼠标移到歌词上时只露出一个「解锁」小按钮,只有它能点
//  · 拖动任意空白处移动,位置记住
Window {
    id: dl
    readonly property bool locked: settings.lyricsLocked
    readonly property int fs: settings.lyricsFontSize
    // 鼠标是否在窗口范围内:由定时器读全局光标位置判断,不依赖窗口收到的鼠标事件
    // (透明像素在 Windows 上不接收鼠标,只靠 containsMouse 时鼠标从字缝移到工具条就会「离开」,工具条随即消失)
    property bool cursorInside: false
    property bool overUnlock: false
    readonly property bool hoverLocked: locked && cursorInside
    // 未锁定:鼠标离开后再留 0.8 秒,方便移到工具条上
    readonly property bool hovered: locked ? hoverLocked : cursorInside || hoverArea.containsMouse || dragging || linger.running
    property bool dragging: false
    onCursorInsideChanged: if (!cursorInside && !locked) linger.restart()
    Timer { id: linger; interval: 800 }

    // 配色:已唱部分的颜色;0 跟随封面强调色
    readonly property var presets: Theme.lyricPresets
    readonly property color sungColor: presets[Math.max(0, Math.min(presets.length - 1, settings.lyricsColor))].c

    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
           | (locked && !overUnlock ? Qt.WindowTransparentForInput : 0)
    color: "transparent"
    // 不挂在主窗口下:主窗口最小化时桌面歌词照常显示
    transientParent: null
    title: qsTr("桌面歌词")
    width: Math.min(Screen.width - 40, Math.max(640, fs * 26))
    // 高度按整首歌是否带翻译固定下来,不随某一句有没有翻译跳动
    height: toolbarH + Math.ceil(fs * 1.35) * (settings.lyricsTwoLine ? 2 : 1)
            + (anyTrans ? Math.ceil(transSize * 1.35) * (settings.lyricsTwoLine ? 2 : 1) : 0)
            + Math.round(fs * 0.12) * (settings.lyricsTwoLine ? 1 : 0) + 28
    readonly property int toolbarH: 44

    function placeDefault() {
        const p = settings.lyricsPos
        // 记住的位置还在某块屏幕上就用它 (换了显示器则回到默认)
        const scr = Qt.application.screens
        for (let i = 0; i < scr.length; ++i) {
            const s = scr[i]
            if (p.x >= s.virtualX && p.x < s.virtualX + s.width - 60 && p.y >= s.virtualY && p.y < s.virtualY + s.height - 40) {
                x = p.x; y = p.y
                return
            }
        }
        x = Screen.virtualX + (Screen.width - width) / 2
        y = Screen.virtualY + Screen.height - height - 120
    }
    onVisibleChanged: if (visible) placeDefault()
    Component.onCompleted: placeDefault()

    // ---------- 歌词数据 ----------
    readonly property var lines: player.lyrics
    readonly property bool synced: player.lyricsSynced && lines.length > 0
    readonly property int cur: synced ? Math.max(0, player.lyricIndex) : -1
    readonly property var curLine: cur >= 0 && cur < lines.length ? lines[cur] : null
    readonly property string fallback: player.hasTrack ? player.title + (player.artist ? "  —  " + player.artist : "") : "拾羽"
    function lineText(i) { return i >= 0 && i < lines.length ? lines[i].text : "" }
    function transText(i) { return i >= 0 && i < lines.length && lines[i].translation ? lines[i].translation : "" }
    function lineWords(i) { return i >= 0 && i < lines.length && lines[i].words ? lines[i].words : [] }
    // 这首歌有没有翻译 (有的话每句原文下面留一行给翻译)
    readonly property bool anyTrans: { for (let i = 0; i < lines.length; ++i) if (lines[i].translation) return true; return false }
    readonly property int transSize: Math.round(fs * 0.62)
    // 当前句演唱进度 0..1 (player.position 约 60Hz 更新,足够平滑)
    function progressOf(i) {
        if (!synced || i !== cur || !curLine) return i < cur ? 1 : 0
        if (player.lyricIndex < 0) return 0
        const d = curLine.duration > 0 ? curLine.duration : 3
        return Math.max(0, Math.min(1, (player.position - curLine.time) / d))
    }
    // 双行交替:偶数句在上 (靠左),奇数句在下 (靠右);另一行预告下一句
    readonly property int topIdx: cur < 0 ? -1 : (cur % 2 === 0 ? cur : cur + 1)
    readonly property int botIdx: cur < 0 ? -1 : (cur % 2 === 1 ? cur : cur + 1)

    // ---------- 悬停检测 (锁定与否都用全局光标位置) ----------
    Timer {
        interval: 100
        repeat: true
        running: dl.visible
        triggeredOnStart: true
        onTriggered: {
            const p = windowHelper.cursorPos()
            const inWin = p.x >= dl.x && p.x < dl.x + dl.width && p.y >= dl.y && p.y < dl.y + dl.height
            dl.cursorInside = inWin
            if (!dl.locked) { dl.overUnlock = false; return }
            const b = unlockBtn.mapToGlobal(0, 0)
            dl.overUnlock = inWin && p.x >= b.x - 6 && p.x < b.x + unlockBtn.width + 6 && p.y >= b.y - 6 && p.y < b.y + unlockBtn.height + 6
        }
        onRunningChanged: if (!running) { dl.cursorInside = false; dl.overUnlock = false }
    }

    // ---------- 底板 (悬停且未锁定时) ----------
    // 未锁定时整块窗口铺一层肉眼看不出的底 (alpha 约 3/255):Windows 上完全透明的像素收不到鼠标,
    // 有了它,窗口内任何位置都能拖动、点到工具条
    Rectangle {
        anchors.fill: parent
        radius: 18
        color: Qt.rgba(0, 0, 0, 0.012)
        visible: !dl.locked
    }
    Rectangle {
        anchors.fill: parent
        radius: 18
        color: Qt.rgba(0.06, 0.055, 0.05, 0.62)
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.08)
        opacity: dl.hovered && !dl.locked ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
    }

    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        onPressed: { dl.dragging = true; dl.startSystemMove() }
        onReleased: dl.dragging = false
        onCanceled: dl.dragging = false
    }
    // 系统拖动结束后不一定收到 released:位置一变就记下来。
    // 注意这里**不能**用 dl.dragging 当条件:startSystemMove() 之后鼠标被系统接管,
    // MouseArea 的 onReleased 会立刻触发把 dragging 清成 false,
    // 等 400ms 后 Timer 真正执行时条件已经不成立 —— 位置永远存不下来。
    onXChanged: savePos.restart()
    onYChanged: savePos.restart()
    Timer { id: savePos; interval: 400; onTriggered: { if (dl.visible) settings.lyricsPos = Qt.point(Math.round(dl.x), Math.round(dl.y)) } }

    // ---------- 工具条 ----------
    component Tool: Rectangle {
        id: tool
        property string icon
        property string label
        property bool on: false
        signal clicked()
        width: label.length ? lab.implicitWidth + 20 : 32
        height: 32
        radius: 16
        color: tm.containsMouse ? Qt.rgba(1, 1, 1, 0.12) : on ? Qt.rgba(1, 1, 1, 0.08) : "transparent"
        Icon { visible: tool.icon.length > 0; anchors.centerIn: parent; name: tool.icon; size: 15 }
        Text {
            id: lab
            visible: tool.label.length > 0
            anchors.centerIn: parent
            text: tool.label
            color: "#f3efe9"
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }
        MouseArea { id: tm; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: tool.clicked() }
    }

    Row {
        id: toolbar
        anchors.horizontalCenter: parent.horizontalCenter
        y: 8
        height: dl.toolbarH - 12
        spacing: 4
        opacity: dl.hovered && !dl.locked ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 180 } }

        Tool { icon: "prev"; onClicked: player.previous() }
        Tool { icon: player.playing ? "pause" : "play"; onClicked: player.toggle() }
        Tool { icon: "next"; onClicked: player.next() }
        Rectangle { width: 1; height: 16; anchors.verticalCenter: parent.verticalCenter; color: Qt.rgba(1, 1, 1, 0.14) }
        Tool { label: "A-"; onClicked: settings.lyricsFontSize = dl.fs - 2 }
        Tool { label: "A+"; onClicked: settings.lyricsFontSize = dl.fs + 2 }
        // 配色小圆点
        Repeater {
            model: dl.presets
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 32; height: 32; radius: 16
                color: cm.containsMouse ? Qt.rgba(1, 1, 1, 0.12) : "transparent"
                Rectangle {
                    anchors.centerIn: parent
                    width: settings.lyricsColor === index ? 16 : 12
                    height: width
                    radius: width / 2
                    color: modelData.c
                    border.width: settings.lyricsColor === index ? 2 : 0
                    border.color: "#ffffff"
                    Behavior on width { NumberAnimation { duration: 120 } }
                }
                MouseArea { id: cm; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: settings.lyricsColor = index }
            }
        }
        Rectangle { width: 1; height: 16; anchors.verticalCenter: parent.verticalCenter; color: Qt.rgba(1, 1, 1, 0.14) }
        Tool { label: settings.lyricsTwoLine ? qsTr("双行") : qsTr("单行"); onClicked: settings.lyricsTwoLine = !settings.lyricsTwoLine }
        Tool { icon: "lock"; onClicked: settings.lyricsLocked = true }
        Tool { icon: "close"; onClicked: settings.desktopLyrics = false }
    }

    // ---------- 歌词 ----------
    // 一行:底层未唱文字 + 按进度裁剪的已唱文字;深色描边,任何桌面背景上都看得清
    component LyricLine: Item {
        id: ll
        property string text
        property var words: []          // 逐字时间轴(酷狗 KRC 才有):[{t:字, s:起始毫秒, d:时长毫秒}, ...]
        property real progress: 0
        property real size: dl.fs
        property bool active: false
        // 这句已经唱过了(在它之后切到了下一句):保持"整句已唱"的样子,
        // 否则切行瞬间整块高亮会突然消失,看起来像闪了一下
        property bool past: false
        property int align: Text.AlignHCenter
        height: Math.ceil(size * 1.35)
        readonly property real textW: Math.min(width, base.implicitWidth)
        readonly property real textX: align === Text.AlignLeft ? 0 : align === Text.AlignRight ? width - textW : (width - textW) / 2
        // 有逐字数据就按字渲染,否则退回整行按比例推进(普通 LRC 没有逐字时间)
        readonly property bool wordMode: words && words.length > 0
        readonly property int nowMs: Math.round(player.position * 1000)
        readonly property color plainColor: active ? "#f6f2ec" : Qt.rgba(0.96, 0.95, 0.93, 0.78)

        // ---------- 整行模式(没有逐字时间时) ----------
        Item {
            anchors.fill: parent
            visible: !ll.wordMode

            // 柔和投影:浅色桌面上也能看清白字
            Text {
                x: ll.textX + 1
                y: 2
                width: ll.textW
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                text: ll.text
                elide: Text.ElideRight
                color: Qt.rgba(0, 0, 0, 0.32)
                style: Text.Outline
                styleColor: Qt.rgba(0, 0, 0, 0.22)
                font: base.font
            }
            Text {
                id: base
                x: ll.textX
                width: ll.textW
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                text: ll.text
                elide: Text.ElideRight
                color: ll.plainColor
                style: Text.Outline
                styleColor: Qt.rgba(0, 0, 0, 0.7)
                font.family: Theme.fontFamily
                font.pixelSize: ll.size
                font.weight: Font.DemiBold
            }
            Item {
                x: ll.textX
                width: ll.textW * ll.progress
                height: parent.height
                clip: true
                visible: ll.active && ll.progress > 0
                Text {
                    width: ll.textW
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    text: ll.text
                    elide: Text.ElideRight
                    color: dl.sungColor
                    style: Text.Outline
                    styleColor: Qt.rgba(0, 0, 0, 0.7)
                    font: base.font
                }
            }
        }

        // ---------- 逐字模式(KRC) ----------
        // 不能用"每个字一个 Text":一行几十个字,每个都带 Outline(要渲染两层),
        // 颜色还要每帧重算 —— 实测非常卡(一行 30 字就是 60 层渲染 + 30 个绑定)。
        //
        // 改成"两个整行 Text + 按字边界裁剪":文字内容始终不变,
        // 每帧只改一个 clip 宽度,GPU 裁剪几乎零成本。
        // 字内按时间插值,所以是连续推进而不是一个字一个字地跳。
        Item {
            id: wm
            anchors.fill: parent
            visible: ll.wordMode

            // 测量层:只用来拿到每个字的宽度,不显示
            Row {
                id: measure
                visible: false
                spacing: 0
                Repeater {
                    model: ll.words
                    Text {
                        text: modelData.t
                        font.family: Theme.fontFamily
                        font.pixelSize: ll.size
                        font.weight: Font.DemiBold
                    }
                }
            }

            // 已唱部分的宽度(像素):唱完的字整段算,正在唱的字按时间比例插值
            readonly property real sungW: {
                if (!ll.active) return ll.past ? measure.implicitWidth : 0
                const ms = ll.nowMs
                let w = 0
                for (let i = 0; i < ll.words.length; ++i) {
                    const it = measure.children[i]
                    if (!it) break
                    const st = ll.words[i].s
                    const du = Math.max(1, ll.words[i].d)
                    if (ms >= st + du) { w += it.width; continue }
                    if (ms >= st) { w += it.width * (ms - st) / du; break }
                    break
                }
                return w
            }
            readonly property real rowW: measure.implicitWidth
            readonly property real rowX: ll.align === Text.AlignLeft ? ll.textX
                                       : ll.align === Text.AlignRight ? ll.width - rowW
                                       : (ll.width - rowW) / 2

            // 未唱层
            Text {
                x: wm.rowX
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                text: ll.text
                color: ll.plainColor
                style: Text.Outline
                styleColor: Qt.rgba(0, 0, 0, 0.7)
                font.family: Theme.fontFamily
                font.pixelSize: ll.size
                font.weight: Font.DemiBold
            }

            // 已唱层:裁到 sungW 宽
            Item {
                x: wm.rowX
                width: wm.sungW
                height: parent.height
                clip: true
                visible: wm.sungW > 0
                Text {
                    width: measure.implicitWidth
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    text: ll.text
                    color: dl.sungColor
                    style: Text.Outline
                    styleColor: Qt.rgba(0, 0, 0, 0.7)
                    font.family: Theme.fontFamily
                    font.pixelSize: ll.size
                    font.weight: Font.DemiBold
                }
            }
        }
    }

    // 一句:原文 + (有翻译时) 下方小一号的翻译,两者对齐方式相同
    component LyricBlock: Column {
        id: lb
        property int idx: -1
        property int align: Text.AlignHCenter
        width: parent ? parent.width : 0
        LyricLine {
            width: lb.width
            align: lb.align
            text: dl.lineText(lb.idx)
            words: dl.lineWords(lb.idx)
            active: lb.idx === dl.cur
            past: lb.idx < dl.cur
            progress: dl.progressOf(lb.idx)
        }
        LyricLine {
            visible: dl.anyTrans
            width: lb.width
            align: lb.align
            size: dl.transSize
            text: dl.transText(lb.idx)
            active: lb.idx === dl.cur
        }
    }

    Column {
        id: lyricCol
        x: 28
        width: parent.width - 56
        anchors.top: parent.top
        anchors.topMargin: dl.toolbarH
        spacing: Math.round(dl.fs * 0.12)

        // 没有可同步的歌词:显示歌名
        LyricLine {
            visible: !dl.synced
            width: parent.width
            text: dl.fallback
            active: true
        }
        // 单行:当前句居中 (有翻译时翻译在下)
        LyricBlock {
            visible: dl.synced && !settings.lyricsTwoLine
            idx: dl.cur
        }
        // 双行交替:偶数句在上靠左,奇数句在下靠右,另一行预告下一句;每句各带自己的翻译
        LyricBlock {
            visible: dl.synced && settings.lyricsTwoLine
            align: Text.AlignLeft
            idx: dl.topIdx
        }
        LyricBlock {
            visible: dl.synced && settings.lyricsTwoLine
            align: Text.AlignRight
            idx: dl.botIdx
        }
    }

    // ---------- 锁定后的解锁按钮 ----------
    Rectangle {
        id: unlockBtn
        anchors.horizontalCenter: parent.horizontalCenter
        y: 8
        width: unlockRow.implicitWidth + 24
        height: 30
        radius: 15
        color: dl.overUnlock ? Qt.rgba(0.06, 0.055, 0.05, 0.85) : Qt.rgba(0.06, 0.055, 0.05, 0.6)
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.12)
        opacity: dl.locked && dl.hoverLocked ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 160 } }
        Row {
            id: unlockRow
            anchors.centerIn: parent
            spacing: 6
            Icon { anchors.verticalCenter: parent.verticalCenter; name: "unlock"; size: 13 }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("解锁")
                color: "#f3efe9"
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
        }
        MouseArea {
            anchors.fill: parent
            enabled: dl.locked
            cursorShape: Qt.PointingHandCursor
            onClicked: { settings.lyricsLocked = false; dl.overUnlock = false }
        }
    }
}
