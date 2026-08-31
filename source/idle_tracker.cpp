// Input sampling behind IdleTracker. See idle_tracker.h for why this exists.

#include "idle_tracker.h"

#include <chrono>

#ifdef _WIN32
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

    // True when any pad reported new input since the last call.
    //
    // This is the whole reason the class exists: XInput activity does not
    // update GetLastInputInfo, so without it a session played entirely on a
    // controller reads as one long idle stretch.
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
            if (m_known[slot] && state.dwPacketNumber != m_packet[slot])
                moved = true;
            m_packet[slot] = state.dwPacketNumber;
            m_known[slot]  = true;
        }
        return moved;
    }

private:
    using XInputGetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);

    XInputGetStateFn m_getState = nullptr;
    DWORD            m_packet[XUSER_MAX_COUNT] = {};
    bool             m_known[XUSER_MAX_COUNT]  = {};
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

    const auto take = [&pads]() -> unsigned long {
        return pads.sawInput() ? 0UL : system_idle_ms();
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
#else
    // No input source to sample off Windows; the session simply reports no idle
    // rather than guessing at one.
    std::unique_lock<std::mutex> lock(m_mutex);
    m_wake.wait(lock, [this] { return m_stopping; });
#endif
}
