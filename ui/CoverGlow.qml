import QtQuick
import QtQuick.Effects

// The cover's own colours bled out behind a lit card, like a backdrop the art
// is throwing light onto.
//
// Drawn on a stage behind the whole page rather than inside the card's grid,
// and moved onto the card every frame it shows. Inside the delegate it was
// cut off by the grid's clip -- a hard line across the top row's glow -- and
// painted over the neighbouring cards instead of behind them.
//
// The light is the cover averaged down to a handful of pixels, then stretched
// past the card on every side and blurred. Blurring the full-size art kept its
// shapes -- a white patch here, an orange one there -- and read as smudges
// under the card rather than a glow. At 6x9 it is a few soft fields of its
// main colours, which is what bleeds out.
//
// The 6x9 is taken on the GPU from the card's own texture. It used to be a
// second decode of the cover URL at that size, but most covers outside the
// library are remote, Qt's image cache keys on the requested size, and QML
// fetches with no HTTP cache -- so that decode was a second download, up to
// most of a second of SteamGridDB PNG before the glow could show.
Item {
    id: glow

    // The card's cover. The glow samples it as drawn, crop and all.
    property Image source
    // The card it sits behind, and the item it is drawn on. The stage has to
    // be outside every clipping view and behind the page's content.
    property Item target
    property Item stage
    property bool lit: false

    parent: glow.stage

    // How far the stretched cover reaches past the card on each side, before
    // the blur spreads it further.
    property real spread: 16

    // Not built until the card is first lit, so a grid full of cards costs
    // nothing until one is hovered. Kept afterwards: hovering back is free.
    // A grid that recycles its cards clears it when one is pooled, so the
    // glow is not armed for a cover the card no longer shows.
    property bool armed: false
    onLitChanged: if (glow.lit) glow.armed = true

    visible: opacity > 0
    // The target's visible is its effective visibility: a lit card on a page
    // the StackLayout just switched away from must not leave its light behind.
    opacity: (glow.lit && glow.target && glow.target.visible
              && body.item && body.item.ready) ? 0.8 : 0.0
    Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

    // Over the target's centre, which its hover scale leaves where it is, and
    // at its scale. Polled rather than bound: mapToItem is a one-off answer
    // and nothing notifies when a grid scrolls the card along.
    function place() {
        if (!glow.target || !glow.stage)
            return
        const c = glow.target.mapToItem(glow.stage, glow.target.width / 2,
                                        glow.target.height / 2)
        glow.width = glow.target.width
        glow.height = glow.target.height
        glow.x = c.x - glow.width / 2
        glow.y = c.y - glow.height / 2
        glow.scale = glow.target.scale
    }
    onVisibleChanged: if (glow.visible) glow.place()

    // Everything else waits for armed, so an unlit card carries this Item and
    // an empty Loader and nothing more. Every card in a grid has a glow, and a
    // frame clock and a second Loader each were paid on every card built while
    // scrolling, for the one under the mouse.
    Loader {
        id: body
        anchors.fill: parent
        active: glow.armed

        sourceComponent: Item {
            // The card's own art, so the glow is ready the moment the cover is.
            readonly property bool ready:
                glow.source !== null && glow.source.status === Image.Ready

            FrameAnimation {
                running: glow.visible
                onTriggered: glow.place()
            }

            // Behind a Loader, so only the lit card (and the one fading out
            // behind it) pays for the framebuffers. The focus strip on the
            // library card turns down a MultiEffect for exactly that cost on
            // every card.
            Loader {
                anchors.fill: parent
                anchors.margins: -glow.spread
                // Dropped a little: the light pools under the card more than
                // above it.
                anchors.topMargin: -glow.spread + 10
                anchors.bottomMargin: -glow.spread - 10
                active: glow.visible

                sourceComponent: Item {
                    // The cover, mipmapped at 96x144. Rendering a 600x900
                    // texture into it samples a scatter of single points, but
                    // the mips below average them back out: 96x144 down to
                    // 6x9 is level 4, each swatch pixel a 16x16 block.
                    ShaderEffectSource {
                        id: fine
                        width: glow.width
                        height: glow.height
                        visible: false
                        sourceItem: glow.source
                        textureSize: Qt.size(96, 144)
                        mipmap: true
                        smooth: true
                    }

                    // The swatch stretched to full size in a layer of its own.
                    // Handed the 6x9 texture directly, MultiEffect blurs at that
                    // size and scales the result up, which came out as a
                    // hard-edged block with streaks; blurring a full-size
                    // texture is what spreads it.
                    Item {
                        id: wash
                        anchors.fill: parent
                        visible: false
                        layer.enabled: true

                        ShaderEffectSource {
                            anchors.fill: parent
                            sourceItem: fine
                            textureSize: Qt.size(6, 9)
                            smooth: true
                        }
                    }

                    MultiEffect {
                        anchors.fill: parent
                        source: wash
                        blurEnabled: true
                        blur: 1.0
                        blurMax: 64
                        // Past blurMax for a wider, softer reach. The coarse
                        // samples this costs are invisible on a wash with no
                        // detail in it.
                        blurMultiplier: 1.6
                        // A touch richer than the art, so a muted cover still
                        // throws a colour rather than grey.
                        saturation: 0.5
                    }
                }
            }
        }
    }
}
