#include "game_manager.h"
#include "ea_manager.h"
#include "riot_manager.h"
#include "vortex_log.h"
#include "app_paths.h"
#include "igdb_manager.h"
#include "idle_tracker.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <system_error>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <ctime>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "psapi.lib")
#endif

using std::cerr;
using std::string;
using std::vector;

string to_lower(string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

string make_canonical(const string &s) {
  string out;
  out.reserve(s.size());
  for (unsigned char c : s)
    if (std::isalnum(c))
      out.push_back(static_cast<char>(std::tolower(c)));
  return out;
}

static bool isLaunchableFile(const fs::directory_entry &entry) {
  if (!entry.is_regular_file())
    return false;

  string ext = to_lower(entry.path().extension().string());
  return ext == ".exe";
}

static vector<string> split_tokens_alnum_lower(const string &s) {
  vector<string> out;
  string cur;
  for (unsigned char ch : s) {
    if (std::isalnum(ch)) {
      // Also split on letter<->digit boundary (e.g. "witcher3" -> ["witcher",
      // "3"])
      if (!cur.empty()) {
        const bool curIsDigit = std::isdigit((unsigned char)cur.back()) != 0;
        const bool chIsDigit = std::isdigit(ch) != 0;
        if (curIsDigit != chIsDigit) {
          out.push_back(cur);
          cur.clear();
        }
      }
      cur.push_back(static_cast<char>(std::tolower(ch)));
    } else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

static bool contains_any_substr(const string &hay,
                                const vector<string> &needles) {
  for (const auto &n : needles) {
    if (hay.find(n) != string::npos)
      return true;
  }
  return false;
}

static int token_overlap_count(const vector<string> &a,
                               const vector<string> &b) {
  int c = 0;
  for (const auto &x : a) {
    if (x.size() < 3)
      continue;
    if (std::find(b.begin(), b.end(), x) != b.end())
      ++c;
  }
  return c;
}

string local_game_title(const fs::path &installDir) {
  // EA installs often sit in a short folder ("Apex") that IGDB matches to the
  // wrong game; the manifest carries the real title.
  EaGame ea;
  if (read_ea_manifest(installDir, ea) && !ea.title.empty())
    return ea.title;
  return installDir.filename().string();
}

void scan_directory_for_games(const fs::path &gameDir,
                              vector<temp_GameEntry> &outGames,
                              bool resolve_igdb) {
  if (!fs::exists(gameDir) || !fs::is_directory(gameDir)) {
    cerr << "Directory does not exist: " << gameDir << "\n";
    return;
  }

  // --- Load Exe Cache ---
  std::map<string, string> exe_cache;
  std::ifstream cacheFile(app_data_path("exe_cache.txt"));
  if (cacheFile.is_open()) {
    string line;
    while (std::getline(cacheFile, line)) {
      auto pos = line.find('=');
      if (pos != string::npos) {
        string folder = line.substr(0, pos);
        string exe = line.substr(pos + 1);
        exe_cache[folder] = exe;
      }
    }
    cacheFile.close();
  }

  auto isBadExeName = [](const fs::path &p) -> bool {
    string stem = to_lower(p.stem().string()); // exe name without extension

    // Hard reject too-long exe names
    if (stem.size() > 40)
      return true;

    // Hard reject known non-game executables
    static const vector<string> blocked = {"unitycrashhandler", "setup",
                                           "updater",           "unins",
                                           "uninstall",         "vc_redist"};

    return contains_any_substr(stem, blocked);
  };

  // Score candidates; higher is better.
  auto score_executable = [&](const fs::path &exePath,
                              const fs::path &gameFolder) -> long long {
    const string stem = to_lower(exePath.stem().string());
    const string folderName = to_lower(gameFolder.filename().string());

    const auto exeTokens = split_tokens_alnum_lower(stem);
    const auto folderTokens = split_tokens_alnum_lower(folderName);

    long long score = 0;

    if (make_canonical(stem) == make_canonical(folderName)) {
      score += 1000000; // Overwhelming bonus ensures selection
      return score;
    }

    // 1) Prefer name similarity with folder
    const int overlap = token_overlap_count(exeTokens, folderTokens);
    score += static_cast<long long>(overlap) * 1000;

    // Big bonus if full stem appears in folder or vice versa
    if (!stem.empty() && folderName.find(stem) != string::npos)
      score += 2000;
    if (!folderName.empty() && stem.find(folderName) != string::npos)
      score += 2000;

    // 2) Penalize suspicious launcher/drm/bootstrap names
    static const vector<string> suspicious = {
        "steam",         "uplay", "ubisoft",  "launcher", "bootstrap", "eac",
        "easyanticheat", "be",    "battleye", "crash",    "helper"};
    if (contains_any_substr(stem, suspicious))
      score -= 1800;

    // 3) Mild preference for larger executable (tie-breaker, not primary)
    std::error_code ec;
    const uintmax_t sz = fs::file_size(exePath, ec);
    if (!ec) {
      // Scale size down so naming quality dominates.
      score += static_cast<long long>(sz / (1024 * 1024)); // +1 per MB
    }

    return score;
  };

  // Folders that are applications, not games.
  //
  // isBadExeName() above rejects by executable name, which cannot help here:
  // winrar.exe inside WinRAR/ is a perfectly ordinary executable and scores
  // well against its own folder. The rejection has to happen a level up.
  //
  // This matters beyond a tidy list. Everything the scan reports is written to
  // the database and feeds the taste profile, so a library of WinRAR, Krita,
  // qBittorrent and Steam does not merely look wrong -- it is what the
  // recommender learns from.
  //
  // Matching is ANCHORED, never a bare substring, and that is the whole design
  // of this list. A naive `folderName.find(needle)` looks fine until you try
  // it on a real library: "itch" is inside "The Witcher", "steam" is inside
  // "SteamWorld Dig", "origin" is inside "Dragon Age: Origins", "obs" is
  // inside "Obsidian" and "edge" is inside "Mirror's Edge". Every one of those
  // is a game the user owns and would silently lose.
  //
  // So: compare canonically (lowercase alphanumerics only, which also absorbs
  // "Master.Collection.2026" and "Riot Games") and require either an exact
  // match or a whole-name prefix. Hiding a game is worse than showing a
  // utility, so anything ambiguous is left in, and every skip is logged.
  auto isNonGameFolder = [](const fs::path &folder) -> bool {
    const string canon = make_canonical(folder.filename().string());
    if (canon.empty())
      return false;

    // Exact canonical name. The safe case: these are never game titles.
    static const vector<string> exact = {
        // Archivers, torrents, downloaders
        "winrar", "7zip", "winzip", "peazip", "qbittorrent", "utorrent",
        "bittorrent", "jdownloader", "internetdownloadmanager",
        // Storefronts and launchers -- they own games, they are not games
        "steam", "epicgames", "epicgameslauncher", "ubisoftconnect", "uplay",
        "eaapp", "eadesktop", "origin", "goggalaxy", "battlenet", "riotgames",
        "riotclient",
        "rockstargames", "rockstargameslauncher", "playnite", "heroic",
        "itch", "itchio", "xboxapp",
        // Creative and productivity tools
        "krita", "gimp", "inkscape", "blender", "audacity", "obsstudio",
        "handbrake", "notepad", "vlc", "vlcmediaplayer", "spotify",
        "libreoffice", "openoffice", "microsoftoffice",
        // Browsers and comms
        "googlechrome", "chrome", "firefox", "mozillafirefox", "opera",
        "bravebrowser", "microsoftedge", "vivaldi", "discord", "telegram",
        "whatsapp", "zoom", "skype", "microsoftteams",
        // System utilities and runtimes
        "nvidia", "nvidiacorporation", "amdsoftware", "radeonsoftware",
        "msiafterburner", "rivatunerstatisticsserver", "java", "python",
        "nodejs", "dotnet", "directx", "vcredist", "commonfiles",
        "windowskits", "windowsnt", "system32", "temp", "drivers",
    };
    for (const string &name : exact) {
      if (canon == name)
        return true;
    }

    // Whole-name prefixes, for versioned or suffixed installs such as
    // "TVPaint Animation Pro 11.08" or "Master.Collection.2026". Still
    // anchored at the start, so a game whose title merely contains one of
    // these words is unaffected.
    static const vector<string> prefixes = {
        "adobe", "autodesk", "tvpaint", "mastercollection", "photoshop",
        "illustrator", "premierepro", "aftereffects", "visualstudio",
        "jetbrains", "unityhub", "unrealengine", "androidstudio",
        "microsoftvisualc", "windowsapp",
    };
    for (const string &prefix : prefixes) {
      if (canon.size() >= prefix.size() && canon.compare(0, prefix.size(), prefix) == 0)
        return true;
    }

    return false;
  };

  // For each immediate subfolder of gameDir, pick the best scored .exe.
  for (const auto &sub : fs::directory_iterator(gameDir)) {
    if (!sub.is_directory())
      continue;

    const fs::path gameFolder = sub.path();
    const string folderAbs = fs::absolute(gameFolder).string();

    if (isNonGameFolder(gameFolder)) {
      vlog::item("Scan", gameFolder.filename().string(), vlog::Status::Skipped,
                 "looks like an application, not a game");
      continue;
    }

    // Vortex must never scan itself. On a default install the launcher sits in
    // its own folder under LocalAppData, and a user pointing the scanner at
    // that parent would otherwise get Vortex listed as one of their games.
    {
      std::error_code ec_same;
      if (fs::equivalent(gameFolder, app_data_dir(), ec_same) && !ec_same) {
        vlog::item("Scan", gameFolder.filename().string(), vlog::Status::Skipped,
                   "this is Vortex's own install folder");
        continue;
      }
    }

    fs::path bestPath;

    // --- Check Cache ---
    auto it_cache = exe_cache.find(folderAbs);
    if (it_cache != exe_cache.end()) {
        fs::path cachedExe(it_cache->second);
        std::error_code ec_exist;
        if (fs::exists(cachedExe, ec_exist) && !ec_exist) {
            bestPath = cachedExe;
        }
    }

    if (bestPath.empty()) {
        long long bestScore = std::numeric_limits<long long>::min();

        std::error_code ec;
        fs::recursive_directory_iterator it(
            gameFolder, fs::directory_options::skip_permission_denied, ec),
            end;

        for (; it != end; it.increment(ec)) {
          if (ec) {
            ec.clear();
            continue;
          }

          const auto &entry = *it;
          if (!entry.is_regular_file(ec)) {
            ec.clear();
            continue;
          }

          if (!isLaunchableFile(entry))
            continue;
          if (isBadExeName(entry.path()))
            continue;

          const long long score = score_executable(entry.path(), gameFolder);
          if (bestPath.empty() || score > bestScore) {
            bestScore = score;
            bestPath = entry.path();
          }
        }
        
        // --- Save to Cache ---
        if (!bestPath.empty()) {
            std::ofstream outFile(app_data_path("exe_cache.txt"), std::ios::app);
            if (outFile.is_open()) {
                outFile << folderAbs << "=" << fs::absolute(bestPath).string() << "\n";
            }
        }
    }

    if (!bestPath.empty()) {
      temp_GameEntry g;
      if (resolve_igdb) {
        IgdbGameInfo info = igdb_resolve_game(local_game_title(gameFolder));
        g.name = info.name;
        g.igdb_id = info.id;
      } else {
        // The folder name (an EA install's manifest title) stands in until
        // the resolve pass replaces it with the canonical IGDB title.
        g.name = local_game_title(gameFolder);
        g.igdb_id = 0;
      }
      g.gamePath = fs::absolute(bestPath);
      g.installDir = fs::absolute(gameFolder);
      outGames.push_back(std::move(g));
    }
  }
}

#ifdef _WIN32
namespace {

// One running process. `lower` is the image path lowercased once, so the
// comparisons below never have to case-fold again; `path` keeps the real
// spelling for the log. `created` is the process creation time as a FILETIME
// tick count, which is what tells a real child from a recycled parent id.
struct DirProcess {
  DWORD pid;
  DWORD parentPid;
  unsigned long long created;
  std::wstring path;
  std::wstring lower;
};

std::wstring lower_copy(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), ::towlower);
  return s;
}

// The log is UTF-8; fs::path::string() goes through the ANSI code page and
// throws on a name it cannot represent.
std::string utf8(const std::wstring &w) {
  if (w.empty())
    return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
  std::string out(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                      out.data(), len, nullptr, nullptr);
  return out;
}

std::string file_name_of(const std::wstring &path) {
  return utf8(fs::path(path).filename().wstring());
}

// installDir as a lowercase prefix ending in a separator, for matching image
// paths against. Empty when there is no folder to match.
std::wstring dir_prefix(const fs::path &installDir) {
  if (installDir.empty())
    return {};
  std::wstring dirStr = installDir.wstring();
  if (dirStr.back() != L'\\' && dirStr.back() != L'/')
    dirStr += L'\\';
  return lower_copy(dirStr);
}

bool under(const DirProcess &p, const std::wstring &prefix) {
  return !prefix.empty() && p.lower.find(prefix) == 0;
}

// Every process whose image can be read. Elevated and protected processes
// still answer PROCESS_QUERY_LIMITED_INFORMATION, so in practice this is
// every process on the machine but the system's own.
std::vector<DirProcess> all_procs() {
  std::vector<DirProcess> procs;
  HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnapshot == INVALID_HANDLE_VALUE)
    return procs;

  PROCESSENTRY32W pe32;
  pe32.dwSize = sizeof(PROCESSENTRY32W);
  if (Process32FirstW(hSnapshot, &pe32)) {
    do {
      HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                    pe32.th32ProcessID);
      if (!hProcess)
        continue;
      wchar_t pathBuf[MAX_PATH];
      DWORD size = MAX_PATH;
      if (QueryFullProcessImageNameW(hProcess, 0, pathBuf, &size)) {
        FILETIME created{}, exited{}, kernel{}, user{};
        unsigned long long createdTicks = 0;
        if (GetProcessTimes(hProcess, &created, &exited, &kernel, &user))
          createdTicks =
              (static_cast<unsigned long long>(created.dwHighDateTime) << 32) |
              created.dwLowDateTime;
        std::wstring procPath = pathBuf;
        std::wstring lower = lower_copy(procPath);
        procs.push_back({pe32.th32ProcessID, pe32.th32ParentProcessID,
                         createdTicks, std::move(procPath), std::move(lower)});
      }
      CloseHandle(hProcess);
    } while (Process32NextW(hSnapshot, &pe32));
  }
  CloseHandle(hSnapshot);
  return procs;
}

