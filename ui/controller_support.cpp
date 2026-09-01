// ─────────────────────────────────────────────────────────────────────────────
// controller_support.cpp — gamepad navigation for the launcher UI.
//
// Everything gamepad-related lives in this one file: the XInput polling, the
// hold-to-repeat logic and the QObject that publishes the result to QML as the
// "controller" context property. main_ui.cpp only calls
// installControllerSupport(); nothing else in the C++ engine knows a pad exists.
//
// The class emits *intent* signals (navigate / accept / cancel / …) rather than
// button names, so main.qml decides what "back" means for whatever is on screen
// instead of this file needing to know about popups and tabs.
//
// XInput is resolved at runtime with LoadLibrary. That keeps the pad optional:
// on a machine without the DLL — or on a non-Windows build — the timer never
// starts and the UI behaves exactly as it did before.
// ─────────────────────────────────────────────────────────────────────────────

#include <QCursor>
#include <QDateTime>
#include <QEvent>
#include <QGuiApplication>
#include <QObject>
#include <QPoint>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QString>
#include <QTimer>

#include <cstdlib>

#ifdef Q_OS_WIN
#include <windows.h>
#include <xinput.h>
#endif

namespace {

// Poll rate. 60 Hz matches the compositor, so a stick flick is never noticeably
// late without costing anything measurable.
constexpr int kPollIntervalMs = 16;

// XInputGetState on an empty slot is expensive (it re-enumerates USB devices),
// so slots we know are empty are only retried this often.
constexpr int kRescanIntervalMs = 2000;

// Hold-to-repeat, tuned to feel like a keyboard's auto-repeat: a deliberate
// pause before the first repeat, then fast enough to cross a full grid.
constexpr int kRepeatDelayMs    = 400;
constexpr int kRepeatIntervalMs = 110;

// Stick thresholds against the ±32767 range. The release value is lower than
// the press value on purpose: without that hysteresis a stick resting just
// around the deadzone edge fires a stream of navigation events.
constexpr int kStickPress   = 12000;
constexpr int kStickRelease = 9000;

// Same idea for the analog triggers (0–255). XInput's own recommended
// threshold is 30, which is low enough that a resting finger toggles filters.
constexpr int kTriggerPress   = 90;
constexpr int kTriggerRelease = 50;

// How far the mouse has to travel before it counts as the user reaching for
// it. Small enough to feel instant, large enough to shrug off jitter and the
// stray move Qt reports when a window moves under a stationary pointer.
constexpr int kMouseWakeSlop = 4;

// Direction names shared with main.qml's controllerNavigate().
constexpr char kLeft[]  = "left";
constexpr char kRight[] = "right";
constexpr char kUp[]    = "up";
constexpr char kDown[]  = "down";

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ControllerSupport
// ─────────────────────────────────────────────────────────────────────────────
class ControllerSupport : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    // True from the moment the pad acts until the mouse is moved again. The UI
    // hangs its highlights off this so hover and pad focus never both show.
    Q_PROPERTY(bool padInControl READ isPadInControl NOTIFY padInControlChanged)

public:
    explicit ControllerSupport(QObject *parent = nullptr);
    ~ControllerSupport() override;

    bool isConnected() const { return m_connected; }
    bool isPadInControl() const { return m_padInControl; }

protected:
    // Watches the whole application for real mouse movement, which is what
    // brings the pointer back after the pad has hidden it.
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    // "left" / "right" / "up" / "down" — left thumbstick or D-pad, with repeat.
    void navigate(const QString &direction);
    void accept();                 // A
    void cancel();                 // B
    void filterPrevious();         // LT
    void filterNext();             // RT
    void toggleRecommendations();  // Back / Select
    void options();                // R3 — the focused card's overflow menu
    void connectedChanged();
    void padInControlChanged();

private:
    void poll();
    void setConnected(bool value);
    void releaseAllInput();
    void hideCursor();
    void showCursor();

    QTimer  m_timer;
    bool    m_connected = false;

    // Direction currently held, and when it should fire again.
    QString m_heldDirection;
    qint64  m_nextRepeatMs = 0;

