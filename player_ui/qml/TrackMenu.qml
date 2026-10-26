import QtQuick
import PlayerUi

// 曲目右键菜单:播放 / 下一首播放 / 加入队列 / 加入歌单 (二级) / 收藏 / 查看专辑 / 查看歌手 /
// 在文件夹中显示 / 复制歌名 / 复制文件路径 / 歌曲信息 / 从歌单移除
// 「播放」沿用所在页面双击时的做法 (ctx.play);在播放队列里打开时 (ctx.queueIndex) 播放 / 移除 / 挪到下一首都只动队列本身
// 键盘:↑↓ 选择、回车执行、→ 打开二级、← 返回
// trackId < 0 且 ctx.albumIndex 有值时是「整张专辑」菜单 (专辑详情的「更多」、专辑墙卡片右键);
// trackId < 0 且 ctx.ids 有值时是「一组歌」菜单 (歌手 / 歌单 / 歌曲页的「更多」、歌手 / 歌单卡片右键),
// ctx.kind = artist / playlist / songs / history 决定附加项,ctx.title 为名字,ctx.tag 为播放来源 (player.queueTag)
Item {
    id: menu
    anchors.fill: parent
    visible: panel.opacity > 0
    property real trackId: -1
    property var ctx: ({})
    property bool shown: false
    property bool subShown: false
    // 点击位置;面板位置随菜单实际高度绑定,菜单项变多时也不会超出窗口底部
    property real reqX: 0
    property real reqY: 0
    signal infoRequested(real trackId)

    readonly property bool fav: playlists.revision >= 0 && trackId >= 0 && playlists.isFavorite(trackId)
    readonly property bool albumMode: trackId < 0 && ctx && ctx.albumIndex !== undefined && ctx.albumIndex >= 0
    readonly property bool listMode: trackId < 0 && !albumMode && ctx && ctx.ids !== undefined
    // 菜单作用的曲目:单曲、整张专辑,或一组歌
    function targetIds() { return albumMode ? library.albumTrackIds(ctx.albumIndex) : listMode ? ctx.ids : [trackId] }
    // 删除歌单 / 清空记录:第一次点只变成「再点一次」,菜单不关
    property string confirmKey: ""
    // 键盘 / 悬停选中的行 (一级菜单下标、二级歌单下标);subFocus = 键盘焦点在二级菜单里
    property int hot: -1
    property int subHot: -1
    property bool subFocus: false
    readonly property int queueIndex: ctx && ctx.queueIndex !== undefined ? ctx.queueIndex : -1
    readonly property var subItems: [{ id: "", name: qsTr("新建歌单"), create: true }]
                                    .concat(playlists.playlists.filter(p => p.id !== (menu.ctx ? menu.ctx.playlistId : undefined)))

    function open(id, px, py, c) {
        trackId = id
        ctx = c || {}
        subShown = false
        subFocus = false
        subClose.stop()
        hot = -1
        subHot = -1
        confirmKey = ""
        rebuild()
        reqX = px
        reqY = py
        shown = true
    }
    function close() { shown = false; subShown = false; subFocus = false; subClose.stop() }
    Timer { id: subClose; interval: 260; onTriggered: { menu.subShown = false; menu.subFocus = false } }

    // 二级菜单靠着「添加到歌单」那一行展开 (右边放不下时放左边)
    function showSub(row) {
        const p = row.mapToItem(menu, 0, 0)
        const right = panel.x + panel.width + sub.width + 4 < menu.width
        sub.x = right ? panel.x + panel.width + 4 : panel.x - sub.width - 4
        sub.y = Math.max(8, Math.min(p.y - 6, menu.height - sub.height - 8))
        subClose.stop()
        subShown = true
    }
    function pickSub(entry) {
        if (entry.create) {
            const name = playlists.suggestName()
            addTo(playlists.create(name), name, true)
        } else {
            addTo(entry.id, entry.name, false)
        }
    }
    // 键盘操作;返回 true 表示按键已处理
    function keyNav(e) {
        const k = e.key
        if (subFocus) {
            const n = subItems.length
            if (k === Qt.Key_Down || k === Qt.Key_Up) {
                subHot = subHot < 0 ? 0 : (subHot + (k === Qt.Key_Down ? 1 : n - 1)) % n
                subList.positionViewAtIndex(subHot, ListView.Contain)
            } else if (k === Qt.Key_Left) { subFocus = false; subShown = false }
            else if (k === Qt.Key_Return || k === Qt.Key_Enter || k === Qt.Key_Space) { if (subHot >= 0) pickSub(subItems[subHot]) }
            else return false
            return true
        }
        const n = items.length
        if (!n) return false
        if (k === Qt.Key_Down || k === Qt.Key_Up) {
            const d = k === Qt.Key_Down ? 1 : -1
            let i = hot
            for (let step = 0; step < n; ++step) {
                i = i < 0 ? (d > 0 ? 0 : n - 1) : (i + d + n) % n
                if (items[i].key !== "sep") break
            }
            hot = i
            subShown = false
            return true
        }
        const entry = hot >= 0 && hot < n ? items[hot] : null
        if (!entry) return k === Qt.Key_Return || k === Qt.Key_Enter
        if (entry.sub && (k === Qt.Key_Right || k === Qt.Key_Return || k === Qt.Key_Enter || k === Qt.Key_Space)) {
            const row = rowRepeater.itemAt(hot)
            if (row) showSub(row)
            subFocus = true
            subHot = 0
            return true
        }
        if (k === Qt.Key_Return || k === Qt.Key_Enter || k === Qt.Key_Space) { act(entry.key); return true }
        return k === Qt.Key_Right || k === Qt.Key_Left
    }

    property var items: []
    function rebuild() {
        if (albumMode) {
            const ids = library.albumTrackIds(ctx.albumIndex)
            const allFav = playlists.revision >= 0 && ids.length > 0 && ids.every(i => playlists.isFavorite(i))
            const info = library.albumInfo(ctx.albumIndex)
            items = [
                { key: "albumPlay", icon: "play", text: qsTr("播放") },
                { key: "albumShuffle", icon: "shuffle", text: qsTr("随机播放") },
                { key: "next", icon: "next-up", text: qsTr("下一首播放") },
                { key: "queue", icon: "queue", text: qsTr("添加到播放队列") },
                { key: "playlist", icon: "playlist", text: qsTr("添加到歌单"), sub: true },
                { key: "favAll", icon: allFav ? "heart-filled" : "heart", text: allFav ? qsTr("全部取消收藏") : qsTr("全部收藏"), on: allFav },
                { key: "sep" },
                { key: "albumOpen", icon: "album-open", text: qsTr("查看专辑"), hide: ctx.fromDetail === true },
                { key: "albumArtist", icon: "artist", text: qsTr("查看歌手"), hide: library.artistIndexOf(info.artist || "") < 0 },
                { key: "sep" },
                { key: "reveal", icon: "folder-open", text: qsTr("在文件夹中显示") },
                { key: "copyAlbum", icon: "copy", text: qsTr("复制专辑名") }
            ].filter(e => !e.hide)
            return
        }
        if (listMode) {
            const ids = ctx.ids
            const has = ids.length > 0
            const allFav = playlists.revision >= 0 && has && ids.every(i => playlists.isFavorite(i))
            const pl = ctx.kind === "playlist" && ctx.playlistId ? playlists.info(ctx.playlistId) : ({})
            const editable = ctx.kind === "playlist" && pl.id !== undefined && !pl.builtin
            const list = [
                { key: "listPlay", icon: "play", text: qsTr("播放"), hide: !has },
                { key: "listShuffle", icon: "shuffle", text: qsTr("随机播放"), hide: !has },
                { key: "next", icon: "next-up", text: qsTr("下一首播放"), hide: !has },
                { key: "queue", icon: "queue", text: qsTr("添加到播放队列"), hide: !has },
                { key: "playlist", icon: "playlist", text: qsTr("添加到歌单"), sub: true, hide: !has },
                // 整个曲库逐首收藏没有意义,歌曲页不给
                { key: "favAll", icon: allFav ? "heart-filled" : "heart", text: allFav ? qsTr("全部取消收藏") : qsTr("全部收藏"), on: allFav, hide: !has || ctx.kind === "songs" }
            ]
            if (ctx.kind === "artist") {
                list.push({ key: "sep" },
                          { key: "listArtist", icon: "artist", text: qsTr("查看歌手"), hide: ctx.fromDetail === true || ctx.artistIndex === undefined },
                          { key: "copyArtist", icon: "copy", text: qsTr("复制歌手名") })
            } else if (editable) {
                list.push({ key: "sep" },
                          { key: "rename", icon: "edit", text: qsTr("重命名") },
                          { key: "deletePlaylist", icon: "trash", text: confirmKey === "deletePlaylist" ? qsTr("再点一次删除") : qsTr("删除歌单"), warn: confirmKey === "deletePlaylist" })
            } else if (ctx.kind === "history" && has) {
                list.push({ key: "sep" },
                          { key: "clearHistory", icon: "trash", text: confirmKey === "clearHistory" ? qsTr("再点一次清空") : qsTr("清空播放记录"), warn: confirmKey === "clearHistory" })
            }
            items = list.filter(e => !e.hide).filter((e, i, a) => e.key !== "sep" || (i > 0 && i < a.length - 1))
            return
        }
        const sameAlbum = ctx && ctx.albumIndex !== undefined && ctx.albumIndex === library.albumOfTrack(trackId)
        const q = queueIndex
        const pos = player.queuePosition
        // 正在播放的这首:播放中不给「播放」,暂停时是「继续播放」
        const isCur = player.hasTrack && trackId === player.trackId
        const curRow = q >= 0 ? q === pos : isCur
        const list = [
            { key: "play", icon: "play", text: isCur && !player.playing ? qsTr("继续播放") : qsTr("播放"), hide: curRow && player.playing },
            { key: "next", icon: "next-up", text: qsTr("下一首播放"), hide: q >= 0 && (q === pos || q === pos + 1) },
            { key: "queue", icon: "queue", text: qsTr("添加到播放队列"), hide: q >= 0 },
            { key: "dequeue", icon: "close", text: qsTr("从播放队列移除"), hide: q < 0 || q === pos },
            { key: "playlist", icon: "playlist", text: qsTr("添加到歌单"), sub: true },
            { key: "fav", icon: menu.fav ? "heart-filled" : "heart", text: menu.fav ? qsTr("取消收藏") : qsTr("收藏") },
            { key: "sep" },
            { key: "album", icon: "album-open", text: qsTr("查看专辑"), hide: sameAlbum || library.albumOfTrack(trackId) < 0 },
            { key: "artist", icon: "artist", text: qsTr("查看歌手"), hide: library.artistOfTrack(trackId) < 0 },
            { key: "sep" },
            { key: "reveal", icon: "folder-open", text: qsTr("在文件夹中显示") },
            { key: "copyName", icon: "copy", text: qsTr("复制歌名") },
            { key: "copyPath", icon: "copy", text: qsTr("复制文件路径") },
            { key: "info", icon: "info", text: qsTr("歌曲信息") }
        ]
        if (ctx && ctx.playlistId)
            list.push({ key: "sep" }, { key: "remove", icon: "trash", text: qsTr("从歌单移除") })
        items = list.filter(e => !e.hide).filter((e, i, a) => e.key !== "sep" || (i > 0 && i < a.length - 1 && a[i - 1].key !== "sep"))
    }
    function act(key) {
        const id = trackId
        if (albumMode && actAlbum(key)) { close(); return }
        if (listMode) { if (actList(key)) close(); return }
        const q = queueIndex
        if (key === "play") {
            if (player.hasTrack && id === player.trackId && !player.playing) player.toggle()
            else if (q >= 0) player.playQueueIndex(q)
            else if (typeof ctx.play === "function") { Nav.pickNow = true; try { ctx.play() } finally { Nav.pickNow = false } }
            else player.playTrack(id)
        }
        else if (key === "next") {
            // 队列里的歌:挪到当前这首后面,而不是再插一份
            if (q >= 0) player.moveInQueue(q, q < player.queuePosition ? player.queuePosition : player.queuePosition + 1)
            else player.playNext(id)
            Nav.toast("next-up", qsTr("将在下一首播放"))
        }
        else if (key === "dequeue") player.removeFromQueue(q)
        else if (key === "queue") { player.enqueue([id]); Nav.toast("queue", qsTr("已添加到播放队列")) }
        else if (key === "fav") playlists.toggleFavorite(id)
        else if (key === "album") Nav.openAlbum(library.albumOfTrack(id))
        else if (key === "artist") Nav.openArtist(library.artistOfTrack(id))
        else if (key === "remove") playlists.removeTrack(ctx.playlistId, id)
        else if (key === "reveal") {
            if (!library.revealTrack(id)) Nav.toast("folder", qsTr("找不到这首歌所在的文件夹"))
        } else if (key === "copyName" || key === "copyPath") {
            const t = library.trackInfo(id)
            if (t.path !== undefined) {
                library.copyToClipboard(key === "copyPath" ? t.path : (t.artist ? t.artist + " - " + t.title : t.title))
                Nav.toast("check", key === "copyPath" ? qsTr("已复制文件路径") : qsTr("已复制歌名"))
            }
        } else if (key === "info") menu.infoRequested(id)
        close()
    }
    // 整张专辑的操作;返回 false 表示不是专辑专属的键
    function actAlbum(key) {
        const a = ctx.albumIndex
        const ids = library.albumTrackIds(a)
        if (!ids.length) return true
        if (key === "albumPlay") { player.shuffle = false; player.playAlbum(a, 0) }
        else if (key === "albumShuffle") { player.shuffle = true; player.playAlbum(a, Math.floor(Math.random() * ids.length)) }
        else if (actIds(key, ids)) {}
        else if (key === "albumOpen") Nav.openAlbum(a)
        else if (key === "albumArtist") Nav.openArtist(library.artistIndexOf(library.albumInfo(a).artist || ""))
        else if (key === "reveal") { if (!library.revealTrack(ids[0])) Nav.toast("folder", qsTr("找不到这张专辑所在的文件夹")) }
        else if (key === "copyAlbum") {
            const info = library.albumInfo(a)
            library.copyToClipboard(info.artist ? info.artist + " - " + info.title : info.title)
            Nav.toast("check", qsTr("已复制专辑名"))
        }
        else return false
        return true
    }
    // 整张专辑与一组歌共用的操作
    function actIds(key, ids) {
        if (key === "next") { player.playNextTracks(ids); Nav.toast("next-up", qsTr("%1 首将在当前歌曲之后播放").arg(ids.length)) }
        else if (key === "queue") { player.enqueue(ids); Nav.toast("queue", qsTr("已添加 %1 首到播放队列").arg(ids.length)) }
        else if (key === "favAll") {
            const allFav = ids.every(i => playlists.isFavorite(i))
            let n = 0
            for (const i of ids) if (playlists.isFavorite(i) === allFav) { playlists.toggleFavorite(i); ++n }
            Nav.toast(allFav ? "heart" : "heart-filled", allFav ? qsTr("已取消收藏 %1 首").arg(n) : qsTr("已收藏 %1 首").arg(n))
        }
        else return false
        return true
    }
    // 一组歌的操作;返回 false 表示菜单先不关 (等第二次确认)
    function actList(key) {
        const ids = ctx.ids
        const tag = ctx.tag || ""
        if (key === "listPlay") { player.shuffle = false; player.playTracks(ids, 0, tag) }
        else if (key === "listShuffle") { player.shuffle = true; player.playTracks(ids, Math.floor(Math.random() * ids.length), tag) }
        else if (actIds(key, ids)) {}
        else if (key === "listArtist") Nav.openArtist(ctx.artistIndex)
        else if (key === "copyArtist") { library.copyToClipboard(ctx.title || ""); Nav.toast("check", qsTr("已复制歌手名")) }
        else if (key === "rename") { Nav.renameNext = ctx.playlistId; Nav.openPlaylist(ctx.playlistId) }
        else if (key === "deletePlaylist" || key === "clearHistory") {
            if (confirmKey !== key) {
                confirmKey = key
                rebuild()
                return false
            }
            if (key === "clearHistory") playlists.clearHistory()
            else {
                const name = ctx.title || ""
                playlists.remove(ctx.playlistId)
                Nav.toast("trash", qsTr("已删除歌单「%1」").arg(name))
            }
        }
        return true
    }
    function addTo(pid, name, created) {
        const ids = targetIds()
        const n = playlists.addTracks(pid, ids)
        // 刚新建的歌单用的是默认名字:提示里给一个「重命名」直达
        if (created && n > 0)
            Nav.toastAction(albumMode || listMode ? qsTr("已添加 %1 首到「%2」").arg(n).arg(name) : qsTr("已添加到「%1」").arg(name),
                            qsTr("重命名"), () => { Nav.renameNext = pid; Nav.openPlaylist(pid) })
        else if (albumMode || listMode)
            Nav.toast(n > 0 ? "check" : "playlist", n > 0 ? qsTr("已添加 %1 首到「%2」").arg(n).arg(name) : qsTr("「%1」里已经有这些歌").arg(name))
        else
            Nav.toast(n > 0 ? "check" : "playlist", n > 0 ? qsTr("已添加到「%1」").arg(name) : qsTr("「%1」里已经有这首歌").arg(name))
        close()
    }

    MouseArea {
        anchors.fill: parent
        enabled: menu.shown
        acceptedButtons: Qt.AllButtons
        onPressed: menu.close()
        onWheel: (w) => menu.close()
    }

    // ---- 一级菜单 ----
    Rectangle {
        id: panel
        width: 212
        x: Math.max(8, Math.min(menu.reqX, menu.width - width - 8))
        y: Math.max(8, Math.min(menu.reqY, menu.height - height - 8))
        implicitHeight: col.implicitHeight + 12
        height: implicitHeight
        radius: Theme.radiusM
        color: Theme.panel
        border.width: 1
        border.color: Qt.rgba(1, 0.96, 0.9, 0.09)
        opacity: menu.shown ? 1 : 0
        scale: menu.shown ? 1 : 0.96
        transformOrigin: Item.TopLeft
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        Behavior on scale { ScaleAnimator { duration: Theme.fast; easing.type: Easing.OutCubic } }
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

        Column {
            id: col
            x: 6
            y: 6
            width: parent.width - 12
            Repeater {
                id: rowRepeater
                model: menu.items
                delegate: Loader {
                    width: col.width
                    sourceComponent: modelData.key === "sep" ? sepC : itemC
                    property var entry: modelData
                    property int row: index
                }
            }
        }
    }
    Component {
        id: sepC
        Item { height: 9; Rectangle { y: 4; x: 8; width: parent.width - 16; height: 1; color: Theme.hairline } }
    }
    Component {
        id: itemC
        Rectangle {
            id: it
            height: 34
            radius: 8
            readonly property bool hot: row === menu.hot || (!!entry.sub && menu.subShown)
            // 待确认的删除 / 清空:警示色底
            color: entry.warn === true ? Theme.warnAlpha(hot ? 0.3 : 0.22) : hot ? Theme.hover : "transparent"
            readonly property bool on: (entry.key === "fav" && menu.fav) || entry.on === true
            Icon {
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                name: entry.icon || ""
                size: 15
                tint: it.on ? Theme.accent : "transparent"
                opacity: it.on ? 1 : (it.hot ? 0.9 : 0.62)
            }
            Text {
                x: 38
                anchors.verticalCenter: parent.verticalCenter
                text: entry.text
                color: it.on ? Theme.accent : Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: 13
            }
            Text {
                visible: !!entry.sub
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "›"
                color: Theme.textTertiary
                font.pixelSize: 16
            }
            MouseArea {
                id: m
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onEntered: {
                    menu.hot = row
                    menu.subFocus = false
                    // 斜着移向二级菜单时会掠过下面几行:稍等一下再收起,不让二级菜单在半路消失
                    if (entry.sub) menu.showSub(it)
                    else if (menu.subShown) subClose.restart()
                }
                onExited: if (menu.hot === row && !entry.sub) menu.hot = -1
                onClicked: {
                    if (!entry.sub) menu.act(entry.key)
                    else { menu.showSub(it); menu.subFocus = true }
                }
            }
        }
    }

    // ---- 二级:歌单 ----
    Rectangle {
        id: sub
        width: 220
        height: Math.min(subList.contentHeight + 12, 360)
        radius: Theme.radiusM
        color: Theme.panel
        border.width: 1
        border.color: Qt.rgba(1, 0.96, 0.9, 0.09)
        opacity: menu.shown && menu.subShown ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            onContainsMouseChanged: if (containsMouse) subClose.stop()
        }

        ListView {
            id: subList
            x: 6; y: 6
            width: parent.width - 12
            height: parent.height - 12
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            // 不列出菜单所在的歌单自己
            model: menu.subItems
            delegate: Rectangle {
                width: subList.width
                height: 34
                radius: 8
                color: sm.containsMouse || (menu.subFocus && index === menu.subHot) ? Theme.hover : "transparent"
                Text {
                    x: 14
                    width: parent.width - 14 - 36
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.name
                    elide: Text.ElideRight
                    color: modelData.create ? Theme.accent : Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                }
                Text {
                    visible: !modelData.create
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.count !== undefined ? modelData.count : ""
                    color: Theme.textTertiary
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                }
                MouseArea {
                    id: sm
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onEntered: { subClose.stop(); menu.subHot = index }
                    onClicked: menu.pickSub(modelData)
                }
            }
        }
    }
}
