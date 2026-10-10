// Input sampling behind IdleTracker. See idle_tracker.h for why this exists.

#include "idle_tracker.h"
#include "vortex_log.h"

#include <algorithm>
#include <chrono>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#endif

namespace {

// Matches monitor_steam_session()'s poll interval. Nothing here needs to be
// finer: the numbers that come out are compared against a five-minute
// threshold, and the OS reports how long it has been idle, so a late sample
// cannot skew the total.
constexpr int kSampleIntervalMs = 2000;

constexpr unsigned long kIdleThresholdMs = 5UL * 60UL * 1000UL;

#ifdef _WIN32

// XInput, resolved at runtime exactly as ui/controller_support.cpp does it, so
// a box without the DLL degrades to keyboard-and-mouse rather than failing to
// start. That file cannot be reused here: it is a QObject in the UI-only
// target, and this has to build into the CLI too.
class GamepadWatcher {
public:
    GamepadWatcher() {
        // Newest first. 9_1_0 ships with every Windows since 7.
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
    }

    // True when any pad shows someone using it since the last call.
    //
    // This is the whole reason the class exists: XInput activity does not
    // update GetLastInputInfo, so without it a session played entirely on a
    // controller reads as one long idle stretch.
    //
    // A changed packet number is NOT that signal. It moves on any change at
    // all, so a stick drifting a few units at rest, or a virtual pad
    // (ViGEm, DS4Windows, a Moonlight stream) re-sending near-identical
    // reports, ticked it on every sample and held idle at zero for whole
    // sessions. Input is a button pressed or released, a trigger pulled, or a
    // stick out past its deadzone -- the deadzones XInput itself recommends,
    // which are what a real hand clears and drift does not. Holding a stick
    // steadily out, auto-running, still counts: that is a stick off centre,
    // not a small change.
    bool sawInput() {
        if (!m_getState) return false;

        bool moved = false;
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            XINPUT_STATE state{};
            // Empty slots are expensive to query (XInputGetState re-enumerates
            // USB), which is why controller_support.cpp backs off on them. Here
            // the sample interval is already the interval it backs off to, so
            // polling every slot each time costs the same.
            if (m_getState(slot, &state) != ERROR_SUCCESS) {
                m_known[slot] = false;
                continue;
            }
            ++connected[slot];

            const XINPUT_GAMEPAD &pad = state.Gamepad;
            // The first sample after a pad appears is its baseline: a button
            // already down is not new input, and there is nothing to compare.
            if (m_known[slot]) {
                if (state.dwPacketNumber != m_packet[slot])
                    ++changed[slot];
                const bool used =
                    pad.wButtons != m_buttons[slot] || pad.wButtons != 0 ||
                    pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
                    pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
                    outside(pad.sThumbLX, pad.sThumbLY,
                            XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) ||
                    outside(pad.sThumbRX, pad.sThumbRY,
                            XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
                if (used) {
                    ++counted[slot];
                    moved = true;
                }
            }
            m_packet[slot]  = state.dwPacketNumber;
            m_buttons[slot] = pad.wButtons;
            m_known[slot]   = true;
        }
        return moved;
    }

    // Per slot, for IdleTracker::summary(): samples the pad was connected
    // for, samples its packet number moved on, samples that were real input.
    long long connected[XUSER_MAX_COUNT] = {};
    long long changed[XUSER_MAX_COUNT]   = {};
    long long counted[XUSER_MAX_COUNT]   = {};

private:
    using XInputGetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);

    // Measured as a radius, so a diagonal is held to the same distance as a
    // straight push.
    static bool outside(SHORT x, SHORT y, int deadzone) {
        const long long dx = x, dy = y;
        return dx * dx + dy * dy >
               static_cast<long long>(deadzone) * deadzone;
    }

    XInputGetStateFn m_getState = nullptr;
    DWORD            m_packet[XUSER_MAX_COUNT]  = {};
    WORD             m_buttons[XUSER_MAX_COUNT] = {};
    bool             m_known[XUSER_MAX_COUNT]   = {};
};

unsigned long system_idle_ms() {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (!GetLastInputInfo(&info)) return 0;

    // dwTime is a 32-bit tick count and wraps every ~49.7 days. Truncating the
    // 64-bit clock to match keeps the subtraction correct across that wrap --
    // comparing the two widths directly yields a nonsense delta afterwards.
    const DWORD now = static_cast<DWORD>(GetTickCount64());
    return static_cast<unsigned long>(now - info.dwTime);
}

#endif  // _WIN32

}  // namespace

