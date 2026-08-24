#pragma once

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// Every Vortex data file — local_game_dirs.txt, Images/, the caches, playtime
// and preferences — lives next to the running executable.
//
// This is deliberately based on the executable's own location rather than the
// working directory: the CLI and the launcher used to disagree about where
// their data lived (the CLI resolved relative paths against whatever directory
// it was started from), so the same install ended up with two configs and two
// artwork folders that drifted apart.
fs::path app_data_dir();

// Convenience: app_data_dir() / name
fs::path app_data_path(const std::string &name);
