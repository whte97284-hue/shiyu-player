import QtQuick
import PlayerUi

// 沉浸页背景:当前封面模糊铺满整窗。只在换封面时渲染一张小纹理 (layer),之后每帧只是拉伸贴图,几乎零开销;
// 换歌时新背景加载完成后在旧背景上淡入
Item {
    id: root
    property string source
    // 模糊半径 (uv)、整体亮度、取样图尺寸;solid = false 时不画底色,可叠在其他背景上
    property real spread: 0.16
    property real dim: 0.5
    property int sampleSize: 48
    property bool solid: true
    property Item front: null

    onSourceChanged: load()
    Component.onCompleted: load()
    function load() {
        if (front && front.src === source) return
        const back = front === a ? b : a
        back.src = source
        if (back.ready) show(back)
    }
    // 新背景放到最上层,从透明淡入;旧背景留在下面,淡入期间不会露出底色
    function show(layer) {
        if (front === layer || layer.src !== source) return
        if (front) front.z = 0
        layer.z = 1
        front = layer
        // 看不见时 (例如播放页收起时切了歌) 直接换上,不做淡入:
        // 隐藏期间动画不会推进,等页面再打开时新背景会从全透明开始淡入,中间露出一大片黑底
        if (!root.visible) { fadeIn.stop(); layer.opacity = 1; hideBack(); return }
        layer.opacity = 0
        fadeIn.target = layer
        fadeIn.restart()
    }
    // 重新显示时若还有没走完的淡入,直接到位
    onVisibleChanged: if (visible && fadeIn.running) { fadeIn.stop(); if (front) front.opacity = 1; hideBack() }
    OpacityAnimator { id: fadeIn; from: 0; to: 1; duration: 900; easing.type: Easing.InOutQuad; onFinished: root.hideBack() }
    // 淡入结束后把下面的旧图层隐藏,任何时候只有一张不透明的图在画;
    // 否则整页淡入淡出时两张图各自半透明叠加,中途会变暗
    function hideBack() { const back = front === a ? b : a; back.opacity = 0 }

    component Layer: Item {
        id: lay
        property string src
        readonly property bool ready: img.status === Image.Ready
        anchors.fill: parent
        opacity: 0
        onReadyChanged: if (ready) root.show(lay)
        Image {
            id: img
            source: lay.src
            sourceSize: Qt.size(root.sampleSize, root.sampleSize)
            asynchronous: true
            visible: false
            smooth: true
        }
        // 所在页面隐藏后,图层纹理会被回收;重新显示时立刻重建一次,
        // 否则重新打开播放页的前几帧只有空纹理,画面会闪黑
        onVisibleChanged: if (visible) { blur.layer.enabled = false; blur.layer.enabled = true }
        Item {
            id: blur
            anchors.fill: parent
            layer.enabled: true
            layer.smooth: true
            // 只在换封面时渲染一次;半分辨率足够细腻,再低放大后会出现块状条纹
            layer.textureSize: Qt.size(Math.max(1, Math.round(width / 2)), Math.max(1, Math.round(height / 2)))
            ShaderEffect {
                anchors.fill: parent
                visible: lay.ready
                property variant source: img
                readonly property real aspect: width / Math.max(1, height)
                property size crop: aspect >= 1 ? Qt.size(1, 1 / aspect) : Qt.size(aspect, 1)
                property real spread: root.spread
                property real dim: root.dim
                fragmentShader: "qrc:/shaders/backdrop.frag.qsb"
            }
        }
    }

    Rectangle { anchors.fill: parent; color: Theme.base; visible: root.solid }
    Layer { id: a }
    Layer { id: b }
}
