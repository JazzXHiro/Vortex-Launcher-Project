pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Floating settings panel. The left column lists setting sections; the right
// pane shows the active one. New sections go in the `sections` list and get
// their own page, in the same order, in the StackLayout below.
Popup {
    id: settingsRoot

    property var api: vortexApi
    readonly property var sections: ["Directories", "Recommendations"]
    property string activeSection: "Directories"
    property string statusMessage: ""

    // Mood ids match scoring.MOOD_LABELS / MOOD_NAMES in analytics/scoring.py
    // and the picker cards in main.qml. The names are user-visible in both
    // places — explain() writes "Fits your <name> mood" onto the cards — so
    // they must not drift apart.
    readonly property var moods: [
        { id: 3, name: "Neutral",     icon: "🎯", accent: "#7f8c8d" },
        { id: 0, name: "Relaxed",     icon: "🌿", accent: "#27ae60" },
        { id: 1, name: "Competitive", icon: "⚔️", accent: "#e74c3c" },
        { id: 2, name: "Immersive",   icon: "🌌", accent: "#9b59b6" }
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
        color: "#121212"
        radius: 20
        border.color: "#252525"
        border.width: 1
    }

    contentItem: RowLayout {
        spacing: 0

        // ── LEFT COLUMN: section list ────────────────────────────────────────
        Rectangle {
            id: sidebar
            Layout.preferredWidth: 230
            Layout.fillHeight: true
            color: "#0d0d0d"
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
                    color: "#666"; font.pixelSize: 12; font.bold: true; font.letterSpacing: 2
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
                                   ? "#1f1f1f"
                                   : (sectionArea.containsMouse ? "#181818" : "transparent")

                            Rectangle {
                                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                                width: 3; height: 20; radius: 2
                                color: "white"
                                visible: settingsRoot.activeSection === sectionButton.modelData
                            }

                            Text {
                                anchors { left: parent.left; leftMargin: 18; verticalCenter: parent.verticalCenter }
                                text: sectionButton.modelData
                                font.pixelSize: 14
                                font.bold: settingsRoot.activeSection === sectionButton.modelData
                                color: settingsRoot.activeSection === sectionButton.modelData ? "white" : "#888"
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

        Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: "#252525" }

        // ── RIGHT PANE: active section ───────────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // Close button
            Rectangle {
                anchors { top: parent.top; right: parent.right; margins: 20 }
                z: 5
                width: 32; height: 32; radius: 16
                color: closeArea.containsMouse ? "#252525" : "transparent"
                Text { anchors.centerIn: parent; text: "✕"; color: "#888"; font.pixelSize: 14 }
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
                    color: "white"; font.pixelSize: 24; font.bold: true
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
                            color: "#888"; font.pixelSize: 14
                            wrapMode: Text.WordWrap
                        }

                        // Directory list
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: "#0d0d0d"; radius: 10
                            border.color: "#222"; border.width: 1
                            clip: true

                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 60
                                visible: dirList.count === 0
                                text: "No game folders configured yet.\nUse the button below to add one."
                                horizontalAlignment: Text.AlignHCenter
                                color: "#555"; font.pixelSize: 14
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
                                    color: dirRow.armed ? "#231717" : "#161616"
                                    border.width: 1
                                    border.color: dirRow.armed ? "#5c2b2b" : "transparent"

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
                                        color: "#555"; font.pixelSize: 13
                                    }

                                    Text {
                                        anchors {
                                            left: parent.left; leftMargin: 44
                                            right: parent.right; rightMargin: dirRow.armed ? 180 : 48
                                            verticalCenter: parent.verticalCenter
                                        }
                                        text: dirRow.modelData
                                        color: dirRow.armed ? "#999" : "#ddd"; font.pixelSize: 14
                                        elide: Text.ElideMiddle
                                    }

                                    // Confirm step — removing deletes artwork, so never one click.
                                    Row {
                                        anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
                                        spacing: 8
                                        visible: dirRow.armed

                                        Rectangle {
                                            width: 86; height: 28; radius: 14
                                            color: confirmArea.containsMouse ? "#e74c3c" : "#c0392b"
                                            Text {
                                                anchors.centerIn: parent
                                                text: "REMOVE"
                                                color: "white"; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1
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
                                            color: cancelArea.containsMouse ? "#2b2b2b" : "transparent"
                                            border.color: "#3a3a3a"; border.width: 1
                                            Text {
                                                anchors.centerIn: parent
                                                text: "CANCEL"
                                                color: "#aaa"; font.pixelSize: 10; font.bold: true; font.letterSpacing: 1
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
                                        color: removeArea.containsMouse ? "#c0392b" : "transparent"

                                        Text {
                                            anchors.centerIn: parent
                                            text: "✕"
                                            color: removeArea.containsMouse ? "white" : "#777"
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
                                color: addDirArea.containsMouse ? "#ffffff" : "#1a1a1a"
                                border.color: addDirArea.containsMouse ? "#ffffff" : "#333"

                                Text {
                                    anchors.centerIn: parent
                                    text: "+ ADD DIRECTORY"
                                    font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                                    color: addDirArea.containsMouse ? "black" : "#aaa"
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
                                color: "#888"; font.pixelSize: 13
                                elide: Text.ElideRight
                            }
                        }
                    }

                    // ── PAGE: Recommendations ────────────────────────────────
                    ColumnLayout {
                        spacing: 18

                        Text {
                            Layout.fillWidth: true
                            text: "How Vortex ranks the picks on the Recommendations tab."
                            color: "#888"; font.pixelSize: 14
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            text: "MOOD"
                            color: "#666"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 2
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
                                color: moodPicker.hovered ? "#181818" : "#111111"
                                border.width: 1
                                border.color: moodPicker.popup.visible
                                              ? moodPicker.selected.accent : "#2a2a2a"

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
                                color: "#777"; font.pixelSize: 12
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
                                    color: moodItem.hovered ? "#1f1f1f"
                                                            : (moodItem.current ? "#1a1a1a" : "transparent")
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
                                        color: moodItem.current ? moodItem.modelData.accent : "#ccc"
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
                                    color: "#141414"; radius: 10
                                    border.color: "#2a2a2a"; border.width: 1
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
                            color: "#666"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 2
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
                                    color: "#ddd"; font.pixelSize: 14
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: "Limits discovery picks to games that are widely played, "
                                          + "highly rated, or newly released. Your own library is unaffected."
                                    color: "#777"; font.pixelSize: 12
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
                                    color: curatedSwitch.checked ? "#1e4d2f" : "#141414"
                                    border.width: 1
                                    border.color: curatedSwitch.checked
                                                  ? "#27ae60"
                                                  : (curatedSwitch.hovered ? "#3a3a3a" : "#2a2a2a")

                                    Behavior on color { ColorAnimation { duration: 150 } }
                                    Behavior on border.color { ColorAnimation { duration: 150 } }

                                    Rectangle {
                                        width: 22; height: 22
                                        radius: height / 2
                                        anchors.verticalCenter: parent.verticalCenter
                                        x: curatedSwitch.checked ? parent.width - width - 4 : 4
                                        color: curatedSwitch.checked ? "#27ae60" : "#555"

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
                            color: "#666"; font.pixelSize: 11; font.bold: true; font.letterSpacing: 2
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
                                    color: "#ddd"; font.pixelSize: 14
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: "Relaxed, Competitive and Immersive stop using your playtime, "
                                          + "and go on the mood and your favourites instead. Neutral is unaffected."
                                    color: "#777"; font.pixelSize: 12
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
                                    color: ignorePlayedSwitch.checked ? "#1e4d2f" : "#141414"
                                    border.width: 1
                                    border.color: ignorePlayedSwitch.checked
                                                  ? "#27ae60"
                                                  : (ignorePlayedSwitch.hovered ? "#3a3a3a" : "#2a2a2a")

                                    Behavior on color { ColorAnimation { duration: 150 } }
                                    Behavior on border.color { ColorAnimation { duration: 150 } }

                                    Rectangle {
                                        width: 22; height: 22
                                        radius: height / 2
                                        anchors.verticalCenter: parent.verticalCenter
                                        x: ignorePlayedSwitch.checked ? parent.width - width - 4 : 4
                                        color: ignorePlayedSwitch.checked ? "#27ae60" : "#555"

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
                                    color: "#ddd"; font.pixelSize: 14
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: "Drops your favourites from the same three moods. "
                                          + "Turn both of these on and the picks come from the mood alone."
                                    color: "#777"; font.pixelSize: 12
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
                                    color: ignoreLikedSwitch.checked ? "#1e4d2f" : "#141414"
                                    border.width: 1
                                    border.color: ignoreLikedSwitch.checked
                                                  ? "#27ae60"
                                                  : (ignoreLikedSwitch.hovered ? "#3a3a3a" : "#2a2a2a")

                                    Behavior on color { ColorAnimation { duration: 150 } }
                                    Behavior on border.color { ColorAnimation { duration: 150 } }

                                    Rectangle {
                                        width: 22; height: 22
                                        radius: height / 2
                                        anchors.verticalCenter: parent.verticalCenter
                                        x: ignoreLikedSwitch.checked ? parent.width - width - 4 : 4
                                        color: ignoreLikedSwitch.checked ? "#27ae60" : "#555"

                                        Behavior on x { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                                        Behavior on color { ColorAnimation { duration: 150 } }
                                    }
                                }

                                // The label lives in the RowLayout above, so the
                                // control itself contributes no extra text.
                                contentItem: Item {}
                            }
                        }

                        Item { Layout.fillHeight: true }
                    }
                }
            }
        }
    }
}
