#pragma once

#include <functional>

#include "igdb_manager.h"   // CredentialCheck
#include <string>
#include <vector>

// Called as the artwork pass advances: (games finished, total, name just
// started). Invoked on the calling thread, so a UI caller must marshal.
//
// It exists because this pass is the long pole of a cold-cache scan -- one
// search plus up to three downloads per game -- and without a per-game signal
// the launcher could only show an indeterminate spinner for the whole thing.
using SgdbProgressFn = std::function<void(int done, int total, const std::string &name)>;

// Ensures that SteamGridDB images (grid, logo, hero) exist for each game in the list.
// images_root is the absolute path string to the top-level "Images" directory.
// If an image folder/file already exists for a game, it skips downloading for that game.
//
// on_progress is optional; omitting it keeps the original blocking behaviour,
// which is what VortexCLI wants.
void ensure_steamgriddb_images(const std::vector<std::string>& game_names,
                               const std::string& images_root,
                               const SgdbProgressFn& on_progress = nullptr);

// Steam's own library_hero.jpg for Steam games whose hero folder is still
// empty after ensure_steamgriddb_images() -- SteamGridDB has no hero for some
// games Steam itself does. Needs no API key. Run it after the SteamGridDB pass
// so a SteamGridDB hero, when there is one, keeps priority. A 404 is stamped
// in the same state file and not retried for a week. Blocking.
struct SteamHeroRequest {
  std::string name;
  int app_id = 0;
};
void ensure_steam_hero_fallback(const std::vector<SteamHeroRequest>& games,
                                const std::string& images_root);

// Folder name (without the images_root prefix) that holds a game's artwork:
// "<sanitized game name>_img". Callers that need to find or delete artwork
// should use this rather than re-deriving the name.
std::string steamgriddb_image_folder_name(const std::string& game_name);

// Ask SteamGridDB whether this key is usable. Mirrors igdb_probe_credentials()
// -- see CredentialCheck in igdb_manager.h for why "refused" and "unreachable"
// are kept apart. Blocking; call it off the UI thread.
CredentialCheck steamgriddb_probe_key(const std::string& api_key);

// Whether the last real artwork request in this process authenticated.
// Artwork silently stopping is otherwise indistinguishable from a library of
// games that happen to have no art.
bool steamgriddb_last_auth_ok();

// SteamGridDB's own CDN URLs for one game's grid, hero and logo, without
// downloading any of them -- for games that are shown but never owned
// (wishlist, favourites, played history), whose art loads live by URL.
// Empty fields mean no art of that kind. `reachable` is false when any request
// failed for a reason other than SteamGridDB answering "not found", so the
// caller knows the empty fields are not a definitive answer. No API key counts
// as reachable with nothing found. Blocking; call it off the UI thread.
struct SgdbArtUrls {
  std::string grid, hero, logo;
  bool reachable = true;
};
SgdbArtUrls steamgriddb_art_urls(const std::string& game_name);

// Deletes the artwork folders of `game_names`, skipping any name that also
// appears in `keep_names` (compared case-insensitively). Artwork is keyed by
// game name, not by install location, so a game still reachable from another
// folder — or from Steam — must keep its images. Returns the number of folders
// actually deleted.
int delete_steamgriddb_images(const std::vector<std::string>& game_names,
                              const std::vector<std::string>& keep_names,
                              const std::string& images_root);