// Every process whose image lives under installDir, matched on a path prefix.
std::vector<DirProcess> procs_in_dir(const fs::path &installDir) {
  std::vector<DirProcess> procs;
  const std::wstring prefix = dir_prefix(installDir);
  if (prefix.empty())
    return procs;
  for (DirProcess &p : all_procs())
    if (under(p, prefix))
      procs.push_back(std::move(p));
  return procs;
}

// The games a running session has proven outside its install folder, keyed by
// that folder's prefix, so QUIT can reach them: quit_games_in_dir() otherwise
// only sees the folder, and Prism's Minecraft runs from AppData. The creation
// time is kept so a recycled id is never mistaken for the game.
struct TrackedProcess {
  DWORD pid;
  unsigned long long created;
};
std::mutex g_outsideMutex;
std::map<std::wstring, std::vector<TrackedProcess>> g_outsideGames;

void publish_outside_games(const std::wstring &prefix,
                           std::vector<TrackedProcess> games) {
  std::lock_guard<std::mutex> lock(g_outsideMutex);
  if (games.empty())
    g_outsideGames.erase(prefix);
  else
    g_outsideGames[prefix] = std::move(games);
}

// The published games for prefix that are still the same processes.
std::vector<DWORD> live_outside_games(const std::wstring &prefix) {
  std::vector<TrackedProcess> games;
  {
    std::lock_guard<std::mutex> lock(g_outsideMutex);
    auto it = g_outsideGames.find(prefix);
    if (it != g_outsideGames.end())
      games = it->second;
  }
  std::vector<DWORD> live;
  if (games.empty())
    return live;
  for (const DirProcess &p : all_procs())
    for (const TrackedProcess &g : games)
      if (p.pid == g.pid && p.created == g.created)
        live.push_back(p.pid);
  return live;
}

