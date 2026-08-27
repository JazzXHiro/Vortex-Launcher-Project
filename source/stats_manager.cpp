// Persistent per-game playtime totals and a human-readable session log.

#include "stats_manager.h"

#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <ctime>

namespace fs = std::filesystem;

struct StatEntry {
    long long time = 0;
    std::string name;
};

static fs::path g_base_dir;

void init_stats_manager(const std::string& base_dir) {
    g_base_dir = fs::path(base_dir);
}

static fs::path get_stats_file_path() {
    return g_base_dir.empty() ? fs::path("playtime_stats.txt") : (g_base_dir / "playtime_stats.txt");
}

static fs::path get_sessions_file_path() {
    return g_base_dir.empty() ? fs::path("playtime_sessions.log") : (g_base_dir / "playtime_sessions.log");
}

static std::string format_time(std::time_t t) {
    char buf[64];
    std::tm tm_info;
#ifdef _WIN32
    localtime_s(&tm_info, &t);
#else
    tm_info = *std::localtime(&t);
#endif
    // Format: 2026-04-28 Tue 19:40:00
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %a %H:%M:%S", &tm_info);
    return std::string(buf);
}

// Reads the playtime data from the file into an unordered_map
static std::unordered_map<std::string, StatEntry> load_stats() {
    std::unordered_map<std::string, StatEntry> stats;
    fs::path stats_path = get_stats_file_path();
    if (!fs::exists(stats_path)) {
        return stats;
    }

    std::ifstream file(stats_path);
    if (!file.is_open()) {
        std::cerr << "[WARN] Failed to open " << stats_path << " for reading.\n";
        return stats;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        auto pos = line.find('=');
        if (pos != std::string::npos) {
            try {
                std::string key = line.substr(0, pos);
                std::string rest = line.substr(pos + 1);
                
                long long time = 0;
                std::string name;
                
                auto pipe_pos = rest.find('|');
                if (pipe_pos != std::string::npos) {
                    time = std::stoll(rest.substr(0, pipe_pos));
                    name = rest.substr(pipe_pos + 1);
                } else {
                    time = std::stoll(rest);
                }
                
                stats[key] = {time, name};
            } catch (...) {
                // Ignore parsing errors for individual lines
            }
        }
    }
    return stats;
}

// Writes the playtime data from the unordered_map back to the file
static void save_stats(const std::unordered_map<std::string, StatEntry>& stats) {
    fs::path stats_path = get_stats_file_path();
    std::ofstream file(stats_path, std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "[WARN] Failed to open " << stats_path << " for writing.\n";
        return;
    }

    file << "# Vortex Playtime Stats\n";
    file << "# Format: GAME_KEY=TOTAL_SECONDS|GAME_NAME\n";
    for (const auto& [key, entry] : stats) {
        file << key << "=" << entry.time;
        if (!entry.name.empty()) {
            file << "|" << entry.name;
        }
        file << "\n";
    }
}

void record_play_session(const std::string& game_key, const std::string& game_name, std::time_t start_time, std::time_t end_time) {
    if (game_key.empty() || end_time <= start_time) return;
    
    long long duration_seconds = static_cast<long long>(end_time - start_time);

    auto stats = load_stats();
    stats[game_key].time += duration_seconds;
    if (!game_name.empty()) {
        stats[game_key].name = game_name;
    }
    save_stats(stats);

    fs::path sessions_path = get_sessions_file_path();
    std::ofstream log_file(sessions_path, std::ios::app);
    if (log_file.is_open()) {
        log_file.seekp(0, std::ios::end);
        if (log_file.tellp() == 0) {
            log_file << "# Vortex Playtime Sessions\n";
            log_file << "# Format: GAME_KEY | GAME_NAME | DURATION_SEC | START_DATE | END_DATE\n";
        }
        log_file << game_key << " | "
                 << (game_name.empty() ? "Unknown" : game_name) << " | "
                 << duration_seconds << " | "
                 << format_time(start_time) << " | "
                 << format_time(end_time) << "\n";
    }
}

std::vector<PlayStat> get_all_play_stats() {
    std::vector<PlayStat> out;
    for (const auto& [key, entry] : load_stats())
        out.push_back({ key, entry.name, entry.time });
    return out;
}

long long get_playtime(const std::string& game_key) {
    if (game_key.empty()) return 0;
    
    auto stats = load_stats();
    auto it = stats.find(game_key);
    if (it != stats.end()) {
        return it->second.time;
    }
    return 0;
}
