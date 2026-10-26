import QtQuick
import PlayerUi

// 侧边导航:不再是一条深色竖栏,而是一枚悬浮的竖向胶囊 (与播放岛、设置页同一套胶囊语言)
//  · 五个主分区;选中项是一枚柔和的强调色圆底,切换时像液滴一样先伸长、再收拢到新位置
//  · 主分区之后紧跟:转换 · 音乐文件夹 · 设置
//  · 悬停只提亮图标并给一层极淡的圆底;停留片刻在右侧浮出名称
Item {
    id: nav
    property string current: "albums"
    signal navigate(string target)
    width: Theme.navWidth

    readonly property var items: [
        { key: "albums", icon: "albums", tip: qsTr("专辑") },
        { key: "burst", icon: "disc", tip: qsTr("封面墙（测试）") },
        { key: "artists", icon: "artist", tip: qsTr("歌手") },
        { key: "songs", icon: "note", tip: qsTr("歌曲") },
        { key: "playlists", icon: "playlist", tip: qsTr("歌单") },
        { key: "search", icon: "search", tip: qsTr("搜索  Ctrl+K") },
        { key: "tools", icon: "tools", tip: qsTr("转换") },
        { key: "folder", icon: "folder", tip: qsTr("音乐文件夹") },
        { key: "settings", icon: "settings", tip: qsTr("设置") }
    ]
    readonly property int itemSize: 44
    readonly property int itemGap: 6
    readonly property int capPad: 5
    readonly property int capW: itemSize + capPad * 2
    function indexIn(list) {
        for (let i = 0; i < list.length; ++i) if (list[i].key === current) return i
        return -1
    }
    readonly property int currentIndex: indexIn(items)

    // 一枚竖向胶囊:极淡暖色底 + 发丝描边,内含若干图标与滑动的选中圆底
    component Dock: Rectangle {
        id: dock
        property var model: []
        property int sel: -1
        property real topEdge: 0
        property real bottomEdge: nav.itemSize
        property int prev: -1
        width: nav.capW
        height: model.length * (nav.itemSize + nav.itemGap) - nav.itemGap + nav.capPad * 2
        radius: width / 2
        color: Qt.rgba(1, 0.96, 0.9, 0.035)
        border.width: 1
        border.color: Theme.hairline

        function slotY(i) { return nav.capPad + i * (nav.itemSize + nav.itemGap) }
        // 液滴式滑动:前缘先到、后缘稍慢跟上,中途拉长,到位后收成圆
        function moveTo(i) {
            if (i < 0) return
            const y = slotY(i)
            if (prev < 0 || Theme.reduced) {
                lead.stop()
                topEdge = y; bottomEdge = y + nav.itemSize
            } else {
                const down = i > prev
                topAnim.duration = down ? 460 : 260
                bottomAnim.duration = down ? 260 : 460
                topAnim.to = y
                bottomAnim.to = y + nav.itemSize
                lead.restart()
            }
            prev = i
        }
        onSelChanged: moveTo(sel)
        Component.onCompleted: moveTo(sel)
        ParallelAnimation {
            id: lead
            NumberAnimation { id: topAnim; target: dock; property: "topEdge"; easing.type: Easing.OutQuint }
            NumberAnimation { id: bottomAnim; target: dock; property: "bottomEdge"; easing.type: Easing.OutQuint }
        }

        Rectangle {
            visible: dock.sel >= 0
            opacity: dock.sel >= 0 ? 1 : 0
            x: nav.capPad
            y: dock.topEdge
            width: nav.itemSize
            height: dock.bottomEdge - dock.topEdge
            radius: width / 2
            color: Theme.accentAlpha(0.17)
        }
        Column {
            x: nav.capPad
            y: nav.capPad
            spacing: nav.itemGap
            Repeater {
                model: dock.model
                delegate: navButton
            }
        }
    }

    Dock {
        id: mainDock
        x: (nav.width - width) / 2
        y: 52
        model: nav.items
        sel: nav.currentIndex
    }


    Component {
        id: navButton
        Item {
            id: btn
            width: nav.itemSize
            height: nav.itemSize
            readonly property bool active: nav.current === modelData.key
            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: Theme.hover
                opacity: m.containsMouse && !btn.active ? 1 : 0
                Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
            }
            Icon {
                anchors.centerIn: parent
                name: modelData.icon
                size: 20
                tint: btn.active ? Theme.accent : "transparent"
                opacity: btn.active ? 1 : (m.containsMouse ? 0.9 : 0.46)
                scale: m.pressed ? 0.86 : 1
                Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
                Behavior on scale { ScaleAnimator { duration: 220; easing.type: Easing.OutBack } }
            }
            // 曲库里有待转换的文件:转换图标右上角一个缓慢呼吸的小点
            Rectangle {
                x: 29; y: 9
                width: 6; height: 6; radius: 3
                color: Theme.accent
                visible: modelData.key === "tools" && decrypt.policy !== 2 && (decrypt.pending.total || 0) > 0 && nav.current !== "tools"
                SequentialAnimation on opacity {
                    running: parent.visible && !Theme.reduced
                    // 无限动画会让整个窗口一直按刷新率重画:滚动中、窗口不在前台时都停下
                    paused: running && (Theme.scrolling || Qt.application.state !== Qt.ApplicationActive)
                    loops: Animation.Infinite
                    OpacityAnimator { from: 1; to: 0.35; duration: 1400; easing.type: Easing.InOutSine }
                    OpacityAnimator { from: 0.35; to: 1; duration: 1400; easing.type: Easing.InOutSine }
                }
            }
            MouseArea {
                id: m
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                // 弹出类 (音乐文件夹) 按下就响应,不等松开
                onClicked: if (modelData.key !== "folder") nav.navigate(modelData.key)
                // 名称提示:与图标按钮同一个提示框,浮在图标右侧
                onContainsMouseChanged: containsMouse ? Nav.hintRight(btn, modelData.tip) : Nav.unhint(btn)
                onPressed: {
                    Nav.unhint(btn)
                    if (modelData.key === "folder") nav.navigate(modelData.key)
                }
            }
        }
    }

}
