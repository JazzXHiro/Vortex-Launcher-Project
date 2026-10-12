pragma ComponentBehavior: Bound
import QtQuick
import Vortex

// The light sweep: a vertical line that crosses its parent left to right,
// eased in and out, then rests before the next pass. Everything behind it is
// one blue, lighter and brighter towards the line and deeper and fainter away
// from it, and the trail stretches while the line is fast and shrinks as it
// slows, which is what reads as motion blur. Position, trail length and
// brightness all hang off the line's speed, so they are bindings on one
// looping clock rather than separate animations that would drift apart.
//
// Shared by the download overlay on a game card (InstallSweep.qml) and the
// Install button on a game's details page. Fill the area it should cross; it
// clips to its own bounds, so a rounded parent masks it (RoundedCorners.qml).
Item {
    id: sweep

    property bool running: false
    // How far the trail reaches behind the line, 0.4..2.0.
    property real trailScale: 1.7
    // The line, and the white core on its leading edge.
    property real lineWidth: 16
    property real coreWidth: 5

    clip: true

    readonly property bool moving: sweep.visible && sweep.running

    // ── Clock ────────────────────────────────────────────────────────────
    //
    // The eased sweep is 2.6s end to end, but its slow first and last stretches
    // happen off the area, where the line is hidden before it enters and gone
    // after it leaves. The clock plays only the part in between, from the
    // moment the halo's leading spill reaches the left edge to the moment
    // the halo clears the right one, then rests. So the gap between passes is
    // restTime and nothing more, and neither end can jump: the area is empty
    // at both.
    readonly property real sweepTime: 2.6
    readonly property real restTime: 0.5
    readonly property real enterAt: sweep.uneased((sweep.startOffset - 12) / sweep.span)
    readonly property real exitAt: sweep.uneased((sweep.startOffset + sweep.width + 40) / sweep.span)
    readonly property real loopTime: (sweep.exitAt - sweep.enterAt) * sweep.sweepTime + sweep.restTime
    property real clock: 0
    NumberAnimation on clock {
        from: 0
        to: sweep.loopTime
        duration: sweep.loopTime * 1000
        loops: Animation.Infinite
        running: sweep.moving
    }

    function ease(t) {   // easeInOutQuart
        t = Math.max(0, Math.min(1, t))
        return t < 0.5 ? 8 * t * t * t * t : 1 - Math.pow(-2 * t + 2, 4) / 2
    }
    function uneased(y) {   // ease()'s inverse
        y = Math.max(0, Math.min(1, y))
        return y < 0.5 ? Math.pow(y / 8, 0.25) : 1 - Math.pow(2 * (1 - y), 0.25) / 2
    }

    readonly property real progress: sweep.enterAt + sweep.clock / sweep.sweepTime
    readonly property bool sweeping: sweep.moving && sweep.progress < sweep.exitAt

    // d(ease)/dp, peaking at 4 halfway across.
    readonly property real velocity:
        (sweep.ease(sweep.progress + 0.01) - sweep.ease(sweep.progress - 0.01)) / 0.02

    // The trail lengths are tuned for a 1280-wide card; on a 240-wide one they
    // would put the accents a whole card behind the line, so they scale with
    // the width.
    readonly property real trail:
        (40 + 110 * sweep.velocity) * sweep.trailScale * sweep.width / 1280

    // headX is the line's leading edge. It starts just far enough off the
    // left edge to hide the line, and ends far enough past the right one that
    // the line, its tail and its halo have all left the area -- every pass
    // crosses all of it rather than parking on the edge and fading there.
    // Starting 40px out, as the 1280-wide design does, kept a 240-wide card
    // blank for the first 40% of every pass.
    // Never less than 16, whatever the line's width: it has to clear the
    // halo's 12px spill ahead of the line, and a shorter run-up puts the
    // slowest part of the ease-in on the area -- a thin line then sat still
    // on the left edge for most of half a second before moving.
    readonly property real startOffset: Math.max(sweep.lineWidth, 16)
    readonly property real endOffset: 44   // the halo reaches 40px behind headX
    readonly property real span: sweep.width + sweep.startOffset + sweep.endOffset
    readonly property real headX: -sweep.startOffset + sweep.ease(sweep.progress) * sweep.span

    // A thin line lagging the head by a share of the trail, fainter the
    // further back, and brighter the faster the line is moving.
    component Accent: Rectangle {
        id: accent
        property real lag
        x: sweep.headX - sweep.trail * accent.lag - accent.width / 2
        height: sweep.height
        opacity: Math.max(0.2, Math.min(0.9, sweep.velocity / 2.5)) * (1 - accent.lag * 0.4)
    }

    // ── The sweep ────────────────────────────────────────────────────────
    //
    // Drawn back to front: trail glow, accents, halo, tail, line, core. Every
    // piece behind the line ends at headX, so the leading edge stays crisp
    // and only the left side trails off.
    Item {
        anchors.fill: parent
        visible: sweep.sweeping

        // Full height behind the line, deep and faint at its far end and
        // lightening into the line.
        Rectangle {
            x: sweep.headX - width
            width: sweep.trail * 1.6
            height: parent.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0;  color: "transparent" }
                GradientStop { position: 0.3;  color: Qt.alpha(Theme.sweepBlueDeep, 0.06) }
                GradientStop { position: 0.6;  color: Qt.alpha(Theme.sweepBlueDeep, 0.16) }
                GradientStop { position: 0.85; color: Qt.alpha(Theme.sweepBlue, 0.30) }
                GradientStop { position: 1.0;  color: Qt.alpha(Theme.sweepBlueLight, 0.45) }
            }
        }

        Accent { lag: 1.1;  width: 1; color: Theme.sweepBlueDeep }
        Accent { lag: 0.7;  width: 2; color: Theme.sweepBlue }
        Accent { lag: 0.35; width: 2; color: Theme.sweepBlueLight }

        // A soft halo round the line, as a gradient rather than a blur effect,
        // which would cost a framebuffer per downloading card. Mostly behind
        // the line, with a short spill ahead of it so the leading edge glows
        // rather than cuts.
        Rectangle {
            x: sweep.headX - 40
            width: 52
            height: parent.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0;  color: "transparent" }
                GradientStop { position: 0.35; color: Qt.alpha(Theme.sweepBlue, 0.08) }
                GradientStop { position: 0.6;  color: Qt.alpha(Theme.sweepBlue, 0.22) }
                GradientStop { position: 0.77; color: Qt.alpha(Theme.sweepBlueLight, 0.40) }
                GradientStop { position: 0.85; color: Qt.alpha(Theme.sweepBlueLight, 0.25) }
                GradientStop { position: 0.93; color: Qt.alpha(Theme.sweepBlue, 0.08) }
                GradientStop { position: 1.0;  color: "transparent" }
            }
        }

        // The line's own tail: solid where it meets the line and dissipating
        // to the left, longer the faster the line moves.
        Rectangle {
            x: sweep.headX - line.width - width
            width: 14 + sweep.trail * 0.2
            height: parent.height
            // Roughly quadratic, so it thins out gradually rather than
            // reading as a solid band with a soft edge.
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0;  color: "transparent" }
                GradientStop { position: 0.45; color: Qt.alpha(Theme.sweepBlue, 0.08) }
                GradientStop { position: 0.7;  color: Qt.alpha(Theme.sweepBlue, 0.22) }
                GradientStop { position: 0.88; color: Qt.alpha(Theme.sweepBlueLight, 0.45) }
                GradientStop { position: 1.0;  color: Qt.alpha(Theme.sweepBlueLight, 0.75) }
            }
        }

        // The line: light blue into a pale tint, with a white core on its
        // leading edge.
        Rectangle {
            id: line
            x: sweep.headX - width
            width: sweep.lineWidth
            height: parent.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: Theme.sweepBlueLight }
                GradientStop { position: 1.0; color: Theme.sweepBlueTint }
            }
        }
        Rectangle {
            x: sweep.headX - width
            width: sweep.coreWidth
            height: parent.height
            color: "#ffffff"
        }
    }}
