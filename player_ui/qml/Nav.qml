pragma Singleton
import QtQuick

// 全局导航总线:任意组件发出意图,由 Main 统一处理 (页面切换、右键菜单、轻提示)
QtObject {
    signal openAlbum(int index)
    signal openArtist(int index)
    // 带来源图块:头像从来源位置飞入详情页
    signal openArtistFrom(int index, Item item)
    signal openPlaylist(string id)
    // 打开「导入歌单」卡片
    signal openImport()
    // ctx: { playlistId } 等上下文 (在歌单中时菜单多出「从歌单移除」)
    signal trackMenu(real trackId, real x, real y, var ctx)
    signal toast(string icon, string text)
    // 列表点歌 (player.pickTracks / pickAlbum) 的结果:没有立即播放时提示一下歌去了哪里
    // 右键菜单的「播放」走同一条点歌路径时置为 true:菜单的播放总是立即播放,不受点歌设置影响
    property bool pickNow: false
    // 歌词来源对应的图标:同名歌词文件 / 内嵌 / 酷狗歌词库 / 在线;没有歌词时是搜索
    function lyricIcon(kind) {
        return ({ file: "lyric-file", embedded: "disc", library: "folder", online: "cloud" })[kind] || "search"
    }
    function picked(result) {
        if (result === "next") toast("next-up", qsTr("下一首播放"))
        else if (result === "queued") toast("queue", qsTr("已加到播放队列"))
    }
    // 带操作按钮的提示 (如清空播放队列后的「撤销」):点按钮执行 fn
    signal toastAction(string text, string actionText, var fn)
    // 看封面大图 (专辑详情点封面)
    signal viewCover(string source, string title, string subtitle)
    // 按钮文字提示 (IconButton 的 tip):悬停时 hint,移开 / 按下时 unhint
    signal hint(Item item, string text)
    signal unhint(Item item)
    // 侧栏图标:提示浮在图标右侧
    signal hintRight(Item item, string text)
    // 静音 / 恢复 (由 Main 统一记住静音前的音量)
    signal toggleMute()
    // 新建歌单后,详情页打开时直接进入改名状态
    property string renameNext: ""

    // 曲目列表的键盘操作 (歌曲 / 歌手 / 歌单页,与专辑页同一套):↑↓ 选择 (第一次从正在播放的那首开始)、
    // Home / End、回车播放 play(i)、菜单键 menu(i, x, y);选中行不藏到底部播放条下面 (inset)
    function listKeys(list, e, playingRow, inset, play, menu) {
        if (list.count === 0) return false
        let i = list.currentIndex
        if (e.key === Qt.Key_Down || e.key === Qt.Key_Up) {
            if (i < 0) i = playingRow >= 0 ? playingRow : (e.key === Qt.Key_Down ? 0 : list.count - 1)
            else i = Math.max(0, Math.min(list.count - 1, i + (e.key === Qt.Key_Down ? 1 : -1)))
        } else if (e.key === Qt.Key_Home) i = 0
        else if (e.key === Qt.Key_End) i = list.count - 1
        else if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter) && i >= 0) { play(i); return true }
        else if (e.key === Qt.Key_Menu && i >= 0 && list.currentItem) {
            const p = list.currentItem.mapToItem(null, 60, list.currentItem.height)
            menu(i, p.x, p.y)
            return true
        } else return false
        list.currentIndex = i
        list.positionViewAtIndex(i, ListView.Contain)
        const it = list.currentItem
        if (it) {
            const over = it.y + it.height - (list.contentY + list.height - inset)
            const maxY = list.originY + list.contentHeight + list.bottomMargin - list.height
            if (over > 0) list.contentY = Math.min(maxY, list.contentY + over)
            // 顶上留出窗口按钮条
            const under = list.contentY + 52 - it.y
            if (under > 0 && i > 0) list.contentY = Math.max(list.originY, list.contentY - under)
        }
        return true
    }

    // 富文本高亮:query 出现在 text 中时以强调色加粗
    function highlight(text, query, color) {
        const esc = (s) => String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        const q = String(query || "").trim()
        if (!q.length) return esc(text)
        const i = String(text).toLowerCase().indexOf(q.toLowerCase())
        if (i < 0) return esc(text)
        return esc(text.substring(0, i)) + "<font color=\"" + color + "\"><b>" + esc(text.substr(i, q.length)) + "</b></font>"
             + esc(text.substring(i + q.length))
    }
}
