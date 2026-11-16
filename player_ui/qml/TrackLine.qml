import QtQuick
import PlayerUi

// 在线列表的通用歌曲行：序号/律动 · 标题 + VIP · 歌手·专辑 · 时长 · 播放 · 下载
//
// active = 这一行就是当前在线曲目（含暂停态，用来高亮）；playing = 真的在出声
// （用来决定律动不动、按钮显示播放还是暂停）。
// 强调色不写死，由页面传 Theme.accent 进来 —— 跟着当前播放曲目的封面主色走。
Item {
    id: row
    // 序号由外面显式传进来（rowIndex）：和 delegate 的 model index 区分开，
    // 否则同名属性会被 required 声明覆盖，所有行都显示 1
    property int rowIndex: 0
    property string title: ""
    property string subtitle: ""
    property string artist: ""
    property string album: ""
    property bool showAlbum: false
    property string highlight: ""
    property bool keyFocus: false
    property string duration: ""
    property bool paywalled: false
    property bool active: false
    property bool playing: false
    property bool pending: false     // 正在解析 / 下载：给转圈，别让人以为点了没反应
    property bool fav: false
    property color accent: Theme.accent
    readonly property color dim: Theme.accentAlpha(0.14)
    readonly property bool wide: width > 720
    readonly property bool hovered: hover.containsMouse || favBtn.hovered || playMouse.containsMouse || dlMouse.containsMouse

    signal played()
    signal downloaded()
    signal favorited()
    signal menuRequested(real px, real py)

    height: 52

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusM
        color: row.active ? Theme.accentAlpha(0.10)
                          : (hover.pressed ? Theme.pressed
                             : (row.hovered ? Theme.hover : "transparent"))
        Behavior on color { ColorAnimation { duration: Theme.fast } }
    }
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusM
        visible: row.keyFocus
        color: "transparent"
        border.width: 1
        border.color: Theme.accentAlpha(0.5)
    }

    // 整行可点播放（与本地 TrackRow 一致）；放在右侧操作按钮之前，按钮会自然接管自己的区域
    MouseArea {
        id: hover
        anchors.fill: parent
        hoverEnabled: !Theme.scrolling
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.PointingHandCursor
        onClicked: (e) => {
            if (e.button === Qt.RightButton) row.menuRequested(e.x, e.y)
            else if (!row.pending) row.played()
        }
    }

    // 当前曲目：左缘一道强调色细条，和本地列表的选中语言一致
    Rectangle {
        width: 2
        height: row.active ? 18 : 0
        radius: 1
        x: 1
        anchors.verticalCenter: parent.verticalCenter
        color: row.accent
        opacity: row.active ? 1 : 0
        Behavior on height { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
    }

    Item {
        id: idxBox
        width: 30
        height: parent.height
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.verticalCenter: parent.verticalCenter

        Text {
            anchors.centerIn: parent
            visible: !row.active
            text: row.rowIndex + 1
            color: Theme.textTertiary
            font.pixelSize: 11
        }
        // 暂停时停在低位，不空转：一眼能看出"这首放着但停住了"
        Row {
            anchors.centerIn: parent
            spacing: 2
            visible: row.active
            Repeater {
                model: 3
                delegate: Rectangle {
                    id: bar
                    required property int index
                    width: 2
                    radius: 1
                    height: 4
                    color: row.accent
                    SequentialAnimation {
                        running: row.playing
                        loops: Animation.Infinite
                        NumberAnimation {
                            target: bar
                            property: "height"
                            to: 13
                            duration: 300 + index * 90
                            easing.type: Easing.InOutSine
                        }
                        NumberAnimation {
                            target: bar
                            property: "height"
                            to: 4
                            duration: 300 + index * 90
                            easing.type: Easing.InOutSine
                        }
                    }
                }
            }
        }
    }

    Item {
        id: centerBox
        anchors.left: idxBox.right
        anchors.leftMargin: 6
        anchors.right: dur.left
        anchors.rightMargin: 14
        height: parent.height
        readonly property bool splitAlbum: row.showAlbum && row.wide && row.album.length > 0 && row.artist.length > 0

        Column {
            anchors.left: parent.left
            anchors.right: albumCol.visible ? albumCol.left : parent.right
            anchors.rightMargin: albumCol.visible ? 18 : 0
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2

            Row {
                spacing: 6
                width: parent.width
                Text {
                    id: titleText
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.highlight.length ? Nav.highlight(row.title, row.highlight, row.accent) : row.title
                    textFormat: row.highlight.length ? Text.StyledText : Text.PlainText
                    color: row.active ? row.accent : Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: row.active ? Font.DemiBold : Font.Normal
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, parent.width - (badge.visible ? badge.width + 6 : 0))
                    Behavior on color { ColorAnimation { duration: Theme.fast } }
                }
                Rectangle {
                    id: badge
                    visible: row.paywalled
                    anchors.verticalCenter: parent.verticalCenter
                    width: 26; height: 15; radius: 4
                    color: Theme.accentAlpha(0.08)
                    border.width: 1
                    border.color: Theme.accentAlpha(0.28)
                    opacity: row.hovered || row.active ? 0.95 : 0.55
                    Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                    Text {
                        anchors.centerIn: parent
                        text: "VIP"
                        color: row.accent
                        font.family: Theme.fontFamily
                        font.pixelSize: 8
                        font.weight: Font.DemiBold
                    }
                }
            }
            Text {
                width: parent.width
                readonly property string baseSub: centerBox.splitAlbum
                                                ? row.artist
                                                : (row.artist.length
                                                   ? row.artist + (row.showAlbum && row.album.length ? " · " + row.album : "")
                                                   : row.subtitle)
                visible: baseSub.length > 0
                text: row.highlight.length ? Nav.highlight(baseSub, row.highlight, row.accent) : baseSub
                textFormat: row.highlight.length ? Text.StyledText : Text.PlainText
                color: Theme.textTertiary
                font.family: Theme.fontFamily
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        Text {
            id: albumCol
            visible: centerBox.splitAlbum
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(parent.width * 0.34, 260)
            text: row.highlight.length ? Nav.highlight(row.album, row.highlight, row.accent) : row.album
            textFormat: row.highlight.length ? Text.StyledText : Text.PlainText
            color: Theme.textTertiary
            font.family: Theme.fontFamily
            font.pixelSize: 12
            elide: Text.ElideRight
        }
    }

    Text {
        id: dur
        width: 46
        horizontalAlignment: Text.AlignRight
        anchors.right: actions.left
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        text: row.duration
        color: Theme.textTertiary
        font.family: Theme.fontFamily
        font.pixelSize: 12
    }

    Row {
        id: actions
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4

        IconButton {
            id: favBtn
            anchors.verticalCenter: parent.verticalCenter
            width: 28; height: 28
            icon: row.fav ? "heart-filled" : "heart"
            iconTint: row.fav ? row.accent : "transparent"
            popOnChange: true
            iconSize: 14
            restOpacity: row.fav ? 0.95 : 0.6
            opacity: row.fav || row.hovered ? 1 : 0
            Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
            onClicked: row.favorited()
        }

        Rectangle {
            width: 28; height: 28; radius: 14
            color: row.pending ? row.dim
                               : (playMouse.containsMouse || row.active ? row.accent : "transparent")
            opacity: row.hovered || row.active || row.pending ? 1 : 0.55
            scale: playMouse.pressed ? 0.9 : (playMouse.containsMouse ? 1.06 : 1.0)
            Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
            Behavior on color { ColorAnimation { duration: Theme.fast } }
            Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
            Icon {
                anchors.centerIn: parent
                visible: !row.pending
                name: row.playing ? "pause" : "play"
                size: 10
                opacity: playMouse.containsMouse || row.active ? 1 : 0.72
                tint: playMouse.containsMouse || row.active ? Theme.onAccent : Theme.textSecondary
            }
            // 解析中：转圈代替播放键，点下去立刻有反馈
            Icon {
                anchors.centerIn: parent
                visible: row.pending
                name: "spinner"
                size: 11
                tint: row.accent
                RotationAnimator on rotation {
                    running: row.pending
                    loops: Animation.Infinite
                    from: 0
                    to: 360
                    duration: 800
                }
            }
            MouseArea {
                id: playMouse
                anchors.fill: parent
                hoverEnabled: !Theme.scrolling
                cursorShape: Qt.PointingHandCursor
                onClicked: if (!row.pending) row.played()
            }
        }
        Rectangle {
            width: 28; height: 28; radius: 14
            color: dlMouse.containsMouse ? row.dim : "transparent"
            opacity: row.hovered ? 1 : 0.35
            scale: dlMouse.pressed ? 0.9 : (dlMouse.containsMouse ? 1.06 : 1.0)
            Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
            Behavior on color { ColorAnimation { duration: Theme.fast } }
            Behavior on scale { NumberAnimation { duration: Theme.fast; easing.type: Easing.OutCubic } }
            Icon {
                anchors.centerIn: parent
                name: "import"
                size: 11
                opacity: dlMouse.containsMouse ? 0.95 : 0.65
                tint: dlMouse.containsMouse ? row.accent : Theme.textSecondary
            }
            MouseArea {
                id: dlMouse
                anchors.fill: parent
                hoverEnabled: !Theme.scrolling
                cursorShape: Qt.PointingHandCursor
                onClicked: row.downloaded()
            }
        }
    }
}