// The game libraries whose presence in a process proves it is a game. A
// launcher's browser, Explorer, an editor or a mod's own tool loads none of
// them. LWJGL is what Minecraft (and most Java games) draw with; Prism's own
// Java-version check is a javaw.exe too, but never loads it.
bool is_game_module(const std::wstring &name) {
  return name.rfind(L"lwjgl", 0) == 0 || name.rfind(L"glfw", 0) == 0 ||
         name == L"unityplayer.dll" || name == L"gameassembly.dll";
}

// The name of the first game library loaded in pid, or empty if none is (yet).
// Needs read access to the process; one that refuses it is simply not proven.
std::string loaded_game_module(DWORD pid) {
  HANDLE hProcess =
      OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!hProcess)
    return {};
  std::string found;
  std::vector<HMODULE> modules(512);
  DWORD needed = 0;
  if (EnumProcessModulesEx(hProcess, modules.data(),
                           static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                           &needed, LIST_MODULES_ALL)) {
    if (needed > modules.size() * sizeof(HMODULE)) {
      modules.resize(needed / sizeof(HMODULE));
      if (!EnumProcessModulesEx(hProcess, modules.data(),
                                static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                                &needed, LIST_MODULES_ALL))
        needed = 0;
    }
    const size_t count =
        std::min(modules.size(), static_cast<size_t>(needed / sizeof(HMODULE)));
    for (size_t i = 0; i < count && found.empty(); ++i) {
      wchar_t nameBuf[MAX_PATH];
      if (GetModuleBaseNameW(hProcess, modules[i], nameBuf, MAX_PATH)) {
        const std::wstring name = lower_copy(nameBuf);
        if (is_game_module(name))
          found = utf8(name);
      }
    }
  }
  CloseHandle(hProcess);
  return found;
}

// What a process in the game's folder is, for the purpose of counting time.
//
// Gacha games ship a launcher that must stay in charge of updates, so the user
// points Vortex at it -- and the launcher is then open for far longer than
// the game. Only Game time is played time.
enum class ProcKind { Helper, Launcher, Game };

