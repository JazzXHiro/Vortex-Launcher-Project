#include <algorithm>
#include <cctype>
#include <ctime>
#include <iostream>
#include <set>
#include <vector>
#include <fstream>
#include <string>

#include "app_paths.h"
#include "game_manager.h"
#include "igdb_manager.h"
#include "metadata_manager.h"
#include "preference_manager.h"
#include "stats_manager.h"
#include "steam_manager.h"
#include "steamgriddb_manager.h"
#include "vortex_log.h"

namespace fs = std::filesystem;

using std::cin;
using std::cout;

enum class GameSource { Steam, Local };

struct UnifiedGame {
    GameSource source = GameSource::Local;
    std::string name;
    long long igdb_id = 0;
    fs::path installDir;
    int appid = 0;
    fs::path gamePath;
};

// Resolved once against the executable's directory, so the CLI reads and writes
// the same files as the launcher no matter where it is started from.
static const fs::path kLocalDirsConfigPath = app_data_path("local_game_dirs.txt");

static std::string trim_copy(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    size_t last = s.find_last_not_of(" \t\r\n");
    s.erase(last == std::string::npos ? 0 : last + 1);
    return s;
}

// Paths dragged into a console (or copied from Explorer) often arrive quoted.
static std::string strip_quotes(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = trim_copy(s.substr(1, s.size() - 2));
    }
    return s;
}

static std::vector<fs::path> read_local_game_dirs() {
    std::vector<fs::path> dirs;
    if (!fs::exists(kLocalDirsConfigPath)) {
        return dirs;
    }
    std::ifstream file(kLocalDirsConfigPath);
    std::string line;
    while (std::getline(file, line)) {
        line = trim_copy(line);
        if (!line.empty() && line[0] != '#') {
            dirs.push_back(line);
        }
    }
    return dirs;
}

static void save_local_game_dirs(const std::vector<fs::path> &dirs) {
    std::ofstream out(kLocalDirsConfigPath);
    if (!out) {
        std::cout << "[WARN] Could not write " << kLocalDirsConfigPath << ".\n";
        return;
    }
    out << "# Local Game Directories\n";
    for (const auto &d : dirs) {
        out << d.string() << "\n";
    }
}

// Same folder reached through different spellings (trailing slash, casing,
// relative path) should only be stored once.
static bool contains_directory(const std::vector<fs::path> &dirs, const fs::path &candidate) {
    std::error_code ec;
    fs::path target = fs::weakly_canonical(candidate, ec);
    if (ec) target = candidate;
    std::string targetKey = to_lower(target.string());

    for (const auto &d : dirs) {
        std::error_code dec;
        fs::path existing = fs::weakly_canonical(d, dec);
        if (dec) existing = d;
        if (to_lower(existing.string()) == targetKey) {
            return true;
        }
    }
    return false;
}

static std::vector<fs::path> get_local_game_directories() {
    std::vector<fs::path> dirs = read_local_game_dirs();
    if (!dirs.empty()) {
        return dirs;
    }

    std::cout << "\n=== Local Games Configuration ===\n";
    std::cout << "No local game directories configured.\n";
    std::cout << "Please enter the full paths to your local game folders (e.g., E:\\Games).\n";
    std::cout << "Enter an empty line when you are finished.\n";
    
    // Clear any leftover newlines from previous formatted inputs
    if (std::cin.peek() == '\n') std::cin.ignore();

    std::string line;
    while (true) {
        std::cout << "Folder path (or press Enter to finish): ";
        if (!std::getline(std::cin, line)) {
            std::cin.clear();
            break;
        }

        line = strip_quotes(trim_copy(line));

        if (line.empty()) {
            if (dirs.empty()) {
                std::cout << "You must enter at least one directory.\n";
                continue;
            }
            break;
        }

        if (!fs::exists(line) || !fs::is_directory(line)) {
            std::cout << "Invalid directory path or directory does not exist. Please try again.\n";
            continue;
        }
        if (contains_directory(dirs, line)) {
            std::cout << "That folder is already in the list.\n";
            continue;
        }
        dirs.push_back(line);
    }

    save_local_game_dirs(dirs);

    return dirs;
}

