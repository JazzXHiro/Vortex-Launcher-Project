#include "steam_manager.h"

#include "idle_tracker.h"
#include "vortex_log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <thread>
#include <iostream>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <string>

#include "game_manager.h"
#include "igdb_manager.h"
#include "json_text.h"
#include "steam_client_internal.h"
#include "steamgriddb_manager.h"   // https_get

using std::string;
using std::vector;
using std::wstring;

static string trim(const string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
    ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    --b;
  return s.substr(a, b - a);
}

static string read_text_file(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  if (!in)
    return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static bool get_steam_path_from_registry(fs::path &outSteamPath) {
#ifdef _WIN32
  HKEY hKey = nullptr;
  const char *subKey = "Software\\Valve\\Steam";
  if (RegOpenKeyExA(HKEY_CURRENT_USER, subKey, 0, KEY_READ, &hKey) !=
      ERROR_SUCCESS) {
    return false;
  }

  char buffer[4096];
  DWORD bufferSize = sizeof(buffer);
  DWORD type = 0;
  LONG rc = RegQueryValueExA(hKey, "SteamPath", nullptr, &type,
                             reinterpret_cast<LPBYTE>(buffer), &bufferSize);
  RegCloseKey(hKey);

  if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
      bufferSize == 0) {
    return false;
  }

  if (bufferSize >= sizeof(buffer))
    buffer[sizeof(buffer) - 1] = '\0';
  else
    buffer[bufferSize] = '\0';

  outSteamPath = fs::path(buffer);
  return !outSteamPath.empty();
#else
  return false;
#endif
}

static vector<fs::path> parse_libraryfolders_vdf(const fs::path &steamPath) {
  vector<fs::path> libs;

  fs::path vdfPath = steamPath / "steamapps" / "libraryfolders.vdf";
  string txt = read_text_file(vdfPath);
  if (txt.empty())
    return libs;

  std::regex pathRe("\"path\"\\s*\"([^\"]+)\"");
  auto begin = std::sregex_iterator(txt.begin(), txt.end(), pathRe);
  auto end = std::sregex_iterator();

  std::unordered_set<string> seen;
  for (auto it = begin; it != end; ++it) {
    string raw = (*it)[1].str();

    string unescaped;
    unescaped.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') {
        unescaped.push_back('\\');
        ++i;
      } else {
        unescaped.push_back(raw[i]);
      }
    }

    fs::path p = fs::path(unescaped);
    string key = p.string();
    if (!key.empty() && !seen.count(key)) {
      seen.insert(key);
      libs.push_back(p);
    }
  }

  string steamKey = steamPath.string();
  if (!steamKey.empty() && !seen.count(steamKey)) {
    libs.push_back(steamPath);
  }

  return libs;
}

vector<fs::path> steam_library_folders() {
  fs::path steamPath;
  if (!get_steam_path_from_registry(steamPath))
    return {};
  return parse_libraryfolders_vdf(steamPath);
}

// ── Minimal VDF reader ───────────────────────────────────────────────────────
// localconfig.vdf cannot be scraped with a regex the way libraryfolders.vdf is:
// the same app id appears in several unrelated sections, so "Playtime" has to be
// read from the specific apps block. This walks the structure instead.
namespace {

struct VdfToken {
    enum Type { String, Open, Close, End };
    Type   type = End;
    string text;
};

class VdfLexer {
public:
    explicit VdfLexer(const string &text) : m_text(text) {}

    VdfToken next() {
        skipTrivia();
        if (m_pos >= m_text.size()) return { VdfToken::End, {} };

        const char c = m_text[m_pos];
        if (c == '{') { ++m_pos; return { VdfToken::Open,  {} }; }
        if (c == '}') { ++m_pos; return { VdfToken::Close, {} }; }
        if (c == '"') return { VdfToken::String, readQuoted() };

        const size_t start = m_pos;
        while (m_pos < m_text.size() && !std::isspace(static_cast<unsigned char>(m_text[m_pos])) &&
               m_text[m_pos] != '{' && m_text[m_pos] != '}')
            ++m_pos;
        return { VdfToken::String, m_text.substr(start, m_pos - start) };
    }

private:
    void skipTrivia() {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (std::isspace(static_cast<unsigned char>(c))) { ++m_pos; continue; }
            if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/') {
                while (m_pos < m_text.size() && m_text[m_pos] != '\n') ++m_pos;
                continue;
            }
            break;
        }
    }

    string readQuoted() {
        ++m_pos;   // opening quote
        string out;
        while (m_pos < m_text.size() && m_text[m_pos] != '"') {
            if (m_text[m_pos] == '\\' && m_pos + 1 < m_text.size()) {
                ++m_pos;
                const char escaped = m_text[m_pos];
                out.push_back(escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped);
            } else {
                out.push_back(m_text[m_pos]);
            }
            ++m_pos;
        }
        if (m_pos < m_text.size()) ++m_pos;   // closing quote
        return out;
    }

    const string &m_text;
    size_t        m_pos = 0;
};

