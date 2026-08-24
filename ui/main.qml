pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Window {
    id: root
    width: 1920; height: 1080; visible: true; title: "Vortex Launcher"; color: "black"

    property string activeFilter: "All"
    property string activeTab: "Library"
    property var api: vortexApi

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
    }

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
        // Library and Favorites are the same grid with a different model.
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

        // Same column count GridView itself lays out with.
        const columns = Math.max(1, Math.floor(grid.width / grid.cellWidth))
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

        // Unowned picks open too — the details page falls back to
        // recommendationList and shows Check on Steam instead of Play.
        detailPopup.launchOrigin = root.activeTab
        detailPopup.focusedAction = 0
        // liveName is the library delegate's resolved title; the Discover
        // grid has no such property and its names never change under it.
        detailPopup.selectedGameName =
            (grid.currentItem && grid.currentItem.liveName)
                ? grid.currentItem.liveName
                : game.name
        detailPopup.open()
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
    // between Library and Recommendations; with four tabs a toggle would leave
    // Favorites and Wishlist unreachable from the pad entirely.
    readonly property var tabs: ["Library", "Recommendations", "Favorites", "Wishlist"]

    function toggleRecommendations() {
        root.padActive = true
        if (root.padBlocked || detailPopup.visible || moodOverlay.visible)
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
    }

    GameDetails {
        id: detailPopup
        onClosed: detailPopup.focusedAction = -1
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

    // Shared by the top-bar "+ FOLDER" shortcut and the settings panel.
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
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 40
        spacing: 30

        // Top navigation + library filters
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 35
            color: "black"
            z: 10

            RowLayout {
                anchors.fill: parent
                spacing: 15

            Row {
                spacing: 10
                Layout.alignment: Qt.AlignLeft

                Repeater {
                    model: ["Library", "Recommendations", "Favorites", "Wishlist"]
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
                        color: tabButton.active ? "white" : (tabButton.hovered ? "#2e2e2e" : "#1a1a1a")
                        border.color: tabButton.hovered ? "#555" : "#333"

                        Behavior on color { ColorAnimation { duration: 150 } }

                        Text {
                            id: tabLabel
                            anchors.centerIn: parent
                            text: tabButton.modelData.toUpperCase()
                            font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                            color: tabButton.active ? "black" : (tabButton.hovered ? "#ddd" : "#888")
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

            Row {
                spacing: 15
                Layout.alignment: Qt.AlignHCenter
                visible: root.activeTab === "Library"

                Repeater {
                    model: ["All", "Steam", "Local"]
                    delegate: Rectangle {
                        id: filterButton
                        required property string modelData

                        readonly property bool active: root.activeFilter === filterButton.modelData
                        readonly property bool hovered:
                            filterArea.containsMouse && root.mouseInControl && !filterButton.active

                        width: 120; height: 35; radius: 17
                        color: filterButton.active ? "white" : (filterButton.hovered ? "#2e2e2e" : "#1a1a1a")
                        border.color: filterButton.hovered ? "#555" : "#333"

                        Behavior on color { ColorAnimation { duration: 150 } }

                        Text {
                            anchors.centerIn: parent
                            text: filterButton.modelData.toUpperCase()
                            font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                            color: filterButton.active ? "black" : (filterButton.hovered ? "#ddd" : "#888")
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

            Rectangle {
                id: addFolderButton
                readonly property bool hovered: addFolderArea.containsMouse && root.mouseInControl

                implicitWidth: 150; implicitHeight: 35; radius: 17
                visible: root.activeTab === "Library"
                color: addFolderButton.hovered ? "#ffffff" : "#1a1a1a"
                border.color: addFolderButton.hovered ? "#ffffff" : "#333"

                Text {
                    anchors.centerIn: parent
                    text: "+ FOLDER"
                    font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                    color: addFolderButton.hovered ? "black" : "#888"
                }

                MouseArea {
                    id: addFolderArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: localFolderDialog.open()
                }
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
                    color: "#888"
                    font.pixelSize: 11
                    font.bold: true
                    font.letterSpacing: 1
                }

                // Determinate whenever a total is known; the first phase has no
                // count yet, so it shows a plain sweep rather than a bar
                // pinned at zero, which reads as stuck.
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 110; height: 3; radius: 2
                    color: "#252525"

                    Rectangle {
                        height: parent.height
                        radius: parent.radius
                        color: "white"
                        width: scanStrip.total > 0
                               ? parent.width * Math.min(1, scanStrip.done / scanStrip.total)
                               : parent.width * 0.25
                        Behavior on width { NumberAnimation { duration: 200 } }

                        SequentialAnimation on x {
                            running: scanStrip.visible && scanStrip.total === 0
                            loops: Animation.Infinite
                            NumberAnimation { from: 0; to: 82; duration: 900; easing.type: Easing.InOutQuad }
                            NumberAnimation { from: 82; to: 0; duration: 900; easing.type: Easing.InOutQuad }
                        }
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: scanStrip.total > 0
                    text: scanStrip.done + " / " + scanStrip.total
                    color: "#555"
                    font.pixelSize: 11
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
                    color: resetLikesButton.hovered ? "#ffffff" : "#1a1a1a"
                    border.color: resetLikesButton.hovered ? "#ffffff" : "#333"

                    Behavior on color { ColorAnimation { duration: 150 } }

                    Text {
                        anchors.centerIn: parent
                        text: "RESET LIKES"
                        font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                        color: resetLikesButton.hovered ? "black" : "#888"
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
                    color: confirmResetArea.containsMouse ? "#e74c3c" : "#c0392b"

                    Text {
                        anchors.centerIn: parent
                        text: "CLEAR ALL LIKES"
                        color: "white"; font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
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
                    color: cancelResetArea.containsMouse ? "#2b2b2b" : "transparent"
                    border.color: "#3a3a3a"; border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: "CANCEL"
                        color: "#aaa"; font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
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
                color: settingsButton.hovered ? "#ffffff" : "#1a1a1a"
                border.color: settingsButton.hovered ? "#ffffff" : "#333"

                Text {
                    anchors.centerIn: parent
                    text: "⚙"
                    font.pixelSize: 17
                    color: settingsButton.hovered ? "black" : "#888"
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
                color: refreshButton.hovered ? "#ffffff" : "#1a1a1a"
                border.color: refreshButton.hovered ? "#ffffff" : "#333"

                Text {
                    anchors.centerIn: parent
                    text: "REFRESH"
                    font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                    color: refreshButton.hovered ? "black" : "#888"
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
            // Library and Favorites share page 0 — same cards, different model.
            currentIndex: root.activeTab === "Recommendations" ? 1
                        : root.activeTab === "Wishlist" ? 2 : 0

            // Game grid
            GridView {
                id: gameGrid
                clip: true
                cellWidth: 280; cellHeight: 440

                ScrollBar.vertical: VortexScrollBar { }

                // No current item until the pad asks for one. A model reload
                // (first scan, filter change) puts it back to -1 for the mouse,
                // and back onto the first card once the pad is in use.
                currentIndex: -1
                Text {
                    anchors.centerIn: parent
                    visible: gameGrid.count === 0 && root.activeTab === "Favorites"
                    text: "NO FAVORITES YET\nOpen a game and tap the heart"
                    horizontalAlignment: Text.AlignHCenter
                    color: "#444"
                    font.pixelSize: 16
                    font.bold: true
                    font.letterSpacing: 2
                }

                onCountChanged: {
                    if (!root.padActive || gameGrid.count === 0)
                        gameGrid.currentIndex = -1
                    else if (gameGrid.currentIndex < 0)
                        gameGrid.currentIndex = 0
                }

                // Favorites is the library grid with a different model rather
                // than a second copy of the card markup.
                model: {
                    if (!root.api) return [];
                    if (root.activeTab === "Favorites")
                        return root.api.favoriteGames || [];
                    if (!root.api.gameList) return [];
                    if (root.activeFilter === "All") return root.api.gameList;
                    return root.api.gameList.filter(function(game) {
                        return game.source === root.activeFilter;
                    });
                }

                delegate: Item {
                    id: gameDelegate
                    required property var modelData
                    required property int index

                    // Mouse hover and controller focus light the card the same
                    // way, but only whichever input is currently driving.
                    readonly property bool highlighted:
                        (cardArea.containsMouse && root.mouseInControl)
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

                    width: 240; height: 400

                    Column {
                        anchors.centerIn: parent
                        spacing: 12

                        Rectangle {
                            id: capsuleContainer
                            width: 240; height: 360
                            color: "#1a1a1a"; radius: 12
                            border.color: gameDelegate.highlighted ? "white" : "#333"
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
                                Text { text: "NO ART"; color: "#333"; font.bold: true }
                            }

                            scale: gameDelegate.highlighted ? 1.04 : 1.0
                            Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutQuart } }
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: gameDelegate.liveName
                            color: gameDelegate.highlighted ? "white" : "#ccc"
                            font.pixelSize: 15; font.weight: Font.DemiBold
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight; width: 220
                        }
                    }

                    MouseArea {
                        id: cardArea
                        anchors.fill: parent; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            // Keep the pad's place in sync when both are in use.
                            if (root.padActive)
                                gameGrid.currentIndex = gameDelegate.index
                            detailPopup.launchOrigin = root.activeTab === "Favorites" ? "Favorites" : "Library"
                            detailPopup.selectedGameName = gameDelegate.liveName
                            detailPopup.open()
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
                            color: "white"
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
                                    return root.api.catalogPhase
                                         + (n > 0 ? " — " + n + " games downloaded"
                                                  : " — this runs once, a few minutes")
                                }
                                if (blocker !== "") return blocker
                                return root.api.recommendationStatus
                            }

                            // Amber for a real blocker, so it reads as
                            // something to act on rather than as status noise.
                            color: (blocker !== "" && !downloadingCatalog) ? "#e0a33a" : "#777"
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
                                            color: "#bbb"
                                            font.pixelSize: 14
                                            font.bold: true
                                            font.letterSpacing: 2
                                        }
                                        Text {
                                            text: sectionColumn.modelData.blurb
                                            color: "#555"
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
                                                        color: "#bbb"
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
                                                        color: "#555"
                                                        font.pixelSize: 11
                                                    }
                                                }
                                            }

                                            Text {
                                                visible: !discoverEmptyState.downloading
                                                         && !sectionColumn.igdbUsable
                                                width: 620
                                                wrapMode: Text.WordWrap
                                                color: "#666"
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
                                                color: "#666"
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
                                                          / Math.max(1, Math.floor(width / 280))) * 488
                                        interactive: false
                                        // Same art size and column pitch as the
                                        // library grid; the card is 448 tall
                                        // (360 art + 88 caption) inside a 488
                                        // cell, which is the library's 400-in-440
                                        // with the two extra caption lines added
                                        // to both numbers.
                                        cellWidth: 280
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

                                            width: 240
                                            height: 448

                                            // Top-anchored, not centred: the
                                            // reason line is one or two lines
                                            // depending on the title, and
                                            // centring pushed the art of the
                                            // two-line cards up out of line
                                            // with its neighbours.
                                            Column {
                                                anchors.top: parent.top
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                spacing: 0

                                                Rectangle {
                                                    id: recommendationCard
                                                    width: 240
                                                    height: 360
                                                    radius: 12
                                                    color: "#141414"
                                                    border.width: 2
                                                    border.color: recommendationDelegate.highlighted ? "white" : "#303030"
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
                                                        color: "#E6000000"
                                                        border.color: "#3a3a3a"
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
                                                            color: "#333"
                                                            font.pixelSize: 18
                                                            font.bold: true
                                                            font.letterSpacing: 2
                                                        }

                                                        Text {
                                                            anchors.horizontalCenter: parent.horizontalCenter
                                                            width: 200
                                                            text: recommendationDelegate.modelData.matched ? "NO ART" : "NOT IN LIBRARY"
                                                            color: "#555"
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
                                                        color: recommendationDelegate.highlighted ? "white" : "#ccc"
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
                                                        color: "#6f6f6f"
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
                                                        color: "#4a4a4a"
                                                        font.pixelSize: 10
                                                        font.letterSpacing: 0.5
                                                        verticalAlignment: Text.AlignVCenter
                                                    }
                                                }
                                            }

                                            MouseArea {
                                                id: recommendationArea
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                // Unowned picks open too: the details page
                                                // resolves them from recommendationList.
                                                onClicked: {
                                                    if (root.padActive) {
                                                        root.recFocus = sectionColumn.modelData.key
                                                        sectionGrid.currentIndex = recommendationDelegate.index
                                                    }
                                                    if (root.api)
                                                        root.api.logRecommendationClick(
                                                            recommendationDelegate.modelData.name,
                                                            recommendationDelegate.index + 1)
                                                    detailPopup.launchOrigin = "Recommendations"
                                                    detailPopup.selectedGameName = recommendationDelegate.modelData.name
                                                    detailPopup.open()
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
                        color: "#444"
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
                    cellWidth: 300
                    cellHeight: 430
                    model: root.api && root.api.wishlistGames ? root.api.wishlistGames : []

                    ScrollBar.vertical: VortexScrollBar { }

                    currentIndex: -1
                    onCountChanged: {
                        if (!root.padActive || wishlistGrid.count === 0)
                            wishlistGrid.currentIndex = -1
                        else if (wishlistGrid.currentIndex < 0)
                            wishlistGrid.currentIndex = 0
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: wishlistGrid.count === 0
                        text: "NOTHING WISHLISTED YET\nOpen a Discover pick and tap Add to Wishlist"
                        horizontalAlignment: Text.AlignHCenter
                        color: "#444"
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

                        width: 260
                        height: 400

                        Column {
                            anchors.centerIn: parent
                            spacing: 10

                            Rectangle {
                                width: 260
                                height: 320
                                radius: 12
                                color: "#141414"
                                border.width: 2
                                border.color: wishlistDelegate.highlighted ? "white" : "#303030"
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
                                    color: "#555"
                                    font.pixelSize: 12
                                }

                                scale: wishlistDelegate.highlighted ? 1.04 : 1.0
                                Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutQuart } }
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 240
                                text: wishlistDelegate.modelData.name
                                color: wishlistDelegate.highlighted ? "white" : "#ccc"
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 240
                                text: wishlistDelegate.modelData.developer || ""
                                color: "#6f6f6f"
                                font.pixelSize: 11
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            id: wishlistArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (root.padActive)
                                    wishlistGrid.currentIndex = wishlistDelegate.index
                                detailPopup.launchOrigin = "Wishlist"
                                detailPopup.selectedGameName = wishlistDelegate.modelData.name
                                detailPopup.open()
                            }
                        }
                    }
                }
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Mood selection overlay — shown on startup, dismissed after mood chosen
    // ─────────────────────────────────────────────────────────────────────────
    Rectangle {
        id: moodOverlay
        anchors.fill: parent
        color: "black"
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
                GradientStop { position: 0.0; color: "#0d0d0d" }
                GradientStop { position: 1.0; color: "#000000" }
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
                    color: "white"
                    font.pixelSize: 72; font.bold: true; font.letterSpacing: 8
                    Layout.alignment: Qt.AlignHCenter
                }
                Text {
                    text: "select your mood to begin"
                    color: "#555"
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
                    accentColor: "#7f8c8d"
                    onSelected: function(idx) { moodOverlay.chooseMood(idx) }
                }

                // Relaxed card
                MoodCard {
                    moodIndex: 0
                    label: "Relaxed"
                    icon: "🌿"
                    description: "casual play\nno pressure"
                    accentColor: "#27ae60"
                    onSelected: function(idx) { moodOverlay.chooseMood(idx) }
                }

                // Competitive card
                MoodCard {
                    moodIndex: 1
                    label: "Competitive"
                    icon: "⚔️"
                    description: "ranked matches\nhigh performance"
                    accentColor: "#e74c3c"
                    onSelected: function(idx) { moodOverlay.chooseMood(idx) }
                }

                // Immersive card
                MoodCard {
                    moodIndex: 2
                    label: "Immersive"
                    icon: "🌌"
                    description: "story & exploration\nlose yourself"
                    accentColor: "#9b59b6"
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
            Qt.color(accentColor).b, 0.12) : "#111111"
        border.color: moodCard.highlighted ? accentColor : "#222"
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
                color: moodCard.highlighted ? moodCard.accentColor : "white"
                font.pixelSize: 22; font.bold: true; font.letterSpacing: 2
                Layout.alignment: Qt.AlignHCenter
                Behavior on color { ColorAnimation { duration: 200 } }
            }

            Text {
                text: moodCard.description
                color: "#666"
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