static void print_directory_list(const std::vector<fs::path> &dirs) {
    if (dirs.empty()) {
        std::cout << "No local game directories are configured yet.\n";
        return;
    }
    std::cout << "Currently configured directories:\n";
    for (size_t i = 0; i < dirs.size(); ++i) {
        std::cout << "  [" << (i + 1) << "] " << dirs[i].string() << "\n";
    }
}

// Drops dirs[index] and deletes the artwork of every game that lived only there.
// A game still reachable from another configured folder — or installed through
// Steam — keeps its images, since the same artwork folder is shared by name.
static void remove_game_directory(std::vector<fs::path> &dirs, size_t index) {
    const fs::path removed = dirs[index];

    std::cout << "\nChecking what lives in \"" << removed.string() << "\"...\n";
    std::vector<temp_GameEntry> removedGames;
    scan_directory_for_games(removed, removedGames);

    // Names that survive the removal — their artwork must be left alone.
    std::vector<std::string> keepNames;
    for (size_t i = 0; i < dirs.size(); ++i) {
        if (i == index) continue;
        std::vector<temp_GameEntry> otherGames;
        scan_directory_for_games(dirs[i], otherGames);
        for (const temp_GameEntry &g : otherGames) keepNames.push_back(g.name);
    }
    for (const SteamGame &g : read_installed_steam_games()) {
        keepNames.push_back(g.name);
    }

    std::set<std::string> keepLookup;
    for (const std::string &n : keepNames) keepLookup.insert(to_lower(n));

    std::vector<std::string> artworkToDelete;
    for (const temp_GameEntry &g : removedGames) {
        if (keepLookup.count(to_lower(g.name))) continue;   // shared with Steam / another folder
        artworkToDelete.push_back(g.name);
    }

    std::cout << "This removes " << removedGames.size() << " game"
              << (removedGames.size() == 1 ? "" : "s") << " from your lists and deletes artwork for "
              << artworkToDelete.size() << " of them.\n";
    std::cout << "Continue? (y/N): ";

    std::string confirm;
    if (!std::getline(std::cin, confirm)) std::cin.clear();
    confirm = trim_copy(confirm);
    if (confirm != "y" && confirm != "Y") {
        std::cout << "[✗] Removal cancelled.\n";
        return;
    }

    dirs.erase(dirs.begin() + static_cast<std::ptrdiff_t>(index));
    save_local_game_dirs(dirs);

    const fs::path imagesRoot = app_data_path("Images");
    // keepNames is passed through as a second guard: artworkToDelete was already
    // filtered, so nothing should be skipped here.
    const int deleted = delete_steamgriddb_images(artworkToDelete, keepNames, imagesRoot.string());

    std::cout << "[✓] Removed \"" << removed.string() << "\" from " << kLocalDirsConfigPath << ".\n";
    std::cout << "    " << removedGames.size() << " game"
              << (removedGames.size() == 1 ? "" : "s") << " will no longer appear in your lists.\n";
    std::cout << "    Deleted artwork for " << deleted << " game"
              << (deleted == 1 ? "" : "s") << ".\n";
}

