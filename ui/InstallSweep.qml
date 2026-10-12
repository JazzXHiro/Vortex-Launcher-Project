pragma ComponentBehavior: Bound
import QtQuick
import Vortex

// A game card while Steam downloads its game: the light sweep (LightSweep.qml)
// across the cover, with the progress written on it. Driven by one entry of
// VortexBridge::steamInstalls.
Item {
    id: overlay

    // 0..100, and one of "starting", "paused", "downloading", "done".
    property int percent: 0
    property string phase: "starting"
    // A second line under the percentage, e.g. "12.4 MB/s".
    property string detail: ""
    // Windows' "Animation effects" is off: no sweep, just the figures.
    property bool reducedMotion: false

    readonly property bool paused: overlay.phase === "paused"

    // Dims the cover so the white figure has contrast.
    Rectangle {
        anchors.fill: parent
        color: Theme.artScrim
        opacity: 0.45
    }

    Column {
        anchors.centerIn: parent
        spacing: 2

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: overlay.phase === "starting" ? "STARTING"
                : overlay.phase === "done" ? "INSTALLED"
                : overlay.percent + "%"
            color: Theme.textPrimary
            font.pixelSize: overlay.phase === "downloading" || overlay.phase === "paused" ? 34 : 16
            font.bold: true
            font.letterSpacing: overlay.phase === "downloading" || overlay.phase === "paused" ? 0 : 1.5
            style: Text.Raised
            styleColor: "#66000000"
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: overlay.paused ? "PAUSED" : overlay.detail
            visible: text !== ""
            color: Theme.textPrimary
            opacity: 0.85
            font.pixelSize: 11
            font.bold: true
            font.letterSpacing: 1.2
        }
    }

    // Over the figures as well as the art: it crosses everything on the card.
    LightSweep {
        anchors.fill: parent
        running: !overlay.paused && overlay.phase !== "done" && !overlay.reducedMotion
    }
}
