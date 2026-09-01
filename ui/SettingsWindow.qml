pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Vortex

// Floating settings panel. The left column lists setting sections; the right
// pane shows the active one. New sections go in the `sections` list and get
// their own page, in the same order, in the StackLayout below.
Popup {
    id: settingsRoot

    property var api: vortexApi
    readonly property var sections: ["Directories", "Removed games", "Recommendations"]
    property string activeSection: "Directories"
    property string statusMessage: ""

    // Mood ids match scoring.MOOD_LABELS / MOOD_NAMES in analytics/scoring.py
    // and the picker cards in main.qml. The names are user-visible in both
    // places — explain() writes "Fits your <name> mood" onto the cards — so
    // they must not drift apart. The accents no longer can: both this list and
    // main.qml's cards read them from Theme.
    readonly property var moods: [
        { id: 3, name: "Neutral",     icon: "🎯", accent: Theme.moodNeutral },
        { id: 0, name: "Relaxed",     icon: "🌿", accent: Theme.moodRelaxed },
        { id: 1, name: "Competitive", icon: "⚔️", accent: Theme.moodCompetitive },
        { id: 2, name: "Immersive",   icon: "🌌", accent: Theme.moodImmersive }
    ]

    // Position of a mood id in `moods`. Falls back to 0 — Neutral, which is
    // also the bridge's default — rather than -1, so the picker always has
    // something to show.
    function moodIndex(id) {
        for (let i = 0; i < settingsRoot.moods.length; ++i) {
            if (settingsRoot.moods[i].id === id)
                return i;
        }
        return 0;
    }

    // The FolderDialog lives in main.qml so the top-bar shortcut and this panel
    // share one picker; main.qml calls reportAddResult() with the outcome.
    signal requestAddDirectory()

    // This panel renders in the overlay layer, above the full-screen loading
    // spinner, so it reports scan progress itself.
    property bool awaitingScan: false

    // Row index whose ✕ was clicked and is awaiting confirmation; -1 for none.
    property int armedIndex: -1

    function reportAddResult(added) {
        if (!added) {
            settingsRoot.statusMessage = "That folder is already in the list.";
            return;
        }
        settingsRoot.awaitingScan = true;
        settingsRoot.statusMessage = "Scanning folder and fetching artwork…";
    }

    Connections {
        target: settingsRoot.api

        function onLoadingChanged() {
            if (settingsRoot.awaitingScan && !settingsRoot.api.isLoading) {
                settingsRoot.awaitingScan = false;
                settingsRoot.statusMessage = "Library updated — artwork downloaded.";
            }
        }

        // Row indices shift when the list changes, so never leave one armed.
        function onLocalDirectoriesChanged() {
            settingsRoot.armedIndex = -1;
        }

        function onDirectoryRemoved(folder, gamesRemoved, artworkDeleted) {
            settingsRoot.statusMessage =
                "Removed " + gamesRemoved + " game" + (gamesRemoved === 1 ? "" : "s") +
                " · deleted artwork for " + artworkDeleted + ".";
        }
    }

    width: Math.min(parent.width * 0.62, 940)
    height: Math.min(parent.height * 0.62, 600)
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    modal: true; focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0

    onOpened: {
        settingsRoot.statusMessage = "";
        settingsRoot.awaitingScan = false;
        settingsRoot.armedIndex = -1;
        if (settingsRoot.api) settingsRoot.api.refreshLocalDirectories();
    }

    background: Rectangle {
        color: Theme.bgPanel
        radius: 20
        border.color: Theme.borderQuiet
        border.width: 1
    }

    contentItem: RowLayout {
        spacing: 0

        // ── LEFT COLUMN: section list ────────────────────────────────────────
        Rectangle {
            id: sidebar
            Layout.preferredWidth: 230
            Layout.fillHeight: true
            color: Theme.bgSunken
            radius: 20

            // Square off the inner edge so only the outer corners stay rounded.
            Rectangle {
                anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
                width: 20
                color: sidebar.color
            }

            Column {
                anchors { top: parent.top; left: parent.left; right: parent.right; margins: 25 }
                spacing: 18

                Text {
                    text: "SETTINGS"
                    color: Theme.textFaint; font.pixelSize: 12; font.bold: true; font.letterSpacing: 2
                }

                Column {
                    width: parent.width - 25
                    spacing: 6

                    Repeater {
                        model: settingsRoot.sections
                        delegate: Rectangle {
                            id: sectionButton
                            required property string modelData

                            width: parent.width; height: 40; radius: 8
                            color: settingsRoot.activeSection === sectionButton.modelData
                                   ? Theme.bgActive
                                   : (sectionArea.containsMouse ? Theme.bgRaised : "transparent")

                            Rectangle {
                                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                                width: 3; height: 20; radius: 2
                                color: Theme.accent
                                visible: settingsRoot.activeSection === sectionButton.modelData
                            }

                            Text {
                                anchors { left: parent.left; leftMargin: 18; verticalCenter: parent.verticalCenter }
                                text: sectionButton.modelData
                                font.pixelSize: 14
                                font.bold: settingsRoot.activeSection === sectionButton.modelData
                                color: settingsRoot.activeSection === sectionButton.modelData ? Theme.textPrimary : Theme.textMuted
                            }

                            MouseArea {
                                id: sectionArea
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    // A row armed on the Directories page must not
                                    // still be armed when the user returns to it.
                                    settingsRoot.armedIndex = -1;
                                    settingsRoot.activeSection = sectionButton.modelData;
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: Theme.divider }

        // ── RIGHT PANE: active section ───────────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // Close button
            Rectangle {
                anchors { top: parent.top; right: parent.right; margins: 20 }
                z: 5
                width: 32; height: 32; radius: 16
                color: closeArea.containsMouse ? Theme.bgEmphasis : "transparent"
                Text { anchors.centerIn: parent; text: "✕"; color: Theme.textMuted; font.pixelSize: 14 }
                MouseArea {
                    id: closeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: settingsRoot.close()
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 35
                spacing: 18

                Text {
                    Layout.fillWidth: true
                    Layout.rightMargin: 40
                    text: settingsRoot.activeSection
                    color: Theme.textPrimary; font.pixelSize: 24; font.bold: true
                }

                // One page per entry in `sections`, in the same order.
                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: settingsRoot.sections.indexOf(settingsRoot.activeSection)

                    // ── PAGE: Directories ────────────────────────────────────
                    ColumnLayout {
                        spacing: 18

                        Text {
                            Layout.fillWidth: true
                            text: "Folders Vortex scans for locally installed games."
                            color: Theme.textMuted; font.pixelSize: 14
                            wrapMode: Text.WordWrap
                        }

                        // Directory list
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: Theme.bgSunken; radius: 10
                            border.color: Theme.borderQuiet; border.width: 1
                            clip: true

                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 60
                                visible: dirList.count === 0
                                text: "No game folders configured yet.\nUse the button below to add one."
                                horizontalAlignment: Text.AlignHCenter
                                color: Theme.textFaint; font.pixelSize: 14
                                wrapMode: Text.WordWrap
                            }

                            ListView {
                                id: dirList
                                anchors.fill: parent
                                anchors.margins: 10
                                clip: true
                                spacing: 6

                                model: settingsRoot.api ? settingsRoot.api.localDirectories : []

                                ScrollBar.vertical: VortexScrollBar { }

                                delegate: Rectangle {
                                    id: dirRow
                                    required property string modelData
                                    required property int index

                                    readonly property bool armed: settingsRoot.armedIndex === dirRow.index

                                    width: dirList.width - 20
                                    height: 46; radius: 8
                                    color: dirRow.armed ? Theme.dangerBgSubtle : Theme.bgSurface
                                    border.width: 1
                                    border.color: dirRow.armed ? Theme.dangerBorder : "transparent"

                                    // Hover tracking only — the row itself is not clickable.
                                    MouseArea {
                                        id: rowHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        acceptedButtons: Qt.NoButton
                                    }

                                    Text {
                                        anchors { left: parent.left; leftMargin: 14; verticalCenter: parent.verticalCenter }
                                        text: (dirRow.index + 1) + "."
                                        color: Theme.textFaint; font.pixelSize: 13
                                    }

                                    Text {
                                        anchors {
                                            left: parent.left; leftMargin: 44
                                            right: parent.right; rightMargin: dirRow.armed ? 180 : 48
                                            verticalCenter: parent.verticalCenter
                                        }
                                        text: dirRow.modelData
                                        color: dirRow.armed ? Theme.textSecondary : Theme.textBody; font.pixelSize: 14
                                        elide: Text.ElideMiddle
                                    }

                                    // Confirm step — removing deletes artwork, so never one click.
                                    Row {
                                        anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                                        spacing: 8
                                        visible: dirRow.armed

                                        Rectangle {
                                            width: 86; height: 28; radius: 14
                                            color: confirmArea.containsMouse ? Theme.danger : Theme.dangerRest
                                            Text {
                                                anchors.centerIn: parent
                                                text: "REMOVE"
                                                color: Theme.textPrimary; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1
                                            }
                                            MouseArea {
                                                id: confirmArea
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                onClicked: {
                                                    settingsRoot.armedIndex = -1;
                                                    settingsRoot.statusMessage = "Removing folder and cleaning up artwork…";
                                                    if (settingsRoot.api)
                                                        settingsRoot.api.removeLocalGameDirectory(dirRow.modelData);
                                                }
                                            }
                                        }

                                        Rectangle {
                                            width: 74; height: 28; radius: 14
                                            color: cancelArea.containsMouse ? Theme.bgEmphasis : "transparent"
                                            border.color: Theme.borderControl; border.width: 1
                                            Text {
                                                anchors.centerIn: parent
                                                text: "CANCEL"
                                                color: Theme.textSecondary; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1
                                            }
                                            MouseArea {
                                                id: cancelArea
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                onClicked: settingsRoot.armedIndex = -1
                                            }
                                        }
                                    }

                                    // Remove button
                                    Rectangle {
                                        anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                                        width: 26; height: 26; radius: 13
                                        visible: !dirRow.armed
                                        opacity: rowHover.containsMouse || removeArea.containsMouse ? 1.0 : 0.35
                                        color: removeArea.containsMouse ? Theme.dangerRest : "transparent"

                                        Text {
                                            anchors.centerIn: parent
                                            text: "✕"
                                            color: removeArea.containsMouse ? Theme.textPrimary : Theme.textMuted
                                            font.pixelSize: 12
                                        }

                                        MouseArea {
                                            id: removeArea
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            onClicked: settingsRoot.armedIndex = dirRow.index
                                        }
                                    }
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 15

                            Rectangle {
                                implicitWidth: 190; implicitHeight: 42; radius: 21
                                color: addDirArea.containsMouse ? Theme.accent : Theme.bgRaised
                                border.color: addDirArea.containsMouse ? Theme.focusRing : Theme.borderControl

                                Text {
                                    anchors.centerIn: parent
                                    text: "+ ADD DIRECTORY"
                                    font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                                    color: addDirArea.containsMouse ? Theme.textInverse : Theme.textSecondary
                                }

                                MouseArea {
                                    id: addDirArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: settingsRoot.requestAddDirectory()
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: settingsRoot.statusMessage
                                color: Theme.textMuted; font.pixelSize: 13
                                elide: Text.ElideRight
                            }
                        }
                    }

                    // ── PAGE: Removed games ──────────────────────────────────
                    //
                    // Games taken out of the library from a card's ⋮ menu. The
                    // scan honours that list, so this page is the only way
                    // back — which is the reason it exists at all.
                    ColumnLayout {
                        spacing: 18

                        Text {
                            Layout.fillWidth: true
                            text: "Games hidden from the library. Their files were never touched — "
                                  + "restoring one puts it back where it was."
                            color: Theme.textMuted; font.pixelSize: 14
                            wrapMode: Text.WordWrap
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: Theme.bgSunken; radius: 10
                            border.color: Theme.borderQuiet; border.width: 1
                            clip: true

                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 60
                                visible: removedList.count === 0
                                // "three-dot" rather than a ⋮ glyph: no font
                                // family is set anywhere in this UI, and the
                                // one the panel falls back to draws it as a
                                // hairline that reads as a stray pipe.
                                text: "Nothing removed.\nUse the three-dot button on a game card to take one out of the library."
                                horizontalAlignment: Text.AlignHCenter
                                color: Theme.textFaint; font.pixelSize: 14
                                wrapMode: Text.WordWrap
                            }

                            ListView {
                                id: removedList
                                anchors.fill: parent
                                anchors.margins: 10
                                clip: true
                                spacing: 6

                                model: settingsRoot.api ? settingsRoot.api.removedGames : []

                                ScrollBar.vertical: VortexScrollBar { }

                                delegate: Rectangle {
                                    id: removedRow
                                    // An object rather than a path string: the
                                    // record carries the identity keys the
                                    // bridge matches on as well as the title.
                                    required property var modelData

                                    width: removedList.width - 20
                                    height: 46; radius: 8
                                    color: Theme.bgSurface

                                    // Hover tracking only — the row itself is
                                    // not clickable, exactly as above.
                                    MouseArea {
                                        id: removedRowHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        acceptedButtons: Qt.NoButton
                                    }

                                    Text {
                                        anchors {
                                            left: parent.left; leftMargin: 16
                                            right: parent.right; rightMargin: 190
                                            verticalCenter: parent.verticalCenter
                                        }
                                        text: removedRow.modelData.name
                                        color: Theme.textBody; font.pixelSize: 14
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        anchors {
                                            right: parent.right; rightMargin: 116
                                            verticalCenter: parent.verticalCenter
                                        }
                                        text: removedRow.modelData.source || ""
                                        color: Theme.textFaint; font.pixelSize: 11
                                        font.bold: true; font.letterSpacing: 1
                                    }

                                    // No confirm step: putting a game back
                                    // cannot lose anything.
                                    Rectangle {
                                        anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                                        width: 92; height: 28; radius: 14
                                        color: restoreArea.containsMouse ? Theme.accent : "transparent"
                                        border.color: restoreArea.containsMouse ? Theme.focusRing : Theme.borderControl
                                        border.width: 1
                                        opacity: removedRowHover.containsMouse
                                                 || restoreArea.containsMouse ? 1.0 : 0.55

                                        Text {
                                            anchors.centerIn: parent
                                            text: "RESTORE"
                                            color: restoreArea.containsMouse ? Theme.textInverse : Theme.textSecondary
                                            font.pixelSize: 10; font.bold: true; font.letterSpacing: 1
                                        }

                                        MouseArea {
                                            id: restoreArea
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            onClicked: {
                                                if (!settingsRoot.api) return;
                                                // Restoring rescans the disk, so
                                                // reuse the wait the add-directory
                                                // button already reports with.
                                                settingsRoot.awaitingScan = true;
                                                settingsRoot.statusMessage =
                                                    "Restoring " + removedRow.modelData.name
                                                    + " — rescanning library…";
                                                settingsRoot.api.restoreToLibrary(removedRow.modelData.name);
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: settingsRoot.statusMessage
                            color: Theme.textMuted; font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                    }

                    // ── PAGE: Recommendations ────────────────────────────────
                    // Taller than the popup at any window size, so the page
                    // scrolls rather than squeezing its sections. Same Flickable
                    // + VortexScrollBar pairing as the recommendations tab.
                    Flickable {
                        id: recsPage

                        clip: true
                        contentWidth: width
                        contentHeight: recsColumn.height
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: VortexScrollBar { }

                        ColumnLayout {
                            id: recsColumn

                            // Leaves the scrollbar's 14px gutter free, so the
                            // wrapping description text never runs under it.
                            width: recsPage.width - 14
                            spacing: 18

                            Text {
                                Layout.fillWidth: true
                                text: "How Vortex ranks the picks on the Recommendations tab."
                                color: Theme.textMuted; font.pixelSize: 14
                                wrapMode: Text.WordWrap
                            }

                            Text {
                                text: "MOOD"
                                color: Theme.textFaint; font.pixelSize: 11; font.bold: true; font.letterSpacing: 2
                            }

                            // The same four moods as the startup picker, reachable
                            // again without restarting. Picking one re-ranks only —
                            // it does not rescan the disk, so it is cheap enough to
                            // change on a whim.
                            ComboBox {
                                id: moodPicker

                                Layout.fillWidth: true
                                Layout.maximumWidth: 260
                                Layout.preferredHeight: 46

                                model: settingsRoot.moods
                                padding: 0
                                rightPadding: 36

                                // currentIndex is -1 until the first assignment lands.
                                readonly property var selected:
                                    settingsRoot.moods[Math.max(0, moodPicker.currentIndex)]

                                // api.currentMood is the source of truth, but choosing an
                                // item makes ComboBox write currentIndex itself, which
                                // would clobber a plain binding. A Binding element keeps
                                // reapplying, so a mood set anywhere else still shows here.
                                Binding {
                                    target: moodPicker
                                    property: "currentIndex"
                                    value: settingsRoot.moodIndex(
                                               settingsRoot.api ? settingsRoot.api.currentMood : 3)
                                }

                                onActivated: function (index) {
                                    if (settingsRoot.api)
                                        settingsRoot.api.setMood(settingsRoot.moods[index].id);
                                }

                                background: Rectangle {
                                    radius: 10
                                    color: moodPicker.hovered ? Theme.bgRaised : Theme.bgPanel
                                    border.width: 1
                                    border.color: moodPicker.popup.visible
                                                  ? moodPicker.selected.accent : Theme.borderMuted

                                    Behavior on color { ColorAnimation { duration: 150 } }
                                    Behavior on border.color { ColorAnimation { duration: 150 } }
                                }

                                contentItem: Item {
                                    Text {
                                        id: pickerIcon
                                        anchors { left: parent.left; leftMargin: 14; verticalCenter: parent.verticalCenter }
                                        text: moodPicker.selected.icon
                                        font.pixelSize: 18
                                    }

                                    Text {
                                        anchors {
                                            left: pickerIcon.right; leftMargin: 12
                                            right: parent.right
                                            verticalCenter: parent.verticalCenter
                                        }
                                        text: moodPicker.selected.name.toUpperCase()
                                        color: moodPicker.selected.accent
                                        font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                                        elide: Text.ElideRight
                                    }
                                }

                                indicator: Text {
                                    anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
                                    text: "▾"
                                    color: Theme.textMuted; font.pixelSize: 12
                                    rotation: moodPicker.popup.visible ? 180 : 0
                                    Behavior on rotation { NumberAnimation { duration: 150 } }
                                }

                                delegate: ItemDelegate {
                                    id: moodItem
                                    required property int index
                                    required property var modelData

                                    readonly property bool current: moodPicker.currentIndex === moodItem.index

                                    width: moodItem.ListView.view ? moodItem.ListView.view.width : moodPicker.width
                                    height: 42
                                    padding: 0

                                    background: Rectangle {
                                        radius: 8
                                        color: moodItem.hovered ? Theme.bgActive
                                                                : (moodItem.current ? Theme.bgRaised : "transparent")
                                    }

                                    contentItem: Item {
                                        Text {
                                            id: itemIcon
                                            anchors { left: parent.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
                                            text: moodItem.modelData.icon
                                            font.pixelSize: 16
                                        }

                                        Text {
                                            anchors {
                                                left: itemIcon.right; leftMargin: 12
                                                right: parent.right; rightMargin: 12
                                                verticalCenter: parent.verticalCenter
                                            }
                                            text: moodItem.modelData.name.toUpperCase()
                                            color: moodItem.current ? moodItem.modelData.accent : Theme.textBody
                                            font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                                            elide: Text.ElideRight
                                        }
                                    }
                                }

                                popup: Popup {
                                    y: moodPicker.height + 6
                                    width: moodPicker.width
                                    implicitHeight: contentItem.implicitHeight + 12
                                    padding: 6

                                    background: Rectangle {
                                        color: Theme.bgSurface; radius: 10
                                        border.color: Theme.borderMuted; border.width: 1
                                    }

                                    contentItem: ListView {
                                        clip: true
                                        implicitHeight: contentHeight
                                        spacing: 2
                                        model: moodPicker.delegateModel
                                        currentIndex: moodPicker.highlightedIndex
                                    }
                                }
                            }

                            Text {
                                Layout.topMargin: 8
                                text: "QUALITY FILTER"
                                color: Theme.textFaint; font.pixelSize: 11; font.bold: true; font.letterSpacing: 2
                            }

                            // Restricts the Discover picks to games that are widely
                            // played, highly rated or newly released. The library
                            // section is deliberately excluded: owned rows carry no
                            // rating counts at all, so "popular" cannot be answered
                            // for them, and the pool is only 18 games for 10 slots.
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.maximumWidth: 420
                                spacing: 14

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3

                                    Text {
                                        text: "Only well-known games"
                                        color: Theme.textBody; font.pixelSize: 14
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: "Limits discovery picks to games that are widely played, "
                                              + "highly rated, or newly released. Your own library is unaffected."
                                        color: Theme.textMuted; font.pixelSize: 12
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                Switch {
                                    id: curatedSwitch

                                    Layout.alignment: Qt.AlignTop
                                    padding: 0
                                    implicitWidth: 52
                                    implicitHeight: 30

                                    // api.curatedOnly is the source of truth, but flicking the
                                    // switch makes Switch write `checked` itself, which would
                                    // clobber a plain binding -- the same reason the mood
                                    // ComboBox above uses a Binding element for currentIndex.
                                    Binding {
                                        target: curatedSwitch
                                        property: "checked"
                                        value: settingsRoot.api ? settingsRoot.api.curatedOnly : false
                                    }

                                    onToggled: {
                                        if (settingsRoot.api)
                                            settingsRoot.api.setCuratedOnly(curatedSwitch.checked);
                                    }

                                    indicator: Rectangle {
                                        implicitWidth: 52
                                        implicitHeight: 30
                                        radius: height / 2
                                        color: curatedSwitch.checked ? Theme.positiveBg : Theme.bgSurface
                                        border.width: 1
                                        border.color: curatedSwitch.checked
                                                      ? Theme.positive
                                                      : (curatedSwitch.hovered ? Theme.borderControl : Theme.borderMuted)

                                        Behavior on color { ColorAnimation { duration: 150 } }
                                        Behavior on border.color { ColorAnimation { duration: 150 } }

                                        Rectangle {
                                            width: 22; height: 22
                                            radius: height / 2
                                            anchors.verticalCenter: parent.verticalCenter
                                            x: curatedSwitch.checked ? parent.width - width - 4 : 4
                                            color: curatedSwitch.checked ? Theme.positive : Theme.bgSwitchHandle

                                            Behavior on x { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                                            Behavior on color { ColorAnimation { duration: 150 } }
                                        }
                                    }

                                    // The label lives in the RowLayout above, so the
                                    // control itself contributes no extra text.
                                    contentItem: Item {}
                                }
                            }

                            Text {
                                Layout.topMargin: 8
                                text: "PROFILE"
                                color: Theme.textFaint; font.pixelSize: 11; font.bold: true; font.letterSpacing: 2
                            }

                            // Drops the games you have actually played from the
                            // profile the moods rank with, leaving the mood itself
                            // and your hearted games. Neutral is deliberately
                            // exempt -- it has no mood weights to fall back on, so
                            // without a history it would rank on ratings alone.
                            // Played games stay out of the results either way.
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.maximumWidth: 420
                                spacing: 14

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3

                                    Text {
                                        text: "Ignore games you've played"
                                        color: Theme.textBody; font.pixelSize: 14
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: "Relaxed, Competitive and Immersive stop using your playtime, "
                                              + "and go on the mood and your favourites instead. Neutral is unaffected."
                                        color: Theme.textMuted; font.pixelSize: 12
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                Switch {
                                    id: ignorePlayedSwitch

                                    Layout.alignment: Qt.AlignTop
                                    padding: 0
                                    implicitWidth: 52
                                    implicitHeight: 30

                                    // Same reason curatedSwitch above uses a Binding
                                    // element: flicking the switch makes it write
                                    // `checked` itself, clobbering a plain binding.
                                    Binding {
                                        target: ignorePlayedSwitch
                                        property: "checked"
                                        value: settingsRoot.api ? settingsRoot.api.ignorePlayedGames : false
                                    }

                                    onToggled: {
                                        if (settingsRoot.api)
                                            settingsRoot.api.setIgnorePlayedGames(ignorePlayedSwitch.checked);
                                    }

                                    indicator: Rectangle {
                                        implicitWidth: 52
                                        implicitHeight: 30
                                        radius: height / 2
                                        color: ignorePlayedSwitch.checked ? Theme.positiveBg : Theme.bgSurface
                                        border.width: 1
                                        border.color: ignorePlayedSwitch.checked
                                                      ? Theme.positive
                                                      : (ignorePlayedSwitch.hovered ? Theme.borderControl : Theme.borderMuted)

                                        Behavior on color { ColorAnimation { duration: 150 } }
                                        Behavior on border.color { ColorAnimation { duration: 150 } }

                                        Rectangle {
                                            width: 22; height: 22
                                            radius: height / 2
                                            anchors.verticalCenter: parent.verticalCenter
                                            x: ignorePlayedSwitch.checked ? parent.width - width - 4 : 4
                                            color: ignorePlayedSwitch.checked ? Theme.positive : Theme.bgSwitchHandle

                                            Behavior on x { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                                            Behavior on color { ColorAnimation { duration: 150 } }
                                        }
                                    }

                                    // The label lives in the RowLayout above, so the
                                    // control itself contributes no extra text.
                                    contentItem: Item {}
                                }
                            }

                            // The counterpart. A game that was played AND hearted
                            // counts as played, so it survives this one and only
                            // the pure hearts go -- the same call the cards make
                            // when they say "played" rather than "liked".
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.maximumWidth: 420
                                spacing: 14

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3

                                    Text {
                                        text: "Ignore games you've liked"
                                        color: Theme.textBody; font.pixelSize: 14
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: "Drops your favourites from the same three moods. "
                                              + "Turn both of these on and the picks come from the mood alone."
                                        color: Theme.textMuted; font.pixelSize: 12
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                Switch {
                                    id: ignoreLikedSwitch

                                    Layout.alignment: Qt.AlignTop
                                    padding: 0
                                    implicitWidth: 52
                                    implicitHeight: 30

                                    // Binding element, same reason as the two switches above.
                                    Binding {
                                        target: ignoreLikedSwitch
                                        property: "checked"
                                        value: settingsRoot.api ? settingsRoot.api.ignoreLikedGames : false
                                    }

                                    onToggled: {
                                        if (settingsRoot.api)
                                            settingsRoot.api.setIgnoreLikedGames(ignoreLikedSwitch.checked);
                                    }

                                    indicator: Rectangle {
                                        implicitWidth: 52
                                        implicitHeight: 30
                                        radius: height / 2
                                        color: ignoreLikedSwitch.checked ? Theme.positiveBg : Theme.bgSurface
                                        border.width: 1
                                        border.color: ignoreLikedSwitch.checked
                                                      ? Theme.positive
                                                      : (ignoreLikedSwitch.hovered ? Theme.borderControl : Theme.borderMuted)

                                        Behavior on color { ColorAnimation { duration: 150 } }
                                        Behavior on border.color { ColorAnimation { duration: 150 } }

                                        Rectangle {
                                            width: 22; height: 22
                                            radius: height / 2
                                            anchors.verticalCenter: parent.verticalCenter
                                            x: ignoreLikedSwitch.checked ? parent.width - width - 4 : 4
                                            color: ignoreLikedSwitch.checked ? Theme.positive : Theme.bgSwitchHandle

                                            Behavior on x { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                                            Behavior on color { ColorAnimation { duration: 150 } }
                                        }
                                    }

                                    // The label lives in the RowLayout above, so the
                                    // control itself contributes no extra text.
                                    contentItem: Item {}
                                }
                            }

                            Text {
                                Layout.topMargin: 8
                                text: "PLAYTIME"
                                color: Theme.textFaint; font.pixelSize: 11; font.bold: true; font.letterSpacing: 2
                            }

                            // Which figure a Steam game's playtime comes from.
                            //
                            // Off, Vortex uses the total it keeps itself: Steam's
                            // lifetime figure taken once when the game is first
                            // seen, plus every session since, with idle time taken
                            // out. On, Steam games show Steam's own live figure,
                            // untouched -- it counts play started outside the
                            // launcher, but nothing measured how much of it was
                            // spent at a pause menu, so nothing is deducted from it.
                            //
                            // Either way Vortex keeps recording, so this can be
                            // switched back without having lost anything.
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.maximumWidth: 420
                                spacing: 14

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3

                                    Text {
                                        text: "Use Steam's own playtime"
                                        color: Theme.textBody; font.pixelSize: 14
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: "Steam games show the total Steam reports, exactly as it reports it. "
                                              + "It counts sessions you started outside Vortex, but none of it can be "
                                              + "broken down into time played and time idle. Leave this off and Vortex "
                                              + "tracks them itself, starting from the hours Steam already had."
                                        color: Theme.textMuted; font.pixelSize: 12
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                Switch {
                                    id: steamPlaytimeSwitch

                                    Layout.alignment: Qt.AlignTop
                                    padding: 0
                                    implicitWidth: 52
                                    implicitHeight: 30

                                    // Same reason the switches above use a Binding
                                    // element: flicking the switch writes `checked`
                                    // itself, clobbering a plain binding.
                                    Binding {
                                        target: steamPlaytimeSwitch
                                        property: "checked"
                                        value: settingsRoot.api ? settingsRoot.api.useSteamPlaytime : false
                                    }

                                    onToggled: {
                                        if (settingsRoot.api)
                                            settingsRoot.api.setUseSteamPlaytime(steamPlaytimeSwitch.checked);
                                    }

                                    indicator: Rectangle {
                                        implicitWidth: 52
                                        implicitHeight: 30
                                        radius: height / 2
                                        color: steamPlaytimeSwitch.checked ? Theme.positiveBg : Theme.bgSurface
                                        border.width: 1
                                        border.color: steamPlaytimeSwitch.checked
                                                      ? Theme.positive
                                                      : (steamPlaytimeSwitch.hovered ? Theme.borderControl : Theme.borderMuted)

                                        Behavior on color { ColorAnimation { duration: 150 } }
                                        Behavior on border.color { ColorAnimation { duration: 150 } }

                                        Rectangle {
                                            width: 22; height: 22
                                            radius: height / 2
                                            anchors.verticalCenter: parent.verticalCenter
                                            x: steamPlaytimeSwitch.checked ? parent.width - width - 4 : 4
                                            color: steamPlaytimeSwitch.checked ? Theme.positive : Theme.bgSwitchHandle

                                            Behavior on x { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                                            Behavior on color { ColorAnimation { duration: 150 } }
                                        }
                                    }

                                    // The label lives in the RowLayout above, so the
                                    // control itself contributes no extra text.
                                    contentItem: Item {}
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
