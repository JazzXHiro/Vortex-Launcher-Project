pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Effects
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

    // What the frosted backdrop is captured from. It must not contain this
    // popup: in a plain Window the overlay popups render into is a child of
    // the window's content item, so capturing that feeds the menu's own tint
    // back into its blur and it darkens with every redraw.
    property Item backdropItem: null
    // Where backdropItem sits in this popup's parent, read once per opening;
    // the capture rectangle is in backdropItem's coordinates.
    property point backdropOrigin: Qt.point(0, 0)

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
    // A hearted game that is not installed has no library row to remove --
    // the heart is the only thing keeping it in the launcher.
    property bool canRemove: true

    // favoriteGames is referenced so this re-reads on favoritesChanged --
    // isFavorite() alone is a plain call with nothing to notify it.
    readonly property bool liked:
        !!menuRoot.api && menuRoot.gameName !== ""
        && !!menuRoot.api.favoriteGames
        && menuRoot.api.isFavorite(menuRoot.gameName)

    // The rows, top to bottom. Remove and uninstall drop out where they cannot
    // apply, so the pad never lands on a row that does nothing.
    readonly property var items: {
        const rows = []
        if (menuRoot.canRemove) rows.push("remove")
        if (menuRoot.canUninstall) rows.push("uninstall")
        rows.push("like")
        return rows
    }

    // Which confirm is showing, if any: "" for the icon list, "remove" or
    // "uninstall" for its two-button step.
    property string confirming: ""

    // -1 means "mouse only", so nothing is outlined until the pad opens this.
    // Same rule as GameDetails.focusedAction.
    property int focusedItem: -1
    // The row under the mouse, or -1; the pad's counterpart is focusedItem.
    property int hoveredItem: -1

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

    // The panel around the button, kept to a hairline so the column stays
    // slim and its cells short -- just enough for the panel's edge to clear
    // the button's own outline. Every cell is this same square.
    readonly property real rim: 1
    readonly property real pillWidth: menuRoot.anchorW + 2 * menuRoot.rim
    readonly property real headerHeight: menuRoot.anchorH + 2 * menuRoot.rim

    // A rounded bar rather than a capsule: the cells are square-ish blocks
    // stacked under each other, and a capsule's half-circle ends would cut
    // into the first and last of them.
    readonly property real shellRadius: menuRoot.confirming !== "" ? 16 : 12
    // A lit cell lets a little of the cover art through, like the panel does.
    readonly property real fillAlpha: 0.7

    // Set when opened: true unless there was no room above, in which case the
    // button becomes the TOP cell and the list stretches downwards instead.
    // Upwards, the button is the BOTTOM cell and the list grows out of it.
    property bool openUpward: false

    // 0 → 1 as the menu unrolls; the enter and exit transitions drive it.
    property real reveal: 0

    // Where the size is heading, rather than the eased value mid-flight --
    // placing against the animated height would flip the panel half way.
    readonly property real targetWidth: menuRoot.confirming !== "" ? 250 : menuRoot.pillWidth
    readonly property real targetHeight: menuRoot.headerHeight + menuBody.implicitHeight

    function openFor(anchorItem, name, dir, uninstallable, local, removable) {
        if (!anchorItem)
            return
        menuRoot.gameName     = name
        menuRoot.installDir   = dir
        menuRoot.canUninstall = !!uninstallable
        menuRoot.isLocal      = !!local
        menuRoot.canRemove    = removable !== false
        menuRoot.confirming   = ""
        menuRoot.armedChoice  = -1

        // x and y are relative to the popup's parent -- the window's content
        // item, since this is declared as a child of the Window -- so that is
        // what the button's position has to be mapped into.
        const frame = menuRoot.parent
        if (!frame)
            return
        // Both corners mapped, not width/height read off the item: the card's
        // button sits on the cover and rides its hover scale, so its drawn
        // size is not its declared one.
        const at = anchorItem.mapToItem(frame, 0, 0)
        const far = anchorItem.mapToItem(frame, anchorItem.width, anchorItem.height)
        menuRoot.anchorX = at.x
        menuRoot.anchorY = at.y
        menuRoot.anchorW = far.x - at.x
        menuRoot.anchorH = far.y - at.y
        if (menuRoot.backdropItem) {
            const origin = menuRoot.backdropItem.mapToItem(frame, 0, 0)
            menuRoot.backdropOrigin = Qt.point(origin.x, origin.y)
        }

        // Upward by preference: the button sits at the foot of the card,
        // beside PLAY, so the list grows back up over the cover instead of
        // spilling off the bottom into the row below. Downward only when the
        // card is so near the top of the window that there is no room above.
        // Decided once per opening: flipping while the confirm step resizes
        // the panel would tear it off the button.
        menuRoot.openUpward = menuRoot.anchorY + menuRoot.anchorH + menuRoot.rim
                              - menuRoot.targetHeight >= 12

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
        // The rows are torn down for the confirm, and a row destroyed under
        // the cursor never reports the mouse leaving it.
        menuRoot.hoveredItem = -1
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

    // Not modal: a modal popup swallows the mouse wheel everywhere, so the page
    // could not be scrolled while this was open. A press outside still has to
    // be CONSUMED as well as closing this, though -- otherwise the same press
    // lands on the card underneath and opens the details page -- so the
    // window lays a catcher over the page while this is visible (see
    // tileMenuCatcher in main.qml) that eats presses and lets the wheel by.
    modal: false
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
    // Rolled back up into the button -- unless the page is scrolling out from
    // under it, when the card is already moving away and a roll towards where
    // the button was would chase nothing; then the whole panel just fades.
    exit: Transition {
        NumberAnimation {
            property: "reveal"
            from: 1; to: menuRoot.fadingOut ? 1 : 0
            duration: menuRoot.fadingOut ? 0 : 170
            easing.type: Easing.InCubic
        }
        NumberAnimation {
            property: "opacity"
            from: 1; to: menuRoot.fadingOut ? 0 : 1
            duration: menuRoot.fadingOut ? 80 : 0
            easing.type: Easing.OutQuad
        }
    }

    property bool fadingOut: false

    // The page under the menu is scrolling: get out of the way at once.
    function dismissForScroll() {
        if (!menuRoot.visible || menuRoot.fadingOut)
            return
        menuRoot.fadingOut = true
        menuRoot.close()
    }

    onClosed: {
        menuRoot.fadingOut = false
        menuRoot.opacity = 1
        menuRoot.confirming = ""
        menuRoot.focusedItem = -1
        menuRoot.hoveredItem = -1
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

            // ── Frosted backdrop ─────────────────────────────────────────────
            // Whatever is behind the panel, blurred, so the art seeping through
            // it reads as glass rather than competing with the icons. Captured
            // from backdropItem, which the caller points at the page beneath --
            // never at anything holding the overlay this popup renders in (see
            // backdropItem).
            //
            // A blur's edge pixels sample past the area they blur, so the
            // capture reaches a blur radius further out on every side and the
            // result is cut back to the panel's rounded shape afterwards;
            // otherwise the rim of the glass would fade to black.
            Item {
                id: backdrop
                readonly property real pad: backdropBlur.blurMax
                anchors.fill: parent
                visible: menuRoot.visible && menuRoot.backdropItem !== null

                layer.enabled: true
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: backdropMask
                }

                ShaderEffectSource {
                    id: backdropSource
                    x: -backdrop.pad; y: -backdrop.pad
                    width: backdrop.width + 2 * backdrop.pad
                    height: backdrop.height + 2 * backdrop.pad
                    visible: false
                    sourceItem: menuRoot.backdropItem
                    // The popup's x/y are in the parent's coordinates, and it
                    // has no margins to push it off them; shifted from there
                    // into backdropItem's own.
                    sourceRect: Qt.rect(menuRoot.x + unroll.x - backdrop.pad - menuRoot.backdropOrigin.x,
                                        menuRoot.y + unroll.y - backdrop.pad - menuRoot.backdropOrigin.y,
                                        backdropSource.width, backdropSource.height)
                    live: menuRoot.visible
                }

                MultiEffect {
                    id: backdropBlur
                    anchors.fill: backdropSource
                    source: backdropSource
                    autoPaddingEnabled: false
                    blurEnabled: true
                    blur: 0.8
                    // Kept tight: the panel is barely wider than the button,
                    // and a wide blur drags in colour from well past the card.
                    blurMax: 16
                }

                // A light tint over the glass so the icons have something to
                // sit on; inside the masked layer, so it is cut to the same
                // shape -- hole and all -- as the blur.
                Rectangle {
                    anchors.fill: parent
                    color: Qt.alpha(Theme.bgSurface, 0.45)
                }
            }

            // ── The glass's shape ────────────────────────────────────────────
            // The panel's outline, minus the lit cell: that one's colour sits
            // straight over the art instead of over frosted glass, so it reads
            // clean rather than smeared. Built as the strips above and below
            // the lit cell, since a mask can only add coverage, not punch it.
            readonly property int litItem:
                menuRoot.padInControl ? menuRoot.focusedItem
                : menuRoot.mouseInControl ? menuRoot.hoveredItem : -1
            readonly property real holeY:
                menuRoot.confirming !== "" ? -1
                : headerFill.lit ? header.y
                : unroll.litItem >= 0
                  ? menuBody.y + menuBody.topPadding + unroll.litItem * menuRoot.headerHeight
                : -1
            readonly property bool hasHole: unroll.holeY >= 0

            Item {
                id: backdropMask
                anchors.fill: parent
                visible: false
                layer.enabled: true

                Rectangle {
                    width: parent.width
                    height: unroll.hasHole ? unroll.holeY : parent.height
                    topLeftRadius: menuRoot.shellRadius
                    topRightRadius: menuRoot.shellRadius
                    bottomLeftRadius: unroll.hasHole ? 0 : menuRoot.shellRadius
                    bottomRightRadius: unroll.hasHole ? 0 : menuRoot.shellRadius
                }
                // Collapsed to nothing rather than hidden when there is no
                // hole: a child of a hidden layered item that turns visible
                // later is never drawn into the layer, which left everything
                // below the lit cell bare.
                Rectangle {
                    y: unroll.hasHole ? unroll.holeY + menuRoot.headerHeight : parent.height
                    width: parent.width
                    height: Math.max(0, parent.height - y)
                    bottomLeftRadius: menuRoot.shellRadius
                    bottomRightRadius: menuRoot.shellRadius
                }
            }

            // ── The button's own cell ────────────────────────────────────────
            // The ⋮ redrawn where the real one sits, lit as "open" -- the
            // panel's first cell, like the action cells below it, rather than a
            // circle of its own. Clicking it closes the menu, which is what
            // clicking the real one would do.
            Item {
                id: header
                width: menuRoot.pillWidth
                height: menuRoot.headerHeight
                x: unroll.width - header.width
                y: menuRoot.openUpward ? unroll.height - header.height : 0

                Rectangle {
                    id: headerFill
                    readonly property bool lit: headerArea.containsMouse && menuRoot.mouseInControl
                    // Rounded only on the panel's outer corners, so the fill
                    // is the cell itself lighting up. In the confirm step the
                    // cell sits in the panel's right-hand corner, so its left
                    // side is interior.
                    readonly property real outer: menuRoot.shellRadius
                    readonly property real leftOuter: menuRoot.confirming !== "" ? 0 : headerFill.outer

                    anchors.fill: parent
                    topLeftRadius:     menuRoot.openUpward ? 0 : headerFill.leftOuter
                    topRightRadius:    menuRoot.openUpward ? 0 : headerFill.outer
                    bottomLeftRadius:  menuRoot.openUpward ? headerFill.leftOuter : 0
                    bottomRightRadius: menuRoot.openUpward ? headerFill.outer : 0
                    color: headerFill.lit ? Qt.alpha(Theme.bgPressed, menuRoot.fillAlpha) : "transparent"
                    Behavior on color { ColorAnimation { duration: 120 } }

                    Column {
                        anchors.centerIn: parent
                        spacing: 3
                        // Turns from ⋮ to ⋯ as the menu unrolls, and back as it
                        // rolls up -- riding reveal so it moves in step with
                        // the panel rather than on a clock of its own.
                        rotation: 90 * menuRoot.reveal
                        Repeater {
                            model: 3
                            Rectangle {
                                width: 4; height: 4; radius: 2
                                color: headerFill.lit ? Theme.textInverse : Theme.accent
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
                // The icon cells run edge to edge and butt against each
                // other; only the confirm step's text needs breathing room.
                leftPadding: menuRoot.confirming !== "" ? 10 : 0
                rightPadding: menuBody.leftPadding
                topPadding: menuRoot.confirming !== "" && menuRoot.openUpward ? 6 : 0
                bottomPadding: menuRoot.confirming !== "" && !menuRoot.openUpward ? 10 : 0
                spacing: 0
                opacity: Math.max(0, (menuRoot.reveal - 0.3) / 0.7)

                // ── Step 1: the actions, as icons ────────────────────────────
                Repeater {
                    model: menuRoot.confirming === "" ? menuRoot.items : []

                    // Each action is a cell of the panel itself, the same size
                    // as the button's own, split from its neighbours by a
                    // full-width rule -- no button-in-a-button.
                    delegate: Item {
                        id: itemSlot
                        required property string modelData
                        required property int index
                        width: menuRoot.pillWidth
                        height: menuRoot.headerHeight

                        // The rule on the side facing the button's cell, so
                        // there is one between that cell and the first row and
                        // one between every pair of rows after it.
                        // Above the fill, so a lit cell keeps its edge.
                        Rectangle {
                            z: 1
                            y: menuRoot.openUpward ? itemSlot.height - 1 : 0
                            width: itemSlot.width; height: 1
                            // A faint light line rather than a dark one, so it
                            // reads as a seam in the glass, not a black rule.
                            color: Qt.alpha(Theme.textPrimary, 0.18)
                        }

                        Rectangle {
                            id: itemButton
                            readonly property bool emphasized:
                                (menuRoot.focusedItem === itemSlot.index && menuRoot.padInControl)
                                || (itemArea.containsMouse && menuRoot.mouseInControl)
                            readonly property bool isLike: itemSlot.modelData === "like"
                            // Remove and Uninstall both take something away, so
                            // both light red; Like lights the same whitish grey
                            // as the ⋮ cell, which the pink heart reads against.
                            readonly property color hotColor:
                                Qt.alpha(itemButton.isLike ? Theme.bgPressed : Theme.dangerRest, menuRoot.fillAlpha)
                            readonly property color glyphColor:
                                (itemButton.isLike && menuRoot.liked) ? Theme.favorite
                                : (itemButton.emphasized && itemButton.isLike) ? Theme.textInverse
                                : itemButton.emphasized ? Theme.textPrimary
                                : Theme.textSecondary

                            // The end cell's fill takes the panel's rounded
                            // corners on its outer side; every other edge is
                            // square against a neighbour.
                            readonly property bool atTop:
                                menuRoot.openUpward && itemSlot.index === 0
                            readonly property bool atBottom:
                                !menuRoot.openUpward && itemSlot.index === menuRoot.items.length - 1

                            // The whole cell lights, not a box inside it.
                            anchors.fill: parent
                            topLeftRadius:     itemButton.atTop ? menuRoot.shellRadius : 0
                            topRightRadius:    itemButton.atTop ? menuRoot.shellRadius : 0
                            bottomLeftRadius:  itemButton.atBottom ? menuRoot.shellRadius : 0
                            bottomRightRadius: itemButton.atBottom ? menuRoot.shellRadius : 0
                            color: itemButton.emphasized ? itemButton.hotColor : "transparent"

                            Behavior on color { ColorAnimation { duration: 120 } }

                            // ✕ -- the settings directory list's "take this out"
                            // mark, so it means the same thing. Drawn from two
                            // bars rather than typed, like the bin beside it:
                            // the glyph comes from whatever fallback font has
                            // it, and renders hairline-thin there.
                            Item {
                                visible: itemSlot.modelData === "remove"
                                anchors.centerIn: parent
                                width: 12; height: 12
                                Repeater {
                                    model: [45, -45]
                                    Rectangle {
                                        required property int modelData
                                        anchors.centerIn: parent
                                        width: 15; height: 2.5; radius: 1.25
                                        rotation: modelData
                                        antialiasing: true
                                        color: itemButton.glyphColor
                                    }
                                }
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
                                onContainsMouseChanged: {
                                    if (itemArea.containsMouse)
                                        menuRoot.hoveredItem = itemSlot.index
                                    else if (menuRoot.hoveredItem === itemSlot.index)
                                        menuRoot.hoveredItem = -1
                                }
                            }

                            ToolTip.visible: itemArea.containsMouse && menuRoot.mouseInControl
                            ToolTip.delay: 500
                            ToolTip.text: itemSlot.modelData === "remove" ? "Remove from launcher"
                                        : itemSlot.modelData === "uninstall" ? "Uninstall"
                                        : menuRoot.liked ? "Unfavorite" : "Favorite"
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
