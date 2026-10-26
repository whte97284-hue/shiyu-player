import QtQuick
import PlayerUi

// 专辑墙顶部横幅 (杂志跨页式):正在播放时展示当前专辑,否则展示「今日推荐」
// 封套 + 半露的黑胶 (播放时滑出更多并旋转),大标题,行动按钮;底部是专辑墙的筛选 / 排序工具栏
Item {
    id: hero
    signal openAlbum(int index, Item coverItem)
    signal sortMenu(Item anchor)
    // 打开专辑封面面板 (锚点是触发它的按钮)
    signal coverPanel(Item anchor)
    property bool sortOpen: false   // 排序菜单开着 (箭头朝上、文字强调色)
    // 网格可见高度 (去掉底部播放栏):窗口矮时封面跟着缩小,第一排专辑不被挤出视野
    property real availableHeight: 640
    // 「换一张」:今日推荐依次往后挑
    property int reroll: 0
    readonly property bool live: player.hasTrack && player.albumIndex >= 0
    // 依赖 library.generation:扫描完成 (版本号变化) 后重新挑选推荐专辑
    property int albumIndex: live ? player.albumIndex : (library.generation >= 0 ? library.featuredAlbum(reroll) : -1)
    property var info: library.generation >= 0 ? library.albumInfo(albumIndex) : ({})
    readonly property bool hasAlbum: info && info.title !== undefined
    readonly property real s: Math.round(Math.min(260, Math.max(168, Math.min(width * 0.21, (availableHeight - 290) * 0.66))))
    readonly property int artTop: 24
    readonly property real textX: s * 1.40 + 56
    // 矮窗口:文字列收紧,第一排专辑多露出一些
    readonly property bool tight: availableHeight < 560
    readonly property int artistIdx: library.generation >= 0 && hasAlbum ? library.artistIndexOf(info.artist || "") : -1
    // 封套与文字列顶端对齐;文字列比封套高时 (矮窗口) 横幅随之加高,下方内容不被盖住
    readonly property real block: Math.max(s, textCol.implicitHeight)
    height: (hasAlbum ? artTop + block + 34 : 24) + 36 + 14
    readonly property Item coverItem: sleeve
    readonly property Item sortAnchor: sortCap

    function openMenu(item, px, py) {
        const p = item.mapToItem(null, px, py)
        Nav.trackMenu(-1, p.x, p.y, { albumIndex: hero.albumIndex })
    }

    Connections {
        target: library
        function onLibraryChanged() { hero.info = library.albumInfo(hero.albumIndex) }
    }
    onAlbumIndexChanged: info = library.albumInfo(albumIndex)

    Item {
        id: art
        visible: hero.hasAlbum
        y: hero.artTop
        width: hero.s
        height: hero.s
        Vinyl {
            width: hero.s * 0.94
            height: width
            y: (hero.s - height) / 2
            x: hero.live && player.playing ? hero.s * 0.40 : hero.s * 0.22
            Behavior on x { enabled: !Theme.reduced; NumberAnimation { duration: 1000; easing.type: Easing.OutCubic } }
            cover: hero.info.cover || ""
            spinning: hero.live && player.playing
            labelSize: 256
        }
        SoftShadow { target: sleeve; spread: 36; offsetY: 20 }
        CrossfadeCover {
            id: sleeve
            width: hero.s
            height: hero.s
            radius: 10
            source: hero.info.cover || ""
            decodeSize: 640
        }
        MouseArea {
            id: sleeveMouse
            anchors.fill: sleeve
            cursorShape: Qt.PointingHandCursor
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: (e) => {
                if (e.button === Qt.RightButton) hero.openMenu(sleeveMouse, e.x, e.y)
                else hero.openAlbum(hero.albumIndex, sleeve)
            }
        }
    }

    Column {
        id: textCol
        visible: hero.hasAlbum
        x: hero.textX
        width: hero.width - x - (shelf.visible ? shelf.width + 48 : 0)
        anchors.top: art.top
        spacing: hero.tight ? 6 : 10

        // 说明这里展示的是什么:正在播放 / 上次播放 / 今日推荐
        Row {
            spacing: 8
            height: 16
            PlayingBars { anchors.verticalCenter: parent.verticalCenter; visible: hero.live; maxHeight: 12 }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: hero.live ? (player.playing ? qsTr("正在播放") : qsTr("上次播放")) : qsTr("今日推荐")
                color: Theme.accent
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.DemiBold
                font.letterSpacing: 1.2
            }
        }
        Text {
            id: titleText
            width: parent.width
            text: hero.info.title || ""
            wrapMode: Text.Wrap
            maximumLineCount: hero.tight ? 1 : 2
            elide: Text.ElideRight
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: Math.round(Math.min(54, Math.max(32, Math.min(hero.width * 0.04, hero.s * 0.21))))
            font.weight: Font.Bold
            font.letterSpacing: -0.8
            lineHeight: 1.04
            MouseArea {
                width: Math.min(parent.width, parent.contentWidth)
                height: parent.height
                cursorShape: Qt.PointingHandCursor
                onClicked: hero.openAlbum(hero.albumIndex, sleeve)
            }
        }
        Text {
            id: artistText
            width: Math.min(implicitWidth, parent.width)
            text: hero.info.artist || ""
            elide: Text.ElideRight
            color: artistMouse.containsMouse ? Theme.text : Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: 18
            font.weight: Font.Medium
            font.underline: artistMouse.containsMouse
            MouseArea {
                id: artistMouse
                anchors.fill: parent
                enabled: hero.artistIdx >= 0
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: Nav.openArtist(hero.artistIdx)
            }
        }
        Row {
            width: parent.width
            spacing: 12
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - (heroBadge.visible ? heroBadge.width + 12 : 0))
                elide: Text.ElideRight
                text: hero.info.summary || ""
                color: Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 13
            }
            QualityBadge { id: heroBadge; text: hero.info.badge || ""; anchors.verticalCenter: parent.verticalCenter }
        }
        Item { width: 1; height: hero.tight ? 2 : 8 }
        Row {
            spacing: 14
            IconButton {
                width: 52; height: 52
                filled: true
                icon: hero.live && player.playing ? "pause" : "play"
                iconSize: 20
                morphOnChange: true
                tip: hero.live ? (player.playing ? qsTr("暂停") : qsTr("继续播放")) : qsTr("播放")
                onClicked: {
                    if (hero.live) { player.toggle(); return }
                    player.shuffle = false
                    Nav.picked(player.pickAlbum(hero.albumIndex, 0, true))
                }
            }
            IconButton {
                id: coverBtn
                objectName: "coverBtn"
                anchors.verticalCenter: parent.verticalCenter
                width: 42; height: 42
                outlined: true
                icon: "album-open"
                iconSize: 17
                tip: qsTr("专辑封面")
                onClicked: hero.coverPanel(coverBtn)
                // 有专辑缺封面时点一个强调色小圆(无字提示)
                Rectangle {
                    readonly property bool on: covers.missingCount > 0
                    visible: on
                    width: 6; height: 6; radius: 3
                    color: Theme.accent
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.top: parent.top
                    anchors.topMargin: 8
                    scale: on ? 1 : 0
                    Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutBack } }
                }
            }
            IconButton {
                id: heroMore
                anchors.verticalCenter: parent.verticalCenter
                width: 42; height: 42
                outlined: true
                icon: "more"
                iconSize: 17
                tip: qsTr("更多")
                onClicked: hero.openMenu(heroMore, 0, heroMore.height + 6)
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: !hero.live
                width: 42; height: 42
                outlined: true
                icon: "refresh"
                iconSize: 16
                tip: qsTr("换一张")
                onClicked: hero.reroll++
            }
        }
    }

    // ---- 宽窗口:横幅右侧放「最近播放 / 最近添加」专辑带,填满大屏上标题右边的空白 ----
    readonly property int shelfCount: Math.min(6, Math.floor((width - textX - 440 + 22) / 134))
    Item {
        id: shelf
        property int tab: 0   // 0 最近播放 / 1 最近添加
        property var played: []
        property var added: []
        readonly property bool hasBoth: played.length > 0 && added.length > 0
        readonly property var items: tab === 0 && played.length ? played : added
        function refresh() {
            const n = hero.shelfCount
            played = n >= 3 && hero.hasAlbum ? albumModel.recentlyPlayed(n, hero.albumIndex) : []
            added = n >= 3 && hero.hasAlbum ? albumModel.recentlyAdded(n, hero.albumIndex) : []
        }
        visible: hero.hasAlbum && hero.shelfCount >= 3 && items.length > 0
        anchors.right: parent.right
        anchors.bottom: art.bottom
        width: hero.shelfCount * 134 - 22
        height: 30 + strip.height

        Component.onCompleted: refresh()
        Connections {
            target: hero
            function onShelfCountChanged() { shelf.refresh() }
            function onAlbumIndexChanged() { shelf.refresh() }
            function onHasAlbumChanged() { shelf.refresh() }
        }
        Connections {
            target: library
            function onLibraryChanged() { shelf.refresh() }
        }
        Connections {
            target: playlists
            function onHistoryChanged() { shelf.refresh() }
        }

        Row {
            spacing: 18
            Repeater {
                model: shelf.hasBoth ? [qsTr("最近播放"), qsTr("最近添加")] : [shelf.played.length ? qsTr("最近播放") : qsTr("最近添加")]
                Item {
                    readonly property bool active: !shelf.hasBoth || shelf.tab === index
                    width: shelfTab.implicitWidth
                    height: 22
                    Text {
                        id: shelfTab
                        text: modelData
                        color: parent.active ? Theme.textSecondary : (shelfTabMouse.containsMouse ? Theme.textSecondary : Theme.textTertiary)
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: parent.active ? Font.DemiBold : Font.Normal
                        font.letterSpacing: 0.6
                    }
                    MouseArea {
                        id: shelfTabMouse
                        anchors.fill: parent
                        anchors.margins: -6
                        enabled: shelf.hasBoth
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: shelf.tab = index
                    }
                }
            }
        }
        AlbumStrip {
            id: strip
            y: 30
            width: parent.width
            coverSize: 112
            model: shelf.items
            onOpen: (index, coverItem) => hero.openAlbum(index, coverItem)
        }
    }

    // ---- 工具栏:左边是数量与总时长,右边是音质筛选、排序、随机来一张 ----
    Item {
        id: toolbar
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 14
        width: parent.width
        height: 36
        visible: library.albumCount > 0

        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, tools.x - 24)
            elide: Text.ElideRight
            text: albumModel.summary
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: 13
        }
        Row {
            id: tools
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 18
            // 筛选:选中的用正文色并带一条强调色短线 (与歌曲页的标签同一样式)
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 18
                Repeater {
                    model: [qsTr("全部"), qsTr("无损"), qsTr("Hi-Res"), qsTr("有损")]
                    Item {
                        readonly property bool active: albumModel.filter === index
                        width: tabText.implicitWidth
                        height: 30
                        Text {
                            id: tabText
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData
                            color: parent.active ? Theme.text : (tabMouse.containsMouse ? Theme.textSecondary : Theme.textTertiary)
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            font.weight: parent.active ? Font.DemiBold : Font.Normal
                            Behavior on color { ColorAnimation { duration: Theme.fast } }
                        }
                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.bottom
                            width: 14; height: 2; radius: 1
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
                            onClicked: albumModel.filter = index
                        }
                    }
                }
            }
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 1; height: 14; color: Theme.hairline }
            // 排序:与筛选标签同一种文字样式;点开小菜单 (菜单放在专辑墙上层,不被网格裁切)
            SortTab {
                id: sortCap
                anchors.verticalCenter: parent.verticalCenter
                height: 30
                centered: true
                fontSize: 13
                text: [qsTr("按标题"), qsTr("按歌手"), qsTr("按年份"), qsTr("最近添加"), qsTr("最近播放")][albumModel.sortMode]
                changed: albumModel.sortMode !== 0
                open: hero.sortOpen
                onClicked: hero.sortMenu(sortCap)
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                width: 34; height: 34
                icon: "shuffle"
                iconSize: 15
                tip: qsTr("随机来一张")
                onClicked: {
                    const ai = albumModel.albumAt(Math.floor(Math.random() * albumModel.count))
                    if (ai < 0) return
                    player.shuffle = false
                    Nav.picked(player.pickAlbum(ai, 0, true))
                    Nav.toast("shuffle", qsTr("随机来一张:%1").arg(library.albumInfo(ai).title || ""))
                }
            }
        }
    }
}
