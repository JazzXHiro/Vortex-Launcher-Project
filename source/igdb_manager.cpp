// Resolves local game folder names to canonical IGDB names.

#include "igdb_manager.h"
#include "app_paths.h"
#include "game_manager.h"
#include "json_text.h"
#include "metadata_manager.h"

#include "secrets.h"
#include "vortex_log.h"

// Credentials are read at runtime from analytics/.env — set IGDB_CLIENT_ID and
// IGDB_CLIENT_SECRET there. They are deliberately not compiled in: the pair
// that used to live here was committed to git history and had to be revoked at
// dev.twitch.tv rather than merely deleted. Keeping them out of the binary
// also means rotating a key no longer requires a rebuild.

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

using std::cout;
using std::string;
using std::wstring;

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

static string winhttp_error_message(const char *operation, DWORD err) {
  LPSTR message = nullptr;
  DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS;
  DWORD len = FormatMessageA(flags, nullptr, err, 0, (LPSTR)&message, 0, nullptr);

  string out = operation;
  out += " failed";
  if (err != 0) {
    out += " (";
    out += std::to_string(err);
    out += ")";
  }
  if (len > 0 && message) {
    out += ": ";
    out += message;
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n'))
      out.pop_back();
  }

  if (message)
    LocalFree(message);
  return out;
}

[[noreturn]] static void throw_winhttp_error(const char *operation) {
  throw std::runtime_error(winhttp_error_message(operation, GetLastError()));
}

static wstring widen_ascii(const string &value) {
  wstring out;
  out.reserve(value.size());
  for (unsigned char ch : value)
    out.push_back(static_cast<wchar_t>(ch));
  return out;
}

static string narrow_ascii(const wstring &value) {
  string out;
  out.reserve(value.size());
  for (wchar_t ch : value)
    out.push_back(static_cast<char>(ch));
  return out;
}

// ---------- tiny WinHTTP RAII helper ------------------------

struct HInternet {
  HINTERNET h = nullptr;
  ~HInternet() {
    if (h)
      WinHttpCloseHandle(h);
  }
};

struct ProxyMode {
  DWORD access_type;
  LPCWSTR proxy_name;
  LPCWSTR proxy_bypass;
  const char *label;
};

static string https_post_once(const wstring &host, const wstring &path,
                              const wstring &headers, const string &body,
                              const ProxyMode &proxy_mode) {
  HInternet session;
  HInternet connect;
  HInternet request;

  session.h = WinHttpOpen(L"VortexLauncher/1.0", proxy_mode.access_type,
                          proxy_mode.proxy_name, proxy_mode.proxy_bypass, 0);
  if (!session.h)
    throw_winhttp_error("WinHttpOpen");

  WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 15000);

  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
  protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
  WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                   sizeof(protocols));

  connect.h =
      WinHttpConnect(session.h, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!connect.h)
    throw_winhttp_error("WinHttpConnect");

  request.h = WinHttpOpenRequest(
      connect.h, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request.h)
    throw_winhttp_error("WinHttpOpenRequest");

  if (!headers.empty())
    if (!WinHttpAddRequestHeaders(request.h, headers.c_str(),
                                  (DWORD)headers.size(), WINHTTP_ADDREQ_FLAG_ADD))
      throw_winhttp_error("WinHttpAddRequestHeaders");

  BOOL ok = WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               (LPVOID)body.c_str(), (DWORD)body.size(),
                               (DWORD)body.size(), 0);
  if (!ok)
    throw_winhttp_error("WinHttpSendRequest");

  if (!WinHttpReceiveResponse(request.h, nullptr))
    throw_winhttp_error("WinHttpReceiveResponse");

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (WinHttpQueryHeaders(request.h,
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX) &&
      status >= 400) {
    throw std::runtime_error("HTTP " + std::to_string(status) + " from " +
                             narrow_ascii(host) + narrow_ascii(path));
  }

  string result;
  DWORD available = 0;
  while (WinHttpQueryDataAvailable(request.h, &available) && available > 0) {
    string chunk(available, '\0');
    DWORD read = 0;
    WinHttpReadData(request.h, &chunk[0], available, &read);
    chunk.resize(read);
    result += chunk;
  }
  return result;
}

