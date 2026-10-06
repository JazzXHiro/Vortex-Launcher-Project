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

    property var api: vortexApi

    // What the images actually load: nothing while the page is closed. Both
    // details pages are Popups that live for the whole session, so without
    // this each one held its last game's decoded banner and logo -- tens of MB
    // apiece -- long after it was dismissed. visible here is the effective
    // visibility, false whenever the Popup is.
    readonly property string shownHero: hero.visible ? hero.heroSource : ""
    readonly property string shownLogo: hero.visible ? hero.logoSource : ""

    // The largest this banner can be drawn: as wide as the screen, and as tall
    // as the details pages ever make it (max(460, 60% of their height)).
    // Taken from the screen rather than the banner's live size so a window
    // resize does not re-decode the art at every step and flash the veil.
    readonly property real maxArtWidth: Screen.width
    readonly property real maxArtHeight: Math.max(460, Screen.height * 0.6)
    readonly property real dpr: Screen.devicePixelRatio

    // A move to another monitor changes the box, and so what the art on show
    // should be decoded at. Often all three change at once; callLater folds
    // them into one pass.
    onMaxArtWidthChanged: Qt.callLater(hero.redecodeAll)
    onMaxArtHeightChanged: Qt.callLater(hero.redecodeAll)
    onDprChanged: Qt.callLater(hero.redecodeAll)

    function artSize(img, src) {
        return hero.api
            ? hero.api.decodeSize(src, img.boxWidth, img.boxHeight, img.crop, hero.dpr)
            : Qt.size(-1, -1)
    }

    // Points img at src, decoded at the size the bridge works out for its box.
    // Imperative rather than two bindings off the same source: whichever of
    // source and sourceSize Qt re-evaluated first would start a decode the
    // other then cancelled -- at full size, when source went first. The size
    // goes in with the old source already cleared, so it reloads nothing.
    // `loaded` is the plain path last asked for; source itself is a resolved
    // URL that no longer compares equal to one.
    function loadArt(img, src) {
        if (img.loaded === src) return
        img.loaded = src
        img.source = ""
        img.decodedAt = hero.artSize(img, src)
        img.sourceSize = img.decodedAt
        img.source = src
    }

    // The same art at the size the current screen wants, decoded in place:
    // retainWhileLoading keeps the old decode on screen until the new one is
    // ready, so nothing flashes. Qt reloads by itself on a DPR change, at the
    // old logical size times the new ratio; this follows up with the size
    // that is right for the new screen, which that is not -- it can overshoot
    // the original or fall short of the box.
    function redecode(img) {
        if (img.loaded === "") return
        const size = hero.artSize(img, img.loaded)
        if (size.width === img.decodedAt.width && size.height === img.decodedAt.height)
            return
        img.decodedAt = size
        img.sourceSize = size
    }

    function redecodeAll() {
        for (const img of [artA, artB, logoArt, centreArt])
            hero.redecode(img)
    }

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
        hero.front.shown
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

    onShownHeroChanged: hero.showArt(hero.shownHero)
    onShownLogoChanged: hero.loadArt(logoArt, hero.shownLogo)
    Component.onCompleted: {
        hero.showArt(hero.shownHero)
        hero.loadArt(logoArt, hero.shownLogo)
    }

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
        if (!hero.visible || src === "" || !hero.front.shown) {
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

    // Any of the banner's images, pointed at its art through loadArt(). Each
    // says what box it is drawn in, at its largest -- covering it when crop,
    // fitting inside it otherwise -- which is what the art is decoded for.
    //
    // shown is whether art is on screen, which status alone no longer says: a
    // re-decode keeps the old art up but reports Loading while it runs, and
    // reading that as "nothing here" would bring the veil back and flip the
    // blur on for the length of the decode. It turns on at Ready and off only
    // when the source is cleared or fails.
    component ArtSlot: Image {
        id: slot
        property string loaded: ""
        property size decodedAt: Qt.size(-1, -1)
        property bool shown: false
        property real boxWidth: hero.maxArtWidth
        property real boxHeight: hero.maxArtHeight
        property bool crop: false
        asynchronous: true
        retainWhileLoading: true
        onStatusChanged: {
            if (slot.status === Image.Ready)
                slot.shown = true
            else if (slot.status !== Image.Loading)
                slot.shown = false
        }
    }

    // One of the backdrop's two images. want is the source as asked for, kept
    // apart from source, which Qt resolves into a URL that no longer compares
    // equal to a plain path.
    component ArtImage: ArtSlot {
        id: art
        property string want: ""
        anchors.fill: parent
        fillMode: Image.PreserveAspectCrop
        // Aspect is kept, so heroIsWide still reads the art's real shape off
        // the smaller decode.
        crop: true
        onWantChanged: hero.loadArt(art, art.want)
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
        opacity: hero.front.shown ? 0.0 : 1.0
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
    readonly property bool hasLogo: hero.shownLogo !== "" && logoArt.status !== Image.Error

    // Centre art, in order of preference: the logo when there is one, else the
    // sharp portrait over its own blur, else nothing — real wide art reads
    // perfectly well on its own. Both sit centred in the clear part above the
    // band.
    ArtSlot {
        id: logoArt
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -band.height / 2
        width: parent.width * 0.4
        height: Math.min(parent.height * 0.4, hero.bandTop * 0.7)
        // The box above, at its largest. SteamGridDB logos are drawn a few
        // hundred pixels wide and stored up to 8334x2051 -- 65 MB decoded.
        boxWidth: hero.maxArtWidth * 0.4
        boxHeight: hero.maxArtHeight * 0.4
        fillMode: Image.PreserveAspectFit
        visible: hero.shownLogo !== ""
        opacity: logoArt.shown ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 450; easing.type: Easing.OutCubic } }
    }

    ArtSlot {
        id: centreArt
        // Decided off the art on show rather than heroSource, so a banner still
        // loading is never drawn here before it is known to be wide; and the
        // last portrait is kept while it fades out under a banner coming in.
        readonly property bool wanted: !hero.hasLogo
            && (hero.front.shown ? !hero.heroIsWide : hero.heroSource === "")

        anchors.centerIn: parent
        anchors.verticalCenterOffset: -band.height / 2
        height: Math.max(0, hero.bandTop - 48)
        fillMode: Image.PreserveAspectFit

        // What the slot should load, or null to keep what it has, so the last
        // portrait stays while it fades out under a banner coming in. The
        // backdrop's art first: when it is portrait it is IGDB's 540x720,
        // which downscales into this slot, where the 264x352 cover would have
        // to be stretched up to fill it. Dropped while the page is closed,
        // like the art above.
        readonly property var target: !hero.visible ? ""
            : centreArt.wanted ? (hero.front.want !== "" ? hero.front.want : hero.coverSource)
            : null
        onTargetChanged: {
            if (centreArt.target !== null)
                hero.loadArt(centreArt, centreArt.target)
        }
        opacity: centreArt.wanted && centreArt.shown ? 1.0 : 0.0
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
