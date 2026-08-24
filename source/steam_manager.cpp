#include "steam_manager.h"

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

using std::string;
using std::vector;

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
                           std::time_t *outStart, std::time_t *outEnd) {
    const int  kStartupTimeoutSeconds = 120;
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

    // Phase 2 — wait for it to go away.
    while (steam_app_active(appid, installDir))
        std::this_thread::sleep_for(kPollInterval);

    const std::time_t end = std::time(nullptr);
    if (outStart) *outStart = start;
    if (outEnd)   *outEnd   = end;
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

  const string lower_name = to_lower(name);
  if (lower_name.find("proton") != string::npos || 
      lower_name.find("runtime") != string::npos ||
      lower_name.find("steamworks") != string::npos) {
      return false;
  }

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
