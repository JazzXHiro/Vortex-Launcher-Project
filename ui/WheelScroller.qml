import QtQuick

// Mouse-wheel scrolling for the launcher's vertical views, at a pace that
// suits 440px card rows.
//
// Flickable's own wheel handling moves roughly a text line's worth per notch,
// which is several notches per row of cards. This takes a fixed step per notch
// and glides there; notches that land mid-glide add on to where it was going
// rather than to where it has got to, so a fast spin covers the distance it
// should instead of being eaten by the animation.
//
// Declare it inside the view it scrolls and point `view` at that view:
//
//     GridView { id: grid; WheelScroller { view: grid } }
//
// Inside a Flickable it lands on the contentItem, which spans the content, so
// it sees the wheel anywhere over the cards. Views inside it that do not
// scroll (interactive: false) pass the wheel on to it.
WheelHandler {
    id: scroller

    required property Flickable view

    // Pixels per wheel notch (an angleDelta of 120).
    property real step: 280

    orientation: Qt.Vertical

    property NumberAnimation glide: NumberAnimation {
        target: scroller.view
        property: "contentY"
        duration: 220
        easing.type: Easing.OutCubic
    }

    function clamp(y) {
        const v = scroller.view
        const top = v.originY - v.topMargin
        const bottom = v.originY + v.contentHeight + v.bottomMargin - v.height
        return Math.max(top, Math.min(Math.max(top, bottom), y))
    }

    onWheel: (event) => scroller.take(event)

    // Also for a wheel that arrives somewhere the handler cannot see -- a
    // MouseArea over the view that has to keep the clicks, say -- and is
    // passed on by hand. Takes either a WheelHandler event or a MouseArea's
    // WheelEvent: both carry pixelDelta and angleDelta.
    function take(event) {
        const v = scroller.view

        // A touchpad reports pixels and brings its own momentum; follow it
        // one to one rather than gliding on top of it.
        if (event.pixelDelta.y !== 0) {
            scroller.glide.stop()
            v.contentY = scroller.clamp(v.contentY - event.pixelDelta.y)
            return
        }

        if (event.angleDelta.y === 0)
            return

        const from = scroller.glide.running ? scroller.glide.to : v.contentY
        scroller.glide.stop()
        scroller.glide.to = scroller.clamp(from - event.angleDelta.y / 120 * scroller.step)
        scroller.glide.start()
    }

    // For anything else that moves the view, so it does not fight a glide
    // still under way.
    function stop() {
        scroller.glide.stop()
    }
}
