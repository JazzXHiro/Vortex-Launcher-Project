pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import Vortex

Popup {
    id: detailsRoot

    property string selectedGameName: ""
    // Which surface opened this page, so a launch can be attributed to it.
    property string launchOrigin: "Library"
    property var api: vortexApi

    // Whichever input moved last owns the highlights — a cursor left sitting on
    // a button would otherwise stay lit next to the one the pad is moving.
    property var pad: controller
    readonly property bool padInControl: detailsRoot.pad ? detailsRoot.pad.padInControl : false
    readonly property bool mouseInControl: !detailsRoot.padInControl
    property var gameData: findGameData()

    // ── Controller focus ─────────────────────────────────────────────────────
    // -1 means "mouse only", so nothing is outlined until the pad opens this
    // page. Uninstall is the last stop on the same left/right run as everything
    // else — parking it off-axis only made it look like the selection vanished.
    // Owned games get Play / Favourite / Uninstall; unowned discovery picks get
    // Check on Steam / Favourite / Wishlist. Both sets are three wide, so the
    // focus run does not change length — only what each slot does.
    readonly property bool isOwned:
        !!detailsRoot.gameData && detailsRoot.gameData.matched !== false

    readonly property int actionPlay: 0        // doubles as "Check on Steam" when unowned
    readonly property int actionFavorite: 1
    readonly property int actionUninstall: 2   // doubles as "Add to Wishlist" when unowned
    readonly property int actionCount: 3
    property int focusedAction: -1

    // isWishlisted() is a plain method, not a bindable property, so bumping
    // this is what re-evaluates the button's saved state.
    property int wishlistRevision: 0

    // The exe just picked through "Change executable". The bridge patches the
    // row in place, which does not re-run findGameData(), so the menu reads
    // this until the page moves to another game.
    property string exeOverride: ""

    // Title rename. editingTitle swaps the heading for an input; renaming
    // marks the selectedGameName change a commit makes, so it is not handled as
    // the user opening a different game.
    property bool editingTitle: false
    property bool renaming: false
    readonly property bool canRename:
        detailsRoot.isOwned && !!detailsRoot.api && !detailsRoot.api.isLoading
        && !detailsRoot.gameLaunching && !detailsRoot.gameRunning
        && !!detailsRoot.gameData && !!detailsRoot.gameData.installDir

    // Windows' own icon font. Win11 ships Segoe Fluent Icons, Win10 only MDL2;
    // the codepoints are shared.
    readonly property string iconFont:
        Qt.fontFamilies().indexOf("Segoe Fluent Icons") >= 0
            ? "Segoe Fluent Icons" : "Segoe MDL2 Assets"

    readonly property int steamAppId:
        detailsRoot.gameData && detailsRoot.gameData.steamAppId
            ? detailsRoot.gameData.steamAppId : 0

    // ── Banner sources ───────────────────────────────────────────────────────
    // Whether heroPath is genuinely wide art is decided from the loaded image,
    // not from where it came from. Three things reach this slot and only the
    // first is a banner: Steam's library_hero.jpg at 1920x620, IGDB's t_720p
    // at 540x720, and the cover the bridge aliases in when neither exists, at
    // 264x352. The last two are portrait, and cropping a portrait into a
    // 1720x490 banner is the upscale that made this page look broken — so the
    // shape of what actually loaded is what picks the treatment.
    readonly property string heroSource:
        detailsRoot.gameData ? (detailsRoot.gameData.heroPath || "") : ""
    readonly property string logoSource:
        detailsRoot.gameData ? (detailsRoot.gameData.logoPath || "") : ""
    readonly property string coverSource:
        detailsRoot.gameData ? (detailsRoot.gameData.coverPath || "") : ""

    // IGDB's Steam links are good but not complete, and a game may simply not
    // be on Steam. A store search is a useful degradation and keeps the button
    // from changing shape depending on data.
    readonly property string steamUrl:
        detailsRoot.steamAppId > 0
            ? "https://store.steampowered.com/app/" + detailsRoot.steamAppId + "/"
            : "https://store.steampowered.com/search/?term="
              + encodeURIComponent(detailsRoot.gameData ? detailsRoot.gameData.name : "")

    // ── Uninstall confirm step ───────────────────────────────────────────────
    // Steam games hand off to Steam, which prompts on its own. A local game is
    // deleted off the disk by us, with nothing to undo it, so that one gets a
    // confirm row first. armedChoice follows the same -1 = "mouse only" rule as
    // focusedAction, and starts on CANCEL so a stray pad press deletes nothing.
    readonly property int armedDelete: 0
    readonly property int armedCancel: 1
    property bool uninstallArmed: false
    property int armedChoice: -1
    readonly property bool isLocalGame:
        !!detailsRoot.gameData && detailsRoot.gameData.source === "Local"

    readonly property string playName: detailsRoot.gameData ? detailsRoot.gameData.name : ""
    // Where this game's launch is, from the bridge; drives the Play button.
    readonly property bool gameLaunching:
        !!detailsRoot.api && detailsRoot.playName !== "" && detailsRoot.api.launchingGames.indexOf(detailsRoot.playName) >= 0
    readonly property bool gameRunning:
        !!detailsRoot.api && detailsRoot.playName !== "" && detailsRoot.api.runningGames.indexOf(detailsRoot.playName) >= 0

    // Play does nothing while a launch is under way, and quits once it is up.
    function playOrQuit() {
        if (detailsRoot.playName === "" || !detailsRoot.api) return
        if (detailsRoot.gameRunning)
            detailsRoot.api.quitGame(detailsRoot.playName)
        else if (!detailsRoot.gameLaunching)
            detailsRoot.api.launchGameFrom(detailsRoot.playName, detailsRoot.launchOrigin)
    }

    function requestUninstall() {
        if (!detailsRoot.gameData || !detailsRoot.api)
            return
        if (!detailsRoot.isLocalGame) {
            detailsRoot.api.uninstallGame(detailsRoot.gameData.name)
            detailsRoot.close()
            return
        }
        detailsRoot.armedChoice =
            detailsRoot.focusedAction >= 0 ? detailsRoot.armedCancel : -1
        detailsRoot.uninstallArmed = true
    }

    function confirmUninstall() {
        if (!detailsRoot.gameData || !detailsRoot.api)
            return
        detailsRoot.cancelUninstall()
        detailsRoot.api.uninstallGame(detailsRoot.gameData.name)
        detailsRoot.close()
    }

    function cancelUninstall() {
        detailsRoot.uninstallArmed = false
        detailsRoot.armedChoice = -1
    }

    function beginRename() {
        if (!detailsRoot.canRename || detailsRoot.editingTitle) return
        detailsRoot.cancelUninstall()
        titleEdit.text = detailsRoot.gameData.name
        detailsRoot.editingTitle = true
        titleEdit.forceActiveFocus()
        titleEdit.selectAll()
    }

    function cancelRename() {
        detailsRoot.editingTitle = false
    }

    // Moves the page onto a renamed row without treating it as a new game.
    function followRename(installDir) {
        const live = detailsRoot.api.gameDetailsForInstallDir(installDir)
        if (!live || !live.name) return
        if (live.name !== detailsRoot.selectedGameName) {
            detailsRoot.renaming = true
            detailsRoot.selectedGameName = live.name
            detailsRoot.renaming = false
        }
        detailsRoot.gameData = live
    }

    // Empty puts the scanned title back; the bridge decides what that is, so
    // the page reads the name back rather than assuming the input's.
    function commitRename() {
        if (!detailsRoot.editingTitle) return
        detailsRoot.editingTitle = false
        if (!detailsRoot.gameData || !detailsRoot.api) return
        const typed = titleEdit.text.trim()
        if (typed === detailsRoot.gameData.name) return
        const installDir = detailsRoot.gameData.installDir
        if (detailsRoot.api.renameGame(installDir, typed))
            detailsRoot.followRename(installDir)
    }

    // Back while armed drops the confirm row instead of the whole page; the
    // caller keeps the page open when this returns true.
    function handleBack() {
        if (detailsRoot.editingTitle) {
            detailsRoot.cancelRename()
            return true
        }
        if (!detailsRoot.uninstallArmed)
            return false
        detailsRoot.cancelUninstall()
        return true
    }

    function navigate(direction) {
        if (detailsRoot.editingTitle)
            return
        if (detailsRoot.uninstallArmed) {
            // Armed, the pad only picks between DELETE and CANCEL.
            if (direction !== "left" && direction !== "right")
                return
            if (detailsRoot.armedChoice < 0) {
                detailsRoot.armedChoice = detailsRoot.armedCancel
                return
            }
            detailsRoot.armedChoice =
                detailsRoot.armedChoice === detailsRoot.armedDelete
                    ? detailsRoot.armedCancel
                    : detailsRoot.armedDelete
            return
        }
        // Every action sits in one row now, so up/down scroll the page the way
        // the Discover page does; left/right walk the row.
        if (direction === "up" || direction === "down") {
            detailsRoot.scrollBy(direction === "down" ? 240 : -240)
            return
        }
        if (detailsRoot.focusedAction < 0) {
            detailsRoot.focusedAction = detailsRoot.actionPlay
            return
        }
        const step = direction === "right" ? 1 : -1
        detailsRoot.focusedAction =
            (detailsRoot.focusedAction + step + detailsRoot.actionCount) % detailsRoot.actionCount
    }

    function activateFocusedAction() {
        if (!detailsRoot.gameData || !detailsRoot.api || detailsRoot.editingTitle)
            return
        if (detailsRoot.uninstallArmed) {
            if (detailsRoot.armedChoice === detailsRoot.armedDelete)
                detailsRoot.confirmUninstall()
            else if (detailsRoot.armedChoice === detailsRoot.armedCancel)
                detailsRoot.cancelUninstall()
            else
                detailsRoot.armedChoice = detailsRoot.armedCancel
            return
        }
        switch (detailsRoot.focusedAction) {
        case detailsRoot.actionPlay:
            if (detailsRoot.isOwned)
                detailsRoot.playOrQuit()
            else
                Qt.openUrlExternally(detailsRoot.steamUrl)
            break
        case detailsRoot.actionFavorite:
            detailsRoot.api.updatePreference(detailsRoot.gameData.name, 1.0)
            break
        case detailsRoot.actionUninstall:
            if (detailsRoot.isOwned)
                detailsRoot.requestUninstall()
            else
                detailsRoot.api.toggleWishlist(detailsRoot.gameData.name)
            break
        default:
            detailsRoot.focusedAction = detailsRoot.actionPlay
        }
    }

    function findIn(list) {
        if (!list) return null;
        for (let i = 0; i < list.length; i++) {
            if (list[i].name === detailsRoot.selectedGameName) return list[i];
        }
        return null;
    }

    function findGameData() {
        if (!detailsRoot.api || detailsRoot.selectedGameName === "") return null;

        // The library first — an owned game always wins, so a title that is
        // both installed and present in the catalog opens as the owned copy.
        const owned = detailsRoot.findIn(detailsRoot.api.gameList);
        if (owned) return owned;

        // Then discovery picks and saved wishlist entries. Without these an
        // unowned game resolved to null and the whole page rendered blank,
        // since it has no row in the launcher's game list to read.
        // playedGames last: a game that is both played and installed was
        // already found above, so this only ever resolves the uninstalled ones.
        return detailsRoot.findIn(detailsRoot.api.recommendationList)
            || detailsRoot.findIn(detailsRoot.api.wishlistGames)
            || detailsRoot.findIn(detailsRoot.api.favoriteGames)
            || detailsRoot.findIn(detailsRoot.api.playedGames);
    }

    // A refresh must never blank the page under the user.
    //
    // findGameData() can only resolve an unowned favourite through
    // favoriteGames(), and taking the heart off drops it out of that list --
    // leaving the page they are still looking at with no row at all: no title,
    // no art, "Unknown" in every field. Nothing was deleted, so keep showing
    // what is on screen; opening another game or reopening this one goes
    // through onSelectedGameNameChanged, which assigns unconditionally and is
    // where a genuinely unresolvable name still has to end up null.
    function refreshGameData() {
        const found = detailsRoot.findGameData();
        if (found) {
            detailsRoot.gameData = found;
            return;
        }
        if (!detailsRoot.gameData)
            return;

        // The one field the kept row must not carry over. `status` is baked in
        // when the row is built and never rewritten, so holding 1.0 here would
        // keep the heart lit after the click that just cleared it. Zeroed, the
        // button falls through to favoriteGames(), which is live. A copy, not
        // an edit in place: assigning the same object back notifies nothing.
        if (detailsRoot.gameData.status === undefined
            || detailsRoot.gameData.status === 0.0)
            return;
        const kept = Object.assign({}, detailsRoot.gameData);
        kept.status = 0.0;
        detailsRoot.gameData = kept;
    }

    Connections {
        target: detailsRoot.api
        function onGameListChanged() { detailsRoot.refreshGameData(); }
        // Hearting no longer emits gameListChanged -- it patches the row in
        // place so the library grid keeps its scroll position -- so re-read the
        // row here too, or an un-hearted owned game keeps its stale status: 1.0
        // and the heart stays lit until the page is reopened.
        function onFavoritesChanged() { detailsRoot.refreshGameData(); }
        function onRecommendationListChanged() { detailsRoot.refreshGameData(); }
        function onWishlistChanged() {
            detailsRoot.refreshGameData();
            detailsRoot.wishlistRevision++;
        }
        function onPlayedGamesChanged() { detailsRoot.refreshGameData(); }
        // A rename rewrites the row in place, then again once its new art and
        // metadata land; neither notifies gameList.
        function onGameRowChanged(installDir) {
            if (detailsRoot.gameData && detailsRoot.gameData.installDir === installDir)
                detailsRoot.followRename(installDir);
        }
    }

    // A Discover pick resolves to its recommendationList entry, and that
    // entry's `status` is baked in at rank time and never rewritten -- so on an
    // unowned game the snapshot alone reads 0.0 forever and the heart never
    // lit. favoriteGames() is the live answer (it re-checks preferences.json)
    // and its favoritesChanged notify is exactly what updatePreference() emits.
    readonly property bool isFavorite: {
        if (!detailsRoot.gameData) return false
        if (detailsRoot.gameData.status === 1.0) return true
        return !!detailsRoot.api && !!detailsRoot.findIn(detailsRoot.api.favoriteGames)
    }

    readonly property bool isWishlisted:
        !!detailsRoot.api && !!detailsRoot.gameData
        && detailsRoot.wishlistRevision >= 0
        && detailsRoot.api.isWishlisted(detailsRoot.gameData.name)

    function scrollBy(delta) {
        const limit = Math.max(0, body.contentHeight - body.height)
        body.contentY = Math.max(0, Math.min(limit, body.contentY + delta))
    }

    // True once the page's own title has scrolled up under the sticky header,
    // which then carries it -- the same hand-off BrowseDetails does.
    property bool titleDocked: false

    function updateTitleDock() {
        const bottom = titleText.mapToItem(page, 0, titleText.height).y
        detailsRoot.titleDocked = bottom - body.contentY < stickyHeader.height
    }

    // Never carry an armed confirm across games or across an open/close cycle.
    onOpened: {
        detailsRoot.cancelUninstall()
        detailsRoot.cancelRename()
        body.contentY = 0
    }

    onSelectedGameNameChanged: {
        if (detailsRoot.renaming)
            return;   // same game, new title -- followRename() sets gameData
        detailsRoot.cancelUninstall();
        detailsRoot.cancelRename();
        detailsRoot.exeOverride = "";
        detailsRoot.gameData = findGameData();
        if (detailsRoot.gameData && detailsRoot.api) {
            detailsRoot.api.logGameClick(detailsRoot.gameData.name, detailsRoot.gameData.genres, detailsRoot.gameData.tags);
            // Unowned picks have no SteamGridDB art. Ask for a hero and logo
            // here rather than when the list loads: this is the only place they
            // are ever shown, and the bridge no-ops on anything already cached
            // or already known to have none. The Connections block above swaps
            // them in when they land, so the page updates without reopening.
            if (!detailsRoot.isOwned) {
                detailsRoot.api.ensureArtwork(detailsRoot.gameData.name);
                // Same lazy deal for the text: a saved favourite whose list
                // entry is long gone has only a name, and this is where the
                // developer, genres and rating come back.
                detailsRoot.api.ensureMetadata(detailsRoot.gameData.name);
            }
        }
    }

    width: parent.width * 0.9; height: parent.height * 0.9
    x: (parent.width - width) / 2; y: (parent.height - height) / 2
    modal: true; focus: true
    padding: 0
    // While the title is being edited, Escape belongs to the input (cancel)
    // and a click outside only ends the edit.
    closePolicy: detailsRoot.editingTitle
                 ? Popup.NoAutoClose
                 : Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Theme.bgPanel
        radius: 20
        border.color: Theme.borderQuiet
        border.width: 1
    }

    // ── Reusable bits ───────────────────────────────────────────────────────
    // The Discover page's own pieces (BrowseDetails.qml), so the two pages
    // read as one design.
    component Chip: Rectangle {
        id: chip
        property string label
        width: chipText.implicitWidth + 22; height: 26; radius: 13
        color: Theme.overlayButton
        border.color: Theme.borderControl
        Text {
            id: chipText
            anchors.centerIn: parent
            text: chip.label
            color: Theme.textBody
            font.pixelSize: 12
        }
    }

    component SectionTitle: Text {
        color: Theme.textMuted
        font.pixelSize: 13; font.bold: true; font.letterSpacing: 2
    }

    component ActionButton: Rectangle {
        id: action
        property string label
        property string glyph
        // Draws HeartIcon in the glyph's place.
        property bool heart: false
        property bool active: false
        property bool focused: false
        property color activeColor: Theme.positiveDim
        property color restColor: Theme.bgActive
        property color hoverColor: Theme.bgEmphasis
        property color ringColor: Theme.focusRing
        property color labelColor: Theme.textPrimary
        property color glyphColor: action.labelColor
        property string iconSource: ""
        property int iconSize: 22
        signal triggered()

        // Pad focus and mouse hover light every action the same way, so the
        // page looks identical whichever one you are holding.
        readonly property bool emphasized:
            (action.focused && detailsRoot.padInControl)
            || (actionArea.containsMouse && detailsRoot.mouseInControl)

        height: 50; radius: 8
        width: actionRow.implicitWidth + 36
        color: action.active ? action.activeColor
                             : (action.emphasized ? action.hoverColor : action.restColor)
        border.width: action.emphasized ? 3 : 0
        border.color: action.ringColor
        scale: action.emphasized ? 1.06 : 1.0

        Behavior on color { ColorAnimation { duration: 150 } }
        Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutQuart } }

        Row {
            id: actionRow
            anchors.centerIn: parent
            spacing: 8
            // Degrades to a text-only button if the asset is missing, rather
            // than leaving a blank gap.
            Image {
                anchors.verticalCenter: parent.verticalCenter
                visible: action.iconSource !== "" && status === Image.Ready
                source: action.iconSource
                // Decoded at twice the drawn size so it stays sharp on a
                // scaled display.
                sourceSize.width: action.iconSize * 2
                sourceSize.height: action.iconSize * 2
                width: visible ? action.iconSize : 0
                height: action.iconSize
                fillMode: Image.PreserveAspectFit
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: action.glyph !== ""
                text: action.glyph
                color: action.glyphColor
                font.pixelSize: 18
            }
            HeartIcon {
                anchors.verticalCenter: parent.verticalCenter
                visible: action.heart
                color: action.glyphColor
                size: 18
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: action.label
                color: action.labelColor
                font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
            }
        }

        MouseArea {
            id: actionArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: action.triggered()
        }
    }

    contentItem: Item {
        clip: true

        Flickable {
            id: body
            anchors.fill: parent
            contentWidth: width
            contentHeight: page.height
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: VortexScrollBar { }

            onContentYChanged: detailsRoot.updateTitleDock()

            Column {
                id: page
                width: body.width
                spacing: 0

                onHeightChanged: detailsRoot.updateTitleDock()

                // ── Hero ────────────────────────────────────────────────────
                // Steam's library header (FrostedHero.qml); what is declared
                // inside goes into its frosted band.
                FrostedHero {
                    id: heroBanner
                    width: parent.width
                    height: Math.max(460, Math.round(body.height * 0.6))
                    heroSource: detailsRoot.heroSource
                    logoSource: detailsRoot.logoSource
                    coverSource: detailsRoot.coverSource

                    // Double-click the title to rename an owned game. The new
                    // name is what IGDB and SteamGridDB are searched by from
                    // then on; an empty one restores the scanned title.
                    Item {
                        width: parent.width
                        height: titleText.height

                        Text {
                            id: titleText
                            width: Math.min(parent.width, implicitWidth)
                            visible: !detailsRoot.editingTitle
                            text: detailsRoot.gameData ? detailsRoot.gameData.name : ""
                            color: Theme.textPrimary
                            font.pixelSize: 40; font.bold: true
                            elide: Text.ElideRight

                            MouseArea {
                                anchors.fill: parent
                                enabled: detailsRoot.canRename
                                cursorShape: Qt.IBeamCursor
                                onDoubleClicked: detailsRoot.beginRename()
                            }
                        }

                        TextInput {
                            id: titleEdit
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            visible: detailsRoot.editingTitle
                            clip: true
                            color: Theme.textPrimary
                            selectionColor: Theme.accent
                            selectedTextColor: Theme.textInverse
                            font.pixelSize: 40; font.bold: true
                            selectByMouse: true

                            onAccepted: detailsRoot.commitRename()
                            onActiveFocusChanged: {
                                if (!activeFocus && detailsRoot.editingTitle)
                                    detailsRoot.commitRename()
                            }
                            Keys.onEscapePressed: function(event) {
                                detailsRoot.cancelRename()
                                event.accepted = true
                            }

                            // The edit's extent, so it reads as a field rather
                            // than a heading with a cursor in it.
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.bottom
                                anchors.topMargin: 2
                                height: 2
                                color: Theme.accent
                            }
                        }
                    }

                    // Play or Check on Steam, the heart, and for an owned game
                    // the two numbers Steam keeps beside its Play button. The
                    // third focus slot sits on the same line at the far right:
                    // Uninstall when owned, Add to Wishlist when not.
                    Item {
                        width: parent.width
                        height: actionBar.height
                        visible: !detailsRoot.uninstallArmed

                        Row {
                            id: actionBar
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 14

                            ActionButton {
                                visible: detailsRoot.isOwned
                                // PLAY, then LAUNCHING on green until the game is up, then QUIT,
                                // which turns red on hover with the icon and label flipped to white.
                                readonly property bool quitHot: detailsRoot.gameRunning && emphasized
                                iconSource: detailsRoot.gameLaunching ? ""
                                          : quitHot ? "assets/quit_hover.png"
                                          : detailsRoot.gameRunning ? "assets/quit.png" : "assets/play.png"
                                iconSize: 14
                                label: detailsRoot.gameLaunching ? "LAUNCHING" : detailsRoot.gameRunning ? "QUIT" : "PLAY"
                                restColor: detailsRoot.gameLaunching ? Theme.positive : Theme.accent
                                hoverColor: detailsRoot.gameLaunching ? Theme.positive
                                          : detailsRoot.gameRunning ? Theme.danger : Theme.accent
                                ringColor: detailsRoot.gameRunning ? Theme.focusRing : Theme.positive
                                labelColor: detailsRoot.gameLaunching || quitHot ? Theme.textPrimary : Theme.textInverse
                                focused: detailsRoot.focusedAction === detailsRoot.actionPlay
                                onTriggered: detailsRoot.playOrQuit()
                            }

                            ActionButton {
                                visible: !detailsRoot.isOwned
                                // Relative so it survives the module's RESOURCE_PREFIX
                                // rather than hardcoding it.
                                iconSource: "assets/steam.png"
                                label: "CHECK ON STEAM"
                                restColor: Theme.steamBg
                                hoverColor: Theme.steamBgPressed
                                ringColor: Theme.steamAccent
                                focused: detailsRoot.focusedAction === detailsRoot.actionPlay
                                onTriggered: Qt.openUrlExternally(detailsRoot.steamUrl)
                            }

                            // Favourite. There is deliberately no dislike counterpart:
                            // asking someone to rate a game they chose to install is
                            // the wrong question, and repeated instant-quits already
                            // tell the recommender the same thing without asking.
                            // Hearting also clears that behavioural penalty, so this
                            // is how you overrule the model when it gets one wrong.
                            ActionButton {
                                heart: true
                                label: detailsRoot.isFavorite ? "FAVORITED" : "FAVORITE"
                                active: detailsRoot.isFavorite
                                activeColor: Theme.favorite
                                focused: detailsRoot.focusedAction === detailsRoot.actionFavorite
                                onTriggered: if (detailsRoot.gameData)
                                    detailsRoot.api.updatePreference(detailsRoot.gameData.name, 1.0)
                            }

                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: detailsRoot.isOwned
                                leftPadding: 18
                                spacing: 32

                                Column {
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 3
                                    Text {
                                        text: "LAST PLAYED"
                                        color: Theme.textMuted
                                        font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                                    }
                                    Text {
                                        text: detailsRoot.gameData && detailsRoot.gameData.lastPlayed
                                              ? detailsRoot.gameData.lastPlayed : "Never"
                                        color: Theme.textSecondary
                                        font.pixelSize: 14
                                    }
                                }

                                // Time actually played -- the session total with idle
                                // taken out -- except for a Steam game showing Steam's
                                // own figure. The caption under YOUR GAME says which.
                                Row {
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 10
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: String.fromCharCode(0xE823)   // clock
                                        font.family: detailsRoot.iconFont
                                        font.pixelSize: 22
                                        color: Theme.textMuted
                                    }
                                    Column {
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 3
                                        Text {
                                            text: "PLAY TIME"
                                            color: Theme.textMuted
                                            font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                                        }
                                        Text {
                                            text: detailsRoot.gameData && detailsRoot.gameData.playtime
                                                  ? detailsRoot.gameData.playtime : "0m"
                                            color: Theme.textSecondary
                                            font.pixelSize: 14
                                        }
                                    }
                                }
                            }
                        }

                        // Steam games hand off to Steam; a local one arms the confirm.
                        ActionButton {
                            id: uninstallButton
                            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                            visible: detailsRoot.isOwned && !detailsRoot.uninstallArmed
                            // The lid lifts on hover or pad focus. The two PNGs
                            // carry dangerRest and dangerIcon baked in.
                            iconSource: emphasized ? "assets/trash_open.png" : "assets/trash.png"
                            label: "UNINSTALL"
                            hoverColor: Theme.dangerBg
                            focused: detailsRoot.focusedAction === detailsRoot.actionUninstall
                            onTriggered: detailsRoot.requestUninstall()
                        }

                        // Saving a game is purely a bookmark: it never feeds or
                        // filters the recommender, so the pick keeps appearing in
                        // Discover with a cart marker.
                        ActionButton {
                            id: wishlistButton
                            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                            visible: !detailsRoot.isOwned
                            glyph: "\u{1F6D2}"
                            label: detailsRoot.isWishlisted ? "WISHLISTED" : "ADD TO WISHLIST"
                            active: detailsRoot.isWishlisted
                            focused: detailsRoot.focusedAction === detailsRoot.actionUninstall
                            onTriggered: {
                                if (!detailsRoot.gameData) return
                                detailsRoot.api.toggleWishlist(detailsRoot.gameData.name)
                                detailsRoot.wishlistRevision++
                            }
                        }
                    }

                    // Confirm step — a local uninstall deletes the game's folder
                    // off the disk, so it is never one click. Steam games skip this.
                    Row {
                        spacing: 14
                        visible: detailsRoot.uninstallArmed

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Delete this game's files from disk?"
                            color: Theme.dangerText
                            font.pixelSize: 16
                        }

                        ActionButton {
                            label: "DELETE"
                            restColor: Theme.dangerRest
                            hoverColor: Theme.danger
                            focused: detailsRoot.armedChoice === detailsRoot.armedDelete
                            onTriggered: detailsRoot.confirmUninstall()
                        }

                        ActionButton {
                            label: "CANCEL"
                            hoverColor: Theme.bgInert
                            labelColor: Theme.textBody
                            focused: detailsRoot.armedChoice === detailsRoot.armedCancel
                            onTriggered: detailsRoot.cancelUninstall()
                        }
                    }
                }

                // ── Body ────────────────────────────────────────────────────
                Column {
                    id: content
                    x: 40
                    width: parent.width - 80
                    topPadding: 20
                    bottomPadding: 50
                    spacing: 40

                    Flow {
                        width: parent.width
                        spacing: 8
                        visible: genreRepeater.count > 0
                        Repeater {
                            id: genreRepeater
                            // The row carries genres pre-joined.
                            model: {
                                const g = detailsRoot.gameData ? detailsRoot.gameData.genres : ""
                                if (!g || g === "Unknown") return []
                                return String(g).split(",")
                                    .map(function (part) { return part.trim() })
                                    .filter(function (part) { return part !== "" })
                            }
                            delegate: Chip {
                                required property string modelData
                                label: modelData
                            }
                        }
                    }

                    // Your game — where it lives, and how its playtime was counted.
                    // Not for an unowned pick, which has neither.
                    Column {
                        id: yourGame
                        width: parent.width
                        spacing: 14
                        visible: detailsRoot.isOwned

                        SectionTitle { text: "GAME PATH" }

                        // A Steam game showing Steam's own figure arrives with
                        // nothing deducted, so say which kind the band's number
                        // is rather than leave the reader to guess.
                        Text {
                            visible: !!detailsRoot.gameData
                                     && detailsRoot.gameData.idleSeconds !== undefined
                                     && detailsRoot.gameData.idleSeconds > 0
                            text: !detailsRoot.gameData ? ""
                                  : detailsRoot.gameData.idleDeducted
                                    ? detailsRoot.gameData.idleTime + " idle, taken out of "
                                      + detailsRoot.gameData.totalPlaytime + " total"
                                    : detailsRoot.gameData.idleTime + " idle observed by Vortex"
                            color: Theme.textMuted
                            font.pixelSize: 14
                        }

                        Rectangle {
                            id: pathBox
                            // Unowned discovery picks carry no install path at all,
                            // so test the key, not just the map.
                            readonly property string installDir:
                                detailsRoot.gameData && detailsRoot.gameData.installDir
                                    ? detailsRoot.gameData.installDir : ""

                            // Local games get the split folder / dropdown segment; the
                            // exe is ours to choose only when we found it ourselves.
                            readonly property bool isLocal: detailsRoot.isLocalGame && pathBox.installDir !== ""

                            // Folder actions belong to the Library tab only; opened from
                            // Favorites, Played, Recommendations or Wishlist the box is a
                            // plain readout.
                            readonly property bool inLibrary: detailsRoot.launchOrigin === "Library"

                            // The exe Play runs, as a bare filename for the menu.
                            readonly property string exeName: {
                                const p = detailsRoot.exeOverride
                                          || (detailsRoot.gameData && detailsRoot.gameData.gamePath) || ""
                                const cut = Math.max(p.lastIndexOf("/"), p.lastIndexOf(String.fromCharCode(92)))
                                return p.substring(cut + 1)
                            }

                            width: Math.min(parent.width, 700); height: 55; color: Theme.bgRaised; radius: 8; border.color: Theme.borderQuiet
                            Text {
                                anchors { left: parent.left; verticalCenter: parent.verticalCenter; leftMargin: 20 }
                                text: pathBox.installDir !== ""
                                      ? pathBox.installDir
                                      : (detailsRoot.isOwned ? "Unknown Path" : "Not installed")
                                color: Theme.textSecondary; font.pixelSize: 15; font.italic: true
                                elide: Text.ElideMiddle
                                // Stop short of the buttons so the path never runs under them.
                                width: pathBox.width - 20
                                       - (localActions.visible ? localActions.width + 12
                                          : openFolderButton.visible ? openFolderButton.width + 24 : 20)
                            }

                            // ── Local: full-height folder | chevron segment ──────────
                            Item {
                                id: localActions
                                visible: pathBox.isLocal && pathBox.inLibrary
                                // Inset by the border so the box's outline stays unbroken.
                                anchors { top: parent.top; bottom: parent.bottom; right: parent.right; margins: 1 }
                                width: 76   // folder 48 + 2px of rules, arrow gets the rest (~26)

                                // Full-height rule that sets the segment off from the path.
                                Rectangle {
                                    anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
                                    width: 1; color: Theme.borderQuiet
                                }

                                Item {
                                    id: folderZone
                                    anchors { left: parent.left; leftMargin: 1; top: parent.top; bottom: parent.bottom }
                                    width: 48

                                    // Darker than the box, so the zone reads as pressed in.
                                    Rectangle {
                                        anchors.fill: parent
                                        color: Theme.bgSunken
                                        opacity: folderZoneArea.containsMouse ? 1 : 0
                                        Behavior on opacity { NumberAnimation { duration: 150 } }
                                    }
                                    Text {
                                        anchors.centerIn: parent
                                        text: String.fromCharCode(0xE8B7)
                                        font.family: detailsRoot.iconFont
                                        font.pixelSize: 16
                                        color: folderZoneArea.containsMouse ? Theme.textPrimary : Theme.textMuted
                                        Behavior on color { ColorAnimation { duration: 150 } }
                                    }
                                    MouseArea {
                                        id: folderZoneArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: if (detailsRoot.api) detailsRoot.api.openInstallFolder(pathBox.installDir)
                                    }
                                    ToolTip.visible: folderZoneArea.containsMouse
                                    ToolTip.delay: 400
                                    ToolTip.text: "Open in Explorer"
                                }

                                // Faint hairline between the two halves of the split button.
                                Rectangle {
                                    anchors { left: folderZone.right; verticalCenter: parent.verticalCenter }
                                    width: 1; height: parent.height * 0.6
                                    color: Theme.borderQuiet
                                }

                                Item {
                                    id: chevronZone
                                    readonly property bool lit: chevronArea.containsMouse || exeMenu.shown
                                    anchors { left: folderZone.right; leftMargin: 1; right: parent.right
                                              top: parent.top; bottom: parent.bottom }

                                    // Rounded on the right to follow the box's corner,
                                    // squared on the left where it meets the hairline.
                                    Item {
                                        anchors.fill: parent
                                        opacity: chevronZone.lit ? 1 : 0
                                        Behavior on opacity { NumberAnimation { duration: 150 } }
                                        Rectangle { anchors.fill: parent; radius: 7; color: Theme.bgSunken }
                                        Rectangle {
                                            anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
                                            width: parent.width / 2; color: Theme.bgSunken
                                        }
                                    }
                                    Text {
                                        anchors.centerIn: parent
                                        text: String.fromCharCode(0xE70D)
                                        font.family: detailsRoot.iconFont
                                        font.pixelSize: 10
                                        color: chevronZone.lit ? Theme.textPrimary : Theme.textMuted
                                        rotation: exeMenu.shown ? 180 : 0
                                        Behavior on color { ColorAnimation { duration: 150 } }
                                        Behavior on rotation { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                                    }
                                    MouseArea {
                                        id: chevronArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: exeMenu.open()
                                    }
                                }
                            }

                            // ── Local: the dropdown ──────────────────────────────────
                            Popup {
                                id: exeMenu
                                // True from the moment it starts opening until it starts
                                // closing, so the chevron turns with the menu, not after.
                                property bool shown: false
                                onAboutToShow: exeMenu.shown = true
                                onAboutToHide: exeMenu.shown = false

                                x: pathBox.width - exeMenu.width
                                y: pathBox.height + 6
                                width: 260
                                padding: 6
                                transformOrigin: Popup.TopRight

                                // Modal so the click that closes it is consumed -- a press
                                // on the chevron then shuts the menu instead of reopening it.
                                modal: true
                                dim: false
                                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

                                enter: Transition {
                                    ParallelAnimation {
                                        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 160; easing.type: Easing.OutCubic }
                                        NumberAnimation { property: "scale"; from: 0.94; to: 1; duration: 160; easing.type: Easing.OutCubic }
                                    }
                                }
                                exit: Transition {
                                    ParallelAnimation {
                                        NumberAnimation { property: "opacity"; to: 0; duration: 110; easing.type: Easing.InCubic }
                                        NumberAnimation { property: "scale"; to: 0.97; duration: 110; easing.type: Easing.InCubic }
                                    }
                                }

                                background: Rectangle {
                                    color: Theme.bgSurface
                                    radius: 10
                                    border.color: Theme.borderControl
                                    border.width: 1
                                }

                                contentItem: Column {
                                    Rectangle {
                                        id: changeExeRow
                                        width: exeMenu.availableWidth
                                        height: 52
                                        radius: 6
                                        color: changeExeArea.containsMouse ? Theme.bgActive : "transparent"
                                        Behavior on color { ColorAnimation { duration: 120 } }

                                        Row {
                                            anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                                            spacing: 12

                                            Text {
                                                anchors.verticalCenter: parent.verticalCenter
                                                text: String.fromCharCode(0xE7AC)   // "Open with"
                                                font.family: detailsRoot.iconFont
                                                font.pixelSize: 16
                                                color: changeExeArea.containsMouse ? Theme.textPrimary : Theme.textSecondary
                                                Behavior on color { ColorAnimation { duration: 120 } }
                                            }
                                            Column {
                                                anchors.verticalCenter: parent.verticalCenter
                                                spacing: 2
                                                Text {
                                                    text: "Change executable"
                                                    color: Theme.textBody
                                                    font.pixelSize: 14
                                                }
                                                Text {
                                                    width: changeExeRow.width - 52
                                                    visible: pathBox.exeName !== ""
                                                    text: pathBox.exeName
                                                    color: Theme.textFaint
                                                    font.pixelSize: 12
                                                    elide: Text.ElideMiddle
                                                }
                                            }
                                        }
                                        MouseArea {
                                            id: changeExeArea
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                exeMenu.close()
                                                exeDialog.open()
                                            }
                                        }
                                    }
                                }
                            }

                            FileDialog {
                                id: exeDialog
                                title: "Choose game executable"
                                nameFilters: ["Executables (*.exe)"]
                                currentFolder: pathBox.installDir !== ""
                                    ? "file:///" + pathBox.installDir.split(String.fromCharCode(92)).join("/")
                                    : ""
                                onAccepted: {
                                    if (!detailsRoot.api) return
                                    const p = detailsRoot.api.setGameExecutable(pathBox.installDir,
                                                                                String(exeDialog.selectedFile))
                                    if (p !== "") detailsRoot.exeOverride = p
                                }
                            }

                            // ── Steam / other: the single folder button ──────────────
                            Rectangle {
                                id: openFolderButton
                                anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: 10 }
                                visible: pathBox.installDir !== "" && !pathBox.isLocal && pathBox.inLibrary
                                width: 36; height: 36; radius: 6
                                color: openFolderArea.containsMouse ? Theme.bgEmphasis : "transparent"
                                border.width: 1
                                border.color: openFolderArea.containsMouse ? Theme.borderStrong : "transparent"

                                Behavior on color { ColorAnimation { duration: 150 } }
                                Behavior on border.color { ColorAnimation { duration: 150 } }

                                Text {
                                    anchors.centerIn: parent
                                    text: String.fromCharCode(0xE8B7)   // folder
                                    font.family: detailsRoot.iconFont
                                    font.pixelSize: 16
                                    color: openFolderArea.containsMouse ? Theme.textPrimary : Theme.textMuted
                                    Behavior on color { ColorAnimation { duration: 150 } }
                                }
                                MouseArea {
                                    id: openFolderArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: if (detailsRoot.api) detailsRoot.api.openInstallFolder(pathBox.installDir)
                                }
                                ToolTip.visible: openFolderArea.containsMouse
                                ToolTip.delay: 400
                                ToolTip.text: "Open in Explorer"
                            }
                        }
                    }

                    // Details
                    Column {
                        width: parent.width
                        spacing: 14
                        visible: factsFlow.facts.length > 0

                        SectionTitle { text: "DETAILS" }

                        Flow {
                            id: factsFlow
                            width: parent.width
                            spacing: 0

                            // Placeholders the bridge fills a missing field with
                            // are left out rather than printed.
                            readonly property var facts: {
                                const row = detailsRoot.gameData
                                const field = function (key) {
                                    const value = row ? row[key] : undefined
                                    return (value && value !== "Unknown" && value !== "N/A") ? String(value) : ""
                                }
                                return [
                                    ["Developer", field("developer")],
                                    ["Genres", field("genres")],
                                    ["Rating", row && row.rating > 0 ? row.rating.toFixed(1) + " / 100" : ""],
                                    ["Time to beat", field("timeToBeat")],
                                    ["Source", field("source")]
                                ].filter(function (fact) { return fact[1] !== "" })
                            }

                            Repeater {
                                model: factsFlow.facts
                                delegate: Row {
                                    id: factRow
                                    required property var modelData
                                    width: factsFlow.width >= 900 ? factsFlow.width / 2 : factsFlow.width
                                    height: Math.max(factValue.implicitHeight, 22) + 14
                                    spacing: 16

                                    Text {
                                        width: 140
                                        text: factRow.modelData[0]
                                        color: Theme.textMuted
                                        font.pixelSize: 14
                                    }
                                    Text {
                                        id: factValue
                                        width: factRow.width - 176
                                        text: factRow.modelData[1]
                                        color: Theme.textBody
                                        font.pixelSize: 14
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── Sticky title ────────────────────────────────────────────────────
        // Slides in once the page's own title has scrolled away, and stays
        // pinned until it scrolls back. Under CLOSE.
        Rectangle {
            id: stickyHeader
            width: parent.width
            height: 84
            y: detailsRoot.titleDocked ? 0 : -12
            opacity: detailsRoot.titleDocked ? 1.0 : 0.0
            visible: opacity > 0
            color: Theme.bgPanel
            radius: 20

            Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
            Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

            // Squares off the bottom corners; only the top follows the popup.
            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: parent.radius
                color: parent.color
            }

            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: 1
                color: Theme.borderQuiet
            }

            // Keeps clicks off the content underneath. Outside the Flickable,
            // so the wheel is passed on by hand.
            MouseArea {
                anchors.fill: parent
                onWheel: (wheel) => detailsRoot.scrollBy(-wheel.angleDelta.y)
            }

            Text {
                anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter
                          leftMargin: 40; rightMargin: 40 + 44 + 20 }
                text: detailsRoot.gameData ? detailsRoot.gameData.name : ""
                color: Theme.textPrimary
                font.pixelSize: 24; font.bold: true
                elide: Text.ElideRight
            }
        }

        // CLOSE — outside the Flickable so it stays put while the page scrolls.
        // Mouse-only; the pad already has Back for this, so it stays off the
        // focus run.
        Rectangle {
            id: closeButton
            readonly property bool emphasized:
                closeArea.containsMouse && detailsRoot.mouseInControl

            anchors { top: parent.top; right: parent.right; margins: 20 }
            width: 44; height: 44; radius: 22
            color: closeButton.emphasized ? "#B32A2A2A" : "#80000000"
            border.width: closeButton.emphasized ? 2 : 1
            border.color: closeButton.emphasized ? Theme.focusRing : Theme.borderControl
            scale: closeButton.emphasized ? 1.1 : 1.0

            Behavior on color { ColorAnimation { duration: 150 } }
            Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutQuart } }

            // White X, decoded at twice the drawn size.
            Image {
                anchors.centerIn: parent
                width: 20; height: 20
                source: "assets/close.png"
                sourceSize.width: 40
                sourceSize.height: 40
                fillMode: Image.PreserveAspectFit
                smooth: true
            }
            MouseArea {
                id: closeArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: detailsRoot.close()
            }
        }
    }
}
