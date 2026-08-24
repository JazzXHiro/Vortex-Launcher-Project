pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shown once, on a fresh install, before the user has entered any credentials.
//
// The guiding constraint: Vortex is fully usable with nothing filled in here.
// The Steam scan, playtime tracking and the local recommender never needed an
// API key. IGDB adds metadata and the Discover catalog; SteamGridDB adds
// artwork. So every pane is skippable, "Skip" is a peer of "Continue" rather
// than fine print, and the welcome pane says plainly what already works.
//
// Everything here is also reachable afterwards from Settings, so skipping is
// recoverable without a reinstall.
Popup {
    id: wizardRoot

    property var api: vortexApi

    // 0 welcome, 1 credentials, 2 catalog
    property int pane: 0
    property string statusMessage: ""
    property bool statusIsError: false
    property bool savedKeys: false
    property bool validating: false

    readonly property string igdbUrl: "https://dev.twitch.tv/console/apps"
    readonly property string sgdbUrl: "https://www.steamgriddb.com/profile/preferences/api"

    modal: true
    focus: true
    closePolicy: Popup.NoAutoClose
    anchors.centerIn: Overlay.overlay
    width: 720
    height: 560
    padding: 0

    background: Rectangle {
        color: "#121212"
        radius: 20
        border.color: "#252525"
        border.width: 1
    }

    Overlay.modal: Rectangle { color: "#cc000000" }

    function finish() {
        wizardRoot.close();
    }

    function saveAndAdvance() {
        // Empty fields are legal: saveCredentials treats "" as "leave alone",
        // so a user who fills in only SteamGridDB is not forced to invent IGDB
        // values to get past this pane.
        const anyEntered = idField.text.trim() !== ""
                        || secretField.text.trim() !== ""
                        || sgdbField.text.trim() !== "";

        if (!anyEntered) {
            wizardRoot.statusMessage = "";
            wizardRoot.pane = 2;
            return;
        }

        const ok = wizardRoot.api.saveCredentials(idField.text, secretField.text,
                                                  sgdbField.text);
        if (!ok) {
            wizardRoot.statusIsError = true;
            wizardRoot.statusMessage = "Could not write analytics/.env. "
                                     + "Check that the install folder is writable.";
            return;
        }

        wizardRoot.savedKeys = true;
        wizardRoot.statusIsError = false;
        wizardRoot.statusMessage = "Saved. Checking the keys...";
        wizardRoot.validating = true;

        // Checked against the real providers, because a wrong key used to be
        // accepted in silence: every lookup failed for the rest of the session
        // and the only evidence was a line in a log file. Saving still went
        // through above -- an offline machine must not stop someone entering a
        // key they know is good.
        wizardRoot.api.validateCredentials(idField.text, secretField.text,
                                           sgdbField.text);
        wizardRoot.pane = 2;
    }

    Connections {
        target: wizardRoot.api

        function onCredentialsValidated(result) {
            wizardRoot.validating = false;

            const igdbOk = result.igdbOk === true;
            const sgdbOk = result.sgdbOk === true;

            if (igdbOk && sgdbOk) {
                wizardRoot.statusIsError = false;
                wizardRoot.statusMessage = "Both keys work.";
                return;
            }

            // Report the specific failure rather than a generic one: "rejected"
            // means re-enter the key, "could not reach" means check the
            // network, and telling someone the wrong one wastes their evening.
            const problems = [];
            if (!igdbOk && result.igdbDetail) problems.push("IGDB: " + result.igdbDetail);
            if (!sgdbOk && result.sgdbDetail) problems.push("SteamGridDB: " + result.sgdbDetail);

            wizardRoot.statusIsError = true;
            wizardRoot.statusMessage = problems.join("  ");
        }

        function onCatalogRefreshFinished(ok, details) {
            wizardRoot.statusIsError = !ok;
            wizardRoot.statusMessage = ok
                ? "Catalog ready. Discover is populated."
                : ("Catalog fetch failed: " + details);
        }
    }

    // ── Reusable labelled input ───────────────────────────────────────────
    component LabelledField: ColumnLayout {
        id: fieldRoot
        property string label: ""
        property string hint: ""
        property alias text: input.text
        property alias echoMode: input.echoMode
        property bool alreadySet: false

        spacing: 6
        Layout.fillWidth: true

        RowLayout {
            spacing: 8
            Text {
                text: fieldRoot.label
                color: "#bbb"
                font.pixelSize: 13
                font.bold: true
            }
            // A key already in .env is never echoed back into the field --
            // it is a secret and this is a plain text box. Say it is set
            // instead, so an empty field does not read as "not configured".
            Text {
                visible: fieldRoot.alreadySet
                text: "already set — leave blank to keep"
                color: "#27ae60"
                font.pixelSize: 11
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            radius: 8
            color: "#0d0d0d"
            border.width: 1
            border.color: input.activeFocus ? "#4a4a4a" : "#252525"

            TextField {
                id: input
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                verticalAlignment: TextInput.AlignVCenter
                color: "white"
                font.pixelSize: 13
                selectByMouse: true
                background: null
                placeholderText: fieldRoot.hint
                placeholderTextColor: "#444"
            }
        }
    }

    // ── Link ──────────────────────────────────────────────────────────────
    component ExternalLink: Text {
        id: linkRoot
        property string url: ""
        color: linkArea.containsMouse ? "#5dade2" : "#3498db"
        font.pixelSize: 12
        font.underline: linkArea.containsMouse

        MouseArea {
            id: linkArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: Qt.openUrlExternally(linkRoot.url)
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        // ── Panes ─────────────────────────────────────────────────────────
        StackLayout {
            currentIndex: wizardRoot.pane
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 36

            // 0 — Welcome
            ColumnLayout {
                spacing: 18

                Text {
                    text: "Welcome to Vortex"
                    color: "white"
                    font.pixelSize: 30
                    font.bold: true
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#999"
                    font.pixelSize: 14
                    lineHeight: 1.3
                    text: "Your Steam library, playtime tracking and recommendations "
                        + "already work. Nothing below is required — you can close this "
                        + "and start using Vortex right now."
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#252525"
                }

                Text {
                    text: "Two optional, free accounts add:"
                    color: "#bbb"
                    font.pixelSize: 13
                    font.bold: true
                }

                ColumnLayout {
                    spacing: 10
                    Repeater {
                        model: [
                            { icon: "🖼", title: "Cover art, hero images and logos",
                              body: "SteamGridDB. Without it, games show a plain title card." },
                            { icon: "🧭", title: "Discover — games you don't own yet",
                              body: "IGDB. Without it, recommendations cover your own library only." }
                        ]
                        delegate: RowLayout {
                            id: benefitRow
                            required property var modelData
                            spacing: 12
                            Layout.fillWidth: true

                            Text {
                                text: benefitRow.modelData.icon
                                font.pixelSize: 20
                                Layout.alignment: Qt.AlignTop
                            }
                            ColumnLayout {
                                spacing: 2
                                Layout.fillWidth: true
                                Text {
                                    text: benefitRow.modelData.title
                                    color: "white"
                                    font.pixelSize: 14
                                }
                                Text {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: benefitRow.modelData.body
                                    color: "#777"
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "You can do this later from Settings at any time."
                    color: "#555"
                    font.pixelSize: 12
                }
            }

            // 1 — Credentials
            ColumnLayout {
                spacing: 16

                Text {
                    text: "API keys"
                    color: "white"
                    font.pixelSize: 26
                    font.bold: true
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "Both are free. Fill in either, both, or neither."
                    color: "#999"
                    font.pixelSize: 13
                }

                ColumnLayout {
                    spacing: 6
                    Layout.fillWidth: true

                    RowLayout {
                        spacing: 8
                        Text {
                            text: "IGDB — via Twitch"
                            color: "#888"
                            font.pixelSize: 12
                            font.bold: true
                            font.letterSpacing: 1
                        }
                        ExternalLink {
                            text: "create an app ↗"
                            url: wizardRoot.igdbUrl
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: "Set the OAuth redirect URL to http://localhost — it is not used. "
                            + "The client secret is shown only once, at creation."
                        color: "#666"
                        font.pixelSize: 11
                    }
                }

                LabelledField {
                    id: idField
                    label: "Client ID"
                    hint: "paste your Twitch application client id"
                    alreadySet: wizardRoot.api.credentialStatus().igdb
                }
                LabelledField {
                    id: secretField
                    label: "Client Secret"
                    hint: "paste your Twitch application client secret"
                    echoMode: TextInput.Password
                    alreadySet: wizardRoot.api.credentialStatus().igdb
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#252525"
                }

                RowLayout {
                    spacing: 8
                    Text {
                        text: "STEAMGRIDDB"
                        color: "#888"
                        font.pixelSize: 12
                        font.bold: true
                        font.letterSpacing: 1
                    }
                    ExternalLink {
                        text: "get your key ↗"
                        url: wizardRoot.sgdbUrl
                    }
                }

                LabelledField {
                    id: sgdbField
                    label: "API Key"
                    hint: "paste your SteamGridDB api key"
                    echoMode: TextInput.Password
                    alreadySet: wizardRoot.api.credentialStatus().steamgriddb
                }

                Item { Layout.fillHeight: true }
            }

            // 2 — Catalog
            ColumnLayout {
                spacing: 18

                Text {
                    text: "Build the Discover catalog"
                    color: "white"
                    font.pixelSize: 26
                    font.bold: true
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: 1.3
                    color: "#999"
                    font.pixelSize: 14
                    text: "Vortex can download around 5,700 games from IGDB to recommend "
                        + "titles you don't already own. This takes several minutes and "
                        + "only needs doing once."
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: !wizardRoot.api.credentialStatus().igdb
                    text: "IGDB credentials are not set, so this step is unavailable. "
                        + "Your own library is still recommended normally."
                    color: "#e67e22"
                    font.pixelSize: 12
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#252525"
                }

                RowLayout {
                    spacing: 12
                    visible: wizardRoot.api.credentialStatus().igdb

                    Rectangle {
                        Layout.preferredWidth: 190
                        Layout.preferredHeight: 42
                        radius: 10
                        color: wizardRoot.api.isCatalogRefreshing()
                                   ? "#1a1a1a"
                                   : (catalogArea.containsMouse ? "#ffffff" : "#1a1a1a")
                        border.width: 1
                        border.color: wizardRoot.api.isCatalogRefreshing()
                                          ? "#333"
                                          : (catalogArea.containsMouse ? "#ffffff" : "#333")

                        Text {
                            anchors.centerIn: parent
                            text: wizardRoot.api.isCatalogRefreshing()
                                      ? "Downloading…"
                                      : "Download catalog"
                            font.pixelSize: 14
                            font.bold: true
                            color: wizardRoot.api.isCatalogRefreshing()
                                       ? "#777"
                                       : (catalogArea.containsMouse ? "#000000" : "#ffffff")
                        }

                        MouseArea {
                            id: catalogArea
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: !wizardRoot.api.isCatalogRefreshing()
                            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: {
                                wizardRoot.statusIsError = false;
                                wizardRoot.statusMessage =
                                    "Downloading. You can keep using Vortex — "
                                    + "this runs in the background.";
                                wizardRoot.api.refreshCatalog();
                            }
                        }
                    }

                    BusyIndicator {
                        running: wizardRoot.api.isCatalogRefreshing()
                        visible: running
                        implicitWidth: 28
                        implicitHeight: 28
                    }
                }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "Skipping is fine — Discover stays sparse until you run this "
                        + "from Settings later."
                    color: "#555"
                    font.pixelSize: 12
                }

                Item { Layout.fillHeight: true }
            }
        }

        // ── Status line ───────────────────────────────────────────────────
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 36
            Layout.rightMargin: 36
            Layout.bottomMargin: 8
            visible: wizardRoot.statusMessage !== ""
            text: wizardRoot.statusMessage
            wrapMode: Text.WordWrap
            color: wizardRoot.statusIsError ? "#e74c3c" : "#27ae60"
            font.pixelSize: 12
        }

        // ── Footer ────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 72
            color: "#0d0d0d"
            radius: 20

            // The radius above rounds all four corners; cover the top two so
            // the footer reads as joined to the pane rather than floating.
            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 20
                color: "#0d0d0d"
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 36
                anchors.rightMargin: 36
                spacing: 12

                // Step dots
                Row {
                    spacing: 6
                    Repeater {
                        model: 3
                        delegate: Rectangle {
                            id: stepDot
                            required property int index
                            width: 7
                            height: 7
                            radius: 4
                            color: stepDot.index === wizardRoot.pane ? "#ffffff" : "#333"
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                // Skip — a peer of Continue, not fine print. The whole wizard
                // is optional and the UI should not pretend otherwise.
                Rectangle {
                    Layout.preferredWidth: 96
                    Layout.preferredHeight: 40
                    radius: 10
                    color: skipArea.containsMouse ? "#1f1f1f" : "transparent"
                    border.width: 1
                    border.color: "#333"

                    Text {
                        anchors.centerIn: parent
                        text: wizardRoot.pane === 2 ? "Skip" : "Skip all"
                        color: "#999"
                        font.pixelSize: 13
                    }
                    MouseArea {
                        id: skipArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: wizardRoot.finish()
                    }
                }

                Rectangle {
                    visible: wizardRoot.pane > 0
                    Layout.preferredWidth: 88
                    Layout.preferredHeight: 40
                    radius: 10
                    color: backArea.containsMouse ? "#1f1f1f" : "transparent"
                    border.width: 1
                    border.color: "#333"

                    Text {
                        anchors.centerIn: parent
                        text: "Back"
                        color: "#999"
                        font.pixelSize: 13
                    }
                    MouseArea {
                        id: backArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            wizardRoot.statusMessage = "";
                            wizardRoot.pane = wizardRoot.pane - 1;
                        }
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 130
                    Layout.preferredHeight: 40
                    radius: 10
                    color: nextArea.containsMouse ? "#ffffff" : "#1a1a1a"
                    border.width: 1
                    border.color: nextArea.containsMouse ? "#ffffff" : "#333"

                    Text {
                        anchors.centerIn: parent
                        text: wizardRoot.pane === 2 ? "Finish" : "Continue"
                        color: nextArea.containsMouse ? "#000000" : "#ffffff"
                        font.pixelSize: 14
                        font.bold: true
                    }
                    MouseArea {
                        id: nextArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (wizardRoot.pane === 0) {
                                wizardRoot.statusMessage = "";
                                wizardRoot.pane = 1;
                            } else if (wizardRoot.pane === 1) {
                                wizardRoot.saveAndAdvance();
                            } else {
                                wizardRoot.finish();
                            }
                        }
                    }
                }
            }
        }
    }
}