// Support processes that run beside either the launcher or the game and say
// nothing about whether anyone is playing: crash reporters, embedded
// browsers, anti-cheat, patchers. Substrings are long enough not to occur in a
// game title; the short ones are prefixes only, so "Dispatch" is not a patcher.
bool is_helper_stem(const std::wstring &stem) {
  static const wchar_t *const contains[] = {
      L"crashhandler", L"crashreport", L"crashpad", L"errorhandler",
      L"qtwebengine",  L"cefview",     L"anticheat", L"battleye",
      L"platformprocess", L"krsdk",    L"prereq",    L"redist",
      L"unins",        L"createdump",  L"krinstall",
      // Embedded browser runtimes, which launchers bundle for their news
      // pages: msedgewebview2 (Kuro), Steam-style web helpers, CefSharp.
      L"webview",      L"webhelper",   L"cefsharp",  L"browsersubprocess",
      L"notification_helper", L"tracing_service",
  };
  static const wchar_t *const prefixes[] = {
      L"ace-", L"7z", L"updater", L"patch", L"hpatch", L"setup", L"install",
  };
  for (const wchar_t *needle : contains)
    if (stem.find(needle) != std::wstring::npos)
      return true;
  for (const wchar_t *prefix : prefixes)
    if (stem.rfind(prefix, 0) == 0)
      return true;
  return false;
}

// Whether a folder holds a game's own executable, judged by the engine files
// that sit beside one.
//
// Names cannot settle launcher-or-game: Endfield's launcher is "Games.exe".
// Files can. A game exe ships next to its engine -- UnityPlayer.dll, an Unreal
// Engine\ folder, DLSS/FSR, Steamworks -- and a launcher never does; it ships a
// UI toolkit and a downloader. The reverse test does not work: Endfield's game
// folder carries the same Qt WebEngine as its launcher. So only game files are
// looked for, and d3dcompiler/libEGL are left off because launchers bundle
// them for their embedded browsers.
bool folder_has_game_files(const fs::path &dir) {
  static const wchar_t *const files[] = {
      L"unityplayer.dll",       L"gameassembly.dll",     L"nvngx_dlss.dll",
      L"sl.interposer.dll",     L"amd_fidelityfx_dx12.dll",
      L"amd_fidelityfx_vk.dll", L"steam_api64.dll",      L"steam_api.dll",
      L"bink2w64.dll",          L"fmod.dll",             L"fmodstudio.dll",
      L"physx3_x64.dll",
  };

  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    const std::wstring name = lower_copy(entry.path().filename().wstring());
    std::error_code ec2;
    if (entry.is_directory(ec2)) {
      if (name == L"monobleedingedge" || name == L"d3d12")
        return true;
      // Unreal's root: Engine\Binaries beside the project folder.
      if (name == L"engine" && fs::is_directory(entry.path() / "Binaries", ec2))
        return true;
      // Unity: Foo_Data\ beside Foo.exe.
      const std::wstring suffix = L"_data";
      if (name.size() > suffix.size() &&
          name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        const fs::path exe =
            dir / (name.substr(0, name.size() - suffix.size()) + L".exe");
        if (fs::exists(exe, ec2))
          return true;
      }
      continue;
    }
    for (const wchar_t *file : files)
      if (name == file)
        return true;
    // Unreal's packaged game binary.
    const std::wstring shipping = L"-shipping.exe";
    if (name.size() > shipping.size() &&
        name.compare(name.size() - shipping.size(), shipping.size(), shipping) == 0)
      return true;
  }
  return false;
}

// folder_has_game_files() per folder, cached for the session: the poll asks
// about the same handful of folders every two seconds.
class GameDirs {
public:
  bool is_game_dir(const fs::path &dir) {
    const std::wstring key = lower_copy(dir.wstring());
    auto it = m_cache.find(key);
    if (it != m_cache.end())
      return it->second;
    const bool game = folder_has_game_files(dir);
    m_cache.emplace(key, game);
    return game;
  }

private:
  std::map<std::wstring, bool> m_cache;
};

// The first folder under installDir holding a game exe, or empty if none does.
// Run once per session; the walk is bounded so a huge install with no engine
// markers anywhere costs a moment, not minutes.
fs::path find_game_dir(const fs::path &installDir, GameDirs &dirs) {
  if (installDir.empty())
    return {};
  const int kMaxDepth = 6;
  const int kMaxEntries = 50000;

  std::error_code ec;
  fs::recursive_directory_iterator it(
      installDir, fs::directory_options::skip_permission_denied, ec), end;
  int visited = 0;
  for (; it != end && visited < kMaxEntries; it.increment(ec), ++visited) {
    if (ec)
      break;
    std::error_code ec2;
    if (it->is_directory(ec2)) {
      if (it.depth() >= kMaxDepth)
        it.disable_recursion_pending();
      continue;
    }
    const std::wstring name = lower_copy(it->path().filename().wstring());
    if (fs::path(name).extension() != L".exe")
      continue;
    const std::wstring stem = fs::path(name).stem().wstring();
    if (is_helper_stem(stem) || stem.find(L"launcher") != std::wstring::npos)
      continue;
    if (dirs.is_game_dir(it->path().parent_path()))
      return it->path().parent_path();
  }
  return {};
}

// launchedLower/launchedDirLower are the exe Vortex started and its folder.
//
// gameDirs is set when the install has a folder with engine files in it, and
// then it decides: only a process running from such a folder is the game.
// That is what keeps a launcher's bundled browser, downloader or tray helper
// from counting, whatever it is called.
//
// Without one, the fallback is the folder of a known launcher: Endfield's
// Games.exe shares 1.6.0\ with nothing but its own updater and browser, while
// the game lives in games\EndField Game\.
ProcKind classify(const DirProcess &p, const std::wstring &launchedLower,
                  const std::wstring &launchedDirLower, bool viaLauncher,
                  GameDirs *gameDirs) {
  const std::wstring stem = fs::path(p.lower).stem().wstring();
  if (is_helper_stem(stem))
    return ProcKind::Helper;
  if (stem.find(L"launcher") != std::wstring::npos)
    return ProcKind::Launcher;
  if (viaLauncher && p.lower == launchedLower)
    return ProcKind::Launcher;
  if (gameDirs)
    return gameDirs->is_game_dir(fs::path(p.lower).parent_path())
               ? ProcKind::Game
               : ProcKind::Launcher;
  if (viaLauncher &&
      fs::path(p.lower).parent_path().wstring() == launchedDirLower)
    return ProcKind::Launcher;
  return ProcKind::Game;
}

