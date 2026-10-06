import QtQuick
import QtQuick.Shapes

// Gamepad loading animation for the details page: an outlined gamepad whose
// outline a coloured strip chases round three times, while the face buttons
// spin, the stick wanders and the whole pad rocks twice.
//
// Rebuilt as vectors from the "gamedetails_load<N>.webm" clips (239 frames at
// 30 fps, 150 px). Played as a sprite sheet the clip was soft when scaled up
// and took ~40 MB of video memory, so it was reduced to what actually moves.
// The outline is the clip's own centreline, traced from its first frame and
// fitted with BÃ©ziers. Six channels -- the strip's head and tail along that
// outline, the rock, the button spin and the stick's x and y -- were measured
// from every frame and fitted with eased keyframes, so the pad redraws on
// every display refresh, stays sharp at any size and costs no texture memory.
//
// All geometry is in the clip's 150 px frame. The item shows the part of it
// the pad ever reaches, 120 x 105 from (15, 25), scaled to fit.
Item {
    id: pad

    property bool running: true
    // Which of the five clips' colourways to draw, 1-5.
    property int variant: 1

    implicitWidth: 224
    implicitHeight: 196

    // [outline, fill, strip] per clip.
    readonly property var palettes: [
        ["#550000", "#ffffff", "#ff8e3c"],
        ["#0856a0", "#ffffff", "#00ddb6"],
        ["#13290d", "#ffffff", "#ee529d"],
        ["#0856a0", "#ffffff", "#9fadba"],
        ["#4d3500", "#edf7ef", "#c6e7c2"]
    ]
    readonly property var palette: palettes[Math.max(0, Math.min(palettes.length - 1, variant - 1))]
    readonly property color outlineColor: palette[0]
    readonly property color fillColor: palette[1]
    readonly property color stripColor: palette[2]

    // Keyframes per channel, in clip frames. Each row is
    // [frame, value, x1, y1, x2, y2]: the value reached at that frame, and the
    // cubic-bezier easing (as in CSS) of the stretch leading up to it. A
    // channel holds its first value before its first key and its last after.
    readonly property var keys: ({
        head: [
            [0, 0, 0, 0, 1, 1],
            [72, 1, 0.333, 0, 0.667, 1],
            [144, 2, 0.333, 0, 0.667, 1],
            [216, 3, 0.333, 0, 0.667, 1]
        ],
        tail: [
            [21.49, 0, 0, 0, 1, 1],
            [93.61, 1, 0.381, 0.15, 0.619, 0.908],
            [165.35, 2, 0.381, 0.15, 0.619, 0.908],
            [236.97, 3, 0.381, 0.15, 0.619, 0.908]
        ],
        wobble: [
            [23.56, 0, 0, 0, 1, 1],
            [33, 13.99, 0.368, 0.132, 0.819, 0.978],
            [46.57, -13.918, 0.176, 0.012, 0.654, 0.942],
            [56.62, 0, 0.332, -0.088, 0.657, 0.902],
            [158.5, 0, 0, 0, 1, 1],
            [168.28, 13.964, 0.369, 0.119, 0.844, 1.076],
            [181.28, -13.975, 0.223, 0.129, 0.676, 1.034],
            [190.75, 0, 0.34, 0.059, 0.661, 0.939]
        ],
        spin: [
            [0.79, 0, 0, 0, 1, 1],
            [54.56, 360, 0.339, 0.034, 0.819, 0.963],
            [72.52, 360, 0, 0, 1, 1],
            [127, 720, 0.192, 0.047, 0.834, 1],
            [144.53, 720, 0, 0, 1, 1],
            [199.01, 1080, 0.192, 0.047, 0.834, 1]
        ],
        stickX: [
            [1.75, 0, 0, 0, 1, 1],
            [17.24, 2.121, 0.429, 0.295, 0.613, 0.772],
            [19.71, 2.123, 0.734, 0.126, 0.802, 0.347],
            [44.04, -1.344, 0.353, 0.053, 0.667, 0.952],
            [55, -1.813, 0.824, 1.495, 0.662, 0.529],
            [68.76, -1.62, 0.985, 0.752, 0.64, 1.105],
            [82.02, 0, 0.239, 0.21, 0.612, 0.999],
            [145.02, 0, 0, 0, 1, 1],
            [160.31, 2.193, 0.201, 0.051, 0.303, 0.196],
            [164.75, 2.11, 0.061, 1.36, 0.154, 0.481],
            [188, -1.356, 0.436, 0.265, 0.702, 0.962],
            [198.18, -1.743, 0.095, 0.338, 0.892, 1.16],
            [212.24, -1.599, 0.722, -0.478, 0.527, 0.655],
            [225.96, 0, 0.257, 0.05, 0.531, 1]
        ],
        stickY: [
            [1.69, 0, 0, 0, 1, 1],
            [17.06, -2.474, 0.458, 0.312, 0.588, 0.824],
            [19.97, -2.39, 0.122, 1.193, 0.468, 0.437],
            [43.07, 3.149, 0.336, 0.102, 0.658, 1.021],
            [55, 0.959, 0.317, -0.151, 0.781, 0.578],
            [68.56, -1.524, 0.175, 0.192, 0.615, 1.209],
            [81.22, 0, 0.214, -0.58, 0.562, 0.999],
            [145.09, 0, 0, 0, 1, 1],
            [161.06, -2.432, 0.237, -0.007, 0.905, 1.217],
            [163.94, -2.385, 0.766, -1.498, 0.042, 1.823],
            [188.35, 3.141, 0.336, 0.099, 0.646, 1.113],
            [200.96, 0.38, 0.262, -0.084, 0.757, 0.652],
            [212.3, -1.579, 0.282, 0.435, 0.624, 1.101],
            [225.52, 0, 0.259, -0.476, 0.549, 0.992]
        ]
    })

    // Playhead in clip frames, 0..239.
    readonly property int frameCount: 239
    property real t: 0
    NumberAnimation on t {
        from: 0; to: pad.frameCount
        duration: 7967                  // the clip's length
        loops: Animation.Infinite
        running: pad.running && pad.visible
    }

    function ease(x, x1, y1, x2, y2) {
        // Solve bx(s) = x by Newton's method, then return by(s).
        let s = x
        for (let i = 0; i < 8; i++) {
            const bx = 3 * (1 - s) * (1 - s) * s * x1 + 3 * (1 - s) * s * s * x2 + s * s * s - x
            const dx = 3 * (1 - s) * (1 - s) * x1 + 6 * (1 - s) * s * (x2 - x1) + 3 * s * s * (1 - x2)
            if (Math.abs(bx) < 1e-5 || dx < 1e-6)
                break
            s = Math.max(0, Math.min(1, s - bx / dx))
        }
        return 3 * (1 - s) * (1 - s) * s * y1 + 3 * (1 - s) * s * s * y2 + s * s * s
    }

    function sample(k, t) {
        if (t <= k[0][0])
            return k[0][1]
        for (let i = 1; i < k.length; i++) {
            const b = k[i]
            if (t <= b[0]) {
                const a = k[i - 1]
                return a[1] + (b[1] - a[1]) * ease((t - a[0]) / (b[0] - a[0]), b[2], b[3], b[4], b[5])
            }
        }
        return k[k.length - 1][1]
    }

    readonly property real head:   sample(keys.head, t)
    readonly property real tail:   sample(keys.tail, t)
    readonly property real rock:   sample(keys.wobble, t)
    readonly property real spin:   sample(keys.spin, t)
    readonly property real stickX: sample(keys.stickX, t)
    readonly property real stickY: sample(keys.stickY, t)

    Item {
        id: stage
        width: 120; height: 105
        anchors.centerIn: parent
        scale: Math.min(pad.width / width, pad.height / height)

        Item {
            id: body
            x: -15; y: -25
            width: 150; height: 150
            transform: Rotation { origin.x: 74.89; origin.y: 76.95; angle: pad.rock }

            Shape {
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer

                ShapePath {
                    strokeColor: pad.outlineColor
                    strokeWidth: 5.58
                    fillColor: pad.fillColor
                    joinStyle: ShapePath.RoundJoin
                    PathSvg { id: outlinePath; path: "M 75.36 86.29 C 73.54 86.29 71.3 87.02 69.65 87.86 C 67.68 88.86 68 88.24 61.65 92.13 C 58.71 93.93 53.47 97.76 50.67 99.88 C 48.68 101.39 45.62 103.76 43.73 104.89 C 42.69 105.51 41.03 106.95 37.26 107.05 C 28.57 107.28 27.82 94.63 28.31 86.7 C 28.58 82.39 29.1 78.07 29.87 73.82 C 30.88 68.3 32.99 61.81 35.58 56.68 C 38.89 50.11 43.86 43.31 50.37 39.95 C 56.27 36.91 57.78 40.24 63.98 41.7 C 67.72 42.59 67.91 42.91 72.47 43.1 C 73.71 43.15 73.53 43.23 75.36 43.23 C 77.19 43.23 77.01 43.15 78.25 43.1 C 82.81 42.91 83 42.59 86.74 41.7 C 92.94 40.24 94.45 36.91 100.35 39.95 C 106.85 43.31 111.83 50.11 115.14 56.68 C 117.72 61.81 119.84 68.3 120.85 73.82 C 121.62 78.07 122.14 82.39 122.4 86.7 C 122.9 94.63 122.15 107.28 113.46 107.05 C 109.69 106.95 108.03 105.51 106.99 104.89 C 105.1 103.76 102.04 101.39 100.04 99.88 C 97.24 97.76 92.01 93.93 89.06 92.13 C 82.72 88.24 83.04 88.86 81.07 87.86 C 79.42 87.02 77.18 86.29 75.36 86.29 Z" }
                }

                // The strip runs from tail to head along the outline, which
                // starts at the bottom centre and goes clockwise. trim.end
                // does not wrap past the path's end but trim.offset does, so
                // the strip is placed by offset.
                ShapePath {
                    strokeColor: pad.stripColor
                    strokeWidth: 4.98
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    joinStyle: ShapePath.RoundJoin
                    trim.start: 0
                    trim.end: Math.max(0, pad.head - pad.tail)
                    trim.offset: pad.tail - Math.floor(pad.tail)
                    PathSvg { path: outlinePath.path }
                }
            }

            // Stick: a dark base whose white cap wanders inside it. Where the
            // cap reaches past the base it simply merges with the body fill.
            Shape {
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: "transparent"
                    fillColor: pad.outlineColor
                    PathAngleArc { centerX: 55.13; centerY: 65.86; radiusX: 11.08; radiusY: 11.08; sweepAngle: 360 }
                }
            }
            Shape {
                x: pad.stickX; y: pad.stickY
                width: 150; height: 150
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: "transparent"
                    fillColor: pad.fillColor
                    PathAngleArc { centerX: 55.13; centerY: 65.86; radiusX: 6.98; radiusY: 6.98; sweepAngle: 360 }
                }
            }

            // Face buttons: two large and two small on the diagonals, spun as
            // one about their centre.
            Shape {
                x: 98.36 - width / 2; y: 66.19 - height / 2
                width: 30; height: 30
                rotation: pad.spin
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: "transparent"
                    fillColor: pad.outlineColor
                    PathAngleArc { centerX: 9.847; centerY: 20.153; radiusX: 4.622; radiusY: 4.622; sweepAngle: 360 }
                    PathAngleArc { centerX: 20.153; centerY: 9.847; radiusX: 4.622; radiusY: 4.622; sweepAngle: 360 }
                    PathAngleArc { centerX: 20.153; centerY: 20.153; radiusX: 3.473; radiusY: 3.473; sweepAngle: 360 }
                    PathAngleArc { centerX: 9.847; centerY: 9.847; radiusX: 3.473; radiusY: 3.473; sweepAngle: 360 }
                }
            }
        }
    }
}