// Send an HTTPS POST and return the response body as a UTF-8 string.
// host    : e.g. L"id.twitch.tv"
// path    : e.g. L"/oauth2/token"
// headers : additional request headers (may be empty)
// body    : UTF-8 POST body
static string https_post(const wstring &host, const wstring &path,
                         const wstring &headers, const string &body) {
  const std::vector<ProxyMode> proxy_modes = {
      {WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
       WINHTTP_NO_PROXY_BYPASS, "automatic proxy"},
      {WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
       WINHTTP_NO_PROXY_BYPASS, "default proxy"},
      {WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
       WINHTTP_NO_PROXY_BYPASS, "direct connection"}};

  string errors;
  for (const ProxyMode &proxy_mode : proxy_modes) {
    try {
      return https_post_once(host, path, headers, body, proxy_mode);
    } catch (const std::exception &e) {
      if (!errors.empty())
        errors += "; ";
      errors += proxy_mode.label;
      errors += ": ";
      errors += e.what();
    }
  }

  throw std::runtime_error(errors);
}

// ---------- token cache -------------------------------------

static string s_access_token; // cached for the process lifetime
static bool s_token_fetch_failed = false;

static bool igdb_fetch_token() {
  const string client_id = get_secret("IGDB_CLIENT_ID");
  const string client_secret = get_secret("IGDB_CLIENT_SECRET");

  if (client_id.empty() || client_secret.empty()) {
    // Warn once. Without this an unconfigured install looks identical to a
    // network failure: metadata silently never resolves.
    static bool warned = false;
    if (!warned) {
      warned = true;
      vlog::line("IGDB", "No credentials. Set IGDB_CLIENT_ID and "
                         "IGDB_CLIENT_SECRET in analytics/.env, or use "
                         "the first-run wizard.");
    }
    return false;
  }
  if (s_token_fetch_failed)
    return false;

  const string body = "client_id=" + client_id +
                      "&client_secret=" + client_secret +
                      "&grant_type=client_credentials";

  string resp;
  try {
    resp = https_post(L"id.twitch.tv", L"/oauth2/token",
                      L"Content-Type: application/x-www-form-urlencoded\r\n"
                      L"Accept: application/json",
                      body);
  } catch (const std::exception &e) {
    s_token_fetch_failed = true;
    // Distinct from a rejected token: this is the request never
    // completing -- no network, DNS, or a proxy in the way.
    vlog::line("IGDB", std::string("Token request to id.twitch.tv failed: ")
                       + e.what());
    return false;
  }

  // Extract "access_token": "<value>" from JSON.
  // Handles both compact (no space) and pretty (space after colon) formats.
  const string key = "\"access_token\"";
  auto pos = resp.find(key);
  if (pos == string::npos) {
    vlog::line("IGDB", "Token rejected by Twitch -- the client id or "
                       "secret is wrong. Response: " + resp);
    return false;
  }
  pos += key.size();
  // skip whitespace, then ':'
  while (pos < resp.size() && resp[pos] == ' ')
    ++pos;
  if (pos >= resp.size() || resp[pos] != ':')
    return false;
  ++pos;
  // skip whitespace, then opening '"'
  while (pos < resp.size() && resp[pos] == ' ')
    ++pos;
  if (pos >= resp.size() || resp[pos] != '"')
    return false;
  ++pos;

  auto end = resp.find('"', pos);
  if (end == string::npos)
    return false;

  s_access_token = resp.substr(pos, end - pos);
  s_token_fetch_failed = false;
  return !s_access_token.empty();
}

