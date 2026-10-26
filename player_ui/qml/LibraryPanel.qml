import QtQuick
import PlayerUi

// 音乐文件夹管理:从侧栏弹出的卡片。列出已添加的文件夹 (可移除),添加 / 重新扫描,整理曲库 / 查找重复
// 打开:卡片从下方 10px 处浮上来并轻微放大 (透明度用 NumberAnimation,随帧更新,
// 否则 visible 绑定在 Animator 结束前一直读到 0,卡片会在动画结束时才突然出现)
Item {
    id: lp
    property bool shown: false
    signal pickFolder()
    signal openTool(string key)
    anchors.fill: parent
    visible: card.opacity > 0

    function open() { shown = true }
    function close() { shown = false }
    function artistCount() { return library.generation >= 0 ? artistModel.rowCount() : 0 }

    MouseArea {
        anchors.fill: parent
        enabled: lp.shown
        onClicked: lp.close()
        onWheel: (w) => w.accepted = true
    }

    Rectangle {
        id: card
        x: Theme.navWidth + 12
        // 从侧栏底部的文件夹按钮旁弹出
        y: lp.height - height - Theme.islandHeight - 14 // 停在底部播放条上方
        width: 540
        height: body.implicitHeight + 36
        radius: Theme.radiusL
        color: Theme.panel
        border.width: 1
        border.color: Qt.rgba(1, 0.96, 0.9, 0.1)
        opacity: lp.shown ? 1 : 0
        scale: lp.shown ? 1 : 0.97
        transformOrigin: Item.BottomLeft
        // 透明度、缩放、位移全部用同一种动画、同一时长与曲线,在同一线程上逐帧推进;
        // 混用 ScaleAnimator (渲染线程) 和 NumberAnimation (主线程) 时两者不同步,结束时会跳一下
        transform: Translate { y: lp.shown ? 0 : 8; Behavior on y { NumberAnimation { duration: lp.shown ? 260 : 140; easing.type: lp.shown ? Easing.OutCubic : Easing.InCubic } } }
        Behavior on opacity { NumberAnimation { duration: lp.shown ? 200 : 120; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: lp.shown ? 260 : 140; easing.type: lp.shown ? Easing.OutCubic : Easing.InCubic } }
        // 吞掉卡片内的点击,避免穿透到背后的关闭区域
        MouseArea { anchors.fill: parent }

        Column {
            id: body
            x: 20
            y: 18
            width: parent.width - 40
            spacing: 0

            // 概览:曲目 / 专辑数量 (图标 + 数字)
            Row {
                spacing: 18
                height: 28
                Repeater {
                    model: [{ icon: "note", n: library.trackCount }, { icon: "albums", n: library.albumCount }, { icon: "artist", n: artistModel.letters.length >= 0 ? artistCount() : 0 }]
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 7
                        Icon { anchors.verticalCenter: parent.verticalCenter; name: modelData.icon; size: 15; opacity: 0.5 }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.n.toLocaleString(Qt.locale(), "f", 0)
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                        }
                    }
                }
            }
            Item { width: 1; height: 10 }

            Repeater {
                model: library.folders
                delegate: Item {
                    id: folderRow
                    width: body.width
                    // 有备注时两行:备注在上,路径在下 (更淡更小)
                    readonly property string note: library.folderNotes[modelData] || ""
                    property bool editing: false
                    height: note.length || editing ? 52 : 44
                    Behavior on height { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                    function beginEdit() { noteInput.text = note; editing = true; noteInput.forceActiveFocus(); noteInput.selectAll() }
                    function commit() { if (!editing) return; editing = false; library.setFolderNote(modelData, noteInput.text) }
                    Rectangle {
                        anchors.fill: parent
                        anchors.leftMargin: -8
                        anchors.rightMargin: -8
                        radius: Theme.radiusS
                        color: rowMouse.containsMouse || folderRow.editing ? Theme.hover : "transparent"
                    }
                    MouseArea { id: rowMouse; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
                    Icon {
                        id: fIcon
                        anchors.verticalCenter: parent.verticalCenter
                        name: "folder"
                        size: 17
                        tint: folderRow.note.length ? Theme.accent : "transparent"
                        opacity: folderRow.note.length ? 0.9 : 0.5
                    }
                    readonly property bool hiddenFolder: library.hiddenFolders.indexOf(modelData) >= 0
                    Column {
                        anchors.left: fIcon.right
                        anchors.leftMargin: 12
                        anchors.right: editBtn.left
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        opacity: folderRow.hiddenFolder ? 0.45 : 1
                        Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
                        // 备注:点铅笔或双击这一行就地编辑,回车保存,Esc 取消,清空即删除备注
                        Item {
                            width: parent.width
                            height: noteText.implicitHeight
                            visible: folderRow.note.length > 0 || folderRow.editing
                            Text {
                                id: noteText
                                width: parent.width
                                visible: !folderRow.editing
                                text: folderRow.note
                                elide: Text.ElideRight
                                color: Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                            TextInput {
                                id: noteInput
                                anchors.fill: parent
                                visible: folderRow.editing
                                color: Theme.text
                                selectionColor: Theme.accentAlpha(0.4)
                                selectedTextColor: Theme.text
                                selectByMouse: true
                                clip: true
                                maximumLength: 40
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                                Keys.onReturnPressed: folderRow.commit()
                                Keys.onEnterPressed: folderRow.commit()
                                Keys.onEscapePressed: { folderRow.editing = false; focus = false }
                                onActiveFocusChanged: if (!activeFocus) folderRow.commit()
                                Text {
                                    visible: !noteInput.text.length
                                    text: qsTr("备注,如「无损收藏」「酷狗下载」")
                                    color: Theme.textTertiary
                                    font: noteInput.font
                                }
                            }
                        }
                        Text {
                            width: parent.width
                            text: modelData
                            elide: Text.ElideMiddle
                            color: folderRow.note.length || folderRow.editing ? Theme.textTertiary : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: folderRow.note.length || folderRow.editing ? 11 : 13
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        anchors.rightMargin: rm.width + hideBtn.width + editBtn.width + 12
                        acceptedButtons: Qt.LeftButton
                        enabled: !folderRow.editing
                        onDoubleClicked: folderRow.beginEdit()
                    }
                    IconButton {
                        id: editBtn
                        anchors.right: hideBtn.left
                        anchors.rightMargin: 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: 30; height: 30
                        icon: "edit"
                        iconSize: 13
                        opacity: rowMouse.containsMouse || folderRow.editing ? 1 : 0
                        Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                        onClicked: folderRow.editing ? folderRow.commit() : folderRow.beginEdit()
                    }
                    // 隐藏 / 显示:隐藏后此文件夹的歌不出现在专辑、歌手、歌曲和搜索里,
                    // 收藏与歌单里已有的照常可见。只有一个文件夹时没有意义,不显示
                    Rectangle {
                        id: hideBtn
                        visible: library.folders.length > 1 || parent.hiddenFolder
                        anchors.right: rm.left
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        height: 24
                        width: visible ? hideText.implicitWidth + 18 : 0
                        radius: 12
                        color: parent.hiddenFolder ? Theme.accentAlpha(0.16) : (hideMouse.containsMouse ? Theme.hover : "transparent")
                        border.width: 1
                        border.color: parent.hiddenFolder ? Theme.accentAlpha(0.4) : Theme.hairline
                        opacity: parent.hiddenFolder || rowMouse.containsMouse || hideMouse.containsMouse ? 1 : 0.5
                        Behavior on color { ColorAnimation { duration: Theme.fast } }
                        Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                        Text {
                            id: hideText
                            anchors.centerIn: parent
                            text: parent.parent.hiddenFolder ? qsTr("已隐藏") : qsTr("隐藏")
                            color: parent.parent.hiddenFolder ? Theme.accent : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                        }
                        MouseArea {
                            id: hideMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: library.setFolderHidden(modelData, !parent.parent.hiddenFolder)
                        }
                    }
                    IconButton {
                        id: rm
                        anchors.right: parent.right
                        anchors.rightMargin: -6
                        anchors.verticalCenter: parent.verticalCenter
                        width: 30; height: 30
                        icon: "close"
                        iconSize: 13
                        opacity: rowMouse.containsMouse ? 1 : 0.35
                        Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                        onClicked: library.removeFolder(modelData)
                    }
                }
            }


            Item { width: 1; height: 12 }
            Rectangle { width: body.width; height: 1; color: Theme.hairline }
            Item { width: 1; height: 12 }

            Row {
                spacing: 12
                IconButton {
                    width: 44; height: 44
                    filled: true
                    icon: "plus"
                    iconSize: 18
                    onClicked: { lp.close(); lp.pickFolder() }
                }
                // 重新扫描;扫描进行中变为取消
                IconButton {
                    visible: library.folders.length > 0
                    anchors.verticalCenter: parent.verticalCenter
                    width: 40; height: 40
                    outlined: true
                    icon: library.scanning ? "close" : "refresh"
                    iconSize: 16
                    onClicked: library.scanning ? library.cancelScan() : library.rescan()
                }
                Rectangle { visible: library.folders.length > 0; anchors.verticalCenter: parent.verticalCenter; width: 1; height: 20; color: Theme.hairline }
                // 整理曲库 / 查找重复:各自进入单独的页面 (先预览,确认后才动文件)
                Repeater {
                    model: library.folders.length > 0 ? [
                        { key: "organize", icon: "organize", text: qsTr("整理曲库") },
                        { key: "dedupe", icon: "duplicate", text: qsTr("查找重复") }
                    ] : []
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        height: 40
                        width: toolRow.implicitWidth + 30
                        radius: 20
                        color: toolMouse.containsMouse ? Theme.pressed : Theme.hover
                        border.width: 1
                        border.color: Theme.hairline
                        scale: toolMouse.pressed ? 0.96 : 1
                        Behavior on scale { ScaleAnimator { duration: Theme.fast; easing.type: Easing.OutCubic } }
                        Row {
                            id: toolRow
                            anchors.centerIn: parent
                            spacing: 7
                            Icon { anchors.verticalCenter: parent.verticalCenter; name: modelData.icon; size: 15; opacity: 0.75 }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.text
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                            }
                        }
                        // 后台任务进行中:右上角一个强调色小点
                        Rectangle {
                            visible: libtools.busy && (modelData.key === "organize" ? ["plan", "organize", "undoOrganize"] : ["find", "apply", "undoDedupe"]).indexOf(libtools.task) >= 0
                            x: parent.width - 12; y: 6
                            width: 6; height: 6; radius: 3
                            color: Theme.accent
                        }
                        MouseArea {
                            id: toolMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { lp.close(); lp.openTool(modelData.key) }
                        }
                    }
                }
                // 歌单去重:只影响显示 (同一首歌的多个副本只显示音质最好的一份),不动任何文件
                Rectangle {
                    id: dedupeToggle
                    visible: library.folders.length > 0
                    anchors.verticalCenter: parent.verticalCenter
                    readonly property bool on: library.displayDedupe
                    height: 40
                    width: toggleRow.implicitWidth + 28
                    radius: 20
                    color: toggleMouse.containsMouse ? Theme.pressed : Theme.hover
                    border.width: 1
                    border.color: Theme.hairline
                    Row {
                        id: toggleRow
                        anchors.centerIn: parent
                        spacing: 8
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("歌单去重")
                            color: dedupeToggle.on ? Theme.text : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                        }
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 30; height: 18; radius: 9
                            color: dedupeToggle.on ? Theme.accent : Qt.rgba(1, 0.96, 0.9, 0.12)
                            Behavior on color { ColorAnimation { duration: Theme.fast } }
                            Rectangle {
                                width: 14; height: 14; radius: 7
                                y: 2
                                x: dedupeToggle.on ? parent.width - width - 2 : 2
                                color: dedupeToggle.on ? Theme.onAccent : Theme.textSecondary
                                Behavior on x { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                            }
                        }
                    }
                    MouseArea {
                        id: toggleMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: library.displayDedupe = !library.displayDedupe
                    }
                }
            }
        }
    }
}
