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
#include <string>
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

    // One line saying why the session came out with the idle it did, for the
    // log, read after stop(): the longest stretch without keyboard or mouse,
    // the longest without controller input, and per connected pad how often it
    // reported a change against how often that change was real input.
    //
    // Idle is the shorter of those two stretches, so a figure of 0 over a long
    // session has exactly two explanations, and this tells them apart. Without
    // it, an 11-hour session with no idle left nothing to go on.
    std::string summary() const;

    static constexpr int kPadSlots = 4;

private:
    void run();
    void accumulate(unsigned long idleMs);

    std::thread             m_thread;
    mutable std::mutex      m_mutex;
    std::condition_variable m_wake;
    bool                    m_stopping    = false;
    long long               m_idleSeconds = 0;
    unsigned long           m_prevIdleMs  = 0;

    // Diagnostics only; nothing here feeds the idle figure.
    long long     m_samples          = 0;
    unsigned long m_maxSystemIdleMs  = 0;
    unsigned long m_maxPadQuietMs    = 0;
    long long     m_padConnected[kPadSlots] = {};
    long long     m_padChanged[kPadSlots]   = {};
    long long     m_padCounted[kPadSlots]   = {};
};

// How long the player must go without touching anything before that stretch
// counts as idle. Five minutes is a grace period, not a guess: cutscenes,
// reading a quest log and a slow turn in a turn-based game all routinely pass a
// minute with no input, and charging those as idle would make the figure
// meaningless for whole genres.
long long idle_threshold_seconds();
