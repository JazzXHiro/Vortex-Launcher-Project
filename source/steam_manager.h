#pragma once

#include <ctime>
#include <filesystem>
#include <vector>
#include <string>

namespace fs = std::filesystem;

struct SteamGame {
	int appid = 0;
	std::string name;
	fs::path installDir;
	long long igdb_id = 0;
};

// Discover installed Steam games.
//
// resolve_igdb=false skips the per-game IGDB lookup, which is a network round
// trip each and is what makes a cold-cache scan slow. The launcher uses it to
// get titles on screen immediately and resolve afterwards; the CLI keeps the
// default so its behaviour is unchanged.
std::vector<SteamGame> read_installed_steam_games(bool resolve_igdb = true);
bool launch_steam_game_by_appid(int appid);
bool uninstall_steam_game_by_appid(int appid);
int get_steam_appid_for_install_dir(const fs::path& installDir);

// ── Steam's own playtime ─────────────────────────────────────────────────────
// Steam records lifetime playtime per app in userdata/<id>/config/localconfig.vdf.
// It counts every session, including games started outside Vortex, so it is the
// authoritative total for Steam titles — our own tracking only ever sees the
// sessions we launched.
//
// Returns 0 when Steam has no figure for the app. Results are cached briefly;
// Steam flushes the file periodically and on exit, so a total can lag until the
// client writes it out.
long long get_steam_playtime_seconds(int appid);

// When Steam last ran the app, as a Unix timestamp, or 0 if unknown.
//
// Sits beside the total because a lifetime figure alone cannot be weighted:
// the recommender decays interest by recency (RECENCY_HALF_LIFE_DAYS in
// analytics/interest.py), so 300 hours finished three years ago and 300 hours
// finished last week have to be told apart. Same cache as the total, so asking
// for both costs one read.
long long get_steam_last_played(int appid);

// Drops the cache so the next query re-reads localconfig.vdf — call after a
// session ends, once Steam has had a chance to persist the new total.
void refresh_steam_playtime();

// True while Steam reports the app as running, read from
// HKCU\Software\Valve\Steam\Apps\<appid>\Running. Steam sets this for the game
// itself, so it does not depend on where the game's processes live on disk.
bool is_steam_game_running(int appid);

// Watches one Steam session: waits for the game to actually start (Steam's
// protocol launch returns immediately and gives us no process to wait on), then
// waits for it to stop. Writes the observed boundaries to outStart/outEnd.
//
// Returns false if the game never started within the startup timeout, so a
// launch that failed or a game stuck behind a large update records nothing
// rather than a few seconds of noise.
bool monitor_steam_session(int appid, const fs::path& installDir,
                           std::time_t* outStart, std::time_t* outEnd);