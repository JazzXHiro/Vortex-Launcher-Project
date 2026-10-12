#pragma once

#include <ctime>
#include <functional>
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
// steam://install -- opens Steam's own install dialog and returns at once.
bool install_steam_game_by_appid(int appid);

// Steam's install folder (where steam.exe lives), empty when not installed.
fs::path steam_install_path();

// ── Installing into a chosen library ─────────────────────────────────────────
// One Steam library as the launcher's drive picker shows it.
struct SteamLibrary {
	fs::path root;                       // the folder that holds "steamapps"
	int folderIndex = -1;                // Steam's own index for it
	std::string drive;                   // "D:"
	std::string label;                   // volume label, "" when none
	unsigned long long freeBytes = 0;
	unsigned long long totalBytes = 0;
	bool isDefault = false;              // Steam's own folder, the one its dialog stars
};

// Every library, in Steam's folder order. Indices come from the running
// client when it answers (steam_client_internal.h), from libraryfolders.vdf
// order otherwise. Logs the list.
std::vector<SteamLibrary> steam_libraries();

struct SteamInstallRequest {
	bool started = false;      // a download is under way or queued
	bool viaClient = false;    // went straight into the chosen library, no dialog
	int  error = 0;            // Steam's EAppError when the client refused, else 0
	std::string detail;        // for the log
};

// Installs into the library with Steam's index `folderIndex` through the
// running client, with no Steam window. Falls back to steam://install, Steam's
// own dialog, when that route is unavailable or Steam refuses the request.
SteamInstallRequest install_steam_game_to(int appid, int folderIndex);

struct SteamInstallStatus {
	enum class Phase { Unknown, Queued, Paused, Downloading, Installed, NotInstalled };
	Phase phase = Phase::Unknown;
	unsigned long long done = 0;
	unsigned long long total = 0;
	unsigned int bytesPerSecond = 0;     // disk writes, not network; 0 when unknown
	// Compressed bytes fetched so far -- Steam's "Downloading data" figure. A
	// network rate is the change in this between polls. 0 when unknown.
	unsigned long long downloaded = 0;
	unsigned int secondsLeft = 0;        // 0xFFFFFFFF when unknown
	std::string source;                  // "client" or "manifest"
};

// Pauses a download, keeping what it has fetched, or resumes it from there,
// through the running client with no Steam window. Steam has no steam://
// equivalent, so there is no fallback: false means it did not happen.
// Blocking; call it off the UI thread.
bool set_steam_install_paused(int appid, bool paused);

struct SteamCancelResult {
	bool cancelled = false;              // Steam dropped it and the manifest is gone
	bool viaClient = false;              // no Steam window was involved
	int  filesRemoved = 0;               // leftovers swept after Steam was done
	unsigned long long bytesRemoved = 0;
	std::string detail;                  // for the log
};

// Cancels a download that has not finished and deletes what it left: Steam's
// UninstallApp through the running client (no window), steam://uninstall's
// dialog as the fallback, then the leftovers of that appid in its library --
// downloading\, temp\, shadercache\ and its folder under common\ unless
// another manifest uses it. Refuses a game that is fully installed. Blocking,
// up to 15 s on the client route; call it off the UI thread.
SteamCancelResult cancel_steam_install(int appid);

// Appids whose manifest exists but is not fully installed yet: first installs
// queued or downloading, including ones started in Steam itself.
std::vector<int> steam_pending_install_appids();

// Where one install stands. Live from the running client when it answers --
// its byte counters move every second -- otherwise from appmanifest_<appid>.acf,
// which Steam only rewrites every 10-30 s.
SteamInstallStatus steam_install_progress(int appid);

// ── Owned games ──────────────────────────────────────────────────────────────
// Steam keeps no complete list of what an account owns on disk, so the games
// that are not installed come from the Web API. The access token is the one the
// Steam store hands a signed-in session (see SteamLogin.qml); it stands in for
// an API key and, unlike a key, sees a private profile's library.
struct SteamOwnedGame {
	int appid = 0;
	std::string name;
	long long playtimeMinutes = 0;
	long long lastPlayed = 0;   // Unix time, 0 when never played
};

// The SteamID64 the token was issued to, read from its JWT payload ("sub").
// 0 when the token is malformed.
unsigned long long steam_id_from_webapi_token(const std::string& token);

// IPlayerService/GetOwnedGames for the token's account. Blocking; call it off
// the UI thread. On failure returns an empty list and sets `error`; an expired
// or revoked token sets `expired` as well, so the caller can ask the user to
// sign in again rather than report a network fault. The token is never logged.
std::vector<SteamOwnedGame> fetch_steam_owned_games(const std::string& token,
                                                    std::string& error,
                                                    bool& expired);
int get_steam_appid_for_install_dir(const fs::path& installDir);

// Every Steam library root (the folders holding steamapps\), from
// libraryfolders.vdf. Empty when Steam is not installed.
std::vector<fs::path> steam_library_folders();

// True while any Steam library still holds appmanifest_<appid>.acf -- the same
// file read_installed_steam_games() discovers games by, so the two agree on
// what "installed" means. steam://uninstall only opens Steam's own confirm
// dialog; the manifest goes once the player confirms and Steam removes it.
bool is_steam_app_installed(int appid);

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
//
// outIdleSeconds, when given, receives how much of the session went by with no
// input. Sampling belongs in here rather than at the call site because it must
// not begin until the game is actually up -- the wait in phase 1 can run for
// two minutes, and none of it is time the player was idle in a game.
//
// onStarted, when given, fires the moment phase 1 sees the game come up.
bool monitor_steam_session(int appid, const fs::path& installDir,
                           std::time_t* outStart, std::time_t* outEnd,
                           long long* outIdleSeconds = nullptr,
                           const std::function<void()>& onStarted = {});