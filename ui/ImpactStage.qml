import QtQuick
import QtQuick.Effects

// The launcher's own layers -- glows, header backdrop, page -- and the
// choreography a details page plays on them as it opens, in the manner of
// Steam's Big Picture:
//
//   impact    the whole launcher sinks inward around the card that was picked
//   black     a quick dip to black, held for a beat
//   reveal    the page lands on the black
//   blur      once it has, the black slowly lifts part way behind it, onto
//             the launcher already frosted
//
// The launcher stays pushed in, blurred and shaded for as long as the page is
// up; release() unwinds it when the page closes.
//
// The blur is fully built while the screen is black, so the launcher never
// comes back sharp and then blurs a second time: what fades up out of the
// black is already the frosted backdrop the page sits on.
//
// The details pages are popups, so they draw in the window's overlay rather
// than in here, and none of this touches them. They time their own entrance
// off revealAt. They also turn their own modal dim off: it snapped on at the
// click, ahead of all of this, and restShade below does its job instead.
Item {
    id: stage

    default property alias content: scene.data

    // How far the launcher sinks on impact. Below 1 pushes it away from the
    // viewer; above 1 would zoom in on the card instead.
    property real pushDepth: 0.955

    property int impactDuration: 180
    property int blackDelay: 60
    property int blackInDuration: 160
    // How long the screen sits fully black before the page lands. The blur
    // is built in this time, so it must not be cut too short.
    property int blackHold: 140
    // How long after the page starts landing the backdrop starts to come up,
    // so the page is on screen first.
    property int backdropDelay: 300
    // How long the black takes to lift to restShade: the slow, gradual part.
    property int liftDuration: 600
    // How dark the launcher stays behind an open page.
    property real restShade: 0.45
    property int releaseDuration: 320

    readonly property int blackPeakAt:
        Math.max(stage.impactDuration, stage.blackDelay + stage.blackInDuration)

    // The moment the page starts to land.
    readonly property int revealAt: stage.blackPeakAt + stage.blackHold

    // What the animations drive. Only ever moved by engage() and release().
    property real push: 1.0
    property real blur: 0.0
    property real black: 0.0

    // The point the launcher sinks toward. Taken from the card just before a
    // page opens; the centre for anything opened without one.
    property real originX: stage.width / 2
    property real originY: stage.height / 2
    property var pendingOrigin: null

    // Called by whatever is about to open a page, with the card it opens from.
    function aimAt(card) {
        if (!card) {
            stage.pendingOrigin = null
            return
        }
        stage.pendingOrigin = card.mapToItem(stage, card.width / 2, card.height / 2)
    }

    function engage() {
        releaseAnim.stop()
        // Only re-aimed from rest: moving the origin while the launcher is
        // still pushed in would jump it sideways.
        if (stage.push === 1.0) {
            stage.originX = stage.pendingOrigin ? stage.pendingOrigin.x : stage.width / 2
            stage.originY = stage.pendingOrigin ? stage.pendingOrigin.y : stage.height / 2
        }
        stage.pendingOrigin = null
        engageAnim.restart()
    }

    // Safe mid-sequence: a page closed during its own entrance unwinds from
    // wherever the launcher had got to.
    function release() {
        engageAnim.stop()
        stage.pendingOrigin = null
        releaseAnim.restart()
    }

    Item {
        id: scene
        anchors.fill: parent

        transform: Scale {
            origin.x: stage.originX
            origin.y: stage.originY
            xScale: stage.push
            yScale: stage.push
        }

        // Only layered while there is blur to draw, so the launcher costs
        // nothing extra with no page open.
        layer.enabled: stage.blur > 0
        layer.smooth: true
        layer.effect: MultiEffect {
            blurEnabled: true
            blur: stage.blur
            blurMax: 48
            // Unpadded, so the blur clamps at the window's edges instead of
            // fading them to transparent.
            autoPaddingEnabled: false
        }
    }

    // Over the launcher, under the overlay the pages draw in.
    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: stage.black
        visible: opacity > 0
    }

    SequentialAnimation {
        id: engageAnim

        ParallelAnimation {
            NumberAnimation {
                target: stage; property: "push"; to: stage.pushDepth
                duration: stage.impactDuration; easing.type: Easing.OutCubic
            }
            SequentialAnimation {
                PauseAnimation { duration: stage.blackDelay }
                NumberAnimation {
                    target: stage; property: "black"; to: 1.0
                    duration: stage.blackInDuration; easing.type: Easing.InOutQuad
                }
            }
        }
        // Built in full behind the black, unseen.
        NumberAnimation {
            target: stage; property: "blur"; to: 1.0
            duration: stage.blackHold
        }
        // The page is landing on the black meanwhile (revealAt).
        PauseAnimation { duration: stage.backdropDelay }
        // Eased at both ends, so the launcher creeps up out of the black
        // rather than jumping most of the way in the first few frames.
        NumberAnimation {
            target: stage; property: "black"; to: stage.restShade
            duration: stage.liftDuration; easing.type: Easing.InOutSine
        }
    }

    ParallelAnimation {
        id: releaseAnim

        NumberAnimation {
            target: stage; property: "push"; to: 1.0
            duration: stage.releaseDuration; easing.type: Easing.OutCubic
        }
        NumberAnimation {
            target: stage; property: "blur"; to: 0.0
            duration: stage.releaseDuration - 60; easing.type: Easing.OutQuad
        }
        NumberAnimation {
            target: stage; property: "black"; to: 0.0
            duration: stage.releaseDuration - 60; easing.type: Easing.OutQuad
        }
    }
}