// launcher_exes.txt -- exes Vortex has watched start a game.
//
// "INSTALL_DIR|EXE_PATH", appended to, in the style of name_overrides.txt. A
// launcher whose name says so needs no entry; this is for the ones that do
// not, like Endfield's Games.exe, which would otherwise be re-learned (and its
// pre-game time discarded late) on every launch.
fs::path launcher_exes_path() { return app_data_path("launcher_exes.txt"); }

bool is_remembered_launcher(const fs::path &exe) {
  std::ifstream file(launcher_exes_path());
  const std::wstring target = lower_copy(exe.wstring());
  string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    const size_t pipe = line.find('|');
    if (pipe == string::npos)
      continue;
    if (lower_copy(fs::path(line.substr(pipe + 1)).wstring()) == target)
      return true;
  }
  return false;
}

void remember_launcher(const fs::path &installDir, const fs::path &exe) {
  const fs::path path = launcher_exes_path();
  const bool fresh = !fs::exists(path);
  std::ofstream out(path, std::ios::app);
  if (!out.is_open())
    return;
  if (fresh)
    out << "# Vortex Launchers\n"
           "# Format: INSTALL_DIR|EXE_PATH -- the exe starts the game, so its "
           "own time is not playtime\n";
  out << installDir.string() << "|" << exe.string() << "\n";
}

// Starts the exe with its own folder as the working directory, elevating if
// its manifest demands it. *outProcess is the process handle, or null when the
// shell elevated it without handing one back.
bool start_process(const fs::path &gamePath, HANDLE *outProcess) {
  *outProcess = nullptr;
  std::wstring command = L"\"" + gamePath.wstring() + L"\"";

  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));

  std::vector<wchar_t> cmdBuf(command.begin(), command.end());
  cmdBuf.push_back(L'\0');

  // Important: set the working directory to the game's folder
  std::wstring workDir = gamePath.parent_path().wstring();

  if (CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL,
                     workDir.c_str(), &si, &pi)) {
    CloseHandle(pi.hThread);
    *outProcess = pi.hProcess;
    return true;
  }

  DWORD err = GetLastError();
  if (err != ERROR_ELEVATION_REQUIRED) {
    std::cerr << "[ERROR] CreateProcess failed. Error code: " << err << "\n";
    return false;
  }

  SHELLEXECUTEINFOW sei = {sizeof(sei)};
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"runas";
  sei.lpFile = gamePath.c_str();
  sei.lpDirectory = workDir.c_str();
  sei.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&sei)) {
    std::cerr << "[ERROR] ShellExecuteEx failed. Error code: "
              << GetLastError() << "\n";
    return false;
  }
  *outProcess = sei.hProcess;
  return true;
}

} // namespace
#endif