    // Who is driving, plus where the mouse sat when the pad took over, so a
    // nudge can be told apart from Qt re-reporting the same position.
    bool    m_padInControl = false;
    QPoint  m_cursorAnchor;

#ifdef Q_OS_WIN
    using XInputGetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);

    XInputGetStateFn m_getState = nullptr;
    DWORD            m_padIndex = 0;      // slot of the pad we currently follow
    qint64           m_nextRescanMs = 0;  // when to look for a pad again
    WORD             m_prevButtons = 0;
    bool             m_leftTriggerHeld  = false;
    bool             m_rightTriggerHeld = false;
#endif
};

ControllerSupport::ControllerSupport(QObject *parent) : QObject(parent) {
#ifdef Q_OS_WIN
    // Newest first. 9_1_0 ships with every Windows since 7, so the last entry
    // is effectively guaranteed to load; the earlier ones are only preferred
    // because they report newer pads more reliably.
    const wchar_t *candidates[] = {L"xinput1_4.dll", L"xinput1_3.dll",
                                   L"xinput9_1_0.dll"};

    for (const wchar_t *name : candidates) {
        if (HMODULE lib = LoadLibraryW(name)) {
            m_getState = reinterpret_cast<XInputGetStateFn>(
                GetProcAddress(lib, "XInputGetState"));
            if (m_getState) break;
            FreeLibrary(lib);
        }
    }

    if (!m_getState) return;  // no XInput on this box — stay dormant

    // Only worth watching the mouse once there is something that can hide it.
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installEventFilter(this);

    m_timer.setInterval(kPollIntervalMs);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &ControllerSupport::poll);
    m_timer.start();
#endif
}

// Never leave the application without a pointer on the way out.
ControllerSupport::~ControllerSupport() {
    showCursor();
}

void ControllerSupport::setConnected(bool value) {
    if (m_connected == value) return;
    m_connected = value;
    emit connectedChanged();
}

// ─────────────────────────────────────────────────────────────────────────────
// Handing control between pad and mouse
//
// The pointer hides with an application-wide override cursor rather than
// per-item cursorShape: it covers every window and outranks the pointing-hand
// cursors the buttons ask for, so nothing flickers back into view under the
// pad's control. The same flag tells QML which input owns the highlights.
// ─────────────────────────────────────────────────────────────────────────────
void ControllerSupport::hideCursor() {
    if (m_padInControl) return;
    m_cursorAnchor = QCursor::pos();
    QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
    m_padInControl = true;
    emit padInControlChanged();
}

void ControllerSupport::showCursor() {
    if (!m_padInControl) return;
    QGuiApplication::restoreOverrideCursor();
    m_padInControl = false;
    emit padInControlChanged();
}