bool iequals_ascii(const string &a, const string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

long long to_long_long(const string &value) {
    try {
        return std::stoll(value);
    } catch (...) {
        return 0;
    }
}

// Consumes tokens until the block opened by the caller is closed.
void skip_vdf_block(VdfLexer &lexer) {
    int depth = 1;
    while (depth > 0) {
        const VdfToken token = lexer.next();
        if (token.type == VdfToken::End)   return;
        if (token.type == VdfToken::Open)  ++depth;
        if (token.type == VdfToken::Close) --depth;
    }
}

// Leaves the lexer positioned just inside the block named by keyPath.
bool enter_vdf_path(VdfLexer &lexer, const vector<string> &keyPath) {
    for (const string &wanted : keyPath) {
        bool entered = false;
        while (!entered) {
            const VdfToken key = lexer.next();
            if (key.type == VdfToken::Close || key.type == VdfToken::End) return false;
            if (key.type != VdfToken::String) continue;

            const VdfToken value = lexer.next();
            if (value.type == VdfToken::Open) {
                if (iequals_ascii(key.text, wanted)) entered = true;
                else skip_vdf_block(lexer);
            } else if (value.type != VdfToken::String) {
                return false;   // malformed
            }
        }
    }
    return true;
}

}  // namespace

struct SteamAppPlaytime {
    long long minutes    = 0;
    long long lastPlayed = 0;
};

// Reads one app entry, ignoring nested blocks such as "cloud" / "autocloud".
static SteamAppPlaytime parse_steam_app_entry(VdfLexer &lexer) {
    SteamAppPlaytime entry;
    while (true) {
        const VdfToken key = lexer.next();
        if (key.type == VdfToken::Close || key.type == VdfToken::End) return entry;
        if (key.type != VdfToken::String) continue;

        const VdfToken value = lexer.next();
        if (value.type == VdfToken::Open) { skip_vdf_block(lexer); continue; }
        if (value.type != VdfToken::String) return entry;

        if (iequals_ascii(key.text, "Playtime"))        entry.minutes    = to_long_long(value.text);
        else if (iequals_ascii(key.text, "LastPlayed")) entry.lastPlayed = to_long_long(value.text);
    }
}

static std::unordered_map<int, SteamAppPlaytime> parse_localconfig(const fs::path &path) {
    std::unordered_map<int, SteamAppPlaytime> playtimes;

    const string text = read_text_file(path);
    if (text.empty()) return playtimes;

    VdfLexer lexer(text);
    if (!enter_vdf_path(lexer, { "UserLocalConfigStore", "Software", "Valve", "Steam", "apps" }))
        return playtimes;

    while (true) {
        const VdfToken key = lexer.next();
        if (key.type == VdfToken::Close || key.type == VdfToken::End) break;
        if (key.type != VdfToken::String) continue;

        const VdfToken value = lexer.next();
        if (value.type != VdfToken::Open) continue;   // "appid" "scalar" — not an entry

        const SteamAppPlaytime entry = parse_steam_app_entry(lexer);
        const int appid = static_cast<int>(to_long_long(key.text));
        if (appid <= 0 || entry.minutes <= 0) continue;

        // Several Steam accounts may share this machine; keep the largest total
        // rather than summing unrelated users together.
        auto existing = playtimes.find(appid);
        if (existing == playtimes.end() || entry.minutes > existing->second.minutes)
            playtimes[appid] = entry;
    }

    return playtimes;
}

static std::unordered_map<int, SteamAppPlaytime> read_all_steam_playtime() {
    std::unordered_map<int, SteamAppPlaytime> playtimes;

    fs::path steamPath;
    if (!get_steam_path_from_registry(steamPath)) return playtimes;

    const fs::path userdata = steamPath / "userdata";
    std::error_code ec;
    if (!fs::exists(userdata, ec) || ec) return playtimes;

    for (const auto &account : fs::directory_iterator(userdata, ec)) {
        if (ec) break;
        if (!account.is_directory()) continue;

        const fs::path config = account.path() / "config" / "localconfig.vdf";
        if (!fs::exists(config, ec)) continue;

        for (const auto &[appid, entry] : parse_localconfig(config)) {
            auto existing = playtimes.find(appid);
            if (existing == playtimes.end() || entry.minutes > existing->second.minutes)
                playtimes[appid] = entry;
        }
    }

    return playtimes;
}

// localconfig.vdf is ~100 KB and queried once per game per list render, so the
// parse is cached. The window is short enough that a total refreshes shortly
// after Steam writes it.
static std::unordered_map<int, SteamAppPlaytime> s_playtimeCache;
static std::time_t s_playtimeCacheTime = 0;
static bool        s_playtimeCacheValid = false;
static const int   kPlaytimeCacheSeconds = 30;

void refresh_steam_playtime() {
    s_playtimeCacheValid = false;
}

static SteamAppPlaytime cached_steam_playtime(int appid) {
    const std::time_t now = std::time(nullptr);
    if (!s_playtimeCacheValid || now - s_playtimeCacheTime > kPlaytimeCacheSeconds) {
        s_playtimeCache      = read_all_steam_playtime();
        s_playtimeCacheTime  = now;
        s_playtimeCacheValid = true;
    }

    auto it = s_playtimeCache.find(appid);
    return it == s_playtimeCache.end() ? SteamAppPlaytime{} : it->second;
}

long long get_steam_playtime_seconds(int appid) {
    if (appid <= 0) return 0;
    return cached_steam_playtime(appid).minutes * 60;
}

long long get_steam_last_played(int appid) {
    if (appid <= 0) return 0;
    return cached_steam_playtime(appid).lastPlayed;
}

