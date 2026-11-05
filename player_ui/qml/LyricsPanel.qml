import QtQuick
import PlayerUi

// 歌词补全面板 —— 从专辑墙横幅的「歌词」按钮弹出
//
// 和封面面板的区别:歌词是按**曲目**算的,一首一份,所以这里是列表不是网格。
// 列表只列「还缺歌词」的曲目 —— 曲库里几百首,把已经齐的也列出来只是噪音。
//
// 视觉上刻意压得很低:一屏只有三种颜色(正文 / 次要 / 极淡),强调色只用在
// 进度条、完成勾和主按钮上。运行中把列表整体压暗,让进度成为唯一焦点。
Item {
    id: root
    property bool shown: false
    property real anchorRight: width - 40
    property real anchorBottom: height - 40
    anchors.fill: parent
    visible: pop.opacity > 0
    z: 6

    property var tracks: []
    property int bottomInset: 140
    // 补完后把标题临时换成「已补 N 首」,三秒后回到「缺 N 首」
    property bool showDone: false
    readonly property bool busy: lyrics.busy

    function reload() {
        const next = lyrics.trackStates()
        if (JSON.stringify(next) === JSON.stringify(tracks)) return
        // 补完几百首时列表会从几百项一次缩到几项。ListView 一次性销毁几百个 delegate,
        // 那些 delegate 的入场动画还在跑 —— 先清空、下一帧再赋值,让它走干净的移除路径。
        if (Math.abs(next.length - tracks.length) > 16) {
            tracks = []
            Qt.callLater(function () { tracks = next })
        } else {
            tracks = next
        }
    }
    function openAt(item) {
        if (item) {
            const p = item.mapToItem(root, 0, item.height)
            anchorRight = p.x + item.width
            anchorBottom = p.y + 12
        }
        reload()
        shown = true
    }
    function close() { shown = false }
    function run() { if (busy) lyrics.cancel(); else lyrics.fetchTracks([]) }

    // 压力测试:造 400 条假数据撑满列表,再 reload 回真实(几十条) —— 复现「补完几百首后
    // 列表一次缩掉一大截」的重建路径。不动曲库。
    function automationStress() {
        const big = []
        for (let i = 0; i < 400; ++i)
            big.push({ id: -1 - i, title: "Stress Track " + i, artist: "Stress Artist", album: "", hasLrc: false })
        tracks = big
        Qt.callLater(function () { Qt.callLater(function () { root.reload() }) })
    }

    Timer {
        id: doneTimer
        interval: 3200
        onTriggered: root.showDone = false
    }

    // 补完不用重扫曲库(歌词不进快照),只需要重新取一次状态
    Connections {
        target: lyrics
        // 让当前这一帧先渲染完,再重建列表
        function onFinished(ok, failed) {
            Qt.callLater(root.reload)
            if (ok > 0) { root.showDone = true; doneTimer.restart() }
        }
    }
    Connections {
        target: library
        function onLibraryChanged() { if (root.shown && !root.busy) root.reload() }
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.shown
        acceptedButtons: Qt.AllButtons
        onPressed: root.close()
        onWheel: (w) => w.accepted = true
    }

    Item {
        id: pop
        readonly property int pad: 18
        readonly property int headH: root.busy ? 84 : 46
        readonly property int footH: 58
        readonly property int rowH: 48
        readonly property int maxRows: 8
        width: Math.min(520, root.width - 40)
        readonly property int rows: Math.min(maxRows, Math.max(1, root.tracks.length))
        readonly property real contentH: headH + rows * rowH + footH + 10
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

            // ---- 标题:图标 + 一句状态 ----
            Icon {
                id: heading
                x: pop.pad
                y: 16
                name: "lyrics"
                size: 18
                opacity: 0.9
            }
            Text {
                x: pop.pad + 26
                y: 15
                text: root.showDone
                      ? qsTr("已补 %1 首").arg(lyrics.okCount)
                      : (root.busy ? qsTr("正在补全")
                         : (root.tracks.length > 0 ? qsTr("缺 %1 首").arg(root.tracks.length) : qsTr("歌词已齐")))
                color: root.showDone ? Theme.accent : Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.Medium
                opacity: 0.92
                Behavior on color { ColorAnimation { duration: Theme.normal } }
            }

            // ---- 运行中的进度信息 ----
            Item {
                id: progressInfo
                x: pop.pad
                y: 44
                width: card.width - pop.pad * 2
                height: 34
                opacity: root.busy ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                transform: Translate {
                    y: root.busy ? 0 : -6
                    Behavior on y { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                }

                Text {
                    id: curTitle
                    anchors.left: parent.left
                    anchors.right: counter.left
                    anchors.rightMargin: 10
                    anchors.top: parent.top
                    height: 16
                    text: lyrics.currentTitle
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    elide: Text.ElideMiddle
                    opacity: 0.85
                    onTextChanged: if (text.length) titlePulse.restart()
                    SequentialAnimation {
                        id: titlePulse
                        NumberAnimation { target: curTitle; property: "opacity"; to: 0.3; duration: 80 }
                        NumberAnimation { target: curTitle; property: "opacity"; to: 0.85; duration: 220; easing.type: Easing.OutCubic }
                    }
                }
                Text {
                    id: counter
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 16
                    text: {
                        let s = lyrics.done + " / " + lyrics.total
                        const eta = lyrics.etaSec
                        if (eta >= 0) s += "  ·  " + (eta >= 60 ? Math.ceil(eta / 60) + "分" : eta + "秒")
                        if (lyrics.threadCount > 1) s += "  ·  " + lyrics.threadCount + "线程"
                        return s
                    }
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    opacity: 0.5
                }

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
                        // 光效从 -46 扫到 width，不裁的话会跑到轨道外面去
                        clip: true
                        width: parent.width * Math.max(0, Math.min(1, lyrics.progress))
                        Behavior on width { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }

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

            // ---- 列表 ----
            ListView {
                id: list
                x: pop.pad
                y: pop.headH
                width: card.width - pop.pad * 2
                height: Math.min(card.height - pop.headH - pop.footH, pop.rows * pop.rowH)
                clip: true
                model: root.tracks
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: true
                // 惯性收得干净一点:默认 1500 会拖很久,2600 上限让快速甩动不至于飞太远
                flickDeceleration: 1900
                maximumFlickVelocity: 2600
                // 运行中压暗,让进度成为焦点;结束时恢复
                opacity: root.busy ? 0.35 : 1
                Behavior on opacity { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                // 行与行之间一条极淡的线,不加也能看,加了更像"清单"
                spacing: 0

                delegate: Item {
                    id: rowItem
                    width: list.width
                    height: pop.rowH
                    opacity: 1
                    Component.onCompleted: { rowItem.opacity = 0; enterAnim.start() }
                    SequentialAnimation {
                        id: enterAnim
                        PauseAnimation { duration: Math.min(index, 14) * 18 }
                        NumberAnimation {
                            target: rowItem; property: "opacity"; to: 1
                            duration: 320; easing.type: Easing.OutQuint
                        }
                    }

                    readonly property string title: modelData.title || ""
                    readonly property string artist: modelData.artist || ""
                    readonly property string album: modelData.album || ""
                    readonly property int durMs: modelData.duration_ms || 0
                    readonly property string durText: {
                        if (durMs <= 0) return ""
                        const t = Math.round(durMs / 1000)
                        return Math.floor(t / 60) + ":" + ("0" + (t % 60)).slice(-2)
                    }
                    // 副标题:歌手 · 专辑。两者相同(单曲)时不重复写
                    readonly property string sub: {
                        const a = artist.trim(), b = album.trim()
                        if (a && b && a !== b) return a + " · " + b
                        return a || b
                    }

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: Theme.radiusS
                        color: Qt.rgba(1, 1, 1, hover.hovered ? 0.035 : 0)
                        Behavior on color { ColorAnimation { duration: Theme.fast } }
                    }

                    Text {
                        id: idx
                        width: 22
                        anchors.verticalCenter: parent.verticalCenter
                        horizontalAlignment: Text.AlignRight
                        text: (index + 1)
                        color: Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        opacity: 0.38
                    }

                    Column {
                        anchors.left: idx.right
                        anchors.leftMargin: 12
                        anchors.right: dur.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3

                        Text {
                            width: parent.width
                            text: rowItem.title
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            elide: Text.ElideRight
                            opacity: 0.92
                        }
                        Text {
                            width: parent.width
                            text: rowItem.sub
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            opacity: 0.5
                            visible: text.length > 0
                        }
                    }

                    Text {
                        id: dur
                        anchors.right: dot.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: rowItem.durText
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        opacity: 0.35
                    }

                    // 状态:一个空心小圆,悬停时放大一点
                    Rectangle {
                        id: dot
                        width: 6; height: 6; radius: 3
                        anchors.right: parent.right
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        color: "transparent"
                        border.width: 1
                        border.color: Qt.rgba(1, 1, 1, hover.hovered ? 0.55 : 0.2)
                        scale: hover.hovered ? 1.35 : 1
                        Behavior on border.color { ColorAnimation { duration: Theme.fast } }
                        Behavior on scale { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                    }

                    HoverHandler { id: hover; enabled: !root.busy }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: Theme.hairline
                        visible: index < root.tracks.length - 1
                    }
                }
            }

            // 平滑滚轮:一格 120px 缓出,连续滚动目标累加(不会一顿一顿)
            SmoothScroll { flickable: list }

            // 全都有歌词:居中一个勾
            Icon {
                anchors.centerIn: list
                name: "check"
                size: 34
                opacity: root.tracks.length === 0 && !root.busy ? 0.18 : 0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: Theme.normal } }
            }

            // ---- 底部 ----
            Rectangle {
                id: foot
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: pop.footH
                color: "transparent"
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 1
                    color: Theme.hairline
                    visible: list.height > 0
                }

                Capsule {
                    id: errCap
                    anchors.left: parent.left
                    anchors.leftMargin: pop.pad
                    anchors.verticalCenter: parent.verticalCenter
                    visible: lyrics.failures.length > 0 && !root.busy
                    // 加「需核对」三个字:失败不等于出错,很多是纯音乐或标题特殊,得人工看一眼
                    text: qsTr("失败 %1 · 需核对").arg(lyrics.failures.length)
                    onClicked: {
                        const f = lyrics.failures
                        let s = ""
                        for (let i = 0; i < f.length && i < 20; ++i) s += f[i].title + "  —  " + f[i].reason + "\n"
                        Nav.toast("info", qsTr("没找到的 %1 首多半是纯音乐,或者标题格式太特殊(超长、含 feat./括号堆叠)。"
                                               + "这些没法靠自动匹配,可以点开详情页手动搜一下,自己挑一条对的。")
                                               .arg(f.length))
                        console.log(s)
                    }
                }

                // 常驻的一行小字,不用点也能看到
                Text {
                    anchors.left: errCap.right
                    anchors.leftMargin: 10
                    anchors.right: allCap.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    visible: errCap.visible
                    text: qsTr("多半是纯音乐或标题特殊,需手动比对")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    opacity: 0.42
                    elide: Text.ElideRight
                }

                Capsule {
                    id: allCap
                    anchors.right: parent.right
                    anchors.rightMargin: pop.pad
                    anchors.verticalCenter: parent.verticalCenter
                    accent: true
                    text: root.busy ? qsTr("取消") : qsTr("全部补全")
                    onClicked: root.run()
                }
            }
        }
    }
}