// ---------- name extraction ---------------------------------

// Extract one game info object starting the search from position `from`.
// Returns the info and advances `from` past the object. Returns empty string
// for name when no more.
static IgdbGameInfo extract_info_at(const string &json, size_t &from) {
  IgdbGameInfo info;

  auto start_obj = json.find('{', from);
  if (start_obj == string::npos) {
    from = string::npos;
    return info;
  }

  size_t end_obj = start_obj + 1;
  int brace_count = 1;
  bool in_str = false;
  while (end_obj < json.size() && brace_count > 0) {
      if (json[end_obj] == '"' && json[end_obj-1] != '\\') in_str = !in_str;
      if (!in_str) {
          if (json[end_obj] == '{') brace_count++;
          else if (json[end_obj] == '}') brace_count--;
      }
      end_obj++;
  }
  if (brace_count > 0) {
      from = string::npos;
      return info;
  }

  from = end_obj;
  string obj = json.substr(start_obj, end_obj - start_obj);

  string root_obj = obj;
  int depth = 0;
  in_str = false;
  for (size_t i = 1; i + 1 < root_obj.size(); ++i) {
      if (root_obj[i] == '"' && root_obj[i-1] != '\\') in_str = !in_str;
      if (!in_str) {
          if (root_obj[i] == '{' || root_obj[i] == '[') {
              depth++;
              root_obj[i] = ' ';
          }
          else if (root_obj[i] == '}' || root_obj[i] == ']') {
              depth--;
              root_obj[i] = ' ';
          }
          else if (depth > 0) {
              root_obj[i] = ' ';
          }
      } else if (depth > 0) {
          root_obj[i] = ' ';
      }
  }

  // Extract "id"
  auto id_pos = root_obj.find("\"id\"");
  if (id_pos != string::npos) {
    id_pos += 4;
    while (id_pos < root_obj.size() && (root_obj[id_pos] == ' ' || root_obj[id_pos] == ':'))
      id_pos++;
    long long id = 0;
    while (id_pos < root_obj.size() && std::isdigit((unsigned char)root_obj[id_pos])) {
      id = id * 10 + (root_obj[id_pos] - '0');
      id_pos++;
    }
    info.id = id;
  }

  // Extract "name"
  auto name_pos = root_obj.find("\"name\"");
  if (name_pos != string::npos) {
    name_pos += 6;
    while (name_pos < root_obj.size() &&
           (root_obj[name_pos] == ' ' || root_obj[name_pos] == ':'))
      name_pos++;
    if (name_pos < root_obj.size() && root_obj[name_pos] == '"')
      info.name = json_read_string(root_obj, name_pos);
  }

  // Extract "rating"
  auto rating_pos = root_obj.find("\"rating\"");
  if (rating_pos == string::npos) rating_pos = root_obj.find("\"total_rating\"");
  if (rating_pos != string::npos) {
    auto colon = root_obj.find(':', rating_pos);
    if (colon != string::npos) {
        rating_pos = colon + 1;
        while (rating_pos < root_obj.size() && root_obj[rating_pos] == ' ') rating_pos++;
        std::string num;
        while (rating_pos < root_obj.size() && (std::isdigit((unsigned char)root_obj[rating_pos]) || root_obj[rating_pos] == '.')) {
            num += root_obj[rating_pos++];
        }
        if (!num.empty()) {
            try { info.rating = std::stod(num); } catch (...) {}
        }
    }
  }

  // Extract "genres"
  auto genres_pos = obj.find("\"genres\"");
  if (genres_pos != string::npos) {
      size_t curr = genres_pos;
      auto genres_end = obj.find(']', curr);
      if (genres_end == string::npos) genres_end = obj.size();
      while (curr < genres_end) {
          auto g_name_pos = obj.find("\"name\"", curr);
          if (g_name_pos == string::npos || g_name_pos > genres_end) break;
          auto colon = obj.find(':', g_name_pos);
          if (colon != string::npos) {
              size_t val_pos = colon + 1;
              while (val_pos < obj.size() && obj[val_pos] == ' ') val_pos++;
              if (val_pos < obj.size() && obj[val_pos] == '"') {
                  const std::string genre_name = json_read_string(obj, val_pos);
                  if (!genre_name.empty()) {
                      info.genres.push_back(genre_name);
                  }
              }
          }
          curr = g_name_pos + 6;
      }
  }

  // Extract "themes" -- identical shape to "genres" above
  // ("themes":[{"name":"Fantasy"},...]), so this is the same bounded scan.
  // The two searches are independent, so the order IGDB returns fields in does
  // not matter.
  auto themes_pos = obj.find("\"themes\"");
  if (themes_pos != string::npos) {
      size_t curr = themes_pos;
      auto themes_end = obj.find(']', curr);
      if (themes_end == string::npos) themes_end = obj.size();
      while (curr < themes_end) {
          auto t_name_pos = obj.find("\"name\"", curr);
          if (t_name_pos == string::npos || t_name_pos > themes_end) break;
          auto colon = obj.find(':', t_name_pos);
          if (colon != string::npos) {
              size_t val_pos = colon + 1;
              while (val_pos < obj.size() && obj[val_pos] == ' ') val_pos++;
              if (val_pos < obj.size() && obj[val_pos] == '"') {
                  const std::string theme_name = json_read_string(obj, val_pos);
                  if (!theme_name.empty()) {
                      info.themes.push_back(theme_name);
                  }
              }
          }
          curr = t_name_pos + 6;
      }
  }

  // Extract "game_modes" -- same nested-array shape as genres and themes.
  auto modes_pos = obj.find("\"game_modes\"");
  if (modes_pos != string::npos) {
      size_t curr = modes_pos;
      auto modes_end = obj.find(']', curr);
      if (modes_end == string::npos) modes_end = obj.size();
      while (curr < modes_end) {
          auto m_name_pos = obj.find("\"name\"", curr);
          if (m_name_pos == string::npos || m_name_pos > modes_end) break;
          auto colon = obj.find(':', m_name_pos);
          if (colon != string::npos) {
              size_t val_pos = colon + 1;
              while (val_pos < obj.size() && obj[val_pos] == ' ') val_pos++;
              if (val_pos < obj.size() && obj[val_pos] == '"') {
                  const std::string mode_name = json_read_string(obj, val_pos);
                  if (!mode_name.empty()) {
                      info.game_modes.push_back(mode_name);
                  }
              }
          }
          curr = m_name_pos + 6;
      }
  }

  // Extract "time_to_beat"
  auto ttb_pos = obj.find("\"game_time_to_beats\"");
  if (ttb_pos != string::npos) {
      auto normally_pos = obj.find("\"normally\"", ttb_pos);
      if (normally_pos != string::npos) {
          auto colon = obj.find(':', normally_pos);
          if (colon != string::npos) {
              normally_pos = colon + 1;
              while (normally_pos < obj.size() && obj[normally_pos] == ' ') normally_pos++;
              std::string num;
              while (normally_pos < obj.size() && std::isdigit((unsigned char)obj[normally_pos])) {
                  num += obj[normally_pos++];
              }
              if (!num.empty()) {
                  try { info.time_to_beat_seconds = std::stoll(num); } catch (...) {}
              }
          }
      }
  }

  // Extract "developer"
  auto comp_pos = obj.find("\"involved_companies\"");
  if (comp_pos != string::npos) {
      size_t curr = comp_pos;
      while (true) {
          auto dev_flag = obj.find("\"developer\"", curr);
          if (dev_flag == string::npos) break;
          auto val_pos = obj.find(':', dev_flag);
          if (val_pos != string::npos) {
              val_pos++;
              while (val_pos < obj.size() && obj[val_pos] == ' ') val_pos++;
              if (val_pos + 4 <= obj.size() && obj.substr(val_pos, 4) == "true") {
                  auto comp_name = obj.rfind("\"name\"", dev_flag);
                  if (comp_name != string::npos && comp_name > comp_pos) {
                      auto colon = obj.find(':', comp_name);
                      if (colon != string::npos) {
                          comp_name = colon + 1;
                          while (comp_name < obj.size() && obj[comp_name] == ' ') comp_name++;
                          if (comp_name < obj.size() && obj[comp_name] == '"') {
                              info.developer = json_read_string(obj, comp_name);
                              break;
                          }
                      }
                  }
              }
          }
          curr = dev_flag + 11;
      }
  }

  return info;
}