bool is_steam_game_running(int appid) {
#ifdef _WIN32
    if (appid <= 0) return false;

    const string subKey = "Software\\Valve\\Steam\\Apps\\" + std::to_string(appid);
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, subKey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    DWORD type = 0;
    BYTE  buffer[64] = {};
    DWORD size = sizeof(buffer);
    const LONG rc = RegQueryValueExA(hKey, "Running", nullptr, &type, buffer, &size);
    RegCloseKey(hKey);

    if (rc != ERROR_SUCCESS) return false;
    if (type == REG_DWORD && size >= sizeof(DWORD))
        return *reinterpret_cast<DWORD *>(buffer) != 0;
    if (type == REG_SZ || type == REG_EXPAND_SZ)
        return size > 0 && buffer[0] == '1';
    return false;
#else
    (void)appid;
    return false;
#endif
}

// A game counts as active if Steam says so, or if any process is running out of
// its install folder. The two cover each other's blind spots: Steam's flag misses
// nothing when a game relaunches into a different directory, and the directory
// check catches titles Steam stops reporting while they are still up.
static bool steam_app_active(int appid, const fs::path &installDir) {
    return is_steam_game_running(appid) || is_game_running_in_dir(installDir);
}

bool monitor_steam_session(int appid, const fs::path &installDir,
                           std::time_t *outStart, std::time_t *outEnd,
                           long long *outIdleSeconds,
                           const std::function<void()> &onStarted) {
    const int  kStartupTimeoutSeconds = 180;
    const auto kPollInterval          = std::chrono::seconds(2);

    // Phase 1 — wait for the game to come up. The clock only starts once it has,
    // so Steam's own startup time is not counted as playtime.
    const std::time_t waitBegan = std::time(nullptr);
    while (!steam_app_active(appid, installDir)) {
        if (std::time(nullptr) - waitBegan > kStartupTimeoutSeconds) {
            // Returning false skips record_play_session AND the analytics sync
            // entirely, so the session is lost and recommendations cannot
            // change. That used to happen in complete silence; a slow first
            // launch (shader precompile, a pending update) looks identical to
            // never having played.
            std::cerr << "[Steam] Gave up waiting for appid " << appid
                      << " to start after " << kStartupTimeoutSeconds
                      << "s - this play session will not be recorded.\n";
            return false;
        }
        std::this_thread::sleep_for(kPollInterval);
    }

    const std::time_t start = std::time(nullptr);
    if (onStarted)
        onStarted();

    // Phase 2 — wait for it to go away, sampling input the whole time. The
    // tracker runs its own thread rather than riding this loop so that the
    // local-game path, which blocks inside WaitForSingleObject and has no loop
    // at all, can measure idle exactly the same way.
    IdleTracker idle;
    idle.start();

    while (steam_app_active(appid, installDir))
        std::this_thread::sleep_for(kPollInterval);

    const std::time_t end = std::time(nullptr);
    const long long idleSeconds = idle.stop();
    vlog::line("Play", idle.summary());

    if (outStart) *outStart = start;
    if (outEnd)   *outEnd   = end;
    if (outIdleSeconds) *outIdleSeconds = idleSeconds;
    return end > start;
}

static bool extract_acf_field(const string &acf, const string &key,
                              string &outValue) {
  std::regex re("\"" + key + "\"\\s*\"([^\"]*)\"");
  std::smatch m;
  if (std::regex_search(acf, m, re) && m.size() >= 2) {
    outValue = m[1].str();
    return true;
  }
  return false;
}

// Proton, the Steam Linux Runtime, Steamworks redistributables: installed and
// owned like games, but nothing anyone would launch from a game library.
static bool is_steam_tool_name(const string &name) {
  const string lower_name = to_lower(name);
  return lower_name.find("proton") != string::npos ||
         lower_name.find("runtime") != string::npos ||
         lower_name.find("steamworks") != string::npos;
}

static bool parse_appmanifest(const fs::path &manifestPath,
                              SteamGame &outGame) {
  string acf = read_text_file(manifestPath);
  if (acf.empty())
    return false;

  string appidStr, name;
  if (!extract_acf_field(acf, "appid", appidStr))
    return false;
  if (!extract_acf_field(acf, "name", name))
    name.clear();

  try {
    outGame.appid = std::stoi(trim(appidStr));
  } catch (...) {
    return false;
  }

  if (is_steam_tool_name(name))
      return false;

  outGame.name = name;
  return true;
}

