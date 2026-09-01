pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Vortex

// ─────────────────────────────────────────────────────────────────────────────
// The overflow menu a game tile's ⋮ button opens.
//
// One instance, declared beside the details page in main.qml, rather than one
// per delegate: the grid builds a card for every game in the library and a
// popup each would be hundreds of them, all but one closed.
//
// Hand-rolled from Rectangles rather than a Controls Menu for the same reason
// every other button in this UI is: the pad drives the selection through
// main.qml's intent routing, and Menu insists on owning the keyboard focus and
// the arrow keys itself.
// ─────────────────────────────────────────────────────────────────────────────
Popup {
    id: menuRoot

    // Whichever input moved last owns the highlight, exactly as the tiles and
    // the details page do it -- a cursor left sitting on a row would otherwise
    // stay lit next to the one the pad is on.
    property var pad: controller
    readonly property bool padInControl: menuRoot.pad ? menuRoot.pad.padInControl : false
    readonly property bool mouseInControl: !menuRoot.padInControl

    // The game this menu was opened for. installDir is carried alongside the
    // name because it is the identity that survives a scan renaming a local
    // game (see VortexBridge::removeFromLibrary).
    property string gameName: ""
    property string installDir: ""

    // Which step is showing: the item list, or its REMOVE / CANCEL confirm.
    property bool confirming: false

    // -1 means "mouse only", so nothing is outlined until the pad opens this.
    // Same rule as GameDetails.focusedAction.
    readonly property int itemRemove: 0
    readonly property int itemCount: 1
    property int focusedItem: -1

    // Within the confirm step. Starts on CANCEL so a stray pad press removes
    // nothing, mirroring the uninstall confirm on the details page.
    readonly property int armedRemove: 0
    readonly property int armedCancel: 1
    property int armedChoice: -1

    // Emitted rather than calling the bridge from in here. Removing a game
    // rebuilds the library list, and holding the grid's scroll position across
    // that rebuild is the business of whoever owns the grid.
    signal removeRequested(string name, string installDir)

    // ── Opening ──────────────────────────────────────────────────────────────
    // Anchored under the button that opened it, in the window's coordinates:
    // this renders in the overlay layer, so the grid's clip does not cut it off
    // and the position has to be mapped out of the delegate.
    //
    // The button's geometry is kept rather than used once, because the box
    // changes size when the confirm step replaces the ✕ and it has to stay
    // pinned to the same corner across that.
    property real anchorX: 0
    property real anchorY: 0
    property real anchorW: 0
    property real anchorH: 0

    function openFor(anchorItem, name, dir) {
        if (!anchorItem)
            return
        menuRoot.gameName    = name
        menuRoot.installDir  = dir
        menuRoot.confirming  = false
        menuRoot.armedChoice = -1

        // x and y are relative to the popup's parent -- the window's content
        // item, since this is declared as a child of the Window -- so that is
        // what the button's position has to be mapped into.
        const frame = menuRoot.parent
        if (!frame)
            return
        const at = anchorItem.mapToItem(frame, 0, 0)
        menuRoot.anchorX = at.x
        menuRoot.anchorY = at.y
        menuRoot.anchorW = anchorItem.width
        menuRoot.anchorH = anchorItem.height

        menuRoot.reposition()
        menuRoot.open()
    }

    function reposition() {
        const frame = menuRoot.parent
        if (!frame)
            return

        // Right-align the box with the button, then keep the whole thing on
        // screen -- a card in the last column would otherwise open off the edge.
        let px = menuRoot.anchorX + menuRoot.anchorW - menuRoot.width
        px = Math.max(12, Math.min(px, frame.width - menuRoot.width - 12))

        // Flip above the button when there is no room below, which is the
        // bottom row of every full grid.
        let py = menuRoot.anchorY + menuRoot.anchorH + 6
        if (py + menuRoot.height > frame.height - 12)
            py = menuRoot.anchorY - 6 - menuRoot.height

        menuRoot.x = px
        menuRoot.y = Math.max(12, py)
    }

    // Both change when the confirm step swaps in, and the box has to be put
    // back under its button afterwards rather than growing off the edge.
    onWidthChanged:  if (menuRoot.visible) menuRoot.reposition()
    onHeightChanged: if (menuRoot.visible) menuRoot.reposition()

    // ── Controller ───────────────────────────────────────────────────────────
    function navigate(direction) {
        if (menuRoot.confirming) {
            // Armed, the pad only picks between REMOVE and CANCEL.
            if (direction !== "left" && direction !== "right")
                return
            if (menuRoot.armedChoice < 0) {
                menuRoot.armedChoice = menuRoot.armedCancel
                return
            }
            menuRoot.armedChoice = menuRoot.armedChoice === menuRoot.armedRemove
                                 ? menuRoot.armedCancel : menuRoot.armedRemove
            return
        }
        if (direction !== "up" && direction !== "down")
            return
        if (menuRoot.focusedItem < 0) {
            menuRoot.focusedItem = 0
            return
        }
        const step = direction === "down" ? 1 : -1
        menuRoot.focusedItem =
            (menuRoot.focusedItem + step + menuRoot.itemCount) % menuRoot.itemCount
    }

    function activateFocused() {
        if (menuRoot.confirming) {
            if (menuRoot.armedChoice === menuRoot.armedRemove)
                menuRoot.confirmRemove()
            else if (menuRoot.armedChoice === menuRoot.armedCancel)
                menuRoot.cancelConfirm()
            else
                menuRoot.armedChoice = menuRoot.armedCancel
            return
        }
        if (menuRoot.focusedItem < 0) {
            menuRoot.focusedItem = 0
            return
        }
        if (menuRoot.focusedItem === menuRoot.itemRemove)
            menuRoot.arm()
    }

    // Back drops the confirm step rather than the whole menu; the caller closes
    // the menu when this returns false.
    function handleBack() {
        if (!menuRoot.confirming)
            return false
        menuRoot.cancelConfirm()
        return true
    }

    function cancelConfirm() {
        menuRoot.confirming = false
        menuRoot.armedChoice = -1
    }

    function arm() {
        // Starts on CANCEL for the pad, unarmed for the mouse -- the same
        // -1 convention the rest of the selection uses.
        menuRoot.armedChoice = menuRoot.focusedItem >= 0 ? menuRoot.armedCancel : -1
        menuRoot.confirming = true
    }

    function confirmRemove() {
        if (menuRoot.gameName !== "")
            menuRoot.removeRequested(menuRoot.gameName, menuRoot.installDir)
        menuRoot.close()
    }

    // ── Shell ────────────────────────────────────────────────────────────────
    // Icon-wide until something has to be read: the first step is a single ✕,
    // and only the confirm has words in it.
    width: menuRoot.confirming ? 250 : 52
    height: menuColumn.implicitHeight
    padding: 0

    // Modal so a click outside is CONSUMED as well as closing this: without it
    // the same press lands on the card underneath and opens the details page.
    // dim stays off -- this is a small menu, not a page.
    modal: true
    dim: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onClosed: {
        menuRoot.confirming = false
        menuRoot.focusedItem = -1
        menuRoot.armedChoice = -1
    }

    background: Rectangle {
        color: Theme.bgSurface
        radius: 10
        border.color: Theme.borderControl
        border.width: 1
    }

    contentItem: Column {
        id: menuColumn
        padding: 10
        spacing: 8

        // ── Step 1: the action, as an icon ───────────────────────────────────
        // ✕ rather than a label. Same glyph the settings directory list uses
        // for "take this out", so it means the same thing in both places.
        Rectangle {
            id: removeRow
            readonly property bool emphasized:
                (menuRoot.focusedItem === menuRoot.itemRemove && menuRoot.padInControl)
                || (removeArea.containsMouse && menuRoot.mouseInControl)

            visible: !menuRoot.confirming
            width: 32; height: 32
            radius: 16
            color: removeRow.emphasized ? Theme.dangerRest : Theme.bgActive
            border.width: removeRow.emphasized ? 2 : 1
            border.color: removeRow.emphasized ? Theme.focusRing : Theme.borderControl

            Behavior on color { ColorAnimation { duration: 120 } }

            Text {
                anchors.centerIn: parent
                text: "✕"
                color: removeRow.emphasized ? Theme.textPrimary : Theme.textSecondary
                font.pixelSize: 14
            }

            MouseArea {
                id: removeArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: menuRoot.arm()
            }
        }

        // ── Step 2: the confirm ──────────────────────────────────────────────
        Column {
            visible: menuRoot.confirming
            spacing: 10

            Text {
                width: menuRoot.width - 28
                leftPadding: 4
                text: "Remove “" + menuRoot.gameName
                      + "” from the launcher? The game stays installed."
                color: Theme.textBody
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }

            Row {
                spacing: 8
                leftPadding: 4

                Rectangle {
                    id: confirmButton
                    readonly property bool emphasized:
                        (menuRoot.armedChoice === menuRoot.armedRemove && menuRoot.padInControl)
                        || (confirmArea.containsMouse && menuRoot.mouseInControl)

                    width: 96; height: 30; radius: 15
                    color: confirmButton.emphasized ? Theme.danger : Theme.dangerRest
                    border.width: confirmButton.emphasized ? 2 : 0
                    border.color: Theme.focusRing

                    Text {
                        anchors.centerIn: parent
                        text: "REMOVE"
                        color: Theme.textPrimary; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1
                    }
                    MouseArea {
                        id: confirmArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: menuRoot.confirmRemove()
                    }
                }

                Rectangle {
                    id: cancelButton
                    readonly property bool emphasized:
                        (menuRoot.armedChoice === menuRoot.armedCancel && menuRoot.padInControl)
                        || (cancelArea.containsMouse && menuRoot.mouseInControl)

                    width: 84; height: 30; radius: 15
                    color: cancelButton.emphasized ? Theme.bgEmphasis : "transparent"
                    border.color: cancelButton.emphasized ? Theme.focusRing : Theme.borderControl
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "CANCEL"
                        color: Theme.textSecondary; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1
                    }
                    MouseArea {
                        id: cancelArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: menuRoot.cancelConfirm()
                    }
                }
            }
        }
    }
}