std::string IdleTracker::summary() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_samples == 0)
        return "idle check: no samples taken";

    std::string out = "idle check: longest without keyboard/mouse " +
                      vlog::duration(static_cast<long long>(m_maxSystemIdleMs / 1000UL));

    std::string pads;
    for (int slot = 0; slot < kPadSlots; ++slot) {
        const long long seen = m_padConnected[slot];
        if (seen == 0) continue;
        const auto pct = [seen](long long n) {
            return std::to_string(n * 100 / seen) + "%";
        };
        if (!pads.empty()) pads += ", ";
        pads += "pad " + std::to_string(slot + 1) + " reported changes on " +
                pct(m_padChanged[slot]) + " of samples, real input on " +
                pct(m_padCounted[slot]);
    }
    if (pads.empty()) {
        out += "; no controller connected";
    } else {
        out += ", without controller input " +
               vlog::duration(static_cast<long long>(m_maxPadQuietMs / 1000UL)) +
               " (" + pads + ")";
    }
    out += "; idle counts after " + vlog::duration(idle_threshold_seconds());
    return out;
}

long long idle_threshold_seconds() {
    return static_cast<long long>(kIdleThresholdMs / 1000UL);
}

IdleTracker::~IdleTracker() {
    stop();
}

void IdleTracker::start() {
    if (m_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping    = false;
        m_idleSeconds = 0;
        m_prevIdleMs  = 0;
    }
    m_thread = std::thread([this]() { run(); });
}

long long IdleTracker::stop() {
    if (m_thread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_idleSeconds;
}

// Sums the full length of every no-input stretch that crossed the threshold,
// grace period included. Taking the length from the OS's own idle clock rather
// than counting elapsed samples means a missed or late sample changes nothing,
// and a stretch is never charged twice.
void IdleTracker::accumulate(unsigned long idleMs) {
    if (m_prevIdleMs >= kIdleThresholdMs && idleMs < m_prevIdleMs)
        m_idleSeconds += static_cast<long long>(m_prevIdleMs / 1000UL);
    m_prevIdleMs = idleMs;
}

void IdleTracker::run() {
#ifdef _WIN32
    GamepadWatcher pads;

    // Establish the pads' baseline packet numbers before the first comparison,
    // or the first sample reports input that never happened.
    pads.sawInput();

    // When a pad last reported input -- or, until one does, when sampling
    // began, since no-input time from before the game came up is not this
    // session's to charge.
    auto lastPadInput = std::chrono::steady_clock::now();

    // A pad sample can only say "input since the last sample", so it becomes
    // an idle figure of its own and the smaller of that and the system's wins.
    // Zeroing just the one sample is not enough: the next quiet interval would
    // hand back GetLastInputInfo's figure, which counts from the last keyboard
    // or mouse touch -- for a controller player, possibly the whole session --
    // and the following pad press would then charge all of it as idle.
    const auto take = [this, &pads, &lastPadInput]() -> unsigned long {
        const auto now = std::chrono::steady_clock::now();
        if (pads.sawInput()) lastPadInput = now;
        const unsigned long sincePadMs = static_cast<unsigned long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastPadInput).count());
        const unsigned long systemMs = system_idle_ms();
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            ++m_samples;
            m_maxSystemIdleMs = std::max(m_maxSystemIdleMs, systemMs);
            m_maxPadQuietMs   = std::max(m_maxPadQuietMs, sincePadMs);
        }
        return std::min(systemMs, sincePadMs);
    };

    for (;;) {
        // Sampled outside the lock on purpose: XInputGetState on an empty slot
        // re-enumerates USB, and holding the mutex across that would stall
        // stop() behind it.
        const unsigned long idleMs = take();
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            accumulate(idleMs);
        }

        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_wake.wait_for(lock, std::chrono::milliseconds(kSampleIntervalMs),
                            [this] { return m_stopping; }))
            break;
    }

    // The game exited somewhere inside the last interval, so take a final
    // reading rather than trusting one that could be two seconds stale, then
    // close out a stretch still open at exit.
    const unsigned long finalIdleMs = take();
    std::lock_guard<std::mutex> guard(m_mutex);
    accumulate(finalIdleMs);
    if (m_prevIdleMs >= kIdleThresholdMs)
        m_idleSeconds += static_cast<long long>(m_prevIdleMs / 1000UL);

    for (int slot = 0; slot < kPadSlots; ++slot) {
        m_padConnected[slot] = pads.connected[slot];
        m_padChanged[slot]   = pads.changed[slot];
        m_padCounted[slot]   = pads.counted[slot];
    }
#else
    // No input source to sample off Windows; the session simply reports no idle
    // rather than guessing at one.
    std::unique_lock<std::mutex> lock(m_mutex);
    m_wake.wait(lock, [this] { return m_stopping; });
#endif
}
