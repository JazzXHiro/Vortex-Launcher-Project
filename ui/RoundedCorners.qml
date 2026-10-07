import QtQuick
import QtQuick.Effects

// Rounds a game card's corners for real. Used as a card's layer.effect:
//
//     layer.enabled: true
//     layer.smooth: true
//     layer.effect: RoundedCorners { radius: card.radius }
//
// clip on a rounded Rectangle cuts to the bounding box, not the rounded
// shape, so the cover art fills the corners the card's radius only appears to
// draw. Masking the whole layer through a rounded shape rounds the art, the
// scrim and everything else on the card together. It costs one layer per card
// on screen; layer.smooth keeps the texture clean under the hover scale.
MultiEffect {
    id: effect

    property real radius: 8

    maskEnabled: true
    maskSource: shape
    maskThresholdMin: 0.5
    maskSpreadAtMin: 1.0

    Item {
        id: shape
        width: effect.width
        height: effect.height
        layer.enabled: true
        visible: false

        Rectangle {
            anchors.fill: parent
            radius: effect.radius
            antialiasing: true
        }
    }
}
