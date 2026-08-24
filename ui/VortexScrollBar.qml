import QtQuick
import QtQuick.Controls

// Shared vertical scrollbar for the launcher's scrollable views.
//
// Deliberately quiet: the track only paints while the mouse is on it, and the
// handle sits at a dim hint until then, so a library that fits on one screen
// shows no furniture at all (AsNeeded hides the whole bar in that case).
//
// Vertical only — the widen-on-hover below grows the handle by shrinking the
// left padding, which assumes the bar runs down the right edge.
ScrollBar {
    id: control

    policy: ScrollBar.AsNeeded
    minimumSize: 0.06

    implicitWidth: 14
    // 5 → 3 takes the handle from 6px wide to 8px once it is worth grabbing.
    leftPadding: control.hovered || control.pressed ? 3 : 5
    rightPadding: 3
    topPadding: 4
    bottomPadding: 4

    Behavior on leftPadding { NumberAnimation { duration: 120 } }

    contentItem: Rectangle {
        implicitWidth: 6
        radius: width / 2
        color: control.pressed ? "white" : (control.hovered ? "#888" : "#3a3a3a")
        Behavior on color { ColorAnimation { duration: 150 } }
    }

    background: Rectangle {
        radius: width / 2
        color: "#141414"
        border.color: "#222"
        border.width: 1
        opacity: control.hovered || control.pressed ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
    }
}
