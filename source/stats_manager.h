#pragma once

#include <string>
#include <ctime>

// Initialize the stats manager with the base directory to locate stats files.
void init_stats_manager(const std::string& base_dir);

// Records a complete play session, adds its duration to the total playtime, and appends to a log file.
void record_play_session(const std::string& game_key, const std::string& game_name, std::time_t start_time, std::time_t end_time);

// Retrieves the total playtime in seconds for the given game_key.
// Returns 0 if no playtime has been recorded.
long long get_playtime(const std::string& game_key);
