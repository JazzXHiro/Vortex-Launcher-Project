pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Effects
import Vortex

// Steam's library header, shared by every details page: the art across the
// top, its lower part frosting into a band that carries whatever the page puts
// in it -- the title and the actions. Children declared inside go into that
// band, a Column, from the top down.
Rectangle {
    id: hero

    // The banner art, the logo drawn over it, and the cover that stands in
    // for both when neither exists.
    property string heroSource: ""
    property string logoSource: ""
    property string coverSource: ""

    default property alias bandData: band.data

    color: Theme.bgRaised
    radius: 20
    // Layer + clip keeps the images inside the window's curved edges.
    layer.enabled: true
    clip: true

    // Whether the art is genuinely wide is decided from the loaded image, not
    // from where it came from: Steam's library_hero.jpg is a 1920x620 banner,
    // but IGDB's t_720p is 540x720 and a cover standing in is 264x352, and
    // cropping a portrait into a banner is an ugly upscale.
    //
    // 16:10 is the loosest thing anyone ships as a banner and the tightest
    // portrait cover is 3:4, so 1.6 separates the two with room to spare. This
    // stays false until the image reports a size, so the banner starts blurred
    // — the safe way round, since un-blurring late is invisible and stretching
    // early is the bug.
    readonly property bool heroIsWide:
        hero.front.status === Image.Ready
        && hero.front.implicitWidth > hero.front.implicitHeight * 1.6

    // Real wide art is already the right shape and stays sharp at the opacity
    // these pages have always used. Portrait art is blurred instead and
    // carries a little more of the frame, since once blurred it is only there
    // to be colour.
    readonly property real backdropOpacity: hero.heroIsWide ? 0.4 : 0.55

    // ── Crossfading the art ─────────────────────────────────────────────────
    // A Browse result opens on its cover and swaps to the banner once IGDB
    // answers. Two images take turns so the new art fades in over the old
    // rather than the old vanishing first: front is the one on show, incoming
    // the one still loading, outgoing the one fading out underneath.
    readonly property int crossfadeDuration: 500
    property ArtImage front: artA
    property ArtImage incoming: null
    property ArtImage outgoing: null

    onHeroSourceChanged: hero.showArt(hero.heroSource)
    Component.onCompleted: hero.showArt(hero.heroSource)

    function finishCrossfade() {
        crossfade.stop()
        hero.front.opacity = 1
        if (hero.outgoing) {
            hero.outgoing.want = ""
            hero.outgoing = null
        }
    }

    function showArt(src) {
        hero.finishCrossfade()
        const back = hero.front === artA ? artB : artA
        if (src === hero.front.want) {
            back.want = ""
            hero.incoming = null
            return
        }
        // Nothing on screen to fade from -- the page is closed (a new game is
        // being opened behind it), the art is going away, or the old art never
        // arrived. Swap straight away; the veil fades the new art in.
        if (!hero.visible || src === "" || hero.front.status !== Image.Ready) {
            back.want = ""
            hero.incoming = null
            hero.front.want = src
            return
        }
        back.opacity = 0
        back.want = src
        hero.incoming = back
        hero.artStatusChanged(back)
    }

    function artStatusChanged(img) {
        if (img !== hero.incoming) return
        if (img.status === Image.Ready) {
            hero.incoming = null
            hero.outgoing = hero.front
            hero.front = img
            img.z = 1
            hero.outgoing.z = 0
            crossfade.target = img
            crossfade.start()
        } else if (img.status === Image.Error) {
            hero.incoming = null
            img.want = ""
        }
    }

    NumberAnimation {
        id: crossfade
        property: "opacity"
        from: 0.0; to: 1.0
        duration: hero.crossfadeDuration
        easing.type: Easing.InOutQuad
        onFinished: hero.finishCrossfade()
    }

    // Where the band starts. The blur begins just above the title (blurLead)
    // and builds up over blurFade, so it is fully frosted a little way into
    // the band and has no edge of its own.
    readonly property real bandTop: hero.height - band.height
    readonly property int blurLead: 40
    readonly property int blurFade: 100
    readonly property real fadeTop: hero.bandTop - hero.blurLead
    readonly property real fadeEnd: hero.fadeTop + hero.blurFade

    // How many strengths of blur the fade is built from. Each is a little
    // stronger than the last and starts a little lower, so the art goes soft
    // gradually instead of crossfading from sharp straight to fully frosted,
    // which reads as a seam.
    readonly property int blurSteps: 5
    readonly property real blurStep: hero.blurFade / (hero.blurSteps + 1)

    // A y inside the banner as a gradient position.
    function frac(y) {
        return Math.max(0, Math.min(1, y / Math.max(1, hero.height)))
    }

    // Drawn through the MultiEffects below, never directly — hiding the source
    // item is how MultiEffect is given something to work on. The layer makes
    // the pair one texture that every effect shares, the crossfade baked in.
    Item {
        id: heroBackdrop
        anchors.fill: parent
        visible: false
        layer.enabled: true

        ArtImage { id: artA }
        ArtImage { id: artB }
    }

    // One of the backdrop's two images. want is the source as asked for, kept
    // apart from source, which Qt resolves into a URL that no longer compares
    // equal to a plain path.
    component ArtImage: Image {
        id: art
        property string want: ""
        anchors.fill: parent
        asynchronous: true
        fillMode: Image.PreserveAspectCrop
        source: art.want
        onStatusChanged: hero.artStatusChanged(art)
    }

    // Drawn opaque, then dimmed once by the scrim below. Each frost layer is a
    // blurred copy of the same opaque art, so where its mask is solid it
    // replaces the sharp art under it rather than stacking on it -- stacked
    // translucent copies would brighten.
    //
    // The frost used to be built the other way round: a blurred copy dimmed
    // inside a hidden Item, which a second MultiEffect then masked. At a
    // fractional display scale (a 125% laptop) Qt left that nested blur at its
    // old size whenever the window shrank, so it covered only the left part of
    // the band and the bare fill showed through as a black block at its right.
    // One effect per layer, sampling the art directly, has nothing nested to
    // go stale.
    MultiEffect {
        anchors.fill: parent
        source: heroBackdrop
        // Eased rather than switched, so a cover crossfading to a wide banner
        // comes into focus as it fades instead of snapping sharp.
        blurEnabled: blur > 0
        blur: hero.heroIsWide ? 0.0 : 1.0
        Behavior on blur { NumberAnimation { duration: hero.crossfadeDuration; easing.type: Easing.InOutQuad } }
        blurMax: 48
        // Unpadded, so the blur clamps at the banner's edges instead of
        // fading them out to the transparent padding.
        autoPaddingEnabled: false
    }

    // The progressive frost: one layer per blur strength, weakest first. Each
    // fades in over two steps starting one step below the last, so
    // neighbouring strengths overlap and blend, and all of them are solid by
    // fadeEnd.
    Repeater {
        model: hero.blurSteps
        delegate: Item {
            id: frostLayer
            required property int index
            readonly property real rampStart: hero.fadeTop + frostLayer.index * hero.blurStep
            readonly property real rampEnd: Math.min(hero.fadeEnd, frostLayer.rampStart + 2 * hero.blurStep)

            anchors.fill: parent

            // Clear above this layer's ramp, solid below it.
            Rectangle {
                id: frostMask
                anchors.fill: parent
                visible: false
                layer.enabled: true
                gradient: Gradient {
                    GradientStop { position: hero.frac(frostLayer.rampStart); color: "transparent" }
                    GradientStop { position: hero.frac(frostLayer.rampEnd); color: "white" }
                }
            }

            MultiEffect {
                anchors.fill: parent
                source: heroBackdrop
                blurEnabled: true
                // Squared, so the early steps stay subtle and the softening
                // gathers pace toward the band.
                blur: Math.pow((frostLayer.index + 1) / hero.blurSteps, 2)
                blurMax: 64
                autoPaddingEnabled: false
                maskEnabled: true
                maskSource: frostMask
                // The lower edge runs from min*(1+spread)-spread to
                // min*(1+spread), so 0.5 with a spread of 1 maps the mask's
                // alpha 0..1 straight through. A threshold of 0 would put that
                // edge below zero and pass the whole mask, frosting the entire
                // hero.
                maskThresholdMin: 0.5
                maskSpreadAtMin: 1.0
            }
        }
    }

    // Fades the art in as it arrives instead of letting it pop in: the
    // banner's own fill laid over the art, lifted once the image is ready.
    // A veil rather than the art's own opacity, since the frost layers drawn
    // translucent would stack and brighten while they faded. It also hides
    // the blur switching off when wide art reports its size.
    Rectangle {
        anchors.fill: parent
        color: hero.color
        opacity: hero.front.status === Image.Ready ? 0.0 : 1.0
        Behavior on opacity { NumberAnimation { duration: 450; easing.type: Easing.OutCubic } }
    }

    // Dims the art to backdropOpacity: the banner's own fill laid over it at
    // the remaining strength comes out the same as the art drawn translucent
    // over that fill.
    Rectangle {
        anchors.fill: parent
        color: hero.color
        opacity: 1 - hero.backdropOpacity
        Behavior on opacity { NumberAnimation { duration: hero.crossfadeDuration } }
    }

    // Darkens the band for the text on it and melts its foot into the panel,
    // so the page below continues with no seam. Eased in over the same
    // distance as the blur, gently at first.
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: hero.frac(hero.fadeTop); color: "transparent" }
            GradientStop {
                position: hero.frac(hero.fadeTop + hero.blurFade * 0.5)
                color: Qt.alpha(Theme.bgPanel, 0.15)
            }
            GradientStop { position: hero.frac(hero.fadeEnd); color: Qt.alpha(Theme.bgPanel, 0.55) }
            GradientStop { position: 1.0; color: Theme.bgPanel }
        }
    }

    // Whether the logo holds the centre. A logo that fails to load gives it up
    // to the portrait below, the same as having no logo at all -- Qt rejects
    // oversized images outright, and a link can simply be dead. Still loading
    // counts as having one, so the portrait does not flash in ahead of it.
    readonly property bool hasLogo: hero.logoSource !== "" && logoArt.status !== Image.Error

    // Centre art, in order of preference: the logo when there is one, else the
    // sharp portrait over its own blur, else nothing — real wide art reads
    // perfectly well on its own. Both sit centred in the clear part above the
    // band.
    Image {
        id: logoArt
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -band.height / 2
        width: parent.width * 0.4
        height: Math.min(parent.height * 0.4, hero.bandTop * 0.7)
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        visible: hero.logoSource !== ""
        source: hero.logoSource
        opacity: status === Image.Ready ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 450; easing.type: Easing.OutCubic } }
    }

    Image {
        id: centreArt
        // Decided off the art on show rather than heroSource, so a banner still
        // loading is never drawn here before it is known to be wide; and the
        // last portrait is kept while it fades out under a banner coming in.
        readonly property bool wanted: !hero.hasLogo
            && (hero.front.status === Image.Ready ? !hero.heroIsWide : hero.heroSource === "")

        anchors.centerIn: parent
        anchors.verticalCenterOffset: -band.height / 2
        height: Math.max(0, hero.bandTop - 48)
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        // The backdrop's art first: when it is portrait it is IGDB's 540x720,
        // which downscales into this slot, where the 264x352 cover would have
        // to be stretched up to fill it.
        Binding on source {
            when: centreArt.wanted
            value: hero.front.want !== "" ? hero.front.want : hero.coverSource
            restoreMode: Binding.RestoreNone
        }
        opacity: centreArt.wanted && status === Image.Ready ? 1.0 : 0.0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: hero.crossfadeDuration; easing.type: Easing.OutCubic } }
    }

    // ── The band ────────────────────────────────────────────────────────────
    Column {
        id: band
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom
                  leftMargin: 40; rightMargin: 40 }
        bottomPadding: 28
        spacing: 14
    }
}
