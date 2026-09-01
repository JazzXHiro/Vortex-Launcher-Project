pragma Singleton
import QtQuick

// The launcher's palette, in one place.
//
// Every colour the UI paints comes from here. Nothing else in ui/ should carry
// a hex literal — if a new one is needed, it gets a name in this file first.
//
// The greys are three deliberate ladders — surfaces, outlines, foregrounds —
// with a step of roughly 0x18 between rungs. That step is about the smallest
// difference that still reads as intentional on a dark screen; anything finer
// looks like a mistake rather than a distinction. Adding a rung between two
// existing ones is nearly always the wrong fix, and the reason to resist it is
// concrete: this replaced a 15-rung foreground ladder in which #999999, #888888
// and #777777 were all doing the same job in three different views.
//
// Naming: `bg*` fills, `border*` outlines, `text*` foregrounds brightest to
// faintest, then the semantic accents. Where a value serves two roles it gets a
// name for each, so the two can be pulled apart later without hunting through
// call sites — bgEmphasis and borderMuted are both #2a2a2a today, and nothing
// says they have to stay that way.
QtObject {

    // ─────────────────────────────────────────────────────────────────────────
    // Surfaces, back to front
    // ─────────────────────────────────────────────────────────────────────────

    // The window itself, and the vertical gradient the mood overlay fades to.
    readonly property color bgWindow:        "black"

    // Recessed: the settings sidebar, list wells, text inputs, the wizard footer.
    readonly property color bgSunken:        "#0d0d0d"

    // A panel sitting on the window: a modal's body, a mood card, the mood picker.
    readonly property color bgPanel:         "#121212"

    // Cards, menus and rows: the tile menu, recommendation and wishlist tiles,
    // a folder row, the scrollbar track, a switch at off.
    readonly property color bgSurface:       "#141414"

    // The workhorse. Pill buttons at rest, library capsules, the hero banner,
    // and a quiet row under the cursor.
    readonly property color bgRaised:        "#1a1a1a"

    // Engaged, but not emphasised: the selected sidebar section, a hovered menu
    // item, a neutral button in the details dialog.
    readonly property color bgActive:        "#1f1f1f"

    // Emphasised by hover or focus: a tab pill, a cancel, the scan progress track.
    readonly property color bgEmphasis:      "#2a2a2a"

    // Inert, and meant to look it: an unreached step dot, a cancel that is not
    // the action being offered.
    readonly property color bgInert:         "#333333"

    // The knob on a switch that is off.
    readonly property color bgSwitchHandle:  "#555555"

    // The three dots on a capsule's overflow button — a foreground mark that
    // happens to be drawn as three Rectangles rather than as text.
    readonly property color bgDot:           "#dddddd"

    // A primary (white) button while held down.
    readonly property color bgPressed:       "#e0e0e0"

    // ─────────────────────────────────────────────────────────────────────────
    // Outlines
    // ─────────────────────────────────────────────────────────────────────────

    readonly property color borderQuiet:     "#252525"  // modal edge, inset panel
    readonly property color borderMuted:     "#2a2a2a"  // a control at rest
    readonly property color borderControl:   "#333333"  // the default outline
    readonly property color borderStrong:    "#4a4a4a"  // hovered or focused

    // The 1-px rules that split a dialog into sections. Painted as a thin
    // Rectangle rather than an outline, so it is a fill by the time it is used.
    readonly property color divider:         "#252525"

    // ─────────────────────────────────────────────────────────────────────────
    // Foregrounds, brightest to faintest
    // ─────────────────────────────────────────────────────────────────────────

    readonly property color textPrimary:     "white"    // headings, selected labels
    readonly property color textInverse:     "black"    // on a white or accent fill
    readonly property color textBody:        "#dddddd"  // body copy, card titles
    readonly property color textSecondary:   "#aaaaaa"  // labels, metadata, prose
    readonly property color textMuted:       "#888888"  // control labels, descriptions
    readonly property color textFaint:       "#666666"  // section headers, hints, counters
    readonly property color textGhost:       "#444444"  // empty states, placeholders

    // ─────────────────────────────────────────────────────────────────────────
    // Accent
    //
    // White is this launcher's accent: the active tab, a hovered button, the
    // focus ring. Two names, one value — because when the accent stops being
    // white, a focus ring and a filled button will not want the same treatment.
    // ─────────────────────────────────────────────────────────────────────────

    readonly property color accent:          "#ffffff"  // filled primary / hover
    readonly property color focusRing:       "#ffffff"  // the highlighted outline

    // ─────────────────────────────────────────────────────────────────────────
    // Semantic
    // ─────────────────────────────────────────────────────────────────────────

    readonly property color positive:        "#27ae60"  // installed, switch on, success
    readonly property color positiveBg:      "#1e4d2f"  // switch track at on
    readonly property color positiveDim:     "#1d4a2b"  // wishlisted button fill
    readonly property color positiveText:    "#7fe0a0"  // the INSTALLED badge label

    readonly property color danger:          "#e74c3c"  // destructive, hovered
    readonly property color dangerRest:      "#c0392b"  // destructive at rest
    readonly property color dangerBg:        "#4a1d1a"  // uninstall while focused
    readonly property color dangerBgSubtle:  "#231717"  // a row armed for removal
    readonly property color dangerBorder:    "#5c2b2b"  // that row's edge
    readonly property color dangerText:      "#ff8a7a"  // "delete this game's files?"
    readonly property color dangerIcon:      "#ff6b5b"  // the trash glyph, focused

    readonly property color warning:         "#e67e22"  // step unavailable
    readonly property color caution:         "#e0a33a"  // a blocker worth acting on
    readonly property color favorite:        "#e0245e"  // the heart, when set

    readonly property color link:            "#3498db"
    readonly property color linkHover:       "#5dade2"

    // Steam's own navy, for the button that hands off to Steam.
    readonly property color steamBg:         "#1b2838"
    readonly property color steamBgPressed:  "#132b45"
    readonly property color steamAccent:     "#66c0f4"

    // ─────────────────────────────────────────────────────────────────────────
    // Moods
    //
    // main.qml paints the mood cards and SettingsWindow.qml lists the same four
    // in a picker. They used to carry separate copies of these four hex values
    // with a comment warning they must not drift; now they cannot.
    // ─────────────────────────────────────────────────────────────────────────

    readonly property color moodNeutral:     "#7f8c8d"
    readonly property color moodRelaxed:     "#27ae60"
    readonly property color moodCompetitive: "#e74c3c"
    readonly property color moodImmersive:   "#9b59b6"

    // ─────────────────────────────────────────────────────────────────────────
    // Translucent overlays
    //
    // These sit over artwork, so they are alpha rather than a flat grey — the
    // cover has to stay readable underneath.
    // ─────────────────────────────────────────────────────────────────────────

    readonly property color scrim:           "#cc000000"  // behind a modal
    readonly property color overlayBadge:    "#D9000000"  // playtime badge
    readonly property color overlayStrong:   "#E6000000"  // the ML PICK badge
    readonly property color overlayButton:   "#B3000000"  // overflow button at rest
    readonly property color overlayPositive: "#D91e4d2f"  // the INSTALLED badge

    // ─────────────────────────────────────────────────────────────────────────
    // Scrollbar
    //
    // Its handle is a fill that happens to share values with the text ladder;
    // named separately so restyling text does not move the scrollbar with it.
    // ─────────────────────────────────────────────────────────────────────────

    readonly property color scrollHandle:      "#3a3a3a"
    readonly property color scrollHandleHover: "#888888"
    readonly property color scrollHandleDown:  "white"
}
