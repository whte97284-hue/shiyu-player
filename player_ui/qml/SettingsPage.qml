import QtQuick
import QtQuick.Dialogs
import PlayerUi

// 设置:按分组排成若干圆角面板,每行左边是名称 + 一句说明,右边是控件,所有控件右对齐、同为 32 高。
// 控件只有四种:开关 (是 / 否)、分段选择 (几选一)、胶囊按钮 (动作)、色块 (配色)。
// 改动即时生效,没有「保存」;需要重扫 / 重启才生效的在说明里写明
Item {
    id: page
    // 内容滚到窗口按钮下面了:Main 据此在顶端铺一条磨砂条
    readonly property bool scrolled: flick.contentY - flick.originY > 8
    property int bottomInset: 140
    readonly property int pad: width > 1400 ? 56 : 40
    readonly property real contentW: Math.min(980, width - pad * 2)
    readonly property int ctrlH: 32

    FolderDialog { id: addFolder; onAccepted: library.addFolderUrls([selectedFolder]) }
    FolderDialog { id: outPicker; onAccepted: decrypt.setOutputUrl(selectedFolder) }

    FontMetrics { id: segFont; font.family: Theme.fontFamily; font.pixelSize: 13; font.weight: Font.DemiBold }
    // 识别到、还没加入的酷狗下载目录 (加入 / 忽略后刷新)
    property int kgTick: 0
    readonly property var kgCands: { library.folders; kgTick; return library.kugouCandidates() }

    // ---------------- 分组标题 ----------------
    component SectionTitle: Text {
        leftPadding: 22
        topPadding: 10
        bottomPadding: 2
        color: Theme.textTertiary
        font.family: Theme.fontFamily
        font.pixelSize: 13
        font.weight: Font.DemiBold
    }

    // ---------------- 行:名称 + 说明 | 控件 ----------------
    component SettingRow: Item {
        id: srow
        property string label
        property string hint
        property bool noSep: false       // 下方不画分隔线
        default property alias control: slot.data
        width: parent ? parent.width : 0
        height: visible ? (hint !== "" ? 62 : 52) : 0
        Column {
            x: 22
            anchors.verticalCenter: parent.verticalCenter
            width: srow.width - 44 - slot.width - 24
            spacing: 3
            Text {
                width: parent.width
                elide: Text.ElideRight
                text: srow.label
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 14
            }
            Text {
                visible: srow.hint !== ""
                width: parent.width
                elide: Text.ElideRight
                text: srow.hint
                color: Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 12
            }
        }
        Row {
            id: slot
            anchors.right: parent.right
            anchors.rightMargin: 22
            anchors.verticalCenter: parent.verticalCenter
            height: page.ctrlH
            spacing: 8
        }
    }

    // ---------------- 开关 ----------------
    component Toggle: Item {
        id: tg
        property bool checked
        signal toggled(bool v)
        width: 44
        height: page.ctrlH
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 44; height: 24; radius: 12
            color: tg.checked ? Theme.accent : Qt.rgba(1, 0.96, 0.9, tgm.containsMouse ? 0.16 : 0.11)
            Behavior on color { ColorAnimation { duration: Theme.fast } }
            Rectangle {
                y: 3
                x: tg.checked ? parent.width - width - 3 : 3
                width: 18; height: 18; radius: 9
                color: tg.checked ? Theme.onAccent : Theme.textSecondary
                Behavior on x { NumberAnimation { duration: Theme.reduced ? 0 : 200; easing.type: Easing.OutCubic } }
            }
        }
        MouseArea { id: tgm; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: tg.toggled(!tg.checked) }
    }

    // ---------------- 分段选择:一条胶囊轨道,选中块在轨道里滑动;同一控件里每格等宽 ----------------
    component Segmented: Item {
        id: seg
        property var options: []          // [{ t: 文本, v: 值 }]
        property var value
        signal picked(var v)
        readonly property int sel: {
            for (let i = 0; i < options.length; ++i) if (options[i].v === value) return i
            return -1
        }
        readonly property real cellW: {
            let w = 0
            for (let i = 0; i < options.length; ++i) w = Math.max(w, segFont.advanceWidth(options[i].t))
            return Math.max(60, Math.ceil(w) + 28)
        }
        width: options.length * cellW + 6
        height: page.ctrlH
        Rectangle { anchors.fill: parent; radius: height / 2; color: Qt.rgba(1, 0.96, 0.9, 0.05) }
        Rectangle {
            visible: seg.sel >= 0
            x: 3 + Math.max(0, seg.sel) * seg.cellW
            y: 3
            width: seg.cellW
            height: parent.height - 6
            radius: height / 2
            color: Theme.accent
            Behavior on x { NumberAnimation { duration: Theme.reduced ? 0 : 300; easing.type: Easing.OutQuint } }
        }
        Row {
            x: 3
            Repeater {
                model: seg.options
                Item {
                    width: seg.cellW
                    height: page.ctrlH
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 3
                        radius: height / 2
                        color: Qt.rgba(1, 0.96, 0.9, 0.06)
                        opacity: sm.containsMouse && index !== seg.sel ? 1 : 0
                        Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                    }
                    Text {
                        anchors.centerIn: parent
                        text: modelData.t
                        color: index === seg.sel ? Theme.onAccent : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        font.weight: index === seg.sel ? Font.DemiBold : Font.Normal
                        Behavior on color { ColorAnimation { duration: Theme.normal } }
                    }
                    MouseArea { id: sm; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: seg.picked(modelData.v) }
                }
            }
        }
    }

    // ---------------- 色块 (配色选择) ----------------
    component Swatch: Item {
        id: sw
        property color swatch
        property bool on
        signal clicked()
        width: 28
        height: page.ctrlH
        Rectangle {
            anchors.centerIn: parent
            width: 28; height: 28; radius: 14
            color: "transparent"
            border.width: 1.5
            border.color: sw.swatch
            opacity: sw.on ? 1 : 0
            Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
        }
        Rectangle {
            anchors.centerIn: parent
            width: 18; height: 18; radius: 9
            color: sw.swatch
            scale: swm.containsMouse ? 1.12 : 1
            Behavior on scale { ScaleAnimator { duration: Theme.fast } }
        }
        MouseArea { id: swm; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: sw.clicked() }
    }

    // ---------------- 面板:圆角容器,行之间细分隔线;进场时依次浮现 ----------------
    component Panel: Rectangle {
        id: pnl
        property int order: 0
        default property alias rows: col.data
        width: page.contentW
        height: col.implicitHeight + 8
        radius: 22
        color: Qt.rgba(1, 0.96, 0.9, 0.03)
        border.width: 1
        border.color: Theme.hairline
        opacity: 0
        transform: Translate { id: lift; y: 14 }
        Component.onCompleted: intro.start()
        SequentialAnimation {
            id: intro
            PauseAnimation { duration: 50 + pnl.order * 50 }
            ParallelAnimation {
                OpacityAnimator { target: pnl; to: 1; duration: 360; easing.type: Easing.OutCubic }
                NumberAnimation { target: lift; property: "y"; to: 0; duration: 500; easing.type: Easing.OutQuint }
            }
        }
        Column {
            id: col
            y: 4
            width: parent.width
        }
        function visibleAfter(i) {
            for (let j = i + 1; j < col.children.length; ++j) {
                const c = col.children[j]
                if (c.visible && c.height > 0) return true
            }
            return false
        }
        Repeater {
            model: Math.max(0, col.children.length - 1)
            Rectangle {
                readonly property Item above: col.children[index]
                x: 22
                y: above ? col.y + above.y + above.height : 0
                width: pnl.width - 44
                height: 1
                color: Theme.hairline
                // 下面还有可见的行才画;标了 noSep 的行 (文件夹列表、展开的列表) 下方不画
                visible: above && above.visible && above.height > 0 && above.noSep !== true && pnl.visibleAfter(index)
            }
        }
    }

    // 就地展开的单选列表 (输出设备):右对齐在控件下方
    component PickList: Item {
        id: pl
        property bool open: false
        property var items: []           // [{ t: 文本, v: 值 }]
        property var current
        signal picked(var v)
        width: parent ? parent.width : 0
        height: open ? plCol.implicitHeight + 12 : 0
        clip: true
        visible: height > 0
        Behavior on height { NumberAnimation { duration: Theme.reduced ? 0 : 300; easing.type: Easing.OutQuint } }
        Column {
            id: plCol
            anchors.right: parent.right
            anchors.rightMargin: 22
            spacing: 4
            opacity: pl.open ? 1 : 0
            Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
            Repeater {
                model: pl.items
                Rectangle {
                    readonly property bool cur: modelData.v === pl.current
                    width: 340
                    height: page.ctrlH
                    radius: height / 2
                    color: cur ? Theme.accent : Qt.rgba(1, 0.96, 0.9, plm.containsMouse ? 0.09 : 0.05)
                    Behavior on color { ColorAnimation { duration: Theme.fast } }
                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: 16
                        anchors.rightMargin: 16
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        text: modelData.t
                        color: parent.cur ? Theme.onAccent : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        font.weight: parent.cur ? Font.DemiBold : Font.Normal
                    }
                    MouseArea { id: plm; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { pl.picked(modelData.v); pl.open = false } }
                }
            }
        }
    }

    function shortPath(p, n) { return p.length > n ? p.slice(0, 3) + "…" + p.slice(p.length - (n - 4)) : p }
    function artistCountOf() { return library.generation >= 0 ? artistModel.rowCount() : 0 }

    Flickable {
        id: flick
        objectName: "settingsFlick"
        anchors.fill: parent
        contentHeight: content.height + 52 + page.bottomInset
        boundsBehavior: Flickable.StopAtBounds
        SmoothScroll { flickable: flick }

        Column {
            id: content
            x: (page.width - page.contentW) / 2
            y: 52
            width: page.contentW
            spacing: 10

            // ======================= 顶栏 =======================
            Panel {
                order: 0
                radius: 30
                Item {
                    width: parent.width
                    height: 52
                    Row {
                        x: 22
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 12
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("设置")
                            color: Theme.text
                            font.family: Theme.fontFamily
                            font.pixelSize: 16
                            font.weight: Font.Bold
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("%1 首  ·  %2 张专辑  ·  %3 位歌手")
                                    .arg(library.trackCount.toLocaleString(Qt.locale(), "f", 0))
                                    .arg(library.albumCount).arg(artistModel.letters.length >= 0 ? page.artistCountOf() : 0)
                            color: Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                        }
                    }
                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8
                        Capsule { interactive: false; text: "v" + settings.version }
                        Capsule { text: qsTr("数据目录"); onClicked: settings.openDataDir() }
                    }
                }
            }

            // ======================= 播放 =======================
            SectionTitle { text: qsTr("播放") }
            Panel {
                order: 1
                // 在列表 (专辑、歌单、歌手、歌曲、搜索) 里点歌或点列表的播放键时怎么放;右键菜单不受影响
                SettingRow {
                    label: qsTr("点歌后")
                    hint: (settings.clickAction === 1 ? qsTr("排到正在播放的歌后面,不打断") : qsTr("在列表里点歌马上开始放")) + qsTr(" · 右键菜单不受影响")
                    Segmented {
                        options: [{ t: qsTr("立即播放"), v: 0 }, { t: qsTr("下一首播放"), v: 1 }]
                        value: settings.clickAction
                        onPicked: (v) => settings.clickAction = v
                    }
                }
                SettingRow {
                    label: qsTr("播放队列")
                    hint: settings.queueMode === 1 ? qsTr("保留原来的队列,把歌加进去") : qsTr("换成所点的这份列表")
                    Segmented {
                        options: [{ t: qsTr("换成新列表"), v: 0 }, { t: qsTr("加到队列"), v: 1 }]
                        value: settings.queueMode
                        onPicked: (v) => settings.queueMode = v
                    }
                }
                SettingRow {
                    label: qsTr("WASAPI 独占")
                    hint: player.exclusive ? qsTr("绕过系统混音直接送往设备,其他程序暂时没声音") : qsTr("经过系统混音,可以和其他程序一起出声")
                    Toggle { checked: player.exclusive; onToggled: (v) => player.exclusive = v }
                }
                SettingRow {
                    id: devRow
                    noSep: devList.open
                    label: qsTr("输出设备")
                    hint: qsTr("切换时不中断播放")
                    Capsule {
                        text: (player.outputDevice || qsTr("系统默认")) + (devList.open ? "  ︿" : "  ﹀")
                        width: Math.min(implicitWidth, 340)
                        onClicked: { if (!devList.open) player.refreshDevices(); devList.open = !devList.open }
                    }
                }
                PickList {
                    id: devList
                    items: [{ t: qsTr("系统默认"), v: "" }].concat(player.outputDevices.map(d => ({ t: d, v: d })))
                    current: player.outputDevice
                    onPicked: (v) => player.outputDevice = v
                }
                SettingRow {
                    label: qsTr("无缝播放")
                    hint: qsTr("曲目之间不留空隙,适合现场和古典专辑")
                    Toggle { checked: player.gapless; onToggled: (v) => player.gapless = v }
                }
                SettingRow {
                    label: qsTr("音量标准化")
                    hint: qsTr("按 ReplayGain 让各首歌音量接近")
                    Segmented {
                        options: [{ t: qsTr("关闭"), v: 0 }, { t: qsTr("按曲目"), v: 1 }, { t: qsTr("按专辑"), v: 2 }]
                        value: player.replayGain
                        onPicked: (v) => player.replayGain = v
                    }
                }
                SettingRow {
                    label: qsTr("淡入淡出")
                    hint: qsTr("暂停和继续时声音渐变,不突兀")
                    Segmented {
                        options: [{ t: qsTr("关闭"), v: 0 }, { t: qsTr("短"), v: 1 }, { t: qsTr("标准"), v: 2 }, { t: qsTr("长"), v: 3 }]
                        value: settings.fadeLevel
                        onPicked: (v) => settings.fadeLevel = v
                    }
                }
                SettingRow {
                    label: qsTr("定时关闭")
                    hint: player.sleepMinutes > 0
                          ? qsTr("还剩 %1,到点淡出暂停").arg(Math.floor(player.sleepRemaining / 60) + ":" + String(player.sleepRemaining % 60).padStart(2, "0"))
                          : player.sleepMinutes === -1 ? qsTr("当前这首播完就停") : qsTr("到点淡出暂停")
                    Segmented {
                        options: [{ t: qsTr("关闭"), v: 0 }, { t: "15", v: 15 }, { t: "30", v: 30 }, { t: "60", v: 60 }, { t: "90", v: 90 }, { t: qsTr("播完本首"), v: -1 }]
                        value: player.sleepMinutes
                        onPicked: (v) => player.setSleepTimer(v)
                    }
                }
                SettingRow {
                    label: qsTr("启动时恢复播放")
                    hint: qsTr("打开时回到上次的队列和进度,先不出声")
                    Toggle { checked: settings.resume; onToggled: (v) => settings.resume = v }
                }
                SettingRow {
                    label: qsTr("播放时不睡眠")
                    hint: qsTr("播放期间电脑不自动睡眠,显示器照常关闭")
                    Toggle { checked: settings.keepAwake; onToggled: (v) => settings.keepAwake = v }
                }
            }

            // ======================= 歌词 =======================
            SectionTitle { text: qsTr("歌词") }
            Panel {
                order: 2
                SettingRow {
                    label: qsTr("歌词来源")
                    hint: qsTr("同名 .lrc 和内嵌歌词都有时用哪个")
                    Segmented {
                        options: [{ t: qsTr("LRC 优先"), v: 0 }, { t: qsTr("内嵌优先"), v: 1 }]
                        value: settings.lyricsSource
                        onPicked: (v) => { settings.lyricsSource = v; player.reloadLyrics() }
                    }
                }
                SettingRow {
                    label: qsTr("在线查找")
                    hint: qsTr("本地没有歌词时从酷狗、网易云、LRCLIB 找,并存到本地")
                    Toggle { checked: settings.onlineLyrics; onToggled: (v) => { settings.onlineLyrics = v; player.reloadLyrics() } }
                }
                SettingRow {
                    visible: settings.onlineLyrics
                    label: qsTr("外语歌补翻译")
                    hint: qsTr("本地歌词没有中文翻译时,在线找一份时间轴一致的带翻译版本;找不到就用本地的")
                    Toggle { checked: settings.lyricsTranslate; onToggled: (v) => { settings.lyricsTranslate = v; player.reloadLyrics() } }
                }
                SettingRow {
                    label: qsTr("桌面歌词")
                    hint: qsTr("置顶显示在桌面 · Ctrl+D")
                    Toggle { checked: settings.desktopLyrics; onToggled: (v) => settings.desktopLyrics = v }
                }
                SettingRow {
                    visible: settings.desktopLyrics
                    label: qsTr("锁定位置")
                    hint: qsTr("鼠标可以穿过歌词,悬停时出现解锁按钮")
                    Toggle { checked: settings.lyricsLocked; onToggled: (v) => settings.lyricsLocked = v }
                }
                SettingRow {
                    visible: settings.desktopLyrics
                    label: qsTr("字号")
                    Segmented {
                        options: [{ t: qsTr("小"), v: 26 }, { t: qsTr("中"), v: 34 }, { t: qsTr("大"), v: 44 }, { t: qsTr("特大"), v: 56 }]
                        value: settings.lyricsFontSize
                        onPicked: (v) => settings.lyricsFontSize = v
                    }
                }
                SettingRow {
                    visible: settings.desktopLyrics
                    label: qsTr("双行显示")
                    hint: qsTr("两句交替显示,有翻译时每句带翻译")
                    Toggle { checked: settings.lyricsTwoLine; onToggled: (v) => settings.lyricsTwoLine = v }
                }
                SettingRow {
                    visible: settings.desktopLyrics
                    label: qsTr("配色")
                    hint: Theme.lyricPresets[Math.max(0, Math.min(Theme.lyricPresets.length - 1, settings.lyricsColor))].name
                    Repeater {
                        model: Theme.lyricPresets
                        Swatch { swatch: modelData.c; on: settings.lyricsColor === index; onClicked: settings.lyricsColor = index }
                    }
                }
            }

            // ======================= 曲库 =======================
            SectionTitle { text: qsTr("曲库") }
            Panel {
                order: 3
                SettingRow {
                    label: qsTr("音乐文件夹")
                    noSep: library.folders.length > 0
                    hint: library.folders.length > 0 ? qsTr("%1 个文件夹,悬停可移除").arg(library.folders.length) : qsTr("还没有添加")
                    Capsule { accent: true; text: qsTr("添加文件夹"); onClicked: addFolder.open() }
                }
                // 已添加的文件夹:每个一行,路径在左,标记与移除在右
                Repeater {
                    model: library.folders
                    Item {
                        width: parent ? parent.width : 0
                        height: 40
                        readonly property bool noSep: index < library.folders.length - 1
                        readonly property bool kg: library.isKugouFolder(modelData)
                        readonly property bool hid: library.hiddenFolders.indexOf(modelData) >= 0
                        Rectangle {
                            x: 14
                            width: parent.width - 28
                            height: 34
                            anchors.verticalCenter: parent.verticalCenter
                            radius: 17
                            color: Qt.rgba(1, 0.96, 0.9, fm.containsMouse ? 0.05 : 0)
                        }
                        MouseArea { id: fm; anchors.fill: parent; hoverEnabled: true }
                        Text {
                            x: 22
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 44 - tagRow.width - 16
                            elide: Text.ElideMiddle
                            text: modelData
                            color: parent.hid ? Theme.textTertiary : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                        }
                        Row {
                            id: tagRow
                            anchors.right: parent.right
                            anchors.rightMargin: 22
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 8
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: parent.parent.kg
                                text: qsTr("酷狗")
                                color: Theme.accent
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: parent.parent.hid
                                text: qsTr("已隐藏")
                                color: Theme.textTertiary
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }
                            Capsule {
                                height: 28
                                text: parent.parent.hid ? qsTr("显示") : qsTr("隐藏")
                                opacity: fm.containsMouse || hovered ? 1 : 0
                                Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                                onClicked: library.setFolderHidden(modelData, !parent.parent.hid)
                            }
                            Capsule {
                                height: 28
                                text: qsTr("移除")
                                opacity: fm.containsMouse || hovered ? 1 : 0
                                Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                                onClicked: library.removeFolder(modelData)
                            }
                        }
                    }
                }
                SettingRow {
                    visible: page.kgCands.length > 0
                    label: qsTr("发现酷狗下载目录")
                    hint: page.kgCands.join("、")
                    Capsule { accent: true; text: qsTr("加入"); onClicked: { library.addKugouFolders(page.kgCands); page.kgTick++ } }
                    Capsule { text: qsTr("忽略"); onClicked: { library.dismissKugouCandidates(page.kgCands); page.kgTick++ } }
                }
                SettingRow {
                    visible: library.extraFileCount > 0
                    label: qsTr("单独加入的歌")
                    hint: qsTr("%1 首不在音乐文件夹里 (拖入或从转换页加入)").arg(library.extraFileCount)
                    Capsule { text: qsTr("全部移出"); onClicked: library.clearExtraFiles() }
                }
                SettingRow {
                    visible: library.excludedFileCount > 0
                    label: qsTr("已从曲库移除的文件")
                    hint: qsTr("%1 个文件在查重时选了「只从曲库移除」,磁盘上还在").arg(library.excludedFileCount)
                    Capsule { text: qsTr("恢复显示"); onClicked: library.clearExcludedFiles() }
                }
                SettingRow {
                    label: qsTr("歌单去重")
                    hint: qsTr("同一首歌只显示音质最好的一份,不动文件")
                    Toggle { checked: library.displayDedupe; onToggled: (v) => library.displayDedupe = v }
                }
                SettingRow {
                    label: qsTr("实时监控")
                    hint: qsTr("文件夹里有新增、删除或改名时自动更新")
                    Toggle { checked: library.watchFolders; onToggled: (v) => library.watchFolders = v }
                }
                SettingRow {
                    label: qsTr("扫描线程")
                    hint: qsTr("机械硬盘选 4,固态硬盘选自动")
                    Segmented {
                        options: [{ t: qsTr("自动"), v: 0 }, { t: "4", v: 4 }, { t: "8", v: 8 }, { t: "16", v: 16 }]
                        value: settings.scanThreads
                        onPicked: (v) => settings.scanThreads = v
                    }
                }
                SettingRow {
                    label: qsTr("CUE 分轨")
                    hint: qsTr("按 CUE 把整轨拆成单曲 · 改后需重扫")
                    Toggle { checked: settings.splitCue; onToggled: (v) => settings.splitCue = v }
                }
                SettingRow {
                    label: qsTr("旧编码标签")
                    hint: qsTr("老歌标签乱码时换一种 · 改后需重扫")
                    Segmented {
                        options: [{ t: qsTr("简体"), v: 936 }, { t: qsTr("繁体"), v: 950 }, { t: qsTr("日文"), v: 932 }]
                        value: settings.codePage
                        onPicked: (v) => settings.codePage = v
                    }
                }
                SettingRow {
                    label: qsTr("读取 MP3 尾部标签")
                    hint: qsTr("ID3v1 / APEv2 · 改后需重扫")
                    Toggle { checked: settings.readId3v1; onToggled: (v) => settings.readId3v1 = v }
                }
                SettingRow {
                    visible: settings.needRescan
                    label: qsTr("需要重新扫描")
                    hint: qsTr("上面几项改过,重扫后生效")
                    Capsule {
                        filled: true
                        text: library.scanning ? qsTr("扫描中…") : qsTr("立即重扫")
                        interactive: !library.scanning
                        onClicked: settings.rebuildLibrary()
                    }
                }
            }

            // ======================= 转换 =======================
            SectionTitle { text: qsTr("加密歌曲转换") }
            Panel {
                order: 4
                SettingRow {
                    label: qsTr("发现加密歌曲时")
                    hint: qsTr("音乐文件夹里的 KGM / NCM / KGG")
                    Segmented {
                        options: [{ t: qsTr("询问"), v: 0 }, { t: qsTr("自动转换"), v: 1 }, { t: qsTr("不转换"), v: 2 }]
                        value: decrypt.policy
                        onPicked: (v) => decrypt.policy = v
                    }
                }
                SettingRow {
                    label: qsTr("输出目录")
                    hint: decrypt.outputDir
                    Capsule { text: qsTr("更改"); onClicked: outPicker.open() }
                    Capsule { text: qsTr("打开"); onClicked: decrypt.openOutputFolder() }
                }
                SettingRow {
                    label: qsTr("写回标签")
                    hint: qsTr("NCM 转换后写入歌名、歌手和封面;同名歌词一起带过去")
                    Toggle { checked: settings.writeTags; onToggled: (v) => settings.writeTags = v }
                }
                SettingRow {
                    label: qsTr("转换工具")
                    hint: decrypt.decoders.map(d => d.id.toUpperCase() + " " + (d.available ? qsTr("就绪") : d.reason)).join("  ·  ")
                }
            }

            // ======================= 外观 =======================
            SectionTitle { text: qsTr("外观") }
            Panel {
                order: 5
                SettingRow {
                    label: qsTr("强调色")
                    hint: settings.accent < 0 ? qsTr("跟随当前专辑封面") : qsTr("固定颜色")
                    Capsule { accent: settings.accent < 0; text: qsTr("跟随封面"); onClicked: settings.accent = -1 }
                    Repeater {
                        model: settings.accentPresets
                        Swatch { swatch: modelData; on: settings.accent === index; onClicked: settings.accent = index }
                    }
                }
                SettingRow {
                    label: qsTr("背景")
                    hint: qsTr("没在播放时显示;播放时换成封面的模糊底图")
                    Segmented {
                        options: [{ t: qsTr("流动"), v: 0 }, { t: qsTr("静止"), v: 1 }, { t: qsTr("纯色"), v: 2 }]
                        value: settings.background
                        onPicked: (v) => settings.background = v
                    }
                }
                SettingRow {
                    label: qsTr("减弱动效")
                    hint: qsTr("缩短过渡动画,停掉流动背景和闪烁提示;歌词动画照常")
                    Toggle { checked: settings.reducedMotion; onToggled: (v) => settings.reducedMotion = v }
                }
                SettingRow {
                    label: qsTr("界面缩放")
                    hint: settings.restartNeeded ? qsTr("重启后生效") : qsTr("文字和控件整体放大或缩小")
                    Segmented {
                        options: [{ t: "90%", v: 0.9 }, { t: "100%", v: 1.0 }, { t: "110%", v: 1.1 }, { t: "125%", v: 1.25 }, { t: "150%", v: 1.5 }]
                        value: settings.uiScale
                        onPicked: (v) => settings.uiScale = v
                    }
                }
            }

            // ======================= 存储 =======================
            SectionTitle { text: qsTr("存储与记录") }
            Panel {
                order: 6
                SettingRow {
                    label: qsTr("封面缓存")
                    hint: Theme.formatBytes(settings.coverCacheBytes)
                    Capsule { text: qsTr("清除"); onClicked: settings.clearCoverCache() }
                }
                SettingRow {
                    label: qsTr("扫描缓存")
                    hint: Theme.formatBytes(settings.scanCacheBytes) + qsTr(" · 重建会重新读取全部文件")
                    Capsule {
                        text: library.scanning ? qsTr("扫描中…") : qsTr("重建")
                        interactive: !library.scanning
                        onClicked: settings.rebuildLibrary()
                    }
                }
                SettingRow {
                    id: histRow
                    property bool cleared: false
                    label: qsTr("播放记录")
                    hint: cleared ? qsTr("已清空") : qsTr("「最近播放」列表")
                    Capsule { text: qsTr("清空"); onClicked: { playlists.clearHistory(); histRow.cleared = true } }
                }
            }

            // ======================= 快捷键 =======================
            SectionTitle { text: qsTr("快捷键") }
            Panel {
                order: 7
                Grid {
                    id: keys
                    width: parent.width
                    columns: 2
                    topPadding: 6
                    bottomPadding: 6
                    readonly property var list: [
                        { k: qsTr("空格"), t: qsTr("播放 / 暂停") }, { k: "Ctrl+K", t: qsTr("搜索") },
                        { k: "Ctrl+←  →", t: qsTr("上一首 / 下一首") }, { k: "← →", t: qsTr("快退 / 快进 5 秒") },
                        { k: "Shift+←  →", t: qsTr("快退 / 快进 30 秒") }, { k: "Ctrl+↑  ↓", t: qsTr("音量") },
                        { k: "Ctrl+M", t: qsTr("静音") }, { k: "Ctrl+L", t: qsTr("收藏当前歌曲") },
                        { k: "Ctrl+P", t: qsTr("播放页") }, { k: "Ctrl+Q", t: qsTr("播放队列") },
                        { k: "Ctrl+I", t: qsTr("歌曲信息") }, { k: "Ctrl+D", t: qsTr("桌面歌词") },
                        { k: "Esc", t: qsTr("关闭 / 返回") }, { k: "Alt+←", t: qsTr("返回") },
                        { k: "PgUp  PgDn", t: qsTr("专辑墙翻页") }, { k: qsTr("直接打字"), t: qsTr("开始搜索") }
                    ]
                    Repeater {
                        model: keys.list
                        Item {
                            width: keys.width / 2
                            height: 40
                            Text {
                                x: 22
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.t
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                            }
                            Rectangle {
                                anchors.right: parent.right
                                anchors.rightMargin: 22
                                anchors.verticalCenter: parent.verticalCenter
                                width: keyText.implicitWidth + 20
                                height: 26
                                radius: 8
                                color: Qt.rgba(1, 0.96, 0.9, 0.06)
                                border.width: 1
                                border.color: Theme.hairline
                                Text {
                                    id: keyText
                                    anchors.centerIn: parent
                                    text: modelData.k
                                    color: Theme.text
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
