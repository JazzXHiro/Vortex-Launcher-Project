// Persistent per-game playtime totals and a human-readable session log.

#include "stats_manager.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <ctime>

namespace fs = std::filesystem;

struct StatEntry {
    long long time = 0;
    // Idle time contained in `time`, and the part of `time` that was imported
    // rather than watched. Both are 0 for rows written before either existed.
    long long idle = 0;
    long long baseline = 0;
    std::string name;
};

// Written as the second line of playtime_stats.txt. Its absence is what marks a
// file as pre-dating the idle and baseline columns, and triggers the one-time
// migration in init_stats_manager().
static const char *kStatsFormatLine =
    "# Format: GAME_KEY=TOTAL_SECONDS|IDLE_SECONDS|BASELINE_SECONDS|GAME_NAME";

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

// Splits a stats line's value into its numeric columns and the name.
//
// The original format was TOTAL|NAME and took everything after the first pipe
// as the name, so new columns can only ever be added in front of it. Numbers
// are therefore consumed greedily from the left: a segment counts as a column
// while it is all digits AND another pipe follows it. The first segment failing
// either test is the name, which reads both the old two-field rows and the new
// four-field ones through one path, with no version flag to keep in sync.
//
// The "another pipe follows" half matters -- without it a game named "1234"
// would be swallowed as a number. The one shape this cannot resolve is a name
// that begins with digits and then a literal pipe, which no store produces.
static StatEntry parse_stat_value(const std::string& rest) {
    static const int kMaxNumericFields = 3;   // total, idle, baseline
    long long fields[kMaxNumericFields] = {0, 0, 0};
    int count = 0;
    std::size_t pos = 0;

    while (count < kMaxNumericFields) {
        const std::size_t pipe = rest.find('|', pos);
        const std::string segment = (pipe == std::string::npos)
                                        ? rest.substr(pos)
                                        : rest.substr(pos, pipe - pos);

        if (segment.empty() ||
            segment.find_first_not_of("0123456789") != std::string::npos)
            break;

        if (pipe == std::string::npos) {
            // Only a bare total may stand with no name after it; a trailing
            // number in any other position belongs to the name.
            if (count == 0) {
                fields[count++] = std::stoll(segment);
                pos = rest.size();
            }
            break;
        }

        fields[count++] = std::stoll(segment);
        pos = pipe + 1;
    }

    StatEntry entry;
    entry.time     = fields[0];
    entry.idle     = count >= 2 ? fields[1] : 0;
    entry.baseline = count >= 3 ? fields[2] : 0;
    entry.name     = rest.substr(pos);
    return entry;
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
                
                stats[key] = parse_stat_value(rest);
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
    file << kStatsFormatLine << "\n";
    for (const auto& [key, entry] : stats) {
        // Every column is written even when zero, trailing pipe included.
        // A row that stopped early would leave its last number sitting
        // exactly where the parser goes looking for the name.
        file << key << "=" << entry.time
             << "|" << entry.idle
             << "|" << entry.baseline
             << "|" << entry.name << "\n";
    }
}

void record_play_session(const std::string& game_key, const std::string& game_name,
                         std::time_t start_time, std::time_t end_time,
                         long long idle_seconds) {
    if (game_key.empty() || end_time <= start_time) return;
    
    long long duration_seconds = static_cast<long long>(end_time - start_time);

    // Idle is measured off a tick counter, which does not advance while the
    // machine is asleep, whereas the wall clock the duration comes from does.
    // Clamping keeps the two from ever disagreeing about which is larger.
    idle_seconds = std::max(0LL, std::min(idle_seconds, duration_seconds));

    auto stats = load_stats();
    stats[game_key].time += duration_seconds;
    stats[game_key].idle += idle_seconds;
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
            log_file << "# Format: GAME_KEY | GAME_NAME | DURATION_SEC | START_DATE | END_DATE | IDLE_SEC\n";
        }
        // DURATION_SEC stays the wall clock the session actually took. Idle
        // is appended rather than deducted so the log keeps saying what
        // happened, and every reader decides for itself what to do with it.
        // It goes last because sync_local_data.py addresses these fields by
        // index, so anything inserted earlier would silently shift the dates.
        log_file << game_key << " | "
                 << (game_name.empty() ? "Unknown" : game_name) << " | "
                 << duration_seconds << " | "
                 << format_time(start_time) << " | "
                 << format_time(end_time) << " | "
                 << idle_seconds << "\n";
    }
}

