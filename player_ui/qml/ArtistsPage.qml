import QtQuick
import PlayerUi

// 歌手墙:圆形头像 (取其代表专辑封面) + 名字;字母索引跳转
// 悬停浮出播放键,正在播放的歌手名字用强调色并带律动角标;右键打开整位歌手的菜单
Item {
    id: page
    // 内容滚到窗口按钮下面了:Main 据此在顶端铺一条磨砂条
    readonly property bool scrolled: grid.contentY - grid.originY > 8
    property int bottomInset: 140
    readonly property int sidePadding: width > 1400 ? 56 : 40
    readonly property int minCell: 168
    readonly property int columns: Math.max(3, Math.floor((width - sidePadding * 2 - 24) / minCell))
    readonly property real cellW: (width - sidePadding * 2 - 24) / columns
    // 正在播放的歌曲的 (首位) 歌手
    readonly property int playingArtist: library.generation >= 0 && player.hasTrack ? library.artistOfTrack(player.trackId) : -1

    function openMenu(item, px, py, artistIndex, name) {
        const p = item.mapToItem(null, px, py)
        Nav.trackMenu(-1, p.x, p.y, { ids: library.artistTrackIds(artistIndex), kind: "artist", title: name,
                                      artistIndex: artistIndex, tag: "artist:" + name })
    }

    // 返回歌手墙时头像飞回的落点。不能用 visible 判断:歌手墙此刻还处于隐藏状态,所有格子的 visible 都是 false;
    // 改用 itemAtIndex 排除回收池里闲置的格子
    function avatarOf(i) {
        for (let k = 0; k < grid.contentItem.children.length; ++k) {
            const c = grid.contentItem.children[k]
            if (c.artistIndex === i && c.avatarItem && grid.itemAtIndex(c.row) === c) return c.avatarItem
        }
        return null
    }

    GridView {
        id: grid
        anchors.fill: parent
        anchors.leftMargin: page.sidePadding
        anchors.rightMargin: page.sidePadding + 24
        model: artistModel
        cellWidth: page.cellW
        cellHeight: page.cellW + 56
        cacheBuffer: 200
        reuseItems: true
        boundsBehavior: Flickable.StopAtBounds
        topMargin: 12
        bottomMargin: page.bottomInset
        SmoothScroll { flickable: grid }

        // 歌手数 (与专辑墙、歌曲页的计数同一样式);第一行头像仍从 y≈64 开始
        header: Item {
            width: grid.width
            height: 52
            Row {
                x: 18
                y: 14
                spacing: 8
                Icon { anchors.verticalCenter: parent.verticalCenter; name: "artist"; size: 15; opacity: 0.5 }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: grid.count.toLocaleString(Qt.locale(), "f", 0)
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                }
            }
        }

        delegate: Item {
            id: cell
            width: grid.cellWidth
            height: grid.cellHeight
            readonly property string letter: model.letter
            readonly property int artistIndex: model.artistIndex
            readonly property int row: index
            readonly property Item avatarItem: avatarImg
            readonly property bool hovered: m.containsMouse || (playLoader.item !== null && playLoader.item.hovered)
            readonly property real d: width - 36
            readonly property bool playing: model.artistIndex === page.playingArtist
            // 当前队列就是这位歌手 (从这里或歌手页点的播放):播放键变成暂停 / 继续
            readonly property bool isQueue: player.queueTag === "artist:" + model.name
            // 复用来显示另一位歌手时,上一位遗留的播放键直接撤掉
            GridView.onReused: btnLinger.stop()

            Item {
                id: avatar
                x: 18
                width: cell.d
                height: cell.d
                y: cell.hovered ? -5 : 0
                scale: m.pressed ? 0.97 : 1
                Behavior on y { YAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
                Behavior on scale { ScaleAnimator { duration: Theme.fast } }
                SoftShadow {
                    target: avatarImg
                    spread: 12
                    offsetY: cell.hovered ? 16 : 10
                    strength: cell.hovered ? 0.9 : 0.55
                    Behavior on offsetY { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
                }
                RoundedImage {
                    id: avatarImg
                    anchors.fill: parent
                    radius: width / 2
                    source: model.cover
                    // 按物理像素解码(见 AlbumCard.qml 的说明)
                    readonly property real pxScale: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
                    decodeSize: Math.ceil(cell.d * Math.max(1.0, Math.min(1.2, pxScale)))
                }
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: Theme.accent
                    opacity: cell.hovered ? 0.9 : 0
                    Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
                }
                // 正在播放:头像底部正中的律动角标
                Loader {
                    active: cell.playing
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: cell.d - 30
                    sourceComponent: Rectangle {
                        width: 30; height: 24; radius: 12
                        color: Qt.rgba(0, 0, 0, 0.55)
                        PlayingBars { anchors.centerIn: parent; maxHeight: 12 }
                    }
                }
                // 悬停播放:放在圆周右下 45° 处;这位歌手的队列暂停中时不悬停也露出继续键
                // 只在需要时创建 (GridView 滚出视野的格子会被销毁重建,少建一组控件滚动更轻);收起时多留一会儿放完淡出
                readonly property bool btnWanted: cell.hovered || (cell.isQueue && !player.playing)
                onBtnWantedChanged: if (!btnWanted) btnLinger.restart()
                Timer { id: btnLinger; interval: Theme.normal + 60 }
                Loader {
                    id: playLoader
                    anchors.fill: parent
                    active: avatar.btnWanted || btnLinger.running
                    sourceComponent: Item {
                        readonly property bool hovered: playBtn.hovered
                        property bool ready: false
                        Component.onCompleted: ready = true
                        IconButton {
                            id: playBtn
                            readonly property bool shown: parent.ready && (cell.hovered || (cell.isQueue && !player.playing))
                            x: cell.d * 0.854 - 20
                            y: cell.d * 0.854 - 20
                            width: 40; height: 40
                            filled: true
                            icon: cell.isQueue && player.playing ? "pause" : "play"
                            iconSize: 17
                            opacity: shown ? 1 : 0
                            scale: shown ? 1 : 0.7
                            visible: opacity > 0
                            Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
                            Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutBack } }
                            onClicked: {
                                if (cell.isQueue) { player.toggle(); return }
                                player.shuffle = false
                                Nav.picked(player.pickTracks(library.artistTrackIds(model.artistIndex), 0, "artist:" + model.name, true))
                            }
                        }
                    }
                }
            }
            Text {
                y: cell.d + 14
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: model.name
                elide: Text.ElideRight
                color: cell.playing ? Theme.accent : Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
            Text {
                y: cell.d + 35
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: model.summary
                color: Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 11
            }
            MouseArea {
                id: m
                anchors.fill: parent
                z: -1
                // 滚动时头像从鼠标下掠过,不逐个上浮
                hoverEnabled: !Theme.scrolling
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                cursorShape: Qt.PointingHandCursor
                onClicked: (e) => {
                    if (e.button === Qt.RightButton) page.openMenu(m, e.x, e.y, model.artistIndex, model.name)
                    else Nav.openArtistFrom(model.artistIndex, avatarImg)
                }
            }
        }

        onContentYChanged: syncLetter.restart()
        Timer {
            id: syncLetter
            interval: 60
            onTriggered: {
                const i = grid.indexAt(40, grid.contentY + 40)
                const it = i >= 0 ? grid.itemAtIndex(i) : null
                if (it) rail.current = it.letter
                else if (artistModel.letters.length) rail.current = artistModel.letters[0]
            }
        }
        // 还没滚动时字母条也标出当前 (第一个) 字母
        onCountChanged: syncLetter.restart()
        Component.onCompleted: syncLetter.restart()
    }

    LetterRail {
        id: rail
        anchors.right: parent.right
        anchors.rightMargin: page.sidePadding - 18
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: -page.bottomInset / 4
        letters: artistModel.letters
        onJump: (l) => {
            const i = artistModel.indexOfLetter(l)
            if (i < 0) return
            // 跳到的那一行留在顶部 44px 窗口按钮条下面,不和最小化 / 关闭按钮叠在一起
            grid.positionViewAtIndex(i, GridView.Beginning)
            grid.contentY = Math.max(grid.originY, grid.contentY - 52)
        }
    }
}
