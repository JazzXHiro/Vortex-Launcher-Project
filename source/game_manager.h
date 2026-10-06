#pragma once

#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>


namespace fs = std::filesystem;

struct temp_GameEntry {
  std::string name;  // Name of the game
  fs::path gamePath; // Path of the game executable
  fs::path installDir; // Path to the game's root installation folder
  long long igdb_id = 0; // IGDB ID
};

std::string to_lower(std::string s);
std::string make_canonical(const std::string& s);

// The title a local install is listed and searched on IGDB under: an EA
// install's manifest title (see read_ea_manifest), otherwise the folder name.
std::string local_game_title(const fs::path &installDir);

// Walk one directory for launchable games.
//
// resolve_igdb=false skips the per-folder IGDB lookup and leaves `name` as the
// folder name with igdb_id 0, so a caller that wants titles on screen fast can
// resolve them in a second pass. See read_installed_steam_games().
void scan_directory_for_games(const fs::path &gameDir,
                              std::vector<temp_GameEntry> &outGames,
                              bool resolve_igdb = true);

// One stretch with the game itself running, as opposed to only its launcher.
struct PlaySegment {
  std::time_t start = 0;
  std::time_t end = 0;
  long long idleSeconds = 0;
  std::string exe; // file name of the process that made it game time
};

struct LocalSession {
  bool started = false;      // the exe could be started at all
  bool viaLauncher = false;  // the picked exe turned out to be a launcher
  long long launcherSeconds = 0; // time only the launcher was open; not played
  std::vector<PlaySegment> segments;
};

// Starts a local game and blocks until everything it started has closed,
// watching every process that runs out of installDir.
//
// When gamePath is a launcher -- its name says so, it is listed in
// launcher_exes.txt, or it is seen starting the game and then remembered --
// only the time the game itself runs is returned, one segment per run, so a
// launcher left open for updates is not counted as play. Otherwise the whole
// run is one segment, a stub that hands off and exits included.
//
// onStarted, when given, fires once the exe is up and has drawn its first
// window (or ten seconds have passed), which is when the UI swaps LAUNCHING
// for QUIT.
LocalSession run_local_session(const fs::path &gamePath,
                               const fs::path &installDir,
                               const std::function<void()> &onStarted = {});

// Launches a game the EA app installed (see read_ea_install) through the EA
// app, and blocks until it has closed. The EA app may take a minute to start
// the game, so it is waited for up to two minutes; the game is whatever runs
// from installDir's engine folder, its anti-cheat launcher aside. One segment
// per run of the game. onStarted fires when the game itself is first seen.
LocalSession run_ea_session(const std::string &contentId,
                            const fs::path &installDir,
                            const std::function<void()> &onStarted = {});
bool is_game_running_in_dir(const fs::path &installDir);
// Closes every process running from installDir: WM_CLOSE to its windows first
// so the game can save, then TerminateProcess on whatever is still up after
// graceSeconds. Blocks for up to that long. Returns how many it found.
int quit_games_in_dir(const fs::path &installDir, int graceSeconds = 5);