import QtQuick
import PlayerUi

// 歌词来源面板:从右下角的来源胶囊弹出,盖在歌词栏上
//  当前来源 (强调色来源图标 + 来源名 + 文件 / 歌名;可保存到本地、在文件夹中显示)
//  → 搜索框 (默认「歌名 歌手」,打开时自动搜一次) → 候选列表 (点一下立即换上,勾号标出正在用的)
// 外语歌:候选在后台预取后标出「翻译」,可只看带翻译的;自动 / 手动换成在线版后可「用本地歌词」换回
// 只用强调色与文字的明暗,不给各个来源配颜色
Item {
    id: panelRoot
    property bool shown: false
    // 弹出锚点:面板右下角对齐这个点 (在 panelRoot 坐标系里)
    property real anchorRight: width - 20
    property real anchorBottom: height - 60
    anchors.fill: parent
    visible: pop.opacity > 0
    z: 5

    readonly property var src: player.lyricSource
    readonly property bool hasSource: !!(src && src.kind && src.kind !== "none")
    property int pending: -1   // 点了、还在拉取的候选
    property bool onlyTranslated: false // 只看带翻译的候选
    function tagOf(i) { return i < player.lyricCandidateTags.length ? player.lyricCandidateTags[i] : -1 }
    readonly property bool anyTranslated: { for (let i = 0; i < player.lyricCandidateTags.length; ++i) if (player.lyricCandidateTags[i] === 1) return true; return false }
    onAnyTranslatedChanged: if (!anyTranslated) onlyTranslated = false

    function open() {
        shown = true
        query.text = player.lyricQuery()
        if (!player.lyricSearching && player.lyricCandidates.length === 0) player.searchLyrics(query.text)
    }
    property int hoverRow: -1  // 截图用:模拟悬停
    function pick(i) { if (player.lyricPicked !== i) { pending = i; player.useLyricCandidate(i) } }
    function save() { player.saveLyrics() }
    // 载入中:强调色细圆弧匀速旋转,停下时淡出
    component Spinner: Icon {
        id: sp
        property bool running: false
        name: "spinner"
        size: 14
        tint: Theme.accent
        opacity: running ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
        RotationAnimator on rotation { from: 0; to: 360; duration: 900; loops: Animation.Infinite; running: sp.visible }
    }
    function close() { shown = false; query.focus = false }
    // 面板内各块错峰浮现:order 越大越晚 (每级 45ms);收起时一起快速淡出
    component Stagger: QtObject {
        property Item target
        property int order: 0
        readonly property bool on: panelRoot.shown
        property real t: on ? 1 : 0
        Behavior on t { SequentialAnimation { PauseAnimation { duration: panelRoot.shown ? 80 + order * 45 : 0 }
                NumberAnimation { duration: panelRoot.shown ? 360 : 100; easing.type: Easing.OutCubic } } }
        onTChanged: if (target) { target.opacity = t; target.lift = (1 - t) * 8 }
    }
    function fmt(sec) {
        if (!(sec > 0)) return ""
        const s = Math.round(sec)
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }

    Connections {
        target: player
        function onLyricSearchChanged() { if (player.lyricPicked >= 0 || player.lyricSearchNote.length) panelRoot.pending = -1 }
        function onTrackChanged() { panelRoot.pending = -1; if (panelRoot.shown) query.text = player.lyricQuery() }
    }

    // 点面板外收起
    MouseArea {
        anchors.fill: parent
        enabled: panelRoot.shown
        acceptedButtons: Qt.AllButtons
        onPressed: panelRoot.close()
        onWheel: (w) => w.accepted = true
    }

    // 面板连同投影一起从右侧按钮处滑出:向左平移 + 轻微放大 + 淡入 (收起时原路退回,更快);
    // 面板里的各块再依次浮现
    Item {
        id: pop
        width: Math.min(420, panelRoot.width - 40)
        // 高度随结果多少伸缩 (至少留四行,搜索中不跳),放不下时列表内部滚动
        height: Math.min(600, panelRoot.anchorBottom - 72, list.y + Math.max(4 * 60, list.contentHeight) + 12)
        Behavior on height { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
        x: panelRoot.anchorRight - width
        y: panelRoot.anchorBottom - height
        transformOrigin: Item.BottomRight
        opacity: panelRoot.shown ? 1 : 0
        scale: panelRoot.shown ? 1 : 0.97
        Behavior on opacity { OpacityAnimator { duration: panelRoot.shown ? 260 : 150; easing.type: Easing.OutCubic } }
        Behavior on scale { ScaleAnimator { duration: panelRoot.shown ? 520 : 180; easing.type: panelRoot.shown ? Easing.OutQuint : Easing.InCubic } }
        transform: Translate {
            x: panelRoot.shown ? 0 : 28
            Behavior on x { NumberAnimation { duration: panelRoot.shown ? 560 : 180; easing.type: panelRoot.shown ? Easing.OutQuint : Easing.InCubic } }
        }
        SoftShadow { target: card; strength: 0.85; spread: 30; offsetY: 16 }

        Rectangle {
            id: card
            anchors.fill: parent
            radius: Theme.radiusL
            color: Theme.panel
            border.width: 1
            border.color: Qt.rgba(1, 0.96, 0.9, 0.08)
            // 吃掉面板内的点击,不让它漏到「点外面收起」
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }
            Keys.onEscapePressed: panelRoot.close()

            readonly property int pad: 20

            // ---- 标题 ----
            Stagger { target: heading; order: 0 }
            Stagger { target: current; order: 1 }
            Stagger { target: field; order: 2 }
            Stagger { target: status; order: 3 }
            Text {
                id: heading
                property real lift: 0
                transform: Translate { y: heading.lift }
                x: card.pad
                y: 22
                text: qsTr("歌词来源")
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 16
                font.weight: Font.Bold
            }
            IconButton {
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: heading.verticalCenter
                width: 32; height: 32
                icon: "close"
                iconSize: 16
                onClicked: panelRoot.close()
            }

            // ---- 当前来源 ----
            Rectangle {
                id: current
                property real lift: 0
                transform: Translate { y: current.lift }
                x: card.pad
                y: heading.y + heading.height + 16
                width: card.width - card.pad * 2
                height: 64
                radius: Theme.radiusM
                color: Theme.accentAlpha(0.08)
                // 来源变了 (换上候选 / 存到本地) 时整块轻轻亮一下
                Rectangle {
                    id: flash
                    anchors.fill: parent
                    radius: parent.radius
                    color: Theme.accentAlpha(0.14)
                    opacity: 0
                    OpacityAnimator on opacity { id: flashAnim; running: false; from: 1; to: 0; duration: 700; easing.type: Easing.OutCubic }
                }
                Connections {
                    target: player
                    function onLyricSourceChanged() { if (panelRoot.shown) flashAnim.restart() }
                }

                Icon {
                    id: dot
                    x: 16
                    anchors.verticalCenter: parent.verticalCenter
                    name: Nav.lyricIcon(panelRoot.hasSource ? panelRoot.src.kind : "none")
                    size: 20
                    tint: Theme.accent
                }
                Column {
                    anchors.left: dot.right
                    anchors.leftMargin: 12
                    anchors.right: actions.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        text: panelRoot.hasSource ? panelRoot.src.label : qsTr("还没有歌词")
                        color: panelRoot.hasSource ? Theme.accent : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                    // 说明 + (换成了在线版时) 行内的「换回本地歌词」,和说明同一行同一字号,不另占按钮位
                    Row {
                        width: parent.width
                        spacing: 8
                        Text {
                            width: Math.min(implicitWidth, parent.width - (revert.visible ? revert.width + parent.spacing : 0))
                            elide: Text.ElideMiddle
                            text: panelRoot.hasSource ? panelRoot.src.detail : qsTr("从下面的结果里挑一份")
                            color: Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                        Text {
                            id: revert
                            visible: !!(panelRoot.src && panelRoot.src.local)
                            text: qsTr("换回本地歌词")
                            color: revertMouse.containsMouse ? Theme.accent : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            font.underline: revertMouse.containsMouse
                            Behavior on color { ColorAnimation { duration: Theme.fast } }
                            MouseArea {
                                id: revertMouse
                                anchors.fill: parent
                                anchors.margins: -4
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: player.useLocalLyrics()
                            }
                        }
                    }
                }
                Row {
                    id: actions
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Capsule {
                        visible: !!(panelRoot.src && panelRoot.src.path)
                        text: qsTr("显示文件")
                        height: 30
                        onClicked: libtools.reveal(panelRoot.src.path)
                    }
                    Capsule {
                        visible: !!(panelRoot.src && panelRoot.src.savable)
                        text: qsTr("保存到本地")
                        filled: true
                        height: 30
                        onClicked: panelRoot.save()
                    }
                }
            }

            // ---- 搜索框 ----
            Rectangle {
                id: field
                property real lift: 0
                transform: Translate { y: field.lift }
                x: card.pad
                y: current.y + current.height + 16
                width: card.width - card.pad * 2
                height: 40
                radius: 20
                color: Qt.rgba(1, 0.96, 0.9, query.activeFocus ? 0.08 : 0.05)
                border.width: 1
                border.color: query.activeFocus ? Theme.accentAlpha(0.5) : "transparent"
                Behavior on border.color { ColorAnimation { duration: Theme.fast } }
                Icon {
                    id: glass
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    name: "search"
                    size: 16
                    opacity: 0.5
                }
                TextInput {
                    id: query
                    anchors.left: glass.right
                    anchors.leftMargin: 10
                    anchors.right: go.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.text
                    selectionColor: Theme.accentAlpha(0.4)
                    selectedTextColor: Theme.text
                    selectByMouse: true
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    clip: true
                    Keys.onReturnPressed: player.searchLyrics(text)
                    Keys.onEnterPressed: player.searchLyrics(text)
                    Keys.onEscapePressed: panelRoot.close()
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !query.text.length
                        text: qsTr("歌名、歌手")
                        color: Theme.textTertiary
                        font: query.font
                    }
                }
                Capsule {
                    id: go
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    height: 32
                    pad: 16
                    accent: true
                    text: qsTr("搜索")
                    onClicked: player.searchLyrics(query.text)
                }
            }

            // ---- 结果状态 ----
            Item {
                id: status
                property real lift: 0
                transform: Translate { y: status.lift }
                x: card.pad
                y: field.y + field.height + 14
                width: card.width - card.pad * 2
                height: 18
                Text {
                    id: statusText
                    anchors.verticalCenter: parent.verticalCenter
                    text: player.lyricSearching ? qsTr("正在搜索")
                        : player.lyricSearchNote.length ? player.lyricSearchNote
                        : player.lyricCandidates.length ? qsTr("找到 %1 份,点一下即可换上").arg(player.lyricCandidates.length)
                        : ""
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                }
                // 搜索中:细圆弧匀速旋转
                Spinner {
                    anchors.left: statusText.right
                    anchors.leftMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    running: player.lyricSearching && panelRoot.shown
                }
                // 筛选:只看带中文翻译的 (有候选确认带翻译时才出现)。与候选行右侧的小标签同一形态,右缘与标签列对齐
                // (标签列右缘 = 列表右内缩 8 + 行右边距 16 + 勾号位 20 + 间距 6,比本行右缘多出 30)
                Rectangle {
                    id: trFilter
                    anchors.right: parent.right
                    anchors.rightMargin: 30
                    anchors.verticalCenter: parent.verticalCenter
                    width: trText.implicitWidth + 14
                    height: 20
                    radius: 10
                    opacity: panelRoot.anyTranslated && !player.lyricSearching ? 1 : 0
                    visible: opacity > 0
                    Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
                    color: panelRoot.onlyTranslated ? Theme.accentAlpha(0.12) : "transparent"
                    border.width: panelRoot.onlyTranslated ? 0 : 1
                    border.color: Qt.rgba(1, 0.96, 0.9, trMouse.containsMouse ? 0.24 : 0.12)
                    Behavior on color { ColorAnimation { duration: Theme.fast } }
                    Text {
                        id: trText
                        anchors.centerIn: parent
                        text: qsTr("只看带翻译")
                        color: panelRoot.onlyTranslated ? Theme.accent : Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                    }
                    MouseArea {
                        id: trMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: panelRoot.onlyTranslated = !panelRoot.onlyTranslated
                    }
                }
            }

            // ---- 候选列表 ----
            ListView {
                id: list
                x: 8
                y: status.y + status.height + 8
                width: card.width - 16
                height: card.height - y - 8
                clip: true
                model: player.lyricCandidates
                boundsBehavior: Flickable.StopAtBounds
                opacity: player.lyricSearching ? 0.4 : 1
                Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
                // 每次出结果整列重新错峰淡入
                property int wave: 0
                onModelChanged: wave++

                delegate: Item {
                    id: row
                    width: list.width
                    readonly property int tr: panelRoot.tagOf(index)
                    readonly property bool hiddenByFilter: panelRoot.onlyTranslated && tr !== 1 && !inUse
                    height: hiddenByFilter ? 0 : 60
                    visible: height > 0
                    clip: true
                    Behavior on height { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                    readonly property bool inUse: player.lyricPicked === index
                    readonly property bool loading: panelRoot.pending === index
                    opacity: 0
                    property real lift: 8
                    transform: Translate { y: row.lift }
                    Component.onCompleted: appear.start()
                    // 重新打开面板时结果行也跟着各块之后依次浮现
                    property int delayBase: 0
                    Connections {
                        target: panelRoot
                        function onShownChanged() { if (panelRoot.shown) { row.delayBase = 200; row.opacity = 0; row.lift = 8; appear.restart() } }
                    }
                    ParallelAnimation {
                        id: appear
                        SequentialAnimation {
                            PauseAnimation { duration: row.delayBase + Math.min(index, 8) * 30 }
                            ParallelAnimation {
                                OpacityAnimator { target: row; from: 0; to: 1; duration: 260; easing.type: Easing.OutCubic }
                                NumberAnimation { target: row; property: "lift"; from: 8; to: 0; duration: 420; easing.type: Easing.OutQuint }
                            }
                        }
                    }

                    Rectangle {
                        anchors.fill: parent
                        anchors.leftMargin: 4
                        anchors.rightMargin: 4
                        radius: Theme.radiusS
                        color: row.inUse ? Theme.accentAlpha(0.10) : Theme.hover
                        opacity: row.inUse || rowMouse.containsMouse || panelRoot.hoverRow === index ? 1 : 0
                        Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                    }
                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        anchors.right: tags.left
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: modelData.title
                            color: row.inUse ? Theme.accent : Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: row.inUse ? Font.DemiBold : Font.Medium
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: [modelData.artist, modelData.album, panelRoot.fmt(modelData.duration)].filter(s => s && s.length).join("  ·  ")
                            color: Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                    }
                    // 右侧:来源 / 逐字 / 无时间轴 小标签 (同一种淡强调色);正在用的换成勾,拉取中换成旋转的细圆弧
                    Row {
                        id: tags
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        component Tag: Rectangle {
                            property alias label: t.text
                            property bool dim: false
                            width: t.implicitWidth + 14
                            height: 20
                            radius: 10
                            color: dim ? "transparent" : Theme.accentAlpha(0.12)
                            border.width: dim ? 1 : 0
                            border.color: Qt.rgba(1, 0.96, 0.9, 0.12)
                            Text {
                                id: t
                                anchors.centerIn: parent
                                color: parent.dim ? Theme.textTertiary : Theme.accent
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                        }
                        // 特性合成一枚强调色标签 (逐字 · 翻译),来源用描边标签,每行最多两枚
                        readonly property var features: [modelData.wordTimed && modelData.synced ? qsTr("逐字") : "", row.tr === 1 ? qsTr("翻译") : ""].filter(s => s.length)
                        Tag { label: tags.features.join(" · "); visible: tags.features.length > 0 }
                        Tag { label: qsTr("无时间轴"); dim: true; visible: !modelData.synced }
                        Tag { label: modelData.source; dim: true }
                        Item {
                            width: 20; height: 20
                            Icon {
                                anchors.centerIn: parent
                                name: "check"
                                size: 16
                                tint: Theme.accent
                                opacity: row.inUse ? 1 : 0
                                scale: row.inUse ? 1 : 0.4
                                Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
                                Behavior on scale { ScaleAnimator { duration: 360; easing.type: Easing.OutBack } }
                            }
                            Spinner { anchors.centerIn: parent; running: row.loading }
                        }
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: panelRoot.pick(index)
                    }
                }
            }
        }
    }
}
