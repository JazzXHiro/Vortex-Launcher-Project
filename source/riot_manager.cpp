#include "riot_manager.h"

#include "json_text.h"
#include "vortex_log.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using std::string;

namespace {

string read_text_file(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  if (!in)
    return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A path as a lowercase, backslash-separated string with no trailing
// separator, so two spellings of one folder compare equal. Riot writes its
// paths with forward slashes and, in RiotClientInstalls.json, a trailing one.
std::wstring folder_key(const fs::path &p) {
  std::wstring s = p.wstring();
  std::replace(s.begin(), s.end(), L'/', L'\\');
  std::transform(s.begin(), s.end(), s.begin(), ::towlower);
  while (!s.empty() && s.back() == L'\\')
    s.pop_back();
  return s;
}

// A UTF-8 path as Riot writes it, in the platform's spelling.
fs::path riot_path(const string &utf8) {
  return fs::u8path(utf8).make_preferred();
}

fs::path riot_data_dir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
  const fs::path root = (n > 0 && n < MAX_PATH) ? fs::path(buf)
                                                : fs::path(L"C:\\ProgramData");
  return root / "Riot Games";
#else
  return {};
#endif
}

// A top-level `key: "value"` line of a product_settings.yaml. Riot quotes
// every string it writes there, and the keys wanted are never nested.
string yaml_value(const string &yaml, const string &key) {
  std::smatch m;
  if (std::regex_search(yaml, m,
                        std::regex("(^|\\n)" + key + ":[ \\t]*\"([^\"\\n]*)\"")))
    return m[2].str();
  return {};
}

// Every "key": "string" pair in a JSON document, at any depth. Enough for
// RiotClientInstalls.json, which is objects of path strings and nothing else.
std::vector<std::pair<string, string>> json_string_pairs(const string &json) {
  std::vector<std::pair<string, string>> pairs;
  size_t pos = json.find('"');
  while (pos != string::npos && pos < json.size()) {
    string key = json_read_string(json, pos);
    const size_t colon = json.find_first_not_of(" \t\r\n", pos + 1);
    if (colon != string::npos && json[colon] == ':') {
      size_t value = json.find_first_not_of(" \t\r\n", colon + 1);
      if (value != string::npos && json[value] == '"') {
        string text = json_read_string(json, value);
        pairs.emplace_back(std::move(key), std::move(text));
        pos = json.find('"', value + 1);
        continue;
      }
    }
    pos = json.find('"', pos + 1);
  }
  return pairs;
}

// The client that owns installDir: its associated_client entry first, then
// the default and live clients. Only one that exists on disk will do.
fs::path riot_client_for(const std::vector<std::pair<string, string>> &installs,
                         const fs::path &installDir) {
  const std::wstring key = folder_key(installDir);
  string associated, rcDefault, rcLive;
  for (const auto &[k, v] : installs) {
    if (k == "rc_default")
      rcDefault = v;
    else if (k == "rc_live")
      rcLive = v;
    else if (associated.empty() && folder_key(riot_path(k)) == key)
      associated = v;
  }
  for (const string *candidate : {&associated, &rcDefault, &rcLive}) {
    if (candidate->empty())
      continue;
    const fs::path exe = riot_path(*candidate);
    std::error_code ec;
    if (fs::is_regular_file(exe, ec))
      return exe;
  }
  return {};
}

#ifdef _WIN32
std::wstring reg_string(HKEY key, const wchar_t *value) {
  DWORD size = 0;
  if (RegGetValueW(key, nullptr, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                   nullptr, nullptr, &size) != ERROR_SUCCESS ||
      size == 0)
    return {};
  std::wstring out(size / sizeof(wchar_t), L'\0');
  if (RegGetValueW(key, nullptr, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                   nullptr, out.data(), &size) != ERROR_SUCCESS)
    return {};
  out.resize(wcsnlen(out.c_str(), out.size()));
  return out;
}

// The DisplayName Riot gave the product's uninstall entry ("VALORANT",
// "League of Legends"), which is the store's own spelling of the title.
string uninstall_display_name(const string &entry) {
  const std::wstring subkey =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Riot Game " +
      fs::u8path(entry).wstring();
  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
      continue;
    const std::wstring name = reg_string(hKey, L"DisplayName");
    RegCloseKey(hKey);
    if (!name.empty())
      return fs::path(name).u8string();
  }
  return {};
}

