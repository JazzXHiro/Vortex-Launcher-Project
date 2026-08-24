#pragma once

#include <filesystem>
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

int launchGame(const fs::path &gamePath);
bool is_game_running_in_dir(const fs::path &installDir);