// Handles a "remove <number>" line. Returns true if the line was such a command
// (whether or not it removed anything), so the caller stops treating it as a path.
static bool handle_remove_command(const std::string &line, std::vector<fs::path> &dirs,
                                  int &removedCount) {
    const std::string lowered = to_lower(line);
    if (lowered.rfind("remove", 0) != 0) return false;
    if (line.size() > 6 && !std::isspace(static_cast<unsigned char>(line[6]))) return false;

    const std::string arg = trim_copy(line.substr(6));
    const bool allDigits = !arg.empty() &&
        std::all_of(arg.begin(), arg.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; });

    if (!allDigits) {
        std::cout << "Usage: remove <number>   (for example: remove 1)\n";
        return true;
    }

    unsigned long long number = 0;
    try {
        number = std::stoull(arg);
    } catch (const std::exception &) {
        // Digits that overflow an unsigned long long — certainly out of range.
        std::cout << "That many directories don't exist yet!\n";
        return true;
    }

    if (number == 0) {
        std::cout << "Directory numbers start at 1.\n";
        return true;
    }
    if (number > dirs.size()) {
        std::cout << "That many directories don't exist yet!\n";
        return true;
    }

    remove_game_directory(dirs, static_cast<size_t>(number - 1));
    ++removedCount;

    std::cout << "\n";
    print_directory_list(dirs);
    std::cout << "\n";
    return true;
}

// Menu option [4]: add or remove game folders after the initial setup.
static void run_add_game_folders_mode() {
    std::vector<fs::path> dirs = read_local_game_dirs();

    std::cout << "\n=== Add Game Folder ===\n";
    print_directory_list(dirs);
    std::cout << "\nEnter a folder path to add (e.g., E:\\Games).\n";
    std::cout << "Type \"remove <number>\" to delete one (e.g., remove 1).\n";
    std::cout << "Press Enter on an empty line to go back to the menu.\n\n";

    // Drop the newline left behind by the "Choose mode:" numeric read.
    std::cin.ignore(10000, '\n');

    int added = 0;
    int removedCount = 0;
    std::vector<fs::path> newDirs;   // only the folders added in this session
    std::string line;
    while (true) {
        std::cout << "Folder path (or press Enter to go back): ";
        if (!std::getline(std::cin, line)) {
            std::cin.clear();
            break;
        }

        line = strip_quotes(trim_copy(line));

        if (line.empty()) {
            break;
        }

        if (handle_remove_command(line, dirs, removedCount)) {
            continue;
        }

        if (!fs::exists(line) || !fs::is_directory(line)) {
            std::cout << "Invalid directory path or directory does not exist. Please try again.\n";
            continue;
        }
        if (contains_directory(dirs, line)) {
            std::cout << "That folder is already in the list.\n";
            continue;
        }

        dirs.push_back(line);
        newDirs.push_back(line);
        save_local_game_dirs(dirs);
        ++added;
        std::cout << "[✓] Added \"" << fs::path(line).string() << "\".\n";
    }

    if (added == 0) {
        // Removals report themselves as they happen.
        if (removedCount == 0) std::cout << "\nNo changes made.\n";
        return;
    }

    std::cout << "\n[✓] Saved " << added << " new folder"
              << (added == 1 ? "" : "s") << " to " << kLocalDirsConfigPath << ".\n";

    // Same pass main() runs at startup, but only over the folders just added —
    // without it the new games would have no artwork until the next launch.
    std::cout << "Checking for new games to download artwork...\n";
    std::vector<temp_GameEntry> newGames;
    for (const fs::path &dir : newDirs) {
        scan_directory_for_games(dir, newGames);
    }

    if (newGames.empty()) {
        std::cout << "No games found in the new folder"
                  << (added == 1 ? "" : "s") << ".\n";
        return;
    }

    std::vector<std::string> newGameNames;
    newGameNames.reserve(newGames.size());
    for (const temp_GameEntry &g : newGames) {
        newGameNames.push_back(g.name);
    }

    fs::path imagesRoot = app_data_path("Images");
    ensure_steamgriddb_images(newGameNames, imagesRoot.string());
    std::cout << "Artwork check complete for " << newGameNames.size()
              << " game" << (newGameNames.size() == 1 ? "" : "s") << ".\n";
}

static std::vector<UnifiedGame> get_local_games() {
  std::vector<temp_GameEntry> localGames;
  auto gameDirs = get_local_game_directories();
  for (const auto& dir : gameDirs) {
      scan_directory_for_games(dir, localGames);
  }
  
  std::vector<UnifiedGame> games;
  games.reserve(localGames.size());
  for (const auto& g : localGames) {
      UnifiedGame ug;
      ug.source = GameSource::Local;
      ug.name = g.name;
      ug.igdb_id = g.igdb_id;
      ug.installDir = g.installDir;
      ug.gamePath = g.gamePath;
      games.push_back(std::move(ug));
  }
  return games;
}

