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

// Deletes the artwork folders of `game_names`, skipping any name that also
// appears in `keep_names` (compared case-insensitively). Artwork is keyed by
// game name, not by install location, so a game still reachable from another
// folder — or from Steam — must keep its images. Returns the number of folders
// actually deleted.
int delete_steamgriddb_images(const std::vector<std::string>& game_names,
                              const std::vector<std::string>& keep_names,
                              const std::string& images_root);
