import QtQuick
import PlayerUi

// 均衡器面板:从播放页右下角的按钮组旁弹出,和歌词来源面板同一种出场方式
//  标题 + 开关 → 预设胶囊 → 频响曲线 (十个圆点可上下拖,双击归零,滚轮微调;背后淡淡铺着实时频谱)
// 曲线就是引擎里那十个滤波器真实的合成频响 (同一套 RBJ 公式);只用强调色和文字明暗
Item {
    id: panelRoot
    property bool shown: false
    property real anchorRight: width - 20
    property real anchorBottom: height - 60
    anchors.fill: parent
    visible: pop.opacity > 0
    z: 5

    readonly property var freqs: player.eqFrequencies
    readonly property bool on: player.eqEnabled
    readonly property string preset: player.eqPreset
    property int dragBand: -1          // 正在拖的一段
    property int hoverBand: -1         // 截图用:模拟悬停

    function open() { shown = true }
    function close() { shown = false; dragBand = -1 }

    // 各块错峰浮现 (与歌词来源面板一致)
    component Stagger: QtObject {
        property Item target
        property int order: 0
        property real t: panelRoot.shown ? 1 : 0
        Behavior on t { SequentialAnimation { PauseAnimation { duration: panelRoot.shown ? 80 + order * 45 : 0 }
                NumberAnimation { duration: panelRoot.shown ? 360 : 100; easing.type: Easing.OutCubic } } }
        onTChanged: if (target) { target.opacity = t; target.lift = (1 - t) * 8 }
    }

    // 每段显示用的增益在各个圆点上 (knob.v):跟随设置值,换预设 / 开关时平滑过渡;拖动时直接跟手
    function gainAt(i) { const k = knobs.itemAt(i); return k ? k.v : 0 }

    // ---- RBJ 双二阶在频率 f 处的幅度 (dB),与引擎同公式:两端搁架 (S = 1),中间峰值 (Q = √2) ----
    readonly property real fs: 48000
    function bandDb(i, gain, f) {
        if (Math.abs(gain) < 1e-4) return 0
        const A = Math.pow(10, gain / 40), w0 = 2 * Math.PI * freqs[i] / fs
        const c = Math.cos(w0), sn = Math.sin(w0)
        let b0, b1, b2, a0, a1, a2
        if (i === 0 || i === 9) {
            const al = sn / 2 * Math.sqrt((A + 1 / A) * (1 / 1 - 1) + 2), sq = 2 * Math.sqrt(A) * al
            if (i === 0) {
                b0 = A * ((A + 1) - (A - 1) * c + sq); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - sq)
                a0 = (A + 1) + (A - 1) * c + sq;       a1 = -2 * ((A - 1) + (A + 1) * c);    a2 = (A + 1) + (A - 1) * c - sq
            } else {
                b0 = A * ((A + 1) + (A - 1) * c + sq); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - sq)
                a0 = (A + 1) - (A - 1) * c + sq;       a1 = 2 * ((A - 1) - (A + 1) * c);      a2 = (A + 1) - (A - 1) * c - sq
            }
        } else {
            const al = sn / (2 * 1.41421356)
            b0 = 1 + al * A; b1 = -2 * c; b2 = 1 - al * A
            a0 = 1 + al / A; a1 = -2 * c; a2 = 1 - al / A
        }
        // |H(e^jw)|²
        const w = 2 * Math.PI * f / fs, cw = Math.cos(w), c2w = Math.cos(2 * w)
        const num = b0 * b0 + b1 * b1 + b2 * b2 + 2 * (b0 * b1 + b1 * b2) * cw + 2 * b0 * b2 * c2w
        const den = a0 * a0 + a1 * a1 + a2 * a2 + 2 * (a0 * a1 + a1 * a2) * cw + 2 * a0 * a2 * c2w
        return 10 * Math.log(num / den) / Math.LN10
    }

    // 点面板外收起
    MouseArea {
        anchors.fill: parent
        enabled: panelRoot.shown
        acceptedButtons: Qt.AllButtons
        onPressed: panelRoot.close()
        onWheel: (w) => w.accepted = true
    }

    Item {
        id: pop
        width: Math.min(520, panelRoot.width - 40)
        height: graphBox.y + graphBox.height + 22
        x: panelRoot.anchorRight - width
        y: panelRoot.anchorBottom - height
        transformOrigin: Item.BottomRight
        opacity: panelRoot.shown ? 1 : 0
        scale: panelRoot.shown ? 1 : 0.97
        Behavior on opacity { OpacityAnimator { duration: panelRoot.shown ? 260 : 150; easing.type: Easing.OutCubic } }
        Behavior on scale { ScaleAnimator { duration: panelRoot.shown ? 520 : 180; easing.type: panelRoot.shown ? Easing.OutQuint : Easing.InCubic } }
        transform: Translate {
            x: panelRoot.shown ? 0 : 28
            Behavior on x { NumberAnimation { duration: panelRoot.shown ? 560 : 180; easing.type: panelRoot.shown ? Easing.OutQuint : Easing.InCubic } }
        }
        SoftShadow { target: card; strength: 0.85; spread: 30; offsetY: 16 }

        Rectangle {
            id: card
            anchors.fill: parent
            radius: Theme.radiusL
            color: Theme.panel
            border.width: 1
            border.color: Qt.rgba(1, 0.96, 0.9, 0.08)
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }
            readonly property int pad: 20

            Stagger { target: head; order: 0 }
            Stagger { target: presets; order: 1 }
            Stagger { target: graphBox; order: 2 }

            // ---- 标题 + 状态 + 开关 ----
            Item {
                id: head
                property real lift: 0
                transform: Translate { y: head.lift }
                x: card.pad
                y: 20
                width: card.width - card.pad * 2
                height: 40
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    Text {
                        text: qsTr("均衡器")
                        color: Theme.text
                        font.family: Theme.fontFamily
                        font.pixelSize: 16
                        font.weight: Font.Bold
                    }
                    Text {
                        text: !panelRoot.on ? qsTr("已关闭,声音原样输出")
                            : panelRoot.preset === "flat" ? qsTr("原声,未做调整")
                            : panelRoot.preset ? qsTr("已开启 · %1").arg(presetName(panelRoot.preset))
                            : qsTr("已开启 · 自定义")
                        color: Theme.textTertiary
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        function presetName(k) {
                            for (const p of player.eqPresets) if (p.key === k) return p.name
                            return ""
                        }
                    }
                }
                Row {
                    anchors.right: parent.right
                    anchors.rightMargin: -8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6
                    // 开关:和设置页同一种样式
                    Item {
                        id: toggle
                        objectName: "eqToggle"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 44; height: 32
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 44; height: 24; radius: 12
                            color: panelRoot.on ? Theme.accent : Qt.rgba(1, 0.96, 0.9, tgm.containsMouse ? 0.16 : 0.11)
                            Behavior on color { ColorAnimation { duration: Theme.fast } }
                            Rectangle {
                                y: 3
                                x: panelRoot.on ? parent.width - width - 3 : 3
                                width: 18; height: 18; radius: 9
                                color: panelRoot.on ? Theme.onAccent : Theme.textSecondary
                                Behavior on x { NumberAnimation { duration: Theme.reduced ? 0 : 200; easing.type: Easing.OutCubic } }
                            }
                        }
                        MouseArea {
                            id: tgm
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: player.eqEnabled = !player.eqEnabled
                        }
                    }
                    IconButton {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 32; height: 32
                        icon: "close"
                        iconSize: 16
                        onClicked: panelRoot.close()
                    }
                }
            }

            // ---- 预设 ----
            Flow {
                id: presets
                property real lift: 0
                transform: Translate { y: presets.lift }
                x: card.pad
                y: head.y + head.height + 14
                width: card.width - card.pad * 2
                spacing: 8
                Repeater {
                    model: player.eqPresets
                    Capsule {
                        height: 28
                        pad: 13
                        text: modelData.name
                        accent: panelRoot.on && panelRoot.preset === modelData.key
                        onClicked: player.applyEqPreset(modelData.key)
                    }
                }
            }

            // ---- 频响曲线 ----
            Item {
                id: graphBox
                property real lift: 0
                transform: Translate { y: graphBox.lift }
                x: card.pad
                y: presets.y + presets.height + 18
                width: card.width - card.pad * 2
                height: 214

                // 绘图区:左侧留出 dB 刻度,上下留出圆点和数值气泡的位置
                readonly property real gx: 30
                readonly property real gw: width - gx - 6
                readonly property real gy: 22
                readonly property real gh: 158
                readonly property real fMin: 20
                readonly property real fMax: 20000
                function xOf(f) { return gx + gw * Math.log(f / fMin) / Math.log(fMax / fMin) }
                function yOf(db) { return gy + gh / 2 - db / 12 * (gh / 2 - 8) }
                function dbOf(y) { return -(y - gy - gh / 2) / (gh / 2 - 8) * 12 }

                // 刻度线:±12 / ±6 极淡,0 dB 稍亮
                Repeater {
                    model: [12, 6, 0, -6, -12]
                    Item {
                        y: graphBox.yOf(modelData)
                        width: graphBox.width
                        Rectangle {
                            x: graphBox.gx
                            width: graphBox.gw
                            height: 1
                            color: modelData === 0 ? Qt.rgba(1, 0.96, 0.9, 0.14) : Qt.rgba(1, 0.96, 0.9, 0.05)
                        }
                        Text {
                            visible: modelData % 12 === 0
                            anchors.verticalCenter: parent.top
                            width: graphBox.gx - 8
                            horizontalAlignment: Text.AlignRight
                            text: modelData > 0 ? "+" + modelData : modelData
                            color: Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                        }
                    }
                }

                // 实时频谱:极淡的强调色细柱,只在开着、播放时浮现
                Item {
                    id: spectrum
                    x: graphBox.gx
                    y: graphBox.gy
                    width: graphBox.gw
                    height: graphBox.gh
                    opacity: panelRoot.shown && player.playing ? 1 : 0
                    visible: opacity > 0
                    Behavior on opacity { OpacityAnimator { duration: Theme.slow } }
                    readonly property var values: visible ? player.spectrum : []
                    Repeater {
                        model: 32
                        Rectangle {
                            readonly property real v: index < spectrum.values.length ? spectrum.values[index] : 0
                            width: Math.max(2, spectrum.width / 32 - 4)
                            x: index * spectrum.width / 32 + 2
                            height: Math.max(0, v) * spectrum.height * 0.8
                            y: spectrum.height - height
                            radius: 1.5
                            color: Theme.accentAlpha(0.09)
                        }
                    }
                }

                // 曲线 + 曲线与 0 dB 之间的淡色填充;关闭时褪成灰色
                Canvas {
                    id: curve
                    anchors.fill: parent
                    renderStrategy: Canvas.Cooperative
                    property color tone: panelRoot.on ? Theme.accent : Qt.rgba(0.953, 0.937, 0.914, 0.32)
                    Behavior on tone { ColorAnimation { duration: Theme.normal } }
                    onToneChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        if (panelRoot.freqs.length < 10) return
                        const g = []
                        for (let i = 0; i < 10; ++i) g.push(panelRoot.gainAt(i))
                        const n = 140, pts = []
                        for (let k = 0; k <= n; ++k) {
                            const f = graphBox.fMin * Math.pow(graphBox.fMax / graphBox.fMin, k / n)
                            let db = 0
                            for (let i = 0; i < 10; ++i) db += panelRoot.bandDb(i, g[i], f)
                            db = Math.max(-15, Math.min(15, db))
                            pts.push([graphBox.xOf(f), graphBox.yOf(db)])
                        }
                        const zero = graphBox.yOf(0)
                        ctx.beginPath()
                        ctx.moveTo(pts[0][0], zero)
                        for (const p of pts) ctx.lineTo(p[0], p[1])
                        ctx.lineTo(pts[n][0], zero)
                        ctx.closePath()
                        ctx.fillStyle = Qt.rgba(tone.r, tone.g, tone.b, panelRoot.on ? 0.13 : 0.05)
                        ctx.fill()
                        ctx.beginPath()
                        ctx.moveTo(pts[0][0], pts[0][1])
                        for (let k = 1; k <= n; ++k) ctx.lineTo(pts[k][0], pts[k][1])
                        ctx.lineWidth = 2
                        ctx.lineJoin = "round"
                        ctx.strokeStyle = tone
                        ctx.stroke()
                    }
                }

                // 十个圆点:每段一条竖向的拖动区 (不必正好按在点上),拖动按位移改增益
                Repeater {
                    id: knobs
                    model: panelRoot.freqs.length
                    Item {
                        id: knob
                        readonly property real f: panelRoot.freqs[index]
                        readonly property real target: index < player.eqGains.length ? player.eqGains[index] : 0
                        property real drag: 0
                        property real v: dragging ? drag : target
                        Behavior on v {
                            enabled: !knob.dragging
                            NumberAnimation { duration: Theme.reduced ? 0 : 460; easing.type: Easing.OutCubic }
                        }
                        onVChanged: curve.requestPaint()
                        Component.onCompleted: curve.requestPaint()
                        readonly property bool dragging: panelRoot.dragBand === index
                        readonly property bool hot: dragging || hit.containsMouse || panelRoot.hoverBand === index
                        readonly property real cx: graphBox.xOf(f)
                        x: cx - width / 2
                        width: graphBox.gw / 10
                        height: graphBox.height

                        // 数值气泡:悬停 / 拖动时在点上方浮现
                        Rectangle {
                            id: bubble
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: graphBox.yOf(knob.v) - 34
                            width: valText.implicitWidth + 14
                            height: 20
                            radius: 10
                            color: Theme.accentAlpha(0.18)
                            opacity: knob.hot ? 1 : 0
                            scale: knob.hot ? 1 : 0.85
                            Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                            Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }
                            Text {
                                id: valText
                                anchors.centerIn: parent
                                text: (knob.v > 0.04 ? "+" : "") + (Math.round(knob.v * 2) / 2).toFixed(1)
                                color: Theme.accent
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                        }

                        Rectangle {
                            id: dot
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: graphBox.yOf(knob.v) - height / 2
                            width: 14; height: 14; radius: 7
                            color: Theme.panel
                            border.width: 2
                            border.color: curve.tone
                            scale: knob.dragging ? 1.3 : knob.hot ? 1.15 : 1
                            Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutBack } }
                            Rectangle {
                                anchors.centerIn: parent
                                width: 6; height: 6; radius: 3
                                color: curve.tone
                                opacity: knob.hot ? 1 : 0
                                Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
                            }
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: graphBox.gy + graphBox.gh + 14
                            text: knob.f >= 1000 ? (knob.f / 1000) + "k" : knob.f
                            color: knob.hot ? Theme.accent : Theme.textTertiary
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                            Behavior on color { ColorAnimation { duration: Theme.fast } }
                        }

                        MouseArea {
                            id: hit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                            preventStealing: true
                            property real startY: 0
                            property real startV: 0
                            onPressed: (m) => {
                                startY = m.y
                                startV = knob.v
                                knob.drag = knob.v
                                panelRoot.dragBand = index
                            }
                            onPositionChanged: (m) => {
                                if (!pressed) return
                                const dv = -(m.y - startY) / (graphBox.gh / 2 - 8) * 12
                                const nv = Math.max(-12, Math.min(12, startV + dv))
                                knob.drag = nv
                                player.setEqGain(index, nv)
                            }
                            onReleased: panelRoot.dragBand = -1
                            onCanceled: panelRoot.dragBand = -1
                            onDoubleClicked: player.setEqGain(index, 0)
                            onWheel: (w) => player.setEqGain(index, knob.v + (w.angleDelta.y > 0 ? 0.5 : -0.5))
                        }
                    }
                }
            }
        }
    }
}
