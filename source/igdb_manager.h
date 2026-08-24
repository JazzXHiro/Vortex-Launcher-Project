#pragma once

#include <string>
#include <vector>

struct IgdbGameInfo {
    std::string name;
    long long id = 0;
    std::string developer = "Unknown";
    double rating = 0.0;
    long long time_to_beat_seconds = 0;
    std::vector<std::string> genres;
    // IGDB themes (Fantasy, Horror, Sandbox, ...). Distinct from genres: a
    // genre is what the game IS, a theme is what it is ABOUT. The recommender
    // treats them as separate label sets and the mood weights lean heavily on
    // themes, so a game without them is only half-described.
    std::vector<std::string> themes;
    // How the game is played, as opposed to what it is (genres) or what it is
    // about (themes). Exactly six values exist in IGDB.
    std::vector<std::string> game_modes;
};

// Queries the IGDB API and returns the canonical game name and ID for the given
// folder/query string.  Falls back to the original folderName (and id 0) if the
// API returns no match, credentials are not set, or any network error occurs.
//
// steamAppId: pass the Steam application id for Steam titles. IGDB maps those
// ids directly, which sidesteps name matching entirely -- store names carry
// decoration and punctuation IGDB does not use ("《Drifting : Weight of
// Feathers》" vs "Drifting: Weight of Feathers"), so a name search can miss a
// game IGDB definitely has. Leave 0 for non-Steam games to search by name.
IgdbGameInfo igdb_resolve_game(const std::string &folderName,
                               bool interactive = true,
                               long long steamAppId = 0);

// Result of checking a credential pair against the live provider.
//
// `rejected` separates "the provider said no" from "we could not ask". They
// need different advice -- one means re-enter the key, the other means check
// the network -- and a single bool cannot carry that.
struct CredentialCheck {
    bool ok = false;
    bool rejected = false;    // reached the provider, it refused the credentials
    std::string detail;       // human-readable, safe to show; never the key itself
};

// Pull the HTTP status out of an exception message, or 0 if there is none.
//
// The two HTTP helpers in this codebase word their errors differently --
// igdb_manager throws "HTTP 400 from host/path", steamgriddb_manager throws
// "HTTP Error 401" -- and a probe that matched on one spelling silently
// misfiled every failure from the other as "could not reach the server",
// which sends the user to check their network over a wrong key.
int http_status_from_error(const std::string &message);

// Ask Twitch whether this id/secret pair is usable, without touching the
// process-wide token cache or the values in analytics/.env.
//
// Exists because a wrong key was previously accepted in silence: the wizard
// wrote it, every lookup failed for the rest of the session, and the only
// evidence was a line in a log file. Blocking, so call it off the UI thread.
CredentialCheck igdb_probe_credentials(const std::string &clientId,
                                       const std::string &clientSecret);

// Whether the last real IGDB call in this process succeeded. Drives the UI
// message: keys being PRESENT and keys WORKING are different questions, and
// the app was only ever able to answer the first.
bool igdb_last_auth_ok();
