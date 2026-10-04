pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Shapes
import Vortex

// The page a Browse result opens on, and since recommendations moved here, the
// page a Recommendations card opens on too. Browse mode is for reading: the
// description, the facts, screenshots and what players thought, with the three
// ways to keep a game -- played, hearted, wishlisted -- as the only actions.
//
// Recommendation mode (openForGame) is the same page with what GameDetails
// offered on top: Play or Check on Steam, uninstall behind a confirm for local
// games, and the playtime and install path of an owned pick. It opens on the
// recommendation's own row and fills in from IGDB once the bridge has resolved
// the name to an id.
Popup {
    id: browseRoot

    property var api: vortexApi
    property var pad: controller
    readonly property bool padInControl: browseRoot.pad ? browseRoot.pad.padInControl : false
    readonly property bool mouseInControl: !browseRoot.padInControl

    readonly property var details: browseRoot.api && browseRoot.api.browseDetails
                                   ? browseRoot.api.browseDetails : ({})
    readonly property var reviews: browseRoot.api && browseRoot.api.browseReviews
                                   ? browseRoot.api.browseReviews : ({})
    readonly property bool loading: !!browseRoot.api && browseRoot.api.browseDetailsLoading
    readonly property bool owned: !!browseRoot.details.ownedName

    // ── Recommendation mode ─────────────────────────────────────────────────
    property bool fromRecommendation: false
    // The card's own spelling, which is what the library, hearts, the wishlist
    // and the played ledger are keyed on -- IGDB's name can differ from it.
    property string gameName: ""
    // Which surface opened the page, so a launch can be attributed to it.
    property string launchOrigin: "Recommendations"
    // The library or recommendation row behind the page. Kept live by the
    // Connections below, the way GameDetails keeps its gameData.
    property var gameRow: null

    readonly property bool isOwned: browseRoot.fromRecommendation
        ? (!!browseRoot.gameRow && browseRoot.gameRow.matched !== false)
        : browseRoot.owned
    readonly property bool isLocalGame:
        browseRoot.fromRecommendation && !!browseRoot.gameRow
        && browseRoot.gameRow.source === "Local"

    // Hearts and the played ledger are keyed on the library's spelling when
    // the game is owned; IGDB's own name otherwise.
    readonly property string actionName: browseRoot.fromRecommendation
        ? browseRoot.gameName
        : (browseRoot.details.ownedName || browseRoot.details.name || "")

    readonly property int steamAppId:
        browseRoot.gameRow && browseRoot.gameRow.steamAppId > 0 ? browseRoot.gameRow.steamAppId
        : (browseRoot.details.steamAppId > 0 ? browseRoot.details.steamAppId : 0)

    // IGDB's Steam links are good but not complete, and a game may simply not
    // be on Steam. A store search is a useful degradation.
    readonly property string steamUrl:
        browseRoot.steamAppId > 0
            ? "https://store.steampowered.com/app/" + browseRoot.steamAppId + "/"
            : "https://store.steampowered.com/search/?term=" + encodeURIComponent(browseRoot.actionName)

    // isFavorite(), isWishlisted() and playedState() are plain methods, so
    // these counters are what make the buttons re-read them.
    property int favoriteRevision: 0
    property int wishlistRevision: 0
    property int playedRevision: 0

    readonly property bool isFavorite:
        browseRoot.favoriteRevision >= 0 && !!browseRoot.api && browseRoot.actionName !== ""
        && browseRoot.api.isFavorite(browseRoot.actionName)
    readonly property bool isWishlisted:
        browseRoot.wishlistRevision >= 0 && !!browseRoot.api && browseRoot.actionName !== ""
        && browseRoot.api.isWishlisted(browseRoot.actionName)
    readonly property string playedState:
        (browseRoot.playedRevision >= 0 && !!browseRoot.api && browseRoot.actionName !== "")
            ? browseRoot.api.playedState(browseRoot.actionName) : "none"

    Connections {
        target: browseRoot.api
        function onFavoritesChanged()   { browseRoot.favoriteRevision++; browseRoot.refreshGameRow() }
        function onWishlistChanged()    { browseRoot.wishlistRevision++; browseRoot.refreshGameRow() }
        function onPlayedGamesChanged() { browseRoot.playedRevision++; browseRoot.refreshGameRow() }
        function onGameListChanged()    { browseRoot.refreshGameRow() }
        function onRecommendationListChanged() { browseRoot.refreshGameRow() }
    }

    function findIn(list) {
        if (!list) return null
        for (let i = 0; i < list.length; i++) {
            if (list[i].name === browseRoot.gameName) return list[i]
        }
        return null
    }

    // The library first, so an owned game always opens as the owned copy;
    // then every list an unowned pick can be found in.
    function findGameRow() {
        if (!browseRoot.api || browseRoot.gameName === "") return null
        return browseRoot.findIn(browseRoot.api.gameList)
            || browseRoot.findIn(browseRoot.api.recommendationList)
            || browseRoot.findIn(browseRoot.api.wishlistGames)
            || browseRoot.findIn(browseRoot.api.favoriteGames)
            || browseRoot.findIn(browseRoot.api.playedGames)
    }

    // A refresh must never blank the page under the user: un-hearting an
    // unowned favourite drops it out of the only list that held it, but it is
    // still the game on screen. Same rule as GameDetails.refreshGameData().
    function refreshGameRow() {
        if (!browseRoot.fromRecommendation) return
        const found = browseRoot.findGameRow()
        if (found) browseRoot.gameRow = found
    }

    // ── Controller ──────────────────────────────────────────────────────────
    // The action row as the pad walks it. Browse keeps its three; a
    // recommendation leads with Play or Check on Steam and ends on Uninstall
    // when owned, Wishlist when not.
    readonly property var actions: !browseRoot.fromRecommendation
        ? ["played", "favorite", "wishlist"]
        : browseRoot.isOwned ? ["play", "played", "favorite", "uninstall"]
                             : ["steam", "played", "favorite", "wishlist"]
    property int focusedAction: -1
    readonly property string focusedKey:
        browseRoot.focusedAction >= 0 && browseRoot.focusedAction < browseRoot.actions.length
            ? browseRoot.actions[browseRoot.focusedAction] : ""

    // Index into details.screenshots of the one shown full size, or -1.
    property int lightboxIndex: -1
    readonly property var screenshots: browseRoot.details.screenshots || []

    // ── Uninstall confirm step ──────────────────────────────────────────────
    // Steam games hand off to Steam, which prompts on its own. A local game is
    // deleted off the disk by us, with nothing to undo it, so that one gets a
    // confirm row first. armedChoice starts on CANCEL so a stray pad press
    // deletes nothing.
    readonly property int armedDelete: 0
    readonly property int armedCancel: 1
    property bool uninstallArmed: false
    property int armedChoice: -1

    function openFor(item) {
        if (!item || !browseRoot.api) return
        browseRoot.fromRecommendation = false
        browseRoot.gameName = ""
        browseRoot.gameRow = null
        browseRoot.cancelUninstall()
        browseRoot.lightboxIndex = -1
        browseRoot.storyExpanded = false
        browseRoot.api.loadBrowseDetails(item.igdbId)
        browseRoot.open()
        body.contentY = 0
    }

    // A Recommendations card: by name, since the pick has no IGDB id of its own.
    function openForGame(name, origin) {
        if (!name || !browseRoot.api) return
        browseRoot.cancelUninstall()
        browseRoot.lightboxIndex = -1
        browseRoot.storyExpanded = false
        browseRoot.fromRecommendation = true
        browseRoot.launchOrigin = origin || "Recommendations"
        browseRoot.gameName = name
        browseRoot.gameRow = browseRoot.findGameRow()
        browseRoot.api.loadBrowseDetailsForGame(name)
        const row = browseRoot.gameRow
        if (row) {
            browseRoot.api.logGameClick(row.name, row.genres, row.tags)
            // Unowned picks fetch their hero and logo, and a name-only row its
            // developer and genres, only when a page asks -- this is that page.
            if (!browseRoot.isOwned) {
                browseRoot.api.ensureArtwork(row.name)
                browseRoot.api.ensureMetadata(row.name)
            }
        }
        browseRoot.open()
        body.contentY = 0
    }

    // Where this game's launch is, from the bridge; drives the Play button.
    readonly property bool gameLaunching:
        !!browseRoot.api && browseRoot.gameName !== "" && browseRoot.api.launchingGames.indexOf(browseRoot.gameName) >= 0
    readonly property bool gameRunning:
        !!browseRoot.api && browseRoot.gameName !== "" && browseRoot.api.runningGames.indexOf(browseRoot.gameName) >= 0

    // Play does nothing while a launch is under way, and quits once it is up.
    function play() {
        if (browseRoot.gameName === "" || !browseRoot.api) return
        if (browseRoot.gameRunning)
            browseRoot.api.quitGame(browseRoot.gameName)
        else if (!browseRoot.gameLaunching)
            browseRoot.api.launchGameFrom(browseRoot.gameName, browseRoot.launchOrigin)
    }
    function openSteam() {
        Qt.openUrlExternally(browseRoot.steamUrl)
    }
    function togglePlayed() {
        if (browseRoot.actionName === "" || browseRoot.playedState === "tracked") return
        browseRoot.api.togglePlayed(browseRoot.actionName)
    }
    function toggleFavorite() {
        if (browseRoot.actionName === "") return
        browseRoot.api.updatePreference(browseRoot.actionName, 1.0)
    }
    function toggleWishlist() {
        if (browseRoot.actionName === "" || browseRoot.isOwned) return
        browseRoot.api.toggleWishlist(browseRoot.actionName)
    }

    function requestUninstall() {
        if (!browseRoot.gameRow || !browseRoot.api) return
        if (!browseRoot.isLocalGame) {
            browseRoot.api.uninstallGame(browseRoot.gameRow.name)
            browseRoot.close()
            return
        }
        browseRoot.armedChoice = browseRoot.focusedAction >= 0 ? browseRoot.armedCancel : -1
        browseRoot.uninstallArmed = true
    }
    function confirmUninstall() {
        if (!browseRoot.gameRow || !browseRoot.api) return
        const name = browseRoot.gameRow.name
        browseRoot.cancelUninstall()
        browseRoot.api.uninstallGame(name)
        browseRoot.close()
    }
    function cancelUninstall() {
        browseRoot.uninstallArmed = false
        browseRoot.armedChoice = -1
    }

    function scrollBy(delta) {
        const limit = Math.max(0, body.contentHeight - body.height)
        body.contentY = Math.max(0, Math.min(limit, body.contentY + delta))
    }

    function stepLightbox(step) {
        const n = browseRoot.screenshots.length
        if (n === 0) return
        browseRoot.lightboxIndex = (browseRoot.lightboxIndex + step + n) % n
    }

    function navigate(direction) {
        if (browseRoot.uninstallArmed) {
            // Armed, the pad only picks between DELETE and CANCEL.
            if (direction !== "left" && direction !== "right") return
            browseRoot.armedChoice = browseRoot.armedChoice === browseRoot.armedCancel
                                     ? browseRoot.armedDelete : browseRoot.armedCancel
            return
        }
        if (browseRoot.lightboxIndex >= 0) {
            if (direction === "left") browseRoot.stepLightbox(-1)
            else if (direction === "right") browseRoot.stepLightbox(1)
            return
        }
        if (direction === "up" || direction === "down") {
            browseRoot.scrollBy(direction === "down" ? 240 : -240)
            return
        }
        if (browseRoot.focusedAction < 0) {
            browseRoot.focusedAction = 0
            return
        }
        const count = browseRoot.actions.length
        const step = direction === "right" ? 1 : -1
        browseRoot.focusedAction = (browseRoot.focusedAction + step + count) % count
    }

    function activateFocusedAction() {
        if (browseRoot.uninstallArmed) {
            if (browseRoot.armedChoice === browseRoot.armedDelete) browseRoot.confirmUninstall()
            else if (browseRoot.armedChoice === browseRoot.armedCancel) browseRoot.cancelUninstall()
            else browseRoot.armedChoice = browseRoot.armedCancel
            return
        }
        if (browseRoot.lightboxIndex >= 0) {
            browseRoot.stepLightbox(1)
            return
        }
        switch (browseRoot.focusedKey) {
        case "play":      browseRoot.play(); break
        case "steam":     browseRoot.openSteam(); break
        case "played":    browseRoot.togglePlayed(); break
        case "favorite":  browseRoot.toggleFavorite(); break
        case "wishlist":  browseRoot.toggleWishlist(); break
        case "uninstall": browseRoot.requestUninstall(); break
        default: browseRoot.focusedAction = 0
        }
    }

    // One level back: the uninstall confirm, then the lightbox, then the page.
    // True means handled.
    function handleBack() {
        if (browseRoot.uninstallArmed) {
            browseRoot.cancelUninstall()
            return true
        }
        if (browseRoot.lightboxIndex < 0)
            return false
        browseRoot.lightboxIndex = -1
        return true
    }

    // Steam review text is BBCode; drop the tags rather than show them.
    function plainReview(text) {
        return String(text || "")
            .replace(/\[\/?[a-z0-9*]+(=[^\]]*)?\]/gi, "")
            .replace(/\n{3,}/g, "\n\n")
            .trim()
    }

    function compact(n) {
        if (n >= 1000000) return (n / 1000000).toFixed(1) + "M"
        if (n >= 1000) return (n / 1000).toFixed(1) + "k"
        return String(n)
    }

    function scoreLine(score, count, noun) {
        if (!score || score <= 0) return ""
        return Math.round(score) + " / 100" + (count > 0 ? "  (" + browseRoot.compact(count) + " " + noun + ")" : "")
    }

    property bool storyExpanded: false

    // True once the page's own title has scrolled up under the sticky header,
    // which then carries it. One line both ways, so scrolling back up to the
    // title hands it back the moment any of it would show again.
    property bool titleDocked: false

    function updateTitleDock() {
        const info = browseRoot.fromRecommendation ? recInfo : browseInfo
        const title = info.titleItem
        const bottom = title.mapToItem(page, 0, title.height).y
        browseRoot.titleDocked = bottom - body.contentY < stickyHeader.height
    }

    onFromRecommendationChanged: browseRoot.updateTitleDock()

    onClosed: {
        browseRoot.focusedAction = -1
        browseRoot.lightboxIndex = -1
        browseRoot.cancelUninstall()
    }

    // Esc goes one level back rather than straight out, so it is handled here
    // instead of through closePolicy.
    Shortcut {
        sequence: "Esc"
        enabled: browseRoot.visible
        onActivated: if (!browseRoot.handleBack()) browseRoot.close()
    }
    Shortcut {
        sequence: "Left"
        enabled: browseRoot.visible && browseRoot.lightboxIndex >= 0
        onActivated: browseRoot.stepLightbox(-1)
    }
    Shortcut {
        sequence: "Right"
        enabled: browseRoot.visible && browseRoot.lightboxIndex >= 0
        onActivated: browseRoot.stepLightbox(1)
    }

    width: parent.width * 0.9; height: parent.height * 0.9
    x: (parent.width - width) / 2; y: (parent.height - height) / 2
    modal: true; focus: true
    padding: 0
    closePolicy: Popup.CloseOnPressOutside

    background: Rectangle {
        color: Theme.bgPanel
        radius: 20
        border.color: Theme.borderQuiet
        border.width: 1
    }

    // ── Reusable bits ───────────────────────────────────────────────────────
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
        property bool active: false
        property bool available: true
        property bool focused: false
        property color activeColor: Theme.positiveDim
        // The recommendation-only buttons -- Play, Check on Steam, Uninstall,
        // the delete confirm -- keep GameDetails' colours through these.
        property color restColor: Theme.bgActive
        property color hoverColor: Theme.bgEmphasis
        property color ringColor: Theme.focusRing
        property color labelColor: Theme.textPrimary
        property color glyphColor: action.labelColor
        property string iconSource: ""
        property int iconSize: 22
        signal triggered()

        readonly property bool hovered: actionArea.containsMouse
        readonly property bool emphasized:
            action.available && ((action.focused && browseRoot.padInControl)
                                 || (actionArea.containsMouse && browseRoot.mouseInControl))

        height: 50; radius: 8
        width: actionRow.implicitWidth + 36
        opacity: action.available ? 1.0 : 0.55
        color: action.active ? action.activeColor
                             : (action.emphasized ? action.hoverColor : action.restColor)
        border.width: action.emphasized || (action.focused && browseRoot.padInControl) ? 3 : 0
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
            cursorShape: action.available ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: if (action.available) action.triggered()
        }
    }

    // The title, facts line, genres and action row. Browse sets it beside the
    // cover inside its banner; a recommendation sets it under the hero, where
    // GameDetails keeps its buttons. One definition so the two cannot drift.
    component BannerInfo: Column {
        // Read by updateTitleDock() to tell when the title has scrolled away.
        readonly property Item titleItem: titleText
        spacing: 14

        Row {
            spacing: 10
            visible: browseRoot.isOwned
            Rectangle {
                width: libraryChip.implicitWidth + 18; height: 24; radius: 12
                color: Theme.overlayPositive
                Text {
                    id: libraryChip
                    anchors.centerIn: parent
                    text: "IN LIBRARY"
                    color: Theme.textPrimary
                    font.pixelSize: 11; font.bold: true; font.letterSpacing: 1
                }
            }
        }

        Text {
            id: titleText
            width: parent.width
            // The card's spelling for a recommendation, so
            // the page is titled as the card that opened it.
            text: browseRoot.fromRecommendation
                  ? browseRoot.gameName : (browseRoot.details.name || "")
            color: Theme.textPrimary
            font.pixelSize: 40; font.bold: true
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }

        Text {
            width: parent.width
            text: [browseRoot.details.year,
                   browseRoot.details.developers,
                   browseRoot.details.publishers !== browseRoot.details.developers
                       ? browseRoot.details.publishers : ""]
                      .filter(function (part) { return !!part }).join("  ·  ")
            color: Theme.textSecondary
            font.pixelSize: 16
            elide: Text.ElideRight
        }

        Flow {
            width: parent.width
            spacing: 8
            Repeater {
                model: browseRoot.details.genres || []
                delegate: Chip {
                    required property string modelData
                    label: modelData
                }
            }
        }

        // The three ways to keep a game, led by Play or
        // Check on Steam for a recommendation. A Flow, so
        // the four-wide recommendation row wraps on a
        // narrow window instead of running off the page.
        Flow {
            width: parent.width
            spacing: 14
            topPadding: 6
            visible: !browseRoot.uninstallArmed

            ActionButton {
                visible: browseRoot.actions.indexOf("play") >= 0
                // PLAY, then LAUNCHING on green until the game is up, then QUIT,
                // which turns red on hover with the icon and label flipped to white.
                readonly property bool quitHot: browseRoot.gameRunning && emphasized
                iconSource: browseRoot.gameLaunching ? ""
                          : quitHot ? "assets/quit_hover.png"
                          : browseRoot.gameRunning ? "assets/quit.png" : "assets/play.png"
                iconSize: 14
                label: browseRoot.gameLaunching ? "LAUNCHING" : browseRoot.gameRunning ? "QUIT" : "PLAY"
                restColor: browseRoot.gameLaunching ? Theme.positive : Theme.accent
                hoverColor: browseRoot.gameLaunching ? Theme.positive
                          : browseRoot.gameRunning ? Theme.danger : Theme.accent
                ringColor: browseRoot.gameRunning ? Theme.focusRing : Theme.positive
                labelColor: browseRoot.gameLaunching || quitHot ? Theme.textPrimary : Theme.textInverse
                focused: browseRoot.focusedKey === "play"
                onTriggered: browseRoot.play()
            }

            ActionButton {
                visible: browseRoot.actions.indexOf("steam") >= 0
                // Relative so it survives the module's
                // RESOURCE_PREFIX, as GameDetails does.
                iconSource: "assets/steam.png"
                label: "CHECK ON STEAM"
                restColor: Theme.steamBg
                hoverColor: Theme.steamBgPressed
                ringColor: Theme.steamAccent
                focused: browseRoot.focusedKey === "steam"
                onTriggered: browseRoot.openSteam()
            }

            ActionButton {
                readonly property bool tracked: browseRoot.playedState === "tracked"
                readonly property bool manual: browseRoot.playedState === "manual"
                glyph: (tracked || manual) ? "✓" : "+"
                label: tracked ? "PLAYED"
                     : manual ? (emphasized ? "REMOVE FROM PLAYED" : "PLAYED")
                     : "ADD TO PLAYED"
                active: tracked || manual
                // Recorded playtime is history; only a mark
                // made by hand can be taken back.
                available: !tracked && browseRoot.actionName !== ""
                focused: browseRoot.focusedKey === "played"
                onTriggered: browseRoot.togglePlayed()

                ToolTip.visible: tracked && hovered
                ToolTip.delay: 400
                ToolTip.text: "Vortex has recorded playtime for this game"
            }

            ActionButton {
                glyph: "♥"
                label: browseRoot.isFavorite ? "FAVORITED" : "FAVORITE"
                active: browseRoot.isFavorite
                activeColor: Theme.favorite
                available: browseRoot.actionName !== ""
                focused: browseRoot.focusedKey === "favorite"
                onTriggered: browseRoot.toggleFavorite()
            }

            ActionButton {
                visible: browseRoot.actions.indexOf("wishlist") >= 0
                glyph: "\u{1F6D2}"
                label: browseRoot.isOwned ? "OWNED"
                     : browseRoot.isWishlisted ? "WISHLISTED" : "ADD TO WISHLIST"
                active: browseRoot.isWishlisted
                // A game you own is not one you want to buy.
                available: !browseRoot.isOwned && browseRoot.actionName !== ""
                focused: browseRoot.focusedKey === "wishlist"
                onTriggered: browseRoot.toggleWishlist()
            }

            // Owned recommendations only. Steam games hand
            // off to Steam; a local one arms the confirm.
            ActionButton {
                visible: browseRoot.actions.indexOf("uninstall") >= 0
                // The lid lifts on hover or pad focus. The two PNGs
                // carry dangerRest and dangerIcon baked in.
                iconSource: emphasized ? "assets/trash_open.png" : "assets/trash.png"
                label: "UNINSTALL"
                hoverColor: Theme.dangerBg
                focused: browseRoot.focusedKey === "uninstall"
                onTriggered: browseRoot.requestUninstall()
            }
        }

        // Confirm step -- a local uninstall deletes the
        // game's folder off the disk, so it is never one
        // click. Steam games skip this entirely.
        Row {
            spacing: 14
            topPadding: 6
            visible: browseRoot.uninstallArmed

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
                focused: browseRoot.armedChoice === browseRoot.armedDelete
                onTriggered: browseRoot.confirmUninstall()
            }

            ActionButton {
                label: "CANCEL"
                hoverColor: Theme.bgInert
                labelColor: Theme.textBody
                focused: browseRoot.armedChoice === browseRoot.armedCancel
                onTriggered: browseRoot.cancelUninstall()
            }
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

            onContentYChanged: browseRoot.updateTitleDock()

            Column {
                id: page
                width: body.width
                spacing: 0

                // Details landing while scrolled can move the title.
                onHeightChanged: browseRoot.updateTitleDock()

                // ── Banner (Browse) ─────────────────────────────────────────
                Item {
                    id: banner
                    width: parent.width
                    height: 420
                    clip: true
                    visible: !browseRoot.fromRecommendation

                    readonly property string heroSource:
                        browseRoot.details.heroUrl || browseRoot.details.coverUrl || ""

                    Image {
                        id: heroImage
                        anchors.fill: parent
                        visible: false
                        asynchronous: true
                        fillMode: Image.PreserveAspectCrop
                        source: banner.heroSource
                    }

                    // Real key art stays sharp; a cover standing in is only
                    // there for colour, so it is blurred like GameDetails does.
                    MultiEffect {
                        anchors.fill: parent
                        source: heroImage
                        blurEnabled: !browseRoot.details.heroUrl
                        blur: 1.0
                        blurMax: 48
                        opacity: browseRoot.details.heroUrl ? 0.45 : 0.55
                    }

                    Rectangle {
                        anchors.fill: parent
                        gradient: Gradient {
                            GradientStop { position: 0.35; color: "transparent" }
                            GradientStop { position: 1.0; color: Theme.bgPanel }
                        }
                    }

                    Row {
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom
                                  leftMargin: 40; rightMargin: 40; bottomMargin: 28 }
                        spacing: 32

                        Rectangle {
                            width: 210; height: 280; radius: 12
                            color: Theme.bgRaised
                            border.color: Theme.borderControl; border.width: 2
                            clip: true

                            Image {
                                id: coverImage
                                anchors.fill: parent
                                anchors.margins: 2
                                asynchronous: true
                                fillMode: Image.PreserveAspectCrop
                                source: browseRoot.details.coverUrl || ""
                            }
                            Text {
                                anchors.centerIn: parent
                                visible: coverImage.status !== Image.Ready
                                text: coverImage.status === Image.Loading ? "" : "NO ART"
                                color: Theme.textGhost; font.bold: true
                            }
                        }

                        BannerInfo {
                            id: browseInfo
                            anchors.bottom: parent.bottom
                            width: parent.width - 242
                        }
                    }
                }

                // ── Banner (Recommendation) ─────────────────────────────────
                // GameDetails' hero banner, so a pick looks the way its library
                // page does: art across the top half, the logo centred on it.
                //
                // Whether heroPath is genuinely wide art is decided from the
                // loaded image, not from where it came from: Steam's
                // library_hero is a banner, but IGDB's t_720p and the cover the
                // bridge aliases in when neither exists are portrait, and
                // cropping a portrait into this shape is an ugly upscale. So the
                // shape of what loaded picks the treatment, as in GameDetails.
                Rectangle {
                    id: recBanner
                    width: parent.width
                    height: Math.round(body.height * 0.5)
                    visible: browseRoot.fromRecommendation
                    color: Theme.bgRaised
                    radius: 20
                    layer.enabled: true
                    clip: true

                    // The row's own art, as GameDetails reads it. IGDB's only
                    // stands in when the row has none at all.
                    readonly property var row: browseRoot.gameRow
                    readonly property string heroSource:
                        (recBanner.row && recBanner.row.heroPath) || browseRoot.details.heroUrl || ""
                    readonly property string logoSource:
                        (recBanner.row && recBanner.row.logoPath) || ""
                    readonly property string coverSource:
                        (recBanner.row && recBanner.row.coverPath) || browseRoot.details.coverUrl || ""

                    // 16:10 is the loosest thing anyone ships as a banner and
                    // the tightest portrait cover is 3:4, so 1.6 separates the
                    // two. False until the image reports a size, so the banner
                    // starts blurred -- un-blurring late is invisible.
                    readonly property bool heroIsWide:
                        recHeroBackdrop.status === Image.Ready
                        && recHeroBackdrop.implicitWidth > recHeroBackdrop.implicitHeight * 1.6

                    // Drawn through the MultiEffect below, never directly.
                    Image {
                        id: recHeroBackdrop
                        anchors.fill: parent
                        visible: false
                        asynchronous: true
                        fillMode: Image.PreserveAspectCrop
                        source: recBanner.heroSource
                    }

                    // Wide art stays sharp; portrait art is blurred and only
                    // there for colour.
                    MultiEffect {
                        anchors.fill: parent
                        source: recHeroBackdrop
                        blurEnabled: !recBanner.heroIsWide
                        blur: 1.0
                        blurMax: 48
                        opacity: recBanner.heroIsWide ? 0.4 : 0.55
                        Behavior on opacity { NumberAnimation { duration: 200 } }
                    }

                    // Centre art, in order of preference: the logo when there
                    // is one, else the sharp portrait over its own blur, else
                    // nothing -- real wide art reads perfectly well on its own.
                    Image {
                        anchors.centerIn: parent
                        width: parent.width * 0.4; height: parent.height * 0.4
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        visible: recBanner.logoSource !== ""
                        source: recBanner.logoSource
                    }

                    Image {
                        anchors.centerIn: parent
                        height: parent.height * 0.8
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        visible: recBanner.logoSource === "" && !recBanner.heroIsWide
                        // heroSource first: when it is portrait it is IGDB's
                        // 540x720, which downscales into this slot where the
                        // 264x352 cover would have to be stretched up.
                        source: !visible ? ""
                              : (recBanner.heroSource !== "" ? recBanner.heroSource
                                                             : recBanner.coverSource)
                    }
                }

                BannerInfo {
                    id: recInfo
                    visible: browseRoot.fromRecommendation
                    x: 40
                    width: parent.width - 80
                    topPadding: 24
                }

                // ── Body ────────────────────────────────────────────────────
                Column {
                    id: content
                    x: 40
                    width: parent.width - 80
                    topPadding: 20
                    bottomPadding: 50
                    spacing: 40

                    Text {
                        visible: browseRoot.loading
                        text: "LOADING…"
                        color: Theme.textGhost
                        font.pixelSize: 14; font.bold: true; font.letterSpacing: 2
                    }

                    // Not for a recommendation: its own row is still on the
                    // page, so IGDB failing loses detail rather than the game.
                    Text {
                        visible: !browseRoot.loading && !!browseRoot.details.error
                                 && !browseRoot.fromRecommendation
                        text: browseRoot.details.error || ""
                        color: Theme.dangerText
                        font.pixelSize: 15
                    }

                    // Your game -- an owned recommendation's playtime and where
                    // it lives, as GameDetails showed them. The path is a plain
                    // readout: folder actions belong to the Library tab.
                    Column {
                        id: yourGame
                        readonly property var row: browseRoot.gameRow
                        readonly property string installDir:
                            yourGame.row && yourGame.row.installDir ? yourGame.row.installDir : ""

                        width: parent.width
                        spacing: 14
                        visible: browseRoot.fromRecommendation && browseRoot.isOwned && !!yourGame.row

                        SectionTitle { text: "YOUR GAME" }

                        Row {
                            spacing: 60

                            // The headline is time actually played -- the session
                            // total with idle taken out -- except for a Steam game
                            // showing Steam's own figure, which arrives with nothing
                            // deducted. The caption says which of the two it is.
                            Column {
                                spacing: 5
                                Text { text: "Playtime"; color: Theme.textMuted; font.pixelSize: 14 }
                                Text {
                                    text: yourGame.row && yourGame.row.playtime ? yourGame.row.playtime : "0m"
                                    color: Theme.textPrimary
                                    font.pixelSize: 32; font.bold: true
                                }
                                Text {
                                    visible: !!yourGame.row && yourGame.row.idleSeconds > 0
                                    text: !yourGame.row ? ""
                                          : yourGame.row.idleDeducted
                                            ? yourGame.row.idleTime + " idle, taken out of "
                                              + yourGame.row.totalPlaytime + " total"
                                            : yourGame.row.idleTime + " idle observed by Vortex"
                                    color: Theme.textMuted
                                    font.pixelSize: 14
                                }
                            }

                            Column {
                                spacing: 5
                                Text { text: "Last played"; color: Theme.textMuted; font.pixelSize: 14 }
                                Text {
                                    text: yourGame.row && yourGame.row.lastPlayed ? yourGame.row.lastPlayed : "Never"
                                    color: Theme.textSecondary
                                    font.pixelSize: 20
                                }
                            }
                        }

                        Rectangle {
                            width: Math.min(parent.width, 700); height: 55
                            color: Theme.bgRaised
                            radius: 8
                            border.color: Theme.borderQuiet
                            Text {
                                anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter
                                          leftMargin: 20; rightMargin: 20 }
                                text: yourGame.installDir !== "" ? yourGame.installDir : "Unknown Path"
                                color: Theme.textSecondary
                                font.pixelSize: 15; font.italic: true
                                elide: Text.ElideMiddle
                            }
                        }
                    }

                    // About
                    Column {
                        width: parent.width
                        spacing: 14
                        visible: !browseRoot.loading && (!!browseRoot.details.summary || !!browseRoot.details.storyline)

                        SectionTitle { text: "ABOUT" }

                        Text {
                            width: Math.min(parent.width, 1100)
                            text: browseRoot.details.summary || ""
                            visible: text !== ""
                            color: Theme.textBody
                            font.pixelSize: 16
                            lineHeight: 1.35
                            wrapMode: Text.WordWrap
                            textFormat: Text.PlainText
                        }

                        Text {
                            width: Math.min(parent.width, 1100)
                            visible: browseRoot.storyExpanded && text !== ""
                            text: browseRoot.details.storyline || ""
                            color: Theme.textSecondary
                            font.pixelSize: 15
                            lineHeight: 1.35
                            wrapMode: Text.WordWrap
                            textFormat: Text.PlainText
                        }

                        Text {
                            id: storyToggle
                            visible: !!browseRoot.details.storyline
                            text: browseRoot.storyExpanded ? "Hide storyline" : "Read the storyline"
                            color: storyToggleArea.containsMouse ? Theme.linkHover : Theme.link
                            font.pixelSize: 14; font.bold: true
                            MouseArea {
                                id: storyToggleArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: browseRoot.storyExpanded = !browseRoot.storyExpanded
                            }
                        }
                    }

                    // Details
                    Column {
                        width: parent.width
                        spacing: 14
                        visible: !browseRoot.loading && factsFlow.facts.length > 0

                        SectionTitle { text: "DETAILS" }

                        Flow {
                            id: factsFlow
                            width: parent.width
                            spacing: 0

                            readonly property var facts: {
                                const d = browseRoot.details
                                // The search row that stands in while loading carries
                                // genres pre-joined; the full answer carries lists.
                                const join = function (list) {
                                    return typeof list === "string" ? list : (list || []).join(", ")
                                }
                                const ttb = [d.ttbHastily ? "Rushed " + d.ttbHastily : "",
                                             d.ttbNormally ? "Normal " + d.ttbNormally : "",
                                             d.ttbCompletely ? "Completionist " + d.ttbCompletely : ""]
                                                .filter(function (part) { return !!part }).join("  ·  ")
                                // A recommendation's own row answers wherever IGDB
                                // has nothing, so a game IGDB never resolved still
                                // shows everything GameDetails used to.
                                const row = browseRoot.fromRecommendation ? browseRoot.gameRow : null
                                const fromRow = function (key) {
                                    const value = row ? row[key] : undefined
                                    return (value && value !== "Unknown" && value !== "N/A") ? String(value) : ""
                                }
                                const rating = d.totalRating > 0 ? d.totalRating
                                             : (row && row.rating > 0 ? row.rating : 0)
                                return [
                                    ["Release date", d.releaseDate || fromRow("releasedAt")],
                                    ["Developer", d.developers || fromRow("developer")],
                                    ["Publisher", d.publishers || ""],
                                    ["Genres", join(d.genres) || fromRow("genres")],
                                    ["Themes", join(d.themes)],
                                    ["Game modes", join(d.modes)],
                                    ["Perspective", join(d.perspectives)],
                                    ["Platforms", join(d.platforms)],
                                    ["Time to beat", ttb || fromRow("timeToBeat")],
                                    ["Rating", row && rating > 0 ? rating.toFixed(1) + " / 100" : ""],
                                    ["Critic score", browseRoot.scoreLine(d.criticScore, d.criticCount, "critics")],
                                    ["User score", browseRoot.scoreLine(d.userScore, d.userCount, "ratings")],
                                    ["Source", fromRow("source")]
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

                    // Screenshots
                    Column {
                        width: parent.width
                        spacing: 14
                        visible: !browseRoot.loading && browseRoot.screenshots.length > 0

                        SectionTitle { text: "SCREENSHOTS" }

                        ListView {
                            id: shotList
                            // Padded out past the column on every side so a
                            // hovered thumbnail can grow without being clipped,
                            // while the first one still lines up with the title.
                            readonly property int popRoom: 10
                            x: -popRoom
                            width: parent.width + 2 * popRoom
                            height: 180 + 2 * popRoom
                            leftMargin: popRoom
                            rightMargin: popRoom
                            orientation: ListView.Horizontal
                            spacing: 14
                            clip: true
                            model: browseRoot.screenshots
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

                            delegate: Item {
                                id: shot
                                required property var modelData
                                required property int index
                                readonly property bool hovered: shotArea.containsMouse && browseRoot.mouseInControl

                                width: 320; height: shotList.height
                                z: shot.hovered ? 1 : 0

                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 320; height: 180; radius: 10
                                    color: Theme.bgRaised
                                    border.width: 2
                                    border.color: Theme.borderMuted
                                    clip: true

                                    scale: shot.hovered ? 1.05 : 1.0
                                    Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutQuart } }

                                    Image {
                                        anchors.fill: parent
                                        anchors.margins: 2
                                        asynchronous: true
                                        fillMode: Image.PreserveAspectCrop
                                        source: shot.modelData.thumb
                                    }
                                    MouseArea {
                                        id: shotArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: browseRoot.lightboxIndex = shot.index
                                    }
                                }
                            }
                        }
                    }

                    // Reviews
                    Column {
                        width: parent.width
                        spacing: 14
                        // A recommendation can still have Steam reviews when
                        // IGDB failed: the bridge fetches them off the row's app.
                        visible: !browseRoot.loading
                                 && (!browseRoot.details.error || browseRoot.fromRecommendation)

                        SectionTitle { text: "REVIEWS" }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            font.pixelSize: 16
                            color: Theme.textBody
                            text: {
                                const d = browseRoot.details
                                const r = browseRoot.reviews
                                if (browseRoot.api && browseRoot.api.browseReviewsLoading)
                                    return "Loading Steam reviews…"
                                if (d.steamAppId > 0 && r.error)
                                    return r.error
                                if (d.steamAppId > 0 && r.total > 0) {
                                    const pct = Math.round(100 * r.positive / Math.max(1, r.positive + r.negative))
                                    return (r.summary ? r.summary + "  ·  " : "")
                                           + pct + "% positive of " + Number(r.total).toLocaleString(Qt.locale(), "f", 0)
                                           + " Steam reviews"
                                }
                                // No Steam app, or one nobody has reviewed.
                                const scores = [
                                    browseRoot.scoreLine(d.criticScore, d.criticCount, "critics"),
                                    browseRoot.scoreLine(d.userScore, d.userCount, "ratings")
                                ]
                                const parts = []
                                if (scores[0]) parts.push("Critics " + scores[0])
                                if (scores[1]) parts.push("IGDB users " + scores[1])
                                const lead = d.steamAppId > 0 ? "No Steam reviews yet." : "Not on Steam, so no player reviews."
                                return parts.length ? lead + "  " + parts.join("  ·  ") : lead
                            }
                        }

                        Repeater {
                            model: browseRoot.reviews.reviews || []
                            delegate: Rectangle {
                                id: reviewCard
                                required property var modelData
                                property bool expanded: false

                                width: Math.min(content.width, 1100)
                                height: reviewColumn.implicitHeight + 32
                                radius: 12
                                color: Theme.bgSurface
                                border.color: Theme.borderQuiet

                                // Backlight from the bottom-right corner: green for a
                                // recommendation, red for a pan. Inset by the border so
                                // the card's edge stays crisp, and rounded to the card's
                                // own radius so the glow never spills past the corners.
                                Shape {
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    preferredRendererType: Shape.CurveRenderer
                                    // Declared before the text column, so it paints under it.
                                    ShapePath {
                                        id: glowPath
                                        readonly property color glow: reviewCard.modelData.votedUp
                                                                      ? Theme.positive : Theme.danger
                                        strokeWidth: -1
                                        fillGradient: RadialGradient {
                                            centerX: reviewCard.width; centerY: reviewCard.height
                                            focalX: centerX; focalY: centerY
                                            centerRadius: Math.min(reviewCard.width * 0.5, 560)
                                            GradientStop { position: 0.0;  color: Qt.alpha(glowPath.glow, 0.32) }
                                            GradientStop { position: 0.45; color: Qt.alpha(glowPath.glow, 0.11) }
                                            GradientStop { position: 1.0;  color: Qt.alpha(glowPath.glow, 0.0) }
                                        }
                                        PathRectangle {
                                            x: 0; y: 0
                                            width: reviewCard.width - 2; height: reviewCard.height - 2
                                            radius: reviewCard.radius - 1
                                        }
                                    }
                                }

                                Column {
                                    id: reviewColumn
                                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 16 }
                                    spacing: 10

                                    Row {
                                        spacing: 12
                                        Image {
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 24; height: 24
                                            source: reviewCard.modelData.votedUp
                                                    ? "assets/thumb_up.png" : "assets/thumb_down.png"
                                            // The assets are 512x512; decode at twice the drawn
                                            // size so it stays sharp on a scaled display
                                            // without the GPU minifying the full bitmap.
                                            sourceSize.width: 48
                                            sourceSize.height: 48
                                            fillMode: Image.PreserveAspectFit
                                            smooth: true
                                        }
                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: reviewCard.modelData.votedUp ? "Recommended" : "Not recommended"
                                            color: reviewCard.modelData.votedUp ? Theme.positiveText : Theme.dangerText
                                            font.pixelSize: 14; font.bold: true
                                        }
                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: reviewCard.modelData.hours + " hrs on record  ·  "
                                                  + reviewCard.modelData.date
                                                  + (reviewCard.modelData.votesUp > 0
                                                     ? "  ·  " + reviewCard.modelData.votesUp + " found this helpful" : "")
                                            color: Theme.textFaint
                                            font.pixelSize: 12
                                        }
                                    }

                                    Text {
                                        id: reviewText
                                        width: parent.width
                                        text: browseRoot.plainReview(reviewCard.modelData.text)
                                        textFormat: Text.PlainText
                                        color: Theme.textBody
                                        font.pixelSize: 14
                                        lineHeight: 1.3
                                        wrapMode: Text.WordWrap
                                        maximumLineCount: reviewCard.expanded ? 1000 : 6
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        visible: reviewText.truncated || reviewCard.expanded
                                        text: reviewCard.expanded ? "Show less" : "Read more"
                                        color: moreArea.containsMouse ? Theme.linkHover : Theme.link
                                        font.pixelSize: 13; font.bold: true
                                        MouseArea {
                                            id: moreArea
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: reviewCard.expanded = !reviewCard.expanded
                                        }
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
        // pinned until it scrolls back. Under CLOSE and the lightbox.
        Rectangle {
            id: stickyHeader
            width: parent.width
            height: 84
            y: browseRoot.titleDocked ? 0 : -12
            opacity: browseRoot.titleDocked ? 1.0 : 0.0
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
                onWheel: (wheel) => browseRoot.scrollBy(-wheel.angleDelta.y)
            }

            Text {
                anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter
                          leftMargin: 40; rightMargin: 40 + 44 + 20 }
                text: browseRoot.fromRecommendation
                      ? browseRoot.gameName : (browseRoot.details.name || "")
                color: Theme.textPrimary
                font.pixelSize: 24; font.bold: true
                elide: Text.ElideRight
            }
        }

        // CLOSE -- outside the Flickable so it stays put while the page scrolls.
        Rectangle {
            id: closeButton
            readonly property bool emphasized: closeArea.containsMouse && browseRoot.mouseInControl

            anchors { top: parent.top; right: parent.right; margins: 20 }
            width: 44; height: 44; radius: 22
            color: closeButton.emphasized ? Theme.bgEmphasis : Theme.overlayButton
            border.width: closeButton.emphasized ? 2 : 1
            border.color: closeButton.emphasized ? Theme.focusRing : Theme.borderControl
            scale: closeButton.emphasized ? 1.1 : 1.0
            Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutQuart } }

            Text {
                anchors.centerIn: parent
                text: "✕"
                color: closeButton.emphasized ? Theme.textPrimary : Theme.textBody
                font.pixelSize: 20
            }
            MouseArea {
                id: closeArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: browseRoot.close()
            }
        }

        // ── Lightbox ────────────────────────────────────────────────────────
        Rectangle {
            id: lightbox
            anchors.fill: parent
            visible: browseRoot.lightboxIndex >= 0
            color: Theme.scrim
            radius: 20

            // Swallows clicks on the backdrop; a click there closes.
            MouseArea {
                anchors.fill: parent
                onClicked: browseRoot.lightboxIndex = -1
            }

            Image {
                id: lightboxImage
                anchors.fill: parent
                anchors.margins: 70
                asynchronous: true
                fillMode: Image.PreserveAspectFit
                source: lightbox.visible && browseRoot.screenshots[browseRoot.lightboxIndex]
                        ? browseRoot.screenshots[browseRoot.lightboxIndex].full : ""
            }

            Text {
                anchors.centerIn: parent
                visible: lightboxImage.status === Image.Loading
                text: "LOADING…"
                color: Theme.textGhost
                font.pixelSize: 14; font.bold: true; font.letterSpacing: 2
            }

            Text {
                anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter; bottomMargin: 26 }
                text: (browseRoot.lightboxIndex + 1) + " / " + browseRoot.screenshots.length
                color: Theme.textMuted
                font.pixelSize: 13
            }

            Repeater {
                model: [-1, 1]
                delegate: Rectangle {
                    id: arrow
                    required property int modelData
                    readonly property bool hovered: arrowArea.containsMouse

                    anchors.verticalCenter: parent.verticalCenter
                    x: arrow.modelData < 0 ? 16 : lightbox.width - width - 16
                    width: 46; height: 46; radius: 23
                    visible: browseRoot.screenshots.length > 1
                    color: arrow.hovered ? Theme.bgEmphasis : Theme.overlayButton
                    border.color: arrow.hovered ? Theme.focusRing : Theme.borderControl

                    // Same white double chevrons as the new-releases rail,
                    // decoded at twice the drawn size.
                    Image {
                        anchors.centerIn: parent
                        width: 20; height: 20
                        source: arrow.modelData < 0 ? "assets/arrow_left.png"
                                                    : "assets/arrow_right.png"
                        sourceSize.width: 40
                        sourceSize.height: 40
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                    }
                    MouseArea {
                        id: arrowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: browseRoot.stepLightbox(arrow.modelData)
                    }
                }
            }
        }
    }
}