std::vector<PlayStat> get_all_play_stats() {
    std::vector<PlayStat> out;
    for (const auto& [key, entry] : load_stats())
        out.push_back({ key, entry.name, entry.time, entry.idle, entry.baseline });
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

PlayStat get_play_stat(const std::string& game_key) {
    PlayStat stat;
    if (game_key.empty()) return stat;

    auto stats = load_stats();
    auto it = stats.find(game_key);
    if (it == stats.end()) return stat;

    stat.key              = game_key;
    stat.name             = it->second.name;
    stat.seconds          = it->second.time;
    stat.idle_seconds     = it->second.idle;
    stat.baseline_seconds = it->second.baseline;
    return stat;
}

long long get_idle_time(const std::string& game_key) {
    if (game_key.empty()) return 0;

    auto stats = load_stats();
    auto it = stats.find(game_key);
    return it == stats.end() ? 0 : it->second.idle;
}

long long get_baseline_playtime(const std::string& game_key) {
    if (game_key.empty()) return 0;

    auto stats = load_stats();
    auto it = stats.find(game_key);
    return it == stats.end() ? 0 : it->second.baseline;
}

bool import_steam_baseline(const std::string& game_key, const std::string& game_name,
                           long long steam_seconds) {
    if (game_key.empty() || steam_seconds <= 0) return false;

    auto stats = load_stats();
    auto it = stats.find(game_key);

    if (it == stats.end()) {
        // First sighting: take Steam's lifetime figure whole. Everything after
        // this is Vortex's to measure.
        StatEntry entry;
        entry.time     = steam_seconds;
        entry.baseline = steam_seconds;
        entry.idle     = 0;   // nobody watched this history; it has no idle to claim
        entry.name     = game_name;
        stats[game_key] = entry;
        save_stats(stats);
        return true;
    }

    // A row with no baseline is one Vortex started keeping before it imported
    // anything -- either an upgrade from a build that had no baseline column, or
    // a game first seen while localconfig.vdf was unreadable. Steam's total
    // already contains every session Vortex recorded, so whatever Steam counts
    // beyond our own total is exactly the play we never saw.
    //
    // Once. Writing a non-zero baseline is what closes the window, so a game
    // cannot keep absorbing Steam's total and drifting back into following it.
    if (it->second.baseline == 0 && it->second.time < steam_seconds) {
        it->second.baseline = steam_seconds - it->second.time;
        it->second.time     = steam_seconds;
        if (!game_name.empty() && it->second.name.empty())
            it->second.name = game_name;
        save_stats(stats);
        return true;
    }

    return false;
}

// Scans settings.json for one boolean. A full JSON parser would be the third in
// this codebase and the UI already owns writing the file; all that is needed
// here is agreement with it on a single key.
//
// Anything unreadable, absent or malformed means the default, matching how
// VortexBridge::loadSettings treats the same file -- a settings file should
// never be able to change what a total means by being broken.
bool use_steam_playtime() {
    const fs::path path = g_base_dir.empty() ? fs::path("settings.json")
                                             : (g_base_dir / "settings.json");
    std::ifstream file(path);
    if (!file.is_open()) return false;

    const std::string text((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

    const std::string key = "\"useSteamPlaytime\"";
    const std::size_t at = text.find(key);
    if (at == std::string::npos) return false;

    const std::size_t colon = text.find(':', at + key.size());
    if (colon == std::string::npos) return false;

    const std::size_t value = text.find_first_not_of(" \t\r\n", colon + 1);
    return value != std::string::npos && text.compare(value, 4, "true") == 0;
}
