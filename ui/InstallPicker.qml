pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import Vortex

// The launcher's own "install to" step, standing in for Steam's dialog.
//
// Lists Steam's libraries with their free space, the default one starred and
// selected, and hands the choice to VortexBridge::installSteamGameTo(), which
// passes it to the running Steam client -- so Steam never comes to the front.
// When that route is unavailable (Steam closed, or its internals not what we
// expect) Install opens Steam's own dialog instead and says so up front.
//
// With a single library there is nothing to choose and the install starts
// straight away.
Item {
    id: picker

    property var api: vortexApi

    property int appid: 0
    property string gameName: ""
    property string coverSource: ""

    property var libraries: []
    property bool loading: false
    property bool clientRoute: true
    property int selected: -1      // index into libraries

    readonly property bool visibleNow: popup.visible

    function openFor(appid, name, cover) {
        if (!picker.api || appid <= 0)
            return
        picker.appid = appid
        picker.gameName = name || ""
        picker.coverSource = cover || ""
        picker.libraries = []
        picker.selected = -1
        picker.loading = true
        popup.open()
        picker.api.requestSteamLibraries()
    }

    function close() { popup.close() }

    function install() {
        if (picker.loading || !picker.api)
            return
        if (!picker.clientRoute || picker.selected < 0) {
            picker.api.installSteamGame(picker.appid)       // Steam's own dialog
        } else {
            const lib = picker.libraries[picker.selected]
            picker.api.installSteamGameTo(picker.appid, lib.folderIndex)
        }
        popup.close()
    }

    // Controller: up/down through the drives, A installs, B cancels.
    function navigate(direction) {
        if (picker.libraries.length === 0)
            return
        if (direction === "up")
            picker.selected = Math.max(0, picker.selected - 1)
        else if (direction === "down")
            picker.selected = Math.min(picker.libraries.length - 1, picker.selected + 1)
    }
    function accept() { picker.install() }

    function formatBytes(bytes) {
        const gb = bytes / (1024 * 1024 * 1024)
        if (gb >= 1000)
            return (gb / 1024).toFixed(2) + " TB"
        return gb.toFixed(2) + " GB"
    }

    Connections {
        target: picker.api
        function onSteamLibrariesReady(libraries, clientRoute) {
            if (!popup.visible)
                return
            picker.libraries = libraries
            picker.clientRoute = clientRoute
            picker.loading = false
            let pick = 0
            for (let i = 0; i < libraries.length; ++i) {
                if (libraries[i].isDefault) { pick = i; break }
            }
            picker.selected = libraries.length > 0 ? pick : -1
            // Nothing to choose between.
            if (libraries.length === 1 && clientRoute)
                picker.install()
        }
    }

    Popup {
        id: popup
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(680, (parent ? parent.width : 680) - 80)
        z: 90
        modal: true
        dim: true
        padding: 28
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color: Theme.bgSurface
            radius: 12
            border.color: Theme.borderQuiet
            border.width: 1
        }

        contentItem: Column {
            spacing: 18

            Text {
                text: "Install"
                color: Theme.textPrimary
                font.pixelSize: 26
                font.bold: true
            }

            // The game, as Steam's dialog shows it.
            Row {
                spacing: 18
                Rectangle {
                    width: 80; height: 120; radius: 6
                    color: Theme.bgRaised
                    clip: true
                    Image {
                        anchors.fill: parent
                        source: picker.coverSource
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize.width: 160
                        sourceSize.height: 240
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: popup.availableWidth - 98
                    text: picker.gameName
                    color: Theme.textBody
                    font.pixelSize: 20
                    elide: Text.ElideRight
                }
            }

            Rectangle { width: popup.availableWidth; height: 1; color: Theme.divider }

            Text {
                text: "INSTALL TO:"
                color: Theme.textSecondary
                font.pixelSize: 13
                font.bold: true
                font.letterSpacing: 1.2
            }

            Text {
                visible: picker.loading
                text: "Checking your Steam libraries…"
                color: Theme.textMuted
                font.pixelSize: 14
            }

            Column {
                visible: !picker.loading
                spacing: 8
                Repeater {
                    model: picker.libraries
                    delegate: Rectangle {
                        id: driveRow
                        required property var modelData
                        required property int index
                        readonly property bool chosen: picker.selected === driveRow.index

                        width: popup.availableWidth
                        height: 56
                        radius: 4
                        color: driveRow.chosen ? Theme.installWave
                             : driveArea.containsMouse ? Theme.bgActive : Theme.bgRaised
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 20
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 260
                            elide: Text.ElideRight
                            text: (driveRow.modelData.label ? driveRow.modelData.label : "Local Drive")
                                  + " (" + driveRow.modelData.drive + ")"
                            color: Theme.textPrimary
                            font.pixelSize: 17
                        }
                        Row {
                            anchors.right: parent.right
                            anchors.rightMargin: 20
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 14
                            Text {
                                visible: driveRow.modelData.isDefault
                                text: "★"
                                color: Theme.textPrimary
                                font.pixelSize: 16
                            }
                            Text {
                                text: picker.formatBytes(driveRow.modelData.freeBytes) + " FREE"
                                color: Theme.textPrimary
                                font.pixelSize: 13
                                font.bold: true
                            }
                        }

                        MouseArea {
                            id: driveArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: picker.selected = driveRow.index
                            onDoubleClicked: {
                                picker.selected = driveRow.index
                                picker.install()
                            }
                        }
                    }
                }
            }

            // Said before the click, not after: the fallback is Steam's own
            // window, which is exactly what this picker exists to avoid.
            Text {
                visible: !picker.loading && !picker.clientRoute
                width: popup.availableWidth
                wrapMode: Text.WordWrap
                text: "Steam isn't reachable from the launcher right now, so Install will open Steam's own install window."
                color: Theme.caution
                font.pixelSize: 13
            }

            Row {
                spacing: 16
                anchors.horizontalCenter: parent.horizontalCenter

                Rectangle {
                    width: 240; height: 44; radius: 4
                    opacity: picker.loading ? 0.5 : 1.0
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: installArea.containsMouse ? Theme.installWave : "#47a3f0" }
                        GradientStop { position: 1.0; color: Theme.installWaveDeep }
                    }
                    Text {
                        anchors.centerIn: parent
                        text: "Install"
                        color: Theme.textPrimary
                        font.pixelSize: 16
                    }
                    MouseArea {
                        id: installArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: picker.loading ? Qt.BusyCursor : Qt.PointingHandCursor
                        onClicked: picker.install()
                    }
                }
                Rectangle {
                    width: 240; height: 44; radius: 4
                    color: cancelArea.containsMouse ? Theme.bgActive : Theme.bgRaised
                    Text {
                        anchors.centerIn: parent
                        text: "Cancel"
                        color: Theme.textPrimary
                        font.pixelSize: 16
                    }
                    MouseArea {
                        id: cancelArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: popup.close()
                    }
                }
            }
        }
    }
}
