import QtQuick
import QtQuick.Shapes
import Vortex

// The heart, wherever a favourite is shown. Drawn from heart.svg's path rather
// than typed as ♥: the glyph's shape and baseline change with whichever font
// supplies it, and a PNG could not take the colour the button asks for.
Shape {
    id: heart
    property color color: Theme.textPrimary
    property real size: 18

    width: heart.size; height: heart.size
    preferredRendererType: Shape.CurveRenderer

    ShapePath {
        strokeWidth: -1
        fillColor: heart.color
        // The path is in the SVG's 24x24 viewBox.
        scale: Qt.size(heart.width / 24, heart.height / 24)
        PathSvg {
            path: "M17.5,1.917a6.4,6.4,0,0,0-5.5,3.3,6.4,6.4,0,0,0-5.5-3.3A6.8,6.8,0,0,0,0,8.967c0,4.547,4.786,9.513,8.8,12.88a4.974,4.974,0,0,0,6.4,0C19.214,18.48,24,13.514,24,8.967A6.8,6.8,0,0,0,17.5,1.917Z"
        }
    }
}
