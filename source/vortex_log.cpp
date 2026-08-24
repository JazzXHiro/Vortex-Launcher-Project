#include "vortex_log.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

#include "app_paths.h"
#include "secrets.h"

namespace vlog {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
std::string g_path;
bool g_initialised = false;

// Past this the log is truncated at startup rather than appended to. Large
// enough to hold several full cold-cache scans, small enough to mail.
constexpr std::uintmax_t kMaxLogBytes = 4 * 1024 * 1024;

std::string now_formatted(const char *format) {
    const std::time_t t =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream out;
    out << std::put_time(&tm, format);
    return out.str();
}

std::string timestamp() { return now_formatted("%H:%M:%S"); }

std::string datestamp() { return now_formatted("%Y-%m-%d %H:%M:%S"); }

const char *status_text(Status status) {
    switch (status) {
        case Status::Ok:      return "OK     ";
        case Status::Cached:  return "CACHED ";
        case Status::Skipped: return "SKIPPED";
        case Status::Fail:    return "FAIL   ";
    }
    return "       ";
}

// Replace any credential value with "***".
//
// A rejected IGDB key once produced a Python traceback carrying the whole
// token URL, secret included, straight into vortex.log -- which is a file
// users are asked to send to someone else when something breaks. The specific
// path is fixed at the source (igdb_catalog.py posts a body now), but a log
// is exactly the wrong place to rely on having thought of every path, so
// everything written passes through here.
//
// Short values are ignored: a two-character "secret" would turn every stray
// occurrence of those characters into ***, and anything that short is not a
// real key.
std::string redact(std::string text) {
    static const char *const kSecretKeys[] = {
        "IGDB_CLIENT_SECRET", "IGDB_CLIENT_ID", "STEAMGRIDDB_API_KEY",
    };

    for (const char *key : kSecretKeys) {
        const std::string value = get_secret(key);
        if (value.size() < 8) continue;

        for (std::string::size_type at = text.find(value);
             at != std::string::npos;
             at = text.find(value, at + 3)) {
            text.replace(at, value.size(), "***");
        }
    }
    return text;
}

// Caller holds g_mutex.
void emit_locked(const std::string &raw) {
    const std::string text = redact(raw);

    std::cout << text << "\n";
    if (g_file.is_open()) {
        g_file << text << "\n";
        // Flushed per line for the same reason stdout is unbuffered: a log that
        // loses its last lines to a crash omits precisely the lines that
        // explain the crash.
        g_file.flush();
    }
}

// Caller holds g_mutex.
//
// Every entry point calls this rather than relying on init() having run.
// Otherwise anything that logged before main() got around to initialising --
// a static constructor, an early scan -- would print to the console and
// silently never reach the file, which is the half you actually want when
// debugging someone else's machine.
void init_locked() {
    if (g_initialised) return;
    g_initialised = true;

    // Unbuffered stdout. Without this the console shows nothing until a buffer
    // fills, and a killed process prints nothing at all -- which is what made
    // an earlier attempt to capture a scan come back empty.
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    const fs::path path = app_data_path("vortex.log");
    g_path = path.string();

    std::error_code ec;
    const bool oversized =
        fs::exists(path, ec) && !ec && fs::file_size(path, ec) > kMaxLogBytes && !ec;

    g_file.open(path, oversized ? std::ios::trunc : std::ios::app);
    if (!g_file.is_open()) {
        // Not fatal: an install in a read-only directory still logs to the
        // console. Say so once rather than failing silently.
        g_path.clear();
        std::cout << "[log] could not open " << path.string()
                  << " -- console only\n";
        return;
    }

    emit_locked("");
    emit_locked("=====================================================");
    emit_locked("Vortex session started " + datestamp());
    emit_locked("=====================================================");
}

}  // namespace

void init() {
    std::lock_guard<std::mutex> guard(g_mutex);
    init_locked();
}

void line(const std::string &category, const std::string &message) {
    std::lock_guard<std::mutex> guard(g_mutex);
    init_locked();
    emit_locked(timestamp() + " [" + category + "] " + message);
}

void phase(const std::string &name, int total) {
    std::lock_guard<std::mutex> guard(g_mutex);
    init_locked();
    emit_locked("");
    if (total > 0)
        emit_locked(timestamp() + " == " + name + " (" + std::to_string(total) + " items)");
    else
        emit_locked(timestamp() + " == " + name);
}

void item(const std::string &category, const std::string &name, Status status,
          const std::string &detail) {
    std::lock_guard<std::mutex> guard(g_mutex);
    init_locked();
    std::string text = timestamp() + "   " + status_text(status) + " [" + category + "] " + name;
    if (!detail.empty()) text += "  -- " + detail;
    emit_locked(text);
}

void phase_done(const std::string &name, double seconds, int ok, int cached,
                int skipped, int failed) {
    std::lock_guard<std::mutex> guard(g_mutex);
    init_locked();

    std::ostringstream out;
    out << timestamp() << " == " << name << " done in " << std::fixed
        << std::setprecision(1) << seconds << "s -- " << ok << " ok, " << cached
        << " cached, " << skipped << " skipped, " << failed << " failed";
    emit_locked(out.str());
}

std::string file_path() {
    std::lock_guard<std::mutex> guard(g_mutex);
    init_locked();
    return g_path;
}

}  // namespace vlog
