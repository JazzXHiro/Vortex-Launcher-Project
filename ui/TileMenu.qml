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
//
// A narrow column of icons -- Remove, Uninstall, Like -- that unrolls out of
// the ⋮ button rather than popping in, so it reads as coming from the card.
// ─────────────────────────────────────────────────────────────────────────────
Popup {
    id: menuRoot

    // Whichever input moved last owns the highlight, exactly as the tiles and
    // the details page do it -- a cursor left sitting on a row would otherwise
    // stay lit next to the one the pad is on.
    property var pad: controller
    readonly property bool padInControl: menuRoot.pad ? menuRoot.pad.padInControl : false
    readonly property bool mouseInControl: !menuRoot.padInControl

    // Read for the heart's state only; every action goes out as a signal.
    property var api: null

    // The game this menu was opened for. installDir is carried alongside the
    // name because it is the identity that survives a scan renaming a local
    // game (see VortexBridge::removeFromLibrary).
    property string gameName: ""
    property string installDir: ""
    // A Played row whose files are already gone has nothing to uninstall.
    property bool canUninstall: false
    // A local uninstall deletes the folder itself, so it gets a confirm step;
    // Steam games hand off to Steam, which prompts on its own.
    property bool isLocal: false

    // favoriteGames is referenced so this re-reads on favoritesChanged --
    // isFavorite() alone is a plain call with nothing to notify it.
    readonly property bool liked:
        !!menuRoot.api && menuRoot.gameName !== ""
        && !!menuRoot.api.favoriteGames
        && menuRoot.api.isFavorite(menuRoot.gameName)

    // The rows, top to bottom. Uninstall drops out where it cannot apply, so
    // the pad never lands on a row that does nothing.
    readonly property var items: menuRoot.canUninstall
                                 ? ["remove", "uninstall", "like"]
                                 : ["remove", "like"]

    // Which confirm is showing, if any: "" for the icon list, "remove" or
    // "uninstall" for its two-button step.
    property string confirming: ""

    // -1 means "mouse only", so nothing is outlined until the pad opens this.
    // Same rule as GameDetails.focusedAction.
    property int focusedItem: -1

    // Within the confirm step. Starts on CANCEL so a stray pad press removes
    // nothing, mirroring the uninstall confirm on the details page.
    readonly property int armedConfirm: 0
    readonly property int armedCancel: 1
    property int armedChoice: -1

    // Emitted rather than calling the bridge from in here. Removing or
    // uninstalling a game rebuilds the library list, and so does un-hearting
    // one on the Favorites tab -- holding the grid's scroll position across
    // that rebuild is the business of whoever owns the grid.
    signal removeRequested(string name, string installDir)
    signal uninstallRequested(string name)
    signal likeRequested(string name)

    // ── Opening ──────────────────────────────────────────────────────────────
    // Grown out of the ⋮ button that opened it: the panel's first cell sits
    // exactly over the button and draws the same dots, so the button reads as
    // stretching into the list rather than a box appearing beside it. Mapped
    // into the window's coordinates because this renders in the overlay layer,
    // where the grid's clip does not cut it off.
    //
    // The button's geometry is kept rather than used once, because the panel
    // widens when the confirm step replaces the icons, and the dots have to
    // stay over the button across that.
    property real anchorX: 0
    property real anchorY: 0
    property real anchorW: 0
    property real anchorH: 0

    // The ring of panel around the button, and so the width of the pill.
    readonly property real rim: 6
    readonly property real pillWidth: menuRoot.anchorW + 2 * menuRoot.rim
    readonly property real headerHeight: menuRoot.anchorH + 2 * menuRoot.rim

    // Set when opened: true when there was no room below, in which case the
    // button becomes the BOTTOM cell and the list stretches upwards out of it.
    property bool openUpward: false

    // 0 → 1 as the menu unrolls; the enter and exit transitions drive it.
    property real reveal: 0

    // Where the size is heading, rather than the eased value mid-flight --
    // placing against the animated height would flip the panel half way.
    readonly property real targetWidth: menuRoot.confirming !== "" ? 250 : menuRoot.pillWidth
    readonly property real targetHeight: menuRoot.headerHeight + menuBody.implicitHeight

    function openFor(anchorItem, name, dir, uninstallable, local) {
        if (!anchorItem)
            return
        menuRoot.gameName     = name
        menuRoot.installDir   = dir
        menuRoot.canUninstall = !!uninstallable
        menuRoot.isLocal      = !!local
        menuRoot.confirming   = ""
        menuRoot.armedChoice  = -1

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

        // Decided once per opening: flipping while the confirm step resizes
        // the panel would tear it off the button.
        menuRoot.openUpward = menuRoot.anchorY - menuRoot.rim + menuRoot.targetHeight
                              > frame.height - 12

        menuRoot.reposition()
        menuRoot.open()
    }

    function reposition() {
        // The right edge and the button's edge of the panel stay a rim's width
        // outside the button, so its dots land exactly on the real ones.
        menuRoot.x = menuRoot.anchorX + menuRoot.anchorW + menuRoot.rim - menuRoot.width
        menuRoot.y = menuRoot.openUpward
                   ? menuRoot.anchorY + menuRoot.anchorH + menuRoot.rim - menuRoot.height
                   : menuRoot.anchorY - menuRoot.rim
    }

    // Both ease when the confirm step swaps in, and the panel has to stay
    // fastened to its button through every frame of that.
    onWidthChanged:  if (menuRoot.visible) menuRoot.reposition()
    onHeightChanged: if (menuRoot.visible) menuRoot.reposition()

    // ── Controller ───────────────────────────────────────────────────────────
    function navigate(direction) {
        if (menuRoot.confirming !== "") {
            // Armed, the pad only picks between the two buttons.
            if (direction !== "left" && direction !== "right")
                return
            if (menuRoot.armedChoice < 0) {
                menuRoot.armedChoice = menuRoot.armedCancel
                return
            }
            menuRoot.armedChoice = menuRoot.armedChoice === menuRoot.armedConfirm
                                 ? menuRoot.armedCancel : menuRoot.armedConfirm
            return
        }
        if (direction !== "up" && direction !== "down")
            return
        if (menuRoot.focusedItem < 0) {
            menuRoot.focusedItem = 0
            return
        }
        const count = menuRoot.items.length
        const step = direction === "down" ? 1 : -1
        menuRoot.focusedItem = (menuRoot.focusedItem + step + count) % count
    }

    function activateFocused() {
        if (menuRoot.confirming !== "") {
            if (menuRoot.armedChoice === menuRoot.armedConfirm)
                menuRoot.confirmArmed()
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
        menuRoot.trigger(menuRoot.items[menuRoot.focusedItem])
    }

    // Back drops the confirm step rather than the whole menu; the caller closes
    // the menu when this returns false.
    function handleBack() {
        if (menuRoot.confirming === "")
            return false
        menuRoot.cancelConfirm()
        return true
    }

    function trigger(kind) {
        if (menuRoot.gameName === "")
            return
        if (kind === "remove") {
            menuRoot.arm("remove")
        } else if (kind === "uninstall") {
            if (menuRoot.isLocal) {
                menuRoot.arm("uninstall")
            } else {
                // Steam asks for itself; a second prompt here would be noise.
                menuRoot.uninstallRequested(menuRoot.gameName)
                menuRoot.close()
            }
        } else if (kind === "like") {
            // Stays open: the heart filling in is the confirmation.
            menuRoot.likeRequested(menuRoot.gameName)
        }
    }

    function cancelConfirm() {
        menuRoot.confirming = ""
        menuRoot.armedChoice = -1
    }

    function arm(kind) {
        // Starts on CANCEL for the pad, unarmed for the mouse -- the same
        // -1 convention the rest of the selection uses.
        menuRoot.armedChoice = menuRoot.focusedItem >= 0 ? menuRoot.armedCancel : -1
        menuRoot.confirming = kind
    }

    function confirmArmed() {
        if (menuRoot.gameName !== "") {
            if (menuRoot.confirming === "uninstall")
                menuRoot.uninstallRequested(menuRoot.gameName)
            else
                menuRoot.removeRequested(menuRoot.gameName, menuRoot.installDir)
        }
        menuRoot.close()
    }

    // ── Shell ────────────────────────────────────────────────────────────────
    // Button-wide until something has to be read: the first step is a pill of
    // icons, and only the confirm has words in it. Both dimensions ease rather
    // than jump when the confirm swaps in.
    width: menuRoot.targetWidth
    height: menuRoot.targetHeight
    padding: 0

    Behavior on width  { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
    Behavior on height { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

    // Modal so a click outside is CONSUMED as well as closing this: without it
    // the same press lands on the card underneath and opens the details page.
    // dim stays off -- this is a small menu, not a page.
    modal: true
    dim: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    // Pulled down out of the button: the pill starts as just the button's own
    // cell and stretches to full length, with the icons fading in a beat
    // behind so they do not pop at full strength into a sliver of panel.
    enter: Transition {
        NumberAnimation {
            property: "reveal"
            from: 0; to: 1
            duration: 280
            easing.type: Easing.OutCubic
        }
    }
    exit: Transition {
        NumberAnimation {
            property: "reveal"
            from: 1; to: 0
            duration: 170
            easing.type: Easing.InCubic
        }
    }

    onClosed: {
        menuRoot.confirming = ""
        menuRoot.focusedItem = -1
        menuRoot.armedChoice = -1
    }

    // Drawn inside the unrolling frame below instead, so the panel itself is
    // what grows rather than the contents sliding over a finished box.
    background: null

    contentItem: Item {
        implicitWidth: menuRoot.width
        implicitHeight: menuRoot.height

        Item {
            id: unroll
            width: parent.width
            // Never shorter than the button's own cell: at reveal 0 the panel
            // is exactly the button, gone solid.
            height: menuRoot.headerHeight
                    + (parent.height - menuRoot.headerHeight) * menuRoot.reveal
            // Grows out of whichever edge holds the button.
            y: menuRoot.openUpward ? parent.height - unroll.height : 0
            clip: true
            // Quick in, so the solid cell does not snap in over the
            // half-transparent button underneath it.
            opacity: Math.min(1, menuRoot.reveal * 5)

            Rectangle {
                anchors.fill: parent
                // See-through so the cover art shows behind it, like the ⋮ button
                // it grows out of; the icons on top stay solid.
                color: Qt.alpha(Theme.bgSurface, 0.7)
                radius: menuRoot.confirming !== "" ? 16 : menuRoot.pillWidth / 2
                border.color: Theme.borderControl
                border.width: 1
            }

            // ── The button's own cell ────────────────────────────────────────
            // The ⋮ redrawn where the real one sits, lit as "open". Clicking it
            // closes the menu, which is what clicking the real one would do.
            Item {
                id: header
                width: menuRoot.pillWidth
                height: menuRoot.headerHeight
                x: unroll.width - header.width
                y: menuRoot.openUpward ? unroll.height - header.height : 0

                Rectangle {
                    anchors.centerIn: parent
                    width: menuRoot.anchorW; height: menuRoot.anchorH
                    radius: width / 2
                    color: headerArea.containsMouse ? Qt.alpha(Theme.bgEmphasis, 0.8) : Qt.alpha(Theme.bgActive, 0.55)
                    border.width: 1
                    border.color: Theme.focusRing

                    Column {
                        anchors.centerIn: parent
                        spacing: 3
                        Repeater {
                            model: 3
                            Rectangle {
                                width: 4; height: 4; radius: 2
                                color: Theme.accent
                            }
                        }
                    }
                }

                MouseArea {
                    id: headerArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: menuRoot.close()
                }
            }

            // In the confirm step the panel is wide and the button's cell only
            // fills its corner, so the question's title goes beside it.
            Text {
                visible: menuRoot.confirming !== ""
                x: 14
                y: header.y
                width: unroll.width - header.width - 14
                height: menuRoot.headerHeight
                verticalAlignment: Text.AlignVCenter
                text: menuRoot.confirming === "uninstall" ? "UNINSTALL" : "REMOVE"
                color: Theme.textSecondary
                font.pixelSize: 11; font.bold: true; font.letterSpacing: 1
                elide: Text.ElideRight
            }

            Column {
                id: menuBody
                // Pinned against the button's cell so the rows stay put while
                // the frame unrolls over them, rather than riding along with it.
                y: menuRoot.openUpward
                   ? unroll.height - menuRoot.headerHeight - menuBody.height
                   : menuRoot.headerHeight
                width: unroll.width
                leftPadding: menuRoot.confirming !== "" ? 10 : menuRoot.rim
                rightPadding: menuBody.leftPadding
                topPadding: menuRoot.openUpward ? menuRoot.rim : 0
                bottomPadding: menuRoot.openUpward ? 0 : 10
                spacing: 6
                opacity: Math.max(0, (menuRoot.reveal - 0.3) / 0.7)

                // ── Step 1: the actions, as icons ────────────────────────────
                Repeater {
                    model: menuRoot.confirming === "" ? menuRoot.items : []

                    delegate: Column {
                        id: itemSlot
                        required property string modelData
                        required property int index
                        spacing: 6

                        // A hairline between rows -- and between the button's
                        // cell and the first one -- so the pill reads as a list.
                        Rectangle {
                            visible: !menuRoot.openUpward || itemSlot.index > 0
                            width: menuRoot.anchorW; height: 1
                            color: Theme.borderControl
                            opacity: 0.6
                        }

                        Rectangle {
                            id: itemButton
                            readonly property bool emphasized:
                                (menuRoot.focusedItem === itemSlot.index && menuRoot.padInControl)
                                || (itemArea.containsMouse && menuRoot.mouseInControl)
                            readonly property bool isLike: itemSlot.modelData === "like"
                            // Remove and Uninstall both take something away, so
                            // both warm to red; Like warms to the heart's pink.
                            readonly property color hotColor:
                                itemButton.isLike ? Theme.favorite : Theme.dangerRest
                            readonly property color glyphColor:
                                (itemButton.isLike && menuRoot.liked) ? Theme.favorite
                                : itemButton.emphasized ? Theme.textPrimary
                                : Theme.textSecondary

                            width: 32; height: 32
                            radius: 16
                            color: itemButton.emphasized
                                   && !(itemButton.isLike && menuRoot.liked)
                                   ? itemButton.hotColor : Qt.alpha(Theme.bgActive, 0.55)
                            border.width: itemButton.emphasized ? 2 : 1
                            border.color: itemButton.emphasized ? Theme.focusRing
                                        : (itemButton.isLike && menuRoot.liked) ? Theme.favorite
                                        : Theme.borderControl

                            Behavior on color { ColorAnimation { duration: 120 } }

                            // ✕ -- same glyph the settings directory list uses
                            // for "take this out", so it means the same thing.
                            Text {
                                visible: itemSlot.modelData === "remove"
                                anchors.centerIn: parent
                                text: "✕"
                                color: itemButton.glyphColor
                                font.pixelSize: 14
                            }

                            // A bin, drawn rather than typed for the same reason
                            // the ⋮ is: no glyph for it renders alike everywhere.
                            Item {
                                visible: itemSlot.modelData === "uninstall"
                                anchors.centerIn: parent
                                width: 12; height: 14
                                Rectangle {        // handle
                                    x: 4; y: 0; width: 4; height: 2; radius: 1
                                    color: itemButton.glyphColor
                                }
                                Rectangle {        // lid
                                    x: 0; y: 2; width: 12; height: 2; radius: 1
                                    color: itemButton.glyphColor
                                }
                                Rectangle {        // body
                                    x: 1.5; y: 5; width: 9; height: 9; radius: 2
                                    color: "transparent"
                                    border.width: 1.5
                                    border.color: itemButton.glyphColor
                                }
                            }

                            // The same heart the details page favourites with --
                            // a like here is that favourite, not a second list.
                            HeartIcon {
                                visible: itemButton.isLike
                                anchors.centerIn: parent
                                color: itemButton.glyphColor
                                size: 14
                                scale: menuRoot.liked ? 1.0 : 0.9
                                Behavior on scale {
                                    NumberAnimation { duration: 220; easing.type: Easing.OutBack }
                                }
                            }

                            MouseArea {
                                id: itemArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: menuRoot.trigger(itemSlot.modelData)
                            }

                            ToolTip.visible: itemArea.containsMouse && menuRoot.mouseInControl
                            ToolTip.delay: 500
                            ToolTip.text: itemSlot.modelData === "remove" ? "Remove from launcher"
                                        : itemSlot.modelData === "uninstall" ? "Uninstall"
                                        : menuRoot.liked ? "Unfavorite" : "Favorite"
                        }

                        // Upwards, the button's cell is under the last row, so
                        // that one carries the hairline beneath it instead.
                        Rectangle {
                            visible: menuRoot.openUpward
                                     && itemSlot.index === menuRoot.items.length - 1
                            width: menuRoot.anchorW; height: 1
                            color: Theme.borderControl
                            opacity: 0.6
                        }
                    }
                }

                // ── Step 2: the confirm ──────────────────────────────────────
                Column {
                    visible: menuRoot.confirming !== ""
                    spacing: 10

                    Text {
                        // Fixed rather than tracking the popup's width, which
                        // eases open and would reflow this on every frame.
                        width: 222
                        leftPadding: 4
                        text: menuRoot.confirming === "uninstall"
                              ? "Delete “" + menuRoot.gameName
                                + "” from disk? This can't be undone."
                              : "Remove “" + menuRoot.gameName
                                + "” from the launcher? The game stays installed."
                        color: menuRoot.confirming === "uninstall" ? Theme.dangerText : Theme.textBody
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }

                    Row {
                        spacing: 8
                        leftPadding: 4

                        Rectangle {
                            id: confirmButton
                            readonly property bool emphasized:
                                (menuRoot.armedChoice === menuRoot.armedConfirm && menuRoot.padInControl)
                                || (confirmArea.containsMouse && menuRoot.mouseInControl)

                            width: 96; height: 30; radius: 15
                            color: confirmButton.emphasized ? Theme.danger : Theme.dangerRest
                            border.width: confirmButton.emphasized ? 2 : 0
                            border.color: Theme.focusRing

                            Text {
                                anchors.centerIn: parent
                                text: menuRoot.confirming === "uninstall" ? "DELETE" : "REMOVE"
                                color: Theme.textPrimary; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1
                            }
                            MouseArea {
                                id: confirmArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: menuRoot.confirmArmed()
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
    }
}
