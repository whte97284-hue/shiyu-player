import QtQuick
import PlayerUi

// 扫描进度胶囊:扫描时出现,完成后显示结果并在 3 秒后淡出
// 文件夹监控触发的后台重扫不出现;后台重扫发现了变化才显示结果
Rectangle {
    id: toast
    property bool shown: enabled && (library.scanShown || hold.running)
    width: label.implicitWidth + 36
    height: 38
    radius: 19
    color: Theme.panel
    border.width: 1
    border.color: Theme.hairline
    opacity: shown ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: Theme.normal } }

    Timer { id: hold; interval: 3200 }
    Connections {
        target: library
        function onScanningChanged() { if (!library.scanning && library.lastScan.shown !== false) hold.restart() }
    }

    Text {
        id: label
        anchors.centerIn: parent
        // 扫描中:已识别 / 总数;完成:曲目数
        text: library.scanning ? (library.scanTotal > 0 ? qsTr("扫描 %1 / %2").arg(library.scanDone).arg(library.scanTotal) : qsTr("扫描中"))
                               : qsTr("%1 首").arg(library.trackCount.toLocaleString(Qt.locale(), "f", 0))
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: 13
    }
    // 底部细进度线
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 1
        x: toast.radius
        height: 2
        radius: 1
        width: (toast.width - toast.radius * 2) * library.progress
        color: Theme.accent
        visible: library.scanning
    }
}