// Every lookup asks for the same shape, so extract_info_at() can parse any of
// them and the recommendation pipeline gets identical genre data no matter
// which pass resolved the game.
static const char *const IGDB_FIELDS =
    "fields id, name, total_rating, involved_companies.company.name, "
    "involved_companies.developer, genres.name, themes.name, game_modes.name;";

// Reduce a store name to bare words for IGDB's fuzzy `search`.
//
// IGDB normalises punctuation and carries no decorative brackets, so a literal
// name match against a store title can fail even when IGDB holds the game:
// Steam's "《Drifting : Weight of Feathers》" never matches "Drifting: Weight of
// Feathers", and stripping only the brackets is not enough because Steam writes
// " : " where IGDB writes ": ". Dropping every non-alphanumeric byte sidesteps
// all of it. Non-ASCII bytes go too, so a fully non-Latin title reduces to an
// empty string -- callers must skip the attempt in that case.
static string search_friendly(const string &name) {
  string out;
  bool gap = false;
  for (unsigned char c : name) {
    if (std::isalnum(c)) {
      if (gap && !out.empty())
        out.push_back(' ');
      gap = false;
      out.push_back(static_cast<char>(c));
    } else {
      gap = true;
    }
  }
  return out;
}

// Walk all "name" fields in the response.
// Priority:
//   1. Canonical exact match  (alphanumeric-only, lowercase)
//   2. First result as fallback
static IgdbGameInfo extract_best_info(const string &json, const string &query) {
  const string queryCanon = make_canonical(query);
  IgdbGameInfo first;
  size_t cursor = 0;

  while (cursor != string::npos) {
    IgdbGameInfo info = extract_info_at(json, cursor);
    if (info.name.empty())
      break;
    if (first.name.empty())
      first = info;
    if (make_canonical(info.name) == queryCanon)
      return info; // canonical match — wins immediately
  }
  return first; // no canonical match — best guess is first result
}