vector<SteamGame> read_installed_steam_games(bool resolve_igdb) {
  vector<SteamGame> games;

  fs::path steamPath;
  if (!get_steam_path_from_registry(steamPath)) {
    std::cerr << "Could not find SteamPath in registry.\n";
    return games;
  }

  vector<fs::path> libraries = parse_libraryfolders_vdf(steamPath);
  if (libraries.empty()) {
    std::cerr << "No Steam libraries found.\n";
    return games;
  }

  std::unordered_set<int> seenAppIds;

  for (const auto &lib : libraries) {
    fs::path steamapps = lib / "steamapps";
    if (!fs::exists(steamapps) || !fs::is_directory(steamapps))
      continue;

    for (const auto &e : fs::directory_iterator(steamapps)) {
      if (!e.is_regular_file())
        continue;

      const fs::path p = e.path();
      const string fn = p.filename().string();

      if (fn.rfind("appmanifest_", 0) != 0)
        continue;
      if (p.extension() != ".acf")
        continue;

      SteamGame g;
      if (!parse_appmanifest(p, g))
        continue;

      if (seenAppIds.count(g.appid))
        continue;
      seenAppIds.insert(g.appid);

      string acf = read_text_file(p);

      // A first install writes its manifest the moment it is queued, long
      // before there is anything to run. Until Steam sets FullyInstalled (4)
      // it stays an owned, not-installed game, so its card keeps showing the
      // download rather than turning into a PLAY button mid-way. An update to
      // an installed game keeps bit 4 and is unaffected; a manifest with no
      // StateFlags at all is taken as installed, as before.
      string stateFlags;
      if (extract_acf_field(acf, "StateFlags", stateFlags) &&
          (to_long_long(trim(stateFlags)) & 4) == 0) {
        seenAppIds.erase(g.appid);
        vlog::line("SteamInstall", "appid " + std::to_string(g.appid) + " (" + g.name +
                                       ") is still installing (StateFlags " + stateFlags +
                                       ") -- not listed as installed yet");
        continue;
      }

      string installdir;
      if (extract_acf_field(acf, "installdir", installdir) &&
          !installdir.empty()) {
        g.installDir = lib / "steamapps" / "common" / installdir;
      }

      // Resolve through the appid: Steam store names carry brackets and
      // spacing IGDB does not use, which makes name search miss games IGDB has.
      //
      // Skipped when the caller wants titles first: g.name is already the
      // store name, so the only thing missing until the resolve pass runs is
      // igdb_id, and everything keyed off it degrades to "Unknown" rather
      // than being wrong.
      if (resolve_igdb) {
        IgdbGameInfo info = igdb_resolve_game(g.name, false, g.appid);
        g.igdb_id = info.id;
      }

      games.push_back(std::move(g));
    }
  }

  std::sort(games.begin(), games.end(),
            [](const SteamGame &a, const SteamGame &b) {
              if (a.name == b.name)
                return a.appid < b.appid;
              return a.name < b.name;
            });

  return games;
}

bool launch_steam_game_by_appid(int appid) {
#ifdef _WIN32
  string uri = "steam://run/" + std::to_string(appid);
  HINSTANCE res = ShellExecuteA(nullptr, "open", uri.c_str(), nullptr, nullptr,
                                SW_SHOWNORMAL);
  return reinterpret_cast<intptr_t>(res) > 32;
#else
  std::cout << "[MOCK] Linux: Skipping Steam launch for AppID " << appid
            << "\n";
  return true;
#endif
}

bool uninstall_steam_game_by_appid(int appid) {
#ifdef _WIN32
  string uri = "steam://uninstall/" + std::to_string(appid);
  HINSTANCE res = ShellExecuteA(nullptr, "open", uri.c_str(), nullptr, nullptr,
                                SW_SHOWNORMAL);
  return reinterpret_cast<intptr_t>(res) > 32;
#else
  return true;
#endif
}

bool install_steam_game_by_appid(int appid) {
#ifdef _WIN32
  string uri = "steam://install/" + std::to_string(appid);
  HINSTANCE res = ShellExecuteA(nullptr, "open", uri.c_str(), nullptr, nullptr,
                                SW_SHOWNORMAL);
  return reinterpret_cast<intptr_t>(res) > 32;
#else
  return true;
#endif
}

fs::path steam_install_path() {
  fs::path steamPath;
  if (!get_steam_path_from_registry(steamPath))
    return {};
  return steamPath.lexically_normal();
}

// ── Installing into a chosen library ─────────────────────────────────────────
namespace {

string path_key(const fs::path &p) {
  string s = p.lexically_normal().string();
  std::replace(s.begin(), s.end(), '/', '\\');
  while (s.size() > 3 && s.back() == '\\')
    s.pop_back();
  return to_lower(s);
}

// Where one app's manifest is, and the library holding it; empty paths if no
// library has one.
fs::path find_app_manifest(int appid, fs::path *libraryOut = nullptr) {
  const string name = "appmanifest_" + std::to_string(appid) + ".acf";
  for (const fs::path &lib : steam_library_folders()) {
    std::error_code ec;
    const fs::path p = lib / "steamapps" / name;
    if (fs::exists(p, ec)) {
      if (libraryOut)
        *libraryOut = lib;
      return p;
    }
  }
  return {};
}

// The manifest for one app in whichever library holds it, or "" if none does.
string read_app_manifest(int appid) {
  const fs::path p = find_app_manifest(appid);
  return p.empty() ? string() : read_text_file(p);
}

unsigned long long acf_number(const string &acf, const string &key) {
  string value;
  if (!extract_acf_field(acf, key, value))
    return 0;
  return static_cast<unsigned long long>((std::max)(0LL, to_long_long(trim(value))));
}

}  // namespace

