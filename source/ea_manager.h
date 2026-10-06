#pragma once

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// A game the EA app installed. contentId is the <contentID> from its
// __Installer\installerdata.xml -- the id EA's own launches pass as offerIds.
struct EaGame {
  std::string contentId;
  std::string title;
};

// Reads installDir\__Installer\installerdata.xml, EA's DiP manifest, and
// nothing else: true for any EA-made install, whichever store it came from.
// title is the en_US <gameTitle> (any locale when there is none), cleaned for
// display; it may be empty.
bool read_ea_manifest(const fs::path &installDir, EaGame &out);

// True when installDir is a game the EA app installed on this PC and can
// launch. Every check must pass, so an ordinary local game is never taken for
// one:
//   1. __Installer\installerdata.xml is an EA DiP manifest with a <contentID>;
//   2. the folder is not a Steam or Epic copy, which ship the same manifest
//      but launch through their own store;
//   3. an uninstall entry for the folder points into Common Files\EAInstaller;
//   4. the origin2:// protocol is registered, i.e. the EA app is installed.
//
// A folder with no manifest returns false silently; one that has a manifest
// but fails a later check logs why, so a misdetection shows in vortex.log.
bool read_ea_install(const fs::path &installDir, EaGame &out);

// Asks the EA app to launch the game, as the game's own exe does when started
// directly. Returns once the request is handed off; the EA app may take a
// minute (sign-in, cloud saves) before the game itself starts.
bool launch_ea_game(const std::string &contentId);
