import QtQuick

// 柔和投影:预模糊的 9 宫格贴图,任意尺寸拉伸零开销
BorderImage {
    id: sh
    property Item target
    property real spread: 26
    property real offsetY: 14
    property real strength: 1.0
    // 下一次目标出现时不淡入,直接到位 (共享元素飞行的副本自己带着投影落地,交接时不能再从零淡入)
    property bool snap: false
    x: target ? target.x - spread : 0
    y: target ? target.y - spread + offsetY : 0
    width: target ? target.width + spread * 2 : 0
    height: target ? target.height + spread * 2 : 0
    source: "qrc:/images/shadow.png"
    border { left: 64; top: 64; right: 64; bottom: 64 }
    // 跟随目标的透明度:
    //  · 目标被隐藏 (共享元素飞行起飞 / 落点先藏起来) 时投影立刻跟着消失,不会在原地多留一个影子;
    //  · 目标一下子显示出来时投影短暂淡入,不会和封面一起「啪」地出现;目标自己慢慢淡入时同步跟随
    property real fade: target ? target.opacity : 1
    opacity: strength * fade
    // 阴影本身是模糊渐变,不需要平滑插值 —— 开着 smooth 会让每次重绘都做一次
    // 高质量双线性采样,一屏几十个阴影时这是实打实的填充成本(实测切页掉帧翻倍)
    smooth: false
    function follow() {
        const t = target ? target.opacity : 1
        if (snap || t <= fade || t - fade < 0.5) { fadeAnim.stop(); fade = t; snap = false; return }
        fadeAnim.to = t
        fadeAnim.restart()
    }
    onTargetChanged: follow()
    Connections {
        target: sh.target
        function onOpacityChanged() { sh.follow() }
    }
    NumberAnimation { id: fadeAnim; target: sh; property: "fade"; duration: 280; easing.type: Easing.OutCubic }
}
