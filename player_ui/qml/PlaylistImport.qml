import QtQuick
import PlayerUi

// 导入在线歌单:贴进酷狗 / 网易云 / QQ音乐的分享链接或 ID → 读出曲目 → 与本地曲库逐首对上 → 用本地有的存成歌单
//  卡片居中浮在暗下去的页面上;打开时从略小处放大淡入,各块错峰浮现,结果行依次浮出
//  只用强调色与文字明暗:本地有 = 强调色标签,可能 = 描边标签,缺少 = 整行变淡
Item {
    id: sheet
    property bool shown: false
    anchors.fill: parent
    visible: card.opacity > 0
    z: 44

    property int filter: 0          // 0 全部 · 1 本地有 · 2 缺少
    property bool includeLikely: true
    readonly property string st: importer.state
    readonly property bool ready: st === "ready"
    readonly property string kind: importer.detect(link.text)
    // 矮窗口:收起副标题、封面缩小,给列表多留几行
    readonly property bool compact: height < 760
    readonly property int saveCount: importer.exact + (includeLikely ? importer.likely : 0)

    function open() {
        shown = true
        if (!ready) Qt.callLater(function() { link.forceActiveFocus() })
    }
    function close() { shown = false; link.focus = false; nameInput.focus = false }
    function submit() { if (link.text.trim().length) importer.parse(link.text) }
    function save() {
        const id = importer.save(nameInput.text, includeLikely)
        if (!id.length) return
        Nav.toast("check", qsTr("已存为歌单,共 %1 首").arg(saveCount))
        close()
        importer.reset()
        link.text = ""
        Nav.openPlaylist(id)
    }
    function fmt(sec) {
        if (!(sec > 0)) return ""
        const s = Math.round(sec)
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }
    // 截图 / 自测
    function automationScroll(y) { list.contentY = Math.min(Number(y), Math.max(0, list.contentHeight - list.height)) }
    function automationLink(t, go) { link.text = t; if (go) submit() }
    function visibleRow(level) { return filter === 0 || (filter === 1 ? level > 0 : level === 0) }

    Connections {
        target: importer
        function onResultChanged() {
            if (!importer.rows.length) return
            nameInput.text = importer.meta.name || ""
            sheet.filter = 0
        }
    }

    // 载入中:强调色细圆弧匀速旋转 (与歌词来源面板同一形态)
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
    // 卡片内各块错峰浮现:order 越大越晚
    component Stagger: QtObject {
        property Item target
        property int order: 0
        property real t: sheet.shown ? 1 : 0
        Behavior on t { SequentialAnimation { PauseAnimation { duration: sheet.shown ? 90 + order * 45 : 0 }
                NumberAnimation { duration: sheet.shown ? 380 : 100; easing.type: Easing.OutCubic } } }
        onTChanged: if (target) { target.opacity = t; target.lift = (1 - t) * 10 }
    }
    // 小标签:强调色实底 / 描边 (与歌词来源候选行的标签一致)
    component Tag: Rectangle {
        property alias label: tagText.text
        property bool dim: false
        width: tagText.implicitWidth + 14
        height: 20
        radius: 10
        color: dim ? "transparent" : Theme.accentAlpha(0.12)
        border.width: dim ? 1 : 0
        border.color: Qt.rgba(1, 0.96, 0.9, 0.14)
        Text {
            id: tagText
            anchors.centerIn: parent
            color: parent.dim ? Theme.textTertiary : Theme.accent
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }
    }
    // 文字页签:与曲库页的「全部 / 最近播放」同一规则,选中用强调色
    component TextTab: Item {
        id: tt
        property string text
        property bool active: false
        signal clicked()
        width: tl.implicitWidth
        height: 24
        Text {
            id: tl
            anchors.verticalCenter: parent.verticalCenter
            text: tt.text
            color: tt.active ? Theme.accent : (tm.containsMouse ? Theme.textSecondary : Theme.textTertiary)
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.weight: tt.active ? Font.DemiBold : Font.Normal
            Behavior on color { ColorAnimation { duration: Theme.fast } }
        }
        MouseArea {
            id: tm
            anchors.fill: parent
            anchors.margins: -6
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: tt.clicked()
        }
    }

    // 暗幕:点空白处收起
    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: sheet.shown ? 0.5 : 0
        Behavior on opacity { OpacityAnimator { duration: sheet.shown ? 320 : 180 } }
        MouseArea {
            anchors.fill: parent
            enabled: sheet.shown
            acceptedButtons: Qt.AllButtons
            onPressed: sheet.close()
            onWheel: (w) => w.accepted = true
        }
    }

    Item {
        id: card
        width: Math.min(560, sheet.width - 64)
        // 高度随状态伸缩:只有输入框时紧凑,出结果后展开 (列表在里面滚动)
        height: sheet.ready ? Math.min(760, sheet.height - 96) : body.y + inputBlock.y + inputBlock.height + 28
        Behavior on height { NumberAnimation { duration: 520; easing.type: Easing.OutQuint } }
        anchors.centerIn: parent
        opacity: sheet.shown ? 1 : 0
        scale: sheet.shown ? 1 : 0.96
        Behavior on opacity { OpacityAnimator { duration: sheet.shown ? 260 : 150; easing.type: Easing.OutCubic } }
        Behavior on scale { ScaleAnimator { duration: sheet.shown ? 520 : 180; easing.type: sheet.shown ? Easing.OutQuint : Easing.InCubic } }
        SoftShadow { target: panel; strength: 0.9; spread: 34; offsetY: 18 }

        Rectangle {
            id: panel
            anchors.fill: parent
            radius: Theme.radiusL
            color: Theme.panel
            border.width: 1
            border.color: Qt.rgba(1, 0.96, 0.9, 0.08)
            clip: true
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }
            readonly property int pad: 24

            Stagger { target: heading; order: 0 }
            Stagger { target: inputBlock; order: 1 }
            Stagger { target: result; order: 2 }
            Stagger { target: footer; order: 3 }

            Column {
                id: heading
                property real lift: 0
                transform: Translate { y: heading.lift }
                x: panel.pad
                y: 24
                width: panel.width - panel.pad * 2 - 40
                spacing: 6
                Text {
                    text: qsTr("导入歌单")
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 18
                    font.weight: Font.Bold
                }
                Text {
                    width: parent.width
                    visible: !sheet.compact
                    text: qsTr("粘贴酷狗、网易云或 QQ音乐的歌单链接,曲库里已有的歌会自动对上")
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
            }
            IconButton {
                anchors.right: parent.right
                anchors.rightMargin: 14
                y: 18
                width: 32; height: 32
                icon: "close"
                iconSize: 16
                onClicked: sheet.close()
            }

            Item {
                id: body
                y: heading.y + heading.height + 18
                width: panel.width
                height: panel.height - y

                // ---- 链接输入 + 识别提示 ----
                Column {
                    id: inputBlock
                    property real lift: 0
                    transform: Translate { y: inputBlock.lift }
                    x: panel.pad
                    width: panel.width - panel.pad * 2
                    spacing: 10
                    Rectangle {
                        id: field
                        width: parent.width
                        height: 42
                        radius: 21
                        color: Qt.rgba(1, 0.96, 0.9, link.activeFocus ? 0.08 : 0.05)
                        border.width: 1
                        border.color: link.activeFocus ? Theme.accentAlpha(0.5) : "transparent"
                        Behavior on border.color { ColorAnimation { duration: Theme.fast } }
                        Icon {
                            id: linkIcon
                            x: 15
                            anchors.verticalCenter: parent.verticalCenter
                            name: "link"
                            size: 16
                            tint: sheet.kind.length ? Theme.accent : "transparent"
                            opacity: sheet.kind.length ? 1 : 0.5
                        }
                        TextInput {
                            id: link
                            anchors.left: linkIcon.right
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
                            Keys.onReturnPressed: sheet.submit()
                            Keys.onEnterPressed: sheet.submit()
                            Keys.onEscapePressed: sheet.close()
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: !link.text.length
                                text: qsTr("歌单链接或 ID")
                                color: Theme.textTertiary
                                font: link.font
                            }
                        }
                        Capsule {
                            id: go
                            anchors.right: parent.right
                            anchors.rightMargin: 5
                            anchors.verticalCenter: parent.verticalCenter
                            height: 32
                            pad: 16
                            accent: true
                            text: sheet.ready ? qsTr("重新读取") : qsTr("读取")
                            opacity: link.text.trim().length ? 1 : 0.4
                            interactive: link.text.trim().length > 0 && sheet.st !== "loading"
                            onClicked: if (interactive) sheet.submit()
                        }
                    }
                    // 一行状态:识别结果 / 选平台 / 读取中 / 出错
                    Item {
                        width: parent.width
                        height: 24
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            leftPadding: 4
                            spacing: 8
                            Spinner { anchors.verticalCenter: parent.verticalCenter; running: sheet.st === "loading" && sheet.shown }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: sheet.st === "loading" ? qsTr("正在读取歌单")
                                    : sheet.st === "error" ? importer.error
                                    : sheet.st === "platform" ? qsTr("这是哪个平台的歌单?")
                                    : sheet.kind === "kugou" ? qsTr("酷狗歌单")
                                    : sheet.kind === "netease" ? qsTr("网易云歌单")
                                    : sheet.kind === "qqmusic" ? qsTr("QQ音乐歌单")
                                    : sheet.kind === "id" ? qsTr("纯数字 ID,读取后选择平台")
                                    : link.text.length ? qsTr("认不出这个链接") : qsTr("支持分享链接、短链和歌单 ID")
                                color: sheet.st === "error" ? Theme.textSecondary : Theme.textTertiary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }
                            // 纯数字 ID:选平台 (三枚胶囊,点了立即读取)
                            Capsule {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: sheet.st === "platform"
                                height: 26
                                pad: 12
                                text: qsTr("酷狗")
                                onClicked: importer.parseAs("kugou")
                            }
                            Capsule {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: sheet.st === "platform"
                                height: 26
                                pad: 12
                                text: qsTr("网易云")
                                onClicked: importer.parseAs("netease")
                            }
                            Capsule {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: sheet.st === "platform"
                                height: 26
                                pad: 12
                                text: qsTr("QQ音乐")
                                onClicked: importer.parseAs("qqmusic")
                            }
                        }
                    }
                }

                // ---- 结果 ----
                Item {
                    id: result
                    property real lift: 0
                    transform: Translate { y: result.lift }
                    y: inputBlock.y + inputBlock.height + 14
                    width: panel.width
                    height: footer.y - y
                    visible: sheet.ready
                    opacity: sheet.ready ? 1 : 0

                    // 歌单信息
                    Mosaic {
                        id: cover
                        x: panel.pad
                        width: sheet.compact ? 52 : 72
                        height: width
                        radius: sheet.compact ? 8 : 10
                        covers: importer.meta.covers || []
                        decodeSize: 160
                    }
                    Column {
                        anchors.left: cover.right
                        anchors.leftMargin: 16
                        anchors.right: parent.right
                        anchors.rightMargin: panel.pad
                        anchors.verticalCenter: cover.verticalCenter
                        spacing: 5
                        Text {
                            width: parent.width
                            text: importer.meta.name || ""
                            elide: Text.ElideRight
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 16
                            font.weight: Font.Bold
                        }
                        Text {
                            width: parent.width
                            text: [importer.meta.source, qsTr("%1 首").arg(importer.meta.total || 0), importer.meta.author].filter(s => s && s.length).join("  ·  ")
                            elide: Text.ElideRight
                            color: Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                        // 本地覆盖率:强调色实段 = 本地有,浅段 = 可能
                        Item {
                            width: parent.width
                            height: 4
                            Rectangle { anchors.fill: parent; radius: 2; color: Qt.rgba(1, 0.96, 0.9, 0.08) }
                            Rectangle {
                                id: likelyBar
                                height: parent.height
                                radius: 2
                                color: Theme.accentAlpha(0.35)
                                width: parent.width * (importer.exact + importer.likely) / Math.max(1, importer.meta.total || 0)
                                Behavior on width { NumberAnimation { duration: 700; easing.type: Easing.OutQuint } }
                            }
                            Rectangle {
                                height: parent.height
                                radius: 2
                                color: Theme.accent
                                width: parent.width * importer.exact / Math.max(1, importer.meta.total || 0)
                                Behavior on width { NumberAnimation { duration: 700; easing.type: Easing.OutQuint } }
                            }
                        }
                    }

                    // 筛选页签 + 右侧操作
                    Row {
                        id: tabs
                        x: panel.pad
                        y: cover.y + cover.height + (sheet.compact ? 14 : 20)
                        spacing: 20
                        TextTab { text: qsTr("全部 %1").arg(importer.meta.total || 0); active: sheet.filter === 0; onClicked: sheet.filter = 0 }
                        TextTab { text: qsTr("本地有 %1").arg(importer.exact + importer.likely); active: sheet.filter === 1; onClicked: sheet.filter = 1 }
                        TextTab { text: qsTr("缺少 %1").arg(importer.missing); active: sheet.filter === 2; onClicked: sheet.filter = 2 }
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: panel.pad
                        anchors.verticalCenter: tabs.verticalCenter
                        visible: importer.missing > 0
                        text: qsTr("复制缺少的曲目")
                        color: copyMouse.containsMouse ? Theme.accent : Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        Behavior on color { ColorAnimation { duration: Theme.fast } }
                        MouseArea {
                            id: copyMouse
                            anchors.fill: parent
                            anchors.margins: -6
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                const n = importer.copyMissing()
                                if (n > 0) Nav.toast("copy", qsTr("已复制 %1 首缺少的曲目").arg(n))
                            }
                        }
                    }
                    Rectangle {
                        x: panel.pad
                        y: tabs.y + tabs.height + 10
                        width: panel.width - panel.pad * 2
                        height: 1
                        color: Theme.hairline
                    }

                    ListView {
                        id: list
                        x: 8
                        y: tabs.y + tabs.height + 12
                        width: panel.width - 16
                        height: parent.height - y
                        clip: true
                        // 按页签筛好再交给列表:不再给隐藏的行建零高度的委托 (几百首时全部实例化,滚动卡顿)
                        model: importer.rows.filter(r => sheet.visibleRow(r.level))
                        boundsBehavior: Flickable.StopAtBounds
                        cacheBuffer: 560
                        reuseItems: true
                        SmoothScroll { flickable: list }
                        // 出结果 / 换页签时首屏几行错峰浮现;之后滚动新出现的行直接显示 (以前按下标排队等动画,
                        // 滚到后面要等半秒才出来,看起来像没刷新)
                        property bool intro: false
                        onModelChanged: { intro = true; introEnd.restart(); positionViewAtBeginning() }
                        Timer { id: introEnd; interval: 700; onTriggered: list.intro = false }

                        delegate: Item {
                            id: row
                            readonly property int level: modelData.level
                            width: list.width
                            height: 56
                            property real lift: 0
                            transform: Translate { y: row.lift }
                            Component.onCompleted: if (list.intro && index < 12) { opacity = 0; lift = 8; appear.start() }
                            ListView.onReused: { appear.stop(); opacity = 1; lift = 0 }
                            ParallelAnimation {
                                id: appear
                                SequentialAnimation {
                                    PauseAnimation { duration: 60 + index * 26 }
                                    ParallelAnimation {
                                        OpacityAnimator { target: row; from: 0; to: 1; duration: 260; easing.type: Easing.OutCubic }
                                        NumberAnimation { target: row; property: "lift"; from: 8; to: 0; duration: 420; easing.type: Easing.OutQuint }
                                    }
                                }
                            }

                            Rectangle {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                radius: Theme.radiusS
                                color: Theme.hover
                                opacity: rowMouse.containsMouse && row.level > 0 ? 1 : 0
                                Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                            }
                            // 缺少的整行变淡;本地有的显示本地那份的专辑封面
                            Item {
                                anchors.fill: parent
                                opacity: row.level === 0 ? 0.45 : 1
                                Item {
                                    id: thumb
                                    x: 16
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 38; height: 38
                                    RoundedImage {
                                        anchors.fill: parent
                                        visible: row.level > 0 && !!modelData.cover
                                        source: visible ? modelData.cover : ""
                                        radius: 6
                                        decodeSize: 80
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        visible: !(row.level > 0 && !!modelData.cover)
                                        radius: 6
                                        color: Qt.rgba(1, 0.96, 0.9, 0.05)
                                        Icon { anchors.centerIn: parent; name: "note"; size: 16; opacity: 0.4 }
                                    }
                                }
                                Column {
                                    anchors.left: thumb.right
                                    anchors.leftMargin: 12
                                    anchors.right: tags.left
                                    anchors.rightMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 4
                                    Text {
                                        width: parent.width
                                        elide: Text.ElideRight
                                        text: modelData.title
                                        color: Theme.text
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 14
                                        font.weight: Font.Medium
                                    }
                                    Text {
                                        width: parent.width
                                        elide: Text.ElideRight
                                        text: [modelData.artist, sheet.fmt(modelData.duration)].filter(s => s && s.length).join("  ·  ")
                                        color: Theme.textTertiary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 12
                                    }
                                }
                                Row {
                                    id: tags
                                    anchors.right: parent.right
                                    anchors.rightMargin: 18
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 6
                                    Tag { label: qsTr("可能"); dim: true; visible: row.level === 1 }
                                    Tag { label: modelData.local || ""; visible: row.level > 0 && !!modelData.local }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: row.level === 0
                                        text: qsTr("本地没有")
                                        color: Theme.textTertiary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 12
                                    }
                                }
                            }
                            MouseArea {
                                id: rowMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: row.level > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                                // 双击试听本地那份
                                onDoubleClicked: if (row.level > 0) Nav.picked(player.pickTracks([modelData.trackId], 0, "", false))
                            }
                        }
                    }
                }

                // ---- 底部:歌单名 + 存为歌单 ----
                Item {
                    id: footer
                    property real lift: 0
                    transform: Translate { y: footer.lift }
                    y: body.height - height
                    width: panel.width
                    height: 76
                    visible: sheet.ready
                    opacity: sheet.ready ? 1 : 0
                    Rectangle { width: parent.width; height: 1; color: Theme.hairline }
                    Rectangle {
                        id: nameField
                        anchors.left: parent.left
                        anchors.leftMargin: panel.pad
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: likelyToggle.visible ? likelyToggle.left : saveBtn.left
                        anchors.rightMargin: 14
                        height: 38
                        radius: 19
                        color: Qt.rgba(1, 0.96, 0.9, nameInput.activeFocus ? 0.08 : 0.05)
                        border.width: 1
                        border.color: nameInput.activeFocus ? Theme.accentAlpha(0.5) : "transparent"
                        Behavior on border.color { ColorAnimation { duration: Theme.fast } }
                        Icon { id: plIcon; x: 14; anchors.verticalCenter: parent.verticalCenter; name: "playlist"; size: 15; opacity: 0.5 }
                        TextInput {
                            id: nameInput
                            anchors.left: plIcon.right
                            anchors.leftMargin: 9
                            anchors.right: parent.right
                            anchors.rightMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            color: Theme.text
                            selectionColor: Theme.accentAlpha(0.4)
                            selectedTextColor: Theme.text
                            selectByMouse: true
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            clip: true
                            Keys.onReturnPressed: sheet.save()
                            Keys.onEscapePressed: sheet.close()
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: !nameInput.text.length
                                text: qsTr("歌单名")
                                color: Theme.textTertiary
                                font: nameInput.font
                            }
                        }
                    }
                    // 「可能」的要不要一起存:勾选框式的小开关
                    Item {
                        id: likelyToggle
                        anchors.right: saveBtn.left
                        anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        visible: importer.likely > 0
                        width: box.width + 7 + likelyText.implicitWidth
                        height: 32
                        Rectangle {
                            id: box
                            anchors.verticalCenter: parent.verticalCenter
                            width: 16; height: 16
                            radius: 5
                            color: sheet.includeLikely ? Theme.accent : "transparent"
                            border.width: sheet.includeLikely ? 0 : 1.5
                            border.color: Qt.rgba(1, 0.96, 0.9, toggleMouse.containsMouse ? 0.4 : 0.24)
                            Behavior on color { ColorAnimation { duration: Theme.fast } }
                            Icon {
                                anchors.centerIn: parent
                                name: "check"
                                size: 12
                                tint: Theme.onAccent
                                opacity: sheet.includeLikely ? 1 : 0
                                scale: sheet.includeLikely ? 1 : 0.5
                                Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                                Behavior on scale { ScaleAnimator { duration: 300; easing.type: Easing.OutBack } }
                            }
                        }
                        Text {
                            id: likelyText
                            anchors.left: box.right
                            anchors.leftMargin: 7
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("含可能的 %1 首").arg(importer.likely)
                            color: toggleMouse.containsMouse ? Theme.textSecondary : Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                        MouseArea {
                            id: toggleMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: sheet.includeLikely = !sheet.includeLikely
                        }
                    }
                    Capsule {
                        id: saveBtn
                        anchors.right: parent.right
                        anchors.rightMargin: panel.pad
                        anchors.verticalCenter: parent.verticalCenter
                        height: 38
                        pad: 18
                        filled: true
                        text: sheet.saveCount > 0 ? qsTr("存为歌单 · %1 首").arg(sheet.saveCount) : qsTr("本地一首都没有")
                        opacity: sheet.saveCount > 0 ? 1 : 0.4
                        interactive: sheet.saveCount > 0
                        onClicked: if (interactive) sheet.save()
                    }
                }
            }
        }
    }
}
