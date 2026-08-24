#pragma once
#include <string>
#include <vector>

struct GameMetadata {
    long long igdb_id = 0;
    std::string developer = "Unknown";
    double rating = 0.0;
    long long time_to_beat_seconds = 0;
    std::vector<std::string> all_genres;
    std::vector<std::string> main_genres;
    // Sixth field in game_metadata.txt, appended after main_genres. Lines
    // written before this existed have only five fields and still load.
    std::vector<std::string> themes;
    // Seventh field: how the game is played (Single player, Multiplayer,
    // Co-operative, Split screen, MMO, Battle Royale). Six values in total.
    std::vector<std::string> game_modes;
};

void save_game_metadata(const GameMetadata& data);
GameMetadata get_game_metadata(long long igdb_id);
