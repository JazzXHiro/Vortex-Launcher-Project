#pragma once

#include <string>

// Gets the preference score for a game: 1.0 for favorited, 0.0 otherwise.
// Negative scores are no longer written — the dislike action was removed — but
// a value <= 0 is still treated as "not favorited" so preference files written
// by older builds keep loading without a migration step.
double get_game_preference(const std::string& game_name);

// Toggles the preference score for a game. If the current score matches target_score, it resets to 0.0.
// Otherwise, it sets the score to target_score. Returns the new score.
double toggle_game_preference(const std::string& game_name, double target_score);

// Clears every stored preference, returning the number of favourites removed.
// Backs the "reset likes" action: the preference file is the recommender's
// only taste input (analytics/recommend.py::load_favorites), so emptying it is
// what actually gives the next run a clean slate.
int clear_game_preferences();
