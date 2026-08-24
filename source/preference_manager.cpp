#include "preference_manager.h"
#include "app_paths.h"
#include <fstream>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

static const fs::path PREF_FILE_PATH = app_data_path("preferences.json");

// Very basic JSON parser/serializer since we don't have a library.
// Assumes format: {"Game Name": 1.0, ...}
//
// The reader and writer must stay symmetric. They previously were not: the
// writer escaped `"` while the reader never unescaped, and neither handled
// backslashes, so a name containing either was silently truncated or lost.
static std::unordered_map<std::string, double> load_preferences() {
    std::unordered_map<std::string, double> prefs;
    if (!fs::exists(PREF_FILE_PATH)) return prefs;

    std::ifstream file(PREF_FILE_PATH);
    std::string line;
    while (std::getline(file, line)) {
        auto quote1 = line.find('"');
        if (quote1 == std::string::npos) continue;

        // Walk to the closing quote, unescaping as we go. Scanning for a bare
        // '"' would stop early on a name containing an escaped quote.
        std::string name;
        size_t i = quote1 + 1;
        bool closed = false;
        for (; i < line.size(); ++i) {
            if (line[i] == '\\' && i + 1 < line.size()) {
                name.push_back(line[++i]);
            } else if (line[i] == '"') {
                closed = true;
                break;
            } else {
                name.push_back(line[i]);
            }
        }
        if (!closed) continue;

        auto colon = line.find(':', i + 1);
        if (colon == std::string::npos) continue;

        std::string val_str = line.substr(colon + 1);

        // Remove trailing commas and spaces
        while (!val_str.empty() && (val_str.back() == ',' || val_str.back() == ' ' || val_str.back() == '\r')) {
            val_str.pop_back();
        }

        try {
            prefs[name] = std::stod(val_str);
        } catch (...) {}
    }
    return prefs;
}

static void save_preferences(const std::unordered_map<std::string, double>& prefs) {
    std::ofstream file(PREF_FILE_PATH, std::ios::trunc);
    if (!file.is_open()) return;

    // Collect the entries we will actually write BEFORE emitting any of them.
    //
    // The previous version counted skipped zero-score entries toward the
    // trailing-comma decision. Unfavouriting leaves one 0.0 entry in the map,
    // and unordered_map iteration order is arbitrary — so whenever that entry
    // happened to come last, the final written line still got a comma and the
    // file ended `"Foo": 1,` followed by `}`. That is invalid JSON.
    //
    // The hand-rolled reader below tolerates it, so the UI looked fine, but
    // analytics/sync_local_data.py parses this file with json.load inside a
    // `except (OSError, ValueError): return {}` — so every favourite silently
    // vanished from the recommender until the next toggle happened to produce
    // a well-formed file.
    std::vector<std::pair<std::string, double>> entries;
    entries.reserve(prefs.size());
    for (const auto& [name, score] : prefs) {
        if (score == 0.0) continue;   // neutral entries are simply not stored

        std::string safe_name;
        safe_name.reserve(name.size());
        for (char c : name) {
            if (c == '"' || c == '\\') safe_name.push_back('\\');
            safe_name.push_back(c);
        }
        entries.emplace_back(std::move(safe_name), score);
    }

    file << "{\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        file << "  \"" << entries[i].first << "\": " << entries[i].second;
        if (i + 1 < entries.size()) file << ",";
        file << "\n";
    }
    file << "}\n";
}

double get_game_preference(const std::string& game_name) {
    auto prefs = load_preferences();
    auto it = prefs.find(game_name);
    if (it != prefs.end()) {
        return it->second;
    }
    return 0.0;
}

double toggle_game_preference(const std::string& game_name, double target_score) {
    auto prefs = load_preferences();
    
    double current = 0.0;
    auto it = prefs.find(game_name);
    if (it != prefs.end()) {
        current = it->second;
    }

    double new_score = target_score;
    if (current == target_score) {
        new_score = 0.0; // Toggle off
    }

    prefs[game_name] = new_score;
    save_preferences(prefs);
    return new_score;
}

int clear_game_preferences() {
    auto prefs = load_preferences();

    int cleared = 0;
    for (const auto& [name, score] : prefs) {
        if (score > 0.0) ++cleared;
    }

    // Nothing stored: leave the file alone rather than creating an empty one
    // on a first run where the user never favourited anything.
    if (prefs.empty()) return cleared;

    save_preferences({});
    return cleared;
}
