#pragma once

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

// Walk one directory for launchable games.
//
// resolve_igdb=false skips the per-folder IGDB lookup and leaves `name` as the
// folder name with igdb_id 0, so a caller that wants titles on screen fast can
// resolve them in a second pass. See read_installed_steam_games().
void scan_directory_for_games(const fs::path &gameDir,
                              std::vector<temp_GameEntry> &outGames,
                              bool resolve_igdb = true);

// Blocks until the game exits. onStarted, when given, fires once the process
// is up and has drawn its first window (or ten seconds have passed), which is
// when the UI swaps LAUNCHING for QUIT.
int launchGame(const fs::path &gamePath,
               const std::function<void()> &onStarted = {});
bool is_game_running_in_dir(const fs::path &installDir);
// Closes every process running from installDir: WM_CLOSE to its windows first
// so the game can save, then TerminateProcess on whatever is still up after
// graceSeconds. Blocks for up to that long. Returns how many it found.
int quit_games_in_dir(const fs::path &installDir, int graceSeconds = 5);