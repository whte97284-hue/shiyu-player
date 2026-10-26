import QtQuick
import QtQuick.Dialogs
import PlayerUi

// 转换页 (胶囊嵌套,只用文字与一个强调色):
//  ┌ 顶栏长胶囊:转换 · 待转换数 · 处理方式 · 输出目录 ……………… 导入文件 | 文件夹 · 搜索 ┐
//  # 全部  # KGM  # NCM  # KGG  # 失败  # 已完成                              清除已完成  [转换]
//  ┌ 列表面板:格式 · 歌名 歌手 ……………………………… 大小 · 行内操作胶囊 ┐
// 曲库扫描时选择不转换的文件、手动导入还没处理的文件,都在这个列表里待命
Item {
    id: page
    property int bottomInset: 140
    readonly property int pad: width > 1400 ? 56 : 40
    readonly property real contentW: Math.min(1120, width - pad * 2)
    readonly property var st: decrypt.stats
    readonly property var q: decrypt.queue

    FileDialog {
        id: filePicker
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("加密音频 (*.kgm *.kgma *.vpr *.ncm *.kgg)")]
        onAccepted: page.feed(selectedFiles)
    }
    FolderDialog { id: folderPicker; onAccepted: page.feed([selectedFolder]) }
    FolderDialog { id: outPicker; onAccepted: decrypt.setOutputUrl(selectedFolder) }
    function feed(urls) {
        const list = []
        for (let i = 0; i < urls.length; ++i) list.push(urls[i])
        const n = decrypt.addUrls(list)
        Nav.toast("", n > 0 ? qsTr("已加入 %1 首").arg(n) : qsTr("没有新的可转换文件"))
    }

    DropArea {
        id: drop
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: (d) => { if (d.hasUrls) page.feed(d.urls); d.acceptProposedAction() }
    }

    // 进场:三层依次浮现
    property bool shownOnce: false
    onVisibleChanged: if (visible && !shownOnce) { shownOnce = true; intro.start() }
    Component.onCompleted: if (visible) { shownOnce = true; intro.start() }
    SequentialAnimation {
        id: intro
        ScriptAction { script: { bar.opacity = 0; chips.opacity = 0; panel.opacity = 0; barT.y = 10; chipsT.y = 10; panelT.y = 16 } }
        ParallelAnimation {
            OpacityAnimator { target: bar; to: 1; duration: 360; easing.type: Easing.OutCubic }
            NumberAnimation { target: barT; property: "y"; to: 0; duration: 480; easing.type: Easing.OutQuint }
            SequentialAnimation {
                PauseAnimation { duration: 70 }
                ParallelAnimation {
                    OpacityAnimator { target: chips; to: 1; duration: 360; easing.type: Easing.OutCubic }
                    NumberAnimation { target: chipsT; property: "y"; to: 0; duration: 480; easing.type: Easing.OutQuint }
                }
            }
            SequentialAnimation {
                PauseAnimation { duration: 140 }
                ParallelAnimation {
                    OpacityAnimator { target: panel; to: 1; duration: 420; easing.type: Easing.OutCubic }
                    NumberAnimation { target: panelT; property: "y"; to: 0; duration: 560; easing.type: Easing.OutQuint }
                }
            }
        }
    }

    Column {
        x: (page.width - page.contentW) / 2
        y: 52
        width: page.contentW
        spacing: 14

        // ======================= 顶栏长胶囊 =======================
        Rectangle {
            id: bar
            transform: Translate { id: barT }
            width: parent.width
            height: 56
            radius: 28
            color: Qt.rgba(1, 0.96, 0.9, 0.035)
            border.width: 1
            border.color: drop.containsDrag ? Theme.accentAlpha(0.6) : Theme.hairline
            Behavior on border.color { ColorAnimation { duration: Theme.fast } }

            Row {
                x: 22
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("转换")
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                    rightPadding: 6
                }
                Capsule {
                    anchors.verticalCenter: parent.verticalCenter
                    interactive: false
                    accent: (page.st.waiting || 0) > 0
                    text: (page.st.waiting || 0).toLocaleString(Qt.locale(), "f", 0)
                }
                Capsule {
                    id: policyCap
                    anchors.verticalCenter: parent.verticalCenter
                    readonly property var labels: [qsTr("扫描时询问"), qsTr("扫描时自动转换"), qsTr("扫描时不转换")]
                    text: labels[decrypt.policy]
                    onClicked: policyMenu.shown = !policyMenu.shown
                }
                Capsule {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("输出 · ") + decrypt.outputDir.replace(/^.*[\\/]/, "")
                    onClicked: outPicker.open()
                }
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                // 导入:外胶囊嵌两个文字钮
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    height: 36
                    width: impRow.implicitWidth + 8
                    radius: 18
                    color: Theme.hover
                    border.width: 1
                    border.color: Theme.hairline
                    Row {
                        id: impRow
                        anchors.centerIn: parent
                        spacing: 0
                        Capsule { height: 28; pad: 12; text: qsTr("导入文件"); color: "transparent"; border.width: 0; onClicked: filePicker.open() }
                        Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 1; height: 14; color: Theme.hairline }
                        Capsule { height: 28; pad: 12; text: qsTr("文件夹"); color: "transparent"; border.width: 0; onClicked: folderPicker.open() }
                    }
                }
                // 搜索
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    height: 36
                    radius: 18
                    width: search.activeFocus || search.text.length ? 240 : 168
                    Behavior on width { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                    color: Theme.hover
                    border.width: 1
                    border.color: search.activeFocus ? Theme.accentAlpha(0.5) : Theme.hairline
                    Icon { id: sIcon; x: 14; anchors.verticalCenter: parent.verticalCenter; name: "search"; size: 13; opacity: 0.45 }
                    TextInput {
                        id: search
                        anchors.left: sIcon.right
                        anchors.leftMargin: 8
                        anchors.right: parent.right
                        anchors.rightMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        color: Theme.text
                        selectionColor: Theme.accentAlpha(0.4)
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        clip: true
                        onTextChanged: page.q.filterText = text
                        Keys.onEscapePressed: { text = ""; focus = false }
                    }
                }
            }
        }

        // ======================= 标签胶囊 + 主操作 =======================
        Item {
            id: chips
            transform: Translate { id: chipsT }
            width: parent.width
            height: 32
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                Repeater {
                    model: [
                        { key: "", label: qsTr("全部"), n: page.st.all || 0 },
                        { key: "kgm", label: "KGM", n: page.st.kgm || 0 },
                        { key: "ncm", label: "NCM", n: page.st.ncm || 0 },
                        { key: "kgg", label: "KGG", n: page.st.kgg || 0 },
                        { key: "failed", label: qsTr("失败"), n: page.st.failed || 0 },
                        { key: "done", label: qsTr("已完成"), n: page.st.done || 0 }
                    ]
                    Capsule {
                        visible: modelData.key === "" || modelData.n > 0
                        accent: page.q.filterKind === modelData.key
                        text: modelData.label + "  " + modelData.n
                        onClicked: page.q.filterKind = modelData.key
                    }
                }
            }
            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                // 工具页转换的歌默认不进曲库,需要时一键加入
                Capsule {
                    visible: decrypt.addableCount > 0 && !decrypt.busy
                    accent: true
                    text: qsTr("加入曲库 · %1").arg(decrypt.addableCount)
                    onClicked: {
                        const n = decrypt.addResultsToLibrary()
                        if (n > 0) Nav.toast("check", qsTr("已加入曲库 %1 首").arg(n))
                    }
                }
                Capsule {
                    visible: (page.st.done || 0) > 0 && !decrypt.busy
                    text: qsTr("清除已完成")
                    onClicked: decrypt.clearFinished()
                }
                // 主操作:空闲 = 转换当前可见的全部;进行中 = 进度胶囊,悬停显示「取消」
                Rectangle {
                    id: mainAct
                    readonly property int pending: (page.st.waiting || 0) + (page.st.failed || 0)
                    height: 32
                    radius: 16
                    width: decrypt.busy ? 136 : goText.implicitWidth + 36
                    Behavior on width { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                    color: decrypt.busy ? Theme.hover : Theme.accent
                    border.width: decrypt.busy ? 1 : 0
                    border.color: Theme.hairline
                    clip: true
                    opacity: decrypt.busy || pending > 0 ? 1 : 0.35
                    Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
                    scale: actMouse.pressed ? 0.96 : 1
                    Behavior on scale { ScaleAnimator { duration: Theme.fast } }
                    Rectangle {
                        visible: decrypt.busy
                        height: parent.height
                        width: parent.width * decrypt.progress
                        color: Theme.accentAlpha(0.3)
                        Behavior on width { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
                    }
                    Text {
                        id: goText
                        anchors.centerIn: parent
                        text: decrypt.busy ? (actMouse.containsMouse ? qsTr("取消") : decrypt.done + " / " + decrypt.total) : qsTr("转换")
                        color: decrypt.busy ? Theme.text : Theme.onAccent
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                    }
                    MouseArea {
                        id: actMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: decrypt.busy || mainAct.pending > 0
                        cursorShape: Qt.PointingHandCursor
                        onClicked: decrypt.busy ? decrypt.cancel() : decrypt.decryptQueue(true)
                    }
                }
            }
        }

        // ======================= 列表面板 =======================
        Rectangle {
            id: panel
            transform: Translate { id: panelT }
            width: parent.width
            height: Math.max(200, page.height - y - 52 - page.bottomInset + 20)
            radius: 22
            color: Qt.rgba(1, 0.96, 0.9, 0.025)
            border.width: 1
            border.color: Theme.hairline
            clip: true

            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: 8
                model: page.q
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: true
                cacheBuffer: 400
                SmoothScroll { flickable: list }
                // 新行淡入,移除的行淡出 (模型重置时不播放)
                add: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 260 } }

                delegate: Item {
                    id: row
                    width: list.width
                    height: 54
                    readonly property bool hovered: rm.containsMouse || rmText.hovered || act.hovered
                    readonly property int status: model.status
                    readonly property bool done: status === 2
                    readonly property bool failed: status === 3
                    Rectangle {
                        anchors.fill: parent
                        radius: 14
                        color: Theme.hover
                        opacity: row.hovered ? 1 : 0
                        Behavior on opacity { OpacityAnimator { duration: 120 } }
                    }
                    MouseArea { id: rm; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }

                    Text {
                        x: 18
                        width: 46
                        anchors.verticalCenter: parent.verticalCenter
                        text: model.kind.toUpperCase()
                        color: Theme.textTertiary
                        font.family: "monospace"
                        font.pixelSize: 12
                        font.letterSpacing: 0.5
                    }
                    Row {
                        x: 76
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 76 - right.width - 24
                        spacing: 10
                        // 已完成的行整体压暗,注意力留给待转换的
                        opacity: row.done ? 0.5 : 1
                        Behavior on opacity { OpacityAnimator { duration: Theme.slow } }
                        Text {
                            id: titleText
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.min(implicitWidth, parent.width * 0.65)
                            text: model.title
                            elide: Text.ElideRight
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.min(implicitWidth, parent.width - titleText.width - 10)
                            text: model.artist
                            elide: Text.ElideRight
                            color: Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                        }
                    }

                    Row {
                        id: right
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 12
                        // 失败原因:与行内其他信息同色,只比正文淡
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: row.failed
                            width: Math.min(implicitWidth, 220)
                            text: model.message
                            elide: Text.ElideRight
                            color: Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                        Capsule {
                            id: rmText
                            anchors.verticalCenter: parent.verticalCenter
                            height: 26
                            pad: 10
                            text: qsTr("移除")
                            color: "transparent"
                            border.width: 0
                            opacity: row.hovered && row.status !== 1 ? 1 : 0
                            enabled: opacity > 0
                            Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                            onClicked: decrypt.removeFromQueue(model.path)
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 62
                            horizontalAlignment: Text.AlignRight
                            text: Theme.formatBytes(model.size)
                            color: Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                        // 行内操作:待命 → 转换;进行中 → 转换中 (强调色细进度在底边);完成 → 产物格式,点击打开位置;失败 → 重试
                        Capsule {
                            id: act
                            anchors.verticalCenter: parent.verticalCenter
                            width: 84
                            height: 28
                            accent: row.status === 0 || row.status === 1
                            interactive: row.status !== 1
                            text: row.status === 0 ? qsTr("转换")
                                : row.status === 1 ? qsTr("转换中")
                                : row.done ? model.output.replace(/^.*\./, "").toUpperCase()
                                : qsTr("重试")
                            onClicked: row.done ? decrypt.openOutput(model.output) : decrypt.decryptPaths([model.path])
                            // 转换中:胶囊里一段来回流动的强调色光带
                            Rectangle {
                                id: flow
                                visible: row.status === 1
                                y: parent.height - 3
                                height: 2
                                radius: 1
                                width: parent.width * 0.3
                                color: Theme.accent
                                XAnimator on x {
                                    running: row.status === 1 && row.visible
                                    from: 8; to: act.width - flow.width - 8
                                    duration: 700
                                    loops: Animation.Infinite
                                    easing.type: Easing.InOutSine
                                }
                            }
                        }
                    }
                }
            }

            // 空列表
            Column {
                anchors.centerIn: parent
                spacing: 8
                visible: page.q.count === 0
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: search.text.length || page.q.filterKind !== "" ? qsTr("没有匹配的文件") : qsTr("没有待转换的文件")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: search.text.length === 0 && page.q.filterKind === ""
                    text: qsTr("拖入 KGM · NCM · KGG 文件或文件夹")
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                }
            }
        }
    }

    // ---------------- 处理方式下拉 ----------------
    MouseArea {
        anchors.fill: parent
        visible: policyMenu.shown
        onClicked: policyMenu.shown = false
    }
    Rectangle {
        id: policyMenu
        property bool shown: false
        readonly property point at: policyCap.mapToItem(page, 0, policyCap.height + 8)
        x: at.x
        y: at.y
        width: 180
        height: pmCol.implicitHeight + 12
        radius: 16
        color: Theme.panel
        border.width: 1
        border.color: Qt.rgba(1, 0.96, 0.9, 0.09)
        opacity: shown ? 1 : 0
        visible: opacity > 0
        scale: shown ? 1 : 0.96
        transformOrigin: Item.TopLeft
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
        Column {
            id: pmCol
            x: 6; y: 6
            width: parent.width - 12
            Repeater {
                model: [qsTr("扫描时询问"), qsTr("扫描时自动转换"), qsTr("扫描时不转换")]
                Rectangle {
                    width: pmCol.width
                    height: 36
                    radius: 10
                    color: pmm.containsMouse ? Theme.hover : "transparent"
                    Text {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData
                        color: decrypt.policy === index ? Theme.accent : Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        font.weight: decrypt.policy === index ? Font.DemiBold : Font.Normal
                    }
                    MouseArea { id: pmm; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { decrypt.policy = index; policyMenu.shown = false } }
                }
            }
        }
    }
}