static std::vector<UnifiedGame> get_steam_games() {
  std::vector<SteamGame> steamGames = read_installed_steam_games();
  std::vector<UnifiedGame> games;
  games.reserve(steamGames.size());
  for (const auto& g : steamGames) {
      UnifiedGame ug;
      ug.source = GameSource::Steam;
      ug.name = g.name;
      ug.igdb_id = g.igdb_id;
      ug.installDir = g.installDir;
      ug.appid = g.appid;
      games.push_back(std::move(ug));
  }
  return games;
}

// Steam's own total is authoritative for Steam games: it counts sessions started
// outside Vortex too, which our log never sees. Our record is the fallback for
// anything Steam has no figure for, and the only source for local games.
static long long display_playtime_seconds(const UnifiedGame &game, const std::string &key) {
    if (game.source == GameSource::Steam) {
        long long steamSeconds = get_steam_playtime_seconds(game.appid);
        if (steamSeconds > 0) return steamSeconds;
    }
    return get_playtime(key);
}

static void run_games_menu(std::vector<UnifiedGame>& games, const std::string& menu_title) {
  if (games.empty()) {
    cout << "No games found for " << menu_title << ".\n";
    return;
  }

  std::sort(games.begin(), games.end(), [](const UnifiedGame &a, const UnifiedGame &b) {
      return to_lower(a.name) < to_lower(b.name);
  });

  while (true) {
    cout << "\n=== " << menu_title << " ===\n";
    for (size_t i = 0; i < games.size(); ++i) {
      cout << "  [" << (i + 1) << "] " << games[i].name;
      if (menu_title == "All Games") {
          if (games[i].source == GameSource::Steam) cout << " (Steam)";
          else cout << " (Local)";
      }

      // Only favourites are marked now; there is no negative preference.
      if (get_game_preference(games[i].name) > 0.0) {
          cout << " [favorite]";
      }

      std::string key = (games[i].igdb_id != 0)
                            ? "igdb_" + std::to_string(games[i].igdb_id)
                            : (games[i].source == GameSource::Steam ? "steam_" + std::to_string(games[i].appid) : "local_" + make_canonical(games[i].name));
      long long pt = display_playtime_seconds(games[i], key);
      if (pt > 0)
        cout << " (Playtime: " << (pt / 60) << " min played)";
      if (games[i].igdb_id != 0) {
        cout << " [IGDB: " << games[i].igdb_id << "]";
      } else {
        cout << " [Unrecognized]";
      }
      cout << "\n";
    }
    cout << "  [0] Back\n\n";

    cout << "Enter selection: ";
    int choice = -1;
    if (!(cin >> choice)) {
      cin.clear();
      cin.ignore(10000, '\n');
      cout << "Invalid input.\n";
      continue;
    }

    if (choice == 0)
      break;
    if (choice < 1 || choice > static_cast<int>(games.size())) {
      cout << "Invalid selection.\n";
      continue;
    }

    UnifiedGame &selected = games[choice - 1];

    while (true) {
      cout << "\n--- Game Details ---\n";
      cout << "Name: " << selected.name;
      if (selected.source == GameSource::Steam) cout << " (AppID: " << selected.appid << ")\n";
      else cout << "\n";

      std::string key = (selected.igdb_id != 0)
                            ? "igdb_" + std::to_string(selected.igdb_id)
                            : (selected.source == GameSource::Steam ? "steam_" + std::to_string(selected.appid) : "local_" + make_canonical(selected.name));
      long long pt = display_playtime_seconds(selected, key);
      cout << "Total Playtime: ";
      if (pt > 0) {
        cout << (pt / 60) << " minutes (" << pt << " seconds)";
        if (selected.source == GameSource::Steam && get_steam_playtime_seconds(selected.appid) > 0)
          cout << " [from Steam]";
        cout << "\n";
      } else {
        cout << "Never played\n";
      }
      if (selected.igdb_id != 0) {
        cout << "IGDB ID: " << selected.igdb_id << "\n";
        GameMetadata meta = get_game_metadata(selected.igdb_id);
        cout << "Developer: " << meta.developer << "\n";
        cout << "Rating: " << (meta.rating > 0 ? std::to_string(meta.rating) : "N/A") << "\n";
        if (meta.time_to_beat_seconds > 0) {
          cout << "Time to Beat: " << (meta.time_to_beat_seconds / 3600) << " hours\n";
        } else {
          cout << "Time to Beat: N/A\n";
        }
        
        cout << "Genres: ";
        if (!meta.all_genres.empty()) {
            for (size_t i = 0; i < meta.all_genres.size(); ++i) {
                cout << meta.all_genres[i] << (i + 1 < meta.all_genres.size() ? ", " : "");
            }
            cout << "\n";
        } else {
            cout << "Unknown\n";
        }
        
        cout << "[ML] Main Genres: ";
        if (!meta.main_genres.empty()) {
            for (size_t i = 0; i < meta.main_genres.size(); ++i) {
                cout << meta.main_genres[i] << (i + 1 < meta.main_genres.size() ? ", " : "");
            }
            cout << "\n";
        } else {
            cout << "Unknown\n";
        }
      } else {
        cout << "IGDB ID: Unrecognized\n";
      }

      cout << "Install Dir: " << selected.installDir.string() << "\n";
      if (selected.source == GameSource::Local) {
          cout << "Path: " << selected.gamePath.string() << "\n";
      }
      cout << "\n";

      cout << "  [1] Launch Game\n";
      cout << "  [2] Favorite\n";
      cout << "  [3] Uninstall\n";
      cout << "  [0] Back to List\n\n";
      cout << "Enter selection: ";

      int action = -1;
      if (!(cin >> action)) {
        cin.clear();
        cin.ignore(10000, '\n');
        cout << "Invalid input.\n";
        continue;
      }

      if (action == 0)
        break;
      
      if (action == 2) {
          double new_pref = toggle_game_preference(selected.name, 1.0);
          if (new_pref > 0) cout << "[✓] " << selected.name << " -> Favorited\n";
          else cout << "[✓] " << selected.name << " -> Not favorited\n";
          continue;
      } else if (action == 3) {
          cout << "\n[?] Are you sure you want to uninstall \"" << selected.name << "\"? (y/N): ";
          std::string confirm;
          if (cin >> confirm && (confirm == "y" || confirm == "Y")) {
              if (selected.source == GameSource::Steam) {
                  if (uninstall_steam_game_by_appid(selected.appid)) {
                      cout << "[✓] Uninstall request sent for \"" << selected.name << "\" (AppID: " << selected.appid << ")\n";
                      cout << "    Steam will handle the rest.\n";
                  } else {
                      cout << "[WARN] Failed to send uninstall request to Steam.\n";
                  }
              } else {
                  int possibleSteamAppId = get_steam_appid_for_install_dir(selected.installDir);
                  if (possibleSteamAppId > 0) {
                      if (uninstall_steam_game_by_appid(possibleSteamAppId)) {
                          cout << "[✓] Uninstall request sent to Steam for \"" << selected.name << "\" (AppID: " << possibleSteamAppId << ")\n";
                          cout << "    Steam will handle the rest.\n";
                      } else {
                          cout << "[WARN] Failed to send uninstall request to Steam.\n";
                      }
                  } else {
                      std::error_code ec;
                      fs::remove_all(selected.installDir, ec);
                      if (!ec) {
                          cout << "[✓] Successfully uninstalled \"" << selected.name << "\".\n";
                          games.erase(games.begin() + choice - 1);
                          break; // Return to the main game list
                      } else {
                          cout << "[WARN] Failed to uninstall \"" << selected.name << "\": " << ec.message() << "\n";
                      }
                  }
              }
          } else {
              cout << "[✗] Uninstall cancelled.\n";
          }
          continue;
      } else if (action == 1) {
        cout << "\nLaunching: " << selected.name << "...\n";
        auto start_time = std::time(nullptr);
        
        if (selected.source == GameSource::Steam) {
            if (!launch_steam_game_by_appid(selected.appid)) {
              cout << "[WARN] Failed to launch via Steam protocol.\n";
              break;
            }
            cout << "Waiting for the game to start...\n";
            std::time_t session_start = 0, session_end = 0;
            if (monitor_steam_session(selected.appid, selected.installDir,
                                      &session_start, &session_end)) {
              long long duration = session_end - session_start;
              record_play_session(key, selected.name, session_start, session_end);
              refresh_steam_playtime();   // Steam writes its own total on exit
              cout << "Game closed. Recorded " << duration << " seconds of playtime.\n";
            } else {
              cout << "[INFO] The game did not start within 2 minutes. No playtime recorded.\n";
            }
        } else {
            int code = launchGame(selected.gamePath);
            auto end_time = std::time(nullptr);
            
            if (code != 0) {
              cout << "[INFO] Process exited with code: " << code << "\n";
            }
            
            if (end_time > start_time) {
              long long duration = end_time - start_time;
              record_play_session(key, selected.name, start_time, end_time);
              cout << "Recorded " << duration << " seconds of playtime.\n";
            }
        }
        break; // Return to the main game list after game closes
      } else {
        cout << "Invalid selection.\n";
      }
    }
  }
}