vector<SteamLibrary> steam_libraries() {
  vector<SteamLibrary> out;
  const fs::path steamPath = steam_install_path();
  const vector<fs::path> vdf = steam_library_folders();

  // Steam's own numbering when the client answers; otherwise libraryfolders.vdf
  // lists them under "0", "1", ... in that same order.
  vector<SteamInternalFolder> folders = steam_internal_library_folders();
  if (folders.empty()) {
    for (size_t i = 0; i < vdf.size(); ++i)
      folders.push_back({static_cast<int>(i), vdf[i]});
  }

  for (const SteamInternalFolder &f : folders) {
    SteamLibrary lib;
    lib.root = f.path;
    lib.folderIndex = f.index;
    lib.isDefault = !steamPath.empty() && path_key(f.path) == path_key(steamPath);
    lib.drive = f.path.root_name().string();

    std::error_code ec;
    const fs::space_info space = fs::space(f.path, ec);
    if (!ec) {
      lib.freeBytes = space.available;
      lib.totalBytes = space.capacity;
    }
#ifdef _WIN32
    wchar_t label[MAX_PATH + 1] = {};
    const std::wstring root = f.path.root_path().wstring();
    if (GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr,
                              nullptr, nullptr, 0)) {
      lib.label = fs::path(label).string();
    }
#endif
    vlog::line("SteamInstall",
               "library " + std::to_string(lib.folderIndex) + ": " + lib.root.string() +
                   " [" + (lib.label.empty() ? "no label" : lib.label) + "] " +
                   std::to_string(lib.freeBytes / (1024ull * 1024 * 1024)) + " GB free" +
                   (lib.isDefault ? ", default" : ""));
    out.push_back(std::move(lib));
  }
  return out;
}

SteamInstallRequest install_steam_game_to(int appid, int folderIndex) {
  SteamInstallRequest req;
  if (appid <= 0)
    return req;

  if (folderIndex >= 0 && steam_internal_available()) {
    const int err = steam_internal_install(appid, folderIndex);
    if (err == 0) {
      req.started = true;
      req.viaClient = true;
      req.detail = "route: client, folder " + std::to_string(folderIndex);
      return req;
    }
    req.error = err;
    req.detail = string("client refused: EAppError ") + std::to_string(err) + " (" +
                 steam_app_error_name(err) + ")";
  } else {
    req.detail = folderIndex < 0 ? string("no folder chosen")
                                 : "client route unavailable: " + steam_internal_unavailable_reason();
  }

  // Steam's own dialog. It takes focus and asks for the drive again, but it
  // reports whatever went wrong (no space, region lock) in Steam's own words,
  // which is better than a launcher that silently did nothing.
  req.started = install_steam_game_by_appid(appid);
  req.detail += "; fallback steam://install " + string(req.started ? "opened" : "failed to open");
  return req;
}

vector<int> steam_pending_install_appids() {
  vector<int> out;
  for (const fs::path &lib : steam_library_folders()) {
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(lib / "steamapps", ec)) {
      const string fn = e.path().filename().string();
      if (fn.rfind("appmanifest_", 0) != 0 || e.path().extension() != ".acf")
        continue;
      const string acf = read_text_file(e.path());
      string flags, appid, name;
      if (!extract_acf_field(acf, "StateFlags", flags) || !extract_acf_field(acf, "appid", appid))
        continue;
      extract_acf_field(acf, "name", name);
      if ((to_long_long(trim(flags)) & 4) != 0 || is_steam_tool_name(name))
        continue;
      const int id = static_cast<int>(to_long_long(trim(appid)));
      if (id > 0)
        out.push_back(id);
    }
  }
  return out;
}

SteamInstallStatus steam_install_progress(int appid) {
  SteamInstallStatus st;
  const string acf = read_app_manifest(appid);

  const SteamInternalProgress live = steam_internal_progress(appid);
  if (live.ok) {
    st.source = "client";
    const std::uint32_t s = live.state;
    if ((s & kSteamAppFullyInstalled) && !(s & kSteamAppUpdateRequired)) {
      st.phase = SteamInstallStatus::Phase::Installed;
      return st;
    }
    if ((s & kSteamAppUninstalled) && !(s & kSteamAppUpdateRequired)) {
      st.phase = SteamInstallStatus::Phase::NotInstalled;
      return st;
    }
    // The counters are trusted only once the update has started -- before
    // that they count preallocated bytes, which reach nearly the full size
    // in seconds -- and only while they agree with the manifest's staged
    // size: a slot that has moved reads as a different number here before it
    // reads as anything obviously wrong.
    const bool started = (s & kSteamAppUpdateStarted) != 0;
    const unsigned long long staged = acf_number(acf, "BytesToStage");
    const bool plausible = started && live.haveBytes &&
                           (staged == 0 || (live.total > staged * 3 / 4 && live.total < staged * 5 / 4));
    if (plausible) {
      st.done = live.done;
      st.total = live.total;
      st.bytesPerSecond = live.bytesPerSecond;
      st.secondsLeft = live.secondsLeft;
      // Checked against the manifest like the staged pair.
      const unsigned long long toDownload = acf_number(acf, "BytesToDownload");
      if (live.haveDownload &&
          (toDownload == 0 || (live.downloadTotal > toDownload * 3 / 4 &&
                               live.downloadTotal < toDownload * 5 / 4)))
        st.downloaded = live.downloadDone;
    }
    if (s & kSteamAppUpdatePaused) {
      st.phase = SteamInstallStatus::Phase::Paused;
      // A paused app's update info reads 0/0, so what it has fetched comes
      // from the manifest, which Steam writes when it pauses. The staging
      // pair, because that is what the live counters measure (their total is
      // BytesToStage), so the figure does not jump between running and paused.
      if (!plausible) {
        st.total = acf_number(acf, "BytesToStage");
        st.done = (std::min)(acf_number(acf, "BytesStaged"), st.total);
        if (st.total == 0) {
          st.total = acf_number(acf, "BytesToDownload");
          st.done = (std::min)(acf_number(acf, "BytesDownloaded"), st.total);
        }
      }
      return st;
    }
    else
      st.phase = (plausible && live.done > 0) ? SteamInstallStatus::Phase::Downloading
                                              : SteamInstallStatus::Phase::Queued;
    if (plausible || !live.haveBytes || !started)
      return st;
    // Live counters disagreed with the manifest: fall through to it.
  }

  st.source = "manifest";
  if (acf.empty()) {
    st.phase = SteamInstallStatus::Phase::NotInstalled;
    return st;
  }
  const unsigned long long flags = acf_number(acf, "StateFlags");
  if ((flags & 4) && !(flags & 2)) {
    st.phase = SteamInstallStatus::Phase::Installed;
    return st;
  }
  st.total = acf_number(acf, "BytesToDownload");
  st.done = (std::min)(acf_number(acf, "BytesDownloaded"), st.total);
  if (flags & 512)
    st.phase = SteamInstallStatus::Phase::Paused;
  else
    st.phase = st.total > 0 ? SteamInstallStatus::Phase::Downloading
                            : SteamInstallStatus::Phase::Queued;
  return st;
}

