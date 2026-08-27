#include "steamgriddb_manager.h"
#include "json_text.h"
#include "game_manager.h"

#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

using std::string;
using std::wstring;
namespace fs = std::filesystem;

// Read at runtime from analytics/.env — set STEAMGRIDDB_API_KEY there. The key
// that used to be compiled in here was committed to source control and had to
// be regenerated at steamgriddb.com; storing it outside the binary also means
// replacing it never needs a rebuild.
#include "secrets.h"
#include "vortex_log.h"

static wstring widen_ascii(const string &value) {
  wstring out;
  out.reserve(value.size());
  for (unsigned char ch : value)
    out.push_back(static_cast<wchar_t>(ch));
  return out;
}

#ifdef _WIN32
struct HInternet {
  HINTERNET h = nullptr;
  ~HInternet() {
    if (h)
      WinHttpCloseHandle(h);
  }
};
#endif

static string https_get(const wstring &host, const wstring &path,
                        const wstring &headers) {
#ifdef _WIN32
  HInternet session;
  HInternet connect;
  HInternet request;

  session.h =
      WinHttpOpen(L"VortexLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session.h)
    throw std::runtime_error("WinHttpOpen failed");

  connect.h =
      WinHttpConnect(session.h, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!connect.h)
    throw std::runtime_error("WinHttpConnect failed");

  request.h = WinHttpOpenRequest(
      connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request.h)
    throw std::runtime_error("WinHttpOpenRequest failed");

  if (!headers.empty()) {
    WinHttpAddRequestHeaders(request.h, headers.c_str(), (DWORD)headers.size(),
                             WINHTTP_ADDREQ_FLAG_ADD);
  }

  BOOL ok = WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
  if (!ok)
    throw std::runtime_error("WinHttpSendRequest failed");

  if (!WinHttpReceiveResponse(request.h, nullptr))
    throw std::runtime_error("WinHttpReceiveResponse failed");

  DWORD statusCode = 0;
  DWORD statusCodeSize = sizeof(statusCode);
  WinHttpQueryHeaders(request.h,
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &statusCode,
                      &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
  if (statusCode != 200) {
    throw std::runtime_error("HTTP Error " + std::to_string(statusCode));
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
#else
  return "{}";
#endif
}

static bool download_file(const string &url, const fs::path &dest_path) {
#ifdef _WIN32
  if (url.substr(0, 8) != "https://")
    return false;
  size_t host_end = url.find('/', 8);
  if (host_end == string::npos)
    return false;

  string host_str = url.substr(8, host_end - 8);
  string path_str = url.substr(host_end);

  wstring host = widen_ascii(host_str);
  wstring path = widen_ascii(path_str);

  try {
    string data = https_get(host, path, L"");
    if (data.empty())
      return false;

    std::ofstream out(dest_path, std::ios::binary);
    if (!out)
      return false;
    out.write(data.data(), data.size());
    return true;
  } catch (...) {
    return false;
  }
#else
  return true;
#endif
}

static string extract_json_value(const string &json, const string &key,
                                 bool is_number = false) {
  string search_key = "\"" + key + "\":";
  size_t pos = json.find(search_key);
  if (pos == string::npos)
    return "";
  pos += search_key.length();

  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                               json[pos] == '\n' || json[pos] == '\r'))
    pos++;

  if (is_number) {
    size_t end = pos;
    while (end < json.size() && std::isdigit((unsigned char)json[end]))
      end++;
    return json.substr(pos, end - pos);
  } else {
    if (pos < json.size() && json[pos] == '"') {
      // Every path separator in an image URL arrives as \/ , and a \u in a game
      // name used to reach the folder this result names.
      return json_read_string(json, pos);
    }
  }
  return "";
}

static string url_encode(const string &value) {
  string escaped;
  for (char c : value) {
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      escaped += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      escaped += buf;
    }
  }
  return escaped;
}

static bool is_dir_empty(const fs::path &p) {
  if (!fs::exists(p) || !fs::is_directory(p))
    return true;
  std::error_code ec;
  return fs::is_empty(p, ec);
}

std::string steamgriddb_image_folder_name(const std::string &game_name) {
  string safe_name = game_name;
  for (char &c : safe_name) {
    if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
        c == '\\' || c == '|' || c == '?' || c == '*') {
      c = '_';
    }
  }
  return safe_name + "_img";
}

