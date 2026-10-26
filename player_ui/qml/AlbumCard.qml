import QtQuick
import PlayerUi

// 专辑卡片:悬停封面上浮放大 + 浮出播放键与「更多」;正在播放的专辑显示律动角标,暂停时播放键常驻
// 右键 / 「更多」打开整张专辑的菜单
Item {
    id: card
    property string title
    property string artist
    property string detail   // 歌手后面的附加信息 (按年份 / 歌手排序时显示年份)
    property string badge
    property string cover
    property int albumIndex: -1
    property bool isPlaying: player.albumIndex === albumIndex && player.hasTrack
    readonly property bool active: isPlaying && player.playing
    readonly property alias coverItem: coverBox
    readonly property bool compact: width < 150
    signal open()
    signal play()

    readonly property bool hovered: mouse.containsMouse || (controls.item !== null && controls.item.hot)

    // 格子被复用来显示另一张专辑时调用:上一张遗留的按钮直接撤掉,不在新专辑上播放淡出
    function settle() { linger.stop() }

    function openMenu(item, px, py) {
        const p = item.mapToItem(null, px, py)
        Nav.trackMenu(-1, p.x, p.y, { albumIndex: card.albumIndex })
    }

    // 投影:悬停时加深并下沉,配合封面上浮形成「拿起」的纵深感
    SoftShadow {
        target: coverBox
        spread: 22
        offsetY: card.hovered ? 20 : 12
        strength: card.hovered ? 1.0 : 0.62
        Behavior on strength { NumberAnimation { duration: Theme.normal } }
        Behavior on offsetY { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
    }

    Item {
        id: coverBox
        width: parent.width
        height: width
        y: card.hovered ? -6 : 0
        scale: card.hovered ? 1.02 : 1
        Behavior on y { YAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
        Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }

        RoundedImage {
            anchors.fill: parent
            radius: 10
            source: card.cover
            // 按**实际物理像素**解码:格子宽 × dpr。原来固定 1.5 倍,在高 DPI 屏上
            // 要么解码过大(浪费上传带宽,全屏 4K 滚动时会卡),要么在高缩放屏上不够清晰。
            // 上限 1.5 是给悬停放大 (1.02) 和飞行动画留的余量。
            readonly property real pxScale: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
            decodeSize: Math.ceil(card.width * Math.max(1.0, Math.min(1.5, pxScale)))
        }

        // 播放键与「更多」只在需要时创建:专辑墙的格子滚出视野就会被销毁、滚进来重新创建 (GridView 不复用格子),
        // 每张卡片少建这两组控件,滚动时逐行创建卡片的开销小很多。收起时多留一会儿,让淡出动画放完
        readonly property bool controlsWanted: card.hovered || (card.isPlaying && !player.playing)
        onControlsWantedChanged: if (!controlsWanted) linger.restart()
        Timer { id: linger; interval: Theme.normal + 60 }
        Loader {
            id: controls
            anchors.fill: parent
            active: coverBox.controlsWanted || linger.running
            sourceComponent: Item {
                readonly property bool hot: playMouse.containsMouse || moreMouse.containsMouse
                // 创建出来的第一帧先是隐藏状态,随后切到目标状态,淡入 / 弹出动画才会播放
                property bool ready: false
                Component.onCompleted: ready = true

                // 播放 / 暂停:正在播放这张时切换暂停,暂停中的这张不悬停也露出继续键
                Rectangle {
                    id: playBtn
                    readonly property bool shown: parent.ready && (card.hovered || (card.isPlaying && !player.playing))
                    width: card.compact ? 38 : 46
                    height: width
                    radius: width / 2
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: card.compact ? 8 : 12
                    color: Theme.accent
                    opacity: shown ? (card.hovered ? 1 : 0.9) : 0
                    scale: shown ? (playMouse.pressed ? 0.92 : 1) : 0.7
                    Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
                    Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutBack } }
                    Icon {
                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset: card.active ? 0 : 1
                        name: (card.active ? "pause" : "play") + (Theme.accentIsLight ? "-dark" : "")
                        size: card.compact ? 17 : 20
                    }
                    MouseArea {
                        id: playMouse
                        anchors.fill: parent
                        enabled: playBtn.shown
                        hoverEnabled: !Theme.scrolling
                        cursorShape: Qt.PointingHandCursor
                        onClicked: card.isPlaying ? player.toggle() : card.play()
                    }
                }

                // 更多:整张专辑的菜单 (下一首播放、加入歌单、收藏全部…)
                Rectangle {
                    id: moreBtn
                    readonly property bool shown: parent.ready && card.hovered
                    width: 30; height: 30; radius: 15
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 10
                    color: moreMouse.containsMouse ? Qt.rgba(0, 0, 0, 0.62) : Qt.rgba(0, 0, 0, 0.45)
                    opacity: shown ? 1 : 0
                    scale: shown ? 1 : 0.7
                    visible: opacity > 0
                    Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
                    Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutBack } }
                    Icon { anchors.centerIn: parent; name: "more"; size: 16 }
                    MouseArea {
                        id: moreMouse
                        anchors.fill: parent
                        hoverEnabled: !Theme.scrolling
                        cursorShape: Qt.PointingHandCursor
                        onClicked: card.openMenu(moreBtn, 0, moreBtn.height + 6)
                    }
                }
            }
        }

        // 正在播放角标:只有正在播放的那张卡片才创建
        Loader {
            active: card.isPlaying
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 10
            sourceComponent: Rectangle {
                width: 30; height: 24; radius: 6
                color: Qt.rgba(0, 0, 0, 0.55)
                PlayingBars { anchors.centerIn: parent; maxHeight: 12 }
            }
        }
    }

    Column {
        y: coverBox.width + 14
        width: parent.width
        spacing: 3
        Text {
            width: parent.width
            text: card.title
            elide: Text.ElideRight
            color: card.isPlaying ? Theme.accent : Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: card.compact ? 13 : 14
            font.weight: Font.DemiBold
        }
        Row {
            width: parent.width
            spacing: 6
            Text {
                width: Math.min(implicitWidth, parent.width - (badgeItem.visible ? badgeItem.width + 6 : 0))
                text: card.detail.length ? card.artist + "  ·  " + card.detail : card.artist
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
            }
            QualityBadge {
                id: badgeItem
                text: card.compact ? "" : card.badge
                fontSize: 9
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    MouseArea {
        id: mouse
        width: coverBox.width
        height: coverBox.height + 52
        // 滚动时卡片从鼠标下掠过,不逐张触发上浮与投影动画
        hoverEnabled: !Theme.scrolling
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (e) => {
            if (e.button === Qt.RightButton) card.openMenu(mouse, e.x, e.y)
            else card.open()
        }
        z: -1
    }
}
