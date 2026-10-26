import QtQuick
import PlayerUi

// 32 段频谱:强调色渐变柱,顶部亮帽;暂停后由内核阻尼自然回落
Item {
    id: spec
    property int bars: 32
    property real gap: 3
    readonly property real barW: (width - gap * (bars - 1)) / bars
    // 每帧只把频谱数组转换一次,而不是每根柱子各转换一次
    readonly property var values: visible ? player.spectrum : []

    Row {
        anchors.bottom: parent.bottom
        spacing: spec.gap
        Repeater {
            model: spec.bars
            Item {
                width: spec.barW
                height: spec.height
                // 柱数少于内核的段数时按比例取样 (低频到高频均匀覆盖)
                readonly property real v: {
                    const n = spec.values.length
                    return n ? (spec.values[Math.min(n - 1, Math.floor((index + 0.5) * n / spec.bars))] || 0) : 0
                }
                // 不再把安静的频段整根藏起来 (那样会变成断断续续的虚线):
                // 低电平用幂曲线略微抬高,并保留一条暗淡的底线
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: Math.max(2, parent.height * Math.pow(v, 0.7))
                    opacity: 0.35 + 0.65 * Math.min(1, v * 4)
                    radius: Math.min(2, width / 2)
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Theme.accent }
                        GradientStop { position: 1.0; color: Theme.accentAlpha(0.12) }
                    }
                }
            }
        }
    }
}
