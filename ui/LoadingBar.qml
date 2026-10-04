import QtQuick
import Vortex

// Indeterminate progress bar: a pill that bounces along a track, trailing two
// fainter echoes of itself.
//
// Rebuilt as vectors from the "loading bar.webm" clip (60 frames at 24 fps).
// Playing those frames as a sprite capped the motion at 24 fps, and a faster
// frame rate would only have sped the bounce up — there were no in-between
// frames to show. So the clip was reduced to what actually moves: three
// nested spans (light echo, mid echo, solid pill), each just a left and a
// right edge. The keyframes below are those edges measured from every frame
// of the clip; between keyframes they are interpolated, so the bar redraws on
// every display refresh — 60, 144, whatever the monitor runs at — with the
// clip's timing intact.
//
// Colours come from Theme like everything else: the echoes are the pill
// colour laid over the track at the clip's own strengths.
Item {
    id: bar

    property bool running: true
    property color trackColor: Theme.textMuted
    property color pillColor:  Theme.steamAccent

    implicitWidth: 117
    implicitHeight: 6

    // One row per clip frame: [lightL, lightR, midL, midR, solidL, solidR],
    // as fractions of the track's length.
    readonly property var keys: [
        [0, 0.177, 0, 0.177, 0, 0.177],
        [0, 0.177, 0, 0.177, 0, 0.177],
        [0, 0.19, 0, 0.1777, 0, 0.1777],
        [0, 0.2092, 0, 0.1781, 0, 0.1781],
        [0, 0.235, 0, 0.1949, 0, 0.1804],
        [0, 0.2668, 0, 0.2128, 0, 0.1802],
        [0, 0.303, 0, 0.239, 0, 0.1951],
        [0, 0.3436, 0, 0.2673, 0.011, 0.2137],
        [0.0211, 0.3864, 0.0211, 0.3037, 0.0211, 0.2394],
        [0.0383, 0.4362, 0.0383, 0.3445, 0.0383, 0.2685],
        [0.064, 0.4828, 0.064, 0.3874, 0.064, 0.3077],
        [0.098, 0.5342, 0.098, 0.4372, 0.098, 0.347],
        [0.1326, 0.5856, 0.1326, 0.4868, 0.1326, 0.3903],
        [0.1749, 0.6358, 0.1749, 0.5361, 0.1749, 0.4387],
        [0.2184, 0.6864, 0.2184, 0.5875, 0.2184, 0.4868],
        [0.2559, 0.7361, 0.267, 0.6389, 0.267, 0.5382],
        [0.3123, 0.7825, 0.3123, 0.6863, 0.3123, 0.5871],
        [0.3636, 0.8253, 0.3636, 0.7377, 0.3636, 0.6385],
        [0.4149, 0.8663, 0.4149, 0.7859, 0.4149, 0.6868],
        [0.4663, 0.9022, 0.4663, 0.8264, 0.4663, 0.7366],
        [0.5175, 0.9338, 0.5175, 0.867, 0.5175, 0.7835],
        [0.5565, 0.9597, 0.5678, 0.9059, 0.5678, 0.8288],
        [0.612, 0.9783, 0.612, 0.9342, 0.612, 0.8672],
        [0.6552, 1, 0.6552, 0.9604, 0.6552, 0.9036],
        [0.6977, 1, 0.6977, 0.98, 0.6977, 0.9359],
        [0.7324, 1, 0.7324, 1, 0.7324, 0.9616],
        [0.766, 1, 0.766, 1, 0.766, 0.9803],
        [0.7917, 1, 0.7917, 1, 0.7917, 1],
        [0.8091, 1, 0.8091, 1, 0.8091, 1],
        [0.8186, 1, 0.8186, 1, 0.8186, 1],
        [0.8264, 1, 0.8264, 1, 0.8264, 1],
        [0.8264, 1, 0.8264, 1, 0.8264, 1],
        [0.8264, 1, 0.8264, 1, 0.8264, 1],
        [0.8261, 1, 0.8261, 1, 0.8261, 1],
        [0.8057, 1, 0.8252, 1, 0.8252, 1],
        [0.7813, 1, 0.8246, 1, 0.8246, 1],
        [0.7531, 1, 0.8031, 1, 0.8234, 1],
        [0.7169, 1, 0.7792, 1, 0.8169, 1],
        [0.6751, 1, 0.7523, 1, 0.8042, 1],
        [0.6268, 1, 0.7133, 1, 0.7808, 1],
        [0.5771, 0.9739, 0.6705, 0.9739, 0.7481, 0.9739],
        [0.5245, 0.9616, 0.6261, 0.9499, 0.7139, 0.9499],
        [0.4695, 0.9225, 0.5747, 0.9225, 0.671, 0.9225],
        [0.4134, 0.8878, 0.5238, 0.8878, 0.6263, 0.8878],
        [0.3576, 0.8454, 0.4685, 0.8454, 0.575, 0.8454],
        [0.3036, 0.8067, 0.4124, 0.7955, 0.5234, 0.7955],
        [0.2496, 0.7491, 0.3571, 0.7491, 0.4679, 0.7491],
        [0.1998, 0.7042, 0.301, 0.6929, 0.4121, 0.6929],
        [0.1536, 0.6398, 0.2491, 0.6398, 0.3565, 0.6398],
        [0.1122, 0.593, 0.1977, 0.5814, 0.3027, 0.5814],
        [0.0757, 0.5284, 0.1526, 0.5284, 0.2495, 0.5284],
        [0.0453, 0.4741, 0.1119, 0.4741, 0.1981, 0.4741],
        [0.0235, 0.4283, 0.0734, 0.4177, 0.1515, 0.4177],
        [0.0093, 0.3793, 0.0442, 0.3698, 0.1114, 0.3698],
        [0, 0.3229, 0.0207, 0.3229, 0.0754, 0.3229],
        [0, 0.2801, 0.01, 0.2801, 0.0458, 0.2801],
        [0, 0.2457, 0, 0.2457, 0.0186, 0.2457],
        [0, 0.2242, 0, 0.2131, 0.009, 0.2131],
        [0, 0.1943, 0, 0.1943, 0, 0.1943],
        [0, 0.1774, 0, 0.1774, 0, 0.1774]
    ]
    readonly property int frameCount: keys.length

    // Monotone cubic (Fritsch–Carlson) tangents per edge. Plain Catmull-Rom
    // would overshoot where the pill comes to rest against an end, nudging it
    // past the track; monotone tangents never go beyond a neighbouring key.
    readonly property var tangents: {
        const n = keys.length, out = []
        for (let i = 0; i < n; i++)
            out.push([0, 0, 0, 0, 0, 0])
        for (let c = 0; c < 6; c++) {
            const d = []
            for (let i = 0; i < n; i++)
                d.push(keys[(i + 1) % n][c] - keys[i][c])
            for (let i = 0; i < n; i++) {
                const a = d[(i + n - 1) % n], b = d[i]
                out[i][c] = a * b <= 0 ? 0 : (a + b) / 2
            }
            for (let i = 0; i < n; i++) {
                const j = (i + 1) % n
                if (d[i] === 0) { out[i][c] = 0; out[j][c] = 0; continue }
                const p = out[i][c] / d[i], q = out[j][c] / d[i], s = p * p + q * q
                if (s > 9) {
                    const k = 3 / Math.sqrt(s)
                    out[i][c] = k * p * d[i]
                    out[j][c] = k * q * d[i]
                }
            }
        }
        return out
    }

    // Playhead in clip frames, 0..frameCount; wraps from the last frame
    // straight into the first, which the clip was made to do.
    property real t: 0
    NumberAnimation on t {
        from: 0; to: bar.frameCount
        duration: 2500                  // the clip's length
        loops: Animation.Infinite
        running: bar.running && bar.visible
    }

    readonly property var edges: {
        const i = Math.floor(bar.t) % bar.frameCount, j = (i + 1) % bar.frameCount
        const u = bar.t - Math.floor(bar.t), u2 = u * u, u3 = u2 * u
        const h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u
        const h01 = -2 * u3 + 3 * u2,    h11 = u3 - u2
        const k0 = bar.keys[i], k1 = bar.keys[j]
        const m0 = bar.tangents[i], m1 = bar.tangents[j]
        const out = []
        for (let c = 0; c < 6; c++) {
            const v = h00 * k0[c] + h10 * m0[c] + h01 * k1[c] + h11 * m1[c]
            out.push(Math.max(0, Math.min(1, v)) * bar.width)
        }
        return out
    }

    component Span: Rectangle {
        required property real startX
        required property real endX
        x: startX
        width: Math.max(0, endX - startX)
        height: bar.height
        radius: bar.height / 2
        antialiasing: true
    }

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: bar.trackColor
        antialiasing: true
    }

    Span {
        startX: bar.edges[0]; endX: bar.edges[1]
        color: Qt.tint(bar.trackColor, Qt.rgba(bar.pillColor.r, bar.pillColor.g, bar.pillColor.b, 0.45))
    }
    Span {
        startX: bar.edges[2]; endX: bar.edges[3]
        color: Qt.tint(bar.trackColor, Qt.rgba(bar.pillColor.r, bar.pillColor.g, bar.pillColor.b, 0.74))
    }
    Span {
        startX: bar.edges[4]; endX: bar.edges[5]
        color: bar.pillColor
    }
}
