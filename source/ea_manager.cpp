#include "ea_manager.h"

#include "steam_manager.h"
#include "vortex_log.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
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

// A manifest title as a display name: XML entities decoded, trademark signs
// dropped (IGDB's titles carry none, so they only spoil the search), trimmed.
string clean_title(string s) {
  static const std::pair<const char *, const char *> replacements[] = {
      {"&amp;", "&"}, {"&apos;", "'"}, {"&quot;", "\""}, {"&lt;", "<"},
      {"&gt;", ">"},  {"\xE2\x84\xA2", ""}, {"\xC2\xAE", ""}, {"\xC2\xA9", ""},
  };
  for (const auto &[from, to] : replacements) {
    const string needle = from;
    for (size_t pos = s.find(needle); pos != string::npos;
         pos = s.find(needle, pos + std::char_traits<char>::length(to)))
      s.replace(pos, needle.size(), to);
  }
  const size_t a = s.find_first_not_of(" \t\r\n");
  const size_t b = s.find_last_not_of(" \t\r\n");
  return a == string::npos ? string() : s.substr(a, b - a + 1);
}

// A path as a lowercase, backslash-separated string with no trailing
// separator, so two spellings of one folder compare equal.
std::wstring folder_key(const fs::path &p) {
  std::wstring s = p.wstring();
  std::replace(s.begin(), s.end(), L'/', L'\\');
  std::transform(s.begin(), s.end(), s.begin(), ::towlower);
  while (!s.empty() && s.back() == L'\\')
    s.pop_back();
  return s;
}

bool is_under(const std::wstring &key, const std::wstring &rootKey) {
  return !rootKey.empty() && key.size() > rootKey.size() &&
         key.compare(0, rootKey.size(), rootKey) == 0 &&
         key[rootKey.size()] == L'\\';
}

// Steam and Epic copies of EA games carry the same __Installer manifest, but
// belong to their own store.
string other_store(const fs::path &installDir, const std::wstring &key) {
  for (const fs::path &lib : steam_library_folders())
    if (is_under(key, folder_key(lib / "steamapps" / "common")))
      return "it is in a Steam library";
  std::error_code ec;
  if (fs::is_directory(installDir / ".egstore", ec))
    return "it is an Epic install";
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

// An uninstall entry for this folder that EA's installer wrote. The EA app
// registers every game it installs this way, with Cleanup.exe under
// Common Files\EAInstaller\<game>\ as the uninstaller.
bool has_ea_uninstall_entry(const std::wstring &key) {
  static const wchar_t *const roots[] = {
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
      L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
  };
  for (const wchar_t *root : roots) {
    HKEY hRoot = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ, &hRoot) !=
        ERROR_SUCCESS)
      continue;
    bool found = false;
    wchar_t name[256];
    for (DWORD i = 0; !found; ++i) {
      DWORD nameLen = 256;
      const LONG rc = RegEnumKeyExW(hRoot, i, name, &nameLen, nullptr, nullptr,
                                    nullptr, nullptr);
      if (rc == ERROR_NO_MORE_ITEMS)
        break;
      if (rc != ERROR_SUCCESS)
        continue;
      HKEY hEntry = nullptr;
      if (RegOpenKeyExW(hRoot, name, 0, KEY_READ, &hEntry) != ERROR_SUCCESS)
        continue;
      const std::wstring location = reg_string(hEntry, L"InstallLocation");
      if (!location.empty() && folder_key(location) == key) {
        std::wstring uninstall = reg_string(hEntry, L"UninstallString");
        std::transform(uninstall.begin(), uninstall.end(), uninstall.begin(),
                       ::towlower);
        found = uninstall.find(L"\\eainstaller\\") != std::wstring::npos;
      }
      RegCloseKey(hEntry);
    }
    RegCloseKey(hRoot);
    if (found)
      return true;
  }
  return false;
}

bool ea_app_registered() {
  HKEY hKey = nullptr;
  if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"origin2\\shell\\open\\command", 0,
                    KEY_READ, &hKey) != ERROR_SUCCESS)
    return false;
  RegCloseKey(hKey);
  return true;
}
#endif

} // namespace

bool read_ea_manifest(const fs::path &installDir, EaGame &out) {
  if (installDir.empty())
    return false;
  const string xml =
      read_text_file(installDir / "__Installer" / "installerdata.xml");
  if (xml.find("<DiPManifest") == string::npos)
    return false;

  std::smatch m;
  if (!std::regex_search(xml, m, std::regex("<contentID>\\s*([A-Za-z0-9._-]+)\\s*</contentID>")))
    return false;
  EaGame game;
  game.contentId = m[1].str();
  // English first, as IGDB is searched in English; any locale beats none.
  if (std::regex_search(xml, m, std::regex("<gameTitle locale=\"en_US\">([^<]*)</gameTitle>")) ||
      std::regex_search(xml, m, std::regex("<gameTitle[^>]*>([^<]*)</gameTitle>")))
    game.title = clean_title(m[1].str());

  out = std::move(game);
  return true;
}

bool read_ea_install(const fs::path &installDir, EaGame &out) {
#ifdef _WIN32
  EaGame game;
  if (!read_ea_manifest(installDir, game))
    return false;

  // From here on the folder is EA-made, so say why it is not launched as one.
  const std::wstring key = folder_key(installDir);
  string why = other_store(installDir, key);
  if (why.empty() && !has_ea_uninstall_entry(key))
    why = "no EAInstaller uninstall entry for the folder";
  if (why.empty() && !ea_app_registered())
    why = "the EA app is not installed (origin2:// not registered)";
  if (!why.empty()) {
    vlog::line("Play", "EA manifest found but not launching through the EA "
                       "app: " + why);
    return false;
  }

  out = std::move(game);
  return true;
#else
  (void)installDir;
  (void)out;
  return false;
#endif
}

bool launch_ea_game(const std::string &contentId) {
#ifdef _WIN32
  const string uri = "origin2://game/launch?offerIds=" + contentId;
  HINSTANCE res = ShellExecuteA(nullptr, "open", uri.c_str(), nullptr, nullptr,
                                SW_SHOWNORMAL);
  return reinterpret_cast<intptr_t>(res) > 32;
#else
  std::cout << "[MOCK] Linux: Skipping EA launch for " << contentId << "\n";
  return true;
#endif
}