static void run_all_mode() {
  auto localGames = get_local_games();
  auto steamGames = get_steam_games();
  
  std::vector<UnifiedGame> allGames;
  allGames.reserve(localGames.size() + steamGames.size());
  
  for (auto& g : localGames) allGames.push_back(std::move(g));
  for (auto& g : steamGames) allGames.push_back(std::move(g));
  
  run_games_menu(allGames, "All Games");
}

static void run_local_mode() {
  auto games = get_local_games();
  run_games_menu(games, "Local Games");
}

static void run_steam_mode() {
  auto games = get_steam_games();
  run_games_menu(games, "Steam Games");
}

int main() {
  // Shares the launcher's log file and unbuffered stdout, so a scan driven
  // from the CLI produces the same diagnostics as one driven from the GUI.
  vlog::init();

  // Playtime files land beside the executable, matching the launcher.
  init_stats_manager(app_data_dir().string());

  cout << "Checking for new games to download artwork...\n";
  auto localGamesInit = get_local_games();
  auto steamGamesInit = get_steam_games();
  std::vector<std::string> allGameNames;
  for (const auto& g : localGamesInit) allGameNames.push_back(g.name);
  for (const auto& g : steamGamesInit) allGameNames.push_back(g.name);
  fs::path imagesRoot = app_data_path("Images");
  ensure_steamgriddb_images(allGameNames, imagesRoot.string());
  cout << "Artwork check complete.\n";

  while (true) {
    cout << "\n=== Vortex Game Launcher ===\n";
    cout << "  [1] All games\n";
    cout << "  [2] Steam games\n";
    cout << "  [3] Local directory games\n";
    cout << "  [4] Add game folder location\n";
    cout << "  [0] Exit\n\n";
    cout << "Choose mode: ";

    int mode = -1;
    if (!(cin >> mode)) {
      cin.clear();
      cin.ignore(10000, '\n');
      cout << "Invalid input.\n";
      continue;
    }

    if (mode == 0) {
      cout << "Cya chap\n";
      break;
    } else if (mode == 1) {
      run_all_mode();
    } else if (mode == 2) {
      run_steam_mode();
    } else if (mode == 3) {
      run_local_mode();
    } else if (mode == 4) {
      run_add_game_folders_mode();
    } else {
      cout << "Invalid selection.\n";
    }
  }

  return 0;
}