#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// A game the Riot Client installed: VALORANT, League of Legends, 2XKO and so
// on. Riot keeps the list itself, in C:\ProgramData\Riot Games -- its answer
// to Steam's libraryfolders.vdf -- so these are found without the user adding
// a folder, and started the way Riot's own desktop shortcut starts them.
struct RiotGame {
  std::string product;   // "valorant", as --launch-product takes it
  std::string patchline; // "live", as --launch-patchline takes it
  std::string title;     // "VALORANT", from Riot's uninstall entry
  fs::path installDir;   // product_install_full_path, e.g. ...\VALORANT\live
  fs::path installRoot;  // product_install_root, e.g. D:\Riot Games
  fs::path clientExe;    // RiotClientServices.exe that owns the install
};

// Every Riot product installed on this PC. Read from
// Metadata\<product>.<patchline>\<product>.<patchline>.product_settings.yaml
// for the install folder and RiotClientInstalls.json for the client. A product
// whose folder is gone (Riot leaves the metadata and uninstall entry behind)
// or that has no client to start it is left out, and logged when log is set.
std::vector<RiotGame> read_installed_riot_games(bool log = false);

// True when dir is game's install folder, or the folder just above it as long
// as that is not the Riot Games root that holds every product.
bool riot_install_matches(const RiotGame &game, const fs::path &dir);

// True when installDir is a Riot product's install folder or the folder just
// above it -- a local-folder scan of D:\Riot Games finds VALORANT\, while Riot
// installs to VALORANT\live\. out.installDir is always Riot's own folder.
bool read_riot_install(const fs::path &installDir, RiotGame &out);

// Asks the Riot Client to start the game:
//   RiotClientServices.exe --launch-product=<p> --launch-patchline=<pl>
// The game's own exe is never started or touched -- Vanguard expects it to
// come from the client. Returns once the client has the request; sign-in and
// Vanguard's own checks come before the game itself starts.
bool launch_riot_game(const RiotGame &game);

// Hands the uninstall to the Riot Client (--uninstall-product), which shows
// its own prompt. Vortex deletes nothing itself.
bool uninstall_riot_game(const RiotGame &game);