// Starts the Riot Client with args and lets it go; it hands back nothing worth
// waiting on, as the game is a process of its own.
bool run_riot_client(const fs::path &client, const std::wstring &args) {
  std::wstring command = L"\"" + client.wstring() + L"\" " + args;
  std::vector<wchar_t> cmdBuf(command.begin(), command.end());
  cmdBuf.push_back(L'\0');
  const std::wstring workDir = client.parent_path().wstring();

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, workDir.c_str(), &si, &pi)) {
    vlog::line("Play", "could not start the Riot Client (error " +
                           std::to_string(GetLastError()) + ")");
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}
#endif

string trim(const string &s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  const size_t b = s.find_last_not_of(" \t\r\n");
  return a == string::npos ? string() : s.substr(a, b - a + 1);
}

string lower(string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

} // namespace

std::vector<RiotGame> read_installed_riot_games(bool log) {
  std::vector<RiotGame> games;
  const fs::path dataDir = riot_data_dir();
  std::error_code ec;
  if (dataDir.empty() || !fs::is_directory(dataDir / "Metadata", ec))
    return games;

  const auto installs =
      json_string_pairs(read_text_file(dataDir / "RiotClientInstalls.json"));

  // Product and patchline go onto the client's command line, so only the
  // plain ids Riot uses are accepted from a folder name.
  static const std::regex entryRe("([A-Za-z0-9_-]+)\\.([A-Za-z0-9_.-]+)");

  for (const auto &dirEntry : fs::directory_iterator(dataDir / "Metadata", ec)) {
    std::error_code ec2;
    if (!dirEntry.is_directory(ec2))
      continue;
    // "Riot Client" is the client's own folder, and has no product id.
    const string entry = dirEntry.path().filename().u8string();
    std::smatch m;
    if (!std::regex_match(entry, m, entryRe))
      continue;
    const string yaml = read_text_file(dirEntry.path() /
                                       fs::u8path(entry + ".product_settings.yaml"));
    const string installPath = yaml_value(yaml, "product_install_full_path");
    if (installPath.empty())
      continue;

    RiotGame game;
    game.product = m[1].str();
    game.patchline = m[2].str();
    game.installDir = riot_path(installPath);
    const string rootPath = yaml_value(yaml, "product_install_root");
    if (!rootPath.empty())
      game.installRoot = riot_path(rootPath);

    if (!fs::is_directory(game.installDir, ec2)) {
      if (log)
        vlog::item("Scan", entry, vlog::Status::Skipped,
                   "Riot lists it at " + installPath + " but the folder is gone");
      continue;
    }
    game.clientExe = riot_client_for(installs, game.installDir);
    if (game.clientExe.empty()) {
      if (log)
        vlog::item("Scan", entry, vlog::Status::Skipped,
                   "no Riot Client found to launch it");
      continue;
    }

#ifdef _WIN32
    game.title = trim(uninstall_display_name(entry));
#endif
    if (game.title.empty()) {
      // ...\VALORANT\live: the patchline folder says nothing, its parent does.
      const fs::path leaf = game.installDir.filename();
      game.title = lower(leaf.u8string()) == lower(game.patchline)
                       ? game.installDir.parent_path().filename().u8string()
                       : leaf.u8string();
    }
    games.push_back(std::move(game));
  }
  return games;
}

bool riot_install_matches(const RiotGame &game, const fs::path &dir) {
  if (dir.empty())
    return false;
  const std::wstring key = folder_key(dir);
  if (folder_key(game.installDir) == key)
    return true;
  // The folder above is only taken when it holds this one product, never
  // the Riot Games root that holds them all.
  return folder_key(game.installDir.parent_path()) == key &&
         key != folder_key(game.installRoot);
}

bool read_riot_install(const fs::path &installDir, RiotGame &out) {
  for (RiotGame &game : read_installed_riot_games()) {
    if (riot_install_matches(game, installDir)) {
      out = std::move(game);
      return true;
    }
  }
  return false;
}

bool launch_riot_game(const RiotGame &game) {
#ifdef _WIN32
  return run_riot_client(game.clientExe,
                         L"--launch-product=" + fs::u8path(game.product).wstring() +
                             L" --launch-patchline=" +
                             fs::u8path(game.patchline).wstring());
#else
  std::cout << "[MOCK] Linux: Skipping Riot launch for " << game.product << "\n";
  return true;
#endif
}

bool uninstall_riot_game(const RiotGame &game) {
#ifdef _WIN32
  return run_riot_client(game.clientExe,
                         L"--uninstall-product=" + fs::u8path(game.product).wstring() +
                             L" --uninstall-patchline=" +
                             fs::u8path(game.patchline).wstring());
#else
  (void)game;
  return false;
#endif
}