// ---------- Cache -------------------------------------------

static std::unordered_map<std::string, IgdbGameInfo> s_igdb_cache;
static bool s_cache_loaded = false;
static const std::filesystem::path IGDB_CACHE_FILE = app_data_path("igdb_cache.txt");

static void load_igdb_cache() {
  if (s_cache_loaded)
    return;
  s_cache_loaded = true;

  if (!std::filesystem::exists(IGDB_CACHE_FILE))
    return;

  std::ifstream file(IGDB_CACHE_FILE);
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;

    auto eq_pos = line.find('=');
    if (eq_pos != std::string::npos) {
      std::string query = line.substr(0, eq_pos);
      std::string rest = line.substr(eq_pos + 1);

      auto pipe_pos = rest.find('|');
      if (pipe_pos != std::string::npos) {
        try {
          IgdbGameInfo info;
          info.id = std::stoll(rest.substr(0, pipe_pos));
          // Same repair as game_metadata.txt: names cached before
          // json_read_string() existed carry the dropped-escape spelling.
          info.name = json_repair_dropped_escapes(rest.substr(pipe_pos + 1));
          // id 0 is a remembered miss, not a corrupt line. Loading it is what
          // stops a game IGDB does not have from costing a lookup every launch.
          //
          // A real id always beats a remembered miss, whatever order the lines
          // appear in: the file is append-only, so a game that missed once and
          // resolved later has both entries in it.
          auto existing = s_igdb_cache.find(query);
          if (existing == s_igdb_cache.end() || info.id > 0)
            s_igdb_cache[query] = info;
        } catch (...) {
        }
      }
    }
  }
}