// ── Cancelling a download ────────────────────────────────────────────────────
// UninstallApp on an app that is still downloading cancels it, and Steam
// deletes the manifest and the downloading\ and temp\ folders itself. What it
// leaves is the install folder under common\ -- empty when the download had
// not reached staging, partly filled when it had -- so that is swept here,
// along with anything else of this appid's that is still around.
namespace {

// Files and bytes under `p`, then removes it. Logs either way.
void remove_leftover(const fs::path &p, SteamCancelResult &result) {
  std::error_code ec;
  if (!fs::exists(p, ec))
    return;
  int files = 0;
  unsigned long long bytes = 0;
  for (auto it = fs::recursive_directory_iterator(p, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code sizeEc;
    if (it->is_regular_file(sizeEc)) {
      ++files;
      bytes += it->file_size(sizeEc);
    }
  }
  ec.clear();
  fs::remove_all(p, ec);
  if (ec) {
    vlog::item("SteamInstall", "remove " + p.string(), vlog::Status::Fail, ec.message());
    return;
  }
  result.filesRemoved += files;
  result.bytesRemoved += bytes;
  vlog::item("SteamInstall", "removed " + p.string(), vlog::Status::Ok,
             std::to_string(files) + " files, " + std::to_string(bytes / (1024 * 1024)) + " MB");
}

// Whether any manifest in any library installs into `installdir`. Two apps
// can share a folder (a demo and its full game, say); the sweep must never
// take one that another install still owns.
bool installdir_in_use(const string &installdir) {
  const string wanted = to_lower(installdir);
  for (const fs::path &lib : steam_library_folders()) {
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(lib / "steamapps", ec)) {
      const string fn = e.path().filename().string();
      if (fn.rfind("appmanifest_", 0) != 0 || e.path().extension() != ".acf")
        continue;
      string dir;
      if (extract_acf_field(read_text_file(e.path()), "installdir", dir) &&
          to_lower(dir) == wanted)
        return true;
    }
  }
  return false;
}

// Whether the running client says the app is gone. Steam can finish an
// uninstall and still leave the manifest behind (StateFlags 514, "update
// paused"); the client's answer is the one to believe then.
bool client_says_uninstalled(int appid) {
  const SteamInternalProgress live = steam_internal_progress(appid);
  return live.ok && (live.state & kSteamAppUninstalled) && !(live.state & kSteamAppUpdateRequired);
}

// A folder name Steam would put under common\ -- one plain path component.
bool safe_installdir(const string &dir) {
  if (dir.empty() || dir == "." || dir == "..")
    return false;
  return dir.find_first_of("\\/:") == string::npos && dir.find("..") == string::npos;
}

}  // namespace

bool set_steam_install_paused(int appid, bool paused) {
  if (appid <= 0)
    return false;
  vlog::line("SteamInstall", string(paused ? "Pause" : "Resume") + " appid " + std::to_string(appid));
  const bool ok = steam_internal_set_paused(appid, paused);
  if (!ok)
    vlog::item("SteamInstall", string(paused ? "pause" : "resume") + " appid " + std::to_string(appid),
               vlog::Status::Fail, steam_internal_unavailable_reason().empty()
                                       ? "Steam refused"
                                       : steam_internal_unavailable_reason());
  return ok;
}

