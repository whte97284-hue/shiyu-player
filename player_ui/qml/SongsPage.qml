import QtQuick
import PlayerUi

// 歌曲页:「全部」按标题拼音分组 (字母分节),右缘字母索引快速跳转;「最近播放」按时间倒序列出播放历史。
// 单击即以当前列表为队列播放
Item {
    id: page
    // 内容滚到窗口按钮下面了:Main 据此在顶端铺一条磨砂条
    readonly property bool scrolled: list.contentY - list.originY > 8
    property int bottomInset: 140
    readonly property int sidePadding: width > 1400 ? 56 : 40
    property bool showHistory: false
    readonly property var source: showHistory ? historyTracks : songsModel
    // 当前播放队列就是这一页的列表:播放键是暂停 / 继续
    readonly property string queueTag: showHistory ? "history" : "songs"
    readonly property bool isCurrent: player.queueTag === queueTag

    function playAll(shuffled) {
        if (source.count === 0) return
        player.shuffle = shuffled
        shuffled ? player.playTracks(source.trackIds(), Math.floor(Math.random() * source.count), queueTag)
                 : Nav.picked(player.pickTracks(source.trackIds(), 0, queueTag, true))
    }
    function openMenu(item, px, py) {
        const p = item.mapToItem(null, px, py)
        Nav.trackMenu(-1, p.x, p.y, { ids: source.trackIds(), kind: showHistory ? "history" : "songs",
                                      title: showHistory ? qsTr("最近播放") : qsTr("全部歌曲"), tag: queueTag })
    }
    onShowHistoryChanged: { list.currentIndex = -1; list.positionViewAtBeginning() }
    // 换排序:回到顶部
    Connections {
        target: songsModel
        function onSortModeChanged() { list.currentIndex = -1; Qt.callLater(list.positionViewAtBeginning) }
        function onContentChanged() { list.currentIndex = -1 }
    }
    Connections {
        target: historyTracks
        function onContentChanged() { if (page.showHistory) list.currentIndex = -1 }
    }
    function keyNav(e) {
        return Nav.listKeys(list, e, isCurrent ? source.rowOfTrack(player.trackId) : -1, bottomInset - 20,
                            (i) => Nav.picked(player.pickTracks(source.trackIds(), i, queueTag)),
                            (i, x, y) => { const it = list.currentItem; if (it) Nav.trackMenu(it.trackId, x, y, it.menuCtx()) })
    }
    readonly property var sortNames: [qsTr("按标题"), qsTr("按歌手"), qsTr("按专辑"), qsTr("最近添加"), qsTr("按时长"), qsTr("按音质")]

    // 鼠标真的移动了才取消键盘选中 (键盘翻动列表时行从静止的鼠标下经过,不算)
    HoverHandler {
        property point last
        property bool seen: false
        onPointChanged: {
            const p = point.scenePosition
            if (!seen || Math.abs(p.x - last.x) + Math.abs(p.y - last.y) < 3) { seen = true; last = p; return }
            last = p
            if (list.currentIndex >= 0) list.currentIndex = -1
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.leftMargin: page.sidePadding
        anchors.rightMargin: page.sidePadding + 18
        model: page.source
        boundsBehavior: Flickable.StopAtBounds
        bottomMargin: page.bottomInset
        cacheBuffer: 200
        reuseItems: true
        spacing: 0
        currentIndex: -1
        keyNavigationEnabled: false
        highlightFollowsCurrentItem: false
        SmoothScroll { flickable: list }

        header: Item {
            width: list.width
            height: 150
            Row {
                y: 62
                spacing: 14
                // 与专辑 / 歌手 / 歌单页同一套按钮尺寸
                IconButton {
                    width: 52; height: 52
                    filled: true
                    morphOnChange: true
                    icon: page.isCurrent && player.playing ? "pause" : "play"
                    iconSize: 20
                    tip: page.isCurrent ? (player.playing ? qsTr("暂停") : qsTr("继续播放")) : qsTr("播放")
                    onClicked: page.isCurrent ? player.toggle() : page.playAll(false)
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 42; height: 42
                    outlined: true
                    icon: "shuffle"
                    iconSize: 17
                    tip: qsTr("随机播放")
                    onClicked: page.playAll(true)
                }
                IconButton {
                    id: moreBtn
                    anchors.verticalCenter: parent.verticalCenter
                    width: 42; height: 42
                    outlined: true
                    icon: "more"
                    iconSize: 17
                    tip: qsTr("更多")
                    onClicked: page.openMenu(moreBtn, 0, height + 6)
                }
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: 10
                    spacing: 8
                    Icon { anchors.verticalCenter: parent.verticalCenter; name: "note"; size: 16; opacity: 0.45 }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: page.source.count.toLocaleString(Qt.locale(), "f", 0)
                        color: Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                    }
                }
            }
            // 排序 (只对「全部」):与「全部 / 最近播放」同一种文字样式,中间一条细竖线隔开
            Row {
                id: sortGroup
                visible: !page.showHistory
                anchors.right: tabs.left
                anchors.rightMargin: 22
                y: 74
                spacing: 22
                SortTab {
                    id: sortCap
                    text: page.sortNames[songsModel.sortMode]
                    changed: songsModel.sortMode !== 0
                    open: sortPop.shown
                    onClicked: sortPop.open(sortCap)
                }
                Item {
                    width: 1; height: 32
                    Rectangle { y: 4; width: 1; height: 14; color: Theme.hairline }
                }
            }
            // 全部 / 最近播放:两个文字标签,选中的那个用正文色并带一条强调色短线
            Row {
                id: tabs
                anchors.right: parent.right
                y: 74
                spacing: 22
                Repeater {
                    model: [{ label: qsTr("全部"), history: false }, { label: qsTr("最近播放"), history: true }]
                    Item {
                        readonly property bool active: page.showHistory === modelData.history
                        width: tabText.implicitWidth
                        height: 32
                        Text {
                            id: tabText
                            text: modelData.label
                            color: parent.active ? Theme.text : (tabMouse.containsMouse ? Theme.textSecondary : Theme.textTertiary)
                            font.family: Theme.fontFamily
                            font.pixelSize: 15
                            font.weight: parent.active ? Font.DemiBold : Font.Normal
                            Behavior on color { ColorAnimation { duration: Theme.fast } }
                        }
                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.bottom
                            width: 16; height: 2; radius: 1
                            color: Theme.accent
                            scale: parent.active ? 1 : 0
                            Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutBack } }
                        }
                        MouseArea {
                            id: tabMouse
                            anchors.fill: parent
                            anchors.margins: -6
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: page.showHistory = modelData.history
                        }
                    }
                }
            }
        }
        // 最近播放为空时的提示
        footer: Item {
            width: list.width
            height: page.showHistory && historyTracks.count === 0 ? 120 : 0
            Text {
                visible: parent.height > 0
                anchors.centerIn: parent
                text: qsTr("还没有播放记录")
                color: Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 13
            }
        }

        section.property: page.showHistory || !songsModel.sectioned ? "" : "letter"
        section.delegate: Item {
            width: list.width
            height: 44
            Text {
                x: 14
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 8
                text: section
                color: Theme.accent
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.Bold
            }
        }

        delegate: TrackRow {
            width: list.width
            readonly property string letter: page.showHistory ? "" : model.letter
            trackId: model.trackId
            number: model.number
            title: model.title
            artist: model.artist
            album: model.album
            quality: model.quality
            duration: model.duration
            cover: model.cover
            albumIndex: model.albumIndex
            showAlbum: true
            keyFocus: ListView.isCurrentItem
            onActivated: Nav.picked(player.pickTracks(page.source.trackIds(), index, page.queueTag, false, Nav.pickNow))
        }

        onContentYChanged: syncLetter.restart()
        Timer {
            id: syncLetter
            interval: 60
            onTriggered: {
                const i = list.indexAt(20, list.contentY + 60)
                const it = i >= 0 ? list.itemAtIndex(i) : null
                if (it) rail.current = it.letter
                else if (songsModel.letters.length) rail.current = songsModel.letters[0]
            }
        }
        // 还没滚动时字母条也标出当前 (第一个) 字母
        onCountChanged: syncLetter.restart()
        Component.onCompleted: syncLetter.restart()
    }

    LetterRail {
        id: rail
        visible: !page.showHistory && songsModel.letters.length > 1
        anchors.right: parent.right
        anchors.rightMargin: page.sidePadding - 18
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: -page.bottomInset / 4
        letters: songsModel.letters
        onJump: (l) => {
            const i = songsModel.indexOfLetter(l)
            if (i < 0) return
            // 分节标题落在顶部 44px 窗口按钮条下面,不和最小化 / 关闭按钮叠在一起
            list.positionViewAtIndex(i, ListView.Beginning)
            list.contentY = Math.max(list.originY, list.contentY - 52)
        }
    }

    SortPopup {
        id: sortPop
        options: page.sortNames
        current: songsModel.sortMode
        onPicked: (i) => songsModel.sortMode = i
    }
}
