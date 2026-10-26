import QtQuick
import QtQuick.Dialogs
import PlayerUi

// 整理曲库 (参考 foobar2000 File Operations):按命名模板把歌移动 / 复制 / 改名到统一的目录结构。
//  ┌ 顶栏长胶囊:‹ 整理曲库 · 移动|复制|只改文件名 · 目标 ……………………………… 撤销上次 ┐
//  ┌ 模板胶囊:模板输入 · 预设 ┐  示例:F:\Music\周杰伦\2003 - 叶惠美\05 东风破.flac
//  # 选项与参与的文件夹 (可点选)
//  # 全部 · 将移动 · 已在位置 · 重复 · 问题                                   [预览] / [开始整理 N 个]
//  ┌ 列表:原位置 → 新位置 ┐
// 预览 (计算计划) 不动任何文件;开始后每一步记日志,可撤销
Item {
    id: page
    property int bottomInset: 140
    signal back()
    readonly property int pad: width > 1400 ? 56 : 40
    readonly property real contentW: Math.min(1120, width - pad * 2)
    readonly property var plan: libtools.plan
    readonly property bool hasPlan: plan.actionable !== undefined
    readonly property bool working: libtools.busy && ["plan", "organize", "undoOrganize"].indexOf(libtools.task) >= 0

    FolderDialog { id: targetPicker; title: qsTr("选择整理到的文件夹"); onAccepted: libtools.setTargetUrl(selectedFolder) }

    Connections {
        target: libtools
        function onMessage(icon, text) { if (page.visible) Nav.toast(icon, text) }
    }

    // 进场:各层依次浮现
    property bool shownOnce: false
    onVisibleChanged: if (visible && !shownOnce) { shownOnce = true; intro.start() }
    Component.onCompleted: if (visible) { shownOnce = true; intro.start() }
    SequentialAnimation {
        id: intro
        ScriptAction { script: { bar.opacity = 0; patternBox.opacity = 0; panel.opacity = 0; barT.y = 10; patT.y = 10; panelT.y = 16 } }
        ParallelAnimation {
            NumberAnimation { target: bar; property: "opacity"; to: 1; duration: 320; easing.type: Easing.OutCubic }
            NumberAnimation { target: barT; property: "y"; to: 0; duration: 460; easing.type: Easing.OutQuint }
            SequentialAnimation {
                PauseAnimation { duration: 60 }
                ParallelAnimation {
                    NumberAnimation { target: patternBox; property: "opacity"; to: 1; duration: 320; easing.type: Easing.OutCubic }
                    NumberAnimation { target: patT; property: "y"; to: 0; duration: 460; easing.type: Easing.OutQuint }
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
                    text: qsTr("整理曲库")
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                    rightPadding: 6
                }
                // 方式:外胶囊嵌三个文字钮
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    height: 36
                    width: modeRow.implicitWidth + 8
                    radius: 18
                    color: Theme.hover
                    border.width: 1
                    border.color: Theme.hairline
                    Row {
                        id: modeRow
                        anchors.centerIn: parent
                        Repeater {
                            model: [qsTr("移动"), qsTr("复制"), qsTr("只改文件名")]
                            Capsule {
                                height: 28
                                pad: 12
                                text: modelData
                                accent: libtools.mode === index
                                border.width: 0
                                color: accent ? Theme.accentAlpha(0.16) : "transparent"
                                interactive: !page.working
                                onClicked: libtools.mode = index
                            }
                        }
                    }
                }
                Capsule {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: libtools.mode !== 2
                    interactive: !page.working
                    text: qsTr("目标 · ") + (libtools.target.length ? libtools.target.replace(/^.*[\\/]/, "") || libtools.target : qsTr("选择文件夹"))
                    accent: !libtools.target.length
                    onClicked: targetPicker.open()
                }
            }
            Row {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                Capsule {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: libtools.canUndoOrganize && !libtools.busy
                    text: qsTr("撤销上次整理")
                    onClicked: libtools.undoOrganize()
                }
            }
        }

        // ======================= 命名模板 =======================
        Item {
            id: patternBox
            transform: Translate { id: patT }
            width: parent.width
            height: patCol.implicitHeight
            Column {
                id: patCol
                width: parent.width
                spacing: 10
                Row {
                    spacing: 8
                    Rectangle {
                        width: patternBox.width - presetCap.width - 8
                        height: 44
                        radius: 22
                        color: Theme.hover
                        border.width: 1
                        border.color: libtools.patternError.length ? Qt.rgba(1, 0.45, 0.35, 0.55) : patIn.activeFocus ? Theme.accentAlpha(0.5) : Theme.hairline
                        Behavior on border.color { ColorAnimation { duration: Theme.fast } }
                        Text {
                            id: patLabel
                            x: 18
                            anchors.verticalCenter: parent.verticalCenter
                            text: libtools.mode === 2 ? qsTr("文件名") : qsTr("模板")
                            color: Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                        }
                        TextInput {
                            id: patIn
                            anchors.left: patLabel.right
                            anchors.leftMargin: 12
                            anchors.right: parent.right
                            anchors.rightMargin: 18
                            anchors.verticalCenter: parent.verticalCenter
                            text: libtools.pattern
                            color: Theme.text
                            selectionColor: Theme.accentAlpha(0.4)
                            selectByMouse: true
                            font.family: "monospace"
                            font.pixelSize: 13
                            clip: true
                            readOnly: page.working
                            // 停止输入 400ms 后再应用 (每次应用都会刷新示例并作废旧计划)
                            onTextEdited: applyPattern.restart()
                            onEditingFinished: { applyPattern.stop(); libtools.pattern = text }
                            Timer { id: applyPattern; interval: 400; onTriggered: libtools.pattern = patIn.text }
                        }
                    }
                    Capsule {
                        id: presetCap
                        height: 44
                        pad: 18
                        text: qsTr("预设")
                        interactive: !page.working
                        onClicked: presetMenu.shown = !presetMenu.shown
                    }
                }
                Row {
                    x: 18
                    spacing: 10
                    width: parent.width - 36
                    Text {
                        text: libtools.patternError.length ? qsTr("模板有误") : qsTr("示例")
                        color: libtools.patternError.length ? "#ff9b85" : Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }
                    Text {
                        width: parent.width - 60
                        text: libtools.patternError.length ? libtools.patternError : libtools.patternPreview
                        elide: Text.ElideMiddle
                        color: libtools.patternError.length ? "#ff9b85" : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }
                }
                // 选项 · 参与的文件夹
                Flow {
                    width: parent.width
                    spacing: 8
                    Repeater {
                        model: [
                            { key: "companions", label: qsTr("歌词跟着走") },
                            { key: "folderArt", label: qsTr("封面跟着走") },
                            { key: "removeEmpty", label: qsTr("清理空文件夹") },
                            { key: "fullWidth", label: qsTr("非法字符用全角") }
                        ]
                        Capsule {
                            visible: libtools.mode !== 2 || modelData.key === "companions" || modelData.key === "fullWidth"
                            height: 30
                            accent: libtools[modelData.key]
                            text: (accent ? "✓ " : "") + modelData.label
                            interactive: !page.working
                            onClicked: libtools[modelData.key] = !libtools[modelData.key]
                        }
                    }
                    Rectangle { width: 1; height: 30; color: "transparent" }
                    Text {
                        height: 30
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("整理这些文件夹")
                        color: Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        rightPadding: 4
                    }
                    Repeater {
                        model: library.folders
                        Capsule {
                            height: 30
                            readonly property bool on: libtools.sources.indexOf(modelData) >= 0
                            accent: on
                            text: (on ? "✓ " : "") + (modelData.replace(/^.*[\\/]/, "") || modelData) + (library.isKugouFolder(modelData) ? qsTr(" · 酷狗") : "")
                            interactive: !page.working
                            onClicked: libtools.toggleSource(modelData)
                        }
                    }
                }
            }
        }

        // ======================= 汇总 + 主操作 =======================
        Item {
            width: parent.width
            height: 32
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                visible: page.hasPlan
                Repeater {
                    model: [
                        { f: "", label: qsTr("全部"), n: libtools.planItems.count >= 0 && page.hasPlan ? (page.plan.toMove + page.plan.toCopy + page.plan.unchanged + page.plan.duplicates + page.plan.errors + page.plan.excluded) : 0, show: true },
                        { f: "change", label: libtools.mode === 1 ? qsTr("将复制") : libtools.mode === 2 ? qsTr("将改名") : qsTr("将移动"), n: page.plan.actionable || 0, show: true },
                        { f: "same", label: qsTr("已在位置"), n: page.plan.unchanged || 0, show: (page.plan.unchanged || 0) > 0 },
                        { f: "dup", label: qsTr("重复"), n: page.plan.duplicates || 0, show: (page.plan.duplicates || 0) > 0 },
                        { f: "error,excluded", label: qsTr("问题"), n: (page.plan.errors || 0) + (page.plan.excluded || 0), show: (page.plan.errors || 0) + (page.plan.excluded || 0) > 0 }
                    ]
                    Capsule {
                        visible: modelData.show
                        accent: libtools.planItems.filter === modelData.f
                        text: modelData.label + "  " + modelData.n
                        onClicked: libtools.planItems.filter = modelData.f
                    }
                }
            }
            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: page.hasPlan && !page.working
                    text: (page.plan.bytes > 0 ? Theme.formatBytes(page.plan.bytes) : "") + (page.plan.companions > 0 ? qsTr(" · 含 %1 个歌词等附属文件").arg(page.plan.companions) : "")
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                }
                Capsule {
                    visible: page.hasPlan && !page.working
                    text: qsTr("重新预览")
                    onClicked: libtools.buildPlan()
                }
                // 主操作:无计划 = 预览;有计划 = 开始;进行中 = 进度胶囊,悬停显示「取消」
                Rectangle {
                    id: mainAct
                    readonly property bool ready: page.hasPlan && page.plan.actionable > 0 && page.plan.ok
                    height: 32
                    radius: 16
                    width: page.working ? 168 : goText.implicitWidth + 36
                    Behavior on width { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                    color: page.working ? Theme.hover : Theme.accent
                    border.width: page.working ? 1 : 0
                    border.color: Theme.hairline
                    clip: true
                    opacity: page.working || !page.hasPlan || ready ? (libtools.busy && !page.working ? 0.35 : 1) : 0.35
                    Behavior on opacity { NumberAnimation { duration: Theme.normal } }
                    scale: actMouse.pressed ? 0.96 : 1
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
                        id: goText
                        anchors.centerIn: parent
                        width: page.working ? parent.width - 24 : implicitWidth
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        text: page.working ? (actMouse.containsMouse && (libtools.task === "organize" || libtools.task === "plan") ? qsTr("取消") : libtools.progressText || qsTr("请稍候…"))
                            : page.hasPlan ? (page.plan.actionable > 0 ? qsTr("开始整理 · %1 个").arg(page.plan.actionable) : qsTr("都已整理好"))
                            : qsTr("预览")
                        color: page.working ? Theme.text : Theme.onAccent
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                    }
                    MouseArea {
                        id: actMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: page.working ? libtools.task === "organize" || libtools.task === "plan" : !libtools.busy && (!page.hasPlan || mainAct.ready)
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.working ? libtools.cancel() : page.hasPlan ? libtools.execute() : libtools.buildPlan()
                    }
                }
            }
        }

        // ======================= 列表 =======================
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

            // 提醒:空间不足、截短、缺标签
            Column {
                id: warnCol
                x: 22
                y: visible ? 14 : 0
                width: parent.width - 44
                spacing: 4
                visible: page.hasPlan && ((page.plan.warnings || []).length > 0)
                Repeater {
                    model: page.plan.warnings || []
                    Text {
                        width: warnCol.width
                        text: "· " + modelData
                        elide: Text.ElideRight
                        color: modelData.indexOf("空间不足") >= 0 ? "#ff9b85" : Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }
                }
            }

            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: 8
                anchors.topMargin: warnCol.visible ? warnCol.height + 22 : 8
                model: libtools.planItems
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: true
                cacheBuffer: 400
                SmoothScroll { flickable: list }

                delegate: Item {
                    id: row
                    width: list.width
                    height: 54
                    Rectangle {
                        anchors.fill: parent
                        radius: 14
                        color: Theme.hover
                        opacity: rowMouse.containsMouse ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 120 } }
                    }
                    MouseArea { id: rowMouse; anchors.fill: parent; hoverEnabled: true; onDoubleClicked: libtools.reveal(model.cat === "change" ? model.from : (model.to || model.from)) }

                    Text {
                        x: 16
                        width: 30
                        anchors.verticalCenter: parent.verticalCenter
                        text: model.kind === "audio" ? qsTr("歌") : model.kind === "art" ? qsTr("图") : qsTr("附")
                        color: model.kind === "audio" ? Theme.textSecondary : Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }
                    Column {
                        x: 52
                        width: parent.width - 52 - right.width - 24
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        opacity: model.cat === "change" ? 1 : 0.55
                        // 新位置在上 (要去哪),原位置在下
                        Text {
                            width: parent.width
                            text: model.cat === "change" || model.cat === "same" ? model.toRel : model.fromRel
                            elide: Text.ElideMiddle
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                        }
                        Text {
                            width: parent.width
                            text: model.cat === "change" ? qsTr("← ") + model.fromRel : model.note
                            elide: Text.ElideMiddle
                            color: Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                        }
                    }
                    Row {
                        id: right
                        anchors.right: parent.right
                        anchors.rightMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 10
                        Capsule {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: model.renamed || model.cross || model.cat === "dup" || model.cat === "error" || model.cat === "excluded"
                            interactive: false
                            height: 22
                            pad: 9
                            accent: model.cat === "error"
                            text: model.cat === "dup" ? qsTr("重复") : model.cat === "error" ? qsTr("无法处理") : model.cat === "excluded" ? qsTr("跳过")
                                : model.renamed ? qsTr("重名加序号") : qsTr("跨盘")
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
                    }
                }
            }

            // 空状态 / 结果
            Column {
                anchors.centerIn: parent
                width: Math.min(parent.width - 80, 520)
                spacing: 10
                visible: libtools.planItems.count === 0
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: page.working ? (libtools.progressText || qsTr("请稍候…"))
                        : libtools.organizeResult.moved !== undefined ? qsTr("上次整理:移动 %1 · 复制 %2 · 失败 %3").arg(libtools.organizeResult.moved).arg(libtools.organizeResult.copied).arg(libtools.organizeResult.failed)
                        : page.hasPlan ? qsTr("这一类没有文件")
                        : qsTr("设置好模板和目标文件夹,点「预览」看看每首歌会去哪里")
                    wrapMode: Text.WordWrap
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    visible: !page.working && !page.hasPlan
                    text: qsTr("预览不会动任何文件。整理时同名歌词、封面一起搬;重名自动加序号;跨盘先复制校验再删原文件;每一步都有记录,可以撤销。歌单、收藏和播放记录会跟着新位置走。")
                    wrapMode: Text.WordWrap
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    lineHeight: 1.3
                }
                Repeater {
                    model: !page.working && !page.hasPlan ? (libtools.organizeResult.failures || []).slice(0, 6) : []
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: modelData.path.replace(/^.*[\\/]/, "") + " · " + modelData.reason
                        elide: Text.ElideMiddle
                        color: "#ff9b85"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }
                }
            }
        }
    }

    // 预设菜单
    Rectangle {
        id: presetMenu
        property bool shown: false
        z: 10
        x: col.x + col.width - width
        y: col.y + patternBox.y + 50
        width: 300
        height: presetCol.implicitHeight + 12
        radius: 16
        color: Theme.panel
        border.width: 1
        border.color: Qt.rgba(1, 0.96, 0.9, 0.09)
        opacity: shown ? 1 : 0
        visible: opacity > 0
        scale: shown ? 1 : 0.96
        transformOrigin: Item.TopRight
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
        Column {
            id: presetCol
            x: 6; y: 6
            width: parent.width - 12
            Repeater {
                model: libtools.presets
                Rectangle {
                    width: presetCol.width
                    height: 40
                    radius: 10
                    color: pm.containsMouse ? Theme.hover : "transparent"
                    Text {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.name
                        color: libtools.pattern === modelData.pattern ? Theme.accent : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                    }
                    MouseArea {
                        id: pm
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            libtools.pattern = modelData.pattern
                            if (index === 4) libtools.mode = 2
                            presetMenu.shown = false
                        }
                    }
                }
            }
        }
    }
    MouseArea {
        anchors.fill: parent
        z: 9
        visible: presetMenu.shown
        onClicked: presetMenu.shown = false
    }
}
