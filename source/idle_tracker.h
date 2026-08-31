#pragma once

// Measures how much of a play session the player was actually there for.
//
// Playtime used to be pure wall clock: monitor_steam_session() watched the
// process come and go, and every second in between counted. A game left on a
// pause menu overnight was indistinguishable from one being played.
//
// This samples user input for the life of a session on its own thread, so it
// works for both launch paths -- the Steam one polls a registry flag, the local
// one blocks inside WaitForSingleObject, and neither has a spare loop to hang
// sampling off.
//
// Deliberately Qt-free: VORTEX_ENGINE_SOURCES is shared with VortexCLI, which
// builds with AUTOMOC off and does not link Qt, so this cannot be a QTimer.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class IdleTracker {
public:
    IdleTracker() = default;
    ~IdleTracker();

    IdleTracker(const IdleTracker&)            = delete;
    IdleTracker& operator=(const IdleTracker&) = delete;

    // Begins sampling. Call once the game is actually up -- time spent waiting
    // for it to start is not part of the session and must not be charged as
    // idle.
    void start();

    // Stops sampling, joins the thread and returns the total idle seconds.
    // Safe to call when start() never ran, or twice; returns the same figure.
    long long stop();

private:
    void run();
    void accumulate(unsigned long idleMs);

    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_wake;
    bool                    m_stopping    = false;
    long long               m_idleSeconds = 0;
    unsigned long           m_prevIdleMs  = 0;
};

// How long the player must go without touching anything before that stretch
// counts as idle. Five minutes is a grace period, not a guess: cutscenes,
// reading a quest log and a slow turn in a turn-based game all routinely pass a
// minute with no input, and charging those as idle would make the figure
// meaningless for whole genres.
long long idle_threshold_seconds();
