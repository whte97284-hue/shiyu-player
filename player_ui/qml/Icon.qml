import QtQuick

// 白色 SVG 图标,按物理像素光栅化保证清晰;明暗通过 opacity 控制
// 设置 tint 后以该颜色着色 (如强调色的收藏心形、当前导航项)
Item {
    id: icon
    property string name
    property int size: 20
    property color tint: "transparent"
    readonly property bool tinted: tint.a > 0
    width: size
    height: size

    Image {
        id: img
        anchors.fill: parent
        source: icon.name ? "qrc:/icons/" + icon.name + ".svg" : ""
        sourceSize: Qt.size(icon.size * 2, icon.size * 2)
        smooth: true
        mipmap: true
        visible: !icon.tinted
    }
    Loader {
        anchors.fill: parent
        active: icon.tinted
        sourceComponent: ShaderEffect {
            property variant source: img
            property color color: icon.tint
            fragmentShader: "qrc:/shaders/tint.frag.qsb"
        }
    }
}