LocalSession run_local_session(const fs::path &gamePath,
                               const fs::path &installDir,
                               const std::function<void()> &onStarted) {
  LocalSession session;
#ifdef _WIN32
  HANDLE process = nullptr;
  if (!start_process(gamePath, &process))
    return session;
  session.started = true;

  // Returns once the exe is pumping messages -- its window is up -- or at
  // once for a process with no GUI. The cap keeps one that never goes idle
  // from sitting on LAUNCHING forever.
  if (process)
    WaitForInputIdle(process, 10000);
  if (onStarted)
    onStarted();

  const std::wstring launchedLower = lower_copy(gamePath.wstring());
  const std::wstring launchedDirLower =
      lower_copy(gamePath.parent_path().wstring());
  std::wstring installDirLower = lower_copy(installDir.wstring());
  while (!installDirLower.empty() &&
         (installDirLower.back() == L'\\' || installDirLower.back() == L'/'))
    installDirLower.pop_back();
  const std::string launchedName = utf8(gamePath.filename().wstring());

  // A folder with engine files in it is where the game runs from; when the
  // install has one, the picked exe is a launcher exactly when it lives
  // anywhere else -- known from the first second, whether or not the game is
  // ever started this time.
  GameDirs gameDirs;
  const fs::path gameDir = find_game_dir(installDir, gameDirs);
  GameDirs *const knownGameDirs = gameDir.empty() ? nullptr : &gameDirs;

  const std::wstring launchedStem = fs::path(launchedLower).stem().wstring();
  bool viaLauncher = launchedStem.find(L"launcher") != std::wstring::npos ||
                     is_remembered_launcher(gamePath) ||
                     (knownGameDirs &&
                      !gameDirs.is_game_dir(gamePath.parent_path()));
  if (viaLauncher && knownGameDirs) {
    vlog::line("Play", launchedName +
                           " is a launcher -- the game's own files are in " +
                           utf8(gameDir.lexically_relative(installDir).wstring()) +
                           "; only time from there is counted");
  } else if (viaLauncher) {
    vlog::line("Play", launchedName +
                           " is a launcher -- only the game's own time is "
                           "counted");
  }

  // Stubs hand off before they exit, but give whatever they start a moment
  // to show up before concluding nothing is running.
  const int kStartupGraceSeconds = 10;
  const std::time_t sessionStart = std::time(nullptr);

  bool inGame = false;
  std::time_t segStart = 0;
  std::string segExe;
  std::unique_ptr<IdleTracker> idle;

  const auto openSegment = [&](std::time_t now, const std::string &exe,
                               const std::string &detail) {
    inGame = true;
    segStart = now;
    segExe = exe;
    idle = std::make_unique<IdleTracker>();
    idle->start();
    vlog::line("Play", "game started: " + detail);
  };
  // keep=false throws the stretch away: it turned out to be the launcher
  // sitting alone, which is exactly the time this exists not to count.
  const auto closeSegment = [&](std::time_t now, bool keep) {
    const long long idleSeconds = idle ? idle->stop() : 0;
    const std::string idleNote = idle ? idle->summary() : std::string();
    idle.reset();
    inGame = false;
    if (!keep)
      return;
    session.segments.push_back({segStart, now, idleSeconds, segExe});
    std::string detail = "game closed: " + segExe + " after " +
                         vlog::duration(static_cast<long long>(now - segStart));
    if (idleSeconds > 0)
      detail += " (" + vlog::duration(idleSeconds) + " idle)";
    vlog::line("Play", detail);
    if (!idleNote.empty())
      vlog::line("Play", idleNote);
  };

  // Games a launcher runs from somewhere else -- Prism starts Minecraft as a
  // javaw.exe out of AppData. Only looked for when the install has no game
  // folder of its own; WuWa and Endfield do, and anything they open outside
  // (a browser for the news page) is ignored outright.
  //
  // Two things must both hold, and neither is a guess. The process descends
  // from the launcher -- parentage, not timing, so a program the user opens
  // meanwhile is never in question -- and it is proven to be a game: engine
  // files beside it, or a game library loaded. Nothing is counted for lacking
  // a bad name; a browser, an editor or a mod's own tool is simply never
  // proven.
  const bool followOutside = knownGameDirs == nullptr;
  const std::wstring installPrefix = dir_prefix(installDir);
  const DWORD launchedPid = process ? GetProcessId(process) : 0;
  struct FamilyMember {
    unsigned long long created;
    std::string parentName;
  };
  std::map<DWORD, FamilyMember> family;
  std::map<DWORD, std::string> proven;     // pid -> what proved it
  std::map<DWORD, std::time_t> lastProbe;  // pid -> when it was last checked
  // Minecraft loads LWJGL only once its window is about to open, which on a
  // heavy modpack can be minutes in, so an unproven process keeps being
  // checked -- just not every poll, since a browser Prism opened may have
  // dozens of processes, each with hundreds of modules.
  const int kProbeSeconds = 6;

  for (;;) {
    const std::time_t now = std::time(nullptr);
    const bool handleAlive =
        process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    const std::vector<DirProcess> everything = all_procs();
    std::vector<DirProcess> procs;
    for (const DirProcess &p : everything)
      if (under(p, installPrefix))
        procs.push_back(p);

    bool launchedAlive = handleAlive;
    bool anythingUp = handleAlive;
    bool launchedIsGame = false;
    std::string otherGame;
    std::string otherDetail;
    std::vector<DWORD> launcherPids;
    const auto scan = [&]() {
      launchedIsGame = false;
      otherGame.clear();
      otherDetail.clear();
      launcherPids.clear();

      std::vector<ProcKind> kinds;
      kinds.reserve(procs.size());
      for (const DirProcess &p : procs)
        kinds.push_back(
            classify(p, launchedLower, launchedDirLower, viaLauncher,
                     knownGameDirs));

      // A launcher brings its own runtime along in folders beneath it --
      // Wuthering Waves' launcher_main.exe runs a private WebView2 out of
      // 2.6.5.0\KRWebViewRuntime\ -- and no name list keeps up with those.
      // So anything in a subfolder of a running launcher's folder is launcher
      // too. The install root is left out: WuWa's launcher.exe sits there,
      // above the game. A process in the launcher's own folder is not swept
      // up either, since a game can share a folder with an exe that merely
      // has "launcher" in its name. Not needed once the game's folder is
      // known, which already settles every process.
      std::vector<std::wstring> launcherDirs;
      for (size_t i = 0; i < procs.size() && !knownGameDirs; ++i) {
        if (kinds[i] != ProcKind::Launcher)
          continue;
        const std::wstring dir = fs::path(procs[i].lower).parent_path().wstring();
        if (dir != installDirLower)
          launcherDirs.push_back(dir + L"\\");
      }
      for (size_t i = 0; i < procs.size(); ++i) {
        if (kinds[i] != ProcKind::Game)
          continue;
        const std::wstring dir = fs::path(procs[i].lower).parent_path().wstring() + L"\\";
        for (const std::wstring &launcherDir : launcherDirs)
          if (dir != launcherDir && dir.rfind(launcherDir, 0) == 0)
            kinds[i] = ProcKind::Launcher;
      }

      for (size_t i = 0; i < procs.size(); ++i) {
        const DirProcess &p = procs[i];
        const ProcKind kind = kinds[i];
        const bool isLaunched = p.lower == launchedLower;
        if (isLaunched)
          launchedAlive = true;
        // A helper left running (an anti-cheat service, say) must not hold
        // the session open forever.
        if (kind != ProcKind::Helper)
          anythingUp = true;
        if (kind == ProcKind::Launcher)
          launcherPids.push_back(p.pid);
        if (kind != ProcKind::Game)
          continue;
        if (isLaunched)
          launchedIsGame = true;
        else if (otherGame.empty())
          otherDetail = otherGame = file_name_of(p.path);
      }
    };

    // The proven game outside the folder, if any; filled once per poll.
    std::string outsideGame;
    std::string outsideDetail;
    const auto followFamily = [&]() {
      if (!followOutside)
        return;
      std::map<DWORD, const DirProcess *> byPid;
      for (const DirProcess &p : everything)
        byPid[p.pid] = &p;

      // Forget processes that have gone, or whose id now belongs to a newer
      // process -- the creation time is what tells the two apart.
      const auto gone = [&](DWORD pid, unsigned long long created) {
        auto it = byPid.find(pid);
        return it == byPid.end() || it->second->created != created;
      };
      for (auto it = family.begin(); it != family.end();)
        it = gone(it->first, it->second.created) ? family.erase(it) : std::next(it);
      for (auto it = proven.begin(); it != proven.end();)
        it = family.count(it->first) ? std::next(it) : proven.erase(it);
      for (auto it = lastProbe.begin(); it != lastProbe.end();)
        it = byPid.count(it->first) ? std::next(it) : lastProbe.erase(it);

      // Seeds: the exe Vortex started, and every launcher process running out
      // of the install folder -- including one that was already open before
      // Play was pressed.
      for (const DirProcess &p : everything) {
        const bool seed =
            (launchedPid && p.pid == launchedPid) ||
            std::find(launcherPids.begin(), launcherPids.end(), p.pid) !=
                launcherPids.end();
        if (seed && !family.count(p.pid))
          family.emplace(p.pid, FamilyMember{p.created, {}});
      }

      // Descendants, to a fixed point so a grandchild started through a short
      // wrapper joins in the same poll. A child must be younger than its
      // parent, or the parent id has been recycled and it is a stranger.
      for (bool grew = true; grew;) {
        grew = false;
        for (const DirProcess &p : everything) {
          if (family.count(p.pid))
            continue;
          auto parent = family.find(p.parentPid);
          if (parent == family.end() || p.created <= parent->second.created)
            continue;
          auto parentProc = byPid.find(p.parentPid);
          family.emplace(
              p.pid, FamilyMember{p.created,
                                  parentProc == byPid.end()
                                      ? std::string()
                                      : file_name_of(parentProc->second->path)});
          grew = true;
        }
      }

      for (const DirProcess &p : everything) {
        if (!family.count(p.pid) || under(p, installPrefix))
          continue;
        auto proof = proven.find(p.pid);
        if (proof == proven.end()) {
          auto probed = lastProbe.find(p.pid);
          if (probed != lastProbe.end() && now - probed->second < kProbeSeconds)
            continue;
          lastProbe[p.pid] = now;

          std::string why;
          if (gameDirs.is_game_dir(fs::path(p.path).parent_path())) {
            why = "engine files beside it";
          } else {
            const std::string module = loaded_game_module(p.pid);
            if (!module.empty())
              why = "loaded " + module;
          }
          if (why.empty())
            continue;
          proof = proven.emplace(p.pid, why).first;
        }

        // Only a proven game holds the session open; an unproven process the
        // launcher left behind does not.
        anythingUp = true;
        if (outsideGame.empty()) {
          outsideGame = file_name_of(p.path);
          const std::string &by = family[p.pid].parentName;
          outsideDetail = outsideGame + " (" +
                          (by.empty() ? std::string() : "started by " + by + "; ") +
                          proof->second + ")";
        }
      }
    };

    // A game in the folder comes first; a proven one outside fills in when
    // there is none.
    const auto mergeOutside = [&]() {
      if (otherGame.empty() && !outsideGame.empty()) {
        otherGame = outsideGame;
        otherDetail = outsideDetail;
      }
    };
    scan();
    followFamily();
    mergeOutside();

    if (followOutside) {
      std::vector<TrackedProcess> outside;
      for (const auto &[pid, why] : proven)
        outside.push_back({pid, family[pid].created});
      publish_outside_games(installPrefix, std::move(outside));
    }

    // Learned: the exe the user picked is still up and something else from
    // the folder is the game, so the picked one is a launcher. A stub that
    // has already exited by the time its child appears is not caught here,
    // and should not be -- its hand-off is the game starting.
    if (!viaLauncher && launchedAlive && !otherGame.empty()) {
      viaLauncher = true;
      remember_launcher(installDir, gamePath);
      vlog::line("Play", "launcher detected: " + launchedName + " started " +
                             otherGame +
                             " -- remembered; launcher time is not counted");
      if (inGame)
        closeSegment(now, false);
      scan();
      mergeOutside();
    }

    const bool gameUp =
        viaLauncher ? !otherGame.empty() : (launchedIsGame || !otherGame.empty());

    if (gameUp && !inGame) {
      if (launchedIsGame && !viaLauncher)
        openSegment(now, launchedName, launchedName);
      else
        openSegment(now, otherGame, otherDetail);
    }
    else if (!gameUp && inGame)
      closeSegment(now, true);

    if (!anythingUp && now - sessionStart >= kStartupGraceSeconds)
      break;
    Sleep(2000);
  }

  const std::time_t sessionEnd = std::time(nullptr);
  publish_outside_games(installPrefix, {});
  if (inGame)
    closeSegment(sessionEnd, true);
  if (process)
    CloseHandle(process);

  session.viaLauncher = viaLauncher;
  if (viaLauncher) {
    long long gameSeconds = 0;
    for (const PlaySegment &seg : session.segments)
      gameSeconds += static_cast<long long>(seg.end - seg.start);
    session.launcherSeconds =
        std::max(0LL, static_cast<long long>(sessionEnd - sessionStart) -
                          gameSeconds);
    vlog::line("Play", "launcher closed -- session over");
  }
#else
  std::cout << "[MOCK] Linux: Skipping actual launch for " << gamePath << "\n";
  (void)installDir;
  (void)onStarted;
#endif
  return session;
}

