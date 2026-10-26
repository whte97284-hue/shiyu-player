import QtQuick
import QtQuick.Window
import PlayerUi

// 封面交叉淡化:新封面在后层加载完成后再淡入并轻微收拢,旧封面同时淡出,切歌时不闪占位色
Item {
    id: root
    property string source
    property real radius: 12
    // 按实际显示的物理像素解码 (不再固定 1024:一张 1024² 解码图常驻 4 MB,交叉淡化时还有两张)
    property int decodeSize: Math.ceil(Math.max(width, height) * Screen.devicePixelRatio)
    property bool frontIsA: true

    onSourceChanged: {
        const front = frontIsA ? a : b
        const back = frontIsA ? b : a
        if (String(front.source) === source) return
        // 后层恰好已是这张 (来回切换时常见):已就绪则直接翻面,否则等它加载完成
        if (String(back.source) === source) {
            if (back.status === Image.Ready) frontIsA = !frontIsA
            return
        }
        back.source = source
    }

    RoundedImage {
        id: a
        anchors.fill: parent
        radius: root.radius
        decodeSize: root.decodeSize
        opacity: root.frontIsA ? 1 : 0
        scale: root.frontIsA ? 1 : 1.04
        Behavior on opacity { OpacityAnimator { duration: 520; easing.type: Easing.OutCubic } }
        Behavior on scale { ScaleAnimator { duration: 700; easing.type: Easing.OutQuint } }
        onStatusChanged: if (status === Image.Ready && !root.frontIsA && String(source) === root.source) root.frontIsA = true
    }
    RoundedImage {
        id: b
        anchors.fill: parent
        radius: root.radius
        decodeSize: root.decodeSize
        opacity: root.frontIsA ? 0 : 1
        scale: root.frontIsA ? 1.04 : 1
        Behavior on opacity { OpacityAnimator { duration: 520; easing.type: Easing.OutCubic } }
        Behavior on scale { ScaleAnimator { duration: 700; easing.type: Easing.OutQuint } }
        onStatusChanged: if (status === Image.Ready && root.frontIsA && String(source) === root.source) root.frontIsA = false
    }
    Component.onCompleted: a.source = source
}
