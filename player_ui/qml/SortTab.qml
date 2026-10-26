import QtQuick
import PlayerUi

// 排序入口:与旁边的「全部 / 最近播放」等文字标签同一种样式 (同字号、同颜色规则、同基线),
// 只多一个小下拉箭头;改过排序时文字用强调色。点开排序小菜单
Item {
    id: st
    property string text
    property int fontSize: 15
    property bool changed: false   // 不是默认排序
    property bool open: false      // 菜单开着
    property bool centered: false  // 文字在高度内垂直居中 (旁边标签居中时);否则贴顶,与贴顶的标签同基线
    signal clicked()
    width: label.implicitWidth + 4 + 12
    height: 32

    Text {
        id: label
        y: st.centered ? Math.round((st.height - implicitHeight) / 2) : 0
        text: st.text
        color: st.changed || st.open ? Theme.accent : (mouse.containsMouse ? Theme.textSecondary : Theme.textTertiary)
        font.family: Theme.fontFamily
        font.pixelSize: st.fontSize
        font.weight: st.changed ? Font.DemiBold : Font.Normal
        Behavior on color { ColorAnimation { duration: Theme.fast } }
    }
    Icon {
        x: label.implicitWidth + 4
        y: label.y + (label.implicitHeight - height) / 2 + 1
        name: "chevron"
        size: 12
        tint: st.changed || st.open ? Theme.accent : "transparent"
        rotation: st.open ? -90 : 90
        opacity: st.changed || st.open || mouse.containsMouse ? 0.9 : 0.45
        Behavior on rotation { NumberAnimation { duration: Theme.normal; easing.type: Easing.OutCubic } }
        Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        anchors.margins: -6
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: st.clicked()
    }
}