#ifdef _WIN32
namespace {

// Riot ships a product's lobby inside its own folder -- League's
// LeagueClient*.exe -- and nothing about its files says it is not the game.
// It is not play, but a match is only ever started from it, so while it is up
// the session is still open.
bool is_riot_client_stem(const std::wstring &stem) {
  return stem.rfind(L"leagueclient", 0) == 0 ||
         stem.rfind(L"riotclient", 0) == 0;
}

// Waits on a game a store's own app was just asked to start, and blocks until
// it has closed. No process is handed back and the first exe the store starts
// is often an anti-cheat launcher, so the game is told apart purely by where
// it runs from.
//
// Nothing here opens a process for more than PROCESS_QUERY_LIMITED_INFORMATION
// (procs_in_dir), the access Task Manager uses, and loaded_game_module() is
// never called: a Vanguard- or EAC-protected game is only ever looked at from
// the outside.
//
// isClient, when set, names the store's own lobby processes in installDir;
// they are never game time, but hold the session open between runs.
LocalSession watch_store_session(const fs::path &installDir,
                                 const std::string &store,
                                 bool (*isClient)(const std::wstring &),
                                 const std::function<void()> &onStarted) {
  LocalSession session;
  session.started = true;

  // The store signs in and syncs before it starts anything -- 13s warm and
  // over 40s cold for Apex in the EA app, far past run_local_session's grace
  // -- so the wait for the game is as long as monitor_steam_session's.
  const int kStartupTimeoutSeconds = 120;
  // A game that closes and comes straight back, say to apply a setting, is
  // still the same session.
  const int kRestartGraceSeconds = 10;

  GameDirs gameDirs;
  GameDirs *const knownGameDirs =
      find_game_dir(installDir, gameDirs).empty() ? nullptr : &gameDirs;
  const auto poll = [&](bool &clientUp) -> std::string {
    clientUp = false;
    std::string game;
    for (const DirProcess &p : procs_in_dir(installDir)) {
      if (isClient && isClient(fs::path(p.lower).stem().wstring())) {
        clientUp = true;
        continue;
      }
      if (game.empty() &&
          classify(p, L"", L"", false, knownGameDirs) == ProcKind::Game)
        game = file_name_of(p.path);
    }
    return game;
  };

  const std::time_t launchedAt = std::time(nullptr);
  std::time_t lastSeen = 0;
  bool notified = false;
  bool inGame = false;
  std::time_t segStart = 0;
  std::string segExe;
  std::unique_ptr<IdleTracker> idle;

  for (;;) {
    const std::time_t now = std::time(nullptr);
    bool clientUp = false;
    const std::string game = poll(clientUp);
    if (!game.empty() || clientUp)
      lastSeen = now;
    if (!game.empty()) {
      if (!inGame) {
        inGame = true;
        segStart = now;
        segExe = game;
        idle = std::make_unique<IdleTracker>();
        idle->start();
        vlog::line("Play", "game started: " + game + " (after " +
                               vlog::duration(static_cast<long long>(
                                   now - launchedAt)) +
                               " in the " + store + ")");
        // QUIT only once there is a game to quit, not while the store signs
        // in.
        if (!notified && onStarted)
          onStarted();
        notified = true;
      }
    } else if (inGame) {
      const long long idleSeconds = idle->stop();
      const std::string idleNote = idle->summary();
      idle.reset();
      inGame = false;
      session.segments.push_back({segStart, now, idleSeconds, segExe});
      std::string detail = "game closed: " + segExe + " after " +
                           vlog::duration(static_cast<long long>(now - segStart));
      if (idleSeconds > 0)
        detail += " (" + vlog::duration(idleSeconds) + " idle)";
      vlog::line("Play", detail);
      vlog::line("Play", idleNote);
    }

    if (!inGame) {
      if (session.segments.empty()) {
        if (now - launchedAt >= kStartupTimeoutSeconds) {
          vlog::line("Play", "the " + store + " did not start the game within " +
                                 std::to_string(kStartupTimeoutSeconds) +
                                 "s -- nothing recorded");
          break;
        }
      } else if (now - lastSeen >= kRestartGraceSeconds) {
        break;
      }
    }
    Sleep(2000);
  }
  return session;
}

} // namespace
#endif

