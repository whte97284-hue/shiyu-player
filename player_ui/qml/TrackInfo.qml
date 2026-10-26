import QtQuick
import PlayerUi

// 歌曲信息:右键菜单「歌曲信息」/ Ctrl+I 打开的居中卡片
//  头部:封面 · 歌名 · 歌手 · 专辑 · 音质标签
//  分组:标签 / 音频 (含回放增益) / 文件 (文件名、所在文件夹可点开、大小、修改时间、封面来源)
//  内容超出窗口时分组区可滚动;「复制全部」以「键: 值」逐行复制,方便贴到论坛
Item {
    id: dlg
    anchors.fill: parent
    visible: card.opacity > 0
    property bool shown: false
    property real trackId: -1
    property var info: ({})

    function open(id) {
        trackId = id
        info = library.trackInfo(id)
        if (info.path === undefined) return
        shown = true
        scroller.contentY = 0
        forceActiveFocus()
    }
    function close() { shown = false }

    Keys.onEscapePressed: close()

    function db(v) { return (v > 0 ? "+" : v < 0 ? "−" : "") + Math.abs(v).toLocaleString(Qt.locale(), "f", 1) + " dB" }
    function peak(v) { return v > 0 ? qsTr("峰值 %1").arg(v.toLocaleString(Qt.locale(), "f", 2)) : "" }

    readonly property var groups: {
        const t = info
        if (t.path === undefined) return []
        const audio = []
        if (t.sampleRate > 0) audio.push((t.sampleRate / 1000).toLocaleString(Qt.locale(), "f", t.sampleRate % 1000 ? 1 : 0) + " kHz")
        if (t.bits > 0) audio.push(t.bits + " bit")
        if (t.channels > 0) audio.push(t.channels === 1 ? qsTr("单声道") : t.channels === 2 ? qsTr("立体声") : qsTr("%1 声道").arg(t.channels))
        const no = t.trackNo > 0 ? (t.discNo > 0 ? qsTr("第 %1 碟 · 第 %2 轨").arg(t.discNo).arg(t.trackNo) : qsTr("第 %1 轨").arg(t.trackNo)) : ""
        const rg = []
        if (t.hasRgTrack) rg.push(qsTr("曲目 %1").arg(db(t.rgTrackGain)) + (t.rgTrackPeak > 0 ? " (" + peak(t.rgTrackPeak) + ")" : ""))
        if (t.hasRgAlbum) rg.push(qsTr("专辑 %1").arg(db(t.rgAlbumGain)) + (t.rgAlbumPeak > 0 ? " (" + peak(t.rgAlbumPeak) + ")" : ""))
        const cover = t.coverSource === "embedded" ? qsTr("内嵌") : t.coverSource ? t.coverSource : qsTr("无")
        const clean = (rows) => rows.filter(r => r.v !== undefined && String(r.v).length > 0)
        return [
            { title: qsTr("标签"), rows: clean([
                { k: qsTr("标题"), v: t.title },
                { k: qsTr("歌手"), v: t.artist },
                { k: qsTr("专辑"), v: t.album },
                { k: qsTr("专辑歌手"), v: t.albumArtist !== t.artist ? t.albumArtist : "" },
                { k: qsTr("年份"), v: t.year },
                { k: qsTr("流派"), v: t.genre },
                { k: qsTr("碟 · 轨"), v: no }
            ]) },
            { title: qsTr("音频"), rows: clean([
                { k: qsTr("格式"), v: t.codec && t.codec !== t.format ? t.format + " / " + t.codec : t.format },
                { k: qsTr("参数"), v: audio.join("  ·  ") },
                { k: qsTr("码率"), v: t.bitrate > 0 ? t.bitrate + " kbps" : "" },
                { k: qsTr("时长"), v: t.duration },
                { k: qsTr("回放增益"), v: rg.join("  ·  ") }
            ]) },
            { title: qsTr("文件"), rows: clean([
                { k: qsTr("文件名"), v: (t.fileName || "") + (t.isCue ? qsTr("  (CUE 分轨)") : "") + (t.exists ? "" : qsTr("  (文件已不存在)")), path: true },
                { k: qsTr("所在文件夹"), v: t.folder, path: true, reveal: true },
                { k: qsTr("大小"), v: t.size > 0 ? Theme.formatBytes(t.size) : "" },
                { k: qsTr("修改时间"), v: t.modified },
                { k: qsTr("封面"), v: cover },
                { k: qsTr("加密原文件"), v: t.source || "", path: true }
            ]) }
        ].filter(g => g.rows.length > 0)
    }
    function copyAll() {
        const lines = []
        for (const g of groups) for (const r of g.rows) lines.push(r.k + ": " + r.v)
        lines.push(qsTr("完整路径") + ": " + info.path)
        library.copyToClipboard(lines.join("\n"))
        Nav.toast("check", qsTr("已复制歌曲信息"))
    }

    // 遮罩:点空白处关闭
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.42)
        opacity: dlg.shown ? 1 : 0
        Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
        MouseArea {
            anchors.fill: parent
            enabled: dlg.shown
            acceptedButtons: Qt.AllButtons
            onPressed: dlg.close()
            onWheel: (w) => {}
        }
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(540, parent.width - 48)
        height: header.height + scroller.height + buttons.height + 22 + 18 + 20 + 22
        radius: Theme.radiusL
        color: Theme.panel
        border.width: 1
        border.color: Qt.rgba(1, 0.96, 0.9, 0.09)
        opacity: dlg.shown ? 1 : 0
        scale: dlg.shown ? 1 : 0.97
        Behavior on opacity { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
        Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => {} }

        // ---- 头部:封面 + 歌名 / 歌手 · 专辑 / 音质 ----
        Item {
            id: header
            x: 24
            y: 22
            width: parent.width - 48
            height: 76
            RoundedImage {
                id: art
                width: 76; height: 76
                radius: 10
                source: dlg.info.cover || ""
                decodeSize: 152
            }
            Column {
                anchors.left: art.right
                anchors.leftMargin: 16
                anchors.right: parent.right
                anchors.verticalCenter: art.verticalCenter
                spacing: 4
                Text {
                    width: parent.width
                    text: dlg.info.title || ""
                    elide: Text.ElideRight
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }
                Text {
                    width: parent.width
                    text: (dlg.info.artist || "") + (dlg.info.album ? "  ·  " + dlg.info.album : "")
                    elide: Text.ElideRight
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                }
                Item { width: 1; height: 2 }
                QualityBadge {
                    text: dlg.info.quality || ""
                    visible: text.length > 0
                }
            }
        }

        // ---- 分组 (可滚动) ----
        Flickable {
            id: scroller
            x: 24
            anchors.top: header.bottom
            anchors.topMargin: 18
            width: parent.width - 48
            height: Math.min(contentHeight, dlg.height - 240)
            contentHeight: groupsCol.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            SmoothScroll { flickable: scroller }

            Column {
                id: groupsCol
                width: scroller.width
                spacing: 16
                Repeater {
                    model: dlg.groups
                    delegate: Column {
                        width: groupsCol.width
                        spacing: 8
                        Row {
                            width: parent.width
                            spacing: 10
                            Text {
                                id: groupTitle
                                text: modelData.title
                                color: Theme.textTertiary
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                font.letterSpacing: 1
                            }
                            Rectangle {
                                anchors.verticalCenter: groupTitle.verticalCenter
                                width: parent.width - groupTitle.width - 10
                                height: 1
                                color: Theme.hairline
                            }
                        }
                        Repeater {
                            model: modelData.rows
                            delegate: Row {
                                width: groupsCol.width
                                spacing: 14
                                Text {
                                    width: 72
                                    text: modelData.k
                                    color: Theme.textTertiary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 12
                                    topPadding: 1
                                }
                                TextEdit {
                                    id: val
                                    // 只读但可选中复制;路径优先在分隔符 / 空格处换行
                                    width: parent.width - 72 - 14
                                    text: modelData.v
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: modelData.path ? TextEdit.WrapAtWordBoundaryOrAnywhere : TextEdit.Wrap
                                    color: modelData.reveal && revealMouse.containsMouse ? Theme.accent : Theme.text
                                    selectionColor: Theme.accentAlpha(0.45)
                                    selectedTextColor: Theme.text
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 13
                                    // 所在文件夹:点击在文件管理器中显示
                                    MouseArea {
                                        id: revealMouse
                                        anchors.fill: parent
                                        enabled: modelData.reveal === true
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: if (!library.revealTrack(dlg.trackId)) Nav.toast("folder", qsTr("找不到这首歌所在的文件夹"))
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Row {
            id: buttons
            anchors.right: parent.right
            anchors.rightMargin: 24
            anchors.top: scroller.bottom
            anchors.topMargin: 20
            height: 34
            spacing: 10
            Capsule {
                height: 34
                text: qsTr("复制全部")
                onClicked: dlg.copyAll()
            }
            Capsule {
                height: 34
                text: qsTr("在文件夹中显示")
                onClicked: if (!library.revealTrack(dlg.trackId)) Nav.toast("folder", qsTr("找不到这首歌所在的文件夹"))
            }
            Capsule {
                height: 34
                filled: true
                text: qsTr("关闭")
                onClicked: dlg.close()
            }
        }
    }
}
