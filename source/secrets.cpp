#include "secrets.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#include "app_paths.h"

namespace fs = std::filesystem;

namespace {

std::mutex g_mutex;
bool g_loaded = false;
std::map<std::string, std::string> g_values;

std::string trim(const std::string &value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

// Accept KEY=value, KEY="value" and KEY='value'. Anything else is ignored
// rather than guessed at.
std::string unquote(const std::string &value) {
  if (value.size() >= 2) {
    const char front = value.front();
    if ((front == '"' || front == '\'') && value.back() == front)
      return value.substr(1, value.size() - 2);
  }
  return value;
}

bool parse_file(const fs::path &path) {
  std::error_code ec;
  if (!fs::exists(path, ec) || ec)
    return false;

  std::ifstream file(path);
  if (!file.is_open())
    return false;

  std::string line;
  while (std::getline(file, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#')
      continue;

    const auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;

    const std::string key = trim(line.substr(0, eq));
    if (key.empty())
      continue;

    // First file to define a key wins, matching the documented search order.
    g_values.emplace(key, unquote(trim(line.substr(eq + 1))));
  }
  return true;
}

// Caller holds g_mutex.
void load_locked() {
  g_values.clear();
  g_loaded = true;

  if (const char *override_path = std::getenv("VORTEX_ENV_FILE")) {
    if (parse_file(fs::path(override_path)))
      return;
  }
  if (parse_file(app_data_path("analytics/.env")))
    return;
  parse_file(app_data_path(".env"));
}

} // namespace

std::string get_secret(const std::string &key, const std::string &fallback) {
  std::lock_guard<std::mutex> guard(g_mutex);
  if (!g_loaded)
    load_locked();

  const auto it = g_values.find(key);
  if (it == g_values.end() || it->second.empty())
    return fallback;
  return it->second;
}

bool has_secret(const std::string &key) { return !get_secret(key).empty(); }

void reload_secrets() {
  std::lock_guard<std::mutex> guard(g_mutex);
  load_locked();
}