int delete_steamgriddb_images(const std::vector<std::string> &game_names,
                              const std::vector<std::string> &keep_names,
                              const std::string &images_root) {
  std::set<string> keep;
  for (const string &name : keep_names)
    keep.insert(to_lower(name));

  fs::path images_root_path(images_root);
  int deleted = 0;

  for (const string &name : game_names) {
    if (keep.count(to_lower(name)))
      continue;   // still owned by another folder or by Steam

    fs::path game_img_dir = images_root_path / steamgriddb_image_folder_name(name);
    std::error_code ec;
    if (!fs::exists(game_img_dir, ec))
      continue;

    fs::remove_all(game_img_dir, ec);
    if (ec) {
      std::cerr << "[SteamGridDB] Could not delete " << game_img_dir.string()
                << ": " << ec.message() << "\n";
    } else {
      ++deleted;
    }
  }

  return deleted;
}

// SteamGridDB genuinely has no logo or hero for some games, and no match at all
// for others. An empty folder cannot tell "nothing exists upstream" apart from
// "never fetched", so without a record those games re-query the API on every
// scan forever. Each query that comes back empty stamps the time here and the
// query is skipped until the stamp ages out, so art uploaded upstream later is
// still picked up eventually.
//
// Only empty *answers* are stamped. A network or HTTP failure throws and leaves
// the stamp untouched, so launching while offline does not write a game off.
static constexpr std::time_t kNegativeCacheSeconds = 7 * 24 * 60 * 60; // 7 days
static const char *const kStateFileName = ".sgdb_state";

struct SgdbState {
  std::time_t search = 0; // last search that matched no game
  std::time_t grid = 0;   // last query that yielded no usable grid
  std::time_t logo = 0;
  std::time_t hero = 0;
};

// A stamp in the future (the clock moved backwards between runs) counts as
// stale, so a bad clock cannot block a game's artwork indefinitely.
static bool attempt_is_fresh(std::time_t stamp, std::time_t now) {
  return stamp > 0 && now >= stamp && (now - stamp) < kNegativeCacheSeconds;
}

static SgdbState load_sgdb_state(const fs::path &game_img_dir) {
  SgdbState state;
  std::ifstream in(game_img_dir / kStateFileName);
  if (!in)
    return state;

  string line;
  while (std::getline(in, line)) {
    size_t eq = line.find('=');
    if (eq == string::npos)
      continue;
    string key = line.substr(0, eq);
    std::time_t value = 0;
    try {
      value = static_cast<std::time_t>(std::stoll(line.substr(eq + 1)));
    } catch (...) {
      continue; // unparsable stamp: treat as never attempted
    }

    if (key == "search")
      state.search = value;
    else if (key == "grid")
      state.grid = value;
    else if (key == "logo")
      state.logo = value;
    else if (key == "hero")
      state.hero = value;
  }
  return state;
}

static void save_sgdb_state(const fs::path &game_img_dir,
                            const SgdbState &state) {
  std::error_code ec;
  fs::create_directories(game_img_dir, ec);
  if (ec)
    return;

  std::ofstream out(game_img_dir / kStateFileName, std::ios::trunc);
  if (!out)
    return;
  out << "search=" << static_cast<long long>(state.search) << "\n"
      << "grid=" << static_cast<long long>(state.grid) << "\n"
      << "logo=" << static_cast<long long>(state.logo) << "\n"
      << "hero=" << static_cast<long long>(state.hero) << "\n";
}

// ---------- credential probe --------------------------------------------

// Whether the last real SteamGridDB request authenticated. Optimistic until
// something says otherwise, so a launcher that has not fetched art yet does
// not report a problem it has no evidence for.
static bool s_last_auth_ok = true;

bool steamgriddb_last_auth_ok() { return s_last_auth_ok; }

CredentialCheck steamgriddb_probe_key(const std::string &api_key) {
  CredentialCheck result;

  if (api_key.empty() || api_key == "YOUR_API_KEY_HERE") {
    result.detail = "No SteamGridDB key entered.";
    return result;
  }

  const wstring headers = L"Authorization: Bearer " + widen_ascii(api_key);

  try {
    // Any real search will do; the point is the Authorization header, not the
    // answer. A game that matches nothing still returns 200 with an empty
    // list, which is a pass -- only the HTTP status is being tested.
    https_get(L"www.steamgriddb.com",
              widen_ascii("/api/v2/search/autocomplete/" + url_encode("portal")),
              headers);
  } catch (const std::exception &e) {
    const string what = e.what();
    const int status = http_status_from_error(what);
    if (status == 401 || status == 403) {
      result.rejected = true;
      result.detail = "SteamGridDB rejected this key. Copy it again from "
                      "steamgriddb.com/profile/preferences/api.";
    } else if (status == 429) {
      // Not a bad key, and telling someone to re-enter a working one would
      // send them chasing the wrong problem.
      result.detail = "SteamGridDB is rate limiting right now; the key itself "
                      "looks fine. Try again shortly.";
    } else {
      result.detail = "Could not reach SteamGridDB to check. " + what;
    }
    return result;
  }

  result.ok = true;
  result.detail = "Key accepted.";
  return result;
}

