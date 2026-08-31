#pragma once

#include <string>
#include <vector>
#include <ctime>

// Initialize the stats manager with the base directory to locate stats files.
void init_stats_manager(const std::string& base_dir);

// Records a complete play session, adds its duration to the total playtime, and appends to a log file.
//
// idle_seconds is the part of the session IdleTracker saw no input for. The
// duration is still stored whole -- the log keeps the wall clock the session
// actually took, and idle is carried beside it rather than subtracted from it,
// so the two can never be confused for one another after the fact.
void record_play_session(const std::string& game_key, const std::string& game_name,
                         std::time_t start_time, std::time_t end_time,
                         long long idle_seconds = 0);

// Retrieves the total playtime in seconds for the given game_key.
// Returns 0 if no playtime has been recorded.
long long get_playtime(const std::string& game_key);

// Total idle seconds accumulated across every session Vortex recorded for this
// game. Always <= get_playtime(); a game imported from Steam and never played
// through Vortex reports 0, because nobody measured that history.
long long get_idle_time(const std::string& game_key);

// The part of get_playtime() that was imported rather than watched. 0 for a
// game Vortex has tracked from the start, which is what makes the imported
// share reportable without a second bookkeeping file.
long long get_baseline_playtime(const std::string& game_key);

// Folds Steam's lifetime total into a game's playtime, once per game.
//
// Steam knows how long a game was played before Vortex ever saw it, and that
// history is lost otherwise. It can only be taken once: Vortex launches Steam
// games through the Steam protocol, so Steam counts Vortex's own sessions too,
// and importing twice would add those hours again.
//
// Two shapes, both one-time:
//   * no row yet      -> Steam's total becomes the starting playtime.
//   * row, baseline 0 -> the game was tracked before it was ever imported (an
//                        upgrade, or a first scan while localconfig.vdf was
//                        unreadable). Steam's total already contains our own
//                        sessions, so the difference is the play we never saw.
//
// Writing a non-zero baseline is what closes the window, so this converges
// rather than letting a game drift back into following Steam's figure.
//
// Returns true when something was imported. A no-op -- returning false -- when
// steam_seconds <= 0, which is what makes an unreadable localconfig.vdf
// recoverable: the next scan simply tries again.
//
// game_key must be the same playtime key record_play_session() uses, not
// "steam_<appid>" -- a Steam game whose IGDB id resolved is stored under
// "igdb_<id>", and seeding the wrong key would strand the import.
bool import_steam_baseline(const std::string& game_key, const std::string& game_name,
                           long long steam_seconds);

// True when the user has asked for Steam's own lifetime total to be displayed
// instead of the one Vortex keeps. Read from settings.json, which the UI owns;
// the CLI reads the same file through here so the two cannot disagree about
// which number they are showing.
bool use_steam_playtime();

// One row of the stats file.
struct PlayStat {
    std::string key;
    std::string name;
    long long   seconds = 0;
    // Idle time inside `seconds`, and the part of `seconds` that came from an
    // external import rather than from a session Vortex watched.
    long long   idle_seconds     = 0;
    long long   baseline_seconds = 0;
};

// Every figure for one game in a single read.
//
// The stats file is parsed whole on each of get_playtime / get_idle_time /
// get_baseline_playtime, and the launcher wants all three for every row it
// builds -- which turned one read per row into three, on every list rebuild and
// twice per game during a scan. `key` is empty in the returned stat when the
// game has no row.
PlayStat get_play_stat(const std::string& game_key);

// Every game the stats file has ever recorded a session for, including ones that
// are no longer installed -- the file is only ever added to. get_playtime() can
// only answer for a key the caller already knows, so it cannot enumerate the
// history; the Played tab needs exactly that.
std::vector<PlayStat> get_all_play_stats();