SteamCancelResult cancel_steam_install(int appid) {
  SteamCancelResult result;
  if (appid <= 0)
    return result;

  fs::path library;
  const fs::path manifest = find_app_manifest(appid, &library);
  if (manifest.empty()) {
    result.detail = "no manifest -- nothing downloading";
    vlog::item("SteamInstall", "cancel appid " + std::to_string(appid), vlog::Status::Skipped,
               result.detail);
    return result;
  }

  const string acf = read_text_file(manifest);
  string installdir;
  extract_acf_field(acf, "installdir", installdir);
  // Cancel stops a download. A finished game is never touched from here.
  if (acf_number(acf, "StateFlags") & 4) {
    result.detail = "refused: the game is fully installed";
    vlog::item("SteamInstall", "cancel appid " + std::to_string(appid), vlog::Status::Fail,
               result.detail);
    return result;
  }
  vlog::line("SteamInstall", "Cancel appid " + std::to_string(appid) + " in " + library.string() +
                                 " (installdir \"" + installdir + "\")");

  // Already uninstalled as far as Steam is concerned: only the manifest is
  // left. UninstallApp would refuse with NotInstalled and the dialog would
  // have nothing to do, so go straight to the sweep.
  const bool stale = client_says_uninstalled(appid);
  const int err = stale ? 0 : steam_internal_uninstall(appid);
  if (stale) {
    result.viaClient = true;
    result.detail = "Steam already had it uninstalled; manifest left over";
    vlog::line("SteamInstall", result.detail);
  } else if (err == 0) {
    result.viaClient = true;
  } else {
    // Steam's own confirm dialog. The sweep below only runs if the player
    // confirms there, because it waits for the manifest to go.
    result.detail = err < 0 ? "client route unavailable" : string("client refused: EAppError ") +
                                                               std::to_string(err) + " (" +
                                                               steam_app_error_name(err) + ")";
    const bool opened = uninstall_steam_game_by_appid(appid);
    result.detail += string("; fallback steam://uninstall ") + (opened ? "opened" : "failed to open");
    vlog::line("SteamInstall", result.detail);
  }

  // Steam took about a second here; the dialog route waits on the player.
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(result.viaClient ? 15 : 120);
  bool gone = false;
  while (std::chrono::steady_clock::now() < deadline) {
    std::error_code ec;
    if (!fs::exists(manifest, ec)) {
      gone = true;
      break;
    }
    if (client_says_uninstalled(appid)) {
      // Done in Steam, manifest kept: remove it the way Steam would have.
      fs::remove(manifest, ec);
      vlog::item("SteamInstall", "remove leftover " + manifest.string(),
                 ec ? vlog::Status::Fail : vlog::Status::Ok, ec ? ec.message() : "client says uninstalled");
      gone = !ec;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  if (!gone) {
    result.detail += (result.detail.empty() ? "" : "; ") + string("manifest still there -- files left alone");
    vlog::item("SteamInstall", "cancel appid " + std::to_string(appid), vlog::Status::Fail, result.detail);
    return result;
  }
  result.cancelled = true;

  const fs::path steamapps = library / "steamapps";
  const string id = std::to_string(appid);
  const auto sweep = [&]() {
    remove_leftover(steamapps / "downloading" / id, result);
    remove_leftover(steamapps / "temp" / id, result);
    remove_leftover(steamapps / "shadercache" / id, result);

    if (!safe_installdir(installdir)) {
      vlog::item("SteamInstall", "install folder", vlog::Status::Skipped,
                 "installdir \"" + installdir + "\" is not a plain folder name -- left alone");
    } else if (installdir_in_use(installdir)) {
      vlog::item("SteamInstall", "install folder", vlog::Status::Skipped,
                 "\"" + installdir + "\" belongs to another installed app -- left alone");
    } else {
      const fs::path common = (steamapps / "common").lexically_normal();
      const fs::path target = (common / installdir).lexically_normal();
      if (target.parent_path() == common)
        remove_leftover(target, result);
    }
  };
  sweep();

  // A download cancelled while it is running can have its stopping job write
  // the manifest back a moment after Steam has finished the uninstall --
  // seen with StateFlags 514 and the bytes it had, while Steam itself said
  // Uninstalled. Left there, Steam would resume the download on its next
  // start. Only a manifest Steam no longer claims is removed.
  std::this_thread::sleep_for(std::chrono::seconds(3));
  std::error_code ec;
  if (fs::exists(manifest, ec)) {
    const SteamInternalProgress live = steam_internal_progress(appid);
    if (live.ok && (live.state & kSteamAppUninstalled) && !(live.state & kSteamAppUpdateRequired)) {
      fs::remove(manifest, ec);
      vlog::item("SteamInstall", "removed " + manifest.string(), ec ? vlog::Status::Fail : vlog::Status::Ok,
                 ec ? ec.message() : "stale manifest rewritten after uninstall");
      if (!ec)
        sweep();
    } else {
      vlog::item("SteamInstall", manifest.string(), vlog::Status::Skipped,
                 "reappeared and Steam still claims it -- left alone");
    }
  }

  vlog::item("SteamInstall", "cancel appid " + id, vlog::Status::Ok,
             string(result.viaClient ? "route client" : "route dialog") + ", removed " +
                 std::to_string(result.filesRemoved) + " files (" +
                 std::to_string(result.bytesRemoved / (1024 * 1024)) + " MB) of leftovers");
  return result;
}

// ── Owned games ──────────────────────────────────────────────────────────────
// GetOwnedGames answers with one flat array of flat objects, so it is read the
// way the other API responses here are: by scanning for keys rather than by
// building a document. The only nesting inside a game is the
// content_descriptorids array, which holds numbers.
namespace {

// The numeric value of "key" inside json[begin, end), or 0 when absent.
long long json_number_in(const string &json, size_t begin, size_t end,
                         const string &key) {
  const string needle = "\"" + key + "\"";
  size_t at = json.find(needle, begin);
  if (at == string::npos || at >= end)
    return 0;
  at += needle.size();
  while (at < end && (json[at] == ':' || std::isspace(static_cast<unsigned char>(json[at]))))
    ++at;
  size_t stop = at;
  while (stop < end && (std::isdigit(static_cast<unsigned char>(json[stop])) || json[stop] == '-'))
    ++stop;
  return to_long_long(json.substr(at, stop - at));
}

// The string value of "key" inside json[begin, end), or "" when absent.
string json_string_in(const string &json, size_t begin, size_t end,
                      const string &key) {
  const string needle = "\"" + key + "\"";
  size_t at = json.find(needle, begin);
  if (at == string::npos || at >= end)
    return {};
  at = json.find('"', at + needle.size());
  if (at == string::npos || at >= end)
    return {};
  return json_read_string(json, at);
}

// Index of the '}' closing the object that opens at json[open].
size_t json_object_end(const string &json, size_t open) {
  int depth = 0;
  for (size_t i = open; i < json.size(); ++i) {
    const char c = json[i];
    if (c == '"') {
      json_read_string(json, i);   // leaves i on the closing quote
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      if (--depth == 0)
        return i;
    }
  }
  return string::npos;
}

string base64url_decode(const string &in) {
  string out;
  int bits = 0;
  unsigned value = 0;
  for (char c : in) {
    int digit;
    if (c >= 'A' && c <= 'Z')      digit = c - 'A';
    else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
    else if (c >= '0' && c <= '9') digit = c - '0' + 52;
    else if (c == '-' || c == '+') digit = 62;
    else if (c == '_' || c == '/') digit = 63;
    else break;   // '=' padding, or the end of the segment
    value = (value << 6) | static_cast<unsigned>(digit);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((value >> bits) & 0xFF));
    }
  }
  return out;
}

// A JWT is three base64url segments joined by dots. Checked before the token
// goes into a URL, so nothing in it can add a parameter or break the path.
bool looks_like_jwt(const string &token) {
  if (token.empty() || token.size() > 4096)
    return false;
  int dots = 0;
  for (char c : token) {
    if (c == '.') { ++dots; continue; }
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
      return false;
  }
  return dots == 2;
}

}  // namespace