bool ControllerSupport::eventFilter(QObject *watched, QEvent *event) {
    if (m_padInControl) {
        switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::HoverMove:
            // Measured against the real pointer position, not the event's:
            // hover events can carry stale coordinates, and the anchor is only
            // reset when the pad hides the cursor again.
            if ((QCursor::pos() - m_cursorAnchor).manhattanLength() > kMouseWakeSlop)
                showCursor();
            break;
        case QEvent::MouseButtonPress:
        case QEvent::Wheel:
            showCursor();
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

// Forget every held input. Called when the pad goes away or the launcher loses
// focus, so a direction held at that moment does not resume repeating later.
void ControllerSupport::releaseAllInput() {
    m_heldDirection.clear();
#ifdef Q_OS_WIN
    m_prevButtons      = 0;
    m_leftTriggerHeld  = false;
    m_rightTriggerHeld = false;
#endif
}

#ifdef Q_OS_WIN

// Dominant-axis direction for the left stick, or an empty string for centred.
// `active` selects the release threshold so an already-held direction needs a
// clearer return to centre before it counts as let go.
static QString stickDirection(SHORT x, SHORT y, bool active) {
    const int threshold = active ? kStickRelease : kStickPress;
    const int ax = std::abs(static_cast<int>(x));
    const int ay = std::abs(static_cast<int>(y));

    if (ax < threshold && ay < threshold) return QString();
    if (ax >= ay) return x > 0 ? QString(kRight) : QString(kLeft);
    return y > 0 ? QString(kUp) : QString(kDown);  // XInput Y is +up
}

static QString dpadDirection(WORD buttons) {
    if (buttons & XINPUT_GAMEPAD_DPAD_LEFT)  return QString(kLeft);
    if (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) return QString(kRight);
    if (buttons & XINPUT_GAMEPAD_DPAD_UP)    return QString(kUp);
    if (buttons & XINPUT_GAMEPAD_DPAD_DOWN)  return QString(kDown);
    return QString();
}

static bool triggerHeld(BYTE value, bool wasHeld) {
    return value >= (wasHeld ? kTriggerRelease : kTriggerPress);
}

void ControllerSupport::poll() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Read the pad we already follow; only sweep the other slots occasionally,
    // since querying an empty one is slow.
    XINPUT_STATE state{};
    bool haveState = m_getState(m_padIndex, &state) == ERROR_SUCCESS;

    if (!haveState && now >= m_nextRescanMs) {
        m_nextRescanMs = now + kRescanIntervalMs;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            if (i == m_padIndex) continue;
            if (m_getState(i, &state) == ERROR_SUCCESS) {
                m_padIndex = i;
                haveState  = true;
                break;
            }
        }
    }

    if (!haveState) {
        if (m_connected) {
            releaseAllInput();
            // Unplugging the pad must not strand the user without a pointer.
            showCursor();
        }
        setConnected(false);
        return;
    }
    setConnected(true);

    // Ignore the pad while another application owns the screen. Launching a
    // game hands focus to it, and its own controller input must not also be
    // walking the launcher's grid behind it.
    if (!QGuiApplication::focusWindow()) {
        releaseAllInput();
        return;
    }

    const XINPUT_GAMEPAD &pad = state.Gamepad;

    // Set by anything the user actually did this frame; the pointer gets out of
    // the way once, at the end, rather than on every individual emit.
    bool acted = false;

    // ── Direction: D-pad wins over the stick when both are pushed ────────────
    QString direction = dpadDirection(pad.wButtons);
    if (direction.isEmpty())
        direction = stickDirection(pad.sThumbLX, pad.sThumbLY,
                                   !m_heldDirection.isEmpty());

    if (direction.isEmpty()) {
        m_heldDirection.clear();
    } else if (direction != m_heldDirection) {
        m_heldDirection = direction;
        m_nextRepeatMs  = now + kRepeatDelayMs;
        acted = true;
        emit navigate(direction);
    } else if (now >= m_nextRepeatMs) {
        m_nextRepeatMs = now + kRepeatIntervalMs;
        acted = true;
        emit navigate(direction);
    }

    // ── Buttons: edge-triggered, so holding never repeats an action ──────────
    const WORD pressed = static_cast<WORD>(pad.wButtons & ~m_prevButtons);
    m_prevButtons = pad.wButtons;

    if (pressed)                       acted = true;
    if (pressed & XINPUT_GAMEPAD_A)    emit accept();
    if (pressed & XINPUT_GAMEPAD_B)    emit cancel();
    if (pressed & XINPUT_GAMEPAD_BACK) emit toggleRecommendations();
    // R3 rather than one of the face buttons: A and B are taken, and X/Y are
    // worth keeping free for actions rather than spending on a menu.
    if (pressed & XINPUT_GAMEPAD_RIGHT_THUMB) emit options();

    // ── Triggers: analog, so edges come from the thresholds above ────────────
    const bool leftNow = triggerHeld(pad.bLeftTrigger, m_leftTriggerHeld);
    if (leftNow && !m_leftTriggerHeld) { acted = true; emit filterPrevious(); }
    m_leftTriggerHeld = leftNow;

    const bool rightNow = triggerHeld(pad.bRightTrigger, m_rightTriggerHeld);
    if (rightNow && !m_rightTriggerHeld) { acted = true; emit filterNext(); }
    m_rightTriggerHeld = rightNow;

    if (acted) hideCursor();
}

#else  // !Q_OS_WIN

void ControllerSupport::poll() {}

#endif

// ─────────────────────────────────────────────────────────────────────────────
// Entry point used by main_ui.cpp. The object is parented to the engine, so it
// dies with it, and QML reaches it through the "controller" context property.
// ─────────────────────────────────────────────────────────────────────────────
void installControllerSupport(QQmlApplicationEngine &engine) {
    auto *controller = new ControllerSupport(&engine);
    engine.rootContext()->setContextProperty("controller", controller);
}

#include "controller_support.moc"
