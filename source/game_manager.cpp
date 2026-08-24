#include "game_manager.h"
#include "vortex_log.h"
#include "app_paths.h"
#include "igdb_manager.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <system_error>
#include <fstream>
#include <map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
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
        IgdbGameInfo info = igdb_resolve_game(gameFolder.filename().string());
        g.name = info.name;
        g.igdb_id = info.id;
      } else {
        // The folder name stands in until the resolve pass replaces it with
        // the canonical IGDB title.
        g.name = gameFolder.filename().string();
        g.igdb_id = 0;
      }
      g.gamePath = fs::absolute(bestPath);
      g.installDir = fs::absolute(gameFolder);
      outGames.push_back(std::move(g));
    }
  }
}

int launchGame(const fs::path &gamePath) {
#ifdef _WIN32
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

  if (!CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL,
                      workDir.c_str(), &si, &pi)) {
    DWORD err = GetLastError();
    if (err == ERROR_ELEVATION_REQUIRED) {
      SHELLEXECUTEINFOW sei = {sizeof(sei)};
      sei.fMask = SEE_MASK_NOCLOSEPROCESS;
      sei.lpVerb = L"runas";
      sei.lpFile = gamePath.c_str();
      sei.lpDirectory = workDir.c_str();
      sei.nShow = SW_SHOWNORMAL;

      if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) {
          WaitForSingleObject(sei.hProcess, INFINITE);
          DWORD exitCode = 0;
          GetExitCodeProcess(sei.hProcess, &exitCode);
          CloseHandle(sei.hProcess);
          return static_cast<int>(exitCode);
        }
        return 0;
      } else {
        std::cerr << "[ERROR] ShellExecuteEx failed. Error code: "
                  << GetLastError() << "\n";
        return -1;
      }
    }
    std::cerr << "[ERROR] CreateProcess failed. Error code: " << err << "\n";
    return -1;
  }

  // Wait until child process exits.
  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return static_cast<int>(exitCode);
#else
  std::cout << "[MOCK] Linux: Skipping actual launch for " << gamePath << "\n";
  return 0;
#endif
}

bool is_game_running_in_dir(const fs::path &installDir) {
#ifdef _WIN32
  if (installDir.empty())
    return false;

  std::wstring dirStr = installDir.wstring();
  if (dirStr.back() != L'\\' && dirStr.back() != L'/') {
    dirStr += L'\\';
  }
  std::transform(dirStr.begin(), dirStr.end(), dirStr.begin(), ::towlower);

  HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnapshot == INVALID_HANDLE_VALUE)
    return false;

  PROCESSENTRY32W pe32;
  pe32.dwSize = sizeof(PROCESSENTRY32W);

  bool found = false;

  if (Process32FirstW(hSnapshot, &pe32)) {
    do {
      HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                    pe32.th32ProcessID);
      if (hProcess) {
        wchar_t pathBuf[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProcess, 0, pathBuf, &size)) {
          std::wstring procPath = pathBuf;
          std::transform(procPath.begin(), procPath.end(), procPath.begin(),
                         ::towlower);
          if (procPath.find(dirStr) == 0) { // starts with dirStr
            found = true;
            CloseHandle(hProcess);
            break;
          }
        }
        CloseHandle(hProcess);
      }
    } while (Process32NextW(hSnapshot, &pe32));
  }

  CloseHandle(hSnapshot);
  return found;
#else
  return false;
#endif
}
