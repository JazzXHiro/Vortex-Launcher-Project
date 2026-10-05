pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Vortex

Window {
    id: root
    width: 1920; height: 1080; visible: true; title: "Vortex Launcher"; color: Theme.bgWindow

    property string activeFilter: "All"
    property string activeTab: "Library"
    property var api: vortexApi

    // The top bar's search box. One query for whichever tab is showing, and
    // cleared on every tab switch -- a filter left over from another list
    // would make this one look half empty for no visible reason.
    property string searchText: ""
    // When the query was last typed or the tab last switched. The cards on
    // show then, and any made just after, rise and fade in one after
    // another, as on Browse; a heart rebuilding Favorites or a rescan leaves
    // it alone, so those cards simply appear.
    property double popStamp: 0
    readonly property bool searchable:
        root.activeTab === "Library" || root.activeTab === "Favorites"
        || root.activeTab === "Wishlist" || root.activeTab === "Played"

    // Same rule as make_canonical() in game_manager.cpp: lowercase
    // alphanumerics only, so "stick fight the" finds "Stick Fight: The Game".
    function canonical(text) {
        return String(text || "").toLowerCase().replace(/[^a-z0-9]/g, "")
    }

    function matchesSearch(names) {
        const query = root.searchText.trim().toLowerCase()
        if (query === "") return true
        const wanted = root.canonical(query)
        for (let i = 0; i < names.length; i++) {
            const name = String(names[i] || "")
            if (name.toLowerCase().indexOf(query) >= 0) return true
            if (wanted !== "" && root.canonical(name).indexOf(wanted) >= 0) return true
        }
        return false
    }

    // True once RESET LIKES has been clicked and is showing its confirm step.
    property bool resetLikesArmed: false

    // Whether IGDB keys are present, which is what decides which Discover
    // empty state to show. credentialStatus() is a plain invokable rather than
    // a property, so QML cannot track it -- this mirrors it and is refreshed
    // on the signal the bridge emits when the keys are written.
    property bool igdbConfigured: false

    // Tracks whether IGDB is USABLE, not merely configured. The Discover
    // empty state used to key off presence alone, so a user whose keys Twitch
    // had rejected was told to go and play more games.
    property bool igdbWorking: false

    function refreshCredentialState() {
        if (!root.api) return
        const status = root.api.credentialStatus()
        root.igdbConfigured = status && status.igdb === true
        root.igdbWorking = root.igdbConfigured && status.igdbWorks === true
    }

    Connections {
        target: root.api
        function onCredentialsChanged() { root.refreshCredentialState() }
        // A refreshed ranking pops in like a tab switch, but only while it
        // is on show -- the stamp would otherwise replay whatever tab is up.
        // Cards rebuilt before this runs replay on the stamp; cards rebuilt
        // after it see a fresh one.
        //
        // The list also re-emits when art or metadata is patched into a
        // pick in place, which must not replay the tab. A finished ranking
        // is the one emit that comes straight after loading ends.
        function onRecommendationLoadingChanged() {
            if (!root.api.isRecommendationLoading)
                root.rankingDoneAt = Date.now()
        }
        function onRecommendationListChanged() {
            if (root.activeTab === "Recommendations"
                && Date.now() - root.rankingDoneAt <= 250)
                root.popStamp = Date.now()
        }
    }
    property double rankingDoneAt: 0

    // ─────────────────────────────────────────────────────────────────────────
    // Controller navigation
    //
    // controller_support.cpp emits intent signals; the routing below decides
    // what each one means for whatever is currently on top. Selection lives in
    // the grids' currentIndex, which stays at -1 — and therefore invisible —
    // until the pad is actually used, so mouse users never see a focus ring.
    // ─────────────────────────────────────────────────────────────────────────
    property var pad: controller
    property bool padActive: false
    property int focusedMood: -1

    // Mood ids in the order the cards appear on screen. focusedMood holds an
    // id, not a position, so the pad has to step through this rather than do
    // arithmetic on the id itself — Neutral is shown first but is id 3, so
    // `(focusedMood + step) % 4` would land on ids that are not where the user
    // sees them. Adding a mood means adding it here and nowhere else.
    readonly property var moodOrder: [3, 0, 1, 2]
    readonly property var filters: ["All", "Steam", "Local"]

    // Exactly one input owns the highlights at a time. A cursor parked on a
    // card keeps reporting containsMouse forever, so without this the mouse
    // leaves a second selector sitting behind the one the pad is moving.
    // The pad claims control when it acts and loses it the moment the mouse
    // moves — the same flag that hides and restores the pointer.
    readonly property bool padInControl: root.pad ? root.pad.padInControl : false
    readonly property bool mouseInControl: !root.padInControl

    // The settings panel is mouse-only, so the pad ignores everything but B
    // there.
    //
    // isLoading used to gate this too, back when a modal overlay covered the
    // whole window during a scan. The scan now publishes the library before it
    // starts fetching details and artwork, so there IS something worth
    // steering while it runs, and blocking the pad would make the controller
    // the one input that still cannot use the app.
    readonly property bool padBlocked: settingsWindow.visible

    // Which recommendation section the pad is steering. The tab shows two
    // independently ranked lists, so "the grid" is ambiguous there.
    property string recFocus: "library"

    // The section grids live inside a Repeater, so they cannot be reached by
    // id from out here; each registers itself on creation instead.
    property var libraryPickGridRef: null
    property var discoverGridRef: null

    function currentGrid() {
        if (root.activeTab === "Recommendations") {
            const grid = root.recFocus === "discover"
                       ? root.discoverGridRef : root.libraryPickGridRef
            return grid ? grid : gameGrid
        }
        if (root.activeTab === "Wishlist")
            return wishlistGrid
        if (root.activeTab === "Browse")
            return browsePage.grid
        // Library, Favorites and Played are the same grid with a different
        // model. Played reuses it rather than declaring its own so the cards
        // cannot drift out of step with the library's size.
        return gameGrid
    }

    // Index maths rather than GridView's own moveCurrentIndex* calls: those
    // refuse to move at all when the target cell does not exist, so a card in
    // column 3 could never step onto a final row holding only two games.
    function padGrid(direction) {
        const grid = root.currentGrid()
        if (grid.count === 0)
            return
        if (grid.currentIndex < 0) {   // first press only wakes the highlight
            grid.currentIndex = 0
            return
        }
        // Browse's new-releases rail is one row: up and down have nowhere to
        // go along it. GridView has no orientation, so grids never stop here.
        if (grid.orientation === ListView.Horizontal
            && (direction === "up" || direction === "down"))
            return

        // The grid's own count. Re-deriving it from width/cellWidth worked
        // only while cellWidth was fixed; now that the cells stretch, the
        // grid is the one thing that knows.
        const columns = Math.max(1, grid.columns)
        let target = grid.currentIndex

        if (direction === "left") {
            target -= 1
        } else if (direction === "right") {
            target += 1
        } else if (direction === "up") {
            target -= columns
        } else if (direction === "down") {
            target += columns
            // A short final row would otherwise swallow the press. Land on the
            // last game instead of refusing to leave the row above it.
            if (target >= grid.count && grid.currentIndex < grid.count - 1)
                target = grid.count - 1
        }

        // Running off the end of one recommendation section steps into the
        // other rather than refusing to move, so both lists are reachable.
        if (root.activeTab === "Recommendations") {
            const discover = root.discoverGridRef
            const library = root.libraryPickGridRef
            if (target >= grid.count && direction === "down"
                && root.recFocus === "library" && discover && discover.count > 0) {
                root.recFocus = "discover"
                discover.currentIndex = 0
                discover.positionViewAtIndex(0, GridView.Contain)
                return
            }
            if (target < 0 && direction === "up"
                && root.recFocus === "discover" && library && library.count > 0) {
                root.recFocus = "library"
                const last = library.count - 1
                library.currentIndex = last
                library.positionViewAtIndex(last, GridView.Contain)
                return
            }
        }

        if (target < 0 || target >= grid.count)
            return
        grid.currentIndex = target
        grid.positionViewAtIndex(target, GridView.Contain)
    }

    function controllerNavigate(direction) {
        root.padActive = true
        if (root.padBlocked)
            return
        if (moodOverlay.visible) {
            if (direction !== "left" && direction !== "right")
                return
            const step = direction === "right" ? 1 : -1
            const order = root.moodOrder
            if (root.focusedMood < 0) {
                root.focusedMood = order[0]      // first press lands on Neutral
            } else {
                const at = order.indexOf(root.focusedMood)
                root.focusedMood = order[(at + step + order.length) % order.length]
            }
            return
        }
        // Above the grid, below the details page: the menu only ever opens
        // over the cards, never on top of a page that is already open.
        if (tileMenu.visible) {
            tileMenu.navigate(direction)
            return
        }
        if (browseDetails.visible) {
            browseDetails.navigate(direction)
            return
        }
        if (detailPopup.visible) {
            detailPopup.navigate(direction)
            return
        }
        root.padGrid(direction)
    }

    function controllerAccept() {
        root.padActive = true
        if (root.padBlocked)
            return
        if (moodOverlay.visible) {
            if (root.focusedMood < 0)
                root.focusedMood = root.moodOrder[0]   // wake on Neutral, not id 0
            else
                moodOverlay.chooseMood(root.focusedMood)
            return
        }
        if (tileMenu.visible) {
            tileMenu.activateFocused()
            return
        }
        if (browseDetails.visible) {
            browseDetails.activateFocusedAction()
            return
        }
        if (detailPopup.visible) {
            detailPopup.activateFocusedAction()
            return
        }

        const grid = root.currentGrid()
        if (grid.currentIndex < 0 || grid.currentIndex >= grid.count)
            return
        // currentItem is the live delegate; the model is the fallback for the
        // rare frame where it has not been created yet.
        const game = (grid.currentItem && grid.currentItem.modelData)
                   ? grid.currentItem.modelData
                   : grid.model[grid.currentIndex]
        if (!game)
            return

        // Browse results open their own page: they are IGDB rows, not games
        // any of the lists GameDetails resolves from would know about.
        if (root.activeTab === "Browse") {
            browseDetails.focusedAction = 0
            browseDetails.openFor(game)
            return
        }

        // liveName is the library delegate's resolved title; the Discover
        // grid has no such property and its names never change under it.
        const name = (grid.currentItem && grid.currentItem.liveName)
                   ? grid.currentItem.liveName
                   : game.name

        // Every tab but Library opens the Browse-style page, by name:
        // Recommendations, Favorites, Wishlist and Played, owned or not. It
        // shows Play or Check on Steam itself.
        if (root.activeTab !== "Library") {
            browseDetails.focusedAction = 0
            browseDetails.openForGame(name, root.activeTab)
            return
        }

        detailPopup.launchOrigin = root.activeTab
        detailPopup.focusedAction = 0
        detailPopup.selectedGameName = name
        detailPopup.open()
    }

    // Start a game from its card, without going through the details page.
    // Origin is the tab it was started from, the same thing GameDetails passes,
    // so the recommender records where the launch came from.
    function playGame(name) {
        if (!root.api || !name)
            return
        root.api.launchGameFrom(name, root.activeTab)
    }

    // X — the pad's equivalent of the PLAY button on a card. A still opens the
    // details page and R3 still opens the tile menu; this is the shortcut past
    // both. Guarded like the others: whatever is open on top owns the input.
    function controllerPlay() {
        root.padActive = true
        if (root.padBlocked || moodOverlay.visible
            || detailPopup.visible || tileMenu.visible || browseDetails.visible)
            return
        if (root.activeTab === "Recommendations" || root.activeTab === "Wishlist"
            || root.activeTab === "Browse")
            return
        if (gameGrid.currentIndex < 0 || !gameGrid.currentItem)
            return
        // The delegate decides: it no-ops for a Played row whose files are gone.
        gameGrid.currentItem.play()
    }

    // R3 — the pad's equivalent of clicking a card's overflow button. Only the
    // library grid has one; the recommendation and wishlist cards are unchanged.
    function controllerOptions() {
        root.padActive = true
        if (root.padBlocked || moodOverlay.visible || detailPopup.visible
            || browseDetails.visible)
            return
        if (tileMenu.visible) {                      // pressing it again closes it
            tileMenu.close()
            return
        }
        if (root.activeTab === "Recommendations" || root.activeTab === "Wishlist"
            || root.activeTab === "Browse")
            return
        if (gameGrid.currentIndex < 0 || !gameGrid.currentItem)
            return
        gameGrid.currentItem.openTileMenu()
        tileMenu.focusedItem = 0                     // the pad opens with a selection
    }

    // B — one level back from wherever we are.
    function controllerBack() {
        root.padActive = true
        if (settingsWindow.visible) {
            settingsWindow.close()
            return
        }
        if (root.padBlocked || moodOverlay.visible)
            return                                   // nothing sits above the mood picker
        if (tileMenu.visible) {
            // Back drops the confirm step first, exactly as it does on the
            // details page, and only closes the menu once there is none.
            if (!tileMenu.handleBack())
                tileMenu.close()
            return
        }
        if (browseDetails.visible) {
            // A screenshot open full size is the level to come back from.
            if (!browseDetails.handleBack())
                browseDetails.close()
            return
        }
        if (detailPopup.visible) {
            // The details page eats Back itself when its uninstall confirm is
            // showing — that step is the "one level" to come back from.
            if (!detailPopup.handleBack())
                detailPopup.close()
            return
        }
        if (root.activeTab !== "Library")
            root.activeTab = "Library"
    }

    // LT / RT — step through ALL / STEAM / LOCAL.
    function cycleFilter(step) {
        root.padActive = true
        if (root.activeTab !== "Library" || root.padBlocked
                || detailPopup.visible || moodOverlay.visible)
            return
        const i = root.filters.indexOf(root.activeFilter)
        root.activeFilter = root.filters[(i + step + root.filters.length) % root.filters.length]
    }

    // Select button — step through the tabs. This used to be a two-way toggle
    // between Library and Recommendations; with more tabs a toggle would leave
    // the rest unreachable from the pad entirely.
    readonly property var tabs: ["Library", "Recommendations", "Browse", "Favorites", "Wishlist", "Played"]

    function toggleRecommendations() {
        root.padActive = true
        if (root.padBlocked || detailPopup.visible || moodOverlay.visible
            || browseDetails.visible)
            return

        const i = root.tabs.indexOf(root.activeTab)
        root.activeTab = root.tabs[(i + 1) % root.tabs.length]

        // Deliberately does NOT reload. Merely looking at the tab is not a
        // reason to reshuffle what is on it; the list changes when you ask
        // (refresh), when the app starts, or when you actually played
        // something.
        if (root.activeTab === "Recommendations")
            root.recFocus = "library"
    }

    onActiveFilterChanged: gameGrid.currentIndex = root.padActive ? 0 : -1
    onActiveTabChanged: {
        root.popStamp = Date.now()
        root.searchText = ""
        // Typing is the only thing to do on an empty Browse tab, so the box
        // takes the keyboard straight away. The pad never reads keys, so this
        // costs a controller user nothing.
        if (root.activeTab === "Browse" && !root.padActive)
            browsePage.focusSearch()
        const grid = root.currentGrid()
        if (root.padActive && grid.currentIndex < 0 && grid.count > 0)
            grid.currentIndex = 0
    }

    Connections {
        target: root.pad
        function onNavigate(direction)      { root.controllerNavigate(direction) }
        function onAccept()                 { root.controllerAccept() }
        function onCancel()                 { root.controllerBack() }
        function onFilterPrevious()         { root.cycleFilter(-1) }
        function onFilterNext()             { root.cycleFilter(1) }
        function onToggleRecommendations()  { root.toggleRecommendations() }
        function onOptions()                { root.controllerOptions() }
        function onPlay()                   { root.controllerPlay() }
    }

    // The tile overflow menu. One instance for the whole grid -- see TileMenu.qml.
    TileMenu {
        id: tileMenu
        api: root.api
        // The page itself, not the window: the window's content item also
        // holds the popup overlay, and a backdrop captured from that would
        // blur the menu back into itself.
        backdropItem: pageContent

        // Removing rebuilds gameList, and the grid binds to that as a plain JS
        // array -- GridView answers a new array by throwing the scroll position
        // away, which sent the user back to the top of the library every time
        // they removed a card. Same fix the details page uses for hearts: pin
        // where the grid was and let its own restoreScroll() re-assert that as
        // the new rows settle.
        onRemoveRequested: (name, installDir) => {
            gameGrid.restoreY = gameGrid.contentY
            if (root.api)
                root.api.removeFromLibrary(name, installDir)
            releaseScrollPin.restart()
        }

        // Same pin again: a local uninstall drops the row at once, and a Steam
        // one drops it when Steam finishes.
        onUninstallRequested: (name) => {
            gameGrid.restoreY = gameGrid.contentY
            if (root.api)
                root.api.uninstallGame(name)
            releaseScrollPin.restart()
        }

        // Hearting patches the row in place, but un-hearting on Favorites
        // takes the card off that grid -- pin for it, and close the menu
        // rather than leave it hanging off a card that is no longer there.
        onLikeRequested: (name) => {
            gameGrid.restoreY = gameGrid.contentY
            const wasLiked = tileMenu.liked
            if (root.api)
                root.api.updatePreference(name, 1.0)
            releaseScrollPin.restart()
            if (wasLiked && root.activeTab === "Favorites")
                tileMenu.close()
        }
    }

    // Lets go of that pin once the grid has settled. contentHeight is not final
    // when countChanged fires, so it cannot be dropped on the line after the
    // call; leaving it set for good would snap a later filter or tab change
    // back to a position that no longer means anything.
    // Laid over the page while the tile menu is open, which is not modal so
    // the wheel still reaches the page. Eats the press that closes the menu
    // so it never lands on the card underneath, and blocks hover so no other
    // card lights up behind it. The wheel goes on through to scroll the page,
    // and fades the menu out first: the card it is fastened to is about to
    // move.
    MouseArea {
        id: tileMenuCatcher
        anchors.fill: parent
        z: 1000
        visible: tileMenu.visible
        hoverEnabled: true
        onPressed: tileMenu.close()
        onWheel: (wheel) => {
            tileMenu.dismissForScroll()
            wheel.accepted = false
        }
    }

    Timer {
        id: releaseScrollPin
        interval: 400
        onTriggered: gameGrid.restoreY = -1
    }

    GameDetails {
        id: detailPopup

        // A heart toggled on this page rebuilds the grid underneath, so this
        // is the last moment the grids' scroll positions are still intact.
        // Both are captured to match browseDetails below; a grid with nothing
        // pending simply never restores.
        onOpened: {
            gameGrid.restoreY = gameGrid.contentY
            wishlistGrid.restoreY = wishlistGrid.contentY
        }

        onClosed: {
            detailPopup.focusedAction = -1
            gameGrid.restoreY = -1
            wishlistGrid.restoreY = -1
        }
    }

    BrowseDetails {
        id: browseDetails

        // Favorites, Wishlist and Played open this page now, and a heart or
        // wishlist toggle on it rebuilds the grid underneath -- the same pin
        // detailPopup takes, for the same reason.
        onOpened: {
            gameGrid.restoreY = gameGrid.contentY
            wishlistGrid.restoreY = wishlistGrid.contentY
        }

        onClosed: {
            gameGrid.restoreY = -1
            wishlistGrid.restoreY = -1
        }
    }

    SettingsWindow {
        id: settingsWindow
        onRequestAddDirectory: {
            localFolderDialog.fromSettings = true
            localFolderDialog.open()
        }
    }

    // Shown once on a fresh install, when no API keys have been entered yet.
    // Deliberately not blocking anything: the library scan below has already
    // started, and every pane of the wizard can be skipped. Opened from
    // Component.onCompleted rather than at construction so the window is up
    // first and the wizard animates over a populated UI, not a black screen.
    FirstRunWizard {
        id: firstRunWizard
    }

    Component.onCompleted: {
        root.refreshCredentialState()
        if (!root.api.hasCredentials())
            firstRunWizard.open()
    }

    // Opened from the settings panel's Directories section, which is now the
    // only way in — the top bar's "+ FOLDER" shortcut duplicated it and went.
    // fromSettings is therefore always true today; it stays because it is what
    // routes the result back to the panel, and unpicking it would fork the
    // dialog's one code path for no gain.
    FolderDialog {
        id: localFolderDialog
        title: "Add Local Game Folder"
        property bool fromSettings: false

        onAccepted: {
            const added = root.api.addLocalGameDirectory(String(selectedFolder))
            if (localFolderDialog.fromSettings)
                settingsWindow.reportAddResult(added)
            localFolderDialog.fromSettings = false
        }
        onRejected: localFolderDialog.fromSettings = false
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Main content: filter bar + game grid
    // ─────────────────────────────────────────────────────────────────────────
    // Where the cards' hover glows are drawn (see CoverGlow.qml). Behind the
    // content and outside every grid, so a glow is never cut off by a grid's
    // clip and sits under the neighbouring cards rather than over them.
    Item {
        id: glowStage
        anchors.fill: parent
    }

    // The scrolling view on the page showing, and whether it has left the top.
    // Browse's grid is typed Item over there; it is a GridView or the
    // horizontal rail, and the rail never leaves the top.
    readonly property Item scrollingView:
        root.activeTab === "Recommendations" ? recommendationFlick
        : root.activeTab === "Wishlist" ? wishlistGrid
        : root.activeTab === "Browse" ? browsePage.grid
        : gameGrid
    readonly property bool contentScrolled:
        root.scrollingView !== null && !root.scrollingView.atYBeginning

    // ── Header backdrop ─────────────────────────────────────────────────────
    //
    // Window-coloured cover over everything above the scrolling view, between
    // the glows and the content. Clear at the top of the page, so the top
    // row's glow rises softly behind the tabs. Once scrolled, the lit card can
    // be half under the view's top edge with its whole glow spilling up into
    // the header -- too strong there -- so the header goes opaque.
    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        // Mapped rather than added up from the layout, since the view sits
        // under a different stack of headers on each page. Re-read when the
        // window resizes and when scrolling starts, which is all that moves
        // it -- and is after the layout has settled.
        height: {
            root.width; root.height; root.contentScrolled   // dependencies
            return root.scrollingView ? root.scrollingView.mapToItem(null, 0, 0).y : 0
        }
        color: Theme.bgWindow
        visible: opacity > 0
        opacity: root.contentScrolled ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 200 } }

        // A short fade past the view's top edge rather than a hard line: the
        // glow in the gutters beside a clipped card tails off into it.
        Rectangle {
            anchors { left: parent.left; right: parent.right; top: parent.bottom }
            height: 24
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.bgWindow }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }
    }

    ColumnLayout {
        id: pageContent
        anchors.fill: parent
        anchors.margins: 40
        spacing: 30

        // Top navigation + library filters
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 35
            // Clear, so a top-row card's glow shows through behind the tabs
            // instead of stopping at a hard line along the bar.
            color: "transparent"
            z: 10

            RowLayout {
                anchors.fill: parent
                spacing: 15

            Row {
                spacing: 10
                Layout.alignment: Qt.AlignLeft

                Repeater {
                    model: ["Library", "Recommendations", "Browse", "Favorites", "Wishlist", "Played"]
                    delegate: Rectangle {
                        id: tabButton
                        required property string modelData

                        readonly property bool active: root.activeTab === tabButton.modelData
                        // Hover stops short of the active look, so the two stay
                        // tellable apart when the mouse is sitting on a tab.
                        readonly property bool hovered:
                            tabArea.containsMouse && root.mouseInControl && !tabButton.active

                        // Derived from the label rather than a per-tab special
                        // case, so adding a tab needs no width bookkeeping.
                        width: tabLabel.implicitWidth + 44
                        height: 35
                        radius: 17
                        color: tabButton.active ? Theme.accent : (tabButton.hovered ? Theme.bgEmphasis : Theme.bgRaised)
                        border.color: tabButton.hovered ? Theme.borderStrong : Theme.borderControl

                        Behavior on color { ColorAnimation { duration: 150 } }

                        Text {
                            id: tabLabel
                            anchors.centerIn: parent
                            text: tabButton.modelData.toUpperCase()
                            font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                            color: tabButton.active ? Theme.textInverse : (tabButton.hovered ? Theme.textBody : Theme.textMuted)
                        }

                        MouseArea {
                            id: tabArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.activeTab = tabButton.modelData
                                // No reload here — see toggleRecommendations().
                                if (tabButton.modelData === "Recommendations")
                                    root.recFocus = "library"
                            }
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
            }

            // ─────────────────────────────────────────────────────────────
            // Scan progress
            //
            // This replaced a full-screen 90%-opaque modal overlay. On a
            // machine with cold caches that overlay sat there for as long as
            // it took to resolve every title and download every cover, with an
            // indeterminate spinner and no way to touch anything — the app
            // looked hung. The scan now publishes titles before it starts
            // fetching, so the only thing still needed is a quiet indication
            // that work is ongoing.
            //
            // Deliberately small and in the bar rather than over the content:
            // it reports, it does not interrupt. It disappears the instant
            // scanActive goes false.
            // ─────────────────────────────────────────────────────────────
            Row {
                id: scanStrip
                spacing: 10
                visible: root.api ? root.api.scanActive : false
                Layout.alignment: Qt.AlignVCenter

                readonly property int done:  root.api ? root.api.scanDone  : 0
                readonly property int total: root.api ? root.api.scanTotal : 0

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.api ? root.api.scanPhase : ""
                    color: Theme.textMuted
                    font.pixelSize: 11
                    font.bold: true
                    font.letterSpacing: 1
                }

                // The count beside it carries the progress; the bar only says
                // "still working".
                LoadingBar {
                    anchors.verticalCenter: parent.verticalCenter
                    running: scanStrip.visible
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: scanStrip.total > 0
                    text: scanStrip.done + " / " + scanStrip.total
                    color: Theme.textFaint
                    font.pixelSize: 11
                }
            }

            // Search within the tab -- not on Recommendations, which is a ranked
            // list rather than a collection to look things up in. Bound both
            // ways through root.searchText so a tab switch clears the box.
            SearchField {
                id: librarySearch
                Layout.alignment: Qt.AlignVCenter
                visible: root.searchable
                placeholderText: root.activeTab === "Library"
                                 ? "Search installed games"
                                 : "Search " + root.activeTab.toLowerCase()
                text: root.searchText
                onTextChanged: {
                    // Equal when the change came down the binding (a tab
                    // switch clearing it, which stamps for itself) rather
                    // than from the keyboard.
                    if (librarySearch.text === root.searchText)
                        return
                    // Stamped first, so it is set before the grids rebuild.
                    root.popStamp = Date.now()
                    root.searchText = librarySearch.text
                }
            }

            Shortcut {
                sequence: "Ctrl+F"
                enabled: (root.searchable || root.activeTab === "Browse")
                         && !detailPopup.visible && !browseDetails.visible
                         && !moodOverlay.visible
                onActivated: root.activeTab === "Browse" ? browsePage.focusSearch()
                                                         : librarySearch.focusField()
            }

            // Sits after the scan strip rather than beside the tabs.
            // Only one of these three is ever visible at once — filters on
            // Library, reset on Favorites, refresh on Recommendations — so
            // they share the same slot on the right.
            Row {
                spacing: 15
                Layout.alignment: Qt.AlignVCenter
                visible: root.activeTab === "Library"

                Repeater {
                    model: ["All", "Steam", "Local"]
                    delegate: Rectangle {
                        id: filterButton
                        required property string modelData

                        readonly property bool active: root.activeFilter === filterButton.modelData
                        readonly property bool hovered:
                            filterArea.containsMouse && root.mouseInControl && !filterButton.active

                        // Hugs its label, like the tab pills above, rather
                        // than padding every filter out to one fixed width.
                        width: filterLabel.implicitWidth + 52
                        height: 35; radius: 17
                        color: filterButton.active ? Theme.accent : (filterButton.hovered ? Theme.bgEmphasis : Theme.bgRaised)
                        border.color: filterButton.hovered ? Theme.borderStrong : Theme.borderControl

                        Behavior on color { ColorAnimation { duration: 150 } }

                        Text {
                            id: filterLabel
                            anchors.centerIn: parent
                            text: filterButton.modelData.toUpperCase()
                            font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                            color: filterButton.active ? Theme.textInverse : (filterButton.hovered ? Theme.textBody : Theme.textMuted)
                        }

                        MouseArea {
                            id: filterArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.activeFilter = filterButton.modelData
                        }
                    }
                }
            }

            // Reset likes — empties the taste profile the recommender ranks
            // from, for a clean start. Not undoable, so it arms first rather
            // than firing on one click, the same two-step the folder removal in
            // SettingsWindow uses. Hidden with nothing to clear.
            Row {
                id: resetLikesRow
                spacing: 8
                readonly property bool hasFavorites:
                    root.api && root.api.favoriteGames && root.api.favoriteGames.length > 0

                visible: root.activeTab === "Favorites" && resetLikesRow.hasFavorites
                // Covers both leaving the tab and clearing the last favourite,
                // so the confirm step is never left armed for a later visit.
                onVisibleChanged: if (!resetLikesRow.visible) root.resetLikesArmed = false

                Rectangle {
                    id: resetLikesButton
                    readonly property bool hovered: resetLikesArea.containsMouse && root.mouseInControl

                    width: 150; height: 35; radius: 17
                    visible: !root.resetLikesArmed
                    color: resetLikesButton.hovered ? Theme.accent : Theme.bgRaised
                    border.color: resetLikesButton.hovered ? Theme.focusRing : Theme.borderControl

                    Behavior on color { ColorAnimation { duration: 150 } }

                    Text {
                        anchors.centerIn: parent
                        text: "RESET LIKES"
                        font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                        color: resetLikesButton.hovered ? Theme.textInverse : Theme.textMuted
                    }

                    MouseArea {
                        id: resetLikesArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.resetLikesArmed = true
                    }
                }

                Rectangle {
                    id: confirmResetButton
                    width: 170; height: 35; radius: 17
                    visible: root.resetLikesArmed
                    color: confirmResetArea.containsMouse ? Theme.danger : Theme.dangerRest

                    Text {
                        anchors.centerIn: parent
                        text: "CLEAR ALL LIKES"
                        color: Theme.textPrimary; font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                    }

                    MouseArea {
                        id: confirmResetArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.resetLikesArmed = false
                            if (root.api)
                                root.api.resetPreferences()
                        }
                    }
                }

                Rectangle {
                    id: cancelResetButton
                    width: 100; height: 35; radius: 17
                    visible: root.resetLikesArmed
                    color: cancelResetArea.containsMouse ? Theme.bgEmphasis : "transparent"
                    border.color: Theme.borderControl; border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "CANCEL"
                        color: Theme.textSecondary; font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                    }

                    MouseArea {
                        id: cancelResetArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.resetLikesArmed = false
                    }
                }
            }

            // Settings — full directory management lives here.
            Rectangle {
                id: settingsButton
                readonly property bool hovered: settingsArea.containsMouse && root.mouseInControl

                implicitWidth: 35; implicitHeight: 35; radius: 17
                // Not the accent fill on hover: the accent is white, and so is the icon.
                color: settingsButton.hovered ? Theme.bgEmphasis : Theme.bgRaised
                border.color: settingsButton.hovered ? Theme.focusRing : Theme.borderControl

                onHoveredChanged: if (hovered && !settingsIcon.running) settingsIcon.restart()

                // 180 frames at 60 fps, 56 px cells on a 15x12 sheet (2x for HiDPI).
                // One full roll per hover; it finishes even if the pointer leaves.
                AnimatedSprite {
                    id: settingsIcon
                    anchors.centerIn: parent
                    width: 28; height: 28
                    source: "assets/settings_roll.png"
                    frameWidth: 56; frameHeight: 56
                    frameCount: 180
                    frameRate: 60
                    loops: 1
                    running: false
                    interpolate: false
                    smooth: true
                }

                MouseArea {
                    id: settingsArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: settingsWindow.open()
                }
            }

            Rectangle {
                id: refreshButton
                readonly property bool hovered: refreshRecommendationArea.containsMouse && root.mouseInControl

                implicitWidth: 120; implicitHeight: 35; radius: 17
                visible: root.activeTab === "Recommendations"
                color: refreshButton.hovered ? Theme.accent : Theme.bgRaised
                border.color: refreshButton.hovered ? Theme.focusRing : Theme.borderControl

                Text {
                    anchors.centerIn: parent
                    text: "REFRESH"
                    font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                    color: refreshButton.hovered ? Theme.textInverse : Theme.textMuted
                }

                MouseArea {
                    id: refreshRecommendationArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.api) root.api.loadRecommendations()
                }
            }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            // Library, Favorites and Played share page 0 — same cards,
            // different model.
            currentIndex: root.activeTab === "Recommendations" ? 1
                        : root.activeTab === "Wishlist" ? 2
                        : root.activeTab === "Browse" ? 3 : 0

            // Game grid
            GridView {
                id: gameGrid
                clip: true
                // Elastic gutters. A GridView fits floor(width/cellWidth)
                // columns and abandons the remainder at the right edge -- a
                // whole card's width of dead space at some sizes. Taking the
                // count from a minimum and dividing the width back out spends
                // the remainder on the gutters instead, and the card itself
                // never changes size.
                //
                // 280 rather than 300: the minimum only decides how many
                // columns fit, and 300 was one short on a 1536-wide desktop --
                // four columns with 124px between them. 280 buys the fifth and
                // pulls the gutters back to ~51.
                readonly property int minCellWidth: 280
                readonly property int columns:
                    Math.max(1, Math.floor(gameGrid.width / gameGrid.minCellWidth))

                cellWidth: gameGrid.width > 0
                           ? Math.floor(gameGrid.width / gameGrid.columns)
                           : gameGrid.minCellWidth
                cellHeight: 440

                ScrollBar.vertical: VortexScrollBar { }

                // No current item until the pad asks for one. A model reload
                // (first scan, filter change) puts it back to -1 for the mouse,
                // and back onto the first card once the pad is in use.
                currentIndex: -1
                Text {
                    anchors.centerIn: parent
                    visible: gameGrid.count === 0 && root.searchText.trim() !== ""
                    text: "NO MATCHES FOR \u201C" + root.searchText.trim() + "\u201D"
                    width: gameGrid.width - 80
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideMiddle
                    color: Theme.textGhost
                    font.pixelSize: 16
                    font.bold: true
                    font.letterSpacing: 2
                }

                Text {
                    anchors.centerIn: parent
                    visible: gameGrid.count === 0 && root.activeTab === "Favorites"
                             && root.searchText.trim() === ""
                    text: "NO FAVORITES YET\nOpen a game and tap the heart"
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.textGhost
                    font.pixelSize: 16
                    font.bold: true
                    font.letterSpacing: 2
                }

                Text {
                    anchors.centerIn: parent
                    visible: gameGrid.count === 0 && root.activeTab === "Played"
                             && root.searchText.trim() === ""
                    text: "NOTHING PLAYED YET\nLaunch a game and it lands here for good"
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.textGhost
                    font.pixelSize: 16
                    font.bold: true
                    font.letterSpacing: 2
                }

                // Where to put the grid back after the details page changes
                // a list it is showing.
                //
                // Favorites and Played are derived lists: a heart moving
                // rebuilds them, GridView is handed a new JS array and answers
                // by dropping the scroll position. The Library escapes this
                // because the bridge patches gameList in place (see
                // updatePreference), which a derived list cannot do.
                //
                // Captured when the details page opens rather than when the
                // change lands -- by then the old position is already gone --
                // and re-asserted rather than applied once, because
                // contentHeight is not final when countChanged fires. Safe to
                // re-assert: the page is modal, so nothing can scroll the grid
                // while a value is pending. -1 means nothing is.
                property real restoreY: -1

                function restoreScroll() {
                    if (gameGrid.restoreY < 0)
                        return
                    gameGrid.contentY = Math.max(0, Math.min(
                        gameGrid.restoreY,
                        gameGrid.contentHeight - gameGrid.height))
                }

                onContentHeightChanged: gameGrid.restoreScroll()

                onCountChanged: {
                    gameGrid.restoreScroll()

                    if (!root.padActive || gameGrid.count === 0)
                        gameGrid.currentIndex = -1
                    else if (gameGrid.currentIndex < 0)
                        gameGrid.currentIndex = 0
                }

                // Favorites is the library grid with a different model rather
                // than a second copy of the card markup.
                model: {
                    if (!root.api) return [];
                    let list;
                    if (root.activeTab === "Favorites")
                        list = root.api.favoriteGames || [];
                    // Everything ever played, installed or not. Comes from the
                    // bridge already ordered and de-duplicated -- see
                    // VortexBridge::playedGames().
                    else if (root.activeTab === "Played")
                        list = root.api.playedGames || [];
                    else if (!root.api.gameList)
                        list = [];
                    else if (root.activeFilter === "All")
                        list = root.api.gameList;
                    else
                        list = root.api.gameList.filter(function(game) {
                            return game.source === root.activeFilter;
                        });

                    if (root.searchText.trim() === "")
                        return list;
                    // The live title as well as the snapshot's: pass 2 renames
                    // a local game in place without republishing the list, so
                    // the card can show a name modelData never had (see
                    // liveDetails below).
                    return list.filter(function(game) {
                        const live = game.installDir
                            ? root.api.gameDetailsForInstallDir(game.installDir) : null;
                        return root.matchesSearch([game.name, live ? live.name : ""]);
                    });
                }

                delegate: Item {
                    id: gameDelegate
                    required property var modelData
                    required property int index

                    // Mouse hover and controller focus light the card the same
                    // way, but only whichever input is currently driving.
                    //
                    // The overflow button and the play button both count as
                    // being on the card. They sit above cardArea and take the
                    // hover off it, and since both only show while the card is
                    // lit, leaving either out made the two chase each other:
                    // hover the button, the card unlights, the button vanishes,
                    // the card lights again.
                    readonly property bool highlighted:
                        ((cardArea.containsMouse || overflowArea.containsMouse
                          || playArea.containsMouse)
                         && root.mouseInControl)
                        || (gameDelegate.GridView.isCurrentItem && root.padInControl)

                    // The live row for this card, re-read whenever the scan
                    // publishes something new.
                    //
                    // Keyed on installDir, NOT on name. The scan publishes
                    // titles in pass 1 and only then resolves them against
                    // IGDB, and for a local game the canonical title replaces
                    // the folder name the card was published under. It updates
                    // the row in place and bumps artRevision instead of
                    // re-emitting gameListChanged, precisely so the grid is NOT
                    // rebuilt and the user's scroll position survives -- which
                    // means modelData here is a stale snapshot whose name may
                    // no longer match any live row. installDir is fixed at scan
                    // time, so it still does.
                    readonly property var liveDetails: {
                        if (!root.api) return null
                        const _ = root.api.artRevision   // dependency, deliberate
                        const byDir = root.api.gameDetailsForInstallDir(
                                          gameDelegate.modelData.installDir || "")
                        if (byDir && byDir.name) return byDir
                        // Nothing with that install directory (a favourite
                        // built from a different source, say): the title is the
                        // only key left.
                        return root.api.gameDetailsFor(gameDelegate.modelData.name)
                    }

                    // Canonical title once resolved, falling back to whatever
                    // the card was published with.
                    readonly property string liveName:
                        (gameDelegate.liveDetails && gameDelegate.liveDetails.name)
                            ? gameDelegate.liveDetails.name
                            : (gameDelegate.modelData.name || "")

                    // The three facts the caption under the title carries.
                    // Same live-row-first rule as liveName above: a session that
                    // just ended shows its new total without a rescan.
                    readonly property string playtimeLabel:
                        (gameDelegate.liveDetails && gameDelegate.liveDetails.playtime)
                            ? gameDelegate.liveDetails.playtime
                            : (gameDelegate.modelData.playtime || "0m")

                    // Live row first, like liveName and playtimeLabel above.
                    // modelData is the snapshot the card was published with, and
                    // during a cold scan that snapshot carries "Unknown" for
                    // every metadata slot until IGDB resolves the title. Reading
                    // it directly meant the caption stayed blank until something
                    // rebuilt the grid; through liveDetails it fills itself in
                    // as the scan lands, which is what artRevision is for.
                    readonly property string sourceLabel:
                        (gameDelegate.liveDetails && gameDelegate.liveDetails.source)
                            ? gameDelegate.liveDetails.source
                            : (gameDelegate.modelData.source || "")

                    readonly property bool fromSteam:
                        gameDelegate.sourceLabel === "Steam"

                    // applyGameMetadata() hands over the whole comma-separated
                    // list ("Role-playing (RPG), Simulator, Strategy"); only the
                    // first one fits under a 240px card.
                    //
                    // IGDB spells its genres out in full, and the long ones
                    // elide mid-word into "Role-playing (RP..." at this width.
                    // Where it carries an abbreviation in brackets that is both
                    // shorter and what anyone actually calls the genre, so
                    // prefer it: "Role-playing (RPG)" reads as "RPG".
                    readonly property string genreLabel: {
                        const live = gameDelegate.liveDetails
                        const g = ((live && live.genres)
                                   ? live.genres
                                   : gameDelegate.modelData.genres) || ""
                        if (g === "" || g === "Unknown") return ""
                        const first = g.split(",")[0].trim()
                        const abbreviated = first.match(/\(([^)]+)\)/)
                        return abbreviated ? abbreviated[1] : first
                    }

                    // Launching something that is not on the disk fails with
                    // nothing to show for it. Played holds uninstalled games,
                    // and so does Favorites -- a hearted Discover pick, or a
                    // played game since removed. Library rows are all installed.
                    readonly property bool launchable:
                        (root.activeTab !== "Played" && root.activeTab !== "Favorites")
                        || gameDelegate.modelData.installed === true

                    // 415, not the old 400: the caption line is the fourth row
                    // of text the comment on the badges below used to rule out.
                    // cellHeight is 440 and already had the room.
                    //
                    // The extra 16 each way is room for the hover scale. The
                    // card grows 1.04x about its centre -- ~5px past each side,
                    // ~7px past the top -- and a delegate exactly the card's
                    // size put the first column flush with the grid's clipped
                    // left edge and the first row with its top, so both lost a
                    // sliver on hover. Centring the card in 256x431 keeps 8px
                    // clear all round and still fits the 440 cell.
                    width: 256; height: 431

                    // A fresh search answer or tab: rise and fade in, a beat
                    // after the card before it, as Browse's results do. Cards
                    // already up when the stamp lands replay too -- going back
                    // to Library does not rebuild its grid. Counted from the
                    // first row in view, so a scrolled grid does not wait out
                    // the cards above it.
                    transform: Translate { id: gameEnterShift }
                    function popIn() {
                        const firstShown = Math.floor(gameGrid.contentY / gameGrid.cellHeight)
                                           * gameGrid.columns
                        gameEnterPause.duration =
                            Math.max(0, Math.min(gameDelegate.index - firstShown, 12)) * 40
                        gameEnterAnim.stop()
                        gameDelegate.opacity = 0
                        gameEnterShift.y = 18
                        gameEnterAnim.start()
                    }
                    Component.onCompleted: {
                        if (Date.now() - root.popStamp <= 250)
                            gameDelegate.popIn()
                    }
                    Connections {
                        target: root
                        function onPopStampChanged() {
                            if (root.activeTab === "Library" || root.activeTab === "Favorites"
                                || root.activeTab === "Played")
                                gameDelegate.popIn()
                        }
                    }
                    SequentialAnimation {
                        id: gameEnterAnim
                        PauseAnimation { id: gameEnterPause }
                        ParallelAnimation {
                            NumberAnimation {
                                target: gameDelegate; property: "opacity"
                                to: 1.0; duration: 380; easing.type: Easing.OutCubic
                            }
                            NumberAnimation {
                                target: gameEnterShift; property: "y"
                                to: 0; duration: 380; easing.type: Easing.OutCubic
                            }
                        }
                    }

                    // Started by the card's own PLAY button and, for the pad,
                    // by root.controllerPlay() reaching through currentItem --
                    // the same shape as openTileMenu() below, and for the same
                    // reason: the delegate is what knows whether this row can
                    // be launched and what it is actually called.
                    function play() {
                        if (!gameDelegate.launchable)
                            return
                        root.playGame(gameDelegate.liveName)
                    }

                    // Opened by the card's own button and, for the pad, by
                    // root.controllerOptions() reaching through currentItem.
                    function openTileMenu() {
                        tileMenu.openFor(overflowButton, gameDelegate.liveName,
                                         gameDelegate.modelData.installDir || "",
                                         gameDelegate.launchable,
                                         gameDelegate.sourceLabel === "Local")
                    }

                    // Drawn on root's glowStage, behind the page: see
                    // CoverGlow.qml.
                    CoverGlow {
                        target: capsuleContainer
                        stage: glowStage
                        source: gameCover
                        lit: gameDelegate.highlighted
                    }

                    Column {
                        anchors.centerIn: parent
                        spacing: 12
                        // Above cardArea, so the play button drawn on the
                        // artwork can take its own clicks. Nothing else in here
                        // holds a MouseArea, so the card stays clickable
                        // everywhere the button is not.
                        z: 1

                        Rectangle {
                            id: capsuleContainer
                            width: 240; height: 360
                            color: Theme.bgRaised; radius: 12
                            border.color: gameDelegate.highlighted ? Theme.focusRing : Theme.borderControl
                            border.width: 2; clip: true

                            Image {
                                id: gameCover
                                anchors.fill: parent

                                // Read through the bridge rather than straight
                                // off modelData, because artwork arrives after
                                // this card already exists. Falls back to the
                                // snapshot so an already-cached cover still
                                // shows on the very first frame.
                                source: (gameDelegate.liveDetails && gameDelegate.liveDetails.coverPath)
                                        ? gameDelegate.liveDetails.coverPath
                                        : (gameDelegate.modelData.coverPath || "")
                                fillMode: Image.PreserveAspectCrop
                                opacity: status === Image.Ready ? 1.0 : 0.0
                                Behavior on opacity { NumberAnimation { duration: 250 } }
                            }

                            Column {
                                anchors.centerIn: parent; spacing: 10
                                visible: gameCover.status !== Image.Ready
                                Text { text: "NO ART"; color: Theme.textGhost; font.bold: true }
                            }

                            // ── Source mark ─────────────────────────────────
                            //
                            // On the artwork rather than in the caption, which
                            // leaves the caption for the two facts that have to
                            // be read rather than recognised. White for both, so
                            // Steam and local read as one set of marks and not
                            // as two logos competing with the cover behind them.
                            Item {
                                anchors { left: parent.left; top: parent.top; margins: 5 }
                                width: 30; height: 30
                                opacity: 0.6

                                // Library only, though this delegate also
                                // serves Favorites and Played. Those two are
                                // already narrowed to games you know, and
                                // Played carries the marker that earns its
                                // place there -- the green installed dot in
                                // the caption, for the rows whose files are
                                // gone. Library is the one grid you scan whole.
                                visible: root.activeTab === "Library"

                                // Steam's own colours, unfiltered. The mark is
                                // a filled disc whose meaning is the contrast
                                // between the pipe glyph and the circle behind
                                // it, so anything that flattens it -- whitening
                                // it especially -- costs the glyph the ground it
                                // reads against. It is the one spot of colour on
                                // an otherwise monochrome card, which is the
                                // trade for it being recognisable at 30px.
                                Image {
                                    anchors.fill: parent
                                    source: "assets/steam.png"
                                    // Decoded at the size it is drawn, the way
                                    // the Check on Steam button does it. The
                                    // asset is 600x600: handed over whole, the
                                    // GPU bilinear-samples a 20x reduction every
                                    // frame and the glyph crawls with aliasing.
                                    sourceSize.width: 30
                                    sourceSize.height: 30
                                    fillMode: Image.PreserveAspectFit
                                    smooth: true
                                    visible: gameDelegate.fromSteam
                                }

                                // The save mark off the design canvas, flattened
                                // to white on transparent at build time rather
                                // than by an effect at run time: it is a solid
                                // silhouette, so the whitening the Steam disc
                                // could not survive costs this one nothing, and
                                // baking it keeps a framebuffer off every card.
                                Image {
                                    anchors.fill: parent
                                    source: "assets/local.png"
                                    sourceSize.width: 30
                                    sourceSize.height: 30
                                    fillMode: Image.PreserveAspectFit
                                    smooth: true
                                    visible: !gameDelegate.fromSteam
                                }
                            }

                            // ── Focus treatment ───────────────────────
                            //
                            // The strip and the play button arrive with the
                            // overflow button, on the same condition, so the
                            // whole card resolves in one step.
                            //
                            // The playtime pill and the INSTALLED badge used to
                            // live down here. They are in the caption now, which
                            // is what freed the bottom of the cover.
                            Item {
                                // Flush with the capsule's own edges rather
                                // than the content box inside its border: the
                                // strip is meant to read as the bottom of the
                                // card, so it spans the full width and sits on
                                // the bottom edge. It paints over the border
                                // along the way, which its 0.5 opacity lets
                                // through.
                                anchors.fill: parent

                                visible: opacity > 0
                                opacity: (gameDelegate.highlighted
                                          || overflowButton.menuOpen) ? 1.0 : 0.0
                                Behavior on opacity { NumberAnimation { duration: 150 } }

                                // The canvas blurs this strip, but a CSS filter
                                // blurs the element's own content rather than
                                // the backdrop, and the content is a smooth
                                // gradient -- so the blur is very nearly a
                                // no-op and a plain Rectangle stands in for it.
                                // Worth the substitution: a MultiEffect here
                                // would cost a framebuffer on every card in the
                                // grid to soften two edges.
                                Rectangle {
                                    id: focusScrim
                                    anchors {
                                        left: parent.left
                                        right: parent.right
                                        bottom: parent.bottom
                                    }
                                    height: 140

                                    // Square, because the artwork underneath is
                                    // square: clip on a rounded Rectangle clips
                                    // to the bounding box, not the rounded
                                    // shape, so the cover fills the corners the
                                    // capsule's own radius only appears to cut.
                                    // Rounding the strip pulled it off those
                                    // corners and left a crescent of bare art.

                                    // Transparent at the top so the cover reads
                                    // straight into the wash, solid at the
                                    // bottom so the pill has full contrast under
                                    // it. No blanket opacity: the fade is in the
                                    // gradient, which keeps the bottom edge
                                    // properly opaque instead of leaving bright
                                    // artwork showing through it.
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: "transparent" }
                                        GradientStop { position: 1.0; color: Theme.artScrim }
                                    }
                                }

                                // The white pill this had before the artboard
                                // pass: centred across the card rather than
                                // filling a corner of it.
                                //
                                // Pinned near the bottom edge rather than to the
                                // wash's centre: the wash is 140 tall now, and
                                // its centre would float the pill a third of the
                                // way up the cover. Down here it sits on the
                                // solid end of the gradient, which is what gives
                                // the white its contrast.
                                Rectangle {
                                    id: playButton
                                    readonly property bool hovered:
                                        playArea.containsMouse && root.mouseInControl

                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 16
                                    width: 104; height: 32; radius: 16

                                    // Nothing to launch on a Played row whose
                                    // files are gone; the strip still reads fine
                                    // without it.
                                    visible: gameDelegate.launchable

                                    color: playButton.hovered ? Theme.playHover : Theme.accent
                                    Behavior on color { ColorAnimation { duration: 120 } }

                                    Row {
                                        anchors.centerIn: parent
                                        spacing: 7

                                        // Same asset as the details pages' PLAY.
                                        // Decoded at twice the drawn size so it
                                        // stays sharp on a scaled display.
                                        Image {
                                            anchors.verticalCenter: parent.verticalCenter
                                            source: playButton.hovered ? "assets/play_hover.png" : "assets/play.png"
                                            sourceSize.width: 20
                                            sourceSize.height: 20
                                            width: 10; height: 10
                                            fillMode: Image.PreserveAspectFit
                                            smooth: true
                                        }

                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: "PLAY"
                                            color: playButton.hovered ? Theme.textPrimary : Theme.textInverse
                                            font.pixelSize: 11
                                            font.bold: true
                                            font.letterSpacing: 1
                                        }
                                    }

                                    MouseArea {
                                        id: playArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: gameDelegate.play()
                                    }
                                }
                            }

                            scale: gameDelegate.highlighted ? 1.04 : 1.0
                            Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutQuart } }
                        }

                        Column {
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: 4

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: gameDelegate.liveName
                                color: gameDelegate.highlighted ? Theme.textPrimary : Theme.textBody
                                font.pixelSize: 15; font.weight: Font.DemiBold
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight; width: 220
                            }

                            // ── Caption ─────────────────────────────────────
                            //
                            // playtime · source · genre. This is where the two
                            // badges that used to sit on the artwork went. The
                            // covers are the point of the grid, and enough of
                            // them are missing in a real library that the
                            // caption often has to carry the card on its own.
                            Row {
                                anchors.horizontalCenter: parent.horizontalCenter
                                spacing: 7

                                // Still the exception it always was: only the
                                // Played tab holds games that are gone from the
                                // disk, so only it earns the marker.
                                Rectangle {
                                    visible: root.activeTab === "Played"
                                             && gameDelegate.modelData.installed === true
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 6; height: 6; radius: 3
                                    color: Theme.positive
                                }

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    // "0m" is what the bridge returns for a game
                                    // that was never launched. An em dash says
                                    // that; a zero reads like a measurement.
                                    text: gameDelegate.playtimeLabel === "0m"
                                          ? "—" : gameDelegate.playtimeLabel
                                    color: Theme.textMuted
                                    font.pixelSize: 11
                                }

                                Text {
                                    visible: gameDelegate.genreLabel !== ""
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "·"
                                    color: Theme.textGhost
                                    font.pixelSize: 11
                                }

                                Text {
                                    visible: gameDelegate.genreLabel !== ""
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: gameDelegate.genreLabel
                                    color: Theme.textMuted
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                    width: Math.min(implicitWidth, 90)
                                }
                            }
                        }
                    }

                    MouseArea {
                        id: cardArea
                        anchors.fill: parent; anchors.margins: 8; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            // Keep the pad's place in sync when both are in use.
                            if (root.padActive)
                                gameGrid.currentIndex = gameDelegate.index
                            // Favorites and Played share this grid with the
                            // library but open the Browse-style page.
                            if (root.activeTab === "Favorites" || root.activeTab === "Played") {
                                browseDetails.openForGame(gameDelegate.liveName, root.activeTab)
                                return
                            }
                            detailPopup.launchOrigin = "Library"
                            detailPopup.selectedGameName = gameDelegate.liveName
                            detailPopup.open()
                        }
                    }

                    // ── Overflow button ─────────────────────────────────────
                    //
                    // Declared AFTER cardArea, and as its sibling rather than a
                    // child of the art: cardArea fills the whole card, so
                    // anything earlier in the file sits under it and would never
                    // see the click.
                    //
                    // Anchored to the delegate rather than to the art it sits
                    // on: capsuleContainer is a child of the Column, so it is
                    // neither parent nor sibling here and anchoring to it is
                    // refused outright. The Column centres 360px of art plus a
                    // ~20px title in 400px, so the art starts 4px down -- 14
                    // puts the button 10px inside its top-right corner, and it
                    // stays put while the art scales under it on hover.
                    Rectangle {
                        id: overflowButton
                        readonly property bool hovered:
                            overflowArea.containsMouse && root.mouseInControl
                        // While its own menu is open the menu's first cell
                        // stands in for this button, so it steps aside rather
                        // than being caught in the menu's frosted backdrop as a
                        // smeared grey disc.
                        readonly property bool menuOpen:
                            tileMenu.visible
                            && tileMenu.gameName === gameDelegate.liveName

                        anchors {
                            top: parent.top
                            right: parent.right
                            topMargin: 8
                            rightMargin: 9
                        }
                        width: 32; height: 32; radius: 16
                        z: 2                     // above the lifted column

                        // Semi-opaque rather than solid: the cover art stays
                        // readable underneath it.
                        color: overflowButton.hovered ? Theme.overlayBadge : Theme.overlayButton
                        border.width: 1
                        border.color: overflowButton.hovered ? Theme.focusRing : Theme.borderStrong

                        visible: opacity > 0
                        opacity: (gameDelegate.highlighted && !overflowButton.menuOpen)
                                 ? 0.6 : 0.0
                        Behavior on opacity { NumberAnimation { duration: 150 } }
                        Behavior on color { ColorAnimation { duration: 120 } }

                        // Drawn rather than typed: the ⋮ glyph renders at a
                        // different weight and baseline in every font, and the
                        // rest of this UI has no font family set at all.
                        Column {
                            anchors.centerIn: parent
                            spacing: 3
                            Repeater {
                                model: 3
                                Rectangle {
                                    width: 4; height: 4; radius: 2
                                    color: overflowButton.hovered ? Theme.accent : Theme.bgDot
                                }
                            }
                        }

                        MouseArea {
                            id: overflowArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (root.padActive)
                                    gameGrid.currentIndex = gameDelegate.index
                                gameDelegate.openTileMenu()
                            }
                        }
                    }
                }
            }

            Item {
                id: recommendationPage

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 18

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Text {
                            text: "RECOMMENDATIONS"
                            color: Theme.textPrimary
                            font.pixelSize: 22
                            font.bold: true
                            font.letterSpacing: 2
                        }

                        // The one line in this tab that is always on screen.
                        //
                        // The sections below scroll, and with enough library
                        // picks the DISCOVER heading — and every explanation
                        // attached to it — sits past the fold, so a user with
                        // rejected credentials saw a working-looking library
                        // and no hint that anything was wrong. Whatever is
                        // actually blocking gets said here instead.
                        Text {
                            readonly property string blocker:
                                root.api ? root.api.authBlocker : ""
                            readonly property bool downloadingCatalog:
                                root.api ? root.api.catalogRefreshing : false

                            text: {
                                if (!root.api) return "Recommendations not loaded"
                                if (downloadingCatalog) {
                                    const n = root.api.catalogFetched
                                    // Discover already has picks: this is the
                                    // periodic refresh, not first-time setup,
                                    // and the current picks stay until it ends.
                                    const list = root.api.recommendationList || []
                                    const refreshing = list.some(function (item) {
                                        return item.section === "discover"
                                    })
                                    return (refreshing ? "Updating catalog in the background"
                                                       : root.api.catalogPhase)
                                         + (n > 0 ? " — " + n + " games downloaded"
                                                  : refreshing ? ""
                                                               : " — this runs once, a few minutes")
                                }
                                if (blocker !== "") return blocker
                                return root.api.recommendationStatus
                            }

                            // Amber for a real blocker, so it reads as
                            // something to act on rather than as status noise.
                            color: (blocker !== "" && !downloadingCatalog) ? Theme.caution : Theme.textMuted
                            font.pixelSize: 13
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    // Two independently ranked lists. Ranked together, discovery
                    // wins every slot on numbers alone — the IGDB catalog is
                    // ~200x larger than the unplayed library — and games the
                    // user could actually launch right now disappear.
                    Flickable {
                        id: recommendationFlick
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        contentHeight: recommendationSections.height
                        ScrollBar.vertical: VortexScrollBar { }

                        Column {
                            id: recommendationSections
                            width: parent.width
                            spacing: 8

                            Repeater {
                                model: [
                                    { key: "library",  title: "FROM YOUR LIBRARY", blurb: "Installed and ready to play" },
                                    { key: "discover", title: "DISCOVER",          blurb: "Not in your library yet" }
                                ]

                                delegate: Column {
                                    id: sectionColumn
                                    required property var modelData
                                    width: recommendationSections.width
                                    spacing: 6

                                    readonly property var picks: {
                                        if (!root.api || !root.api.recommendationList) return []
                                        return root.api.recommendationList.filter(function (item) {
                                            // Fallback output carries no section; treat it as library.
                                            return (item.section || "library") === sectionColumn.modelData.key
                                        })
                                    }

                                    // Discover keeps its heading when empty and
                                    // explains itself instead. Hiding it made
                                    // "the catalog was never downloaded" look
                                    // identical to "this feature does not
                                    // exist", which is how it went unnoticed.
                                    readonly property bool isDiscover:
                                        sectionColumn.modelData.key === "discover"
                                    readonly property bool igdbUsable: root.igdbWorking

                                    visible: sectionColumn.picks.length > 0
                                             || (sectionColumn.isDiscover
                                                 && root.api
                                                 && !root.api.isRecommendationLoading)

                                    Row {
                                        spacing: 10
                                        Text {
                                            text: sectionColumn.modelData.title
                                            color: Theme.textSecondary
                                            font.pixelSize: 14
                                            font.bold: true
                                            font.letterSpacing: 2
                                        }
                                        Text {
                                            text: sectionColumn.modelData.blurb
                                            color: Theme.textFaint
                                            font.pixelSize: 12
                                            anchors.verticalCenter: parent.verticalCenter
                                        }
                                    }

                                    // ── Discover empty / downloading state ──
                                    Item {
                                        id: discoverEmptyState
                                        width: parent.width
                                        height: visible ? 92 : 0
                                        visible: sectionColumn.isDiscover
                                                 && sectionColumn.picks.length === 0

                                        readonly property bool downloading:
                                            root.api ? root.api.catalogRefreshing : false

                                        Column {
                                            anchors.verticalCenter: parent.verticalCenter
                                            spacing: 8

                                            Row {
                                                spacing: 12
                                                visible: discoverEmptyState.downloading

                                                BusyIndicator {
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    running: parent.visible
                                                    implicitWidth: 22
                                                    implicitHeight: 22
                                                }

                                                Column {
                                                    spacing: 3
                                                    Text {
                                                        text: root.api ? root.api.catalogPhase : ""
                                                        color: Theme.textSecondary
                                                        font.pixelSize: 13
                                                    }
                                                    Text {
                                                        // A running count, not a
                                                        // percentage: IGDB is paged
                                                        // by keyset until the pages
                                                        // run out, so the total is
                                                        // genuinely unknown until
                                                        // the end.
                                                        text: (root.api && root.api.catalogFetched > 0)
                                                              ? (root.api.catalogFetched
                                                                 + " games downloaded so far")
                                                              : "This runs once and takes a few minutes."
                                                        color: Theme.textFaint
                                                        font.pixelSize: 11
                                                    }
                                                }
                                            }

                                            Text {
                                                visible: !discoverEmptyState.downloading
                                                         && !sectionColumn.igdbUsable
                                                width: 620
                                                wrapMode: Text.WordWrap
                                                color: Theme.textFaint
                                                font.pixelSize: 12
                                                // Says which of the two it is
                                                // rather than guessing: keys
                                                // absent and keys refused need
                                                // different actions.
                                                text: root.api && root.api.authBlocker !== ""
                                                      ? root.api.authBlocker
                                                      : ("Discover suggests games you don't own yet. "
                                                         + "It needs free IGDB credentials — add them in "
                                                         + "Settings and the catalogue downloads itself.")
                                            }

                                            Text {
                                                visible: !discoverEmptyState.downloading
                                                         && sectionColumn.igdbUsable
                                                width: 620
                                                wrapMode: Text.WordWrap
                                                color: Theme.textFaint
                                                font.pixelSize: 12
                                                text: "Nothing to suggest yet. Play a few games so Vortex "
                                                    + "learns what you like, or refresh the catalogue "
                                                    + "from Settings."
                                            }
                                        }
                                    }

                                    GridView {
                                        id: sectionGrid
                                        objectName: sectionColumn.modelData.key
                                        width: parent.width
                                        // Sized to content: the outer Flickable
                                        // scrolls, so an inner scroll area would
                                        // fight it.
                                        height: Math.ceil(sectionColumn.picks.length
                                                          / sectionGrid.columns) * sectionGrid.cellHeight
                                        interactive: false
                                        // Same art size and column pitch as the
                                        // library grid -- including its elastic
                                        // gutters, so the two tabs line up at
                                        // any width. The card is 448 tall (360
                                        // art + 88 caption) inside a 488 cell,
                                        // which is the library's 400-in-440 with
                                        // the two extra caption lines added to
                                        // both numbers.
                                        readonly property int minCellWidth: 280
                                        readonly property int columns:
                                            Math.max(1, Math.floor(sectionGrid.width
                                                                   / sectionGrid.minCellWidth))

                                        cellWidth: sectionGrid.width > 0
                                                   ? Math.floor(sectionGrid.width / sectionGrid.columns)
                                                   : sectionGrid.minCellWidth
                                        cellHeight: 488
                                        model: sectionColumn.picks

                                        Component.onCompleted: {
                                            if (sectionColumn.modelData.key === "library")
                                                root.libraryPickGridRef = sectionGrid
                                            else
                                                root.discoverGridRef = sectionGrid
                                        }

                                        currentIndex: -1
                                        onCountChanged: {
                                            if (!root.padActive || sectionGrid.count === 0)
                                                sectionGrid.currentIndex = -1
                                            else if (sectionGrid.currentIndex < 0)
                                                sectionGrid.currentIndex = 0
                                        }

                                        delegate: Item {
                                            id: recommendationDelegate
                                            required property var modelData
                                            required property int index

                                            readonly property bool isFocusedSection:
                                                root.recFocus === sectionColumn.modelData.key
                                            readonly property bool highlighted:
                                                (recommendationArea.containsMouse && root.mouseInControl)
                                                || (recommendationDelegate.GridView.isCurrentItem
                                                    && recommendationDelegate.isFocusedSection
                                                    && root.padInControl)

                                            // 256 for the hover scale's
                                            // overhang, as in the library grid
                                            // -- which also keeps the two tabs'
                                            // columns lined up.
                                            width: 256
                                            height: 448

                                            // The tab pop-in, as on the library
                                            // grid. Both sections sit in one
                                            // Flickable and their grids never
                                            // scroll, so the beat is counted
                                            // from where the card is on screen
                                            // rather than from its index.
                                            transform: Translate { id: recommendationEnterShift }
                                            function popIn() {
                                                const shownY = sectionColumn.y + sectionGrid.y
                                                               + recommendationDelegate.y
                                                               - recommendationFlick.contentY
                                                const slot = Math.floor(shownY / sectionGrid.cellHeight)
                                                             * sectionGrid.columns
                                                             + recommendationDelegate.index % sectionGrid.columns
                                                recommendationEnterPause.duration =
                                                    Math.max(0, Math.min(slot, 12)) * 40
                                                recommendationEnterAnim.stop()
                                                recommendationDelegate.opacity = 0
                                                recommendationEnterShift.y = 18
                                                recommendationEnterAnim.start()
                                            }
                                            Component.onCompleted: {
                                                if (Date.now() - root.popStamp <= 250)
                                                    recommendationDelegate.popIn()
                                            }
                                            Connections {
                                                target: root
                                                function onPopStampChanged() {
                                                    if (root.activeTab === "Recommendations")
                                                        recommendationDelegate.popIn()
                                                }
                                            }
                                            SequentialAnimation {
                                                id: recommendationEnterAnim
                                                PauseAnimation { id: recommendationEnterPause }
                                                ParallelAnimation {
                                                    NumberAnimation {
                                                        target: recommendationDelegate; property: "opacity"
                                                        to: 1.0; duration: 380; easing.type: Easing.OutCubic
                                                    }
                                                    NumberAnimation {
                                                        target: recommendationEnterShift; property: "y"
                                                        to: 0; duration: 380; easing.type: Easing.OutCubic
                                                    }
                                                }
                                            }

                                            // Top-anchored, not centred: the
                                            // reason line is one or two lines
                                            // depending on the title, and
                                            // centring pushed the art of the
                                            // two-line cards up out of line
                                            // with its neighbours.
                                            CoverGlow {
                                                target: recommendationCard
                                                stage: glowStage
                                                source: recommendationCover
                                                lit: recommendationDelegate.highlighted
                                            }

                                            Column {
                                                anchors.top: parent.top
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                spacing: 0

                                                Rectangle {
                                                    id: recommendationCard
                                                    width: 240
                                                    height: 360
                                                    radius: 12
                                                    color: Theme.bgSurface
                                                    border.width: 2
                                                    border.color: recommendationDelegate.highlighted ? Theme.focusRing : Theme.borderMuted
                                                    clip: true

                                                    Image {
                                                        id: recommendationCover
                                                        anchors.fill: parent
                                                        source: recommendationDelegate.modelData.coverPath || ""
                                                        fillMode: Image.PreserveAspectCrop
                                                        opacity: status === Image.Ready ? 1.0 : 0.0
                                                        Behavior on opacity { NumberAnimation { duration: 250 } }
                                                    }

                                                    // Already-saved marker. The wishlist never affects
                                                    // ranking, so a saved game keeps appearing here —
                                                    // this just shows you already have it.
                                                    Rectangle {
                                                        visible: root.api
                                                                 && !recommendationDelegate.modelData.matched
                                                                 && root.api.isWishlisted(recommendationDelegate.modelData.name)
                                                        anchors.right: parent.right
                                                        anchors.top: parent.top
                                                        anchors.margins: 10
                                                        width: 30; height: 30; radius: 15
                                                        color: Theme.overlayStrong
                                                        border.color: Theme.borderControl
                                                        Text {
                                                            anchors.centerIn: parent
                                                            text: "\u{1F6D2}"
                                                            font.pixelSize: 14
                                                        }
                                                    }

                                                    Column {
                                                        anchors.centerIn: parent
                                                        spacing: 8
                                                        visible: recommendationCover.status !== Image.Ready

                                                        Text {
                                                            anchors.horizontalCenter: parent.horizontalCenter
                                                            text: "ML PICK"
                                                            color: Theme.textGhost
                                                            font.pixelSize: 18
                                                            font.bold: true
                                                            font.letterSpacing: 2
                                                        }

                                                        Text {
                                                            anchors.horizontalCenter: parent.horizontalCenter
                                                            width: 200
                                                            text: recommendationDelegate.modelData.matched ? "NO ART" : "NOT IN LIBRARY"
                                                            color: Theme.textFaint
                                                            font.pixelSize: 12
                                                            horizontalAlignment: Text.AlignHCenter
                                                        }
                                                    }

                                                    scale: recommendationDelegate.highlighted ? 1.04 : 1.0
                                                    Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutQuart } }
                                                }

                                                // ── Caption ──
                                                //
                                                // One fixed-height block with a
                                                // reserved slot per line, so a
                                                // card's geometry never depends
                                                // on how long its title is or
                                                // whether it has a match figure.
                                                // Letting the text size itself
                                                // is what made the grid ragged:
                                                // a two-line reason pushed the
                                                // lines below it down, and a
                                                // missing match line collapsed
                                                // the slot altogether, so no two
                                                // captions shared a baseline.
                                                Item {
                                                    width: 240
                                                    height: 88

                                                    Text {
                                                        id: recommendationTitle
                                                        anchors.top: parent.top
                                                        anchors.topMargin: 12
                                                        anchors.horizontalCenter: parent.horizontalCenter
                                                        width: 220
                                                        height: 20
                                                        text: recommendationDelegate.modelData.name
                                                        color: recommendationDelegate.highlighted ? Theme.textPrimary : Theme.textBody
                                                        font.pixelSize: 15
                                                        font.weight: Font.DemiBold
                                                        horizontalAlignment: Text.AlignHCenter
                                                        verticalAlignment: Text.AlignVCenter
                                                        elide: Text.ElideRight
                                                    }

                                                    // Replaces the score badge. MMR reorders picks for
                                                    // variety, so the numbers were not monotonically
                                                    // descending and read as a bug; the reason is what
                                                    // the player can actually act on.
                                                    //
                                                    // Two lines are always reserved, at a fixed line
                                                    // height so the box is exactly 2 × 15 whatever the
                                                    // font metrics do, and the text is centred inside
                                                    // them — a one-line reason then sits optically
                                                    // centred instead of hugging the top of a visibly
                                                    // empty box.
                                                    Text {
                                                        id: recommendationReason
                                                        anchors.top: recommendationTitle.bottom
                                                        anchors.topMargin: 6
                                                        anchors.horizontalCenter: parent.horizontalCenter
                                                        width: 220
                                                        height: 30
                                                        text: recommendationDelegate.modelData.reason || ""
                                                        color: Theme.textFaint
                                                        font.pixelSize: 11
                                                        lineHeight: 15
                                                        lineHeightMode: Text.FixedHeight
                                                        horizontalAlignment: Text.AlignHCenter
                                                        verticalAlignment: Text.AlignVCenter
                                                        wrapMode: Text.WordWrap
                                                        maximumLineCount: 2
                                                        elide: Text.ElideRight
                                                    }

                                                    // How strong the link above
                                                    // actually is.
                                                    //
                                                    // The reason line names one of
                                                    // your games; on its own that
                                                    // is an assertion. recommend.py
                                                    // already computes the cosine
                                                    // behind it (inspiredBy, from
                                                    // scoring.inspiration_sources)
                                                    // and used to throw it away, so
                                                    // showing the top match costs
                                                    // nothing and turns the claim
                                                    // into something checkable.
                                                    // Click the card for the full
                                                    // per-mood breakdown in the
                                                    // console.
                                                    //
                                                    // Fades out rather than hiding:
                                                    // visible:false would give the
                                                    // cards without one a shorter
                                                    // caption than their neighbours.
                                                    Text {
                                                        readonly property var topMatch: {
                                                            const list = recommendationDelegate.modelData.inspiredBy
                                                            return (list && list.length > 0) ? list[0] : null
                                                        }

                                                        anchors.top: recommendationReason.bottom
                                                        anchors.topMargin: 6
                                                        anchors.horizontalCenter: parent.horizontalCenter
                                                        height: 14
                                                        opacity: (topMatch !== null
                                                                  && recommendationDelegate.modelData.reason
                                                                  && String(recommendationDelegate.modelData.reason).indexOf("Because you") === 0)
                                                                 ? 1.0 : 0.0
                                                        text: topMatch
                                                              ? (Math.round(topMatch.similarity * 100) + "% match")
                                                              : ""
                                                        color: Theme.textGhost
                                                        font.pixelSize: 10
                                                        font.letterSpacing: 0.5
                                                        verticalAlignment: Text.AlignVCenter
                                                    }
                                                }
                                            }

                                            MouseArea {
                                                id: recommendationArea
                                                anchors.fill: parent
                                                anchors.leftMargin: 8; anchors.rightMargin: 8
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                // Opens the Browse-style page, owned or
                                                // not: it resolves the pick by name.
                                                onClicked: {
                                                    if (root.padActive) {
                                                        root.recFocus = sectionColumn.modelData.key
                                                        sectionGrid.currentIndex = recommendationDelegate.index
                                                    }
                                                    if (root.api)
                                                        root.api.logRecommendationClick(
                                                            recommendationDelegate.modelData.name,
                                                            recommendationDelegate.index + 1)
                                                    browseDetails.openForGame(
                                                        recommendationDelegate.modelData.name,
                                                        "Recommendations")
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        visible: root.api && !root.api.isRecommendationLoading
                                 && root.api.recommendationList
                                 && root.api.recommendationList.length === 0
                        text: "NO RECOMMENDATIONS YET"
                        color: Theme.textGhost
                        font.pixelSize: 18
                        font.bold: true
                        font.letterSpacing: 2
                    }
                }
            }

            // ── Wishlist ────────────────────────────────────────────────────
            // Renders entirely from wishlist.json, which stores a metadata
            // snapshot per entry. Wishlisted games are unowned, so there is no
            // library row to read — and this way the tab still works with
            // Postgres stopped and no network.
            Item {
                GridView {
                    id: wishlistGrid
                    anchors.fill: parent
                    clip: true
                    // Elastic gutters, as on the library grid above.
                    readonly property int minCellWidth: 280
                    readonly property int columns:
                        Math.max(1, Math.floor(wishlistGrid.width / wishlistGrid.minCellWidth))

                    cellWidth: wishlistGrid.width > 0
                               ? Math.floor(wishlistGrid.width / wishlistGrid.columns)
                               : wishlistGrid.minCellWidth
                    cellHeight: 440
                    model: {
                        const list = root.api && root.api.wishlistGames ? root.api.wishlistGames : []
                        if (root.searchText.trim() === "")
                            return list
                        return list.filter(function(game) {
                            return root.matchesSearch([game.name])
                        })
                    }

                    ScrollBar.vertical: VortexScrollBar { }

                    currentIndex: -1

                    // Same as gameGrid.restoreY, for the same reason: removing
                    // a game from the wishlist rebuilds wishlistGames.
                    property real restoreY: -1

                    function restoreScroll() {
                        if (wishlistGrid.restoreY < 0)
                            return
                        wishlistGrid.contentY = Math.max(0, Math.min(
                            wishlistGrid.restoreY,
                            wishlistGrid.contentHeight - wishlistGrid.height))
                    }

                    onContentHeightChanged: wishlistGrid.restoreScroll()

                    onCountChanged: {
                        wishlistGrid.restoreScroll()

                        if (!root.padActive || wishlistGrid.count === 0)
                            wishlistGrid.currentIndex = -1
                        else if (wishlistGrid.currentIndex < 0)
                            wishlistGrid.currentIndex = 0
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: wishlistGrid.count === 0 && root.searchText.trim() !== ""
                        text: "NO MATCHES FOR \u201C" + root.searchText.trim() + "\u201D"
                        width: wishlistGrid.width - 80
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideMiddle
                        color: Theme.textGhost
                        font.pixelSize: 16
                        font.bold: true
                        font.letterSpacing: 2
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: wishlistGrid.count === 0 && root.searchText.trim() === ""
                        text: "NOTHING WISHLISTED YET\nOpen a Discover pick and tap Add to Wishlist"
                        horizontalAlignment: Text.AlignHCenter
                        color: Theme.textGhost
                        font.pixelSize: 16
                        font.bold: true
                        font.letterSpacing: 2
                    }

                    delegate: Item {
                        id: wishlistDelegate
                        required property var modelData
                        required property int index

                        readonly property bool highlighted:
                            (wishlistArea.containsMouse && root.mouseInControl)
                            || (wishlistDelegate.GridView.isCurrentItem && root.padInControl)

                        // Same card as the library grid: 240x360 art in a
                        // 240x415 cell, so switching tabs doesn't resize them.
                        // Padded to 256x431 for the hover scale, as there.
                        width: 256
                        height: 431

                        // The search and tab pop-in, as on the library grid above.
                        transform: Translate { id: wishlistEnterShift }
                        function popIn() {
                            const firstShown = Math.floor(wishlistGrid.contentY / wishlistGrid.cellHeight)
                                               * wishlistGrid.columns
                            wishlistEnterPause.duration =
                                Math.max(0, Math.min(wishlistDelegate.index - firstShown, 12)) * 40
                            wishlistEnterAnim.stop()
                            wishlistDelegate.opacity = 0
                            wishlistEnterShift.y = 18
                            wishlistEnterAnim.start()
                        }
                        Component.onCompleted: {
                            if (Date.now() - root.popStamp <= 250)
                                wishlistDelegate.popIn()
                        }
                        Connections {
                            target: root
                            function onPopStampChanged() {
                                if (root.activeTab === "Wishlist")
                                    wishlistDelegate.popIn()
                            }
                        }
                        SequentialAnimation {
                            id: wishlistEnterAnim
                            PauseAnimation { id: wishlistEnterPause }
                            ParallelAnimation {
                                NumberAnimation {
                                    target: wishlistDelegate; property: "opacity"
                                    to: 1.0; duration: 380; easing.type: Easing.OutCubic
                                }
                                NumberAnimation {
                                    target: wishlistEnterShift; property: "y"
                                    to: 0; duration: 380; easing.type: Easing.OutCubic
                                }
                            }
                        }

                        CoverGlow {
                            target: wishlistCard
                            stage: glowStage
                            source: wishlistCover
                            lit: wishlistDelegate.highlighted
                        }

                        Column {
                            anchors.centerIn: parent
                            spacing: 12

                            Rectangle {
                                id: wishlistCard
                                width: 240
                                height: 360
                                radius: 12
                                color: Theme.bgSurface
                                border.width: 2
                                border.color: wishlistDelegate.highlighted ? Theme.focusRing : Theme.borderMuted
                                clip: true

                                Image {
                                    id: wishlistCover
                                    anchors.fill: parent
                                    source: wishlistDelegate.modelData.coverPath || ""
                                    fillMode: Image.PreserveAspectCrop
                                    opacity: status === Image.Ready ? 1.0 : 0.0
                                    Behavior on opacity { NumberAnimation { duration: 250 } }
                                }

                                Text {
                                    anchors.centerIn: parent
                                    visible: wishlistCover.status !== Image.Ready
                                    text: "NOT IN LIBRARY"
                                    color: Theme.textFaint
                                    font.pixelSize: 12
                                }

                                scale: wishlistDelegate.highlighted ? 1.04 : 1.0
                                Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutQuart } }
                            }

                            Column {
                                anchors.horizontalCenter: parent.horizontalCenter
                                spacing: 4

                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: 220
                                    text: wishlistDelegate.modelData.name
                                    color: wishlistDelegate.highlighted ? Theme.textPrimary : Theme.textBody
                                    font.pixelSize: 15
                                    font.weight: Font.DemiBold
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
                                }

                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: 220
                                    text: wishlistDelegate.modelData.developer || ""
                                    color: Theme.textFaint
                                    font.pixelSize: 11
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        MouseArea {
                            id: wishlistArea
                            anchors.fill: parent
                            anchors.margins: 8
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (root.padActive)
                                    wishlistGrid.currentIndex = wishlistDelegate.index
                                browseDetails.openForGame(wishlistDelegate.modelData.name, "Wishlist")
                            }
                        }
                    }
                }
            }

            // ── Browse ──────────────────────────────────────────────────────
            // The whole IGDB catalog rather than the user's own lists. See
            // BrowsePage.qml; results open BrowseDetails, not GameDetails.
            BrowsePage {
                id: browsePage
                igdbConfigured: root.igdbConfigured
                padActive: root.padActive
                padInControl: root.padInControl
                glowStage: glowStage
                onOpenRequested: (item) => browseDetails.openFor(item)
                onSettingsRequested: settingsWindow.open()
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Mood selection overlay — shown on startup, dismissed after mood chosen
    // ─────────────────────────────────────────────────────────────────────────
    Rectangle {
        id: moodOverlay
        anchors.fill: parent
        color: Theme.bgWindow
        visible: true
        z: 10

        // Single entry point for both the mouse and the pad — this screen is
        // the first thing shown, so the controller has to get past it too.
        function chooseMood(index) {
            moodOverlay.visible = false
            root.focusedMood = -1
            root.api.initialize(index)
        }

        // Subtle background gradient
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0.0; color: Theme.bgSunken }
                GradientStop { position: 1.0; color: Theme.bgWindow }
            }
        }

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 60

            // Title block
            ColumnLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 10

                Text {
                    text: "VORTEX"
                    color: Theme.textPrimary
                    font.pixelSize: 72; font.bold: true; font.letterSpacing: 8
                    Layout.alignment: Qt.AlignHCenter
                }
                Text {
                    text: "select your mood to begin"
                    color: Theme.textFaint
                    font.pixelSize: 16; font.letterSpacing: 3
                    Layout.alignment: Qt.AlignHCenter
                }
            }

            // Mood cards
            Row {
                spacing: 30
                Layout.alignment: Qt.AlignHCenter

                // Neutral card — first, and the default. Applies no mood
                // weighting at all; recommendations come purely from play
                // history, playtime and favourites.
                MoodCard {
                    moodIndex: 3
                    label: "Neutral"
                    icon: "🎯"
                    description: "just my history\nno mood filter"
                    accentColor: Theme.moodNeutral
                    onSelected: function(idx) { moodOverlay.chooseMood(idx) }
                }

                // Relaxed card
                MoodCard {
                    moodIndex: 0
                    label: "Relaxed"
                    icon: "🌿"
                    description: "casual play\nno pressure"
                    accentColor: Theme.moodRelaxed
                    onSelected: function(idx) { moodOverlay.chooseMood(idx) }
                }

                // Competitive card
                MoodCard {
                    moodIndex: 1
                    label: "Competitive"
                    icon: "⚔️"
                    description: "ranked matches\nhigh performance"
                    accentColor: Theme.moodCompetitive
                    onSelected: function(idx) { moodOverlay.chooseMood(idx) }
                }

                // Immersive card
                MoodCard {
                    moodIndex: 2
                    label: "Immersive"
                    icon: "🌌"
                    description: "story & exploration\nlose yourself"
                    accentColor: Theme.moodImmersive
                    onSelected: function(idx) { moodOverlay.chooseMood(idx) }
                }
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // MoodCard component (inline)
    // ─────────────────────────────────────────────────────────────────────────
    component MoodCard: Rectangle {
        id: moodCard

        required property int    moodIndex
        required property string label
        required property string icon
        required property string description
        required property color  accentColor

        signal selected(int idx)

        readonly property bool highlighted:
            (moodHover.containsMouse && root.mouseInControl)
            || (moodCard.moodIndex === root.focusedMood && root.padInControl)

        width: 240; height: 320; radius: 16
        color: moodCard.highlighted ? Qt.rgba(
            Qt.color(accentColor).r,
            Qt.color(accentColor).g,
            Qt.color(accentColor).b, 0.12) : Theme.bgPanel
        border.color: moodCard.highlighted ? accentColor : Theme.borderQuiet
        border.width: moodCard.highlighted ? 2 : 1

        Behavior on color  { ColorAnimation { duration: 200 } }
        Behavior on border.color { ColorAnimation { duration: 200 } }

        scale: moodCard.highlighted ? 1.04 : 1.0
        Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutQuart } }

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 20

            Text {
                text: moodCard.icon
                font.pixelSize: 52
                Layout.alignment: Qt.AlignHCenter
            }

            Text {
                text: moodCard.label.toUpperCase()
                color: moodCard.highlighted ? moodCard.accentColor : Theme.textPrimary
                font.pixelSize: 22; font.bold: true; font.letterSpacing: 2
                Layout.alignment: Qt.AlignHCenter
                Behavior on color { ColorAnimation { duration: 200 } }
            }

            Text {
                text: moodCard.description
                color: Theme.textFaint
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                Layout.alignment: Qt.AlignHCenter
            }
        }

        MouseArea {
            id: moodHover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: moodCard.selected(moodCard.moodIndex)
        }
    }
}