// allowNegative controls whether a miss (id 0) is remembered.
//
// Only pass true when IGDB actually answered and had nothing -- that is a fact
// about the catalogue and is worth caching, since otherwise the game costs a
// full round trip on every launch forever. A lookup that failed because the
// network or the API was down must stay retryable; persisting that would let a
// single bad run mark the game unresolvable permanently.
static void save_to_cache(const std::string &query, const IgdbGameInfo &info,
                          bool allowNegative = false) {
  if (info.id <= 0 && !allowNegative)
    return;

  s_igdb_cache[query] = info;
  std::ofstream file(IGDB_CACHE_FILE, std::ios::app);
  if (file.is_open()) {
    file.seekp(0, std::ios::end);
    if (file.tellp() == 0) {
      file << "# IGDB Resolution Cache\n";
      file << "# Format: SEARCH_QUERY=IGDB_ID|CANONICAL_NAME\n";
    }
    file << query << "=" << info.id << "|" << info.name << "\n";
  }

  if (info.id <= 0)
    return; // nothing to hang metadata off

  GameMetadata meta;
  meta.igdb_id = info.id;
  meta.developer = info.developer;
  meta.rating = info.rating;
  meta.time_to_beat_seconds = info.time_to_beat_seconds;
  meta.all_genres = info.genres;
  meta.themes = info.themes;
  meta.game_modes = info.game_modes;

  string ttb_body = "where game_id = " + std::to_string(info.id) + "; fields normally;";
  try {
      string ttb_resp = https_post(L"api.igdb.com", L"/v4/game_time_to_beats",
          L"Client-ID: " + widen_ascii(get_secret("IGDB_CLIENT_ID")) + L"\r\n" +
          L"Authorization: Bearer " + widen_ascii(s_access_token) + L"\r\n" +
          L"Content-Type: text/plain", ttb_body);

      auto norm_pos = ttb_resp.find("\"normally\"");
      if (norm_pos != string::npos) {
          auto colon = ttb_resp.find(':', norm_pos);
          if (colon != string::npos) {
              size_t val_pos = colon + 1;
              while (val_pos < ttb_resp.size() && ttb_resp[val_pos] == ' ') val_pos++;
              string num;
              while (val_pos < ttb_resp.size() && std::isdigit((unsigned char)ttb_resp[val_pos])) {
                  num += ttb_resp[val_pos++];
              }
              if (!num.empty()) {
                  try { meta.time_to_beat_seconds = std::stoll(num); } catch (...) {}
              }
          }
      }
  } catch (...) {}

  save_game_metadata(meta);
}

// ---------- public API --------------------------------------

// Scan for the first three-digit number that looks like an HTTP status, so
// both "HTTP 400 from id.twitch.tv/..." and "HTTP Error 401" are understood.
int http_status_from_error(const string &message) {
  for (size_t i = 0; i + 2 < message.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(message[i]))) continue;

    // Must be exactly three digits, not part of a longer number.
    if (!std::isdigit(static_cast<unsigned char>(message[i + 1])) ||
        !std::isdigit(static_cast<unsigned char>(message[i + 2])))
      continue;
    if (i + 3 < message.size() &&
        std::isdigit(static_cast<unsigned char>(message[i + 3])))
      continue;
    if (i > 0 && std::isdigit(static_cast<unsigned char>(message[i - 1])))
      continue;

    const int value = std::stoi(message.substr(i, 3));
    if (value >= 100 && value <= 599) return value;
  }
  return 0;
}

