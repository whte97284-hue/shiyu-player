import QtQuick
import PlayerUi

// 详情页的精简标题栏:头部滚出视野后从顶端浮出 (返回 · 封面 · 标题 · 更多 · 随机 · 播放)
// 专辑 / 歌手 / 歌单详情共用;点标题区回到顶部
Item {
    id: bar
    property ListView view
    property int sidePadding: 44
    property string title
    property string subtitle
    property string cover
    property bool round: false      // 圆形头像 (歌手)
    property bool current: false    // 当前队列就是这个页面的列表
    readonly property bool playing: current && player.playing   // 播放键显示为暂停
    property string playTip: current ? (player.playing ? qsTr("暂停") : qsTr("继续播放")) : qsTr("播放")
    // 头部里「大标题区」的高度 (从头部顶端算起);< 0 时用整个头部
    property real heroHeight: -1
    signal back()
    signal play()
    signal shuffle()
    signal more(Item anchor)

    z: 2
    width: parent ? parent.width : 0
    height: 100
    readonly property real heroBottom: view && view.headerItem
                                       ? view.headerItem.y + (heroHeight >= 0 ? heroHeight : view.headerItem.height) - view.contentY : 1e6
    readonly property bool shown: heroBottom < height + 46
    opacity: shown ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { OpacityAnimator { duration: Theme.fast } }

    // 背景:与播放栏同一做法,铺一份与窗口背景对齐的模糊封面再压暗,像磨砂玻璃。
    // 底边不画实线、不硬切:背景往下多延伸一截并渐隐,列表内容柔和地隐入栏下,不会像一块突兀的黑条
    Item {
        id: glass
        width: parent.width
        height: parent.height + 40
        layer.enabled: bar.visible
        layer.effect: ShaderEffect {
            property real fadeTop: 0.001
            property real fadeBottom: 44 / glass.height
            fragmentShader: "qrc:/shaders/fade.frag.qsb"
        }
        Rectangle { anchors.fill: parent; color: Theme.base }
        Item {
            anchors.fill: parent
            clip: true
            Backdrop {
                x: -Theme.navWidth
                y: 0
                width: bar.width + Theme.navWidth
                height: bar.Window.window ? bar.Window.window.height : bar.height
                source: player.cover
                solid: false
                opacity: player.hasTrack ? 1 : 0
            }
        }
        Rectangle { anchors.fill: parent; color: Qt.rgba(0.05, 0.045, 0.04, player.hasTrack ? 0.5 : 0.9) }
    }
    // 挡住下面的列表 (精简栏本身不吃滚轮,滚轮照常滚列表)
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = false }

    // 顶部 44px 留给窗口按钮 (标题栏拖动区),内容放在下面一行
    Item {
        x: bar.sidePadding
        y: 44
        width: bar.width - bar.sidePadding * 2
        height: 56
        IconButton {
            id: backBtn
            x: -8
            anchors.verticalCenter: parent.verticalCenter
            width: 36; height: 36
            icon: "back"
            iconSize: 18
            tip: qsTr("返回")
            onClicked: bar.back()
        }
        RoundedImage {
            id: coverImg
            anchors.left: backBtn.right
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 36; height: 36
            radius: bar.round ? 18 : 6
            source: bar.cover
            decodeSize: 96
        }
        Column {
            anchors.left: coverImg.right
            anchors.leftMargin: 12
            anchors.right: buttons.left
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 1
            Text {
                width: parent.width
                elide: Text.ElideRight
                text: bar.title
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                elide: Text.ElideRight
                text: bar.subtitle
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
        }
        // 点标题区回到顶部
        MouseArea {
            anchors.left: coverImg.left
            anchors.right: buttons.left
            height: parent.height
            cursorShape: Qt.PointingHandCursor
            onClicked: if (bar.view) bar.view.positionViewAtBeginning()
        }
        Row {
            id: buttons
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10
            IconButton {
                id: moreBtn
                anchors.verticalCenter: parent.verticalCenter
                width: 32; height: 32
                icon: "more"
                iconSize: 15
                tip: qsTr("更多")
                onClicked: bar.more(moreBtn)
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                width: 32; height: 32
                outlined: true
                icon: "shuffle"
                iconSize: 14
                tip: qsTr("随机播放")
                onClicked: bar.shuffle()
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                width: 38; height: 38
                filled: true
                morphOnChange: true
                icon: bar.playing ? "pause" : "play"
                iconSize: 16
                tip: bar.playTip
                onClicked: bar.play()
            }
        }
    }
}
