pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts

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

    // Back while armed drops the confirm row instead of the whole page; the
    // caller keeps the page open when this returns true.
    function handleBack() {
        if (!detailsRoot.uninstallArmed)
            return false
        detailsRoot.cancelUninstall()
        return true
    }

    function navigate(direction) {
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
        if (detailsRoot.focusedAction < 0) {
            detailsRoot.focusedAction = detailsRoot.actionPlay
            return
        }
        // Up/down hop straight between the action row and uninstall, which sits
        // in the opposite corner; left/right walk every stop in order.
        if (direction === "up" || direction === "down") {
            detailsRoot.focusedAction =
                detailsRoot.focusedAction === detailsRoot.actionUninstall
                    ? detailsRoot.actionPlay
                    : detailsRoot.actionUninstall
            return
        }
        const step = direction === "right" ? 1 : -1
        detailsRoot.focusedAction =
            (detailsRoot.focusedAction + step + detailsRoot.actionCount) % detailsRoot.actionCount
    }

    function activateFocusedAction() {
        if (!detailsRoot.gameData || !detailsRoot.api)
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
                detailsRoot.api.launchGameFrom(detailsRoot.gameData.name, detailsRoot.launchOrigin)
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
        return detailsRoot.findIn(detailsRoot.api.recommendationList)
            || detailsRoot.findIn(detailsRoot.api.wishlistGames)
            || detailsRoot.findIn(detailsRoot.api.favoriteGames);
    }

    Connections {
        target: detailsRoot.api
        function onGameListChanged() { detailsRoot.gameData = detailsRoot.findGameData(); }
        function onRecommendationListChanged() { detailsRoot.gameData = detailsRoot.findGameData(); }
        function onWishlistChanged() {
            detailsRoot.gameData = detailsRoot.findGameData();
            detailsRoot.wishlistRevision++;
        }
    }

    // Never carry an armed confirm across games or across an open/close cycle.
    onOpened: detailsRoot.cancelUninstall()

    onSelectedGameNameChanged: {
        detailsRoot.cancelUninstall();
        detailsRoot.gameData = findGameData();
        if (detailsRoot.gameData && detailsRoot.api) {
            detailsRoot.api.logGameClick(detailsRoot.gameData.name, detailsRoot.gameData.genres, detailsRoot.gameData.tags);
            // Unowned picks have no SteamGridDB art. Ask for a hero and logo
            // here rather than when the list loads: this is the only place they
            // are ever shown, and the bridge no-ops on anything already cached
            // or already known to have none. The Connections block above swaps
            // them in when they land, so the page updates without reopening.
            if (!detailsRoot.isOwned)
                detailsRoot.api.ensureArtwork(detailsRoot.gameData.name);
        }
    }

    width: parent.width * 0.9; height: parent.height * 0.9
    x: (parent.width - width) / 2; y: (parent.height - height) / 2
    modal: true; focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle { 
        color: "#121212"
        radius: 20 
        border.color: "#252525"
        border.width: 1 
    }

    contentItem: ColumnLayout {
        spacing: 0; anchors.fill: parent

        // --- TOP SECTION (Hero + Logo) ---
        Rectangle {
            id: heroBanner
            Layout.fillWidth: true; Layout.preferredHeight: parent.height * 0.5
            color: "#1a1a1a"
            radius: 20 
            // Fix 1: Layer + Clip ensures images follow the curved edges of the window
            layer.enabled: true
            clip: true 

            // 16:10 is the loosest thing anyone ships as a banner and the
            // tightest portrait cover is 3:4, so 1.6 separates the two with
            // room to spare. This stays false until the image reports a size,
            // so the banner starts blurred — the safe way round, since
            // un-blurring late is invisible and stretching early is the bug.
            readonly property bool heroIsWide:
                heroBackdrop.status === Image.Ready
                && heroBackdrop.implicitWidth > heroBackdrop.implicitHeight * 1.6

            // Drawn through the MultiEffect below, never directly — hiding the
            // source item is how MultiEffect is given something to work on.
            Image {
                id: heroBackdrop
                anchors.fill: parent
                visible: false
                asynchronous: true
                fillMode: Image.PreserveAspectCrop
                source: detailsRoot.heroSource
            }

            MultiEffect {
                anchors.fill: parent
                source: heroBackdrop
                // Real wide art is already the right shape and stays sharp at
                // the opacity this page has always used. Portrait art is
                // blurred instead and carries a little more of the frame, since
                // once blurred it is only there to be colour.
                blurEnabled: !heroBanner.heroIsWide
                blur: 1.0
                blurMax: 48
                opacity: heroBanner.heroIsWide ? 0.4 : 0.55
                Behavior on opacity { NumberAnimation { duration: 200 } }
            }

            // Centre art, in order of preference: the logo when there is one
            // (Steam's logo.png fills this slot for unowned picks now), else
            // the sharp portrait over its own blur, else nothing — real wide
            // art reads perfectly well on its own.
            Image {
                anchors.centerIn: parent
                width: parent.width * 0.4; height: parent.height * 0.4
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                visible: detailsRoot.logoSource !== ""
                source: detailsRoot.logoSource
            }

            Image {
                anchors.centerIn: parent
                height: parent.height * 0.8
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                visible: detailsRoot.logoSource === "" && !heroBanner.heroIsWide
                // heroSource first: when it is portrait it is IGDB's 540x720,
                // which downscales into this slot, where the 264x352 cover
                // would have to be stretched up to fill it.
                source: !visible ? ""
                      : (detailsRoot.heroSource !== "" ? detailsRoot.heroSource
                                                       : detailsRoot.coverSource)
            }
        }

        // --- BOTTOM SECTION ---
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true

            // UNINSTALL
            Rectangle {
                id: uninstallButton
                // Pad focus and mouse hover light every action the same way, so
                // the page looks identical whichever one you are holding.
                readonly property bool emphasized:
                    (detailsRoot.focusedAction === detailsRoot.actionUninstall
                     && detailsRoot.padInControl)
                    || (uninstallArea.containsMouse && detailsRoot.mouseInControl)

                anchors { top: parent.top; right: parent.right; margins: 25 }
                width: 50; height: 50; radius: 8
                // Uninstalling a game you don't own is meaningless; that slot
                // becomes Add to Wishlist instead.
                visible: !detailsRoot.uninstallArmed && detailsRoot.isOwned
                // Loud on purpose: a dark red outline on a dark button read as
                // "the selection disappeared" rather than "uninstall is picked".
                color: uninstallButton.emphasized ? "#4a1d1a" : "#222"
                border.width: uninstallButton.emphasized ? 3 : 0
                border.color: "white"
                scale: uninstallButton.emphasized ? 1.12 : 1.0

                Behavior on color { ColorAnimation { duration: 150 } }
                Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutQuart } }

                Text {
                    anchors.centerIn: parent; text: "🗑"
                    color: uninstallButton.emphasized ? "#ff6b5b" : "#c0392b"; font.pixelSize: 24
                }
                MouseArea {
                    id: uninstallArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: detailsRoot.requestUninstall()
                }
            }

            // ADD TO WISHLIST — unowned games only. Saving a game is purely a
            // bookmark: it never feeds or filters the recommender, so the pick
            // keeps appearing in Discover with a cart marker.
            Rectangle {
                id: wishlistButton
                readonly property bool saved:
                    !!detailsRoot.api && !!detailsRoot.gameData
                    && detailsRoot.wishlistRevision >= 0
                    && detailsRoot.api.isWishlisted(detailsRoot.gameData.name)
                readonly property bool emphasized:
                    (detailsRoot.focusedAction === detailsRoot.actionUninstall
                     && detailsRoot.padInControl)
                    || (wishlistArea.containsMouse && detailsRoot.mouseInControl)

                anchors { top: parent.top; right: parent.right; margins: 25 }
                height: 50; radius: 8
                width: wishlistRow.implicitWidth + 32
                visible: !detailsRoot.isOwned
                color: wishlistButton.saved ? "#1d4a2b" : (wishlistButton.emphasized ? "#2a2a2a" : "#222")
                border.width: wishlistButton.emphasized ? 3 : 0
                border.color: "white"
                scale: wishlistButton.emphasized ? 1.08 : 1.0

                Behavior on color { ColorAnimation { duration: 150 } }
                Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutQuart } }

                Row {
                    id: wishlistRow
                    anchors.centerIn: parent
                    spacing: 8
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "\u{1F6D2}"
                        font.pixelSize: 18
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: wishlistButton.saved ? "WISHLISTED" : "ADD TO WISHLIST"
                        color: "white"
                        font.pixelSize: 12
                        font.bold: true
                        font.letterSpacing: 1
                    }
                }

                MouseArea {
                    id: wishlistArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (!detailsRoot.gameData) return
                        detailsRoot.api.toggleWishlist(detailsRoot.gameData.name)
                        detailsRoot.wishlistRevision++
                    }
                }
            }

            // Confirm step — a local uninstall deletes the game's folder off the
            // disk, so it is never one click. Steam games skip this entirely.
            Row {
                anchors { top: parent.top; right: parent.right; margins: 25 }
                spacing: 12
                visible: detailsRoot.uninstallArmed

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "delete this game's files from disk?"
                    color: "#ff8a7a"; font.pixelSize: 16
                }

                Rectangle {
                    id: confirmDeleteButton
                    readonly property bool emphasized:
                        (detailsRoot.armedChoice === detailsRoot.armedDelete
                         && detailsRoot.padInControl)
                        || (confirmDeleteArea.containsMouse && detailsRoot.mouseInControl)

                    width: 120; height: 50; radius: 8
                    color: confirmDeleteButton.emphasized ? "#e74c3c" : "#c0392b"
                    border.width: confirmDeleteButton.emphasized ? 3 : 0
                    border.color: "white"
                    scale: confirmDeleteButton.emphasized ? 1.06 : 1.0

                    Behavior on color { ColorAnimation { duration: 150 } }
                    Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutQuart } }

                    Text {
                        anchors.centerIn: parent; text: "DELETE"
                        color: "white"; font.bold: true; font.pixelSize: 15; font.letterSpacing: 1
                    }
                    MouseArea {
                        id: confirmDeleteArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: detailsRoot.confirmUninstall()
                    }
                }

                Rectangle {
                    id: cancelDeleteButton
                    readonly property bool emphasized:
                        (detailsRoot.armedChoice === detailsRoot.armedCancel
                         && detailsRoot.padInControl)
                        || (cancelDeleteArea.containsMouse && detailsRoot.mouseInControl)

                    width: 120; height: 50; radius: 8
                    color: cancelDeleteButton.emphasized ? "#333" : "#222"
                    border.width: cancelDeleteButton.emphasized ? 3 : 1
                    border.color: cancelDeleteButton.emphasized ? "white" : "#3a3a3a"
                    scale: cancelDeleteButton.emphasized ? 1.06 : 1.0

                    Behavior on color { ColorAnimation { duration: 150 } }
                    Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutQuart } }

                    Text {
                        anchors.centerIn: parent; text: "CANCEL"
                        color: "#ddd"; font.bold: true; font.pixelSize: 15; font.letterSpacing: 1
                    }
                    MouseArea {
                        id: cancelDeleteArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: detailsRoot.cancelUninstall()
                    }
                }
            }

            Column {
                anchors { top: parent.top; left: parent.left; margins: 40 }
                spacing: 25

                Row {
                    spacing: 20
                    Button {
                        id: playBtn
                        // Button brings its own `hovered`, so the mouse needs no
                        // extra MouseArea here — just fold it into the same flag.
                        readonly property bool emphasized:
                            (detailsRoot.focusedAction === detailsRoot.actionPlay
                             && detailsRoot.padInControl)
                            || (playBtn.hovered && detailsRoot.mouseInControl)

                        hoverEnabled: true
                        scale: playBtn.emphasized ? 1.04 : 1.0
                        Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutQuart } }

                        background: Rectangle {
                            // Wider when unowned: "CHECK ON STEAM" plus the logo
                            // does not fit the 180px play button.
                            implicitWidth: detailsRoot.isOwned ? 180 : 260
                            implicitHeight: 65
                            radius: 8
                            color: detailsRoot.isOwned
                                   ? (playBtn.down ? "#e0e0e0" : "white")
                                   : (playBtn.down ? "#132b45" : "#1b2838")   // Steam navy
                            border.width: playBtn.emphasized ? 3 : 0
                            border.color: detailsRoot.isOwned ? "#27ae60" : "#66c0f4"
                        }
                        contentItem: Item {
                            implicitWidth: playContent.implicitWidth
                            implicitHeight: playContent.implicitHeight
                            Row {
                                id: playContent
                                anchors.centerIn: parent
                                spacing: 10
                                Image {
                                    id: steamLogo
                                    anchors.verticalCenter: parent.verticalCenter
                                    // Degrades to a text-only button if the asset
                                    // is missing, rather than leaving a blank gap.
                                    visible: !detailsRoot.isOwned && status === Image.Ready
                                    // Relative so it survives the module's
                                    // RESOURCE_PREFIX rather than hardcoding it.
                                    source: detailsRoot.isOwned
                                            ? "" : "assets/steam.png"
                                    sourceSize.width: 26
                                    sourceSize.height: 26
                                    width: visible ? 26 : 0
                                    height: 26
                                    fillMode: Image.PreserveAspectFit
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: detailsRoot.isOwned ? "play" : "CHECK ON STEAM"
                                    color: detailsRoot.isOwned ? "black" : "white"
                                    font.bold: true
                                    font.pixelSize: detailsRoot.isOwned ? 24 : 15
                                    font.letterSpacing: detailsRoot.isOwned ? 0 : 1
                                }
                            }
                        }
                        onClicked: {
                            if (!detailsRoot.gameData) return
                            if (detailsRoot.isOwned)
                                detailsRoot.api.launchGameFrom(detailsRoot.gameData.name, detailsRoot.launchOrigin)
                            else
                                Qt.openUrlExternally(detailsRoot.steamUrl)
                        }
                    }
                    
                    Row {
                        spacing: 12; anchors.verticalCenter: parent.verticalCenter

                        // Favourite. There is deliberately no dislike counterpart:
                        // asking someone to rate a game they chose to install is
                        // the wrong question, and repeated instant-quits already
                        // tell the recommender the same thing without asking.
                        // Hearting also clears that behavioural penalty, so this
                        // is how you overrule the model when it gets one wrong.
                        Rectangle {
                            id: favoriteButton
                            // A Discover pick resolves to its recommendationList
                            // entry, and that entry's `status` is baked in at
                            // rank time and never rewritten — so on an unowned
                            // game the snapshot alone reads 0.0 forever and the
                            // heart never lit. favoriteGames() is the live
                            // answer (it re-checks preferences.json) and its
                            // gameListChanged notify is exactly what
                            // updatePreference() emits, so the fill animates on
                            // the tap.
                            readonly property bool isFavorite: {
                                if (!detailsRoot.gameData) return false
                                if (detailsRoot.gameData.status === 1.0) return true
                                return !!detailsRoot.api
                                       && !!detailsRoot.findIn(detailsRoot.api.favoriteGames)
                            }
                            readonly property bool emphasized:
                                (detailsRoot.focusedAction === detailsRoot.actionFavorite
                                 && detailsRoot.padInControl)
                                || (favoriteArea.containsMouse && detailsRoot.mouseInControl)

                            width: 50; height: 50; radius: 25
                            border.color: favoriteButton.emphasized ? "white" : "#333"
                            border.width: favoriteButton.emphasized ? 2 : 1
                            color: favoriteButton.isFavorite ? "#e0245e" : "#1a1a1a"
                            scale: favoriteButton.emphasized ? 1.12 : 1.0

                            Behavior on color { ColorAnimation { duration: 150 } }
                            Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutQuart } }

                            Text {
                                anchors.centerIn: parent
                                text: "♥"
                                color: favoriteButton.isFavorite ? "white" : "#666"
                                font.pixelSize: 22
                            }
                            MouseArea {
                                id: favoriteArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: if (detailsRoot.gameData) detailsRoot.api.updatePreference(detailsRoot.gameData.name, 1.0)
                            }
                        }
                    }
                }

                Column {
                    spacing: 5
                    // Fix 3: Font scale up for readability
                    Text { text: "playtime"; color: "#888"; font.pixelSize: 18; font.letterSpacing: 1 }
                    Text { text: detailsRoot.gameData ? detailsRoot.gameData.playtime : "0 Hours"; color: "white"; font.pixelSize: 32; font.bold: true }
                    // Fix 5: Brighter color for Last Played
                    Text { text: "Last Played: " + (detailsRoot.gameData ? detailsRoot.gameData.lastPlayed : "Never"); color: "#aaa"; font.pixelSize: 15 }
                }

                Rectangle {
                    width: 550; height: 55; color: "#1a1a1a"; radius: 8; border.color: "#222"
                    Text {
                        anchors { left: parent.left; verticalCenter: parent.verticalCenter; leftMargin: 20 }
                        // Unowned discovery picks carry no install path at all,
                        // so test the key, not just the map.
                        text: detailsRoot.gameData && detailsRoot.gameData.installDir
                              ? detailsRoot.gameData.installDir
                              : (detailsRoot.isOwned ? "Unknown Path" : "Not installed")
                        color: "#aaa"; font.pixelSize: 15; font.italic: true
                        elide: Text.ElideMiddle; width: 510
                    }
                }
            }

            // Fix 4: Scale up the bottom right meta data
            Column {
                anchors { bottom: parent.bottom; right: parent.right; margins: 40 }
                spacing: 12
                Text { text: "developer: " + (detailsRoot.gameData ? detailsRoot.gameData.developer : "Unknown"); color: "#bbb"; font.pixelSize: 16; anchors.right: parent.right }
                Text { text: "genres: " + (detailsRoot.gameData ? detailsRoot.gameData.genres : "Unknown"); color: "#bbb"; font.pixelSize: 16; anchors.right: parent.right }
                Text { text: "rating: " + (detailsRoot.gameData && detailsRoot.gameData.rating ? detailsRoot.gameData.rating.toFixed(1) : "N/A"); color: "#bbb"; font.pixelSize: 16; anchors.right: parent.right }
                Text { text: "time to beat: " + (detailsRoot.gameData ? detailsRoot.gameData.timeToBeat : "N/A"); color: "#bbb"; font.pixelSize: 16; anchors.right: parent.right }
                Text { text: "source: " + (detailsRoot.gameData ? detailsRoot.gameData.source : "Local"); color: "#888"; font.bold: true; font.pixelSize: 14; anchors.right: parent.right; font.capitalization: Font.AllUppercase }
            }
        }
    }
}