unsigned long long steam_id_from_webapi_token(const string &token) {
  if (!looks_like_jwt(token))
    return 0;
  const size_t first = token.find('.');
  const size_t second = token.find('.', first + 1);
  const string payload = base64url_decode(token.substr(first + 1, second - first - 1));
  const string sub = json_string_in(payload, 0, payload.size(), "sub");
  try {
    return sub.empty() ? 0 : std::stoull(sub);
  } catch (...) {
    return 0;
  }
}

vector<SteamOwnedGame> fetch_steam_owned_games(const string &token,
                                               string &error, bool &expired) {
  vector<SteamOwnedGame> games;
  error.clear();
  expired = false;

  const unsigned long long steamId = steam_id_from_webapi_token(token);
  if (steamId == 0) {
    error = "the Steam session token could not be read";
    expired = true;
    return games;
  }

  const string path = "/IPlayerService/GetOwnedGames/v1/?access_token=" + token +
                      "&steamid=" + std::to_string(steamId) +
                      "&include_appinfo=1&include_played_free_games=1";
  const wstring wpath(path.begin(), path.end());   // ASCII: looks_like_jwt

  string body;
  try {
    body = https_get(L"api.steampowered.com", wpath, L"");
  } catch (const std::exception &e) {
    // The message is only ever "HTTP Error <status>" or a WinHTTP step name,
    // never the URL, so it is safe to pass on.
    const string what = e.what();
    expired = what == "HTTP Error 401" || what == "HTTP Error 403";
    error = expired ? "the Steam session has expired" : what;
    return games;
  }

  // An account that owns nothing gets {"response":{}} -- no "games" at all.
  size_t at = body.find("\"games\"");
  if (at == string::npos || (at = body.find('[', at)) == string::npos)
    return games;

  for (++at; at < body.size();) {
    const char c = body[at];
    if (c == ']')
      break;
    if (c != '{') {
      ++at;
      continue;
    }
    const size_t end = json_object_end(body, at);
    if (end == string::npos)
      break;   // truncated response; keep what was read

    SteamOwnedGame game;
    game.appid = static_cast<int>(json_number_in(body, at, end, "appid"));
    game.name = json_string_in(body, at, end, "name");
    game.playtimeMinutes = json_number_in(body, at, end, "playtime_forever");
    game.lastPlayed = json_number_in(body, at, end, "rtime_last_played");
    if (game.appid > 0 && !game.name.empty() && !is_steam_tool_name(game.name))
      games.push_back(std::move(game));

    at = end + 1;
  }

  std::sort(games.begin(), games.end(),
            [](const SteamOwnedGame &a, const SteamOwnedGame &b) {
              if (a.name == b.name)
                return a.appid < b.appid;
              return a.name < b.name;
            });
  return games;
}

bool is_steam_app_installed(int appid) {
  fs::path steamPath;
  if (!get_steam_path_from_registry(steamPath))
    return false;

  const string manifest = "appmanifest_" + std::to_string(appid) + ".acf";
  for (const auto &lib : parse_libraryfolders_vdf(steamPath)) {
    std::error_code ec;
    if (fs::exists(lib / "steamapps" / manifest, ec))
      return true;
  }
  return false;
}

int get_steam_appid_for_install_dir(const fs::path& installDir) {
  auto games = read_installed_steam_games();
  for (const auto &g : games) {
    std::error_code ec;
    if (fs::equivalent(g.installDir, installDir, ec)) {
      return g.appid;
    }
  }
  return 0;
}