void ensure_steamgriddb_images(const std::vector<std::string> &game_names,
                               const std::string &images_root,
                               const SgdbProgressFn &on_progress) {
  const int total = static_cast<int>(game_names.size());
  const auto started = std::chrono::steady_clock::now();
  int n_ok = 0, n_cached = 0, n_skipped = 0, n_failed = 0;

  // Reported even when the phase does nothing, so the caller's progress
  // indicator advances to completion instead of stalling at zero.
  auto report = [&](int done, const std::string &name) {
    if (on_progress) on_progress(done, total, name);
  };

  vlog::phase("Artwork (SteamGridDB)", total);

  fs::path images_root_path(images_root);
  string api_key = get_secret("STEAMGRIDDB_API_KEY");
  if (api_key.empty() || api_key == "YOUR_API_KEY_HERE") {
    // Previously a single message and an immediate return, which on the
    // console looked identical to "there was nothing to fetch".
    s_last_auth_ok = false;
    vlog::line("SteamGridDB",
               "No API key -- set STEAMGRIDDB_API_KEY in analytics/.env. "
               "Skipping artwork for all " + std::to_string(total) + " games.");
    vlog::phase_done("Artwork (SteamGridDB)", 0.0, 0, 0, total, 0);
    report(total, "");
    return;
  }

  wstring headers = L"Authorization: Bearer " + widen_ascii(api_key);
  std::time_t now = std::time(nullptr);

  int index = 0;
  for (const auto &name : game_names) {
    report(index, name);
    ++index;

    fs::path game_img_dir = images_root_path / steamgriddb_image_folder_name(name);
    fs::path grid_dir = game_img_dir / "grid";
    fs::path logo_dir = game_img_dir / "logo";
    fs::path hero_dir = game_img_dir / "hero";

    SgdbState state = load_sgdb_state(game_img_dir);
    bool state_dirty = false;
    std::vector<std::string> downloaded;

    // An empty folder is only worth retrying once its last empty answer has
    // aged out.
    bool need_grid = is_dir_empty(grid_dir) && !attempt_is_fresh(state.grid, now);
    bool need_logo = is_dir_empty(logo_dir) && !attempt_is_fresh(state.logo, now);
    bool need_hero = is_dir_empty(hero_dir) && !attempt_is_fresh(state.hero, now);

    if (!need_grid && !need_logo && !need_hero) {
      ++n_cached;
      vlog::item("SteamGridDB", name, vlog::Status::Cached, "artwork already present");
      continue; // Every folder either has content or was recently found empty
    }
    if (attempt_is_fresh(state.search, now)) {
      ++n_skipped;
      vlog::item("SteamGridDB", name, vlog::Status::Skipped,
                 "matched nothing recently, not re-searching");
      continue; // The game itself matched nothing recently; do not re-search
    }

    string search_url_path = "/api/v2/search/autocomplete/" + url_encode(name);
    string search_resp;
    try {
      search_resp = https_get(
          L"www.steamgriddb.com",
          widen_ascii(search_url_path), headers);
    } catch (const std::exception &e) {
      ++n_failed;
      // https_get throws "HTTP Error <status>" for anything that is not a 200,
      // so e.what() says whether this is a bad API key (401), a rate limit
      // (429) or a transport error. Discarding it left every cause looking the
      // same, and a 401/403 has to be distinguishable from a game that simply
      // is not on SteamGridDB.
      const std::string what = e.what();
      const int status = http_status_from_error(what);
      if (status == 401 || status == 403)
        s_last_auth_ok = false;
      vlog::item("SteamGridDB", name, vlog::Status::Fail,
                 std::string("search failed: ") + what);
      continue; // Transport failure, not an empty answer: retry next scan
    }

    string id_str = extract_json_value(search_resp, "id", true);
    if (id_str.empty()) {
      ++n_skipped;
      vlog::item("SteamGridDB", name, vlog::Status::Skipped, "no match on SteamGridDB");
      state.search = now;
      save_sgdb_state(game_img_dir, state);
      continue;
    }
    if (state.search != 0) {
      state.search = 0; // the game exists upstream after all
      state_dirty = true;
    }

    fs::create_directories(grid_dir);
    fs::create_directories(logo_dir);
    fs::create_directories(hero_dir);

    if (need_grid) {
      string grids_path =
          "/api/v2/grids/game/" + id_str + "?dimensions=600x900";
      try {
        string grids_resp =
            https_get(L"www.steamgriddb.com",
                      widen_ascii(grids_path), headers);
        string grid_url = extract_json_value(grids_resp, "url");
        bool got_grid = false;
        if (!grid_url.empty()) {
          string ext = ".png";
          if (grid_url.find(".jpg") != string::npos)
            ext = ".jpg";
          else if (grid_url.find(".jpeg") != string::npos)
            ext = ".jpeg";

          fs::path file_path = grid_dir / ("grid" + ext);
          if (download_file(grid_url, file_path)) {
            downloaded.push_back("grid");
            got_grid = true;
          }
        }
        state.grid = got_grid ? 0 : now;
        state_dirty = true;
      } catch (const std::exception &e) {
        // Transport failure: leave the stamp alone so the next scan retries,
        // but say what happened -- silence here made a failed download look
        // identical to a game that simply has no grid art.
        vlog::item("SteamGridDB", name, vlog::Status::Fail,
                   std::string("grid download failed: ") + e.what());
      }
    }

    if (need_logo) {
      string logos_path = "/api/v2/logos/game/" + id_str;
      try {
        string logos_resp =
            https_get(L"www.steamgriddb.com",
                      widen_ascii(logos_path), headers);
        string logo_url = extract_json_value(logos_resp, "url");
        bool got_logo = false;
        if (!logo_url.empty()) {
          string ext = ".png";
          if (logo_url.find(".jpg") != string::npos)
            ext = ".jpg";
          fs::path file_path = logo_dir / ("logo" + ext);
          if (download_file(logo_url, file_path)) {
            downloaded.push_back("logo");
            got_logo = true;
          }
        }
        state.logo = got_logo ? 0 : now;
        state_dirty = true;
      } catch (const std::exception &e) {
        // Transport failure: leave the stamp alone so the next scan retries,
        // but say what happened -- silence here made a failed download look
        // identical to a game that simply has no logo art.
        vlog::item("SteamGridDB", name, vlog::Status::Fail,
                   std::string("logo download failed: ") + e.what());
      }
    }

    if (need_hero) {
      string heroes_path = "/api/v2/heroes/game/" + id_str;
      try {
        string heroes_resp =
            https_get(L"www.steamgriddb.com",
                      widen_ascii(heroes_path), headers);
        string hero_url = extract_json_value(heroes_resp, "url");
        bool got_hero = false;
        if (!hero_url.empty()) {
          string ext = ".png";
          if (hero_url.find(".jpg") != string::npos)
            ext = ".jpg";
          fs::path file_path = hero_dir / ("hero" + ext);
          if (download_file(hero_url, file_path)) {
            downloaded.push_back("hero");
            got_hero = true;
          }
        }
        state.hero = got_hero ? 0 : now;
        state_dirty = true;
      } catch (const std::exception &e) {
        // Transport failure: leave the stamp alone so the next scan retries,
        // but say what happened -- silence here made a failed download look
        // identical to a game that simply has no hero art.
        vlog::item("SteamGridDB", name, vlog::Status::Fail,
                   std::string("hero download failed: ") + e.what());
      }
    }

    if (state_dirty)
      save_sgdb_state(game_img_dir, state);

    if (downloaded.empty()) {
      ++n_skipped;
      vlog::item("SteamGridDB", name, vlog::Status::Skipped, "no usable art returned");
    } else {
      ++n_ok;
      s_last_auth_ok = true;
      std::string what;
      for (size_t i = 0; i < downloaded.size(); ++i) {
        if (i) what += " + ";
        what += downloaded[i];
      }
      vlog::item("SteamGridDB", name, vlog::Status::Ok, "downloaded " + what);
    }
  }

  report(total, "");
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  vlog::phase_done("Artwork (SteamGridDB)", elapsed, n_ok, n_cached, n_skipped, n_failed);
}