LocalSession run_ea_session(const std::string &contentId,
                            const fs::path &installDir,
                            const std::function<void()> &onStarted) {
  LocalSession session;
#ifdef _WIN32
  vlog::line("Play", "EA game -- launching through the EA app (offerIds=" +
                         contentId + ")");
  if (!launch_ea_game(contentId))
    return session;
  session = watch_store_session(installDir, "EA app", nullptr, onStarted);
#else
  std::cout << "[MOCK] Linux: Skipping EA launch for " << contentId << "\n";
  (void)installDir;
  (void)onStarted;
#endif
  return session;
}

LocalSession run_riot_session(const RiotGame &game,
                              const std::function<void()> &onStarted) {
  LocalSession session;
#ifdef _WIN32
  vlog::line("Play", "Riot game -- launching through the Riot Client "
                     "(--launch-product=" + game.product +
                         " --launch-patchline=" + game.patchline + ")");
  if (!launch_riot_game(game))
    return session;
  session = watch_store_session(game.installDir, "Riot Client",
                                is_riot_client_stem, onStarted);
#else
  std::cout << "[MOCK] Linux: Skipping Riot launch for " << game.product << "\n";
  (void)onStarted;
#endif
  return session;
}

bool is_game_running_in_dir(const fs::path &installDir) {
#ifdef _WIN32
  return !procs_in_dir(installDir).empty();
#else
  (void)installDir;
  return false;
#endif
}

#ifdef _WIN32
// The ids of every process whose image lives under installDir.
static std::vector<DWORD> pids_in_dir(const fs::path &installDir) {
  std::vector<DWORD> pids;
  for (const DirProcess &p : procs_in_dir(installDir))
    pids.push_back(p.pid);
  return pids;
}

static BOOL CALLBACK post_close_to_pid(HWND hwnd, LPARAM lParam) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == static_cast<DWORD>(lParam) && IsWindowVisible(hwnd))
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
  return TRUE;
}
#endif

int quit_games_in_dir(const fs::path &installDir, int graceSeconds,
                      bool allowForce) {
#ifdef _WIN32
  // Only the close request, as Alt+F4 sends it: a Vanguard-protected game
  // refuses PROCESS_TERMINATE anyway, and killing one mid-match counts as
  // leaving it. The game puts up its own prompt and the session sees it go.
  if (!allowForce) {
    const std::vector<DWORD> pids = pids_in_dir(installDir);
    for (DWORD pid : pids)
      EnumWindows(post_close_to_pid, static_cast<LPARAM>(pid));
    return static_cast<int>(pids.size());
  }

  // A game the session found outside the folder -- Minecraft, started by
  // Prism -- goes first, and on its own: closing the launcher does not close
  // it, and it gets longer to save, since a world killed mid-save is lost.
  const std::wstring prefix = dir_prefix(installDir);
  const std::vector<DWORD> outside = live_outside_games(prefix);
  if (!outside.empty()) {
    for (DWORD pid : outside)
      EnumWindows(post_close_to_pid, static_cast<LPARAM>(pid));
    const int gameGrace = std::max(graceSeconds, 30);
    for (int waited = 0; waited < gameGrace * 2; ++waited) {
      if (live_outside_games(prefix).empty())
        break;
      Sleep(500);
    }
    for (DWORD pid : live_outside_games(prefix)) {
      HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
      if (hProcess) {
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
      }
    }
  }

  const std::vector<DWORD> pids = pids_in_dir(installDir);
  for (DWORD pid : pids)
    EnumWindows(post_close_to_pid, static_cast<LPARAM>(pid));

  for (int waited = 0; waited < graceSeconds * 2; ++waited) {
    if (pids_in_dir(installDir).empty())
      return static_cast<int>(outside.size() + pids.size());
    Sleep(500);
  }

  // Still up: no window, ignored WM_CLOSE, or sitting on a confirm dialog.
  // An elevated game refuses the handle, and there is nothing more to do.
  for (DWORD pid : pids_in_dir(installDir)) {
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hProcess) {
      TerminateProcess(hProcess, 0);
      CloseHandle(hProcess);
    }
  }
  return static_cast<int>(outside.size() + pids.size());
#else
  (void)installDir;
  (void)graceSeconds;
  (void)allowForce;
  return 0;
#endif
}
