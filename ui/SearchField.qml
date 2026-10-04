import QtQuick
import QtQuick.Controls
import Vortex

// The pill-shaped search box shared by the top bar's library filter and the
// Browse tab. Shaped like the tab and filter pills it sits beside, so it reads
// as one more control in the row rather than a form field dropped into it.
//
// Keyboard only, deliberately: the controller has no text entry, and the pad
// keeps steering the grid underneath while this holds the query.
Rectangle {
    id: field

    property alias text: input.text
    property alias placeholderText: placeholder.text
    property int fontSize: 13

    // Enter. Browse uses it to search now instead of waiting out the debounce.
    signal accepted()

    function focusField() {
        input.forceActiveFocus()
        input.selectAll()
    }

    function clear() {
        input.text = ""
    }

    readonly property bool hovered: hoverArea.containsMouse

    implicitWidth: 260
    implicitHeight: 35
    radius: height / 2
    color: input.activeFocus ? Theme.bgEmphasis : (field.hovered ? Theme.bgEmphasis : Theme.bgRaised)
    border.width: input.activeFocus ? 2 : 1
    border.color: input.activeFocus ? Theme.focusRing
                                    : (field.hovered ? Theme.borderStrong : Theme.borderControl)

    Behavior on color { ColorAnimation { duration: 150 } }

    // Under the input, so a click anywhere on the pill -- the icon, the
    // padding -- lands the caret rather than doing nothing.
    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.IBeamCursor
        onClicked: input.forceActiveFocus()
    }

    Text {
        id: icon
        anchors { left: parent.left; leftMargin: 14; verticalCenter: parent.verticalCenter }
        // Segoe's own search glyph, the same font GameDetails uses for its
        // folder icons; MDL2 shares the codepoint on Windows 10.
        text: String.fromCharCode(0xE721)
        font.family: Qt.fontFamilies().indexOf("Segoe Fluent Icons") >= 0
                     ? "Segoe Fluent Icons" : "Segoe MDL2 Assets"
        font.pixelSize: field.fontSize
        color: input.activeFocus ? Theme.textBody : Theme.textMuted
    }

    TextInput {
        id: input
        anchors {
            left: icon.right; leftMargin: 10
            right: clearButton.visible ? clearButton.left : parent.right
            rightMargin: clearButton.visible ? 6 : 16
            verticalCenter: parent.verticalCenter
        }
        clip: true
        color: Theme.textPrimary
        selectionColor: Theme.accent
        selectedTextColor: Theme.textInverse
        font.pixelSize: field.fontSize
        selectByMouse: true

        onAccepted: field.accepted()

        // Esc empties the box first and lets go of the keyboard second, so
        // two presses always get you back to where you were.
        Keys.onEscapePressed: (event) => {
            if (input.text !== "")
                input.text = ""
            else
                input.focus = false
            event.accepted = true
        }

        Text {
            id: placeholder
            anchors.fill: parent
            verticalAlignment: Text.AlignVCenter
            visible: input.text === ""
            color: Theme.textFaint
            font.pixelSize: field.fontSize
            elide: Text.ElideRight
        }
    }

    Rectangle {
        id: clearButton
        readonly property bool hovered: clearArea.containsMouse

        anchors { right: parent.right; rightMargin: 6; verticalCenter: parent.verticalCenter }
        width: parent.height - 12; height: width; radius: width / 2
        visible: input.text !== ""
        color: clearButton.hovered ? Theme.bgActive : "transparent"

        Text {
            anchors.centerIn: parent
            text: "✕"
            font.pixelSize: field.fontSize - 2
            color: clearButton.hovered ? Theme.textPrimary : Theme.textMuted
        }

        MouseArea {
            id: clearArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                input.text = ""
                input.forceActiveFocus()
            }
        }
    }
}
