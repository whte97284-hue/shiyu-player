import QtQuick
import PlayerUi

// 品质标签:Hi-Res / DSD 跟随当前强调色描边,其余为低调灰 (不使用固定的金色,避免与封面主色冲突)
Rectangle {
    id: badge
    property string text
    property bool premium: /DSD|\/(88|96|176|192|352|384)|Hi-Res|^24|^32/.test(text)
    property int fontSize: 10
    visible: text.length > 0
    implicitWidth: label.implicitWidth + 12
    implicitHeight: label.implicitHeight + 4
    radius: 4
    color: premium ? Theme.accentAlpha(0.10) : Qt.rgba(1, 1, 1, 0.06)
    border.width: 1
    border.color: premium ? Theme.accentAlpha(0.42) : Qt.rgba(1, 1, 1, 0.12)
    Text {
        id: label
        anchors.centerIn: parent
        text: badge.text
        color: badge.premium ? Theme.accent : Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: badge.fontSize
        font.weight: Font.DemiBold
        font.letterSpacing: 0.4
    }
}
