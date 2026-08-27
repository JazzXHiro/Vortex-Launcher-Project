#include "metadata_manager.h"
#include "app_paths.h"
#include "game_manager.h"
#include "json_text.h"
#include <unordered_map>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <utility>

namespace fs = std::filesystem;

static const fs::path METADATA_FILE_PATH = app_data_path("game_metadata.txt");

static std::vector<std::string> split_str(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        if (!token.empty()) tokens.push_back(token);
    }
    return tokens;
}

// Splits on the delimiter while KEEPING empty fields, unlike split_str above.
//
// That difference matters: split_str drops empties, which is right for comma
// lists (where a blank is noise) and wrong for the pipe-separated record (where
// position carries the meaning). A game with no themes writes an empty slot,
// and dropping it would shift game_modes into the themes position and silently
// mislabel every later field.
static std::vector<std::string> split_fields(const std::string& s, char delimiter) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t pos = s.find(delimiter, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
}

static std::string join_str(const std::vector<std::string>& vec, char delimiter) {
    std::string res;
    for (size_t i = 0; i < vec.size(); ++i) {
        res += vec[i];
        if (i + 1 < vec.size()) res += delimiter;
    }
    return res;
}

static std::vector<std::string> derive_main_genres(const std::vector<std::string>& all_genres) {
    std::vector<std::string> priority = {"Shooter", "Adventure", "Simulator", "RPG", "Platform", "Puzzle", "Fighting", "Racing", "Visual Novel", "Indie"};
    std::vector<std::string> main_genres;

    for (const auto& genre : all_genres) {
        std::string lower_genre = to_lower(genre);
        bool is_priority = false;
        for (const auto& p : priority) {
            std::string lower_p = to_lower(p);
            if (lower_genre.find(lower_p) != std::string::npos) {
                is_priority = true;
                break;
            }
            if (p == "RPG" && lower_genre.find("role-playing") != std::string::npos) {
                is_priority = true;
                break;
            }
        }
        if (is_priority) {
            main_genres.push_back(genre);
            if (main_genres.size() == 2) break;
        }
    }

    if (main_genres.empty()) {
        for (size_t i = 0; i < std::min((size_t)2, all_genres.size()); ++i) {
            main_genres.push_back(all_genres[i]);
        }
    }

    return main_genres;
}

static std::vector<std::string> repair_all(std::vector<std::string> values, bool* repaired) {
    for (std::string& value : values) {
        std::string fixed = json_repair_dropped_escapes(value);
        if (repaired && fixed != value) *repaired = true;
        value = std::move(fixed);
    }
    return values;
}

static std::string repair_one(const std::string& value, bool* repaired) {
    std::string fixed = json_repair_dropped_escapes(value);
    if (repaired && fixed != value) *repaired = true;
    return fixed;
}

// repaired, when given, is set if any value on disk still carried a dropped
// escape -- which is what tells repair_metadata_cache_file() there is a file
// worth rewriting.
static std::unordered_map<long long, GameMetadata> load_metadata(bool* repaired = nullptr) {
    std::unordered_map<long long, GameMetadata> cache;
    if (!fs::exists(METADATA_FILE_PATH)) return cache;

    std::ifstream file(METADATA_FILE_PATH);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        try {
            long long id = std::stoll(line.substr(0, pos));
            std::string rest = line.substr(pos + 1);

            GameMetadata data;
            data.igdb_id = id;

            // Positional parse. This replaced a nest of find() calls where each
            // trailing field took "everything after the last pipe" -- every time
            // a field was appended, the previous last field silently swallowed
            // it (main_genres becoming "Racing|Action,Fantasy") and corrupted
            // data rather than failing. Indexing a split makes the record
            // extensible: an absent field is simply absent.
            //
            //   0 Developer  1 Rating  2 TimeToBeat  3 AllGenres
            //   4 MainGenres  5 Themes  6 GameModes
            //
            // Shorter lines are older records and stay valid: 5 fields predate
            // themes, 6 predate game modes.
            const std::vector<std::string> fields = split_fields(rest, '|');
            auto field = [&fields](size_t i) -> std::string {
                return i < fields.size() ? fields[i] : std::string();
            };

            if (fields.size() >= 2) {
                // Repaired on the way in, so no caller has to know that lines
                // written before json_read_string() existed hold "Beat u0027em
                // up" where IGDB sent an apostrophe. What is on disk is put
                // right separately, by repair_metadata_cache_file().
                data.developer = repair_one(field(0), repaired);
                if (!field(1).empty()) data.rating = std::stod(field(1));
                if (!field(2).empty()) data.time_to_beat_seconds = std::stoll(field(2));
                data.all_genres  = repair_all(split_str(field(3), ','), repaired);
                data.main_genres = repair_all(split_str(field(4), ','), repaired);
                data.themes      = repair_all(split_str(field(5), ','), repaired);
                data.game_modes  = repair_all(split_str(field(6), ','), repaired);
            }
            cache[id] = data;
        } catch (...) {
        }
    }
    return cache;
}

static void save_metadata_cache(const std::unordered_map<long long, GameMetadata>& cache) {
    std::ofstream file(METADATA_FILE_PATH, std::ios::trunc);
    if (!file.is_open()) return;

    file << "# Vortex Game Metadata\n";
    file << "# Format: IGDB_ID=Developer|Rating|Time_To_Beat_Seconds|All_Genres|Main_Genres|Themes|Game_Modes\n";
    for (const auto& [id, data] : cache) {
        file << id << "=" << data.developer << "|" << data.rating << "|" << data.time_to_beat_seconds
             << "|" << join_str(data.all_genres, ',') << "|" << join_str(data.main_genres, ',')
             << "|" << join_str(data.themes, ',') << "|" << join_str(data.game_modes, ',') << "\n";
    }
}

void save_game_metadata(const GameMetadata& data) {
    if (data.igdb_id <= 0) return;
    auto cache = load_metadata();
    
    GameMetadata copy = data;
    if (copy.main_genres.empty() && !copy.all_genres.empty()) {
        copy.main_genres = derive_main_genres(copy.all_genres);
    }
    
    cache[copy.igdb_id] = copy;
    save_metadata_cache(cache);
}

bool repair_metadata_cache_file() {
    bool repaired = false;
    auto cache = load_metadata(&repaired);
    // Rewriting is not only cosmetic: analytics/sync_local_data.py reads this
    // file straight off disk, and "Beat u0027em up" is a different label to it
    // than "Beat 'em up", so the recommender was grouping the same genre under
    // two names. One rewrite fixes every line, since the whole cache is dumped.
    if (repaired) save_metadata_cache(cache);
    return repaired;
}

GameMetadata get_game_metadata(long long igdb_id) {
    GameMetadata fallback;
    fallback.igdb_id = igdb_id;
    if (igdb_id <= 0) return fallback;

    auto cache = load_metadata();
    auto it = cache.find(igdb_id);
    if (it != cache.end()) {
        return it->second;
    }
    return fallback;
}