// ---------- credential probe --------------------------------------------

// Whether the last real IGDB call in this process authenticated. Starts true
// so a launcher that has not called IGDB yet does not accuse the user of
// having bad keys.
static bool s_last_auth_ok = true;

bool igdb_last_auth_ok() { return s_last_auth_ok; }

CredentialCheck igdb_probe_credentials(const std::string &clientId,
                                       const std::string &clientSecret) {
  CredentialCheck result;

  if (clientId.empty() || clientSecret.empty()) {
    result.detail = "Both the client ID and the client secret are needed.";
    return result;
  }

  const string body = "client_id=" + clientId +
                      "&client_secret=" + clientSecret +
                      "&grant_type=client_credentials";

  string resp;
  try {
    resp = https_post(L"id.twitch.tv", L"/oauth2/token",
                      L"Content-Type: application/x-www-form-urlencoded\r\n"
                      L"Accept: application/json",
                      body);
  } catch (const std::exception &e) {
    // A 4xx is Twitch answering; anything else is not reaching it. The two
    // need opposite advice, so they must not collapse into one message.
    const string what = e.what();
    const int status = http_status_from_error(what);
    if (status >= 400 && status < 500) {
      result.rejected = true;
      result.detail = "Twitch rejected these credentials. Generating a new "
                      "secret in the Twitch console invalidates the old one, "
                      "so make sure this is the current secret for this "
                      "client ID.";
    } else {
      result.detail = "Could not reach Twitch to check. " + what;
    }
    return result;
  }

  if (resp.find("\"access_token\"") == string::npos) {
    result.rejected = true;
    result.detail = "Twitch replied but issued no token; the credentials look "
                    "wrong.";
    return result;
  }

  result.ok = true;
  result.detail = "Credentials accepted.";
  return result;
}

long long igdb_cached_id_for(const std::string &name) {
  load_igdb_cache();

  auto it = s_igdb_cache.find(name);
  if (it != s_igdb_cache.end() && it->second.id > 0)
    return it->second.id;

  // The cache is keyed by whatever string was searched -- a folder name for a
  // local game, a store name for a Steam one -- so an exact hit is the lucky
  // case. Match on the canonical form as well, against both the key and the
  // resolved name, exactly as findGameByName() does in the bridge.
  const string canon = make_canonical(name);
  if (canon.empty())
    return 0;

  for (const auto &entry : s_igdb_cache) {
    if (entry.second.id <= 0)
      continue;
    if (make_canonical(entry.first) == canon ||
        make_canonical(entry.second.name) == canon)
      return entry.second.id;
  }
  return 0;
}

