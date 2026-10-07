pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtMultimedia
import Vortex

// One Steam trailer, streamed and played in place on the details page.
//
// Lives only while the page's trailer lightbox is open: BrowseDetails' Loader
// destroys it on close, which stops the stream and frees the decoder, so a
// closed page holds no video memory. The stream is Steam's HLS one, served
// through TrailerPlaylistServer (see trailer_playlist_server.h).
//
// FFmpeg cannot seek those streams in place, so a seek reloads the stream from
// the chunk holding the moment wanted (api.trailerSeek) and the player counts
// on from that chunk's start: offsetMs plus the stream's own position. Seeks
// land on chunk boundaries, three seconds apart.
Item {
    id: player

    property url source
    property bool startMuted: false
    // The bridge, for trailerSeek(). Without it, seeking falls back to the
    // player's own, which is right for a plain mp4.
    property var api: null

    readonly property bool playing: mediaPlayer.playbackState === MediaPlayer.PlayingState
    property string errorText: ""

    // Where the playing stream starts in the trailer, and the trailer's full
    // length once a seek has told us (a stream starting part-way only knows
    // its own remainder).
    property real offsetMs: 0
    property real totalMs: 0
    readonly property real positionMs: player.offsetMs + mediaPlayer.position
    readonly property real durationMs: player.totalMs > 0 ? player.totalMs
                                                          : player.offsetMs + mediaPlayer.duration

    // Windows' own icon font, as BrowseDetails uses for its clock.
    readonly property string iconFont:
        Qt.fontFamilies().indexOf("Segoe Fluent Icons") >= 0
            ? "Segoe Fluent Icons" : "Segoe MDL2 Assets"

    function togglePlay() {
        if (player.playing) mediaPlayer.pause()
        else if (mediaPlayer.mediaStatus === MediaPlayer.EndOfMedia) player.seekTo(0)
        else mediaPlayer.play()
    }

    function load(url, play) {
        player.errorText = ""
        mediaPlayer.source = ""
        mediaPlayer.source = url
        if (play) mediaPlayer.play()
        else mediaPlayer.pause()
    }

    function seekTo(ms) {
        const jump = player.api ? player.api.trailerSeek(player.source.toString(), ms) : null
        if (!jump || !jump.url) {
            mediaPlayer.setPosition(ms - player.offsetMs)
            return
        }
        const resume = player.playing || mediaPlayer.mediaStatus === MediaPlayer.EndOfMedia
        player.offsetMs = jump.offsetMs
        player.totalMs = jump.durationMs
        player.load(jump.url, resume)
    }

    function timeLabel(ms) {
        const total = Math.max(0, Math.floor(ms / 1000))
        const seconds = total % 60
        return Math.floor(total / 60) + ":" + (seconds < 10 ? "0" : "") + seconds
    }

    // A new trailer starts from the top, playing.
    onSourceChanged: {
        player.offsetMs = 0
        player.totalMs = 0
        player.load(player.source, true)
    }
    // The initial source may or may not have come through onSourceChanged.
    Component.onCompleted: if (mediaPlayer.source.toString() === "") player.load(player.source, true)

    MediaPlayer {
        id: mediaPlayer
        videoOutput: video
        audioOutput: AudioOutput {
            id: audio
            muted: player.startMuted
            volume: 0.8
        }
        onErrorOccurred: (error, errorString) => player.errorText = errorString
    }

    VideoOutput {
        id: video
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectFit
    }

    // Click toggles playback. Also what keeps a click on the video from
    // reaching the lightbox's backdrop, which would close it.
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: player.togglePlay()
    }

    // Controls show while the pointer moves over the player or it is paused,
    // and get out of the way of the picture otherwise.
    property bool controlsShown: true
    HoverHandler {
        id: hover
        onPointChanged: {
            player.controlsShown = true
            hideTimer.restart()
        }
    }
    Timer {
        id: hideTimer
        interval: 2500
        running: player.playing
        onTriggered: player.controlsShown = false
    }

    Text {
        anchors.centerIn: parent
        visible: player.errorText === ""
                 && (mediaPlayer.mediaStatus === MediaPlayer.LoadingMedia
                     || mediaPlayer.mediaStatus === MediaPlayer.BufferingMedia
                     || mediaPlayer.mediaStatus === MediaPlayer.StalledMedia)
        text: "LOADING…"
        color: Theme.textGhost
        font.pixelSize: 14; font.bold: true; font.letterSpacing: 2
    }

    Text {
        anchors.centerIn: parent
        width: parent.width * 0.6
        visible: player.errorText !== ""
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: "This trailer couldn't be played.\n" + player.errorText
        color: Theme.textMuted
        font.pixelSize: 14
    }

    component IconButton: Rectangle {
        id: iconButton
        property string glyph
        signal clicked()
        width: 36; height: 36; radius: 8
        color: iconArea.containsMouse ? Theme.bgEmphasis : "transparent"
        Behavior on color { ColorAnimation { duration: 120 } }
        Text {
            anchors.centerIn: parent
            text: iconButton.glyph
            font.family: player.iconFont
            font.pixelSize: 16
            color: Theme.textPrimary
        }
        MouseArea {
            id: iconArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: iconButton.clicked()
        }
    }

    component BarSlider: Slider {
        id: barSlider
        padding: 0
        implicitHeight: 20
        background: Rectangle {
            x: barSlider.leftPadding
            y: barSlider.topPadding + (barSlider.availableHeight - height) / 2
            width: barSlider.availableWidth
            height: 4; radius: 2
            color: Theme.borderControl
            Rectangle {
                width: barSlider.visualPosition * parent.width
                height: parent.height; radius: 2
                color: Theme.textPrimary
            }
        }
        handle: Rectangle {
            x: barSlider.leftPadding + barSlider.visualPosition * (barSlider.availableWidth - width)
            y: barSlider.topPadding + (barSlider.availableHeight - height) / 2
            width: 12; height: 12; radius: 6
            color: Theme.textPrimary
            visible: barSlider.hovered || barSlider.pressed
        }
        HoverHandler { cursorShape: Qt.PointingHandCursor }
    }

    // ── Control bar ─────────────────────────────────────────────────────────
    Rectangle {
        id: bar
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 52
        radius: 10
        color: Theme.overlayStrong
        opacity: player.controlsShown || !player.playing || barHover.hovered ? 1.0 : 0.0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }

        HoverHandler { id: barHover }
        // Keeps clicks on the bar from toggling playback underneath.
        MouseArea { anchors.fill: parent }

        Row {
            id: leftControls
            anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
            spacing: 4

            // Play / Pause
            IconButton {
                glyph: player.playing ? "" : ""
                onClicked: player.togglePlay()
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: player.timeLabel(player.positionMs) + " / " + player.timeLabel(player.durationMs)
                color: Theme.textBody
                font.pixelSize: 12
                font.features: { "tnum": 1 }
            }
        }

        BarSlider {
            id: seek
            anchors { left: leftControls.right; right: rightControls.left; verticalCenter: parent.verticalCenter
                      leftMargin: 14; rightMargin: 14 }
            from: 0
            to: Math.max(1, player.durationMs)
            enabled: player.durationMs > 0
            // Follows playback except while being dragged, so the handle does
            // not fight the hand moving it.
            Binding on value {
                when: !seek.pressed
                value: player.positionMs
                restoreMode: Binding.RestoreNone
            }
            // Seeks once, on release: each seek reloads the stream, so one
            // per pixel of a drag would never let it play.
            onPressedChanged: if (!seek.pressed) player.seekTo(seek.value)
        }

        Row {
            id: rightControls
            anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
            spacing: 6

            // Volume / Mute
            IconButton {
                glyph: audio.muted || audio.volume === 0 ? "" : ""
                onClicked: audio.muted = !audio.muted
            }

            BarSlider {
                anchors.verticalCenter: parent.verticalCenter
                width: 90
                from: 0; to: 1
                value: audio.muted ? 0 : audio.volume
                onMoved: {
                    audio.volume = value
                    audio.muted = value === 0
                }
            }
        }
    }
}
