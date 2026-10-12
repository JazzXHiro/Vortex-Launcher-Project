#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ── Installing through the running Steam client ─────────────────────────────
// steam://install opens Steam's own dialog: it takes focus, asks for a library
// and shows the licence. Steam has no public call that installs into a chosen
// library instead, but steamclient64.dll -- the library every Steam game loads
// to talk to the client -- exposes IClientAppManager over the same local pipe,
// and its InstallApp takes a library folder index. That is what this wraps.
//
// It is undocumented. Method positions come from OpenSteamworks
// (github.com/OpenSteamClient/OpenSteamworks) and were checked against the
// client of 2026-09-03; Valve inserts methods with client updates, so every
// connection is validated before anything is called (see connect_locked() in
// the .cpp), and anything unexpected leaves the module unavailable for the
// session. Callers then fall back to steam://install.
//
// Only methods the interface map marks cannotcallincrossprocess=0 are ever
// called. One that is not -- BGetLibraryFolderInfo -- sends its out-pointers
// across the pipe as plain integers, and Steam crashed dereferencing them.
//
// All calls run on one worker thread that owns the DLL and the pipe, each with
// a timeout: a wedged Steam stalls the caller for at most a few seconds once,
// after which the module reports itself unavailable instead of blocking. The
// pipe is released after a quiet spell, so an idle launcher does not hold one.

struct SteamInternalFolder {
    int      index = -1;   // Steam's LibraryFolder_t
    fs::path path;
};

struct SteamInternalProgress {
    bool          ok = false;          // the call went through at all
    std::uint32_t state = 0;           // EAppState bits (GetAppInstallState)
    int           folder = -1;         // library index the app lives in, -1 none
    // From GetUpdateInfo. Zero while queued; done/total only once Steam has
    // worked out the download, and only if the reading passed its checks.
    bool          haveBytes = false;
    std::uint64_t done = 0;            // staged: written to disk, uncompressed
    std::uint64_t total = 0;           // BytesToStage
    // Fetched from the network, compressed -- what Steam's "Downloading data"
    // line shows. Only when the reading passed its checks.
    bool          haveDownload = false;
    std::uint64_t downloadDone = 0;
    std::uint64_t downloadTotal = 0;   // BytesToDownload
    // Steam's own smoothed rate, but of disk writes (its DISK USAGE figure),
    // not of the network: a game that compresses 3:1 reads 3x too fast here.
    std::uint32_t bytesPerSecond = 0;
    std::uint32_t secondsLeft = 0;     // 0xFFFFFFFF when Steam does not know
};

// EAppState bits used by callers.
constexpr std::uint32_t kSteamAppUninstalled    = 1u;
constexpr std::uint32_t kSteamAppUpdateRequired = 2u;
constexpr std::uint32_t kSteamAppFullyInstalled = 4u;
constexpr std::uint32_t kSteamAppUpdatePaused   = 512u;
// Set when Steam logs "update started": the download and staging proper. Before
// it, while Steam reconfigures and preallocates, GetUpdateInfo's counters hold
// the preallocation instead -- 25 GB of 26 GB four seconds in.
constexpr std::uint32_t kSteamAppUpdateStarted  = 1024u;

// Connects if needed and reports whether installs can go through this route.
// Blocking (worst case a few seconds); call it off the UI thread.
bool steam_internal_available();

// Why the last connect failed, for the log and the fallback message.
std::string steam_internal_unavailable_reason();

// Steam's own library list, in its folder index order. Empty if unavailable.
std::vector<SteamInternalFolder> steam_internal_library_folders();

// InstallApp into `folderIndex`, then moves it to the front of the download
// queue as user-initiated -- InstallApp alone leaves it queued and paused.
// Returns Steam's EAppError (0 = started), or -1 if the route is unavailable.
int steam_internal_install(int appid, int folderIndex);

// Pauses one app's download, keeping what it has fetched, or resumes it from
// there -- Steam's own PauseAppUpdate / ResumeAppUpdate. No window. False if
// Steam refused or the route is unavailable.
bool steam_internal_set_paused(int appid, bool paused);

// UninstallApp: on an app still downloading, cancels the download and has
// Steam delete its manifest and the downloading/temp folders -- with no
// window, unlike steam://uninstall. Steam leaves the empty install folder
// under common\ behind; cancel_steam_install() sweeps it. Returns Steam's
// EAppError (0 = done), or -1 if the route is unavailable.
int steam_internal_uninstall(int appid);

// Install state and byte progress for one app.
SteamInternalProgress steam_internal_progress(int appid);

// EAppError value as its name, for the log.
const char *steam_app_error_name(int error);

// Releases the pipe and stops the worker. Safe to call more than once.
void steam_internal_shutdown();
