import QtQuick
import PlayerUi

// 播放队列抽屉:从右侧滑出,停在播放条上方。标题下写曲目数与剩余时长,右上角「定位」与「清空」(可撤销);
// 每行封面 · 歌名 / 歌手 · 时长,正在播放的一行用强调色;悬停出现拖动把手与移除按钮,单击播放
Item {
    id: panel
    property bool shown: false
    signal closeRequested()

    visible: slide.x < width
    clip: false

    // 点面板外关闭 (不拦截播放条:遮罩从窗口顶端一直盖到面板底边,也就是播放条上沿)
    MouseArea {
        parent: panel.parent
        x: 0; y: 0
        width: panel.x
        height: panel.y + panel.height
        z: panel.z - 1
        enabled: panel.shown
        visible: panel.shown
        onClicked: panel.closeRequested()
        onWheel: (w) => w.accepted = false
    }

    // 正在播放的那一行在不在视野里
    readonly property real rowStep: 64
    readonly property bool currentVisible: {
        const y = list.originY + player.queuePosition * rowStep
        return player.queuePosition < 0 || (y >= list.contentY - 8 && y + rowStep - 2 <= list.contentY + list.height + 8)
    }
    function locate(animated) {
        if (player.queuePosition < 0) return
        // 上方留一行已播放的作参照
        const maxY = Math.max(list.originY, list.originY + list.contentHeight - list.height)
        const t = Math.max(list.originY, Math.min(maxY, list.originY + Math.max(0, player.queuePosition - 1) * rowStep))
        if (!animated || Theme.reduced) { list.contentY = t; return }
        locateAnim.to = t
        locateAnim.restart()
    }
    NumberAnimation { id: locateAnim; target: list; property: "contentY"; duration: 520; easing.type: Easing.OutCubic }
    function clearWithUndo() {
        const n = player.queueLength - 1
        player.clearQueue()
        Nav.toastAction(qsTr("已清空播放队列 (%1 首)").arg(n.toLocaleString(Qt.locale(), "f", 0)), qsTr("撤销"), () => {
            if (!player.undoClearQueue()) Nav.toast("queue", qsTr("无法撤销:正在播放的歌已不在原队列中"))
        })
    }
    // 截图验证用:滚到第 row 行附近;在第 row 行打开右键菜单
    function automationScroll(row) { list.positionViewAtIndex(Number(row), ListView.Beginning) }
    function automationMenu(row) {
        list.positionViewAtIndex(Number(row), ListView.Center)
        const it = list.itemAtIndex(Number(row))
        if (it) it.openMenu(it.width * 0.5, it.height * 0.5)
    }

    Item {
        id: slide
        width: parent.width
        height: parent.height
        x: panel.shown ? 0 : width + 40
        Behavior on x { NumberAnimation { duration: panel.shown ? 460 : 300; easing.type: panel.shown ? Easing.OutQuint : Easing.InCubic } }

        SoftShadow {
            target: sheet
            spread: 40
            offsetY: 0
            strength: 0.85
        }
        Rectangle {
            id: sheet
            // 右边与底边伸出窗口 / 藏到播放条下面,只露出左上角的圆角
            anchors.fill: parent
            anchors.rightMargin: -20
            anchors.bottomMargin: -20
            radius: 16
            color: Qt.rgba(0.082, 0.075, 0.068, 0.985)
            border.width: 1
            border.color: Theme.hairline
            MouseArea { anchors.fill: parent; onWheel: (w) => w.accepted = false }
        }

        // ---- 标题 ----
        Column {
            id: head
            x: 24
            y: 20
            spacing: 3
            Text {
                text: qsTr("播放队列")
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 20
                font.weight: Font.Bold
            }
            Text {
                visible: player.queueLength > 0
                text: qsTr("%1 首 · 剩余 %2").arg(player.queueLength.toLocaleString(Qt.locale(), "f", 0)).arg(player.queueRemaining)
                color: Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 16
            y: 22
            spacing: 4
            // 定位:正在播放的那首滚出视野时出现
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                width: 30; height: 30
                icon: "locate"
                iconSize: 15
                tip: qsTr("定位到正在播放")
                opacity: !panel.currentVisible && player.queueLength > 0 ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                onClicked: panel.locate(true)
            }
            // 清空:只保留正在播放的这一首;提示里可以撤销
            Rectangle {
                id: clearBtn
                anchors.verticalCenter: parent.verticalCenter
                height: 30
                width: clearRow.implicitWidth + 20
                radius: 15
                color: clearMouse.containsMouse ? Theme.hover : "transparent"
                opacity: player.queueLength > 1 ? 1 : 0.35
                Row {
                    id: clearRow
                    anchors.centerIn: parent
                    spacing: 6
                    Icon { anchors.verticalCenter: parent.verticalCenter; name: "trash"; size: 14; opacity: 0.7 }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("清空")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                    }
                }
                MouseArea {
                    id: clearMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: player.queueLength > 1
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.clearWithUndo()
                }
            }
        }

        ListView {
            id: list
            anchors.top: head.bottom
            anchors.topMargin: 14
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            x: 6
            width: parent.width - 16
            clip: true
            model: player.queueModel
            spacing: 2
            cacheBuffer: 600
            reuseItems: true
            boundsBehavior: Flickable.StopAtBounds
            // 拖动排序时被拖的行设为当前项,滚出视野也不会被回收
            currentIndex: -1
            highlightFollowsCurrentItem: false
            keyNavigationEnabled: false
            SmoothScroll { flickable: list }
            // 打开时把正在播放的那首放在视野里 (上方留一行已播放的作参照)
            Connections {
                target: panel
                function onShownChanged() { if (panel.shown) list.positionViewAtIndex(Math.max(0, player.queuePosition - 1), ListView.Beginning) }
            }
            // 新增 / 移除 / 挪动的行轻轻滑动、淡入淡出,而不是瞬间跳位
            displaced: Transition { NumberAnimation { property: "y"; duration: 280; easing.type: Easing.OutCubic } }
            add: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.normal } }
            remove: Transition { NumberAnimation { property: "opacity"; to: 0; duration: Theme.fast } }

            // ---- 拖动排序 (与歌单页同一做法):悬停时行左侧出现把手,按住上下拖,强调色细线标出落点 ----
            property int dragFrom: -1
            property int dropAt: -1
            property int edgeDir: 0
            property real edgeDepth: 0
            property real dragY: 0
            function updateDrop() {
                let i = indexAt(40, contentY + dragY)
                if (i < 0) i = contentY + dragY < originY ? 0 : count - 1
                dropAt = i
            }
            function endDrag() {
                dragFrom = -1
                dropAt = -1
                edgeDir = 0
                currentIndex = -1
                ghost.visible = false
            }
            Timer {
                interval: 16
                repeat: true
                running: list.dragFrom >= 0 && list.edgeDir !== 0
                onTriggered: {
                    const step = 3 + 12 * Math.min(1, list.edgeDepth / 48)
                    const maxY = Math.max(list.originY, list.originY + list.contentHeight - list.height)
                    list.contentY = Math.max(list.originY, Math.min(maxY, list.contentY + list.edgeDir * step))
                    list.updateDrop()
                }
            }

            delegate: Item {
                id: row
                width: list.width
                height: 62
                readonly property bool current: model.queueIndex === player.queuePosition
                readonly property bool played: model.queueIndex < player.queuePosition
                readonly property bool hovered: (rowMouse.containsMouse || rm.hovered || grip.containsMouse) && list.dragFrom < 0
                readonly property bool dragged: list.dragFrom === index
                function openMenu(px, py) {
                    const p = row.mapToItem(null, px, py)
                    Nav.trackMenu(model.trackId, p.x, p.y, { queueIndex: model.queueIndex })
                }
                opacity: dragged ? 0.35 : 1
                // 抽屉打开时,可见的行自上而下依次滑入淡入
                transform: Translate { id: rowShift }
                Connections {
                    target: panel
                    function onShownChanged() {
                        if (!panel.shown || Theme.reduced) return
                        const rel = Math.max(0, Math.min(10, Math.round((row.y - list.contentY) / row.height)))
                        rowIn.delay = 60 + rel * 28
                        rowIn.restart()
                    }
                }
                SequentialAnimation {
                    id: rowIn
                    property int delay: 0
                    ScriptAction { script: { row.opacity = 0; rowShift.x = 28 } }
                    PauseAnimation { duration: rowIn.delay }
                    ParallelAnimation {
                        NumberAnimation { target: row; property: "opacity"; to: 1; duration: 320; easing.type: Easing.OutCubic }
                        NumberAnimation { target: rowShift; property: "x"; to: 0; duration: 520; easing.type: Easing.OutQuint }
                    }
                    // 动画写过 opacity,结束后恢复绑定 (拖动时的半透明)
                    ScriptAction { script: row.opacity = Qt.binding(() => row.dragged ? 0.35 : 1) }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusM
                    color: Theme.accentAlpha(0.10)
                    visible: row.current
                }
                // 悬停底色:透明度渐变 (不逐帧插值颜色)
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusM
                    color: Theme.hover
                    opacity: row.hovered && !row.current ? 1 : 0
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
                        if (e.button === Qt.RightButton) row.openMenu(e.x, e.y)
                        else if (!row.current) player.playQueueIndex(model.queueIndex)
                    }
                }
                // 拖动把手
                Icon {
                    x: 3
                    anchors.verticalCenter: parent.verticalCenter
                    name: "grip"
                    size: 14
                    opacity: row.hovered || grip.pressed ? 0.6 : 0
                    Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                }
                MouseArea {
                    id: grip
                    width: 22
                    height: parent.height
                    hoverEnabled: !Theme.scrolling
                    cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                    preventStealing: true
                    onPressed: (e) => {
                        list.currentIndex = index
                        list.dragFrom = index
                        list.dropAt = index
                        ghost.title = model.title
                        ghost.cover = model.cover
                        move(e)
                    }
                    onPositionChanged: (e) => { if (pressed) move(e) }
                    onReleased: {
                        const from = list.dragFrom, to = list.dropAt
                        list.endDrag()
                        if (from >= 0 && to >= 0 && to !== from) player.moveInQueue(from, to)
                    }
                    onCanceled: list.endDrag()
                    function move(e) {
                        const lv = mapToItem(list, e.x, e.y)
                        list.dragY = lv.y
                        list.updateDrop()
                        const v = mapToItem(slide, e.x, e.y)
                        ghost.y = v.y - ghost.height / 2
                        ghost.visible = true
                        if (lv.y < 40) { list.edgeDir = -1; list.edgeDepth = 40 - lv.y }
                        else if (lv.y > list.height - 40) { list.edgeDir = 1; list.edgeDepth = lv.y - list.height + 40 }
                        else list.edgeDir = 0
                    }
                }
                RoundedImage {
                    id: qc
                    x: 22
                    anchors.verticalCenter: parent.verticalCenter
                    width: 44; height: 44
                    radius: 7
                    source: model.cover
                    decodeSize: 88
                    opacity: row.played ? 0.55 : 1
                }
                Loader {
                    anchors.centerIn: qc
                    active: row.current
                    sourceComponent: Rectangle {
                        width: 44; height: 44; radius: 7
                        color: Qt.rgba(0, 0, 0, 0.5)
                        PlayingBars { anchors.centerIn: parent; maxHeight: 14 }
                    }
                }
                Column {
                    anchors.left: qc.right
                    anchors.leftMargin: 14
                    anchors.right: dur.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    Text {
                        width: parent.width
                        text: model.title
                        elide: Text.ElideRight
                        color: row.current ? Theme.accent : (row.played ? Theme.textSecondary : Theme.text)
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: row.current ? Font.DemiBold : Font.Normal
                    }
                    Text {
                        width: parent.width
                        text: model.artist
                        elide: Text.ElideRight
                        color: row.current ? Theme.accentAlpha(0.75) : Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }
                }
                Text {
                    id: dur
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: model.duration
                    opacity: row.hovered && !row.current ? 0 : 1
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                }
                IconButton {
                    id: rm
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    width: 30; height: 30
                    icon: "close"
                    iconSize: 12
                    tip: qsTr("从播放队列移除")
                    enabled: !row.current
                    opacity: row.hovered && !row.current ? 1 : 0
                    Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                    onClicked: player.removeFromQueue(model.queueIndex)
                }
                // 落点指示线
                Rectangle {
                    visible: list.dragFrom >= 0 && list.dropAt === index && list.dropAt !== list.dragFrom
                    x: 16
                    width: parent.width - 32
                    height: 2
                    radius: 1
                    y: list.dropAt > list.dragFrom ? parent.height : -2
                    color: Theme.accent
                }
            }
        }

        // 拖动中的行:跟随指针的半透明浮层
        Rectangle {
            id: ghost
            property string title
            property string cover
            visible: false
            x: list.x + 12
            width: list.width - 24
            height: 48
            radius: 10
            color: Theme.panel
            border.width: 1
            border.color: Theme.accentAlpha(0.45)
            z: 5
            RoundedImage {
                x: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 32; height: 32
                radius: 5
                source: ghost.cover
                decodeSize: 64
            }
            Text {
                x: 50
                width: parent.width - 60
                anchors.verticalCenter: parent.verticalCenter
                text: ghost.title
                elide: Text.ElideRight
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 13
            }
        }

        // 空队列
        Column {
            anchors.centerIn: list
            visible: player.queueLength === 0
            spacing: 12
            Icon { anchors.horizontalCenter: parent.horizontalCenter; name: "queue"; size: 32; opacity: 0.2 }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("播放队列是空的")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 14
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("在歌曲上右键 → 下一首播放")
                color: Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
        }
    }
}