IgdbGameInfo igdb_resolve_game(const std::string &folderName,
                               bool interactive, long long steamAppId) {
  load_igdb_cache();

  // Check cache first
  auto it = s_igdb_cache.find(folderName);
  if (it != s_igdb_cache.end()) {
    vlog::item("IGDB", folderName, vlog::Status::Cached,
               it->second.id > 0 ? ("id " + std::to_string(it->second.id))
                                 : "previously unmatched");
    return it->second;
  }

  IgdbGameInfo fallback;
  fallback.name = folderName;

  // Guard: no credentials
  const string client_id = get_secret("IGDB_CLIENT_ID");
  if (client_id.empty()) {
    vlog::item("IGDB", folderName, vlog::Status::Skipped,
               "no IGDB credentials -- set IGDB_CLIENT_ID in analytics/.env");
    save_to_cache(folderName, fallback);
    return fallback;
  }

  // Fetch token once per process
  if (s_access_token.empty()) {
    if (!igdb_fetch_token()) {
      s_last_auth_ok = false;
      // Previously returned with no log line at all, which is why a machine
      // with bad credentials produced a phase header for sixteen games and
      // then absolute silence. Every other outcome in this function reports
      // itself; this one has to as well.
      vlog::item("IGDB", folderName, vlog::Status::Fail,
                 "IGDB authentication unavailable");
      return fallback;
    }
  }

  // Build query body (Apicalypse syntax)
  // Escape double-quotes inside folderName just in case
  string safe = folderName;
  for (size_t i = 0; i < safe.size(); ++i)
    if (safe[i] == '"') {
      safe.insert(i, "\\");
      ++i;
    }

  // Build headers
  const wstring headers =
      L"Client-ID: " + widen_ascii(client_id) + L"\r\n" +
      L"Authorization: Bearer " + widen_ascii(s_access_token) + L"\r\n" +
      L"Content-Type: text/plain";

  // Tracks whether IGDB ever answered. Only a real answer justifies caching a
  // miss -- see save_to_cache().
  bool apiAnswered = false;
  IgdbGameInfo resolved;

  // Zeroth pass: Steam application id.
  //
  // IGDB maps store ids in external_games, which makes this exact and immune to
  // the title decoration, punctuation and localisation that defeat name search.
  // Note the field is external_game_source; the older `category` field no
  // longer matches (it returns nothing for source 1 / Steam).
  if (steamAppId > 0) {
    const string appid_body =
        "where external_games.external_game_source = 1 & external_games.uid = \"" +
        std::to_string(steamAppId) + "\"; " + IGDB_FIELDS + " limit 10;";
    try {
      const string appid_resp =
          https_post(L"api.igdb.com", L"/v4/games", headers, appid_body);
      apiAnswered = true;
      resolved = extract_best_info(appid_resp, folderName);
    } catch (const std::exception &e) {
      vlog::item("IGDB", folderName, vlog::Status::Fail,
                 std::string("appid lookup failed: ") + e.what());
    }
  }

  // First pass: Case-insensitive substring match.
  // This bypasses IGDB's popularity bias which buries exact generic matches (like "The Finals") under blockbusters (like "Final Fantasy").
  if (resolved.name.empty()) {
    const string name_body =
        "where name ~ *\"" + safe + "\"*; " + IGDB_FIELDS + " limit 50;";
    try {
      const string name_resp =
          https_post(L"api.igdb.com", L"/v4/games", headers, name_body);
      apiAnswered = true;
      resolved = extract_best_info(name_resp, folderName);
    } catch (const std::exception &e) {
      vlog::item("IGDB", folderName, vlog::Status::Fail,
                 std::string("name search failed: ") + e.what());
    }
  }

  // Second pass: IGDB's fuzzy `search` over the name reduced to bare words.
  // search_friendly() returns empty for a fully non-Latin title, and its
  // comment is explicit that callers must skip the attempt in that case --
  // searching for "" matches the whole catalogue.
  if (resolved.name.empty()) {
    const string cleaned = search_friendly(folderName);
    if (!cleaned.empty()) {
      const string cleaned_body =
          "search \"" + cleaned + "\"; " + IGDB_FIELDS + " limit 10;";
      try {
        const string cleaned_resp =
            https_post(L"api.igdb.com", L"/v4/games", headers, cleaned_body);
        apiAnswered = true;
        resolved = extract_best_info(cleaned_resp, folderName);
      } catch (...) {}
    }
  }

  if (resolved.name.empty()) {
    if (interactive) {
      cout << "[IGDB] No match found for \"" << folderName
           << "\", keeping folder name.\n";
    }
    vlog::item("IGDB", folderName, vlog::Status::Skipped,
               apiAnswered ? "no match, keeping folder name"
                           : "IGDB never answered, will retry next scan");
    save_to_cache(folderName, fallback, apiAnswered);
    return fallback;
  }

  s_last_auth_ok = true;
  vlog::item("IGDB", folderName, vlog::Status::Ok,
             "-> " + resolved.name + " (id " + std::to_string(resolved.id) + ")");
  save_to_cache(folderName, resolved);
  return resolved;
}
