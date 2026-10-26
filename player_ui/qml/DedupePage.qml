import QtQuick
import QtQuick.Dialogs
import PlayerUi

// 查找重复歌曲:按「完全相同 → 只是标签不同 → 同一首不同音质 → 文件名相同 → 不同版本」分组,
// 每组自动建议保留一份;勾选要处理的,再选怎么处理 (回收站 / 隔离文件夹 / 只从曲库移除 / 硬链接 / 永久删除)。
//  ┌ 顶栏长胶囊:‹ 查找重复 · 同一首不同音质 · 无标签按文件名 · 时长 ±3 秒 ………… 撤销上次 [开始查找] ┐
//  全部 · 完全相同 · 只是标签不同 …        勾选规则 ▾      [处理 N 个 · 腾出 X ▾]
//  ┌ 分组卡片列表 ┐
Item {
    id: page
    property int bottomInset: 140
    signal back()
    readonly property int pad: width > 1400 ? 56 : 40
    readonly property real contentW: Math.min(1120, width - pad * 2)
    readonly property var sum: libtools.dupSummary
    readonly property bool hasResult: sum.groups !== undefined
    readonly property bool working: libtools.busy && ["find", "apply", "undoDedupe"].indexOf(libtools.task) >= 0
    property int menu: 0   // 0 无 1 勾选规则 2 处理方式
    property bool confirmDelete: false

    FolderDialog { id: quarantinePicker; title: qsTr("选择隔离文件夹"); onAccepted: libtools.setQuarantineUrl(selectedFolder) }

    Connections {
        target: libtools
        function onMessage(icon, text) { if (page.visible) Nav.toast(icon, text) }
    }

    property bool shownOnce: false
    onVisibleChanged: if (visible && !shownOnce) { shownOnce = true; intro.start() }
    Component.onCompleted: if (visible) { shownOnce = true; intro.start() }
    SequentialAnimation {
        id: intro
        ScriptAction { script: { bar.opacity = 0; tools.opacity = 0; panel.opacity = 0; barT.y = 10; toolsT.y = 10; panelT.y = 16 } }
        ParallelAnimation {
            NumberAnimation { target: bar; property: "opacity"; to: 1; duration: 320; easing.type: Easing.OutCubic }
            NumberAnimation { target: barT; property: "y"; to: 0; duration: 460; easing.type: Easing.OutQuint }
            SequentialAnimation {
                PauseAnimation { duration: 60 }
                ParallelAnimation {
                    NumberAnimation { target: tools; property: "opacity"; to: 1; duration: 320; easing.type: Easing.OutCubic }
                    NumberAnimation { target: toolsT; property: "y"; to: 0; duration: 460; easing.type: Easing.OutQuint }
                }
            }
            SequentialAnimation {
                PauseAnimation { duration: 120 }
                ParallelAnimation {
                    NumberAnimation { target: panel; property: "opacity"; to: 1; duration: 380; easing.type: Easing.OutCubic }
                    NumberAnimation { target: panelT; property: "y"; to: 0; duration: 520; easing.type: Easing.OutQuint }
                }
            }
        }
    }

    Column {
        id: col
        x: (page.width - page.contentW) / 2
        y: 52
        width: page.contentW
        spacing: 14

        // ======================= 顶栏 =======================
        Rectangle {
            id: bar
            transform: Translate { id: barT }
            width: parent.width
            height: 56
            radius: 28
            color: Qt.rgba(1, 0.96, 0.9, 0.035)
            border.width: 1
            border.color: Theme.hairline

            Row {
                x: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10
                IconButton { anchors.verticalCenter: parent.verticalCenter; width: 40; height: 40; icon: "back"; iconSize: 16; onClicked: page.back() }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("查找重复")
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                    rightPadding: 6
                }
                Capsule {
                    anchors.verticalCenter: parent.verticalCenter
                    accent: libtools.dupSameSong
                    text: (accent ? "✓ " : "") + qsTr("同一首不同音质")
                    interactive: !page.working
                    onClicked: libtools.dupSameSong = !libtools.dupSameSong
                }
                Capsule {
                    anchors.verticalCenter: parent.verticalCenter
                    accent: libtools.dupSameName
                    text: (accent ? "✓ " : "") + qsTr("无标签按文件名")
                    interactive: !page.working
                    onClicked: libtools.dupSameName = !libtools.dupSameName
                }
                Capsule {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: libtools.dupSameSong || libtools.dupSameName
                    text: qsTr("时长差 ±%1 秒").arg(libtools.dupTolerance)
                    interactive: !page.working
                    onClicked: {
                        const steps = [2, 3, 5, 10]
                        const i = steps.indexOf(libtools.dupTolerance)
                        libtools.dupTolerance = steps[(i + 1) % steps.length]
                    }
                }
            }
            Row {
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                Capsule {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: libtools.canUndoDedupe && !libtools.busy
                    text: qsTr("撤销上次")
                    onClicked: libtools.undoDedupe()
                }
                Rectangle {
                    id: findBtn
                    anchors.verticalCenter: parent.verticalCenter
                    height: 34
                    radius: 17
                    width: page.working ? 210 : findText.implicitWidth + 36
                    Behavior on width { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                    color: page.working ? Theme.hover : page.hasResult ? Qt.rgba(1, 0.96, 0.9, 0.05) : Theme.accent
                    border.width: page.working ? 1 : 0
                    border.color: Theme.hairline
                    clip: true
                    opacity: libtools.busy && !page.working ? 0.35 : 1
                    scale: findMouse.pressed ? 0.96 : 1
                    Behavior on scale { ScaleAnimator { duration: Theme.fast } }
                    Rectangle {
                        visible: page.working
                        height: parent.height
                        // clip 不会按圆角裁剪:进度条自己做成胶囊,最短也是一个圆
                        radius: height / 2
                        width: Math.max(height, parent.width * libtools.progress)
                        color: Theme.accentAlpha(0.3)
                        Behavior on width { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
                    }
                    Text {
                        id: findText
                        anchors.centerIn: parent
                        width: page.working ? parent.width - 24 : implicitWidth
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        text: page.working ? (findMouse.containsMouse ? qsTr("取消") : libtools.progressText || qsTr("请稍候…"))
                            : page.hasResult ? qsTr("重新查找") : qsTr("开始查找")
                        color: page.working || page.hasResult ? Theme.text : Theme.onAccent
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                    }
                    MouseArea {
                        id: findMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: page.working || !libtools.busy
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.working ? libtools.cancel() : libtools.findDuplicates()
                    }
                }
            }
        }

        // ======================= 分类 + 处理 =======================
        Item {
            id: tools
            transform: Translate { id: toolsT }
            width: parent.width
            height: 34
            // 分类胶囊放不下时横向滚动,不压到右边的按钮
            Flickable {
                id: catFlick
                anchors.left: parent.left
                anchors.right: actRow.left
                anchors.rightMargin: 12
                height: parent.height
                contentWidth: catRow.width
                contentHeight: height
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentWidth > width
                clip: true
                visible: page.hasResult && page.sum.groups > 0
            Row {
                id: catRow
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                Repeater {
                    model: [
                        { f: "", label: qsTr("全部"), k: "" },
                        { f: "identical", label: qsTr("完全相同"), k: "identical" },
                        { f: "audio", label: qsTr("只是标签不同"), k: "audio" },
                        { f: "song", label: qsTr("不同音质"), k: "song" },
                        { f: "name", label: qsTr("文件名相同"), k: "name" },
                        { f: "versions", label: qsTr("不同版本"), k: "versions" }
                    ]
                    Capsule {
                        readonly property int n: modelData.k === "" ? (page.sum.groups || 0) : ((page.sum.byCat || {})[modelData.k] || 0)
                        visible: n > 0
                        accent: libtools.dupGroups.filter === modelData.f
                        text: modelData.label + "  " + n
                        onClicked: libtools.dupGroups.filter = modelData.f
                    }
                }
            }
            }
            Row {
                id: actRow
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                visible: page.hasResult && page.sum.groups > 0
                Capsule {
                    id: ruleCap
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("勾选规则 ▾")
                    interactive: !libtools.busy
                    onClicked: page.menu = page.menu === 1 ? 0 : 1
                }
                Capsule {
                    id: actCap
                    anchors.verticalCenter: parent.verticalCenter
                    filled: (page.sum.selected || 0) > 0
                    interactive: !libtools.busy && (page.sum.selected || 0) > 0
                    opacity: interactive ? 1 : 0.4
                    text: (page.sum.selected || 0) > 0
                        ? qsTr("处理 %1 个 · 腾出 %2 ▾").arg(page.sum.selected).arg(Theme.formatBytes(page.sum.reclaim || 0))
                        : qsTr("没有勾选要处理的")
                    onClicked: { page.confirmDelete = false; page.menu = page.menu === 2 ? 0 : 2 }
                }
            }
        }

        // ======================= 分组列表 =======================
        Rectangle {
            id: panel
            transform: Translate { id: panelT }
            width: parent.width
            height: Math.max(220, page.height - y - 52 - page.bottomInset + 20)
            radius: 22
            color: Qt.rgba(1, 0.96, 0.9, 0.025)
            border.width: 1
            border.color: Theme.hairline
            clip: true

            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: 10
                model: libtools.dupGroups
                spacing: 8
                boundsBehavior: Flickable.StopAtBounds
                cacheBuffer: 600
                SmoothScroll { flickable: list }

                delegate: Rectangle {
                    id: card
                    required property int index
                    required property var model
                    readonly property var members: model.members || []
                    width: list.width
                    height: head.height + memCol.height + 10
                    radius: 16
                    color: Qt.rgba(1, 0.96, 0.9, 0.03)

                    Item {
                        id: head
                        width: parent.width
                        height: 46
                        Capsule {
                            id: kindCap
                            x: 14
                            anchors.verticalCenter: parent.verticalCenter
                            height: 22
                            pad: 9
                            interactive: false
                            accent: card.model.cat === "identical" || card.model.cat === "audio"
                            text: card.model.kindLabel
                        }
                        Text {
                            anchors.left: kindCap.right
                            anchors.leftMargin: 10
                            anchors.right: headRight.left
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: card.model.title
                            elide: Text.ElideRight
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }
                        Row {
                            id: headRight
                            anchors.right: parent.right
                            anchors.rightMargin: 16
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 10
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: card.model.review
                                text: qsTr("请确认")
                                color: "#e2b56c"
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("%1 份 · 留 %2").arg(card.model.count).arg(card.model.keepCount)
                                    + (card.model.reclaim > 0 ? qsTr(" · 腾出 ") + Theme.formatBytes(card.model.reclaim) : "")
                                color: card.model.keepCount === 0 ? "#ff9b85" : Theme.textTertiary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }
                        }
                    }

                    Column {
                        id: memCol
                        y: head.height
                        x: 6
                        width: parent.width - 12
                        Repeater {
                            model: card.members
                            Item {
                                id: mrow
                                required property int index
                                required property var modelData
                                readonly property bool keep: modelData.keep
                                width: memCol.width
                                height: 48
                                Rectangle {
                                    anchors.fill: parent
                                    radius: 12
                                    color: Theme.hover
                                    opacity: mm.containsMouse ? 1 : 0
                                    Behavior on opacity { NumberAnimation { duration: 120 } }
                                }
                                MouseArea {
                                    id: mm
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    enabled: !libtools.busy
                                    onClicked: libtools.setKeep(card.model.src, mrow.index, !mrow.keep)
                                    onDoubleClicked: libtools.reveal(mrow.modelData.path)
                                }
                                // 保留 / 处理 切换
                                Rectangle {
                                    id: tog
                                    x: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 56
                                    height: 24
                                    radius: 12
                                    color: mrow.keep ? Theme.accentAlpha(0.16) : Qt.rgba(1, 0.45, 0.35, 0.16)
                                    Behavior on color { ColorAnimation { duration: Theme.fast } }
                                    Text {
                                        anchors.centerIn: parent
                                        text: mrow.keep ? qsTr("保留") : qsTr("处理")
                                        color: mrow.keep ? Theme.accent : "#ff9b85"
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 12
                                        font.weight: Font.DemiBold
                                    }
                                }
                                Column {
                                    anchors.left: tog.right
                                    anchors.leftMargin: 12
                                    anchors.right: mRight.left
                                    anchors.rightMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2
                                    opacity: mrow.keep ? 1 : 0.6
                                    Row {
                                        width: parent.width
                                        spacing: 8
                                        Text {
                                            id: nameText
                                            width: Math.min(implicitWidth, parent.width - (noteText.visible ? noteText.implicitWidth + 8 : 0))
                                            text: mrow.modelData.name
                                            elide: Text.ElideMiddle
                                            color: Theme.text
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 13
                                        }
                                        Text {
                                            id: noteText
                                            visible: text.length > 0
                                            text: mrow.modelData.note + (mrow.modelData.suggested ? qsTr(" · 建议保留") : "")
                                            color: mrow.modelData.suggested ? Theme.accent : Theme.textTertiary
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 12
                                        }
                                    }
                                    Text {
                                        width: parent.width
                                        text: mrow.modelData.dir
                                        elide: Text.ElideMiddle
                                        color: Theme.textTertiary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 11
                                    }
                                }
                                Row {
                                    id: mRight
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 12
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: mrow.modelData.duration
                                        color: Theme.textTertiary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 12
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 62
                                        horizontalAlignment: Text.AlignRight
                                        text: Theme.formatBytes(mrow.modelData.size)
                                        color: Theme.textTertiary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 12
                                    }
                                    IconButton {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 30; height: 30
                                        icon: "folder-open"
                                        iconSize: 14
                                        tip: qsTr("在文件夹中显示")
                                        onClicked: libtools.reveal(mrow.modelData.path)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // 空状态 / 结果
            Column {
                anchors.centerIn: parent
                width: Math.min(parent.width - 80, 520)
                spacing: 10
                visible: libtools.dupGroups.count === 0
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: page.working ? (libtools.progressText || qsTr("请稍候…"))
                        : page.hasResult && page.sum.groups > 0 ? qsTr("这一类没有重复")
                        : page.hasResult ? qsTr("查了 %1 首,没有发现重复").arg(page.sum.scanned || 0)
                        : libtools.dedupeResult.done !== undefined ? qsTr("上次处理了 %1 个,腾出 %2").arg(libtools.dedupeResult.done).arg(Theme.formatBytes(libtools.dedupeResult.freed || 0))
                        : qsTr("点「开始查找」,在整个曲库里找重复的歌")
                    wrapMode: Text.WordWrap
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    visible: !page.working && !page.hasResult
                    text: qsTr("先按大小和内容比对出完全相同的文件,再找只是标签不同、同一首不同音质的。每组会建议保留音质最好、位置最合适的一份,其余默认勾选处理。默认移到回收站,随时可以撤销;歌单和收藏会改指向保留的那份。")
                    wrapMode: Text.WordWrap
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    lineHeight: 1.3
                }
            }
        }
    }

    // ======================= 弹出菜单 =======================
    MouseArea {
        anchors.fill: parent
        z: 9
        visible: page.menu !== 0
        onClicked: page.menu = 0
    }
    Rectangle {
        id: popup
        readonly property var items: page.menu === 1 ? [
                { t: qsTr("自动 (推荐)"), d: qsTr("先看音质,再看位置和标签"), v: 0 },
                { t: qsTr("保留音质最好的"), d: qsTr("无损 > 码率高 > 体积大"), v: 1 },
                { t: qsTr("保留自己整理的文件夹里的"), d: qsTr("酷狗下载目录里的优先处理"), v: 2 },
                { t: qsTr("保留最早的"), d: "", v: 3 },
                { t: qsTr("保留最新的"), d: "", v: 4 },
                { t: qsTr("保留最大的"), d: "", v: 5 },
                { t: qsTr("保留最小的"), d: "", v: 6 }
            ] : page.menu === 2 ? [
                { t: qsTr("移到回收站"), d: qsTr("最稳妥,可以撤销"), v: 0 },
                { t: qsTr("移到隔离文件夹"), d: libtools.quarantine, v: 1, change: true },
                { t: qsTr("只从曲库移除"), d: qsTr("文件不动,以后扫描也不再收录"), v: 3 },
                { t: qsTr("换成硬链接"), d: qsTr("同一个盘里的完全相同文件:保留路径,只占一份空间"), v: 4 },
                { t: page.confirmDelete ? qsTr("再点一次确认永久删除") : qsTr("永久删除"), d: qsTr("不经过回收站,无法撤销"), v: 2, danger: true }
            ] : []
        z: 10
        width: 320
        x: col.x + col.width - width
        y: col.y + tools.y + tools.height + 6
        height: popCol.implicitHeight + 12
        radius: 16
        color: Theme.panel
        border.width: 1
        border.color: Qt.rgba(1, 0.96, 0.9, 0.09)
        opacity: page.menu !== 0 ? 1 : 0
        visible: opacity > 0
        scale: page.menu !== 0 ? 1 : 0.96
        transformOrigin: Item.TopRight
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
        Column {
            id: popCol
            x: 6; y: 6
            width: parent.width - 12
            Repeater {
                model: popup.items
                Rectangle {
                    width: popCol.width
                    height: modelData.d ? 52 : 40
                    radius: 10
                    color: pmouse.containsMouse ? Theme.hover : "transparent"
                    Column {
                        x: 12
                        width: parent.width - 24 - (chg.visible ? chg.width + 8 : 0)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            text: modelData.t
                            color: modelData.danger ? "#ff9b85" : Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                        }
                        Text {
                            visible: !!modelData.d
                            width: parent.width
                            text: modelData.d || ""
                            elide: Text.ElideMiddle
                            color: Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                        }
                    }
                    MouseArea {
                        id: pmouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (page.menu === 1) { libtools.autoSelect(modelData.v); page.menu = 0; return }
                            if (modelData.danger && !page.confirmDelete) { page.confirmDelete = true; return }
                            libtools.applyDedupe(modelData.v)
                            page.menu = 0
                        }
                    }
                    Capsule {
                        id: chg
                        visible: !!modelData.change
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        height: 26
                        pad: 10
                        text: qsTr("更改")
                        onClicked: quarantinePicker.open()
                    }
                }
            }
        }
    }
}
