import QtQuick
import PlayerUi

// 空曲库 / 首次导入:无文字,只有一个圆
//  · 空闲:细环 + 中心加号,向外缓慢扩散的涟漪;点击选择文件夹 (也可直接拖入窗口)
//  · 查找文件:一段短弧绕环旋转
//  · 识别中:弧线随进度画满一圈,中心是已识别数量;悬停数量变为 ×,点击取消
//  · 扫完却没找到音乐:圆轻轻摇头,回到加号
Item {
    id: ob
    signal pickFolder()

    readonly property bool importing: library.scanning
    readonly property bool indeterminate: importing && library.scanTotal === 0
    readonly property real side: 168

    // 中心略高于几何中心,视觉上更稳
    Item {
        id: dial
        width: ob.side
        height: ob.side
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -24
        transform: Translate { id: nudge }
        scale: mouse.pressed ? 0.97 : (mouse.containsMouse && !ob.importing ? 1.03 : 1)
        Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutCubic } }

        // 涟漪:空闲时一圈细环向外扩散并淡出
        Repeater {
            model: 2
            Rectangle {
                id: ripple
                anchors.centerIn: parent
                width: ob.side
                height: ob.side
                radius: width / 2
                color: "transparent"
                border.width: 1
                border.color: Theme.accentAlpha(0.5)
                opacity: 0
                visible: !ob.importing
                SequentialAnimation {
                    running: ob.visible && !ob.importing
                    loops: Animation.Infinite
                    PauseAnimation { duration: index * 1600 }
                    ParallelAnimation {
                        ScaleAnimator { target: ripple; from: 1; to: 1.55; duration: 3200; easing.type: Easing.OutCubic }
                        OpacityAnimator { target: ripple; from: 0.7; to: 0; duration: 3200; easing.type: Easing.OutCubic }
                    }
                    PauseAnimation { duration: (1 - index) * 1600 }
                }
            }
        }

        // 圆面:悬停时透出一层极淡的强调色
        Rectangle {
            anchors.fill: parent
            anchors.margins: 3
            radius: width / 2
            color: Theme.accentAlpha(mouse.containsMouse ? 0.08 : 0.035)
            Behavior on color { ColorAnimation { duration: Theme.normal } }
        }

        ShaderEffect {
            id: ring
            anchors.fill: parent
            property color color: Theme.accent
            property color trackColor: Theme.accentAlpha(ob.importing ? 0.26 : (mouse.containsMouse ? 0.75 : 0.42))
            property real side: width
            property real arcWidth: 3
            property real trackWidth: 1.5
            property real start: 0
            property real span: ob.indeterminate ? 0.18 : (ob.importing ? library.progress : 0)
            Behavior on span { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }
            Behavior on trackColor { ColorAnimation { duration: Theme.normal } }
            fragmentShader: "qrc:/shaders/ring.frag.qsb"

            NumberAnimation on start {
                running: ob.indeterminate && ob.visible
                from: 0; to: 1
                duration: 1100
                loops: Animation.Infinite
            }
            onVisibleChanged: if (!ob.indeterminate) start = 0
        }

        // 中心:加号 (空闲) / 数量 (识别中) / × (识别中悬停)
        Item {
            id: plus
            anchors.centerIn: parent
            width: 34; height: 34
            opacity: ob.importing ? 0 : 1
            scale: ob.importing ? 0.6 : (mouse.containsMouse ? 1.12 : 1)
            Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
            Behavior on scale { ScaleAnimator { duration: Theme.normal; easing.type: Easing.OutBack } }
            Rectangle { anchors.centerIn: parent; width: parent.width; height: 2.5; radius: 1.25; color: Theme.accent }
            Rectangle { anchors.centerIn: parent; width: 2.5; height: parent.height; radius: 1.25; color: Theme.accent }
        }

        Text {
            id: count
            anchors.centerIn: parent
            property real shown: library.scanDone
            Behavior on shown { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
            text: Math.round(shown).toLocaleString(Qt.locale(), "f", 0)
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: shown >= 10000 ? 28 : 34
            font.weight: Font.Light
            opacity: ob.importing && !ob.indeterminate && !mouse.containsMouse ? 1 : 0
            Behavior on opacity { OpacityAnimator { duration: Theme.normal } }
        }

        Icon {
            anchors.centerIn: parent
            name: "close"
            size: 22
            opacity: ob.importing && mouse.containsMouse ? 0.85 : 0
            Behavior on opacity { OpacityAnimator { duration: Theme.fast } }
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: ob.importing ? library.cancelScan() : ob.pickFolder()
        }
    }

    // 识别到酷狗的下载目录:圆下方一枚小胶囊,点一下直接加入 (唯一的文字,只在找到时出现)
    readonly property var kgCands: { library.folders; return library.kugouCandidates() }
    Capsule {
        anchors.horizontalCenter: dial.horizontalCenter
        anchors.top: dial.bottom
        anchors.topMargin: 36
        visible: ob.kgCands.length > 0 && !ob.importing
        accent: true
        text: qsTr("发现酷狗音乐文件夹 · 加入")
        onClicked: library.addKugouFolders(ob.kgCands)
    }

    // 扫完没有找到音乐:摇头示意
    SequentialAnimation {
        id: shake
        loops: 1
        NumberAnimation { target: nudge; property: "x"; to: -10; duration: 70; easing.type: Easing.OutQuad }
        NumberAnimation { target: nudge; property: "x"; to: 8; duration: 90 }
        NumberAnimation { target: nudge; property: "x"; to: -5; duration: 80 }
        NumberAnimation { target: nudge; property: "x"; to: 0; duration: 90; easing.type: Easing.OutQuad }
    }
    Connections {
        target: library
        function onScanningChanged() {
            if (!library.scanning && library.trackCount === 0 && !(library.lastScan.cancelled === true)) shake.restart()
        }
    }
}
