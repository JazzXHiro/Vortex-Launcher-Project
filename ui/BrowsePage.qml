pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Vortex

// The Browse tab: a search box over the whole IGDB catalog and a grid of what
// it finds -- or, before anything is typed, a rail of what just came out. Everything
// past the cover -- description, screenshots, reviews -- is BrowseDetails'
// job, fetched only when a result is opened.
Item {
    id: page

    property var api: vortexApi
    // Mirrors root.igdbConfigured; credentialStatus() is not bindable.
    property bool igdbConfigured: false
    property bool padActive: false
    property bool padInControl: false
    readonly property bool mouseInControl: !page.padInControl

    // main.qml steers whichever view is showing with the pad like any other
    // tab's grid: the new-releases rail, or the search results grid.
    readonly property Item grid: page.showingNew ? newRail : browseGrid

    signal openRequested(var item)
    signal settingsRequested()

    function focusSearch() {
        searchBox.focusField()
    }

    readonly property string query: searchBox.text.trim()
    readonly property var results: page.api && page.api.browseResults ? page.api.browseResults : []
    readonly property bool searching: !!page.api && page.api.browseSearching
    // Below the search threshold the rail of new releases replaces the grid.
    readonly property bool showingNew: page.query.length < 2
    readonly property var newReleases:
        page.api && page.api.browseNewReleases ? page.api.browseNewReleases : []
    readonly property bool newLoading: !!page.api && page.api.browseNewReleasesLoading

    // The views' models, handed over here rather than bound, so answerStamp
    // is always set before the cards for an answer are made. Cards made just
    // after it rise and fade in one after another; cards made later, by
    // scrolling, simply appear.
    property var gridModel: []
    property var railModel: []
    property double answerStamp: 0
    function showAnswer() {
        page.answerStamp = Date.now()
        page.gridModel = page.showingNew ? [] : page.results
        page.railModel = page.showingNew ? page.newReleases : []
    }
    onResultsChanged: page.showAnswer()
    onNewReleasesChanged: page.showAnswer()
    onShowingNewChanged: page.showAnswer()

    // The bridge ignores repeat calls, so every way onto the tab can ask.
    function refreshNew() {
        if (page.api && page.visible && page.igdbConfigured)
            page.api.loadNewReleases()
    }
    Component.onCompleted: {
        page.showAnswer()
        page.refreshNew()
    }
    // Coming back to the tab replays the pop-in of whichever view is up. Its
    // cards are kept while the tab is away, so there is no new answer to stamp.
    signal shown()
    onVisibleChanged: {
        page.refreshNew()
        if (page.visible)
            page.shown()
    }
    onIgdbConfiguredChanged: page.refreshNew()

    // Waits out a burst of typing before asking IGDB. Each keystroke would
    // otherwise be its own request -- the bridge drops the stale answers, but
    // there is no reason to send them in the first place.
    Timer {
        id: debounce
        interval: 400
        onTriggered: page.runSearch()
    }

    function runSearch() {
        debounce.stop()
        if (!page.api) return
        // One letter matches half the catalog and tells IGDB nothing.
        if (page.query.length < 2)
            page.api.clearBrowse()
        else
            page.api.searchCatalog(page.query)
    }

    Column {
        id: header
        anchors { top: parent.top; horizontalCenter: parent.horizontalCenter }
        spacing: 14
        width: Math.min(parent.width, 640)

        SearchField {
            id: searchBox
            width: parent.width
            height: 48
            fontSize: 16
            placeholderText: "Search any game on IGDB"
            enabled: page.igdbConfigured
            onTextChanged: debounce.restart()
            onAccepted: page.runSearch()
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            // Faded rather than hidden, so it keeps its room and the grid
            // under it does not jump down as an answer arrives.
            opacity: (page.showingNew ? page.newReleases.length > 0
                                      : page.results.length > 0 && !page.searching) ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
            text: page.showingNew
                  ? "NEW RELEASES"
                  : page.results.length + (page.results.length === 1 ? " RESULT" : " RESULTS")
            color: Theme.textFaint
            font.pixelSize: 11; font.bold: true; font.letterSpacing: 2
        }
    }

    // ── States ──────────────────────────────────────────────────────────────
    Column {
        anchors.centerIn: parent
        spacing: 18
        opacity: page.grid.count === 0 ? 1.0 : 0.0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            horizontalAlignment: Text.AlignHCenter
            color: Theme.textGhost
            font.pixelSize: 16; font.bold: true; font.letterSpacing: 2
            text: {
                if (!page.igdbConfigured)
                    return "BROWSE NEEDS IGDB KEYS\nAdd them in Settings to search the catalog"
                if (page.showingNew) {
                    if (page.newLoading)
                        return "LOADING NEW RELEASES…"
                    if (page.api && page.api.browseNewReleasesStatus !== "")
                        return page.api.browseNewReleasesStatus.toUpperCase()
                } else if (page.searching) {
                    return "SEARCHING…"
                } else if (page.api && page.api.browseStatus !== "") {
                    return page.api.browseStatus.toUpperCase()
                }
                return "SEARCH FOR ANY GAME\nSee its details, screenshots and reviews"
            }
        }

        Rectangle {
            id: settingsLink
            readonly property bool hovered: settingsLinkArea.containsMouse && page.mouseInControl
            anchors.horizontalCenter: parent.horizontalCenter
            visible: !page.igdbConfigured
            width: settingsLinkLabel.implicitWidth + 44; height: 35; radius: 17
            color: settingsLink.hovered ? Theme.accent : Theme.bgRaised
            border.color: settingsLink.hovered ? Theme.focusRing : Theme.borderControl

            Text {
                id: settingsLinkLabel
                anchors.centerIn: parent
                text: "OPEN SETTINGS"
                font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                color: settingsLink.hovered ? Theme.textInverse : Theme.textMuted
            }
            MouseArea {
                id: settingsLinkArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: page.settingsRequested()
            }
        }
    }

    // ── Results ─────────────────────────────────────────────────────────────
    GridView {
        id: browseGrid
        anchors { top: header.bottom; topMargin: 24; left: parent.left; right: parent.right; bottom: parent.bottom }
        clip: true
        // Elastic gutters, as on the library grid.
        readonly property int minCellWidth: 280
        readonly property int columns:
            Math.max(1, Math.floor(browseGrid.width / browseGrid.minCellWidth))

        cellWidth: browseGrid.width > 0
                   ? Math.floor(browseGrid.width / browseGrid.columns)
                   : browseGrid.minCellWidth
        cellHeight: 440
        visible: !page.showingNew
        // Empty while the rail is up, so each card belongs to one view only.
        model: page.gridModel
        currentIndex: -1
        delegate: resultCard

        ScrollBar.vertical: VortexScrollBar { }

        // A new answer is a new array; start from the top of it.
        onModelChanged: {
            browseGrid.contentY = 0
            browseGrid.currentIndex = page.padActive && browseGrid.count > 0 ? 0 : -1
        }
    }

    // ── New releases ────────────────────────────────────────────────────────
    ListView {
        id: newRail
        // Padded out past the cards on every side so a highlighted cover can
        // grow without being clipped, as BrowseDetails' screenshot strip does.
        readonly property int popRoom: 12
        // Rail cards are three quarters of a grid card, so more of the list shows.
        readonly property int cardWidth: 180
        readonly property int coverHeight: 270
        readonly property int cardHeight: 320
        readonly property int pitch: newRail.cardWidth + newRail.spacing

        // The scroll range, margins included.
        readonly property real startX: newRail.originX - newRail.leftMargin
        readonly property real endX:
            Math.max(newRail.startX, newRail.originX + newRail.contentWidth
                                     + newRail.rightMargin - newRail.width)
        readonly property bool canBack: newRail.contentX > newRail.startX + 1
        readonly property bool canForward: newRail.contentX < newRail.endX - 1

        anchors {
            top: header.bottom; topMargin: 24 - newRail.popRoom
            left: parent.left; right: parent.right
        }
        height: newRail.cardHeight + 2 * newRail.popRoom
        visible: page.showingNew
        orientation: ListView.Horizontal
        spacing: 20
        leftMargin: newRail.popRoom + 16
        rightMargin: newRail.popRoom + 16
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: page.railModel
        currentIndex: -1
        delegate: resultCard

        // VortexScrollBar is vertical-only.
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

        // The edge arrows: a page of cards at a time, keeping the last card
        // of the old page in view so the jump is easy to follow.
        function scrollPage(step) {
            const shown = newRail.width - newRail.leftMargin - newRail.rightMargin
            const cards = Math.max(1, Math.floor(shown / newRail.pitch) - 1)
            const target = newRail.contentX + step * cards * newRail.pitch
            railScroll.stop()
            railScroll.to = Math.max(newRail.startX, Math.min(newRail.endX, target))
            railScroll.start()
        }

        NumberAnimation {
            id: railScroll
            target: newRail
            property: "contentX"
            duration: 350
            easing.type: Easing.OutCubic
        }

        // A horizontal ListView ignores the vertical wheel entirely, and that
        // is the only wheel most mice have. Turn it into sideways scrolling;
        // a touchpad's sideways swipe still reaches the list on its own.
        WheelHandler {
            orientation: Qt.Vertical
            onWheel: (event) => {
                // A touchpad reports pixels; a mouse notch (120) moves about a card.
                const delta = event.pixelDelta.y !== 0 ? event.pixelDelta.y
                                                       : event.angleDelta.y * 1.5
                if (delta === 0)
                    return
                railScroll.stop()
                newRail.contentX = Math.max(newRail.startX,
                                            Math.min(newRail.endX, newRail.contentX - delta))
            }
        }

        // A refreshed list is a new array; start from the front of it.
        onModelChanged: {
            railScroll.stop()
            newRail.contentX = newRail.startX
            newRail.currentIndex = page.padActive && newRail.count > 0 ? 0 : -1
        }
    }

    // Over the rail's ends, centred on the covers. Each shows only while there
    // is more of the rail its way, and only for the mouse -- the pad already
    // walks the rail card by card.
    Repeater {
        model: [-1, 1]
        delegate: Rectangle {
            id: railArrow
            required property int modelData
            readonly property bool active: page.showingNew && page.mouseInControl
                && (railArrow.modelData < 0 ? newRail.canBack : newRail.canForward)
            readonly property bool hovered: railArrowArea.containsMouse

            x: railArrow.modelData < 0 ? newRail.x + 8
                                       : newRail.x + newRail.width - width - 8
            y: newRail.y + newRail.popRoom + newRail.coverHeight / 2 - height / 2
            // A slim tab rather than a round button, so it hides less of the
            // cover it sits over.
            width: 30; height: 84; radius: 8
            color: railArrow.hovered ? Theme.bgEmphasis : Theme.overlayButton
            border.color: railArrow.hovered ? Theme.focusRing : Theme.borderControl
            opacity: railArrow.active ? 1 : 0
            enabled: railArrow.active
            Behavior on opacity { NumberAnimation { duration: 150 } }

            // Double chevrons, white on transparent so they read against the
            // dark tab. The assets are 512x512; decoded at twice the drawn
            // size, as the review thumbs are.
            Image {
                anchors.centerIn: parent
                width: 18; height: 18
                source: railArrow.modelData < 0 ? "assets/arrow_left.png"
                                                : "assets/arrow_right.png"
                sourceSize.width: 36
                sourceSize.height: 36
                fillMode: Image.PreserveAspectFit
                smooth: true
            }
            MouseArea {
                id: railArrowArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: newRail.scrollPage(railArrow.modelData)
            }
        }
    }

    // One card for both views. Only the showing view has a model, so
    // page.showingNew says which one a card is in.
    Component {
        id: resultCard

        Item {
            id: resultDelegate
            required property var modelData
            required property int index

            readonly property Item view: page.showingNew ? newRail : browseGrid
            // The rail's cards are the smaller size.
            readonly property bool compact: page.showingNew
            readonly property bool highlighted:
                (resultArea.containsMouse && page.mouseInControl)
                || (resultDelegate.view.currentIndex === resultDelegate.index
                    && page.padInControl)

            width: resultDelegate.compact ? newRail.cardWidth : 240
            // On the rail the card spans its height, leaving the pop room
            // above and below the centred cover.
            height: resultDelegate.compact ? newRail.height : 415

            // Part of a fresh answer: rise and fade in, a beat after the card
            // before it, as BrowseDetails' sections do.
            transform: Translate { id: enterShift }
            function popIn(beat) {
                enterPause.duration = Math.max(0, Math.min(beat, 12)) * 40
                enterAnim.stop()
                resultDelegate.opacity = 0
                enterShift.y = 18
                enterAnim.start()
            }
            Component.onCompleted: {
                if (Date.now() - page.answerStamp <= 250)
                    resultDelegate.popIn(resultDelegate.index)
            }
            // A tab switch back: counted from the first card in view, so a
            // scrolled rail or grid does not wait out the cards before it.
            Connections {
                target: page
                function onShown() {
                    if (page.showingNew)
                        resultDelegate.popIn(Math.floor((resultDelegate.x - newRail.contentX)
                                                        / newRail.pitch))
                    else
                        resultDelegate.popIn(resultDelegate.index
                            - Math.floor(browseGrid.contentY / browseGrid.cellHeight)
                              * browseGrid.columns)
                }
            }
            SequentialAnimation {
                id: enterAnim
                PauseAnimation { id: enterPause }
                ParallelAnimation {
                    NumberAnimation {
                        target: resultDelegate; property: "opacity"
                        to: 1.0; duration: 380; easing.type: Easing.OutCubic
                    }
                    NumberAnimation {
                        target: enterShift; property: "y"
                        to: 0; duration: 380; easing.type: Easing.OutCubic
                    }
                }
            }

            Column {
                anchors.centerIn: parent
                spacing: resultDelegate.compact ? 10 : 12

                Rectangle {
                    width: resultDelegate.width
                    height: resultDelegate.compact ? newRail.coverHeight : 360
                    radius: resultDelegate.compact ? 10 : 12
                    color: Theme.bgSurface
                    border.width: 2
                    border.color: resultDelegate.highlighted ? Theme.focusRing : Theme.borderMuted
                    clip: true

                    Image {
                        id: resultCover
                        anchors.fill: parent
                        source: resultDelegate.modelData.coverUrl || ""
                        asynchronous: true
                        fillMode: Image.PreserveAspectCrop
                        opacity: status === Image.Ready ? 1.0 : 0.0
                        Behavior on opacity { NumberAnimation { duration: 250 } }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: resultCover.status !== Image.Ready
                        text: resultCover.status === Image.Loading ? "" : "NO ART"
                        color: Theme.textFaint
                        font.pixelSize: 12
                    }

                    Rectangle {
                        anchors { left: parent.left; top: parent.top; margins: 10 }
                        visible: resultDelegate.modelData.owned === true
                        width: ownedLabel.implicitWidth + 16; height: 22; radius: 11
                        color: Theme.overlayPositive
                        Text {
                            id: ownedLabel
                            anchors.centerIn: parent
                            text: "IN LIBRARY"
                            color: Theme.textPrimary
                            font.pixelSize: 10; font.bold: true; font.letterSpacing: 1
                        }
                    }

                    scale: resultDelegate.highlighted ? 1.04 : 1.0
                    Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutQuart } }
                }

                Column {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 4

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: resultDelegate.width - 20
                        text: resultDelegate.modelData.name
                        color: resultDelegate.highlighted ? Theme.textPrimary : Theme.textBody
                        font.pixelSize: resultDelegate.compact ? 13 : 15
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: resultDelegate.width - 20
                        // New releases are about when, so they get the full date.
                        text: [page.showingNew ? resultDelegate.modelData.releaseDate
                                               : resultDelegate.modelData.year,
                               resultDelegate.modelData.developer]
                                  .filter(function (part) { return !!part }).join("  ·  ")
                        color: Theme.textFaint
                        font.pixelSize: 11
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                }
            }

            MouseArea {
                id: resultArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (page.padActive)
                        resultDelegate.view.currentIndex = resultDelegate.index
                    page.openRequested(resultDelegate.modelData)
                }
            }
        }
    }
}
