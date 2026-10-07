#include "vortex_bridge.h"
#include "app_paths.h"
#include "ea_manager.h"
#include "game_manager.h"
#include "idle_tracker.h"
#include "igdb_manager.h"
#include "json_text.h"
#include "metadata_manager.h"
#include "preference_manager.h"
#include "riot_manager.h"
#include "stats_manager.h"
#include "steam_manager.h"
#include "secrets.h"
#include "steamgriddb_manager.h"
#include "trailer_playlist_server.h"
#include "vortex_log.h"

#include <QMetaObject>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QtMath>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <functional>
#include <memory>
#include <QVersionNumber>

#include <chrono>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Base directory owning Images/, local_game_dirs.txt and the caches: the folder
// the executable lives in. Shared with the CLI through app_paths.h so both
// binaries always agree — this used to search upward for a folder containing
// "Images", which landed somewhere different depending on the build layout.
// ─────────────────────────────────────────────────────────────────────────────
static fs::path resolveBaseDir() {
    return app_data_dir();
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
VortexBridge::VortexBridge(QObject *parent) : QObject(parent),
    m_baseDir(resolveBaseDir()) {
    init_stats_manager(m_baseDir.string());
    repair_metadata_cache_file();
    // Before the saved lists: loading them applies cached live art.
    loadLiveArtCache();
    loadWishlist();
    loadFavoriteSnapshots();
    loadRemovedGames();
    loadPlayedLedger();
    seedPlayedLedgerFromStats();
    backfillPlayedLedgerMetadata();
    loadSettings();
}

static std::string trimCopy(std::string value) {
    const char *ws = " \t\r\n\"";
    const size_t first = value.find_first_not_of(ws);
    if (first == std::string::npos) return "";
    const size_t last = value.find_last_not_of(ws);
    return value.substr(first, last - first + 1);
}

// Single location — baseDir is the executable's own folder. There used to be a
// second "beside the exe" fallback here; reading two files merged their contents
// and writing updated both, which let separate copies drift out of sync.
static std::vector<fs::path> localGameConfigPaths(const fs::path &baseDir) {
    return { baseDir / "local_game_dirs.txt" };
}

static std::vector<fs::path> readLocalGameDirectories(const fs::path &baseDir) {
    std::vector<fs::path> dirs;

    for (const fs::path &configPath : localGameConfigPaths(baseDir)) {
        if (!fs::exists(configPath)) continue;

        std::ifstream file(configPath);
        std::string line;
        while (std::getline(file, line)) {
            line = trimCopy(line);
            if (line.empty() || line[0] == '#') continue;

            fs::path dir(line);
            std::error_code ec;
            if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) continue;

            fs::path canonical = fs::weakly_canonical(dir, ec);
            if (ec) canonical = fs::absolute(dir, ec);
            if (ec) canonical = dir;

            if (std::find(dirs.begin(), dirs.end(), canonical) == dirs.end())
                dirs.push_back(canonical);
        }
    }

    return dirs;
}

static void writeLocalGameDirectories(const std::vector<fs::path> &dirs,
                                      const fs::path &baseDir) {
    for (const fs::path &configPath : localGameConfigPaths(baseDir)) {
        std::error_code ec;
        fs::create_directories(configPath.parent_path(), ec);

        std::ofstream out(configPath);
        if (!out) continue;

        out << "# Local Game Directories\n";
        for (const fs::path &dir : dirs)
            out << dir.string() << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// name_overrides.txt — titles the user typed over the scanned ones.
//
// "INSTALL_DIR|NAME", appended to and last-line-wins like exe_cache.txt, with an
// empty NAME putting the scanned title back. Keyed by install directory because
// that is fixed in pass 1, where the name is exactly what is being replaced.
// A pipe rather than exe_cache's '=' because Windows paths may hold '=' but
// never '|', so the first pipe always ends the path whatever the title holds.
// ─────────────────────────────────────────────────────────────────────────────
static fs::path nameOverridesPath() {
    return app_data_path("name_overrides.txt");
}

static QString overrideKey(const fs::path &installDir) {
    return QString::fromStdString(installDir.string()).toLower();
}

static QHash<QString, std::string> readNameOverrides() {
    QHash<QString, std::string> overrides;
    std::ifstream file(nameOverridesPath());
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t pipe = line.find('|');
        if (pipe == std::string::npos || pipe == 0) continue;

        const QString key = overrideKey(fs::path(line.substr(0, pipe)));
        const std::string name = trimCopy(line.substr(pipe + 1));
        if (name.empty())
            overrides.remove(key);
        else
            overrides.insert(key, name);
    }
    return overrides;
}

static bool appendNameOverride(const fs::path &installDir, const std::string &name) {
    const fs::path path = nameOverridesPath();
    const bool fresh = !fs::exists(path);
    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) return false;
    if (fresh)
        out << "# Vortex Name Overrides\n# Format: INSTALL_DIR|NAME (empty NAME = scanned name)\n";
    out << installDir.string() << "|" << name << "\n";
    return static_cast<bool>(out);
}

// One game's IGDB lookup, shared by the scan's pass 2 and renameGame().
//
// A renamed game is searched by the user's title alone -- not by appid, not by
// folder -- since putting a better search term in is the point of renaming, and
// its title is never swapped for IGDB's spelling.
static void resolveIgdb(BridgeGame &bg) {
    IgdbGameInfo info;
    if (!bg.customName.empty())
        info = igdb_resolve_game(bg.name, false);
    else if (bg.source == "Steam")
        info = igdb_resolve_game(bg.name, false, bg.appid);
    // A Riot install sits in a patchline folder (...\VALORANT\live), and IGDB
    // matched "live" to Katamari Damacy Rolling Live. Riot's own title instead.
    else if (bg.isRiot)
        info = igdb_resolve_game(bg.scannedName.empty() ? bg.name : bg.scannedName);
    else
        info = igdb_resolve_game(local_game_title(bg.installDir));

    if (info.id <= 0) {
        // A renamed game that IGDB has no answer for must not keep the id its
        // old title resolved to -- that is the wrong game's metadata.
        if (!bg.customName.empty()) bg.igdb_id = 0;
        return;
    }

    bg.igdb_id = info.id;
    // The canonical IGDB title replaces the folder name for local games. Steam
    // store names are already presentable, so those are left alone rather than
    // swapping in a subtly different spelling under the user mid-scan.
    if (bg.customName.empty() && bg.source != "Steam" && !info.name.empty())
        bg.name = info.name;
}

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::setLoading(bool val) {
    if (m_isLoading != val) {
        m_isLoading = val;
        emit loadingChanged();
    }
}

void VortexBridge::setRecommendationLoading(bool val) {
    if (m_isRecommendationLoading != val) {
        m_isRecommendationLoading = val;
        emit recommendationLoadingChanged();
    }
}

void VortexBridge::setRecommendationStatus(const QString &status) {
    if (m_recommendationStatus != status) {
        m_recommendationStatus = status;
        emit recommendationStatusChanged();
    }
}

// Where older builds downloaded artwork for unowned games. Nothing writes here
// any more -- unowned art loads live by URL -- and removeLegacyCandidateImages()
// deletes what an older build left behind. Still named because rows saved by
// those builds hold file:// paths into it, which applyLiveArtFallback() has to
// recognise to replace.
static fs::path legacyCandidateImagesDir(const fs::path &baseDir) {
    return baseDir / "CandidateImages";
}

static void removeLegacyCandidateImages(const fs::path &baseDir) {
    const fs::path root = legacyCandidateImagesDir(baseDir);
    std::error_code ec;
    if (!fs::exists(root, ec))
        return;

    std::uintmax_t bytes = 0;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code sizeEc;
        if (it->is_regular_file(sizeEc)) {
            const std::uintmax_t size = it->file_size(sizeEc);
            if (!sizeEc)
                bytes += size;
        }
    }
    std::error_code removeEc;
    const std::uintmax_t files = fs::remove_all(root, removeEc);
    vlog::line("Artwork", "Deleted the old artwork cache (CandidateImages): " +
                          std::to_string(files) + " entries, " +
                          std::to_string(bytes / (1024 * 1024)) + " MB" +
                          (removeEc ? " -- some files could not be removed" : ""));
}

// ─────────────────────────────────────────────────────────────────────────────
// Where unowned artwork comes from.
//
// Steam's CDN is addressable straight from an appid, with none of the search
// request SteamGridDB needs first, so a pick IGDB linked to Steam gets real
// wide art for a single GET and no API key. IGDB carries the rest: the size
// token in a cover URL can simply be rewritten, same image id, no second call.
// Neither path adds a request to the catalog fetch or a column to the schema.
// ─────────────────────────────────────────────────────────────────────────────
static QString steamArtUrl(int appId, const QString &file) {
    return QStringLiteral("https://cdn.cloudflare.steamstatic.com/steam/apps/%1/%2")
               .arg(appId).arg(file);
}

// t_cover_big is 264x374 -- the size the catalog stores, and far too small for
// a banner. t_720p is 1280x720 off the same id. Still portrait-shaped, so
// GameDetails blurs it rather than pretending it is a hero.
static QString igdbHeroUrl(const QString &coverUrl) {
    if (coverUrl.isEmpty())
        return {};

    QString url = coverUrl;
    // cover_of() in igdb_catalog.py writes t_cover_big; t_thumb is accepted too
    // in case an older recommendations.json is still on disk.
    url.replace(QStringLiteral("/t_cover_big/"), QStringLiteral("/t_720p/"));
    url.replace(QStringLiteral("/t_thumb/"), QStringLiteral("/t_720p/"));
    return url == coverUrl ? QString() : url;
}

// Whether a failed artwork request is the server answering "nothing here", as
// opposed to never reaching it. A 200 with no body counts as an answer.
static bool isDefinitiveMiss(QNetworkReply::NetworkError error) {
    return error == QNetworkReply::NoError
        || error == QNetworkReply::ContentNotFoundError
        || error == QNetworkReply::ContentAccessDenied
        || error == QNetworkReply::ContentGoneError
        || error == QNetworkReply::ContentOperationNotPermittedError;
}

static QString findImagePath(const fs::path &gameDir, const std::string &type) {
    for (const std::string &ext : {".jpg", ".png", ".jpeg"}) {
        fs::path full = gameDir / type / (type + ext);
        if (fs::exists(full)) {
            // QUrl::fromLocalFile() escapes the URL-significant characters
            // that steamgriddb_image_folder_name() leaves alone because Windows
            // permits them in filenames: '#' would otherwise open a fragment and
            // '%' would read as an escape, truncating the path QML receives.
            // ('?' never reaches here -- the sanitizer already turned it into
            // '_'.)
            //
            // Non-ASCII is a separate concern and needs no escaping: toString()
            // emits it verbatim. What it does need is a UTF-8 process code page,
            // or .string() below and QString::fromStdString() disagree about the
            // encoding and the path stops matching the directory on disk. See
            // vortex.manifest.
            QString abs = QString::fromStdString(fs::absolute(full).string());
            return QUrl::fromLocalFile(abs).toString();
        }
    }
    return "";
}

// Leading and trailing spaces off one pipe-separated field.
static std::string trimmed(const std::string &text) {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return std::string();
    const size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

static QString getLastPlayedDate(const std::string &gameKey,
                                 const fs::path &baseDir) {
    std::ifstream file(baseDir / "playtime_sessions.log");
    if (!file.is_open()) return "Never";
    // The key is the first field of
    // "KEY | NAME | SECONDS | START | END | IDLE", so it is matched as a prefix
    // rather than searched for anywhere in the line -- a plain find() makes
    // "igdb_1877" match every "igdb_18770" session too.
    //
    // The end date is found by counting from the END of the line, not from the
    // start and not by taking the last pipe outright.
    //
    // Taking the last pipe was right until idle was appended behind the dates,
    // after which it returned the idle seconds -- which parseDateToEpoch scores
    // as "Never", and the Played tab then sorts on. Counting from the start
    // instead would have traded that for a different break: a game whose NAME
    // contains a pipe shifts every field along, and the old reading was immune
    // to that.
    //
    // So: the trailing field is idle when it is a plain integer, and the end
    // date sits one before it; otherwise the line predates idle and the end
    // date is last. Dates always carry '-' and ':', so the two never look alike.
    const std::string prefix = gameKey + " |";
    std::string line, lastDate = "Never";
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.compare(0, prefix.size(), prefix) != 0) continue;

        std::vector<std::string> fields;
        for (size_t start = 0; start <= line.size();) {
            const size_t pipe = line.find('|', start);
            const size_t end  = pipe == std::string::npos ? line.size() : pipe;
            fields.push_back(trimmed(line.substr(start, end - start)));
            if (pipe == std::string::npos) break;
            start = pipe + 1;
        }
        if (fields.size() < 5) continue;

        const std::string &last = fields.back();
        const bool trailingIdle =
            !last.empty() && last.find_first_not_of("0123456789") == std::string::npos;

        const size_t dateAt = fields.size() - (trailingIdle ? 2 : 1);
        if (dateAt < 4) continue;   // not enough fields ahead of it to be a session

        // "2026-04-28 Tue 19:40:00" -> "2026-04-28"
        if (fields[dateAt].size() >= 10)
            lastDate = fields[dateAt].substr(0, 10);
    }
    return QString::fromStdString(lastDate);
}

// "YYYY-MM-DD" (what getLastPlayedDate returns) as a Unix timestamp, or 0 for
// "Never" and anything unparseable.
//
// Day granularity, deliberately: the sessions log stores formatted local time
// and the only consumer is the Played tab's ordering, where the total playtime
// breaks same-day ties. Reconstructing the exact second would mean parsing the
// log's "%Y-%m-%d %a %H:%M:%S" back through the locale it was written in.
static long long parseDateToEpoch(const QString &date) {
    const QDate parsed = QDate::fromString(date, QStringLiteral("yyyy-MM-dd"));
    if (!parsed.isValid())
        return 0;
    return QDateTime(parsed, QTime(0, 0)).toSecsSinceEpoch();
}

// Human-readable total, shared by the live rows and by the ledger entries seeded
// straight out of playtime_stats.txt -- two spellings of the same figure on the
// same screen is exactly the kind of thing that reads as a bug.
// Hours and minutes rather than decimal hours -- "15.7 Hours" reads like a
// broken clock at a glance, even though the .7 was only ever seven tenths.
static QString formatPlaytimeLabel(long long seconds) {
    if (seconds <= 0)
        return QStringLiteral("0m");
    const long long mins = std::max(1LL, seconds / 60);
    if (mins < 60)
        return QString::number(mins) + "m";
    return QString::number(mins / 60) + "h " + QString::number(mins % 60) + "m";
}

// Builds the playtime key string the same way the CLI does.
static std::string makePtKey(const BridgeGame &bg) {
    if (bg.igdb_id > 0)
        return "igdb_" + std::to_string(bg.igdb_id);
    if (bg.source == "Steam")
        return "steam_" + std::to_string(bg.appid);
    return "local_" + make_canonical(bg.name);
}

static QString pathToQString(const fs::path &path) {
    return QString::fromStdString(path.string());
}

// The analytics folder is copied next to the executable by the build (see
// CMakeLists.txt), so there is exactly one place to look. This used to search
// four locations — including %USERPROFILE%\Downloads — which silently picked up
// whichever stray copy happened to exist first.
static fs::path analyticsDir(const fs::path &baseDir) {
    return baseDir / "analytics";
}

static std::optional<fs::path> existingPath(const fs::path &path) {
    std::error_code ec;
    if (fs::exists(path, ec) && !ec)
        return path;
    return std::nullopt;
}

// Interpreters to try, in order. The bundled one first, then PATH, then
// installations discovered from the registry and from the Python Install
// Manager's shim directory.
//
// The bundled interpreter is what makes the packaged build self-contained: the
// installer ships an embeddable Python with sklearn, numpy and pandas already
// in it, so the recipient never installs Python at all. It is deliberately
// first -- a system Python that happens to be on PATH will not have the
// analytics dependencies, and falling through to it would produce a
// ModuleNotFoundError instead of a working recommender.
//
// On a development machine there is no python/ directory beside the exe, this
// candidate is skipped, and the search below behaves exactly as it always has.
//
// Resolving beyond PATH matters: Python 3.14 installs its shims under
// %LOCALAPPDATA%\Python\bin and relies on that being added to PATH. When it
// isn't, a complete, working install — interpreter, sklearn, numpy, pandas —
// is invisible to the app, and recommendations silently fall back to a plain
// ranked library.
static QStringList pythonInterpreterCandidates() {
    static const QStringList cached = []() -> QStringList {
        QStringList found;

        auto addIfUsable = [&found](const QString &path) {
            if (path.isEmpty() || found.contains(path)) return;
            if (QFile::exists(path)) found << path;
        };

        // 0. The interpreter the installer put next to the executable.
        addIfUsable(pathToQString(app_data_path("python/python.exe")));

        // 1. Anything already on PATH.
        for (const QString &name : { "python", "python3" })
            addIfUsable(QStandardPaths::findExecutable(name));

        // 2. Registered installations, newest version first. Per-user installs
        //    come before machine-wide ones, matching what "python" would pick.
        const QStringList registryRoots = {
            "HKEY_CURRENT_USER\\SOFTWARE\\Python\\PythonCore",
            "HKEY_LOCAL_MACHINE\\SOFTWARE\\Python\\PythonCore",
            "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Python\\PythonCore",
        };

        for (const QString &root : registryRoots) {
            QSettings registry(root, QSettings::NativeFormat);
            QStringList versions = registry.childGroups();

            std::sort(versions.begin(), versions.end(),
                      [](const QString &a, const QString &b) {
                          return QVersionNumber::fromString(a) > QVersionNumber::fromString(b);
                      });

            for (const QString &version : versions) {
                // A registry key's default value is read through "Default".
                const QString installDir =
                    registry.value(version + "/InstallPath/Default").toString();
                if (!installDir.isEmpty())
                    addIfUsable(QDir(installDir).filePath("python.exe"));
            }
        }

        // 3. Python Install Manager shims, whether or not they made it onto PATH.
        const QString localAppData =
            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        if (!localAppData.isEmpty())
            addIfUsable(QDir(localAppData).filePath("Python/bin/python.exe"));

        return found;
    }();

    return cached;
}

// Every way we know of to invoke a script, in preference order.
static QList<QPair<QString, QStringList>> pythonCommands(const QStringList &scriptArgs) {
    // -u before the script path: Python block-buffers stdout when it is a
    // pipe, so a long-running script's progress lines would all arrive in one
    // burst when the process exits rather than as the work happens. The
    // catalog fetch runs for several minutes and reports a running count, so
    // buffered output would make it indistinguishable from a hang.
    QList<QPair<QString, QStringList>> commands;
    const QStringList unbuffered = QStringList{ "-u" } + scriptArgs;

    for (const QString &interpreter : pythonInterpreterCandidates())
        commands.append({ interpreter, unbuffered });

    // Last resort: the py launcher resolves installs PATH and the registry miss.
    commands.append({ "py", QStringList{ "-3" } + unbuffered });
    return commands;
}

// Run a Python script and deliver its stdout LINE BY LINE as it arrives.
//
// runPythonScript() waits for the process to exit and then reads everything,
// which is right for a script that finishes in a second and wrong for the
// catalog fetch: that runs for minutes and prints a running count, and a
// progress report delivered after the work finishes is not a progress report.
//
// Returns false on any failure, with the accumulated stderr in `details`.
static bool runPythonScriptStreaming(const fs::path &scriptPath,
                                     const QStringList &extraArgs,
                                     const std::function<void(const QString &)> &onLine,
                                     QString *details,
                                     int timeoutMs) {
    std::error_code ec;
    if (!fs::exists(scriptPath, ec) || ec) {
        if (details) *details = "script not found: " + pathToQString(scriptPath);
        return false;
    }

    const QString script = pathToQString(scriptPath);
    const QString workingDir = pathToQString(scriptPath.parent_path());

    QStringList errors;
    for (const auto &command : pythonCommands(QStringList{ script } + extraArgs)) {
        QProcess process;
        process.setWorkingDirectory(workingDir);

        QString pending;
        QObject::connect(&process, &QProcess::readyReadStandardOutput,
                         [&process, &pending, &onLine]() {
            pending += QString::fromLocal8Bit(process.readAllStandardOutput());

            // Emit only complete lines; a partial one is held until its
            // newline arrives, so a count is never reported half-written.
            int newline;
            while ((newline = pending.indexOf('\n')) >= 0) {
                const QString line = pending.left(newline).trimmed();
                pending.remove(0, newline + 1);
                if (!line.isEmpty() && onLine) onLine(line);
            }
        });

        process.start(command.first, command.second);

        if (!process.waitForStarted(5000)) {
            errors << command.first + ": " + process.errorString();
            continue;
        }

        // Pump the event loop for this thread so readyReadStandardOutput
        // fires while we wait, rather than only at exit.
        QElapsedTimer elapsed;
        elapsed.start();
        bool finished = false;
        while (elapsed.elapsed() < timeoutMs) {
            if (process.waitForFinished(200)) { finished = true; break; }
            if (process.state() == QProcess::NotRunning) { finished = true; break; }
        }

        if (!finished) {
            process.kill();
            process.waitForFinished(3000);
            errors << command.first + ": timed out";
            continue;
        }

        if (!pending.trimmed().isEmpty() && onLine)
            onLine(pending.trimmed());

        const QString err = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();

        if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
            if (details) *details = err;
            return true;
        }
        errors << command.first + ": " + (err.isEmpty() ? QStringLiteral("no output") : err);
    }

    if (details) *details = errors.join(" | ");
    return false;
}

static bool runRecommendationScript(const fs::path &scriptPath, int mood,
                                    const QString &runId, bool curated,
                                    bool ignorePlayed, bool ignoreLiked,
                                    QString *details) {
    const QString script = pathToQString(scriptPath);
    const QString moodArg = QString::number(mood);

    // runId is echoed back in recommendations_meta.json so we can tell a fresh
    // result from a leftover file.
    QStringList scriptArgs{ script, moodArg, runId };
    // Appended, not inserted: recommend.py strips the flag before reading the
    // positional arguments, so order does not matter to it -- but keeping the
    // positionals first means the command line still reads the way every
    // previous build logged it.
    if (curated)
        scriptArgs << "--curated";
    if (ignorePlayed)
        scriptArgs << "--ignore-played";
    if (ignoreLiked)
        scriptArgs << "--ignore-liked";

    const QList<QPair<QString, QStringList>> commands = pythonCommands(scriptArgs);

    QStringList errors;
    for (const auto &command : commands) {
        QProcess process;
        process.setWorkingDirectory(pathToQString(scriptPath.parent_path()));
        process.start(command.first, command.second);

        if (!process.waitForStarted(5000)) {
            errors << command.first + ": " + process.errorString();
            continue;
        }

        if (!process.waitForFinished(120000)) {
            process.kill();
            process.waitForFinished(3000);
            errors << command.first + ": timed out";
            continue;
        }

        if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
            // Success still carries diagnostics worth keeping. recommend.py
            // exits 0 and warns on stderr when it ran but had nothing to work
            // with -- no genre data, a stale catalog -- and that warning is the
            // explanation for an empty list. Clearing details here threw away
            // the answer to "it worked, so why is there nothing on screen".
            if (details)
                *details = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            return true;
        }

        QString err = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        QString out = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
        errors << command.first + ": " + (err.isEmpty() ? out : err);
    }

    if (details) *details = errors.join(" | ");
    return false;
}

// Fire-and-forget maintenance script (sync / retrain). Runs with the analytics
// folder as the working directory so the scripts resolve their own data files
// the same way regardless of where the launcher was started from.
// Returns false if the script could not be run to a clean exit.
//
// This used to return void and discard everything. When the post-play sync
// failed — Postgres down, a missing dependency, a constraint error — the new
// session never reached the database, yet the recommendation run that followed
// still succeeded against the *stale* data and the UI reported
// "ML recommendations ready". The user saw a refresh that provably could not
// change anything, with nothing to indicate why.
static bool runPythonScript(const fs::path &scriptPath, QString *details = nullptr,
                            QString *stdOut = nullptr,
                            const QStringList &extraArgs = QStringList(),
                            int timeoutMs = 300000) {
    std::error_code ec;
    if (!fs::exists(scriptPath, ec) || ec) {
        if (details) *details = "script not found: " + pathToQString(scriptPath);
        return false;
    }

    const QString script = pathToQString(scriptPath);
    const QString workingDir = pathToQString(scriptPath.parent_path());

    QStringList errors;
    for (const auto &command : pythonCommands(QStringList{ script } + extraArgs)) {
        QProcess process;
        process.setWorkingDirectory(workingDir);
        process.start(command.first, command.second);

        if (!process.waitForStarted(5000)) {
            errors << command.first + ": " + process.errorString();
            continue;
        }

        if (!process.waitForFinished(timeoutMs)) {
            process.kill();
            process.waitForFinished(3000);
            errors << command.first + ": timed out";
            continue;
        }

        const QString out = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();

        if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
            if (stdOut) *stdOut = out;
            return true;
        }

        const QString err = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        errors << command.first + ": " + (err.isEmpty() ? out : err);
    }

    if (details) *details = errors.join(" | ");
    return false;
}

static QVariantMap findGameByName(const QVariantList &games, const QString &name) {
    for (const QVariant &entry : games) {
        const QVariantMap game = entry.toMap();
        if (QString::compare(game.value("name").toString(), name, Qt::CaseInsensitive) == 0)
            return game;
    }

    // Fall back to the canonical form. Exact comparison alone missed titles
    // that differ only in punctuation between the launcher and the database —
    // "Stick Fight The Game" (the folder) versus "Stick Fight: The Game" (the
    // resolved name) — and those rendered as "NOT IN LIBRARY" despite being
    // installed. Same rule as make_canonical() in game_manager.cpp.
    const std::string wanted = make_canonical(name.toStdString());
    if (wanted.empty())
        return {};

    for (const QVariant &entry : games) {
        const QVariantMap game = entry.toMap();
        if (make_canonical(game.value("name").toString().toStdString()) == wanted)
            return game;
    }
    return {};
}

// The IGDB half of a game row: developer, rating, time to beat, genres. Shared
// so a game that is no longer installed describes itself exactly as it did when
// it was -- the played ledger used to hardcode "Unknown" in all five slots and a
// game hearted out of the Played tab looked like it had lost its metadata.
//
// Every slot is written on both paths, so calling this on a row that already
// carries stale placeholders replaces them.
static void applyGameMetadata(QVariantMap &game, long long igdbId) {
    if (igdbId <= 0) {
        game["developer"]  = QString("Unknown");
        game["rating"]     = 0.0;
        game["timeToBeat"] = QString("N/A");
        game["genres"]     = QString("Unknown");
        game["tags"]       = QString("Unknown");
        return;
    }

    const GameMetadata meta = get_game_metadata(igdbId);
    game["developer"]  = QString::fromStdString(meta.developer);
    game["rating"]     = meta.rating;
    game["timeToBeat"] = (meta.time_to_beat_seconds > 0)
                         ? QString::number(meta.time_to_beat_seconds / 3600) + " Hours"
                         : "N/A";

    const auto join = [](const std::vector<std::string> &values) {
        QStringList parts;
        for (const std::string &value : values)
            parts << QString::fromStdString(value);
        return parts.join(", ");
    };

    const QString genres = join(meta.main_genres);
    const QString tags   = join(meta.all_genres);
    game["genres"] = genres.isEmpty() ? QString("Unknown") : genres;
    game["tags"]   = tags.isEmpty()   ? QString("Unknown") : tags;
}

// Whether a row still has nothing but the placeholders applyGameMetadata()
// writes for an unresolved game. Developer and genres together, because a
// handful of real games genuinely have one or the other missing upstream.
static bool metadataIsBlank(const QVariantMap &item) {
    const QString developer = item.value("developer").toString();
    const QString genres    = item.value("genres").toString();
    return (developer.isEmpty() || developer == "Unknown") &&
           (genres.isEmpty()    || genres    == "Unknown");
}

// Stands up the keys a details page reads straight off the row, for a game no
// list could supply one for. QML renders a missing key as "undefined", so they
// all have to exist even where the answer is not known yet -- applyGameMetadata()
// writes its own placeholders for an id of 0, and ensureMetadata() replaces
// those with the real values when the page opens.
static QVariantMap bareSnapshotFor(const QString &name) {
    QVariantMap snapshot;
    snapshot["name"]       = name;
    snapshot["source"]     = "IGDB";
    snapshot["matched"]    = false;
    snapshot["installDir"] = QString();
    snapshot["steamAppId"] = 0;
    snapshot["playtime"]   = "Not in library";
    snapshot["lastPlayed"] = "N/A";
    applyGameMetadata(snapshot, igdb_cached_id_for(name.toStdString()));
    return snapshot;
}

// The set of games that exist on this machine right now, with the exact names
// the UI shows. The analytics side cannot work either of those out for itself:
// igdb_cache.txt is append-only and is never pruned on uninstall, and it is
// keyed by the folder or Steam name that was searched rather than the resolved
// name the launcher displays.
static void writeInstalledGames(const fs::path &baseDir,
                                const std::vector<BridgeGame> &games) {
    // Never blank the file from an empty scan — a failed or interrupted scan
    // would otherwise wipe the library section on the next sync.
    if (games.empty()) return;

    std::ofstream out(baseDir / "installed_games.txt");
    if (!out) return;

    out << "# Games installed as of the last successful scan.\n";
    out << "# Written by the launcher; read by analytics/sync_local_data.py.\n";
    out << "# Format: NAME|SOURCE|IGDB_ID|PLAYTIME_SECONDS|LAST_PLAYED\n";
    for (const BridgeGame &game : games) {
        // Steam's own totals, carried across so the recommender can use
        // them.
        //
        // Vortex only records sessions it launched itself, so someone who
        // plays through Steam had a completely empty taste profile no
        // matter how many hours they had. Steam counts every session and
        // knows when the last one was; both are needed, because interest
        // decays with recency and a lifetime total cannot be weighted on
        // its own.
        //
        // 0 for local games: there is no external record of those, and
        // any play Vortex saw is already in playtime_sessions.log.
        //
        // Only the IMPORTED part is exported while Vortex owns the total.
        // synthesize_steam_sessions() turns this figure into synthetic
        // sessions, and playtime_sessions.log already carries every session
        // Vortex recorded -- exporting Steam's live total, which contains those
        // same sessions, would have the recommender count them twice. The
        // baseline is exactly the history the log does not have.
        //
        // In "use Steam's own playtime" mode none of the total is Vortex's to
        // begin with, so Steam's live figure is still what to send.
        long long playtimeSeconds = 0;
        long long lastPlayed = 0;
        if (game.source == "Steam" && game.appid > 0) {
            playtimeSeconds = use_steam_playtime()
                                  ? get_steam_playtime_seconds(game.appid)
                                  : get_play_stat(makePtKey(game)).baseline_seconds;
            lastPlayed = get_steam_last_played(game.appid);
        }

        out << game.name << "|" << game.source << "|" << game.igdb_id
            << "|" << playtimeSeconds << "|" << lastPlayed << "\n";
    }
}

static QVariantMap recommendationFromGameMap(QVariantMap game, double score, bool matched) {
    game["score"] = score;
    game["matched"] = matched;
    if (!game.contains("source"))
        game["source"] = matched ? "Library" : "ML";
    return game;
}

// Confirms recommendations.json was produced by the run we just started.
// Without this the bridge happily displayed a stale file as a fresh result
// whenever the script failed. A nonce beats a timestamp here because the build
// copies analytics/ next to the exe on every build, rewriting mtimes.
// catalogAgeDays, when given, receives the sidecar's catalog_age_days for a
// matching run, or -1 when the run did not match or the catalog is empty.
static bool recommendationRunMatches(const fs::path &metaPath, const QString &runId,
                                     double *catalogAgeDays = nullptr) {
    if (catalogAgeDays) *catalogAgeDays = -1.0;

    QFile file(pathToQString(metaPath));
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return false;

    const QJsonObject meta = doc.object();
    if (meta.value("run_id").toString() != runId)
        return false;

    if (catalogAgeDays) {
        const QJsonValue age = meta.value("catalog_age_days");
        if (age.isDouble())
            *catalogAgeDays = age.toDouble();
    }
    return true;
}

static QVariantList readRecommendationJson(const fs::path &jsonPath, const QVariantList &games) {
    QFile file(pathToQString(jsonPath));
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    // Must stay a top-level array. Anything else silently degrades to the
    // local fallback, so per-item metadata goes in the items themselves and
    // run metadata goes in the sidecar file.
    if (parseError.error != QJsonParseError::NoError || !doc.isArray())
        return {};

    QVariantList list;
    for (const QJsonValue &value : doc.array()) {
        if (!value.isObject()) continue;

        const QJsonObject object = value.toObject();
        const QString name = object.value("name").toString().trimmed();
        if (name.isEmpty()) continue;

        const double score = object.value("score").toDouble(0.0);
        const QString reason = object.value("reason").toString();
        const QString section = object.value("section").toString("library");

        // The evidence behind `reason`: the user's own games this pick
        // resembles, most similar first. recommend.py fills it from
        // scoring.inspiration_sources(); absent from an older
        // recommendations.json, which is why nothing here requires it.
        QVariantList inspiredBy;
        for (const QJsonValue &entry : object.value("inspiredBy").toArray()) {
            const QJsonObject src = entry.toObject();
            QVariantMap one;
            one["name"]       = src.value("name").toString();
            one["similarity"] = src.value("similarity").toDouble(0.0);
            one["played"]     = src.value("played").toBool(false);
            inspiredBy << one;
        }

        QVariantMap matchedGame = findGameByName(games, name);
        if (!matchedGame.isEmpty()) {
            // A game that IS in the library has no business under a heading
            // that reads "Not in your library yet". The recommender decides
            // the section from the `installed` flag, so this only fires when
            // that flag is wrong -- which is how a locally installed game
            // ended up presented as a discovery. Fixed at the source in
            // sync_local_data.py; kept here so a future mismatch is caught
            // and named rather than drawn on screen.
            if (section == "discover") {
                vlog::item("Recommend", name.toStdString(), vlog::Status::Skipped,
                           "listed as discovery but it is in the library");
                continue;
            }

            QVariantMap item = recommendationFromGameMap(matchedGame, score, true);
            // recommendationFromGameMap starts from the library map, so these
            // have to be set after it or the keys are simply absent.
            item["reason"] = reason;
            item["section"] = section;
            item["inspiredBy"] = inspiredBy;
            list << item;
            continue;
        }

        // The mirror of the guard above: a "from your library" pick that
        // matches nothing in the library is not a library pick. It used to be
        // drawn as a grey "NOT IN LIBRARY" placeholder sitting in the library
        // grid -- games the user had since uninstalled or deleted, kept alive
        // by a stale `installed` flag in the analytics database. Nothing here
        // can rescue such an entry: there is no cover, no playtime and no
        // install path to show, and the section it claims promises all three.
        if (section == "library") {
            vlog::item("Recommend", name.toStdString(), vlog::Status::Skipped,
                       "listed as library but it is not installed");
            continue;
        }

        // Unowned discovery candidate: everything the details page needs
        // travels with the item, because there is no library entry to read.
        QVariantMap item;
        item["name"] = name;
        item["score"] = score;
        item["reason"] = reason;
        item["section"] = section;
        item["inspiredBy"] = inspiredBy;
        item["source"] = "IGDB";
        item["developer"] = object.value("developer").toString("Unknown");
        item["rating"] = object.value("rating").toDouble(0.0);
        item["genres"] = object.value("genres").toString("Unknown");
        item["tags"] = object.value("tags").toString("Unknown");
        item["timeToBeat"] = object.value("timeToBeat").toString("N/A");
        item["steamAppId"] = object.value("steamAppId").toInt(0);
        item["coverUrl"] = object.value("coverUrl").toString();
        item["releasedAt"] = object.value("releasedAt").toString();
        item["playtime"] = "Not in library";
        item["lastPlayed"] = "N/A";
        // Nothing is installed for these, but the key has to exist: the details
        // page reads it directly and QML cannot assign undefined to a QString.
        item["installDir"] = QString();
        item["status"] = 0.0;
        item["matched"] = false;

        // Art loads live by URL; nothing is downloaded for these. IGDB's cover
        // stands in until the live-art lookup answers (applyLiveArtwork()),
        // and for the hero too -- GameDetails blurs a portrait stand-in
        // deliberately rather than stretching it sharp.
        const QString coverUrl = item.value("coverUrl").toString();
        const QString heroUrl = igdbHeroUrl(coverUrl);
        item["coverPath"] = coverUrl;
        item["heroPath"] = heroUrl.isEmpty() ? coverUrl : heroUrl;
        item["logoPath"] = QString();
        list << item;
    }

    return list;
}

static QVariantList buildLocalRecommendationFallback(const QVariantList &games) {
    std::vector<std::pair<double, QVariantMap>> ranked;

    for (const QVariant &entry : games) {
        QVariantMap game = entry.toMap();
        const double status = game.value("status").toDouble();

        double rating = game.value("rating").toDouble();
        if (rating > 10.0) rating /= 100.0;
        else rating /= 10.0;

        // Weights mirror the Python ranker's [0,1] range so the score badge in
        // the UI means the same thing whichever ranker produced it. This
        // fallback has no vectorizer, so it cannot honour mood or similarity —
        // the status line says so rather than passing itself off as ML output.
        double score = rating * 0.75;
        if (status > 0.0) score += 0.20;
        if (game.value("source").toString() == "Steam") score += 0.05;
        if (score > 1.0) score = 1.0;

        ranked.push_back({ score, recommendationFromGameMap(game, score, true) });
    }

    std::sort(ranked.begin(), ranked.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });

    QVariantList list;
    const size_t count = std::min<size_t>(10, ranked.size());
    for (size_t i = 0; i < count; ++i)
        list << ranked[i].second;

    return list;
}

// ─────────────────────────────────────────────────────────────────────────────
// Build one QVariantMap from a BridgeGame (called from background thread – safe
// because QVariant types are reentrant value types).
// ─────────────────────────────────────────────────────────────────────────────
QVariantMap VortexBridge::buildGameMap(const BridgeGame &bg) const {
    fs::path imageRoot  = m_baseDir / "Images";
    fs::path gameDir    = imageRoot / steamgriddb_image_folder_name(bg.name);
    std::string ptKey   = makePtKey(bg);

    QVariantMap game;
    game["name"]       = QString::fromStdString(bg.name);
    game["source"]     = QString::fromStdString(bg.source);
    game["isEa"]       = bg.isEa;
    game["isRiot"]     = bg.isRiot;
    game["appid"]      = bg.appid;
    game["installDir"] = QString::fromStdString(bg.installDir.string());
    game["gamePath"]   = QString::fromStdString(bg.gamePath.string());   // empty for Steam

    game["coverPath"]  = findImagePath(gameDir, "grid");
    game["heroPath"]   = findImagePath(gameDir, "hero");
    game["logoPath"]   = findImagePath(gameDir, "logo");
    // No hero anywhere: the cover stands in, which FrostedHero recognises as
    // portrait and blurs into a tinted backdrop. Without it a game that has a
    // logo got bare fill behind it, since the centred cover only shows when
    // there is no logo.
    if (game["heroPath"].toString().isEmpty())
        game["heroPath"] = game["coverPath"];

    // One read of the stats file, not one per figure -- this runs for every row
    // on every list rebuild, and twice per game during a scan.
    const PlayStat stat = get_play_stat(ptKey);

    // Vortex's own record is the total: Steam's lifetime figure imported once
    // when the game was first seen, plus every session since. The exception is
    // the "use Steam's own playtime" setting, which hands Steam titles back to
    // Steam's live figure for anyone who plays outside the launcher.

    long long ptSec        = 0;
    bool      steamSourced = false;
    if (use_steam_playtime() && bg.source == "Steam") {
        ptSec        = get_steam_playtime_seconds(bg.appid);
        steamSourced = ptSec > 0;
    }
    if (ptSec <= 0)
        ptSec = stat.seconds;

    const long long idleSec = std::min(stat.idle_seconds, ptSec);

    // Steam's figure is displayed exactly as Steam reports it. It covers
    // sessions Vortex never watched, so taking out idle that was only ever
    // measured for our own would produce a number meaning neither one thing nor
    // the other. The flag is on steamSourced rather than the setting because
    // the fallback above lands on our own total, which idle does apply to.
    const long long activeSec = steamSourced ? ptSec : ptSec - idleSec;

    game["playtime"]     = formatPlaytimeLabel(activeSec);
    game["idleTime"]     = formatPlaytimeLabel(idleSec);
    game["idleSeconds"]  = static_cast<qlonglong>(idleSec);
    game["idleDeducted"] = !steamSourced;
    game["totalPlaytime"] = formatPlaytimeLabel(ptSec);
    game["lastPlayed"] = getLastPlayedDate(ptKey, m_baseDir);

    // The same three facts in a form something other than a label can use: the
    // Played tab keys its ledger on playKey, sorts on lastPlayedAt and sums
    // playtimeSeconds. Derived here rather than recomputed there, so the tab and
    // the details page can never disagree about how long you played something.
    game["playKey"]         = QString::fromStdString(ptKey);
    game["playtimeSeconds"] = static_cast<qlonglong>(activeSec);

    // Steam knows the exact second; everything else is pinned to the day its
    // last session ended.
    long long lastPlayedAt = 0;
    if (bg.source == "Steam")
        lastPlayedAt = get_steam_last_played(bg.appid);
    if (lastPlayedAt <= 0)
        lastPlayedAt = parseDateToEpoch(game["lastPlayed"].toString());
    game["lastPlayedAt"] = static_cast<qlonglong>(lastPlayedAt);

    applyGameMetadata(game, bg.igdb_id);

    game["status"] = get_game_preference(bg.name);
    return game;
}

// ─────────────────────────────────────────────────────────────────────────────
// Lightweight refresh — rebuilds QVariantList from cached m_internalGames.
// Does NOT re-scan or hit the network. Called after preference / playtime changes.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::refreshGameList() {
    QVariantList list;
    list.reserve(static_cast<int>(m_internalGames.size()));
    for (const BridgeGame &bg : m_internalGames)
        list << buildGameMap(bg);
    m_gameList = list;
    invalidateGameIndex();
    syncPlayedLedger();
    emit gameListChanged();
    emit favoritesChanged();
}

// Public entry point. Coalesces bursts of calls: hearting a game, finishing a
// game and switching tabs can all fire in quick succession, and each one used
// to spawn its own Python interpreter (~1-3s of startup) for a result that the
// next call immediately discarded.
void VortexBridge::loadRecommendations() {
    if (m_isRecommendationLoading) {
        m_recommendationQueued = true;
        return;
    }

    if (!m_debounce) {
        m_debounce = new QTimer(this);
        m_debounce->setSingleShot(true);
        m_debounce->setInterval(1200);
        connect(m_debounce, &QTimer::timeout, this, &VortexBridge::startRecommendationRun);
    }
    // Do NOT restart a window that is already running. start() would reset it
    // on every call, so holding down refresh pushed the run further away each
    // time instead of coalescing into one.
    if (!m_debounce->isActive())
        m_debounce->start();
}

void VortexBridge::startRecommendationRun() {
    if (m_isRecommendationLoading) {
        m_recommendationQueued = true;
        return;
    }
    setRecommendationLoading(true);
    setRecommendationStatus("Loading recommendations...");

    const fs::path baseDir = m_baseDir;
    const int mood = m_currentMood;
    const bool curated = m_curatedOnly;
    const bool ignorePlayed = m_ignorePlayedGames;
    const bool ignoreLiked = m_ignoreLikedGames;
    const QVariantList gameListSnapshot = m_gameList;
    // Nonce proving the JSON we read came from the run we just started.
    const QString runId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QThread *thread = QThread::create([this, baseDir, mood, curated, ignorePlayed,
                                       ignoreLiked, gameListSnapshot, runId]() {
        const std::optional<fs::path> scriptPath =
            existingPath(analyticsDir(baseDir) / "recommend.py");

        bool scriptAttempted = false;
        bool scriptOk = false;
        QString scriptDetails;

        vlog::phase("Recommendations");

        if (scriptPath) {
            scriptAttempted = true;
            scriptOk = runRecommendationScript(*scriptPath, mood, runId, curated,
                                               ignorePlayed, ignoreLiked,
                                               &scriptDetails);

            // scriptDetails used to be captured here and then never read, so
            // the one thing that could explain a failure -- the Python stderr,
            // which says exactly what went wrong -- was thrown away, and the
            // user was left with "ML unavailable" and no way to find out why.
            if (scriptOk) {
                vlog::item("Recommend", "recommend.py", vlog::Status::Ok,
                           "mood " + std::to_string(mood) +
                           (curated ? ", curated" : "") +
                           (ignorePlayed ? ", ignoring played" : "") +
                           (ignoreLiked ? ", ignoring liked" : ""));
                // Warnings from a successful run: "no genre data", "catalog is
                // N days old". These are why an empty list is empty.
                for (const QString &warning : scriptDetails.split('\n', Qt::SkipEmptyParts))
                    vlog::line("Recommend", warning.trimmed().toStdString());
            } else {
                vlog::item("Recommend", "recommend.py", vlog::Status::Fail,
                           scriptDetails.isEmpty()
                               ? "no output"
                               : scriptDetails.toStdString());
            }
        } else {
            vlog::item("Recommend", "recommend.py", vlog::Status::Skipped,
                       "not found in analytics/");
        }

        QVariantList recommendations;
        QString status;
        const std::optional<fs::path> jsonPath =
            existingPath(analyticsDir(baseDir) / "recommendations.json");
        const fs::path metaPath = analyticsDir(baseDir) / "recommendations_meta.json";

        // Only trust the file if the sidecar echoes our nonce. Previously the
        // JSON was read even when the script failed and reported as
        // "ML cache loaded", so a stale result was indistinguishable from a
        // fresh one -- and the build copies analytics/ over the output on
        // every build, so a committed stale file would be served as current.
        // Read even when the list itself comes back empty: an empty Discover
        // is exactly the case where the catalog's state matters most.
        // A no-op once the folder is gone; here because this already runs off
        // the UI thread, and an old cache can hold thousands of files.
        removeLegacyCandidateImages(baseDir);

        double catalogAgeDays = -1.0;
        if (jsonPath && recommendationRunMatches(metaPath, runId, &catalogAgeDays)) {
            recommendations = readRecommendationJson(*jsonPath, gameListSnapshot);
            if (!recommendations.isEmpty())
                status = "ML recommendations ready";
        }

        if (recommendations.isEmpty()) {
            recommendations = buildLocalRecommendationFallback(gameListSnapshot);
            if (recommendations.isEmpty()) {
                status = "No recommendations available";
            } else if (scriptAttempted) {
                status = scriptOk ? "ML returned no results; showing ranked library"
                                  : "ML unavailable; showing ranked library";
            } else {
                status = "ML scripts not found; showing ranked library";
            }
        }

        // The same sentence the UI shows, in the log, so a screenshot of one
        // and a copy of the other cannot disagree.
        vlog::line("Recommend", std::to_string(recommendations.size()) +
                                " shown -- " + status.toStdString());

        // How many discovery picks survived. Zero with no download running is
        // what the Discover empty state keys off, and what decides whether the
        // one-time catalog fetch is worth starting.
        int discoverCount = 0;
        for (const QVariant &entry : recommendations) {
            if (entry.toMap().value("section").toString() == "discover")
                ++discoverCount;
        }

        QMetaObject::invokeMethod(this, [this, recommendations, status, discoverCount,
                                         catalogAgeDays]() {
            m_recommendationList = recommendations;
            // Live art the cache already knows goes on before the first paint;
            // the rest is looked up, the visible picks ahead of the saved lists.
            for (QVariant &entry : m_recommendationList) {
                QVariantMap item = entry.toMap();
                if (item.value("matched").toBool())
                    continue;
                prepareLiveArtwork(item, true);
                entry = item;
            }
            m_discoverCandidateCount = discoverCount;
            m_catalogAgeDays = catalogAgeDays;
            setRecommendationLoading(false);
            setRecommendationStatus(status);

            // Nothing to discover from means the catalog was never fetched; an
            // old one is due its periodic refresh. Only with credentials present.
            maybeAutoFetchCatalog();
            emit recommendationListChanged();

            if (m_recommendationQueued) {
                m_recommendationQueued = false;
                loadRecommendations();
            }
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// ── Live art for one game, now, because its details page just opened.
//
// Every unowned row is already queued when its list loads; this moves the game
// a page is open on to the front, so its banner and logo are the next answer
// rather than wait behind a whole Discover grid. No-op for owned games, whose
// art is SteamGridDB's under Images/.
void VortexBridge::ensureArtwork(QString name) {
    name = name.trimmed();
    if (name.isEmpty())
        return;

    QVariantMap item = findGameByName(m_recommendationList, name);
    if (item.isEmpty())
        item = savedRowFor(name);
    if (item.isEmpty() || item.value("matched").toBool())
        return;

    // The row's own spelling: findGameByName() matches canonically, and the
    // live-art cache is keyed on the name every list shares.
    resolveLiveArtwork(item.value("name").toString(), true);
}


// Real developer, genres and rating for one unowned pick, resolved when its
// details page opens.
//
// A snapshot normally carries these from whichever list it was copied out of.
// The exception is a favourite that outlived every list -- hearted from
// Discover, un-hearted, then hearted again after the recommendations moved on
// -- which has nothing but a name to build from and rendered "Unknown" in every
// slot. igdb_cached_id_for() answers offline for any title a scan has already
// resolved; a name the cache has never seen is what the lookup below is for.
// igdb_resolve_game() writes both the resolution cache and game_metadata.txt on
// the way through, so a title costs one request per machine, hit or miss.
//
// No-op for owned games and for rows that already carry real metadata.
void VortexBridge::ensureMetadata(QString name) {
    name = name.trimmed();
    if (name.isEmpty())
        return;

    // Same list order and the same `matched` exclusion as ensureArtwork().
    QVariantMap item = findGameByName(m_recommendationList, name);
    if (item.isEmpty())
        item = findGameByName(m_wishlist, name);
    if (item.isEmpty())
        item = findGameByName(m_favoriteSnapshots, name);
    if (item.isEmpty())
        item = findGameByName(m_playedLedger, name);
    if (item.isEmpty() || item.value("matched").toBool())
        return;
    if (!metadataIsBlank(item))
        return;

    // The row's own spelling, for the same reason ensureArtwork() takes it:
    // findGameByName() matches canonically, and the caches are keyed literally.
    name = item.value("name").toString();
    if (m_metadataAsked.contains(name))
        return;      // request in flight, or IGDB has already said it has none

    const long long cached = igdb_cached_id_for(name.toStdString());
    if (cached > 0) {
        applyResolvedMetadata(name, cached);
        return;
    }

    // Blocking HTTPS, so off the UI thread -- every other network call in here
    // is dispatched the same way.
    m_metadataAsked.insert(name);
    QThread *thread = QThread::create([this, name]() {
        const long long resolved = igdb_resolve_game(name.toStdString(), false).id;
        QMetaObject::invokeMethod(this, [this, name, resolved]() {
            // Cleared only on success. A title IGDB has no answer for stays in
            // the set and is asked once per session rather than once per page
            // open; a success needs no entry, because the row stops being blank
            // and the check above returns before reaching here.
            if (resolved > 0) {
                m_metadataAsked.remove(name);
                applyResolvedMetadata(name, resolved);
            }
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// Writes one resolved id's metadata into every list carrying that name, and
// emits so an open details page re-reads it. Shaped like applyLiveArtwork() and
// for the same reason: an unowned game can sit in any of these four lists at
// once, and the two that persist have to be written back to disk.
void VortexBridge::applyResolvedMetadata(const QString &name, long long igdbId) {
    auto apply = [&](QVariantList &list) {
        bool changed = false;
        for (QVariant &entry : list) {
            QVariantMap item = entry.toMap();
            if (QString::compare(item.value("name").toString(), name,
                                 Qt::CaseInsensitive) != 0)
                continue;
            if (!metadataIsBlank(item))
                continue;      // came from a live row, which knows more
            applyGameMetadata(item, igdbId);
            entry = item;
            changed = true;
        }
        return changed;
    };

    if (apply(m_recommendationList))
        emit recommendationListChanged();
    if (apply(m_wishlist)) {
        saveWishlist();
        emit wishlistChanged();
    }
    if (apply(m_favoriteSnapshots)) {
        saveFavoriteSnapshots();
        emit favoritesChanged();
    }
    if (apply(m_playedLedger)) {
        savePlayedLedger();
        emit playedGamesChanged();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Live art for unowned games.
//
// Recommendation, wishlist, favourite and played rows for games that are not
// installed keep no image files at all. Their art slots hold remote URLs that
// QML loads directly -- SteamGridDB's CDN when it has the game (the same source
// owned games use), otherwise Steam's CDN by appid, otherwise IGDB's cover.
//
// Only the URLs are kept: in live_artwork.json, keyed by name, so a game is
// looked up once per machine however many times it reappears in Discover; and
// in the saved lists' own JSON, which they serialise whole anyway.
// ─────────────────────────────────────────────────────────────────────────────

// A game SteamGridDB and Steam both had nothing for is asked again after this
// long, as the old .artwork_state negative cache did; one with art never is --
// a CDN URL does not go stale.
static constexpr qint64 kLiveArtEmptyRetrySeconds = 7 * 24 * 60 * 60;

static const char *const kLiveArtKeys[] = { "liveCoverUrl", "liveHeroUrl", "liveLogoUrl" };

static QString liveArtKey(const QString &name) {
    return name.trimmed().toLower();
}

static bool liveArtHasAny(const QVariantMap &art) {
    for (const char *key : kLiveArtKeys)
        if (!art.value(key).toString().isEmpty())
            return true;
    return false;
}

// Whether a URL is a file:// path into the old CandidateImages/ cache. Rows
// written by older builds hold these, and the folder is deleted.
static bool isUnderCandidateRoot(const QString &value, const fs::path &candidateRoot) {
    if (!value.startsWith(QLatin1String("file:")))
        return false;
    const QString rootPath =
        QDir::cleanPath(QString::fromStdString(fs::absolute(candidateRoot).string()));
    const QString local = QDir::cleanPath(QUrl(value).toLocalFile());
    return local.startsWith(rootPath + QLatin1Char('/'), Qt::CaseInsensitive);
}

// Whether a slot holds a file:// path whose file is no longer there. The ledger
// keeps the path a game had while it was installed, and removeLocalGameDirectory()
// deletes the files without touching the row.
static bool isMissingLocalFile(const QString &value) {
    return value.startsWith(QLatin1String("file:"))
        && !QFileInfo::exists(QUrl(value).toLocalFile());
}

// Repoints one row's art slots. A slot holding real art under Images/ -- an
// owned or once-owned game -- is kept. One pointing into the old
// CandidateImages/ always goes, even with nothing to put in its place. An
// empty, remote or dead-file slot is rebuilt from the live URLs the row
// carries, so a better answer replaces an earlier stand-in -- but only when
// there is something to put there, so an owned game's ledger row with no
// Images/ art and no coverUrl is left as it was.
static void applyLiveArtFallback(QVariantMap &item, const fs::path &candidateRoot) {
    const QString coverUrl = item.value("coverUrl").toString();
    QString cover = item.value("liveCoverUrl").toString();
    if (cover.isEmpty())
        cover = coverUrl;
    QString hero = item.value("liveHeroUrl").toString();
    if (hero.isEmpty())
        hero = igdbHeroUrl(coverUrl);
    const QString logo = item.value("liveLogoUrl").toString();

    auto rebuild = [&](const char *key, const QString &value) {
        const QString current = item.value(key).toString();
        if (isUnderCandidateRoot(current, candidateRoot)
            || ((current.isEmpty() || current.startsWith(QLatin1String("http"))
                 || isMissingLocalFile(current))
                && !value.isEmpty()))
            item[key] = value;
    };
    rebuild("coverPath", cover);
    rebuild("heroPath", hero.isEmpty() ? cover : hero);
    rebuild("logoPath", logo);
}

// Whether a row's cover is a real file of its own under Images/, and so needs
// no lookup.
static bool hasLocalCover(const QVariantMap &item, const fs::path &candidateRoot) {
    const QString cover = item.value("coverPath").toString();
    return cover.startsWith(QLatin1String("file:"))
        && !isUnderCandidateRoot(cover, candidateRoot)
        && QFileInfo::exists(QUrl(cover).toLocalFile());
}

fs::path VortexBridge::liveArtCachePath() const {
    return m_baseDir / "live_artwork.json";
}

void VortexBridge::loadLiveArtCache() {
    m_liveArtCache.clear();
    QFile file(pathToQString(liveArtCachePath()));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (it.value().isObject())
            m_liveArtCache.insert(it.key(), it.value().toObject().toVariantMap());
    }
}

void VortexBridge::saveLiveArtCache() const {
    QJsonObject root;
    for (auto it = m_liveArtCache.constBegin(); it != m_liveArtCache.constEnd(); ++it)
        root.insert(it.key(), QJsonObject::fromVariantMap(it.value()));
    QFile file(pathToQString(liveArtCachePath()));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

// An entry with art is final. An empty one -- nothing upstream -- is retried
// once it ages out; a stamp in the future (the clock went backwards) counts
// as aged out, so a bad clock cannot hold a game's art off indefinitely.
bool VortexBridge::liveArtCacheIsFresh(const QString &name) const {
    const auto it = m_liveArtCache.constFind(liveArtKey(name));
    if (it == m_liveArtCache.constEnd())
        return false;
    if (liveArtHasAny(it.value()))
        return true;
    const qint64 at = it.value().value("at").toLongLong();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    return at > 0 && now >= at && now - at < kLiveArtEmptyRetrySeconds;
}

QVariantMap VortexBridge::savedRowFor(const QString &name) const {
    QVariantMap row = findGameByName(m_wishlist, name);
    if (row.isEmpty())
        row = findGameByName(m_favoriteSnapshots, name);
    if (row.isEmpty())
        row = findGameByName(m_playedLedger, name);
    return row;
}

void VortexBridge::prepareLiveArtwork(QVariantMap &row, bool urgent) {
    const QString name = row.value("name").toString();
    const fs::path legacyRoot = legacyCandidateImagesDir(m_baseDir);

    const auto cached = m_liveArtCache.constFind(liveArtKey(name));
    if (cached != m_liveArtCache.constEnd()) {
        for (const char *key : kLiveArtKeys) {
            const QString url = cached.value().value(key).toString();
            if (!url.isEmpty())
                row[key] = url;
        }
    } else if (row.value("liveArtResolved").toBool() && !name.isEmpty()) {
        // Resolved by a build that kept the answer only in the row itself:
        // adopt it, so the same game showing up in Discover is not asked again.
        QVariantMap art;
        for (const char *key : kLiveArtKeys)
            art[key] = row.value(key).toString();
        art["at"] = QDateTime::currentSecsSinceEpoch();
        m_liveArtCache.insert(liveArtKey(name), art);
        saveLiveArtCache();
    }

    applyLiveArtFallback(row, legacyRoot);
    if (!hasLocalCover(row, legacyRoot) && !liveArtCacheIsFresh(name))
        resolveLiveArtwork(name, urgent);
}

void VortexBridge::resolveLiveArtwork(const QString &name, bool urgent) {
    if (name.isEmpty() || liveArtCacheIsFresh(name))
        return;

    const QString key = liveArtKey(name);
    if (m_liveArtAsked.contains(key)) {
        // Queued already: a details page opening on it jumps the line. In
        // flight, or asked and unanswered this session: nothing to do.
        const int at = m_liveArtQueue.indexOf(name);
        if (urgent && at > 0)
            m_liveArtQueue.move(at, 0);
        return;
    }
    m_liveArtAsked.insert(key);
    m_liveArtPending.insert(key);
    if (urgent)
        m_liveArtQueue.prepend(name);
    else
        m_liveArtQueue.append(name);
    pumpLiveArtwork();
}

// A few lookups at once: each is four sequential SteamGridDB requests, and a
// fresh Discover grid is a dozen-plus games all waiting on theirs.
static constexpr int kMaxLiveArtWorkers = 3;

void VortexBridge::pumpLiveArtwork() {
    while (m_liveArtRunning < kMaxLiveArtWorkers && !m_liveArtQueue.isEmpty()) {
        ++m_liveArtRunning;
        const QString name = m_liveArtQueue.takeFirst();

        // Blocking WinHTTP, so off the UI thread like ensureMetadata().
        QThread *thread = QThread::create([this, name]() {
            const SgdbArtUrls sgdb = steamgriddb_art_urls(name.toStdString());
            QVariantMap art;
            art["liveCoverUrl"] = QString::fromStdString(sgdb.grid);
            art["liveHeroUrl"]  = QString::fromStdString(sgdb.hero);
            art["liveLogoUrl"]  = QString::fromStdString(sgdb.logo);
            const bool reachable = sgdb.reachable;
            QMetaObject::invokeMethod(this, [this, name, art, reachable]() {
                // The slot is SteamGridDB's. Steam's probes are cheap HEADs and
                // may sit waiting on IGDB, so they do not hold it.
                --m_liveArtRunning;
                finishLiveArtwork(name, art, reachable);
                pumpLiveArtwork();
            }, Qt::QueuedConnection);
        });
        connect(thread, &QThread::finished, thread, &QThread::deleteLater);
        thread->start();
    }
}

// Fills whatever SteamGridDB had no answer for from Steam's CDN. HEAD, not GET:
// the question is only whether the URL exists, and an old app 404s on
// library_hero.jpg -- a URL QML would otherwise draw as a blank banner.
void VortexBridge::finishLiveArtwork(const QString &name, const QVariantMap &art,
                                     bool definitive) {
    QVariantMap row = findGameByName(m_recommendationList, name);
    if (row.isEmpty())
        row = savedRowFor(name);
    if (row.isEmpty())
        row = findGameByName(m_browseSnapshots, name);   // a Discover page's game
    const int appId = row.value("steamAppId").toInt();

    // A Discover page asked before IGDB answered, and SteamGridDB left a gap
    // Steam might fill: hold on until the app id is known.
    if (appId <= 0
        && (art.value("liveHeroUrl").toString().isEmpty()
            || art.value("liveLogoUrl").toString().isEmpty())
        && liveArtAwaitsBrowseAppId(name)) {
        QVariantMap parked;
        parked["name"]       = name;
        parked["art"]        = art;
        parked["definitive"] = definitive;
        m_liveArtAwaitingBrowse.insert(liveArtKey(name), parked);
        return;
    }

    QList<QPair<QString, QString>> probes;   // art key -> URL
    if (appId > 0) {
        if (art.value("liveHeroUrl").toString().isEmpty())
            probes.append({ QStringLiteral("liveHeroUrl"),
                            steamArtUrl(appId, QStringLiteral("library_hero.jpg")) });
        if (art.value("liveLogoUrl").toString().isEmpty())
            probes.append({ QStringLiteral("liveLogoUrl"),
                            steamArtUrl(appId, QStringLiteral("logo.png")) });
    }

    auto done = [this](const QString &name, const QVariantMap &art, bool definitive) {
        applyLiveArtwork(name, art, definitive);
    };

    if (probes.isEmpty()) {
        done(name, art, definitive);
        return;
    }

    if (!m_network)
        m_network = new QNetworkAccessManager(this);

    struct Pending {
        QVariantMap art;
        bool definitive;
        int remaining;
    };
    auto pending = std::make_shared<Pending>(Pending{ art, definitive, int(probes.size()) });

    for (const auto &[key, url] : probes) {
        QNetworkRequest request { QUrl(url) };
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = m_network->head(request);
        connect(reply, &QNetworkReply::finished, this,
                [reply, key, url, name, pending, done]() {
            reply->deleteLater();
            const QNetworkReply::NetworkError error = reply->error();
            if (error == QNetworkReply::NoError
                && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200)
                pending->art[key] = url;
            else if (!isDefinitiveMiss(error))
                pending->definitive = false;   // offline: try again next launch
            if (--pending->remaining == 0)
                done(name, pending->art, pending->definitive);
        });
    }
}

// The open Discover page is on this game and IGDB has not answered yet.
bool VortexBridge::liveArtAwaitsBrowseAppId(const QString &name) const {
    return m_browseDetailsLoading
        && QString::compare(m_browseDetails.value("name").toString(), name,
                            Qt::CaseInsensitive) == 0;
}

// Sends on every parked answer whose page is no longer waiting. `answered` is
// the game IGDB has just described -- its row now holds whatever Steam app id
// there is, so its answer keeps its own verdict. Any other went on without the
// id it waited for (IGDB failed, or the page moved on), so it is not
// definitive and Steam is still asked on a later run.
void VortexBridge::releaseLiveArtAwaitingBrowse(const QString &answered) {
    const QList<QString> keys = m_liveArtAwaitingBrowse.keys();
    for (const QString &key : keys) {
        const QVariantMap parked = m_liveArtAwaitingBrowse.value(key);
        const QString name = parked.value("name").toString();
        if (liveArtAwaitsBrowseAppId(name))
            continue;
        m_liveArtAwaitingBrowse.remove(key);
        const bool definitive = parked.value("definitive").toBool()
            && !answered.isEmpty() && key == liveArtKey(answered);
        finishLiveArtwork(name, parked.value("art").toMap(), definitive);
    }
}

// Same shape as applyResolvedMetadata(): one game can sit in the recommendation
// list and all three saved lists at once. Each saved list that changed is
// written back to disk; the recommendation list is rebuilt every run and only
// needs the emit.
void VortexBridge::applyLiveArtwork(const QString &name, const QVariantMap &art,
                                    bool definitive) {
    m_liveArtPending.remove(liveArtKey(name));

    // Only a complete answer is cached; anything less is asked again on the
    // next launch, with the fallback URLs showing meanwhile.
    if (definitive) {
        QVariantMap entry;
        for (const char *key : kLiveArtKeys)
            entry[key] = art.value(key).toString();
        entry["at"] = QDateTime::currentSecsSinceEpoch();
        m_liveArtCache.insert(liveArtKey(name), entry);
        saveLiveArtCache();
    }

    const fs::path legacyRoot = legacyCandidateImagesDir(m_baseDir);
    auto apply = [&](QVariantList &list) {
        bool changed = false;
        for (QVariant &entry : list) {
            QVariantMap item = entry.toMap();
            if (item.value("matched").toBool())
                continue;
            if (QString::compare(item.value("name").toString(), name,
                                 Qt::CaseInsensitive) != 0)
                continue;

            const QVariantMap before = item;
            for (auto it = art.constBegin(); it != art.constEnd(); ++it) {
                if (!it.value().toString().isEmpty())
                    item[it.key()] = it.value();
            }
            if (definitive)
                item["liveArtResolved"] = true;
            applyLiveArtFallback(item, legacyRoot);
            if (item == before)
                continue;
            entry = item;
            changed = true;
        }
        return changed;
    };

    // GameDetails re-runs findGameData() on each of these (GameDetails.qml:191),
    // so emitting is all an already-open page needs to pick the art up.
    if (apply(m_recommendationList))
        emit recommendationListChanged();
    if (apply(m_wishlist)) {
        saveWishlist();
        emit wishlistChanged();
    }
    if (apply(m_favoriteSnapshots)) {
        saveFavoriteSnapshots();
        // Nothing in m_gameList moved, so this must not be gameListChanged:
        // art arriving for an unowned favourite would rebuild the library grid
        // and scroll it back to the top mid-scan.
        emit favoritesChanged();
    }
    if (apply(m_playedLedger)) {
        savePlayedLedger();
        emit playedGamesChanged();
    }
    // Read only when one is wishlisted or hearted, so nothing to emit.
    apply(m_browseSnapshots);

    // An open Discover page on this game: whatever arrived goes up, and the
    // wait is over either way, so IGDB's banner may stand in for the rest.
    if (QString::compare(m_browseDetails.value("name").toString(), name,
                         Qt::CaseInsensitive) == 0) {
        const QString hero = art.value("liveHeroUrl").toString();
        const QString logo = art.value("liveLogoUrl").toString();
        if (!hero.isEmpty())
            m_browseDetails["bannerUrl"] = hero;
        if (!logo.isEmpty())
            m_browseDetails["logoUrl"] = logo;
        m_browseDetails["artPending"] = false;
        emit browseDetailsChanged();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Favourites / wishlist
// ─────────────────────────────────────────────────────────────────────────────
QVariantList VortexBridge::favoriteGames() const {
    // Owned favourites come from the live game list — buildGameMap already
    // sets `status` from get_game_preference().
    QVariantList list;
    QSet<QString> seen;
    //
    // Every row says whether it is installed, the way playedGames() does: the
    // card's PLAY button reads it, and this list mixes library rows with ones
    // that have nothing on the disk to launch.
    for (const QVariant &entry : m_gameList) {
        QVariantMap game = entry.toMap();
        if (game.value("status").toDouble() > 0.0) {
            game["installed"] = true;
            list << game;
            seen.insert(game.value("name").toString().toLower());
        }
    }

    // Then favourites that are not installed. Hearting a Discover pick wrote
    // it to preferences.json (so it did influence the recommender) but it has
    // no row in m_gameList, so before this it could never be displayed
    // anywhere — the tab appeared to list only library games.
    //
    // Owned wins on a name clash: once a game is actually installed, the live
    // library entry is better than the snapshot taken when it was hearted.
    // Built on first use only: most favourites are owned and never reach it.
    QVariantList playedByName;

    for (const QVariant &entry : m_favoriteSnapshots) {
        QVariantMap snapshot = entry.toMap();
        const QString name = snapshot.value("name").toString();
        if (name.isEmpty() || seen.contains(name.toLower()))
            continue;
        // Taken out of the library, so it must not come back through the
        // snapshot list -- the heart itself is left alone, so restoring the
        // game restores the favourite with it.
        if (isRemovedName(name))
            continue;
        // The preference file remains the source of truth; a snapshot whose
        // heart was removed elsewhere must not linger.
        if (get_game_preference(name.toStdString()) <= 0.0)
            continue;

        // A game that was played and then uninstalled already has a full row in
        // the played history -- artwork, playtime, developer and genres. That
        // row is both richer and fresher than a snapshot frozen at the moment of
        // the heart click, so it wins here for the same reason the live library
        // row wins above. Without this, hearting from the Played tab replaced
        // the page with the bare snapshot and the art and metadata vanished.
        //
        // playedGames() rather than m_playedLedger: one title can hold several
        // ledger rows (Stellar Blade is both igdb_117170 and local_stellarblade)
        // and only the folded row carries the summed playtime and the source the
        // Played tab itself shows. Reading the ledger directly picked whichever
        // row came first and the totals changed under the heart.
        if (playedByName.isEmpty())
            playedByName = playedGames();

        QVariantMap played = findGameByName(playedByName, name);
        if (!played.isEmpty()) {
            // Presentation from the played row, identity from the snapshot.
            // findGameByName() matches canonically, so the two can spell the
            // title differently ("Stick Fight The Game" against "Stick Fight:
            // The Game"), and preferences.json is keyed on the exact string
            // that was hearted. Carrying the other spelling into the card would
            // send the next heart click to toggle_game_preference() under a
            // name it has never seen, which writes a second entry instead of
            // clearing the first -- the game would refuse to unlike.
            // `installed` comes with the played row as playedGames() set it.
            played["name"]   = name;
            played["status"] = 1.0;
            list << played;
            continue;
        }

        snapshot["installed"] = false;
        list << snapshot;
    }
    return list;
}

fs::path VortexBridge::favoriteSnapshotPath() const {
    return m_baseDir / "favorite_snapshots.json";
}

// Points a snapshot's three art slots at whatever Images/ holds for that name.
//
// Images/ is keyed by name, so a game's artwork can always be found again even
// when no list still carries a row for it. Re-resolving rather than trusting a
// stored value also matters on load: the saved paths are absolute file:// URLs
// and stop working the moment the app is moved to another folder. Same
// reasoning as loadPlayedLedger().
//
// The old CandidateImages/ cache is not consulted: it is deleted, and an
// unowned row gets live URLs from prepareLiveArtwork() instead.
static void bindArtworkFromCache(const fs::path &baseDir, const QString &name,
                                 QVariantMap &item) {
    if (name.isEmpty()) return;

    // "grid" is the cover.
    static const std::pair<const char *, const char *> kArtSlots[] = {
        { "coverPath", "grid" }, { "heroPath", "hero" }, { "logoPath", "logo" }
    };

    const fs::path gameDir =
        baseDir / "Images" / steamgriddb_image_folder_name(name.toStdString());

    for (const auto &slot : kArtSlots) {
        // A favourite that is installed, or was played and then uninstalled,
        // has its full-size art here.
        const QString found = findImagePath(gameDir, slot.second);
        // Nothing on disk leaves whatever the row already had.
        if (!found.isEmpty())
            item[slot.first] = found;
    }
}

void VortexBridge::loadFavoriteSnapshots() {
    m_favoriteSnapshots.clear();

    QFile file(pathToQString(favoriteSnapshotPath()));
    if (!file.open(QIODevice::ReadOnly)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) return;

    for (const QJsonValue &value : doc.array()) {
        if (!value.isObject()) continue;
        QVariantMap entry = value.toObject().toVariantMap();
        const QString name = entry.value("name").toString();
        bindArtworkFromCache(m_baseDir, name, entry);
        prepareLiveArtwork(entry);
        // A row written before snapshots were kept across an un-heart can still
        // be holding placeholders. The resolution cache covers anything a scan
        // has already seen; the rest waits for ensureMetadata() and one lookup.
        if (metadataIsBlank(entry)) {
            const long long id = igdb_cached_id_for(name.toStdString());
            if (id > 0)
                applyGameMetadata(entry, id);
        }
        m_favoriteSnapshots << entry;
    }
}

void VortexBridge::saveFavoriteSnapshots() const {
    QJsonArray array;
    for (const QVariant &entry : m_favoriteSnapshots)
        array.append(QJsonObject::fromVariantMap(entry.toMap()));

    QFile file(pathToQString(favoriteSnapshotPath()));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
}

// Keeps a renderable copy of an unowned favourite. Same reasoning as the
// wishlist: the game is absent from m_gameList, so without a snapshot there is
// nothing to draw a card or a details page from — and this way the tab still
// works with Postgres stopped and no network.
void VortexBridge::updateFavoriteSnapshot(const QString &name, bool favorited) {
    // Deliberately kept on un-heart rather than removed.
    //
    // favoriteGames() already gates every snapshot on get_game_preference(), so
    // a kept row is invisible the moment the heart comes off -- removing it buys
    // nothing and costs the only copy of the game's art and metadata that
    // survives the recommendations reshuffling past it. Deleting it meant
    // hearting the same game again rebuilt it from a map holding nothing but a
    // name: a blank card, and "Unknown" in every slot on the details page.
    //
    // Added to and never pruned, like the played ledger and for the same
    // reason. resetPreferences() is what empties it.
    for (const QVariant &entry : m_favoriteSnapshots) {
        if (QString::compare(entry.toMap().value("name").toString(),
                             name, Qt::CaseInsensitive) == 0)
            return;
    }

    if (!favorited || !findGameByName(m_gameList, name).isEmpty())
        return;   // un-favourited, or owned and therefore already renderable

    QVariantMap snapshot = findGameByName(m_recommendationList, name);
    if (snapshot.isEmpty())
        snapshot = findGameByName(m_wishlist, name);
    // Played history last: a game hearted from the Played tab is in none of the
    // lists above, and a snapshot holding nothing but a name is not renderable
    // -- which is exactly what emptied the details page. Folded rows, not raw
    // ledger rows, so a title split across two play keys keeps its full total.
    if (snapshot.isEmpty())
        snapshot = findGameByName(playedGames(), name);
    // Hearted from the Browse page: the details it fetched are the only row.
    if (snapshot.isEmpty())
        snapshot = findGameByName(m_browseSnapshots, name);
    if (snapshot.isEmpty())
        snapshot = bareSnapshotFor(name);

    snapshot.remove("score");
    snapshot.remove("reason");
    snapshot.remove("inspiredBy");
    snapshot.remove("section");
    snapshot.remove("similarity");
    snapshot.remove("installed");   // a played row's momentary install state
    snapshot["status"] = 1.0;
    snapshot["addedAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    bindArtworkFromCache(m_baseDir, name, snapshot);
    prepareLiveArtwork(snapshot);

    m_favoriteSnapshots << snapshot;
    saveFavoriteSnapshots();
}

// ─────────────────────────────────────────────────────────────────────────────
// Removed games — the library minus what the user took out of it
//
// "Remove from library" is not "uninstall": the files stay exactly where they
// are and Steam still knows about the game. All that changes is that the
// launcher stops listing it. The scan rebuilds the library from the disk every
// time, so without a persisted record here the game would be back on the next
// rescan, which is to say within seconds.
//
// Identity is a record rather than a name because none of the three keys is
// sufficient on its own: appid is empty for local games, installDir moves if
// the user moves the folder, and the NAME of a local game is rewritten mid-scan
// when IGDB resolves it (see loadGames pass 2). Matching tries all three.
// ─────────────────────────────────────────────────────────────────────────────
fs::path VortexBridge::removedGamesPath() const {
    return m_baseDir / "removed_games.json";
}

void VortexBridge::loadRemovedGames() {
    m_removed.clear();

    QFile file(pathToQString(removedGamesPath()));
    if (!file.open(QIODevice::ReadOnly)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) return;

    for (const QJsonValue &value : doc.array()) {
        if (!value.isObject()) continue;
        const QVariantMap entry = value.toObject().toVariantMap();
        // A record with no identity at all would match everything, which is the
        // one failure mode worth guarding: it would empty the library.
        if (entry.value("name").toString().isEmpty()
            && entry.value("installDir").toString().isEmpty()
            && entry.value("appid").toInt() <= 0)
            continue;
        m_removed << entry;
    }
}

void VortexBridge::saveRemovedGames() const {
    QJsonArray array;
    for (const QVariant &entry : m_removed)
        array.append(QJsonObject::fromVariantMap(entry.toMap()));

    QFile file(pathToQString(removedGamesPath()));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
}

// Free function, taking the list by argument: the scan thread filters with a
// snapshot captured at scan start rather than reading the member underneath the
// main thread.
static bool isRemovedIn(const QVariantList &removed, const BridgeGame &bg) {
    if (removed.isEmpty()) return false;

    const QString installDir = QString::fromStdString(bg.installDir.string());
    const std::string canonical = make_canonical(bg.name);

    for (const QVariant &entry : removed) {
        const QVariantMap record = entry.toMap();

        const int appid = record.value("appid").toInt();
        if (appid > 0 && bg.appid > 0)
            { if (appid == bg.appid) return true; continue; }

        const QString dir = record.value("installDir").toString();
        if (!dir.isEmpty() && !installDir.isEmpty()) {
            // Case-insensitive: Windows paths that differ only in case are the
            // same folder, and the two strings come from different scans.
            if (QString::compare(dir, installDir, Qt::CaseInsensitive) == 0)
                return true;
            continue;
        }

        const QString name = record.value("name").toString();
        if (!name.isEmpty()
            && make_canonical(name.toStdString()) == canonical)
            return true;
    }
    return false;
}

bool VortexBridge::isRemovedName(const QString &name) const {
    if (m_removed.isEmpty() || name.isEmpty()) return false;
    const std::string canonical = make_canonical(name.toStdString());
    for (const QVariant &entry : m_removed) {
        const QString stored = entry.toMap().value("name").toString();
        if (!stored.isEmpty()
            && make_canonical(stored.toStdString()) == canonical)
            return true;
    }
    return false;
}

fs::path VortexBridge::wishlistPath() const {
    return m_baseDir / "wishlist.json";
}

void VortexBridge::loadWishlist() {
    m_wishlist.clear();

    QFile file(pathToQString(wishlistPath()));
    if (!file.open(QIODevice::ReadOnly)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) return;

    // Repaired on the way in, exactly as loadFavoriteSnapshots() does: the
    // stored art paths are absolute file:// URLs that break if the app is
    // moved, and a row saved by a build that still rebuilt the entry on a
    // re-add can be holding placeholders. ensureMetadata() covers what the
    // offline cache cannot answer, when the page opens.
    for (const QJsonValue &value : doc.array()) {
        if (!value.isObject()) continue;
        QVariantMap entry = value.toObject().toVariantMap();
        const QString name = entry.value("name").toString();
        bindArtworkFromCache(m_baseDir, name, entry);
        prepareLiveArtwork(entry);
        if (metadataIsBlank(entry)) {
            const long long id = igdb_cached_id_for(name.toStdString());
            if (id > 0)
                applyGameMetadata(entry, id);
        }
        m_wishlist << entry;
    }
}

void VortexBridge::saveWishlist() const {
    QJsonArray array;
    for (const QVariant &entry : m_wishlist)
        array.append(QJsonObject::fromVariantMap(entry.toMap()));

    QFile file(pathToQString(wishlistPath()));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
}

// ─────────────────────────────────────────────────────────────────────────────
// Played history
//
// playtime_stats.txt has always held every game that was ever played, including
// ones long since uninstalled -- it is only ever added to. Nothing displayed it:
// the details page reads a total for a game you are already looking at, and you
// can only look at games that still exist. The ledger below is what turns that
// file into something renderable, following wishlist.json and
// favorite_snapshots.json exactly -- an entry with no row in gameList has no
// artwork, no developer and no genres to draw a card from otherwise.
// ─────────────────────────────────────────────────────────────────────────────
fs::path VortexBridge::playedLedgerPath() const {
    return m_baseDir / "played_games.json";
}

// The three artwork slots, and the Images/ subfolder each one lives in.
static const std::pair<const char *, const char *> kPlayedArtSlots[] = {
    { "coverPath", "grid" }, { "heroPath", "hero" }, { "logoPath", "logo" }
};

void VortexBridge::loadPlayedLedger() {
    m_playedLedger.clear();

    QFile file(pathToQString(playedLedgerPath()));
    if (!file.open(QIODevice::ReadOnly)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) return;

    const fs::path imageRoot = m_baseDir / "Images";

    for (const QJsonValue &value : doc.array()) {
        if (!value.isObject()) continue;

        QVariantMap entry = value.toObject().toVariantMap();
        const QString name = entry.value("name").toString();
        if (entry.value("key").toString().isEmpty() || name.isEmpty())
            continue;

        // Artwork is re-resolved rather than trusted: the stored value is an
        // absolute file:// URL, so it stops working the moment the app is moved
        // to another folder. What is actually on disk wins, and the stored path
        // is the fallback for the case where Images/ no longer has it --
        // uninstallGame() leaves artwork alone, but removeLocalGameDirectory()
        // deletes it.
        const fs::path gameDir =
            imageRoot / steamgriddb_image_folder_name(name.toStdString());
        for (const auto &slot : kPlayedArtSlots) {
            const QString found = findImagePath(gameDir, slot.second);
            if (!found.isEmpty())
                entry[slot.first] = found;
        }
        prepareLiveArtwork(entry);

        m_playedLedger << entry;
    }
}

void VortexBridge::savePlayedLedger() const {
    QJsonArray array;
    for (const QVariant &entry : m_playedLedger)
        array.append(QJsonObject::fromVariantMap(entry.toMap()));

    QFile file(pathToQString(playedLedgerPath()));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
}

// The IGDB id behind a ledger row. An igdb_ key carries it outright; anything
// else has to go through the resolution cache, which is offline and is the same
// mapping the scan used when the game was still installed.
static long long playedIgdbId(const QString &key, const QString &name) {
    if (key.startsWith("igdb_")) {
        const long long id = key.mid(5).toLongLong();
        if (id > 0)
            return id;
    }
    return igdb_cached_id_for(name.toStdString());
}

// One pass over the history for rows written by an older build.
//
// Three things are wrong with them. Rows the stats file seeded carry "Unknown"
// in every metadata slot -- that is all playtime_stats.txt knows -- and nothing
// ever went back over them, so a game uninstalled long ago showed a blank
// details page forever. Rows of any age can carry the dropped-escape spelling
// of a name or a genre. And the oldest carry a playtime label in a format this
// build no longer writes, which the library grid now shows under every card
// rather than only in the badge on the Played tab. Rows that already hold real
// metadata keep it: those came from a live scan, which knows more than the
// caches do.
void VortexBridge::backfillPlayedLedgerMetadata() {
    bool changed = false;

    for (int i = 0; i < m_playedLedger.size(); ++i) {
        QVariantMap entry = m_playedLedger[i].toMap();
        const QVariantMap before = entry;

        // The ledger holds its own copy of the metadata, taken when the row was
        // written, so a row saved before json_read_string() existed keeps the
        // dropped-escape spelling ("Beat u0027em up") even though the caches it
        // came from now read back clean.
        for (const char *key : { "name", "developer", "genres", "tags" }) {
            const QString value = entry.value(key).toString();
            if (value.isEmpty())
                continue;
            const QString repaired = QString::fromStdString(
                json_repair_dropped_escapes(value.toStdString()));
            if (repaired != value)
                entry[key] = repaired;
        }

        // "playtime" is a label derived from "playtimeSeconds", and older builds
        // wrote it in formats formatPlaytimeLabel() never produces -- "5.1
        // Hours" against 18197 seconds, which is 5h 3m, and "2 Minutes" against
        // 170. Every writer since goes through formatPlaytimeLabel(), so only
        // rows that predate it disagree, and re-deriving settles the pair on the
        // seconds: that is the figure the tab sorts and sums on, and the only
        // one of the two that was ever authoritative.
        //
        // Deliberately not extended to idleTime or totalPlaytime. Both are
        // derived too, but neither disagrees with its seconds anywhere in the
        // ledger, and totalPlaytime's input is a sum rather than a stored field
        // -- recomputing it here would be guessing at a problem that does not
        // exist.
        if (entry.contains("playtimeSeconds")) {
            const QString derived =
                formatPlaytimeLabel(entry.value("playtimeSeconds").toLongLong());
            if (entry.value("playtime").toString() != derived)
                entry["playtime"] = derived;
        }

        if (metadataIsBlank(entry)) {
            const long long id = playedIgdbId(entry.value("key").toString(),
                                              entry.value("name").toString());
            if (id > 0)
                applyGameMetadata(entry, id);
        }

        if (entry == before)
            continue;

        m_playedLedger[i] = entry;
        changed = true;
    }

    if (changed)
        savePlayedLedger();
}

// Backfill from playtime_stats.txt for keys the ledger has never seen. Runs on
// every start, not just the first: the stats file is also written by the CLI,
// and a session recorded there while the launcher was closed would otherwise
// never reach the tab.
//
// These entries carry only what the stats file knows -- a name, a total and a
// date. Everything else fills itself in the first time the game is installed
// and scanned, which is when syncPlayedLedger() overwrites the row.
void VortexBridge::seedPlayedLedgerFromStats() {
    QSet<QString> known;
    for (const QVariant &entry : m_playedLedger)
        known.insert(entry.toMap().value("key").toString());

    const fs::path imageRoot = m_baseDir / "Images";
    bool changed = false;

    for (const PlayStat &stat : get_all_play_stats()) {
        if (stat.key.empty() || stat.seconds <= 0)
            continue;

        const QString key = QString::fromStdString(stat.key);
        if (known.contains(key))
            continue;

        // A stats row with no name is unrenderable as anything but its key; that
        // is still better than dropping the history on the floor.
        const QString name =
            QString::fromStdString(stat.name.empty() ? stat.key : stat.name);

        QVariantMap entry;
        entry["key"]  = key;
        entry["name"] = name;

        // The key encodes where the game came from, which is all that is left to
        // go on once it is gone from the disk. An igdb_ key says nothing about
        // its origin -- it is the identity Vortex resolved, not a store.
        if (key.startsWith("steam_")) {
            const int appid = key.mid(6).toInt();
            entry["source"]     = QStringLiteral("Steam");
            entry["appid"]      = appid;
            entry["steamAppId"] = appid;
        } else {
            entry["source"] = key.startsWith("local_") ? QStringLiteral("Local")
                                                       : QStringLiteral("Unknown");
            entry["appid"]      = 0;
            entry["steamAppId"] = 0;
        }

        // Derived the same way buildGameMap() derives them, so a game that is
        // still installed and one that is gone cannot disagree about how long
        // it was played. These rows are always Vortex's own figures -- the
        // stats file is the only thing left once a game is uninstalled -- so
        // idle always applies here.
        const long long ledgerIdle   = std::min(stat.idle_seconds, stat.seconds);
        const long long ledgerActive = stat.seconds - ledgerIdle;

        entry["playtimeSeconds"] = static_cast<qlonglong>(ledgerActive);
        entry["playtime"]        = formatPlaytimeLabel(ledgerActive);
        entry["idleTime"]        = formatPlaytimeLabel(ledgerIdle);
        entry["idleSeconds"]     = static_cast<qlonglong>(ledgerIdle);
        entry["idleDeducted"]    = true;
        entry["totalPlaytime"]   = formatPlaytimeLabel(stat.seconds);

        const QString lastPlayed = getLastPlayedDate(stat.key, m_baseDir);
        entry["lastPlayed"]   = lastPlayed;
        entry["lastPlayedAt"] = static_cast<qlonglong>(parseDateToEpoch(lastPlayed));

        const fs::path gameDir =
            imageRoot / steamgriddb_image_folder_name(name.toStdString());
        for (const auto &slot : kPlayedArtSlots)
            entry[slot.first] = findImagePath(gameDir, slot.second);

        applyGameMetadata(entry, playedIgdbId(key, name));
        entry["installDir"] = QString();
        entry["status"]     = 0.0;
        entry["matched"]    = false;

        m_playedLedger << entry;
        known.insert(key);
        changed = true;
    }

    if (changed)
        savePlayedLedger();
}

void VortexBridge::syncPlayedLedger() {
    QHash<QString, int> at;
    for (int i = 0; i < m_playedLedger.size(); ++i)
        at.insert(m_playedLedger[i].toMap().value("key").toString(), i);

    bool changed = false;

    for (const QVariant &entry : m_gameList) {
        const QVariantMap game = entry.toMap();
        const QString key = game.value("playKey").toString();
        if (key.isEmpty() || game.value("playtimeSeconds").toLongLong() <= 0)
            continue;

        QVariantMap snapshot = game;
        snapshot["key"] = key;
        // Stored as the game will look once it is gone: no install path, and not
        // matched to anything in the library. playedGames() puts the live values
        // back for as long as the game is actually installed.
        snapshot["installDir"] = QString();
        snapshot["matched"]    = false;
        // Not the live preference. Whether a game is hearted is answered from
        // preferences.json every time the details page asks, and carrying a copy
        // here would rewrite the file on every heart click for no gain.
        snapshot["status"] = 0.0;
        if (game.value("source").toString() == "Steam")
            snapshot["steamAppId"] = game.value("appid");

        const auto found = at.constFind(key);
        if (found == at.constEnd()) {
            at.insert(key, m_playedLedger.size());
            m_playedLedger << snapshot;
            changed = true;
        } else if (m_playedLedger[*found].toMap() != snapshot) {
            m_playedLedger[*found] = snapshot;
            changed = true;
        }
    }

    if (changed)
        savePlayedLedger();

    // Emitted either way: a game that was uninstalled since the last scan adds
    // nothing to the ledger, but it does change what the tab has to draw --
    // its row loses the INSTALLED badge and stops being launchable.
    emit playedGamesChanged();
}

// Everything ever played, live rows first so an installed game keeps its real
// install path and a working Play button. Computed on demand for the same
// reason favoriteGames() is: the alternative is a cached list that goes stale
// the moment a scan updates a row in place.
QVariantList VortexBridge::playedGames() const {
    QVariantList merged;
    QHash<QString, int> byName;   // canonical name -> index in merged
    QSet<QString> liveKeys;

    // One title, one card. playtime_stats.txt really does hold the same game
    // under two keys -- "Dungeon Village" is both igdb_27038 and igdb_19814,
    // from the title resolving differently on two different scans -- and two
    // identical cards side by side reads as a bug rather than as history.
    auto fold = [&](const QVariantMap &item) {
        const QString canonical = QString::fromStdString(
            make_canonical(item.value("name").toString().toStdString()));

        const auto found = byName.constFind(canonical);
        if (found == byName.constEnd()) {
            byName.insert(canonical, merged.size());
            merged << item;
            return;
        }

        const QVariantMap kept = merged[*found].toMap();

        // The installed copy owns the metadata -- it has the current artwork and
        // the resolved IGDB details -- but the totals are the sum of both, and
        // the date is whichever is later.
        QVariantMap winner = kept.value("installed").toBool() ? kept : item;
        winner["playtimeSeconds"] =
            kept.value("playtimeSeconds").toLongLong()
            + item.value("playtimeSeconds").toLongLong();
        winner["playtime"] = formatPlaytimeLabel(
            winner.value("playtimeSeconds").toLongLong());
        winner["installed"] = kept.value("installed").toBool()
                              || item.value("installed").toBool();

        const QVariantMap &later =
            item.value("lastPlayedAt").toLongLong()
                > kept.value("lastPlayedAt").toLongLong() ? item : kept;
        winner["lastPlayedAt"] = later.value("lastPlayedAt");
        winner["lastPlayed"]   = later.value("lastPlayed");

        merged[*found] = winner;
    };

    for (const QVariant &entry : m_gameList) {
        QVariantMap game = entry.toMap();
        if (game.value("playtimeSeconds").toLongLong() <= 0)
            continue;
        liveKeys.insert(game.value("playKey").toString());
        game["installed"] = true;
        fold(game);
    }

    for (const QVariant &entry : m_playedLedger) {
        QVariantMap item = entry.toMap();
        if (liveKeys.contains(item.value("key").toString()))
            continue;
        // m_gameList above is already filtered; the ledger is not, and it holds
        // a row for every game that was ever played -- including the one just
        // removed, which would otherwise reappear here as "uninstalled".
        if (isRemovedName(item.value("name").toString()))
            continue;
        // Marked played by hand while installed: the live row has the real
        // install path and a working Play button, so it stands in for the
        // snapshot taken at the click -- keeping the marker and its date.
        if (item.value("manual").toBool()) {
            QVariantMap live = findGameByName(m_gameList, item.value("name").toString());
            if (!live.isEmpty()) {
                live["installed"] = true;
                live["manual"]    = true;
                live["addedAt"]   = item.value("addedAt");
                fold(live);
                continue;
            }
        }
        item["installed"] = false;
        // Uninstalled, so the details page must not offer Play or Uninstall --
        // launchGameFrom() and uninstallGame() both search m_internalGames and
        // would return without a word. isOwned reads this.
        item["matched"] = false;
        fold(item);
    }

    // Most recently played first, then the biggest total, then the title. The
    // date alone is not enough to order by: everything that is not a Steam game
    // only knows the DAY it was last played (see parseDateToEpoch).
    //
    // A game marked played by hand has no date of its own, so it sorts by when
    // it was marked -- otherwise "Add to Played" would drop it at the bottom of
    // the tab, where nobody would see that the click worked.
    const auto sortAt = [](const QVariantMap &item) {
        const qlonglong at = item.value("lastPlayedAt").toLongLong();
        return at > 0 ? at : item.value("addedAt").toLongLong();
    };
    std::sort(merged.begin(), merged.end(),
              [&sortAt](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap();
        const QVariantMap y = b.toMap();

        const qlonglong xAt = sortAt(x);
        const qlonglong yAt = sortAt(y);
        if (xAt != yAt) return xAt > yAt;

        const qlonglong xSec = x.value("playtimeSeconds").toLongLong();
        const qlonglong ySec = y.value("playtimeSeconds").toLongLong();
        if (xSec != ySec) return xSec > ySec;

        return QString::compare(x.value("name").toString(),
                                y.value("name").toString(),
                                Qt::CaseInsensitive) < 0;
    });

    return merged;
}

// ─────────────────────────────────────────────────────────────────────────────
// App settings — a general object, so later toggles do not each grow a file.
// ─────────────────────────────────────────────────────────────────────────────
fs::path VortexBridge::settingsPath() const {
    return m_baseDir / "settings.json";
}

void VortexBridge::loadSettings() {
    // Absent, empty and malformed all mean "defaults", exactly as loadWishlist
    // treats a missing file. A settings file is the last thing that should be
    // able to stop the launcher starting.
    QFile file(pathToQString(settingsPath()));
    if (!file.open(QIODevice::ReadOnly)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return;

    const QJsonObject object = doc.object();
    if (object.contains("curatedOnly"))
        m_curatedOnly = object.value("curatedOnly").toBool(false);
    if (object.contains("ignorePlayedGames"))
        m_ignorePlayedGames = object.value("ignorePlayedGames").toBool(false);
    if (object.contains("ignoreLikedGames"))
        m_ignoreLikedGames = object.value("ignoreLikedGames").toBool(false);
    if (object.contains("useSteamPlaytime"))
        m_useSteamPlaytime = object.value("useSteamPlaytime").toBool(false);
    if (object.contains("trailersStartMuted"))
        m_trailersStartMuted = object.value("trailersStartMuted").toBool(false);
}

void VortexBridge::saveSettings() const {
    QJsonObject object;
    object["curatedOnly"] = m_curatedOnly;
    object["ignorePlayedGames"] = m_ignorePlayedGames;
    object["ignoreLikedGames"] = m_ignoreLikedGames;
    // stats_manager::use_steam_playtime() reads this same key straight off disk,
    // so the CLI shows whichever total the launcher is showing.
    object["useSteamPlaytime"] = m_useSteamPlaytime;
    object["trailersStartMuted"] = m_trailersStartMuted;

    QFile file(pathToQString(settingsPath()));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

// ─────────────────────────────────────────────────────────────────────────────
// setCuratedOnly — "only well-known games" toggle from the settings panel.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::setCuratedOnly(bool enabled) {
    if (enabled == m_curatedOnly)
        return;
    m_curatedOnly = enabled;
    saveSettings();
    emit curatedOnlyChanged();
    // Re-rank only. The filter narrows the candidate pool inside recommend.py;
    // nothing about the installed set has changed, so a disk rescan here would
    // cost seconds for an identical library section.
    loadRecommendations();
}

// ─────────────────────────────────────────────────────────────────────────────
// setIgnorePlayedGames — "ignore games you've played" toggle from the settings
// panel. Only the profile is affected: recommend.py still excludes the played
// set from the candidates, so this never starts recommending games back.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::setIgnorePlayedGames(bool enabled) {
    if (enabled == m_ignorePlayedGames)
        return;
    m_ignorePlayedGames = enabled;
    saveSettings();
    emit ignorePlayedGamesChanged();
    // Re-rank only, for the same reason setCuratedOnly() does: the profile is
    // rebuilt inside recommend.py and nothing on disk has changed.
    loadRecommendations();
}

// ─────────────────────────────────────────────────────────────────────────────
// setIgnoreLikedGames — "ignore games you've liked" toggle. The counterpart to
// setIgnorePlayedGames, and identical in every respect but which half of the
// profile it drops. Hearted games stay out of the candidates either way.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::setIgnoreLikedGames(bool enabled) {
    if (enabled == m_ignoreLikedGames)
        return;
    m_ignoreLikedGames = enabled;
    saveSettings();
    emit ignoreLikedGamesChanged();
    loadRecommendations();
}

// ─────────────────────────────────────────────────────────────────────────────
// setUseSteamPlaytime — "use Steam's own playtime" toggle from the settings
// panel. Changes which figure Steam games display, nothing else: the baseline
// import and the idle tracking run in both positions, so this can be flipped
// back and forth without losing a session or double counting one.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::setUseSteamPlaytime(bool enabled) {
    if (enabled == m_useSteamPlaytime)
        return;
    m_useSteamPlaytime = enabled;
    saveSettings();
    emit useSteamPlaytimeChanged();

    // Rebuild rather than re-rank, and no rescan: every number this changes is
    // already on disk, and the recommender was never given either of them.
    // refreshGameList() re-syncs the Played ledger on its way through.
    refreshGameList();
}

void VortexBridge::setTrailersStartMuted(bool enabled) {
    if (enabled == m_trailersStartMuted)
        return;
    m_trailersStartMuted = enabled;
    saveSettings();
    emit trailersStartMutedChanged();
}

// A row taken off the wishlist keeps its "wishlisted" flag at false rather than
// leaving m_wishlist. Absent means true: every entry written before the flag
// existed was, by definition, on the list.
static bool isWishlistedRow(const QVariantMap &entry) {
    return entry.value("wishlisted", true).toBool();
}

QVariantList VortexBridge::wishlistGames() const {
    QVariantList list;
    for (const QVariant &entry : m_wishlist) {
        if (isWishlistedRow(entry.toMap()))
            list << entry;
    }
    return list;
}

bool VortexBridge::isWishlisted(QString name) const {
    for (const QVariant &entry : m_wishlist) {
        const QVariantMap row = entry.toMap();
        if (QString::compare(row.value("name").toString(), name,
                             Qt::CaseInsensitive) == 0)
            return isWishlistedRow(row);
    }
    return false;
}

bool VortexBridge::toggleWishlist(QString name) {
    // Flagged rather than removed. wishlistGames() already hides an unflagged
    // row, so dropping it bought nothing and cost the only copy of the game's
    // art and metadata -- removing an entry and adding it straight back rebuilt
    // it from the recommendations, and for a game those have since moved past,
    // that meant a blank page under the user without the panel ever closing.
    // Added to and never pruned, like the favourite snapshots.
    for (int i = 0; i < m_wishlist.size(); ++i) {
        QVariantMap entry = m_wishlist[i].toMap();
        if (QString::compare(entry.value("name").toString(), name,
                             Qt::CaseInsensitive) != 0)
            continue;

        const bool saved = !isWishlistedRow(entry);
        entry["wishlisted"] = saved;
        if (saved)   // re-added: the list reads as when you saved it, not first saved it
            entry["addedAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        m_wishlist[i] = entry;
        saveWishlist();
        emit wishlistChanged();
        return saved;
    }

    // Snapshot the metadata: wishlist entries are unowned, so they are absent
    // from gameList and there would be nothing to render them from later. It
    // also keeps the tab working with Postgres stopped and no network.
    // Same order updateFavoriteSnapshot() builds from, and for the same reason:
    // a game can be saved from any tab, and only the list it is showing in has
    // a row to copy.
    QVariantMap snapshot = findGameByName(m_recommendationList, name);
    if (snapshot.isEmpty())
        snapshot = findGameByName(m_favoriteSnapshots, name);
    if (snapshot.isEmpty())
        snapshot = findGameByName(playedGames(), name);
    if (snapshot.isEmpty())
        snapshot = findGameByName(m_browseSnapshots, name);
    if (snapshot.isEmpty())
        snapshot = bareSnapshotFor(name);

    snapshot.remove("score");
    snapshot.remove("reason");
    snapshot.remove("inspiredBy");
    snapshot.remove("section");
    snapshot.remove("similarity");
    snapshot.remove("installed");   // a played row's momentary install state
    snapshot["wishlisted"] = true;
    snapshot["addedAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    bindArtworkFromCache(m_baseDir, name, snapshot);
    prepareLiveArtwork(snapshot);

    m_wishlist << snapshot;
    saveWishlist();
    emit wishlistChanged();
    // Deliberately no loadRecommendations() here: the wishlist is a saved list
    // and has no influence on ranking, so there is nothing to recompute.
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Browse — free search over the whole IGDB catalog
//
// IGDB is queried through igdb_query(), which is blocking WinHTTP like every
// other IGDB call here, so each request runs on its own thread and hands the
// raw JSON back to the main thread to parse. Steam's reviews need no key and
// go through m_network like the cover downloads do.
// ─────────────────────────────────────────────────────────────────────────────
static QString igdbImageUrl(const QString &imageId, const char *size) {
    if (imageId.isEmpty())
        return {};
    return QStringLiteral("https://images.igdb.com/igdb/image/upload/t_%1/%2.jpg")
               .arg(QLatin1String(size), imageId);
}

// Apicalypse strings are double-quoted; the query is whatever the user typed.
static std::string apicalypseQuoted(const QString &text) {
    std::string out;
    for (const char c : text.toStdString()) {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

static QStringList igdbNames(const QJsonValue &value) {
    QStringList names;
    for (const QJsonValue &entry : value.toArray()) {
        const QString name = entry.toObject().value("name").toString();
        if (!name.isEmpty())
            names << name;
    }
    return names;
}

// involved_companies is one list with a developer/publisher flag on each
// entry; `role` picks which of the two to collect.
static QStringList igdbCompanies(const QJsonValue &value, const char *role) {
    QStringList names;
    for (const QJsonValue &entry : value.toArray()) {
        const QJsonObject company = entry.toObject();
        if (!company.value(QLatin1String(role)).toBool())
            continue;
        const QString name = company.value("company").toObject().value("name").toString();
        if (!name.isEmpty() && !names.contains(name))
            names << name;
    }
    return names;
}

static QString hoursLabel(qlonglong seconds) {
    if (seconds <= 0)
        return {};
    const qlonglong hours = (seconds + 1800) / 3600;
    return hours > 0 ? QString::number(hours) + " Hours" : QStringLiteral("< 1 Hour");
}

// One grid card per game in an IGDB /games answer. Search results and new
// releases share this shape, so either can open BrowseDetails.
static QVariantList browseRows(const QByteArray &json, const QVariantList &library) {
    QVariantList rows;
    for (const QJsonValue &value : QJsonDocument::fromJson(json).array()) {
        const QJsonObject game = value.toObject();
        const QString name = game.value("name").toString();
        if (name.isEmpty())
            continue;

        QVariantMap row;
        row["igdbId"]   = game.value("id").toVariant().toLongLong();
        row["name"]     = name;
        row["coverUrl"] = igdbImageUrl(
            game.value("cover").toObject().value("image_id").toString(), "cover_big");
        const qlonglong released = game.value("first_release_date").toVariant().toLongLong();
        const QDate date = released > 0
            ? QDateTime::fromSecsSinceEpoch(released).date() : QDate();
        row["year"]        = date.isValid() ? QString::number(date.year()) : QString();
        row["releaseDate"] = date.isValid() ? date.toString("d MMM yyyy") : QString();
        row["developer"] = igdbCompanies(game.value("involved_companies"), "developer")
                               .value(0);
        row["genres"]    = igdbNames(game.value("genres")).join(", ");
        row["rating"]    = game.value("total_rating").toDouble(0.0);
        row["owned"]     = !findGameByName(library, name).isEmpty();
        rows << row;
    }
    return rows;
}

// What to tell the user when an igdb_query() for the grid throws.
static QString browseErrorMessage(const QString &error) {
    if (error.contains("credentials are not set"))
        return QStringLiteral("Browse needs IGDB keys. Add them in Settings.");
    if (error.contains("authentication"))
        return QStringLiteral("IGDB refused the saved keys. Check them in Settings.");
    return QStringLiteral("Could not reach IGDB. Check your connection.");
}

void VortexBridge::searchCatalog(QString query) {
    query = query.trimmed();
    if (query.isEmpty()) {
        clearBrowse();
        return;
    }
    const int seq = ++m_browseSearchSeq;

    m_browseSearching = true;
    m_browseStatus.clear();
    emit browseResultsChanged();

    const std::string body =
        "search \"" + apicalypseQuoted(query) + "\"; "
        "fields id,name,cover.image_id,first_release_date,total_rating,"
        "genres.name,involved_companies.company.name,involved_companies.developer; "
        "where version_parent = null; limit 50;";

    QThread *thread = QThread::create([this, seq, body]() {
        QByteArray json;
        QString error;
        try {
            json = QByteArray::fromStdString(igdb_query("games", body));
        } catch (const std::exception &e) {
            error = QString::fromStdString(e.what());
        }
        QMetaObject::invokeMethod(this, [this, seq, json, error]() {
            applyBrowseResults(seq, json, error);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void VortexBridge::clearBrowse() {
    ++m_browseSearchSeq;            // anything still in flight is now stale
    m_browseResults.clear();
    m_browseSearching = false;
    m_browseStatus.clear();
    emit browseResultsChanged();
}

void VortexBridge::applyBrowseResults(int seq, const QByteArray &json,
                                      const QString &error) {
    if (seq != m_browseSearchSeq)
        return;                     // superseded by a later keystroke

    m_browseSearching = false;
    m_browseResults.clear();

    if (!error.isEmpty()) {
        vlog::line("Browse", "Search failed: " + error.toStdString());
        m_browseStatus = browseErrorMessage(error);
        emit browseResultsChanged();
        return;
    }

    m_browseResults = browseRows(json, m_gameList);
    m_browseStatus = m_browseResults.isEmpty()
        ? QStringLiteral("No games found.") : QString();
    emit browseResultsChanged();
}

// Recent enough to count as new, and how many IGDB follows a game needs to
// make the list. Without the hype floor the window is mostly untracked
// shovelware released the same week.
static constexpr qint64 kNewReleaseWindowDays = 45;
static constexpr int    kNewReleaseMinHypes   = 3;
// A good list is reused for this long before the next visit refetches it.
static constexpr qint64 kNewReleaseRefreshMs  = 60LL * 60 * 1000;

void VortexBridge::loadNewReleases() {
    if (m_browseNewReleasesLoading)
        return;
    if (m_browseNewReleasesClock.isValid()
        && m_browseNewReleasesClock.elapsed() < kNewReleaseRefreshMs)
        return;

    m_browseNewReleasesLoading = true;
    m_browseNewReleasesStatus.clear();
    emit browseNewReleasesChanged();

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 since = now - kNewReleaseWindowDays * 24 * 60 * 60;
    // game_type = 0 is "main game"; see igdb_catalog.py on why not `category`.
    const std::string body =
        "fields id,name,cover.image_id,first_release_date,total_rating,"
        "genres.name,involved_companies.company.name,involved_companies.developer; "
        "where first_release_date <= " + std::to_string(now) +
        " & first_release_date >= " + std::to_string(since) +
        " & game_type = 0 & version_parent = null & cover != null"
        " & hypes >= " + std::to_string(kNewReleaseMinHypes) + "; "
        "sort first_release_date desc; limit 60;";

    QThread *thread = QThread::create([this, body]() {
        QByteArray json;
        QString error;
        try {
            json = QByteArray::fromStdString(igdb_query("games", body));
        } catch (const std::exception &e) {
            error = QString::fromStdString(e.what());
        }
        QMetaObject::invokeMethod(this, [this, json, error]() {
            applyNewReleases(json, error);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void VortexBridge::applyNewReleases(const QByteArray &json, const QString &error) {
    m_browseNewReleasesLoading = false;

    if (!error.isEmpty()) {
        // The clock is left alone, so the next visit to the tab retries.
        vlog::line("Browse", "New releases failed: " + error.toStdString());
        m_browseNewReleasesStatus = browseErrorMessage(error);
        emit browseNewReleasesChanged();
        return;
    }

    m_browseNewReleases = browseRows(json, m_gameList);
    m_browseNewReleasesStatus = m_browseNewReleases.isEmpty()
        ? QStringLiteral("No new releases found.") : QString();
    m_browseNewReleasesClock.start();
    emit browseNewReleasesChanged();
}

void VortexBridge::loadBrowseDetails(qlonglong igdbId) {
    if (igdbId <= 0)
        return;
    const int seq = ++m_browseDetailsSeq;

    // The grid row stands in until the full answer lands, so the page opens
    // on a title and a cover rather than on nothing.
    QVariantMap seed;
    for (const QVariantList *list : {&m_browseResults, &m_browseNewReleases}) {
        for (const QVariant &entry : *list) {
            const QVariantMap row = entry.toMap();
            if (row.value("igdbId").toLongLong() == igdbId) {
                seed = row;
                break;
            }
        }
        if (!seed.isEmpty())
            break;
    }
    // The grid row carries genres pre-joined; the page reads them as a list.
    if (seed.value("genres").typeId() == QMetaType::QString) {
        QStringList genres;
        for (const QString &genre : seed.value("genres").toString().split(',', Qt::SkipEmptyParts))
            genres << genre.trimmed();
        seed["genres"] = genres;
    }
    seed["igdbId"] = igdbId;

    // The banner and logo need only the name, which the grid row already
    // has, so their lookup starts now rather than once IGDB has answered: a
    // cached answer goes straight up, and SteamGridDB runs alongside IGDB.
    // Only Steam's CDN, the fallback, waits for IGDB's Steam app id.
    const QString name = seed.value("name").toString();
    const bool owned = !name.isEmpty() && !findGameByName(m_gameList, name).isEmpty();
    if (!name.isEmpty() && !owned) {
        const auto cached = m_liveArtCache.constFind(liveArtKey(name));
        if (cached != m_liveArtCache.constEnd()) {
            seed["bannerUrl"] = cached.value().value("liveHeroUrl").toString();
            seed["logoUrl"]   = cached.value().value("liveLogoUrl").toString();
        }
    }

    m_browseDetails = seed;
    m_browseDetailsLoading = true;
    emit browseDetailsChanged();
    releaseLiveArtAwaitingBrowse();     // the last page's game, if it had one

    m_browseReviews.clear();
    m_browseReviewsLoading = false;
    emit browseReviewsChanged();
    setBrowseTrailers({});

    if (!name.isEmpty() && !owned)
        resolveLiveArtwork(name, true);
    queryBrowseDetails(seq, igdbId);
}

void VortexBridge::loadBrowseDetailsForGame(QString name) {
    name = name.trimmed();
    if (name.isEmpty())
        return;
    const int seq = ++m_browseDetailsSeq;

    // The library row first, so an owned game seeds from its own record; the
    // recommendation row otherwise, which carries everything a Discover pick
    // has. Mapped to the keys the IGDB answer uses, so the page reads one shape.
    QVariantMap row = findGameByName(m_gameList, name);
    if (row.isEmpty())
        row = findGameByName(m_recommendationList, name);

    const auto known = [](const QString &value) {
        return (value.isEmpty() || value == "Unknown" || value == "N/A") ? QString() : value;
    };

    QVariantMap seed;
    seed["name"]       = row.isEmpty() ? name : row.value("name").toString();
    seed["coverUrl"]   = row.value("coverPath").toString();
    seed["developers"] = known(row.value("developer").toString());
    QStringList genres;
    for (const QString &genre : known(row.value("genres").toString()).split(',', Qt::SkipEmptyParts))
        genres << genre.trimmed();
    seed["genres"]      = genres;
    seed["ttbNormally"] = known(row.value("timeToBeat").toString());
    seed["totalRating"] = row.value("rating").toDouble();
    seed["steamAppId"]  = row.value("steamAppId").toInt();
    m_browseDetails = seed;
    m_browseDetailsLoading = true;
    emit browseDetailsChanged();
    releaseLiveArtAwaitingBrowse();     // the last page's game, if it had one

    m_browseReviews.clear();
    m_browseReviewsLoading = false;
    emit browseReviewsChanged();
    setBrowseTrailers({});

    // An owned game already knows its id from the scan; anything else goes
    // through the offline cache, then one lookup -- ensureMetadata()'s chain.
    long long igdbId = 0;
    if (row.value("matched", true).toBool()) {
        const std::string wanted = make_canonical(seed.value("name").toString().toStdString());
        for (const BridgeGame &bg : m_internalGames) {
            if (bg.igdb_id > 0 && make_canonical(bg.name) == wanted) {
                igdbId = bg.igdb_id;
                break;
            }
        }
    }
    if (igdbId <= 0)
        igdbId = igdb_cached_id_for(seed.value("name").toString().toStdString());
    if (igdbId > 0) {
        queryBrowseDetails(seq, igdbId);
        return;
    }

    // Blocking HTTPS, so off the UI thread like every other lookup in here.
    const std::string lookup = seed.value("name").toString().toStdString();
    QThread *thread = QThread::create([this, seq, lookup]() {
        long long resolved = 0;
        try {
            resolved = igdb_resolve_game(lookup, false).id;
        } catch (...) {}
        QMetaObject::invokeMethod(this, [this, seq, resolved]() {
            if (seq != m_browseDetailsSeq)
                return;                 // the page has moved on to another game
            if (resolved > 0)
                queryBrowseDetails(seq, resolved);
            else
                finishUnresolvedBrowseDetails(seq);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// IGDB has no answer for this name. The seed is everything there is to show,
// so it stays as it is -- no error, since none of it is wrong -- and a Steam
// game still gets its reviews from the app id the library already knows.
void VortexBridge::finishUnresolvedBrowseDetails(int seq) {
    if (seq != m_browseDetailsSeq)
        return;
    m_browseDetailsLoading = false;
    emit browseDetailsChanged();
    releaseLiveArtAwaitingBrowse();
    const int appId = m_browseDetails.value("steamAppId").toInt();
    if (appId > 0)
        fetchSteamReviews(seq, appId);
    loadBrowseTrailers(seq);
}

void VortexBridge::queryBrowseDetails(int seq, qlonglong igdbId) {
    // Asked already this session: the stored answer goes through the same
    // path a fresh one would, so ownership, art and the snapshot are worked
    // out again from what is true now -- only the round trip is skipped.
    const auto cached = m_browseDetailsCache.constFind(igdbId);
    if (cached != m_browseDetailsCache.constEnd()) {
        applyBrowseDetails(seq, cached.value().first, cached.value().second, QString());
        return;
    }

    const std::string id = std::to_string(igdbId);
    const std::string gameBody =
        "fields id,name,summary,storyline,first_release_date,"
        "total_rating,total_rating_count,aggregated_rating,aggregated_rating_count,"
        "rating,rating_count,cover.image_id,screenshots.image_id,"
        "videos.video_id,videos.name,"
        "artworks.image_id,artworks.width,artworks.height,"
        "genres.name,themes.name,game_modes.name,player_perspectives.name,"
        "platforms.name,involved_companies.company.name,"
        "involved_companies.developer,involved_companies.publisher,"
        "external_games.uid,external_games.external_game_source; "
        "where id = " + id + ";";
    const std::string ttbBody =
        "fields hastily,normally,completely; where game_id = " + id + ";";

    QThread *thread = QThread::create([this, seq, igdbId, gameBody, ttbBody]() {
        QByteArray game, ttb;
        QString error;
        try {
            game = QByteArray::fromStdString(igdb_query("games", gameBody));
            // Time to beat is a nicety; a failure here must not cost the page.
            try {
                ttb = QByteArray::fromStdString(igdb_query("game_time_to_beats", ttbBody));
            } catch (...) {}
        } catch (const std::exception &e) {
            error = QString::fromStdString(e.what());
        }
        QMetaObject::invokeMethod(this, [this, seq, igdbId, game, ttb, error]() {
            // Kept even when the page has moved on, since it is still right.
            // Not a failure, nor an answer missing its time to beat (an empty
            // reply, as against IGDB's "[]" for a game it has none for), so
            // the next open asks again rather than showing less all session.
            if (error.isEmpty() && !ttb.isEmpty()
                && !QJsonDocument::fromJson(game).array().at(0).toObject().isEmpty())
                m_browseDetailsCache.insert(igdbId, { game, ttb });
            applyBrowseDetails(seq, game, ttb, error);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void VortexBridge::applyBrowseDetails(int seq, const QByteArray &gameJson,
                                      const QByteArray &ttbJson, const QString &error) {
    if (seq != m_browseDetailsSeq)
        return;                     // the page has moved on to another game

    m_browseDetailsLoading = false;
    const QJsonObject game = QJsonDocument::fromJson(gameJson).array().at(0).toObject();
    if (!error.isEmpty() || game.isEmpty()) {
        vlog::line("Browse", "Details failed: " + error.toStdString());
        m_browseDetails["error"] = QStringLiteral("Could not load this game from IGDB.");
        emit browseDetailsChanged();
        releaseLiveArtAwaitingBrowse();     // no app id is coming
        // A recommendation's seed can already know its Steam app; the reviews
        // do not depend on IGDB, so they are still worth showing.
        const int appId = m_browseDetails.value("steamAppId").toInt();
        if (appId > 0)
            fetchSteamReviews(seq, appId);
        loadBrowseTrailers(seq);
        return;
    }

    const QString name = game.value("name").toString();
    const qlonglong igdbId = game.value("id").toVariant().toLongLong();

    QVariantMap d;
    d["igdbId"]    = igdbId;
    d["name"]      = name;
    d["summary"]   = game.value("summary").toString();
    d["storyline"] = game.value("storyline").toString();

    const qlonglong released = game.value("first_release_date").toVariant().toLongLong();
    if (released > 0) {
        const QDate date = QDateTime::fromSecsSinceEpoch(released).date();
        d["releaseDate"] = date.toString("d MMM yyyy");
        d["year"]        = QString::number(date.year());
    } else {
        d["releaseDate"] = QString();
        d["year"]        = QString();
    }

    const QStringList developers = igdbCompanies(game.value("involved_companies"), "developer");
    const QStringList genres  = igdbNames(game.value("genres"));
    const QStringList themes  = igdbNames(game.value("themes"));
    const QStringList modes   = igdbNames(game.value("game_modes"));
    d["developers"]   = developers.join(", ");
    d["publishers"]   = igdbCompanies(game.value("involved_companies"), "publisher").join(", ");
    d["genres"]       = genres;
    d["themes"]       = themes;
    d["modes"]        = modes;
    d["perspectives"] = igdbNames(game.value("player_perspectives"));
    d["platforms"]    = igdbNames(game.value("platforms"));

    d["totalRating"] = game.value("total_rating").toDouble(0.0);
    d["criticScore"] = game.value("aggregated_rating").toDouble(0.0);
    d["criticCount"] = game.value("aggregated_rating_count").toInt(0);
    d["userScore"]   = game.value("rating").toDouble(0.0);
    d["userCount"]   = game.value("rating_count").toInt(0);

    const QString coverUrl = igdbImageUrl(
        game.value("cover").toObject().value("image_id").toString(), "cover_big");
    d["coverUrl"] = coverUrl;

    QVariantList screenshots;
    for (const QJsonValue &shot : game.value("screenshots").toArray()) {
        const QString imageId = shot.toObject().value("image_id").toString();
        if (imageId.isEmpty())
            continue;
        QVariantMap one;
        one["thumb"] = igdbImageUrl(imageId, "screenshot_med");
        one["full"]  = igdbImageUrl(imageId, "1080p");
        screenshots << one;
    }
    d["screenshots"] = screenshots;

    // IGDB's videos are YouTube ids; the page plays them only when Steam has
    // no trailers of its own (browseTrailers).
    QVariantList youtubeVideos;
    for (const QJsonValue &entry : game.value("videos").toArray()) {
        const QJsonObject video = entry.toObject();
        const QString videoId = video.value("video_id").toString();
        if (videoId.isEmpty())
            continue;
        QVariantMap one;
        one["id"]    = videoId;
        one["name"]  = video.value("name").toString();
        one["thumb"] = QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(videoId);
        youtubeVideos << one;
    }
    d["youtubeVideos"] = youtubeVideos;

    // IGDB's banner, the last resort after SteamGridDB and Steam's CDN. Its
    // artworks are community uploads of any shape -- Portal 2's first is its
    // square icon -- so only a genuinely wide one counts, by FrostedHero's
    // 1.6 test; screenshots are the fallback.
    QString heroUrl;
    for (const QJsonValue &entry : game.value("artworks").toArray()) {
        const QJsonObject artwork = entry.toObject();
        if (artwork.value("width").toInt() > artwork.value("height").toInt() * 1.6) {
            heroUrl = igdbImageUrl(artwork.value("image_id").toString(), "1080p");
            if (!heroUrl.isEmpty())
                break;
        }
    }
    if (heroUrl.isEmpty() && !screenshots.isEmpty())
        heroUrl = screenshots.first().toMap().value("full").toString();
    d["heroUrl"] = heroUrl;

    // Steam is external_game_source 1 -- the same mapping igdb_resolve_game()
    // uses for its appid lookup.
    int steamAppId = 0;
    for (const QJsonValue &entry : game.value("external_games").toArray()) {
        const QJsonObject ext = entry.toObject();
        if (ext.value("external_game_source").toInt() != 1)
            continue;
        steamAppId = ext.value("uid").toString().toInt();
        if (steamAppId > 0)
            break;
    }
    d["steamAppId"] = steamAppId;

    const QJsonObject ttb = QJsonDocument::fromJson(ttbJson).array().at(0).toObject();
    const qlonglong ttbNormally = ttb.value("normally").toVariant().toLongLong();
    d["ttbHastily"]    = hoursLabel(ttb.value("hastily").toVariant().toLongLong());
    d["ttbNormally"]   = hoursLabel(ttbNormally);
    d["ttbCompletely"] = hoursLabel(ttb.value("completely").toVariant().toLongLong());

    // Owned under a different spelling (Steam's store name, a folder name):
    // hearts and the played ledger are keyed on the library's own name.
    const QVariantMap owned = findGameByName(m_gameList, name);
    d["ownedName"] = owned.value("name").toString();

    // The banner and logo ahead of IGDB's: an owned game's own Images/ art,
    // else the live lookup's -- SteamGridDB, then Steam's CDN for a Steam
    // game. Until that answers, artPending keeps IGDB's banner off screen so
    // it is not shown only to be swapped out.
    const QString key = liveArtKey(name);
    bool lookUp = false;
    if (!owned.isEmpty()) {
        d["bannerUrl"] = owned.value("heroPath").toString();
        d["logoUrl"]   = owned.value("logoPath").toString();
    } else {
        const auto cached = m_liveArtCache.constFind(key);
        if (cached != m_liveArtCache.constEnd()) {
            d["bannerUrl"] = cached.value().value("liveHeroUrl").toString();
            d["logoUrl"]   = cached.value().value("liveLogoUrl").toString();
        }
        lookUp = !name.isEmpty() && !liveArtCacheIsFresh(name);
    }
    d["artPending"] = lookUp
        && (m_liveArtPending.contains(key) || !m_liveArtAsked.contains(key));

    m_browseDetails = d;
    emit browseDetailsChanged();

    // Discover-shaped, so the wishlist, favourite and played paths can store
    // it exactly as they store a recommendation.
    QVariantMap snapshot;
    snapshot["name"]       = name;
    snapshot["igdbId"]     = igdbId;
    snapshot["source"]     = "IGDB";
    snapshot["developer"]  = developers.isEmpty() ? QStringLiteral("Unknown") : developers.first();
    snapshot["rating"]     = d.value("totalRating");
    snapshot["genres"]     = genres.isEmpty() ? QStringLiteral("Unknown") : genres.join(", ");
    const QStringList tags = genres + themes;
    snapshot["tags"]       = tags.isEmpty() ? QStringLiteral("Unknown") : tags.join(", ");
    snapshot["timeToBeat"] = ttbNormally > 0
                             ? QString::number(ttbNormally / 3600) + " Hours"
                             : QStringLiteral("N/A");
    snapshot["steamAppId"] = steamAppId;
    snapshot["coverUrl"]   = coverUrl;
    snapshot["coverPath"]  = coverUrl;
    snapshot["heroPath"]   = heroUrl.isEmpty() ? coverUrl : heroUrl;
    snapshot["logoPath"]   = QString();
    snapshot["releasedAt"] = d.value("releaseDate");
    snapshot["playtime"]   = "Not in library";
    snapshot["lastPlayed"] = "N/A";
    snapshot["installDir"] = QString();
    snapshot["status"]     = 0.0;
    snapshot["matched"]    = false;

    bool replaced = false;
    for (QVariant &entry : m_browseSnapshots) {
        if (entry.toMap().value("igdbId").toLongLong() == igdbId) {
            entry = snapshot;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        m_browseSnapshots << snapshot;

    // After the snapshot is in, so finishLiveArtwork() finds its Steam app id
    // -- both for a lookup starting only now and for one that started with
    // the page and has been waiting on that id.
    if (lookUp)
        resolveLiveArtwork(name, true);
    releaseLiveArtAwaitingBrowse(name);

    QVariantMap meta;
    meta["igdbId"]     = igdbId;
    meta["developer"]  = snapshot.value("developer");
    meta["rating"]     = d.value("totalRating");
    meta["ttbSeconds"] = ttbNormally;
    meta["genres"]     = genres;
    meta["themes"]     = themes;
    meta["modes"]      = modes;
    m_browseMetadata.insert(QString::fromStdString(make_canonical(name.toStdString())), meta);

    if (steamAppId > 0)
        fetchSteamReviews(seq, steamAppId);
    loadBrowseTrailers(seq);
}

// Steam's trailers for the open page: by IGDB's Steam app id when there is
// one, else by Steam's own store search, which finds the many games sold on
// Steam that IGDB has no Steam link for. The search is fuzzy -- "Alan Wake 2"
// answers with a Beat Saber DLC -- so only a canonical-name match counts.
void VortexBridge::loadBrowseTrailers(int seq) {
    if (seq != m_browseDetailsSeq)
        return;
    const int appId = m_browseDetails.value("steamAppId").toInt();
    if (appId > 0) {
        fetchSteamTrailers(seq, appId);
        return;
    }

    const QString name = m_browseDetails.value("name").toString();
    const QString key = QString::fromStdString(make_canonical(name.toStdString()));
    if (key.isEmpty())
        return;
    const auto cached = m_steamSearchCache.constFind(key);
    if (cached != m_steamSearchCache.constEnd()) {
        if (cached.value() > 0)
            fetchSteamTrailers(seq, cached.value());
        return;
    }

    setBrowseTrailers({}, true);
    if (!m_network) m_network = new QNetworkAccessManager(this);

    QUrl url(QStringLiteral("https://store.steampowered.com/api/storesearch/"));
    QUrlQuery query;
    query.addQueryItem("term", name);
    query.addQueryItem("cc", "us");
    query.addQueryItem("l", "english");
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "VortexLauncher/1.0");

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, seq, key]() {
        reply->deleteLater();
        // Not cached, so the next open asks Steam again.
        if (reply->error() != QNetworkReply::NoError) {
            if (seq == m_browseDetailsSeq)
                setBrowseTrailers({});
            return;
        }

        int found = 0;
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        for (const QJsonValue &value : root.value("items").toArray()) {
            const QJsonObject item = value.toObject();
            if (item.value("type").toString() == "app"
                && QString::fromStdString(make_canonical(
                       item.value("name").toString().toStdString())) == key) {
                found = item.value("id").toInt();
                break;
            }
        }
        m_steamSearchCache.insert(key, found);
        if (seq != m_browseDetailsSeq)
            return;
        if (found > 0)
            fetchSteamTrailers(seq, found);
        else
            setBrowseTrailers({});
    });
}

void VortexBridge::fetchSteamTrailers(int seq, int appId) {
    const auto cached = m_browseTrailersCache.constFind(appId);
    if (cached != m_browseTrailersCache.constEnd()) {
        setBrowseTrailers(cached.value());
        return;
    }

    setBrowseTrailers({}, true);
    if (!m_network) m_network = new QNetworkAccessManager(this);

    QUrl url(QStringLiteral("https://store.steampowered.com/api/appdetails"));
    url.setQuery(QStringLiteral("appids=%1&filters=movies").arg(appId));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "VortexLauncher/1.0");

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, seq, appId]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (seq == m_browseDetailsSeq)
                setBrowseTrailers({});
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject app = root.value(QString::number(appId)).toObject();

        // Steam moved its trailers to adaptive streams in 2025 and dropped
        // the mp4 links; older answers still carry them, and either plays.
        QVariantList trailers;
        for (const QJsonValue &value : app.value("data").toObject().value("movies").toArray()) {
            const QJsonObject movie = value.toObject();
            const QJsonObject mp4 = movie.value("mp4").toObject();
            QString stream = mp4.value("max").toString();
            if (stream.isEmpty())
                stream = mp4.value("480").toString();
            if (stream.isEmpty()) {
                if (!m_trailerServer)
                    m_trailerServer = new TrailerPlaylistServer(m_network, this);
                stream = m_trailerServer->localUrlFor(movie.value("hls_h264").toString());
            }
            if (stream.isEmpty())
                continue;
            QVariantMap one;
            one["name"]  = movie.value("name").toString();
            one["thumb"] = movie.value("thumbnail").toString();
            one["url"]   = stream;
            trailers << one;
        }
        // Kept even when the page has moved on, since it is still right.
        m_browseTrailersCache.insert(appId, trailers);
        if (seq == m_browseDetailsSeq)
            setBrowseTrailers(trailers);
    });
}

QVariantMap VortexBridge::trailerSeek(QString url, qint64 positionMs) const {
    return m_trailerServer ? m_trailerServer->seek(url, positionMs) : QVariantMap();
}

void VortexBridge::setBrowseTrailers(const QVariantList &trailers, bool loading) {
    if (trailers == m_browseTrailers && loading == m_browseTrailersLoading)
        return;
    m_browseTrailers = trailers;
    m_browseTrailersLoading = loading;
    emit browseTrailersChanged();
}

void VortexBridge::fetchSteamReviews(int seq, int appId) {
    // Fetched already this session: nothing in it depends on anything but
    // the app, so it goes straight up.
    const auto cached = m_browseReviewsCache.constFind(appId);
    if (cached != m_browseReviewsCache.constEnd()) {
        m_browseReviewsLoading = false;
        m_browseReviews = cached.value();
        emit browseReviewsChanged();
        return;
    }

    m_browseReviewsLoading = true;
    emit browseReviewsChanged();

    if (!m_network) m_network = new QNetworkAccessManager(this);

    QUrl url(QStringLiteral("https://store.steampowered.com/appreviews/%1").arg(appId));
    url.setQuery(QStringLiteral("json=1&language=english&filter=all"
                                "&purchase_type=all&num_per_page=10"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "VortexLauncher/1.0");

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, seq, appId]() {
        reply->deleteLater();

        QVariantMap result;
        const QJsonObject root = reply->error() == QNetworkReply::NoError
            ? QJsonDocument::fromJson(reply->readAll()).object() : QJsonObject();

        if (root.value("success").toInt() != 1) {
            // Not cached, so the next open asks Steam again.
            if (seq != m_browseDetailsSeq)
                return;
            m_browseReviewsLoading = false;
            result["error"] = QStringLiteral("Steam reviews are unavailable right now.");
            m_browseReviews = result;
            emit browseReviewsChanged();
            return;
        }

        const QJsonObject summary = root.value("query_summary").toObject();
        result["summary"]  = summary.value("review_score_desc").toString();
        result["positive"] = summary.value("total_positive").toInt();
        result["negative"] = summary.value("total_negative").toInt();
        result["total"]    = summary.value("total_reviews").toInt();

        QVariantList reviews;
        for (const QJsonValue &value : root.value("reviews").toArray()) {
            const QJsonObject review = value.toObject();
            const QString text = review.value("review").toString().trimmed();
            if (text.isEmpty())
                continue;
            QVariantMap one;
            one["text"]    = text;
            one["votedUp"] = review.value("voted_up").toBool();
            one["votesUp"] = review.value("votes_up").toInt();
            one["date"]    = QDateTime::fromSecsSinceEpoch(
                review.value("timestamp_created").toVariant().toLongLong())
                .date().toString("d MMM yyyy");
            const int minutes = review.value("author").toObject()
                                    .value("playtime_forever").toInt();
            one["hours"]   = QString::number(minutes / 60.0, 'f', 1);
            reviews << one;
        }
        result["reviews"] = reviews;
        // Kept even when the page has moved on, since it is still right.
        m_browseReviewsCache.insert(appId, result);
        if (seq != m_browseDetailsSeq)
            return;
        m_browseReviewsLoading = false;
        m_browseReviews = result;
        emit browseReviewsChanged();
    });
}

bool VortexBridge::isFavorite(QString name) const {
    return !name.isEmpty() && get_game_preference(name.toStdString()) > 0.0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Add to Played — history Vortex never saw
//
// The played ledger only ever learned about games from recorded playtime, so a
// game finished on a console or a friend's PC could never appear. These rows
// are flagged `manual`, carry no playtime, and are the only ledger rows that
// can be taken back off: recorded play is history, a click is a claim.
// ─────────────────────────────────────────────────────────────────────────────
QVariantMap VortexBridge::snapshotFromLists(const QString &name) const {
    QVariantMap snapshot = findGameByName(m_gameList, name);
    if (snapshot.isEmpty())
        snapshot = findGameByName(m_browseSnapshots, name);
    if (snapshot.isEmpty())
        snapshot = findGameByName(m_recommendationList, name);
    if (snapshot.isEmpty())
        snapshot = findGameByName(m_wishlist, name);
    if (snapshot.isEmpty())
        snapshot = findGameByName(m_favoriteSnapshots, name);
    if (snapshot.isEmpty())
        snapshot = bareSnapshotFor(name);
    return snapshot;
}

QString VortexBridge::playedState(QString name) const {
    const QVariantMap row = findGameByName(playedGames(), name);
    if (row.isEmpty())
        return QStringLiteral("none");
    if (row.value("playtimeSeconds").toLongLong() > 0)
        return QStringLiteral("tracked");
    return row.value("manual").toBool() ? QStringLiteral("manual")
                                        : QStringLiteral("none");
}

bool VortexBridge::togglePlayed(QString name) {
    name = name.trimmed();
    if (name.isEmpty())
        return false;

    const QString state = playedState(name);
    if (state == QLatin1String("tracked"))
        return false;

    const std::string wanted = make_canonical(name.toStdString());

    if (state == QLatin1String("manual")) {
        for (int i = m_playedLedger.size() - 1; i >= 0; --i) {
            const QVariantMap row = m_playedLedger[i].toMap();
            if (row.value("manual").toBool()
                && make_canonical(row.value("name").toString().toStdString()) == wanted)
                m_playedLedger.removeAt(i);
        }
        savePlayedLedger();
        writeManualPlayed();
        emit playedGamesChanged();
        return false;
    }

    QVariantMap row = snapshotFromLists(name);
    for (const char *key : { "score", "reason", "inspiredBy", "section",
                             "similarity", "installed", "wishlisted" })
        row.remove(key);
    const QString rowName = row.value("name").toString().isEmpty()
                            ? name : row.value("name").toString();

    // The IGDB id is what lets the recommender find the game's genres, and
    // what keeps the key apart from a stats-seeded igdb_ row for the same id.
    qlonglong igdbId = row.value("igdbId").toLongLong();
    if (igdbId <= 0)
        igdbId = igdb_cached_id_for(rowName.toStdString());

    row["key"] = igdbId > 0
        ? QStringLiteral("manual_igdb_%1").arg(igdbId)
        : QStringLiteral("manual_") + QString::fromStdString(wanted);
    row["name"]            = rowName;
    row["igdbId"]          = igdbId;
    row["manual"]          = true;
    row["addedAt"]         = static_cast<qlonglong>(QDateTime::currentSecsSinceEpoch());
    row["playtimeSeconds"] = 0;
    row["playtime"]        = formatPlaytimeLabel(0);
    row["lastPlayed"]      = QStringLiteral("N/A");
    row["lastPlayedAt"]    = 0;
    row["installDir"]      = QString();
    row["matched"]         = false;
    row["status"]          = 0.0;
    bindArtworkFromCache(m_baseDir, rowName, row);
    prepareLiveArtwork(row);

    // Browse already holds the full IGDB record; game_metadata.txt is where
    // sync_local_data.py reads genres from, and a game with none would add
    // nothing to the taste profile.
    QVariantMap meta = m_browseMetadata.value(QString::fromStdString(wanted));
    if (meta.isEmpty())
        meta = m_browseMetadata.value(
            QString::fromStdString(make_canonical(rowName.toStdString())));
    if (igdbId > 0 && !meta.isEmpty()) {
        const auto toStd = [](const QVariant &list) {
            std::vector<std::string> out;
            for (const QString &item : list.toStringList())
                out.push_back(item.toStdString());
            return out;
        };
        GameMetadata data;
        data.igdb_id              = igdbId;
        data.developer            = meta.value("developer").toString().toStdString();
        data.rating               = meta.value("rating").toDouble();
        data.time_to_beat_seconds = meta.value("ttbSeconds").toLongLong();
        data.all_genres           = toStd(meta.value("genres"));
        data.themes               = toStd(meta.value("themes"));
        data.game_modes           = toStd(meta.value("modes"));
        save_game_metadata(data);
    }

    m_playedLedger << row;
    savePlayedLedger();
    writeManualPlayed();
    emit playedGamesChanged();
    // No loadRecommendations(), same as a heart: the next refresh picks it up.
    return true;
}

void VortexBridge::writeManualPlayed() const {
    std::ofstream out(m_baseDir / "manual_played.txt");
    if (!out) return;

    out << "# Games marked played by hand from the Browse page.\n";
    out << "# Written by the launcher; read by analytics/sync_local_data.py.\n";
    out << "# Format: NAME|IGDB_ID|ADDED_AT_EPOCH\n";
    for (const QVariant &entry : m_playedLedger) {
        const QVariantMap row = entry.toMap();
        if (!row.value("manual").toBool())
            continue;
        QString name = row.value("name").toString();
        name.replace('|', ' ');
        out << name.toStdString() << "|" << row.value("igdbId").toLongLong()
            << "|" << row.value("addedAt").toLongLong() << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// initialize — called by the QML mood picker; stores mood then kicks off scan.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::initialize(int mood) {
    m_currentMood = mood;
    emit moodChanged();
    loadGames();
}

// ─────────────────────────────────────────────────────────────────────────────
// setMood — mood change from the settings panel, after startup.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::setMood(int mood) {
    if (mood == m_currentMood)
        return;
    m_currentMood = mood;
    emit moodChanged();
    // Debounced, so clicking through all four chips to see what they do costs
    // one Python run rather than four.
    loadRecommendations();
}

// ─────────────────────────────────────────────────────────────────────────────
// loadGames — full scan on a background QThread, in three passes.
//
//  1. Filesystem only: Steam manifests + local_game_dirs.txt. Publishes the
//     game list immediately, so the library is on screen and usable while the
//     rest runs.
//  2. IGDB: resolve each title and pull its metadata.
//  3. SteamGridDB: download missing artwork, reporting per-game progress.
//
// Passes 2 and 3 update rows in place (updateGameRow) instead of republishing
// the list, so the grid is never rebuilt underneath the user.
//
// This used to be one pass that fetched everything before showing anything,
// behind a full-screen modal spinner. On a machine with cold caches that meant
// minutes of an app that looked hung.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::loadGames() {
    // Guard: only one scan at a time. A request that arrives mid-scan (e.g. a
    // folder added from the settings panel) is queued instead of dropped --
    // otherwise the in-flight scan, which started before the new folder was
    // written, would leave those games missing until the next launch.
    //
    // A rename still resolving holds the scan back the same way: its playtime
    // rekey has to land before the Steam baseline import below looks for a row
    // under the new key, or Steam's total is imported a second time.
    if (m_isLoading || m_renamesInFlight > 0) {
        m_rescanQueued = true;
        return;
    }
    setLoading(true);
    setScanProgress(true, "Finding games", 0, 0);

    const fs::path baseDir    = m_baseDir;   // capture by value for lambda
    const fs::path imagesRoot = baseDir / "Images";
    // Snapshot, not the member: the scan thread must not read m_removed while
    // the main thread could be appending to it. A removal made during the scan
    // is picked up by the rescan removeFromLibrary() queues.
    const QVariantList removedSnapshot = m_removed;

    QThread *thread = QThread::create([this, baseDir, imagesRoot, removedSnapshot]() {
        // Whatever happens below -- an exception, or an early return added
        // later -- the UI must not be left with a progress strip that never
        // clears and a loading flag that never drops. A scope guard rather
        // than a call before each return, because the latter is what rots.
        struct ScanGuard {
            VortexBridge *bridge;
            // Disarmed once the normal delivery below takes over clearing the
            // state. Without this the guard still fires after delivery, and
            // because delivery may start a queued rescan, its clean-up would
            // land on the NEW scan and switch off a progress strip that had
            // just legitimately come back on.
            bool armed = true;

            ~ScanGuard() {
                if (!armed) return;
                QMetaObject::invokeMethod(bridge, [b = bridge]() {
                    b->setScanProgress(false, QString(), 0, 0);
                    if (b->isLoading()) b->setLoading(false);
                }, Qt::QueuedConnection);
            }
        } guard{ this };

        const auto scanStart = std::chrono::steady_clock::now();

        // --- Pass 1: filesystem only -------------------------------------
        // Deliberately no IGDB resolution here. That is a network round trip
        // per game and used to sit between the user and seeing anything at
        // all; the titles are already known from Steam manifests and folder
        // names, so there is no reason to wait for the network to show them.
        vlog::phase("Scanning for games");

        std::vector<SteamGame> steamGames = read_installed_steam_games(false);
        std::vector<temp_GameEntry> localEntries;
        for (const fs::path &dir : readLocalGameDirectories(baseDir))
            scan_directory_for_games(dir, localEntries, false);
        // Riot's own install list, so VALORANT is found wherever the Riot
        // Client put it, no folder needed.
        const std::vector<RiotGame> riotGames = read_installed_riot_games(true);
        std::vector<bool> riotListed(riotGames.size(), false);

        // Games the user removed from the library are dropped HERE, before
        // either vector exists, rather than when the QVariantList is built:
        // updateGameRow() writes m_gameList by position, so m_internalGames and
        // m_gameList have to stay index-parallel, and they only do if both are
        // built from the same filtered set.
        // The user's own titles go on here, in pass 1, so every pass after it
        // -- the IGDB search, the SteamGridDB search, the artwork folder --
        // works from the name the user chose.
        const QHash<QString, std::string> overrides = readNameOverrides();
        auto applyOverride = [&overrides](BridgeGame &bg) {
            bg.scannedName = bg.name;
            const auto it = overrides.constFind(overrideKey(bg.installDir));
            if (it != overrides.constEnd()) {
                bg.name       = *it;
                bg.customName = *it;
            }
        };

        std::vector<BridgeGame> internalGames;
        for (const SteamGame &g : steamGames) {
            BridgeGame bg;
            bg.name       = g.name;
            bg.source     = "Steam";
            bg.appid      = g.appid;
            bg.igdb_id    = g.igdb_id;
            bg.installDir = g.installDir;
            if (isRemovedIn(removedSnapshot, bg)) continue;
            applyOverride(bg);
            internalGames.push_back(std::move(bg));
        }
        for (const temp_GameEntry &g : localEntries) {
            BridgeGame bg;
            bg.name       = g.name;
            bg.source     = "Local";
            bg.igdb_id    = g.igdb_id;
            bg.installDir = g.installDir;
            bg.gamePath   = g.gamePath;
            // A local folder that is a Riot install (D:\Riot Games added as a
            // game folder) is that game, not a second copy of it. Marked
            // before the removed check, so a removed one stays removed.
            for (size_t r = 0; r < riotGames.size() && !bg.isRiot; ++r)
                if (riot_install_matches(riotGames[r], bg.installDir)) {
                    bg.isRiot = riotListed[r] = true;
                    bg.name   = riotGames[r].title; // not the folder's name
                }
            if (isRemovedIn(removedSnapshot, bg)) continue;
            // The same test Play uses, so the card's EA mark means it will
            // launch through the EA app and not just that EA made it.
            EaGame ea;
            bg.isEa       = !bg.isRiot && read_ea_install(bg.installDir, ea);
            applyOverride(bg);
            internalGames.push_back(std::move(bg));
        }
        int riotCount = 0;
        for (size_t r = 0; r < riotGames.size(); ++r) {
            if (riotListed[r]) continue;
            const RiotGame &g = riotGames[r];
            // Filed as Local, like an EA install: the filters, playtime keys
            // and analytics need nothing new, and the card's mark says Riot.
            BridgeGame bg;
            bg.name       = g.title;
            bg.source     = "Local";
            bg.installDir = g.installDir;
            bg.gamePath   = g.clientExe; // what Play actually starts
            bg.isRiot     = true;
            if (isRemovedIn(removedSnapshot, bg)) continue;
            applyOverride(bg);
            internalGames.push_back(std::move(bg));
            ++riotCount;
        }

        const int total = static_cast<int>(internalGames.size());
        vlog::line("Scan", std::to_string(steamGames.size()) + " Steam + " +
                           std::to_string(localEntries.size()) + " local + " +
                           std::to_string(riotCount) + " Riot = " +
                           std::to_string(total) + " games");

        // Publish now. From here the library is on screen and usable; the two
        // passes below only enrich rows that already exist.
        {
            QVariantList list;
            list.reserve(total);
            for (const BridgeGame &bg : internalGames)
                list << buildGameMap(bg);

            QMetaObject::invokeMethod(this, [this, list, internalGames]() mutable {
                m_internalGames = internalGames;
                m_gameList      = list;
                invalidateGameIndex();
                // Steam totals are already on these rows, so a game played
                // outside Vortex enters the ledger here -- before the details
                // pass has had a chance to rename anything.
                syncPlayedLedger();
                emit gameListChanged();
                emit favoritesChanged();
            }, Qt::QueuedConnection);
        }

        // --- Pass 2: IGDB resolution and metadata -------------------------
        reportScanProgress("Fetching details", 0, total);
        vlog::phase("Game details (IGDB)", total);

        for (int i = 0; i < total; ++i) {
            BridgeGame &bg = internalGames[i];
            resolveIgdb(bg);
            updateGameRow(i, bg);
            reportScanProgress("Fetching details", i + 1, total);
        }

        // --- Steam baseline import ----------------------------------------
        // Take Steam's lifetime total for any game Vortex has not accounted for
        // yet, so a library that was played for years before the launcher
        // existed does not read as never played.
        //
        // This has to sit AFTER pass 2. A Steam game that IGDB resolved is
        // filed under "igdb_<id>", and importing under the "steam_<appid>" key
        // it carried before resolution would strand the hours under a key
        // record_play_session() never writes to.
        //
        // It also runs whatever the "use Steam's own playtime" setting says.
        // Skipping it while that setting is on would let the first session
        // recorded in that mode create the row, closing the import window for
        // good -- and turning the setting off afterwards would then show a
        // total with every pre-Vortex hour missing.
        //
        // A no-op once a game has a baseline, so this is one stats read per
        // Steam game per scan and cannot double count.
        for (const BridgeGame &bg : internalGames) {
            if (bg.source != "Steam" || bg.appid <= 0) continue;
            if (import_steam_baseline(makePtKey(bg), bg.name,
                                      get_steam_playtime_seconds(bg.appid)))
                vlog::line("Scan", "imported Steam playtime for " + bg.name);
        }

        // --- PrismLauncher history import ---------------------------------
        // Vortex follows Minecraft only from the first time it launches Prism;
        // everything before that is in Prism's own records. Taken once, the
        // first time the install is seen, and never again: from then on the
        // launcher tracking is the record, and re-reading Prism would count
        // every Vortex session a second time.
        //
        // Real sessions come first -- Minecraft keeps one log per launch, and
        // their spans reproduce Prism's totals to the second -- so the
        // recommender gets dates to weight, not one lump. Whatever Prism
        // counts beyond the surviving logs becomes the baseline, the same as
        // a Steam import.
        //
        // After pass 2 for the same reason as the Steam import: the key must
        // be the IGDB one record_play_session() writes under.
        for (const BridgeGame &bg : internalGames) {
            if (bg.source != "Local" || bg.installDir.empty()) continue;
            std::error_code ec;
            if (!fs::exists(bg.installDir / "prismlauncher.exe", ec)) continue;

            const std::string key = makePtKey(bg);
            if (key.empty() || launcher_history_imported(key)) continue;

            QString details, output;
            const fs::path script = analyticsDir(m_baseDir) / "prism_history.py";
            if (!runPythonScript(script, &details, &output,
                                 QStringList{ QString::fromStdString(bg.installDir.string()) },
                                 60000)) {
                // Not marked, so the next scan tries again -- a missing
                // interpreter or an absent data folder is not "nothing to import".
                vlog::item("Scan", bg.name + " (PrismLauncher history)",
                           vlog::Status::Fail,
                           details.isEmpty() ? "no output" : details.trimmed().toStdString());
                continue;
            }

            long long prismTotal = 0;
            std::vector<std::pair<std::time_t, std::time_t>> found;
            for (const QString &line : output.split('\n', Qt::SkipEmptyParts)) {
                const QStringList parts = line.trimmed().split(' ', Qt::SkipEmptyParts);
                if (parts.size() >= 2 && parts[0] == "TOTAL")
                    prismTotal = parts[1].toLongLong();
                else if (parts.size() >= 3 && parts[0] == "SESSION")
                    found.emplace_back(static_cast<std::time_t>(parts[1].toLongLong()),
                                       static_cast<std::time_t>(parts[2].toLongLong()));
            }

            // A session Vortex already recorded is its own; the log's copy of
            // it would only count it twice.
            const auto existing = recorded_sessions(key);
            int imported = 0;
            long long loggedSeconds = 0;
            for (const auto &[start, end] : found) {
                const bool overlaps = std::any_of(
                    existing.begin(), existing.end(), [&](const auto &s) {
                        return start < s.second && s.first < end;
                    });
                if (overlaps || end <= start) continue;
                record_play_session(key, bg.name, start, end, 0);
                loggedSeconds += static_cast<long long>(end - start);
                ++imported;
            }

            // The "row exists, baseline 0" branch: Prism's total beyond what
            // is now recorded is play no surviving log accounts for. Launcher-
            // neutral despite the name -- it only compares two totals.
            const long long before = get_play_stat(key).seconds;
            import_steam_baseline(key, bg.name, prismTotal);
            const long long unlogged = get_play_stat(key).seconds - before;

            mark_launcher_history_imported(key, "prism");
            vlog::line("Scan", "imported " + bg.name + " history from PrismLauncher: " +
                               std::to_string(imported) + " sessions (" +
                               vlog::duration(loggedSeconds) + ") from game logs" +
                               (unlogged > 0 ? " + " + vlog::duration(unlogged) +
                                               " with no log"
                                             : std::string()) +
                               " -- total " + vlog::duration(get_play_stat(key).seconds) +
                               "; Vortex tracks it from here");
        }

        // Real outcomes, not a separate poll: whether the keys WORK is a
        // different question from whether they are present, and only the
        // calls that actually went out can answer it.
        {
            const bool authOk = igdb_last_auth_ok();
            QMetaObject::invokeMethod(this, [this, authOk]() {
                if (m_igdbAuthOk != authOk) {
                    m_igdbAuthOk = authOk;
                    emit credentialsChanged();
                }
            }, Qt::QueuedConnection);
        }

        // --- Pass 3: artwork ----------------------------------------------
        std::vector<std::string> allNames;
        allNames.reserve(internalGames.size());
        for (const BridgeGame &bg : internalGames)
            allNames.push_back(bg.name);

        reportScanProgress("Downloading artwork", 0, total);
        if (!allNames.empty()) {
            ensure_steamgriddb_images(
                allNames, imagesRoot.string(),
                [this](int done, int count, const std::string &) {
                    reportScanProgress("Downloading artwork", done, count);
                });
        }

        // SteamGridDB has no hero for some games that Steam itself ships one
        // for, which left their banner as bare fill behind the logo.
        {
            std::vector<SteamHeroRequest> steamHeroes;
            for (const BridgeGame &bg : internalGames)
                if (bg.source == "Steam" && bg.appid > 0)
                    steamHeroes.push_back({ bg.name, bg.appid });
            ensure_steam_hero_fallback(steamHeroes, imagesRoot.string());
        }

        {
            const bool artOk = steamgriddb_last_auth_ok();
            QMetaObject::invokeMethod(this, [this, artOk]() {
                if (m_artworkAuthOk != artOk) {
                    m_artworkAuthOk = artOk;
                    emit credentialsChanged();
                }
            }, Qt::QueuedConnection);
        }

        // Artwork changed the files buildGameMap() resolves paths against, so
        // every row is rebuilt once here rather than once per download.
        for (int i = 0; i < total; ++i)
            updateGameRow(i, internalGames[i]);

        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - scanStart).count();
        vlog::phase_done("Scan", elapsed, total, 0, 0, 0);

        // --- Pass 4: push the library into the analytics database ----------
        // Without this the database stays empty on a fresh install: the sync
        // used to run only after a play session, so a new user got
        // "ML unavailable" until they had played and quit a game. Runs here,
        // on the scan thread, because the delivery below is on the main thread
        // and this spawns an interpreter.
        //
        // writeInstalledGames() has to precede it -- sync_local_data.py reads
        // installed_games.txt to decide what is still installed.
        writeInstalledGames(baseDir, internalGames);

        {
            const std::optional<fs::path> syncScript =
                existingPath(analyticsDir(baseDir) / "sync_local_data.py");
            if (syncScript) {
                vlog::phase("Sync to database");
                QString syncDetails, syncOut;
                const bool syncOk = runPythonScript(*syncScript, &syncDetails, &syncOut);
                if (syncOk) {
                    vlog::item("Sync", "sync_local_data.py", vlog::Status::Ok,
                               syncOut.trimmed().remove('\r').replace('\n', " | ").toStdString());
                } else {
                    // A failed sync means the recommender is about to run
                    // against stale or absent data, so the reason has to be in
                    // the log rather than inferred from a bad result.
                    vlog::item("Sync", "sync_local_data.py", vlog::Status::Fail,
                               syncDetails.isEmpty() ? "no output"
                                                     : syncDetails.toStdString());
                }
            }
        }

        // --- Deliver -------------------------------------------------------
        // From here the block below owns clearing the scan state; the guard
        // exists only for the paths that never reach this point.
        guard.armed = false;

        QMetaObject::invokeMethod(this, [this, internalGames]() mutable {
            m_internalGames = std::move(internalGames);
            setScanProgress(false, QString(), 0, 0);
            setLoading(false);

            // Again now that the rows are final: pass 2 resolves IGDB ids (which
            // changes the playtime key) and renames local games, and pass 3 fills
            // in the artwork the ledger keeps its own copy of.
            syncPlayedLedger();

            if (m_rescanQueued) {
                m_rescanQueued = false;
                loadGames();          // picks up folders added during this scan
            } else {
                // installed_games.txt and the database sync already happened
                // on the scan thread above, so the recommender is reading
                // current data by the time it starts.
                loadRecommendations();
            }
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// Replace one row in place and schedule a coalesced notify.
//
// In place, and WITHOUT gameListChanged: the library grid binds to a plain JS
// array, so a notify hands GridView a new array, rebuilds every delegate and
// throws away the scroll position and controller focus. Doing that once per
// downloaded cover would make the grid unusable for the whole scan -- which is
// the opposite of the point. Mutating the map and bumping artRevision instead
// re-evaluates only the bindings that mention it.
void VortexBridge::updateGameRow(int index, const BridgeGame &game) {
    QVariantMap map = buildGameMap(game);
    QMetaObject::invokeMethod(this, [this, index, map]() {
        if (index < 0 || index >= m_gameList.size()) return;
        m_gameList[index] = map;
        invalidateGameIndex();
        scheduleArtNotify();
    }, Qt::QueuedConnection);
}

void VortexBridge::scheduleArtNotify() {
    if (!m_artNotifyTimer) {
        m_artNotifyTimer = new QTimer(this);
        m_artNotifyTimer->setSingleShot(true);
        m_artNotifyTimer->setInterval(250);
        connect(m_artNotifyTimer, &QTimer::timeout, this, [this]() {
            ++m_artRevision;
            emit artRevisionChanged();
        });
    }
    if (!m_artNotifyTimer->isActive())
        m_artNotifyTimer->start();
}

void VortexBridge::setScanProgress(bool active, const QString &phase, int done,
                                   int total) {
    if (m_scanActive == active && m_scanPhase == phase &&
        m_scanDone == done && m_scanTotal == total)
        return;

    m_scanActive = active;
    m_scanPhase  = phase;
    m_scanDone   = done;
    m_scanTotal  = total;
    emit scanProgressChanged();
}

// Callable from the scan thread; marshals onto the main thread.
void VortexBridge::reportScanProgress(const QString &phase, int done, int total) {
    QMetaObject::invokeMethod(this, [this, phase, done, total]() {
        setScanProgress(true, phase, done, total);
    }, Qt::QueuedConnection);
}

// First occurrence wins for both keys, as the linear scans these replace did.
void VortexBridge::ensureGameIndex() const {
    if (m_gameIndexValid)
        return;
    m_rowByInstallDir.clear();
    m_rowByName.clear();
    m_rowByInstallDir.reserve(m_gameList.size());
    m_rowByName.reserve(m_gameList.size());
    for (int i = 0; i < m_gameList.size(); ++i) {
        const QVariantMap map = m_gameList[i].toMap();
        const QString dir = map.value("installDir").toString();
        if (!dir.isEmpty() && !m_rowByInstallDir.contains(dir))
            m_rowByInstallDir.insert(dir, i);
        const QString name = map.value("name").toString();
        if (!m_rowByName.contains(name))
            m_rowByName.insert(name, i);
    }
    m_gameIndexValid = true;
}

QSize VortexBridge::decodeSize(QString source, qreal width, qreal height,
                               bool crop, qreal dpr) const {
    const QSize natural(-1, -1);
    const QUrl url(source);
    if (!url.isLocalFile() || width <= 0 || height <= 0 || dpr <= 0)
        return natural;

    const QSize original = QImageReader(url.toLocalFile()).size();
    if (original.isEmpty())
        return natural;

    const qreal rw = width * dpr / original.width();
    const qreal rh = height * dpr / original.height();
    const qreal ratio = crop ? qMax(rw, rh) : qMin(rw, rh);
    if (ratio >= 1.0)
        return natural;

    // Both sides at the one ratio, so Qt's own fit/crop arithmetic lands on
    // exactly this size. Logical, since Qt multiplies sourceSize by the dpr;
    // rounded up so the decode never comes out a pixel short of the box.
    return QSize(qCeil(original.width() * ratio / dpr),
                 qCeil(original.height() * ratio / dpr));
}

QVariantMap VortexBridge::gameDetailsFor(QString name) const {
    ensureGameIndex();
    const auto found = m_rowByName.constFind(name);
    if (found == m_rowByName.constEnd() || *found >= m_gameList.size())
        return {};
    return m_gameList[*found].toMap();
}

// Keyed on install directory, which -- unlike the title -- is decided in pass 1
// and never changes afterwards. Delegates hold the pass-1 snapshot of a row
// (the grid is deliberately not rebuilt when artwork lands, see updateGameRow),
// so a lookup by modelData.name misses for every local game whose folder name
// differs from the canonical IGDB title the resolve pass swaps in.
QVariantMap VortexBridge::gameDetailsForInstallDir(QString installDir) const {
    if (installDir.isEmpty())
        return {};

    ensureGameIndex();
    const auto found = m_rowByInstallDir.constFind(installDir);
    if (found == m_rowByInstallDir.constEnd() || *found >= m_gameList.size())
        return {};
    return m_gameList[*found].toMap();
}

// Append one feedback event as NDJSON.
//
// The launcher links only Qt and winhttp — there is no libpq here — so C++
// writes a line-delimited log and sync_local_data.py folds it into
// recommendation_events on its next run. Keeping every database access on the
// Python side matches how the rest of the analytics pipeline already works.
void VortexBridge::appendFeedbackEvent(const QVariantMap &fields) {
    QJsonObject object = QJsonObject::fromVariantMap(fields);
    object["at"] = QDateTime::currentDateTime().toString(Qt::ISODate);

    const fs::path path = analyticsDir(m_baseDir) / "feedback_events.log";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    QFile file(pathToQString(path));
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.write("\n");
}

// Comma-joined labels, or "Unknown" for an empty set -- matching what
// buildGameMap() writes into the game map, so the log and the UI agree.
static std::string joinLabels(const std::vector<std::string> &labels) {
    if (labels.empty()) return "Unknown";
    std::string out;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i) out += ", ";
        out += labels[i];
    }
    return out;
}

void VortexBridge::logGameClick(QString name, QString genres, QString tags) {
    // Previously this only printed to stdout — including a line claiming the
    // genres were "being utilized for model training", which nothing read.
    appendFeedbackEvent({
        { "event_type", "click" },
        { "name", name },
        { "origin", "Library" },
        { "genres", genres },
        { "tags", tags },
    });

    logGameProfile(name);
}

// Everything Vortex knows about one game, printed when its profile opens.
//
// Split deliberately across the two sides. This half runs from memory and
// appears instantly, so a profile still explains itself with no database, no
// network and no interpreter. The Python half that follows adds the one thing
// the C++ cannot reach: keywords live only in the IGDB catalog table, and
// GameMetadata has no field for them.
void VortexBridge::logGameProfile(const QString &name) {
    const BridgeGame *found = nullptr;
    for (const BridgeGame &bg : m_internalGames) {
        if (QString::fromStdString(bg.name) == name) {
            found = &bg;
            break;
        }
    }

    const QVariantMap map = gameDetailsFor(name);

    vlog::phase("Game profile: " + name.toStdString());

    if (!found) {
        // Unowned discovery picks are not in m_internalGames; the recommender
        // carries their details instead, so say which case this is rather than
        // printing a half-empty block.
        vlog::line("Profile", "not in the local library (a Discover pick); "
                              "details come from the recommendation itself");
    } else {
        vlog::line("Profile", "source      " + found->source +
                              (found->appid > 0
                                   ? "  (appid " + std::to_string(found->appid) + ")"
                                   : ""));
        vlog::line("Profile", "developer   " +
                              map.value("developer").toString().toStdString());

        const double rating = map.value("rating").toDouble();
        vlog::line("Profile", "rating      " +
                              (rating > 0.0 ? QString::number(rating, 'f', 1).toStdString()
                                            : std::string("n/a")));
        vlog::line("Profile", "time to beat " +
                              map.value("timeToBeat").toString().toStdString());
        vlog::line("Profile", "playtime    " +
                              map.value("playtime").toString().toStdString() +
                              "   last played " +
                              map.value("lastPlayed").toString().toStdString());
        vlog::line("Profile", std::string("favourite   ") +
                              (map.value("status").toDouble() > 0.0 ? "yes" : "no"));

        if (!found->installDir.empty())
            vlog::line("Profile", "installed   " + found->installDir.string());

        // Themes and game modes are in GameMetadata but were never copied into
        // the game map, so read them straight from the cache rather than
        // widening the map for a logging feature.
        if (found->igdb_id > 0) {
            const GameMetadata meta = get_game_metadata(found->igdb_id);
            vlog::line("Profile", "genres      " + joinLabels(meta.main_genres));
            vlog::line("Profile", "tags        " + joinLabels(meta.all_genres));
            vlog::line("Profile", "themes      " + joinLabels(meta.themes));
            vlog::line("Profile", "game modes  " + joinLabels(meta.game_modes));
        } else {
            vlog::line("Profile", "no IGDB match, so no genres, themes or "
                                  "game modes are known for this game");
        }
    }

    // Keywords, and the authoritative label set the recommender actually uses.
    explainGameToLog(name, QStringList{ "--labels" });
}

void VortexBridge::logRecommendationClick(QString name, int rank) {
    appendFeedbackEvent({
        { "event_type", "click" },
        { "name", name },
        { "origin", "Recommendations" },
        { "rank", rank },
    });

    explainGameToLog(name);
}

// Print the full per-mood breakdown for one game to the console and the log.
//
// Diagnostics only: it must never delay the click or block the launch that
// usually follows, so it runs detached and every failure is swallowed after
// being logged. Spawning an interpreter costs about a second, which is exactly
// why this is not on the UI thread.
void VortexBridge::explainGameToLog(const QString &name,
                                    const QStringList &extraArgs) {
    const std::optional<fs::path> scriptPath =
        existingPath(analyticsDir(m_baseDir) / "explain_game.py");
    if (!scriptPath) return;

    const fs::path script = *scriptPath;

    QThread *thread = QThread::create([script, name, extraArgs]() {
        QString details;
        QString output;
        const bool ok = runPythonScript(script, &details, &output,
                                        QStringList{ name } + extraArgs, 60000);

        if (!ok) {
            vlog::line("Explain", "could not explain " + name.toStdString() +
                                  ": " + details.toStdString());
            return;
        }

        // Printed as one block rather than line by line: another thread's
        // scan output would otherwise interleave through the middle of a
        // table that is only readable whole.
        vlog::line("Explain", "breakdown for " + name.toStdString());
        for (const QString &line : output.split('\n'))
            vlog::line("Explain", line.toStdString());
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// Looks up the real path in m_internalGames, launches on a background thread,
// waits for the game to close, records the play session, then refreshes the list
// so the playtime display updates.
void VortexBridge::launchGameFrom(QString name, QString origin) {
    // A second press while the first launch is still going would start the
    // game twice and record two overlapping sessions.
    if (m_launchingGames.contains(name) || m_runningGames.contains(name))
        return;

    // The ground truth the recommender never had: which surface produced this
    // launch. Without it, a game started from the Recommendations tab is
    // indistinguishable from one started from the library.
    appendFeedbackEvent({
        { "event_type", "launch" },
        { "name", name },
        { "origin", origin },
    });

    // Find game in internal list
    BridgeGame found;
    bool ok = false;
    for (const BridgeGame &bg : m_internalGames) {
        if (QString::fromStdString(bg.name) == name) {
            found = bg;
            ok    = true;
            break;
        }
    }
    if (!ok) return;

    std::string ptKey = makePtKey(found);

    vlog::phase("Play session");
    vlog::line("Play", "launching " + found.name + " (" + found.source +
                       ", from " + origin.toStdString() + ")");

    setLaunchState(name, true, false);
    const auto markRunning = [this, name]() {
        QMetaObject::invokeMethod(this, [this, name]() {
            setLaunchState(name, false, true);
        }, Qt::QueuedConnection);
    };

    QThread *thread = QThread::create([this, found, ptKey, name, markRunning]() {
        bool        played       = false;
        bool        syncOk       = true;
        bool        material     = false;

        // Each stretch the game itself ran. Steam yields at most one; a local
        // game started through its launcher can yield several, and the time
        // only the launcher was open is in none of them.
        std::vector<PlaySegment> segments;
        LocalSession local;

        if (found.source == "Steam") {
            launch_steam_game_by_appid(found.appid);

            // Steam's protocol launch hands back no process to wait on, so the
            // session is observed: wait for the game to appear, then to go away.
            // Idle sampling belongs inside that call -- it must not start until
            // the game is actually up, or the wait for it counts as idle.
            PlaySegment seg;
            if (monitor_steam_session(found.appid, found.installDir,
                                      &seg.start, &seg.end,
                                      &seg.idleSeconds, markRunning))
                segments.push_back(seg);
        } else {
            // Blocks until the game and any launcher it came from have closed,
            // sampling idle per run itself. A game the EA app installed bounces
            // through the EA app when started directly, so it is launched there
            // and waited for instead, as is a Riot game through the Riot
            // Client; every other local game is untouched.
            RiotGame riot;
            EaGame ea;
            if (read_riot_install(found.installDir, riot))
                local = run_riot_session(riot, markRunning);
            else if (read_ea_install(found.installDir, ea))
                local = run_ea_session(ea.contentId, found.installDir,
                                       markRunning);
            else
                local = run_local_session(found.gamePath, found.installDir,
                                          markRunning);
            segments = local.segments;
        }
        played = !segments.empty();

        // The button goes back to PLAY now, not after the sync below, which
        // can take a few seconds and has nothing to do with the game.
        QMetaObject::invokeMethod(this, [this, name]() {
            setLaunchState(name, false, false);
        }, Qt::QueuedConnection);

        if (!played) {
            // Nothing downstream can change, and silence here is what makes
            // "I played it and nothing happened" unanswerable.
            std::string why;
            if (found.source != "Steam" && !local.started)
                why = "could not start " + found.gamePath.filename().string();
            else if (local.viaLauncher)
                why = "launcher was open " + vlog::duration(local.launcherSeconds) +
                      " but the game never started; nothing recorded";
            else
                why = "no session detected -- the game never ran, or exited "
                      "immediately";
            vlog::item("Play", found.name, vlog::Status::Skipped, why);
        }

        if (played) {
            long long durationSeconds = 0;
            long long idleSeconds     = 0;
            for (const PlaySegment &seg : segments) {
                durationSeconds += static_cast<long long>(seg.end - seg.start);
                idleSeconds     += seg.idleSeconds;
            }

            // The duration is reported unaltered, with idle beside it rather
            // than taken out of it -- that is what goes in the log, and a
            // figure quietly missing its idle would not match what is on disk.
            std::string played_for = "played for " + vlog::duration(durationSeconds);
            if (idleSeconds > 0)
                played_for += " (" + vlog::duration(idleSeconds) + " idle)";
            if (segments.size() > 1)
                played_for += " across " + std::to_string(segments.size()) + " runs";
            if (local.viaLauncher)
                played_for += " | launcher open " +
                              vlog::duration(local.launcherSeconds) + ", not counted";
            vlog::item("Play", found.name, vlog::Status::Ok, played_for);

            // One session per run, so the history shows when the game was
            // actually up rather than one span bridging the launcher time.
            for (const PlaySegment &seg : segments)
                record_play_session(ptKey, found.name, seg.start, seg.end,
                                    seg.idleSeconds);

            // Steam persists its own total when the game exits — drop our cached
            // copy so the refreshed list picks the new figure up.
            if (found.source == "Steam")
                refresh_steam_playtime();

            // Push the new session into the database. No retrain here: recommend.py
            // refits in-process (well under 100ms at catalog size), so a
            // separate train.py run would only add a second interpreter
            // startup and a chance for the two to disagree.
            QString syncError, syncOutput;
            syncOk = runPythonScript(analyticsDir(m_baseDir) / "sync_local_data.py",
                                     &syncError, &syncOutput);
            if (!syncOk) {
                // Was a bare std::cerr, so a failed sync reached the console
                // and never the log file -- the half a user can actually send.
                vlog::item("Play", "sync_local_data.py", vlog::Status::Fail,
                           syncError.isEmpty() ? "no output"
                                               : syncError.toStdString());
            } else {
                // sync reports whether the session it just recorded could
                // actually move the ranking. A short session is excluded from
                // both the interest and disinterest profiles by construction,
                // so re-running would only reshuffle the list for no reason.
                material = syncOutput.contains("MATERIAL=1");

                vlog::item("Play", "sync_local_data.py", vlog::Status::Ok,
                           syncOutput.trimmed().remove('\r').replace('\n', " | ").toStdString());

                // Whether the session moved the ranking is decided by
                // is_material() in sync_local_data.py, using the bands in
                // interest.py. Reported here rather than recomputed: two
                // definitions of "counted" would drift apart.
                vlog::line("Play", material
                                       ? "session counted -- re-ranking recommendations"
                                       : "session too short to change recommendations");
            }
        }

        // Refresh playtime display on main thread (lightweight — no re-scan).
        QMetaObject::invokeMethod(this, [this, played, syncOk, material]() {
            refreshGameList();
            if (!played) {
                // No session was recorded, so nothing downstream can have
                // changed. Say so rather than implying a successful refresh.
                setRecommendationStatus("No play session detected; recommendations unchanged");
            } else if (!syncOk) {
                setRecommendationStatus("Could not sync play data; recommendations may be stale");
            } else if (!material) {
                setRecommendationStatus("Session too short to change recommendations");
            }

            // Only re-run when the session could actually have changed the
            // model. Otherwise the list would be reshuffled by exploration
            // alone, which reads as the recommendations randomly changing
            // every time a game is opened and closed.
            if (played && syncOk && material)
                loadRecommendations();
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void VortexBridge::setLaunchState(const QString &name, bool launching, bool running) {
    m_launchingGames.removeAll(name);
    m_runningGames.removeAll(name);
    if (launching) m_launchingGames.append(name);
    if (running)   m_runningGames.append(name);
    emit launchStateChanged();
}

void VortexBridge::quitGame(QString name) {
    if (!m_runningGames.contains(name))
        return;
    for (const BridgeGame &bg : m_internalGames) {
        if (QString::fromStdString(bg.name) != name)
            continue;
        // A local game's installDir is its folder; fall back to the exe's own
        // in case a record ever arrives without one.
        const fs::path dir = !bg.installDir.empty() ? bg.installDir
                                                    : bg.gamePath.parent_path();
        vlog::line("Play", "quitting " + bg.name);
        // Blocks through the grace period, so off the UI thread. The launch
        // thread notices the exit and flips the button back to PLAY. A Riot
        // game is only asked to close, never killed: Vanguard refuses the
        // kill, and a match left that way is penalised.
        const bool allowForce = !bg.isRiot;
        QThread *thread = QThread::create([dir, allowForce]() {
            quit_games_in_dir(dir, 5, allowForce);
        });
        connect(thread, &QThread::finished, thread, &QThread::deleteLater);
        thread->start();
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// uninstallGame — uses real installDir from m_internalGames.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::uninstallGame(QString name) {
    for (const BridgeGame &bg : m_internalGames) {
        if (QString::fromStdString(bg.name) == name) {
            // The Riot Client removes its own games, after its own prompt.
            // Deleting the folder under it would leave Riot -- and so this
            // scan -- still listing the game.
            RiotGame riot;
            if (bg.source != "Steam" && read_riot_install(bg.installDir, riot)) {
                if (uninstall_riot_game(riot))
                    watchRiotUninstall(riot.installDir);
                return;
            }
            int steamAppId = bg.source == "Steam"
                                 ? bg.appid
                                 : get_steam_appid_for_install_dir(bg.installDir);
            if (steamAppId > 0) {
                // Steam does the removal itself, after its own prompt, so a
                // re-scan now would still find the game. Wait for it instead.
                if (uninstall_steam_game_by_appid(steamAppId))
                    watchSteamUninstall(steamAppId);
                return;
            }
            std::error_code ec;
            fs::remove_all(bg.installDir, ec);
            // Re-scan so the removed game disappears from the grid.
            loadGames();
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// watchRiotUninstall — re-scan once the Riot Client has removed the game.
//
// The same shape as watchSteamUninstall below: the client's uninstall returns
// before the player has confirmed anything, so the folder is polled until it
// goes, and the poll gives up after a while in case they cancelled.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::watchRiotUninstall(const fs::path &installDir) {
    const QString key = QString::fromStdString(installDir.string()).toLower();
    if (m_pendingRiotUninstalls.contains(key))
        return;
    m_pendingRiotUninstalls.insert(key);

    constexpr int kPollMs   = 2000;
    constexpr int kGiveUpMs = 30 * 60 * 1000;

    auto *timer = new QTimer(this);
    timer->setInterval(kPollMs);
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    connect(timer, &QTimer::timeout, this, [this, timer, elapsed, installDir, key]() {
        std::error_code ec;
        const bool gone = !fs::exists(installDir, ec);
        if (!gone && elapsed->elapsed() < kGiveUpMs)
            return;
        timer->stop();
        timer->deleteLater();
        m_pendingRiotUninstalls.remove(key);
        if (gone) {
            vlog::line("Library", "Riot Client removed " + installDir.string()
                                      + " -- rescanning");
            loadGames();
        }
    });
    timer->start();
}

// ─────────────────────────────────────────────────────────────────────────────
// watchSteamUninstall — re-scan once Steam has actually removed the game.
//
// steam://uninstall returns as soon as Steam's confirm dialog is up; the
// manifest only goes after the player confirms there. Polled rather than
// watched with QFileSystemWatcher because the game can sit in any of Steam's
// libraries. The poll gives up after a while so a dialog the player cancelled
// does not keep a timer alive for the rest of the session.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::watchSteamUninstall(int appid) {
    if (m_pendingSteamUninstalls.contains(appid))
        return;
    m_pendingSteamUninstalls.insert(appid);

    constexpr int kPollMs   = 2000;
    constexpr int kGiveUpMs = 30 * 60 * 1000;

    auto *timer = new QTimer(this);
    timer->setInterval(kPollMs);
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    connect(timer, &QTimer::timeout, this, [this, timer, elapsed, appid]() {
        const bool gone = !is_steam_app_installed(appid);
        if (!gone && elapsed->elapsed() < kGiveUpMs)
            return;
        timer->stop();
        timer->deleteLater();
        m_pendingSteamUninstalls.remove(appid);
        if (gone) {
            vlog::line("Library", "Steam removed app " + std::to_string(appid)
                                      + " -- rescanning");
            loadGames();
        }
    });
    timer->start();
}

// fromLocalFile, not a hand-built "file:///" string, so a '#' or '%' in the
// path is escaped instead of being read as a fragment or an escape.
bool VortexBridge::openInstallFolder(QString path) {
    if (path.isEmpty() || !QFileInfo(path).isDir())
        return false;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

// ─────────────────────────────────────────────────────────────────────────────
// setGameExecutable — overrule the scanner's pick of which .exe a local game
// runs.
//
// exe_cache.txt is the scanner's own record and it is last-line-wins (see
// scan_directory_for_games), so appending one line is all it takes for the
// choice to survive every later scan. The in-memory row is patched as well so
// Play uses the new exe straight away, in place for the same scroll-position
// reason as updatePreference().
// ─────────────────────────────────────────────────────────────────────────────
QString VortexBridge::setGameExecutable(QString installDir, QString exeUrl) {
    const QUrl url(exeUrl);
    const QString exe = url.isLocalFile() ? url.toLocalFile() : exeUrl;
    const QFileInfo info(exe);
    if (exe.isEmpty() || !info.isFile()
        || info.suffix().compare("exe", Qt::CaseInsensitive) != 0)
        return QString();

    for (int i = 0; i < static_cast<int>(m_internalGames.size()); ++i) {
        BridgeGame &bg = m_internalGames[i];
        if (bg.source != "Local"
            || QString::compare(QString::fromStdString(bg.installDir.string()),
                                installDir, Qt::CaseInsensitive) != 0)
            continue;

        const fs::path exePath =
            QDir::toNativeSeparators(info.absoluteFilePath()).toStdString();

        std::ofstream out(app_data_path("exe_cache.txt"), std::ios::app);
        if (!out.is_open())
            return QString();
        out << bg.installDir.string() << "=" << exePath.string() << "\n";
        out.close();

        bg.gamePath = exePath;
        const QString newPath = QString::fromStdString(exePath.string());
        if (i < m_gameList.size()) {
            QVariantMap row = m_gameList[i].toMap();
            row["gamePath"] = newPath;
            m_gameList[i] = row;
            invalidateGameIndex();
        }
        vlog::line("Library", bg.name + " now launches " + exePath.string());
        return newPath;
    }
    return QString();
}

// ─────────────────────────────────────────────────────────────────────────────
// renameGame — the user's title becomes the game's name from here on.
//
// Two halves. Synchronously: record the override, retitle the row in place and
// carry the heart across, so the page and the grid show the new name at once.
// Then, off the UI thread: resolve the new title against IGDB and fetch its
// SteamGridDB art, which is the reason to rename at all -- and once that lands,
// move the playtime if the new IGDB id changed the key it is filed under.
// ─────────────────────────────────────────────────────────────────────────────

// Moves a heart from one title to another; preferences.json is keyed by name.
static void moveFavorite(const std::string &from, const std::string &to) {
    if (from == to || get_game_preference(from) <= 0.0) return;
    toggle_game_preference(from, 1.0);   // matches the current score: clears it
    if (get_game_preference(to) <= 0.0)
        toggle_game_preference(to, 1.0);
}

bool VortexBridge::renameGame(QString installDir, QString newName) {
    // The scan holds its own copy of every row and writes them back by
    // position; a rename under it would be overwritten by the old title.
    if (m_isLoading || installDir.isEmpty())
        return false;

    int index = -1;
    for (int i = 0; i < static_cast<int>(m_internalGames.size()); ++i) {
        if (QString::compare(QString::fromStdString(m_internalGames[i].installDir.string()),
                             installDir, Qt::CaseInsensitive) == 0) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return false;

    BridgeGame &bg = m_internalGames[index];
    std::string name = newName.simplified().toStdString();
    // Empty, or the scanned title typed back in, is a revert.
    const bool revert = name.empty() || name == bg.scannedName;
    if (revert)
        name = bg.scannedName.empty() ? bg.name : bg.scannedName;
    if (name == bg.name && (revert ? bg.customName.empty() : bg.customName == name))
        return true;   // nothing to do

    if (!appendNameOverride(bg.installDir, revert ? std::string() : name))
        return false;

    const std::string oldName = bg.name;
    const std::string oldKey  = makePtKey(bg);
    bg.name       = name;
    bg.customName = revert ? std::string() : name;
    moveFavorite(oldName, name);

    if (index < m_gameList.size())
        m_gameList[index] = buildGameMap(bg);
    invalidateGameIndex();
    scheduleArtNotify();
    emit favoritesChanged();
    emit gameRowChanged(installDir);
    vlog::line("Library", oldName + " renamed to " + name
                          + (revert ? " (scanned name restored)" : ""));

    ++m_renamesInFlight;
    const BridgeGame snapshot   = bg;
    const fs::path   imagesRoot = m_baseDir / "Images";
    QThread *thread = QThread::create([this, snapshot, imagesRoot, oldKey, installDir]() {
        BridgeGame resolved = snapshot;
        resolveIgdb(resolved);

        ensure_steamgriddb_images({ resolved.name }, imagesRoot.string());
        if (resolved.source == "Steam" && resolved.appid > 0)
            ensure_steam_hero_fallback({ { resolved.name, resolved.appid } },
                                       imagesRoot.string());

        QMetaObject::invokeMethod(this, [this, resolved, oldKey, installDir]() {
            --m_renamesInFlight;

            int at = -1;
            for (int i = 0; i < static_cast<int>(m_internalGames.size()); ++i) {
                if (QString::compare(QString::fromStdString(m_internalGames[i].installDir.string()),
                                     installDir, Qt::CaseInsensitive) == 0) {
                    at = i;
                    break;
                }
            }

            // A later rename of the same game supersedes this one; its own
            // completion does the bookkeeping for the title that stuck.
            if (at >= 0 && m_internalGames[at].customName == resolved.customName) {
                BridgeGame &live = m_internalGames[at];
                const std::string shownName = live.name;
                live.igdb_id = resolved.igdb_id;
                live.name    = resolved.name;   // a revert can take IGDB's spelling
                moveFavorite(shownName, live.name);

                const std::string newKey = makePtKey(live);
                rekey_play_stats(oldKey, newKey, live.name);
                if (newKey != oldKey) {
                    // The hours moved to the new key; a ledger row left on the
                    // old one would show up as a second, uninstalled game.
                    const QString stale = QString::fromStdString(oldKey);
                    for (int i = m_playedLedger.size() - 1; i >= 0; --i) {
                        if (m_playedLedger[i].toMap().value("key").toString() == stale)
                            m_playedLedger.removeAt(i);
                    }
                    savePlayedLedger();
                }

                if (at < m_gameList.size())
                    m_gameList[at] = buildGameMap(live);
                invalidateGameIndex();
                syncPlayedLedger();
                scheduleArtNotify();
                emit favoritesChanged();
                emit gameRowChanged(installDir);
                loadRecommendations();
            }

            if (m_renamesInFlight == 0 && m_rescanQueued) {
                m_rescanQueued = false;
                loadGames();
            }
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// removeFromLibrary — take a game out of the launcher, leave it on the disk.
//
// The record is written first and the lists are corrected second, because the
// record is what makes the removal survive the next scan; a list that is right
// now and wrong after a restart would be the worse half to get.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::removeFromLibrary(QString name, QString installDir) {
    if (name.isEmpty() && installDir.isEmpty())
        return;

    // Find the live row so the record carries every key that can identify the
    // game later -- installDir alone is what a QML card knows, but appid is
    // what survives a Steam library being moved.
    int index = -1;
    for (int i = 0; i < static_cast<int>(m_internalGames.size()); ++i) {
        const BridgeGame &bg = m_internalGames[i];
        if (!installDir.isEmpty()
            && QString::compare(QString::fromStdString(bg.installDir.string()),
                                installDir, Qt::CaseInsensitive) == 0) {
            index = i;
            break;
        }
        if (index < 0 && !name.isEmpty()
            && QString::compare(QString::fromStdString(bg.name), name,
                                Qt::CaseInsensitive) == 0)
            index = i;                       // keep looking for an installDir hit
    }

    QVariantMap record;
    record["name"]       = name;
    record["installDir"] = installDir;
    record["appid"]      = 0;
    record["source"]     = QString();
    if (index >= 0) {
        const BridgeGame &bg = m_internalGames[index];
        if (name.isEmpty())       record["name"] = QString::fromStdString(bg.name);
        if (installDir.isEmpty())
            record["installDir"] = QString::fromStdString(bg.installDir.string());
        record["appid"]  = bg.appid;
        record["source"] = QString::fromStdString(bg.source);
    }
    record["removedAt"] = QDateTime::currentSecsSinceEpoch();

    m_removed << record;
    saveRemovedGames();
    emit removedGamesChanged();
    vlog::line("Library", "removed " + record["name"].toString().toStdString()
                          + " (files left in place)");

    // A scan in flight holds its own copy of the game vector and writes rows
    // back by position, so pulling one out from under it would send the rest of
    // its updates to the wrong cards. Let the scan finish and rescan after it;
    // the queue for exactly this already exists.
    if (m_isLoading) {
        m_rescanQueued = true;
        return;
    }

    if (index >= 0)
        m_internalGames.erase(m_internalGames.begin() + index);

    // Rebuilds m_gameList from the (now shorter) vector, keeping the two
    // index-parallel, and emits gameListChanged + favoritesChanged.
    refreshGameList();
    emit playedGamesChanged();
}

// ─────────────────────────────────────────────────────────────────────────────
// restoreToLibrary — put one back.
//
// A full rescan rather than a list rebuild: the game is not in m_internalGames
// any more, and the disk is the only place its install directory, appid and
// artwork can be read from again.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::restoreToLibrary(QString name) {
    if (name.isEmpty()) return;

    const std::string canonical = make_canonical(name.toStdString());
    bool changed = false;
    for (int i = m_removed.size() - 1; i >= 0; --i) {
        const QString stored = m_removed[i].toMap().value("name").toString();
        if (stored.isEmpty()
            || make_canonical(stored.toStdString()) != canonical)
            continue;
        m_removed.removeAt(i);
        changed = true;
    }
    if (!changed) return;

    saveRemovedGames();
    emit removedGamesChanged();
    vlog::line("Library", "restored " + name.toStdString());
    loadGames();
}

// ─────────────────────────────────────────────────────────────────────────────
// updatePreference — updates preferences.json via the engine, then does a
// lightweight refresh so the favourite state updates immediately.
// ─────────────────────────────────────────────────────────────────────────────
double VortexBridge::updatePreference(QString name, double score) {
    double newStatus = toggle_game_preference(name.toStdString(), score);
    updateFavoriteSnapshot(name, newStatus > 0.0);

    // One field on one row changed, so patch it in place rather than calling
    // refreshGameList(). Same reasoning as updateGameRow(): rebuilding
    // m_gameList emits gameListChanged, the library grid binds to that array,
    // and GridView answers a new array by dropping the scroll position -- so
    // hearting a game halfway down the library threw the user back to the top.
    // favoriteGames() reads `status` straight off these rows, and its notify is
    // favoritesChanged, so the Favorites tab and the heart still update at once.
    for (int i = 0; i < m_gameList.size(); ++i) {
        QVariantMap row = m_gameList[i].toMap();
        if (QString::compare(row.value("name").toString(), name,
                             Qt::CaseInsensitive) == 0) {
            row["status"] = newStatus;
            m_gameList[i] = row;
            invalidateGameIndex();
            break;
        }
    }
    emit favoritesChanged();
    // Deliberately no loadRecommendations(): hearting does change the profile,
    // but reshuffling the list the instant you tap the heart is the same
    // unasked-for movement as reloading on a tab switch. The new weight is
    // picked up on the next real refresh. Same precedent as toggleWishlist().
    return newStatus;
}

// ─────────────────────────────────────────────────────────────────────────────
// resetPreferences — clears the whole taste profile in one action.
//
// preferences.json is the recommender's only favourite input
// (analytics/recommend.py::load_favorites reads it directly rather than going
// through Postgres), so emptying it is all that "start fresh" requires. The
// snapshots go with it: they exist purely to render unowned favourites, and
// with no favourites left there is nothing for them to render.
//
// Playtime, launch history and the wishlist are deliberately untouched — the
// button resets likes, not the library.
// ─────────────────────────────────────────────────────────────────────────────
int VortexBridge::resetPreferences() {
    const int cleared = clear_game_preferences();

    if (!m_favoriteSnapshots.isEmpty()) {
        m_favoriteSnapshots.clear();
        saveFavoriteSnapshots();
    }

    refreshGameList();   // clears every heart and empties the Favorites tab

    // The one place a preference change reloads immediately: the user asked
    // for a fresh start, so leaving picks that were ranked from the profile
    // just deleted would be the opposite of the request.
    loadRecommendations();
    return cleared;
}

QVariantList VortexBridge::localDirectories() const {
    QVariantList list;
    for (const fs::path &dir : readLocalGameDirectories(m_baseDir))
        list.append(QString::fromStdString(dir.string()));
    return list;
}

void VortexBridge::refreshLocalDirectories() {
    emit localDirectoriesChanged();
}

// The config write happens immediately so the settings list updates at once;
// scanning and artwork cleanup run on a background thread because they walk
// every remaining folder and read the Steam library.
void VortexBridge::removeLocalGameDirectory(QString folderPath) {
    QUrl url(folderPath);
    QString folder = url.isLocalFile() ? url.toLocalFile() : folderPath;
    if (folder.isEmpty()) return;

    std::error_code ec;
    fs::path target    = folder.toStdString();
    fs::path canonical = fs::weakly_canonical(target, ec);
    if (ec) canonical = target;

    std::vector<fs::path> dirs = readLocalGameDirectories(m_baseDir);
    auto it = std::find(dirs.begin(), dirs.end(), canonical);
    if (it == dirs.end()) return;

    dirs.erase(it);
    writeLocalGameDirectories(dirs, m_baseDir);
    emit localDirectoriesChanged();

    const fs::path baseDir    = m_baseDir;
    const fs::path imagesRoot = baseDir / "Images";
    const QString  removedLabel = QString::fromStdString(canonical.string());

    QThread *thread = QThread::create([this, canonical, baseDir, imagesRoot, removedLabel]() {
        std::vector<temp_GameEntry> removedGames;
        scan_directory_for_games(canonical, removedGames);

        // Names still reachable after the removal keep their artwork.
        std::vector<std::string> keepNames;
        for (const fs::path &dir : readLocalGameDirectories(baseDir)) {
            std::vector<temp_GameEntry> otherGames;
            scan_directory_for_games(dir, otherGames);
            for (const temp_GameEntry &g : otherGames) keepNames.push_back(g.name);
        }
        for (const SteamGame &g : read_installed_steam_games())
            keepNames.push_back(g.name);
        // A Riot game stays listed from Riot's own install list after its
        // folder is dropped, so its artwork stays too.
        for (const RiotGame &g : read_installed_riot_games()) {
            keepNames.push_back(g.title);
            keepNames.push_back(igdb_resolve_game(g.title, false).name);
        }

        std::vector<std::string> removedNames;
        removedNames.reserve(removedGames.size());
        for (const temp_GameEntry &g : removedGames) removedNames.push_back(g.name);

        const int artworkDeleted = delete_steamgriddb_images(removedNames, keepNames,
                                                             imagesRoot.string());
        const int gamesRemoved   = static_cast<int>(removedGames.size());

        QMetaObject::invokeMethod(this, [this, removedLabel, gamesRemoved, artworkDeleted]() {
            emit directoryRemoved(removedLabel, gamesRemoved, artworkDeleted);
            loadGames();
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

bool VortexBridge::addLocalGameDirectory(QString folderUrl) {
    QUrl url(folderUrl);
    QString folder = url.isLocalFile() ? url.toLocalFile() : folderUrl;
    if (folder.isEmpty()) return false;

    fs::path dir = folder.toStdString();
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return false;

    fs::path canonical = fs::weakly_canonical(dir, ec);
    if (ec) canonical = fs::absolute(dir, ec);
    if (ec) canonical = dir;

    std::vector<fs::path> dirs = readLocalGameDirectories(m_baseDir);
    if (std::find(dirs.begin(), dirs.end(), canonical) != dirs.end())
        return false;

    dirs.push_back(canonical);
    writeLocalGameDirectories(dirs, m_baseDir);
    emit localDirectoriesChanged();

    loadGames();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// First-run credentials
//
// Vortex is usable with none of these set: the Steam scan, playtime tracking
// and the local recommender never touched an API. IGDB fills in metadata and
// the Discover catalog, SteamGridDB fills in artwork. Both are free, and both
// are asked for by a wizard the user can skip entirely — which is why nothing
// here blocks startup or reports an error when the file is absent.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// The keys the wizard manages. Anything else already in .env — DB_PATH, a
// hand-added value — is preserved on write.
constexpr const char *kIgdbId     = "IGDB_CLIENT_ID";
constexpr const char *kIgdbSecret = "IGDB_CLIENT_SECRET";
constexpr const char *kSgdbKey    = "STEAMGRIDDB_API_KEY";

fs::path envFilePath(const fs::path &baseDir) {
    return analyticsDir(baseDir) / ".env";
}

// Rewrite `path` so each key in `updates` has the given value, leaving every
// other line — comments included — exactly as it was.
//
// A whole-file rewrite from a template would be simpler, but it would discard
// the comments in .env.example that tell the user where each key came from,
// and silently drop any key this build does not know about.
bool writeEnvFile(const fs::path &path, const QMap<QString, QString> &updates) {
    QStringList lines;

    QFile existing(pathToQString(path));
    if (existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&existing);
        while (!in.atEnd()) lines << in.readLine();
        existing.close();
    }

    QSet<QString> written;
    for (QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#')) continue;

        const int eq = trimmed.indexOf('=');
        if (eq <= 0) continue;

        const QString key = trimmed.left(eq).trimmed();
        if (!updates.contains(key)) continue;

        line = key + "=" + updates.value(key);
        written.insert(key);
    }

    for (auto it = updates.constBegin(); it != updates.constEnd(); ++it) {
        if (written.contains(it.key())) continue;
        lines << it.key() + "=" + it.value();
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    // Write through a temporary and rename: a half-written .env would leave the
    // user with credentials that parse to garbage and no obvious way to tell.
    const QString finalPath = pathToQString(path);
    const QString tempPath  = finalPath + ".tmp";

    QFile out(tempPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return false;

    {
        QTextStream stream(&out);
        for (const QString &line : lines) stream << line << "\n";
    }
    out.close();

    QFile::remove(finalPath);
    return QFile::rename(tempPath, finalPath);
}

}  // namespace

bool VortexBridge::hasCredentials() const {
    return has_secret(kIgdbId) && has_secret(kIgdbSecret) && has_secret(kSgdbKey);
}

QVariantMap VortexBridge::credentialStatus() const {
    QVariantMap status;
    status["igdb"]        = has_secret(kIgdbId) && has_secret(kIgdbSecret);
    status["steamgriddb"] = has_secret(kSgdbKey);
    // The catalog is what Discover reads. Reported separately because having
    // IGDB keys and having actually fetched with them are different states,
    // and the wizard's last pane needs to tell them apart.
    status["catalogRefreshing"] = m_catalogRefreshing;
    // Present vs working. The Discover empty state used to key off presence
    // alone, so a user with rejected keys was advised to play more games.
    status["igdbWorks"] = m_igdbAuthOk;
    status["artworkWorks"] = m_artworkAuthOk;
    return status;
}

bool VortexBridge::saveCredentials(QString igdbClientId, QString igdbClientSecret,
                                   QString steamGridDbKey) {
    QMap<QString, QString> updates;

    // An empty field means "leave this one alone", so Settings can change one
    // key without the user re-entering the other two.
    auto put = [&updates](const char *key, const QString &value) {
        const QString trimmed = value.trimmed();
        if (!trimmed.isEmpty()) updates.insert(QString::fromLatin1(key), trimmed);
    };

    put(kIgdbId, igdbClientId);
    put(kIgdbSecret, igdbClientSecret);
    put(kSgdbKey, steamGridDbKey);

    if (updates.isEmpty()) return true;   // nothing to do, not a failure

    if (!writeEnvFile(envFilePath(m_baseDir), updates)) {
        qWarning("[Vortex] could not write analytics/.env");
        return false;
    }

    // Without this the values loaded at startup — an empty map on a fresh
    // install — stay cached and the keys just entered do nothing until the
    // launcher is restarted.
    reload_secrets();

    emit credentialsChanged();
    return true;
}

// Check the entered credentials against the live providers and report back.
//
// A wrong key used to be written and accepted in silence: every lookup failed
// for the rest of the session and the only evidence was a line in a log file
// the user had to be told to find. This answers at the moment of entry.
//
// Saving is NOT blocked on the result. A validation that fails because the
// machine is offline must not stop someone entering a key they know is good.
void VortexBridge::validateCredentials(QString igdbClientId,
                                       QString igdbClientSecret,
                                       QString steamGridDbKey) {
    // Empty means "keep what is stored", matching saveCredentials(), so the
    // probe has to test the effective values rather than only the typed ones.
    const std::string id = igdbClientId.trimmed().isEmpty()
                               ? get_secret(kIgdbId)
                               : igdbClientId.trimmed().toStdString();
    const std::string secret = igdbClientSecret.trimmed().isEmpty()
                                   ? get_secret(kIgdbSecret)
                                   : igdbClientSecret.trimmed().toStdString();
    const std::string sgdb = steamGridDbKey.trimmed().isEmpty()
                                 ? get_secret(kSgdbKey)
                                 : steamGridDbKey.trimmed().toStdString();

    QThread *thread = QThread::create([this, id, secret, sgdb]() {
        QVariantMap result;

        if (id.empty() && secret.empty()) {
            result["igdbOk"] = false;
            result["igdbDetail"] = QStringLiteral("No IGDB credentials entered.");
        } else {
            vlog::line("Credentials", "checking IGDB credentials with Twitch");
            const CredentialCheck check = igdb_probe_credentials(id, secret);
            result["igdbOk"] = check.ok;
            result["igdbRejected"] = check.rejected;
            result["igdbDetail"] = QString::fromStdString(check.detail);
            vlog::item("Credentials", "IGDB",
                       check.ok ? vlog::Status::Ok : vlog::Status::Fail,
                       check.detail);
        }

        if (sgdb.empty()) {
            result["sgdbOk"] = false;
            result["sgdbDetail"] = QStringLiteral("No SteamGridDB key entered.");
        } else {
            vlog::line("Credentials", "checking the SteamGridDB key");
            const CredentialCheck check = steamgriddb_probe_key(sgdb);
            result["sgdbOk"] = check.ok;
            result["sgdbRejected"] = check.rejected;
            result["sgdbDetail"] = QString::fromStdString(check.detail);
            vlog::item("Credentials", "SteamGridDB",
                       check.ok ? vlog::Status::Ok : vlog::Status::Fail,
                       check.detail);
        }

        QMetaObject::invokeMethod(this, [this, result]() {
            const bool igdbOk = result.value("igdbOk").toBool();
            m_igdbAuthOk = igdbOk;
            m_authBlocker = igdbOk
                ? QString()
                : result.value("igdbDetail").toString();
            emit credentialsChanged();
            emit credentialsValidated(result);

            // A key that now works deserves the fetch it was previously
            // denied, without making the user restart.
            if (igdbOk) {
                m_catalogAutoFetchClock.invalidate();
                maybeAutoFetchCatalog();
            }
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// One sentence naming whatever is actually stopping metadata and Discover from
// working, or empty when nothing is. Shown on the Recommendations header, which
// is the only part of that tab guaranteed to be on screen -- the sections below
// it scroll away once the library has enough picks to fill the viewport.
QString VortexBridge::authBlocker() const {
    if (!m_authBlocker.isEmpty()) return m_authBlocker;

    if (!has_secret(kIgdbId) || !has_secret(kIgdbSecret))
        return QStringLiteral("IGDB credentials not set -- game details and "
                              "Discover are unavailable. Add them in Settings.");

    if (!m_igdbAuthOk)
        return QStringLiteral("IGDB rejected your credentials -- game details "
                              "and Discover are unavailable. Re-enter them in "
                              "Settings.");

    if (!m_artworkAuthOk)
        return QStringLiteral("SteamGridDB is refusing requests, so artwork "
                              "cannot download. Check the key in Settings.");

    return QString();
}

void VortexBridge::refreshCatalog() {
    if (m_catalogRefreshing) return;

    if (!has_secret(kIgdbId) || !has_secret(kIgdbSecret)) {
        emit catalogRefreshFinished(
            false, QStringLiteral("IGDB credentials are not set. Add them first."));
        return;
    }

    const std::optional<fs::path> scriptPath =
        existingPath(analyticsDir(m_baseDir) / "igdb_catalog.py");
    if (!scriptPath) {
        emit catalogRefreshFinished(
            false, QStringLiteral("igdb_catalog.py is missing from analytics/."));
        return;
    }

    m_catalogRefreshing = true;
    setCatalogProgress("Starting download", 0);
    emit catalogRefreshingChanged();

    const fs::path script = *scriptPath;

    QThread *thread = QThread::create([this, script]() {
        QString details;

        const bool ok = runPythonScriptStreaming(
            script, QStringList{ "--refresh" },
            [this](const QString &line) {
                vlog::line("Catalog", line.toStdString());

                // igdb_catalog.py already prints "  fetched N games (last id …)"
                // once per page, so the count comes free -- no extra output had
                // to be added to the script. The total is unknown in advance
                // (keyset paging runs until the pages stop coming), so this is
                // an honest running count rather than a percentage.
                static const QRegularExpression fetched(
                    QStringLiteral("fetched\\s+(\\d+)\\s+games"));
                const auto match = fetched.match(line);
                if (match.hasMatch()) {
                    reportCatalogProgress("Downloading games",
                                          match.captured(1).toInt());
                    return;
                }
                if (line.startsWith("Fetching time-to-beat"))
                    reportCatalogProgress("Fetching play times", -1);
                else if (line.startsWith("Normalised") || line.startsWith("Inserted"))
                    reportCatalogProgress("Saving catalog", -1);
            },
            &details, 45 * 60 * 1000);

        QMetaObject::invokeMethod(this, [this, ok, details]() {
            m_catalogRefreshing = false;
            setCatalogProgress(QString(), 0);
            emit catalogRefreshingChanged();
            emit catalogRefreshFinished(ok, details);

            if (ok) {
                vlog::line("Catalog", "refresh complete");
                // Discover reads the catalog, so the visible list is stale
                // until the recommender runs again against the new rows.
                loadRecommendations();
            } else {
                vlog::line("Catalog", "refresh FAILED: " + details.toStdString());
            }
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// The catalog is refreshed in the background once it is this old, so new
// releases and rating changes reach Discover. Matches CATALOG_REFRESH_DAYS in
// recommend.py, which reports the age this is compared against.
static constexpr double kCatalogRefreshDays = 5.0;

// Minimum gap between automatic attempts. Called after every recommendation
// run -- every heart, every mood change -- so without it a fetch that keeps
// failing (offline, IGDB down) would be retried on each one. Long enough to
// stop that, short enough that a launcher left open for days still refreshes.
static constexpr qint64 kCatalogAutoFetchRetryMs = 6LL * 60 * 60 * 1000;

// Start a catalog download in the background when Discover needs one: the
// install has never had a catalog, or the one it has is kCatalogRefreshDays old.
//
// The first case exists because the first-run wizard's offer is easy to skip
// -- which is exactly what happened on the machine this was written for: the
// log showed a working library, working artwork, and a Discover section with
// nothing in it and no explanation.
//
// Deliberately silent when credentials are missing. That is not a failure, it
// is a user who has not entered keys yet, and the wizard and the Discover
// empty state both already say so.
void VortexBridge::maybeAutoFetchCatalog() {
    if (m_catalogRefreshing) return;
    if (m_catalogAutoFetchClock.isValid()
        && m_catalogAutoFetchClock.elapsed() < kCatalogAutoFetchRetryMs)
        return;

    const bool empty = m_discoverCandidateCount == 0 && m_catalogAgeDays < 0;
    const bool stale = m_catalogAgeDays >= kCatalogRefreshDays;
    if (!empty && !stale) return;

    // Only a real attempt starts the retry window. Checked after the
    // empty/stale test, so a fresh catalog never pushes back the next
    // refresh by six hours.
    m_catalogAutoFetchClock.start();

    if (!has_secret(kIgdbId) || !has_secret(kIgdbSecret)) {
        vlog::line("Catalog",
                   "skipped: no IGDB credentials, so Discover stays empty "
                   "until they are entered in Settings");
        return;
    }

    // The old catalog stays in place until the new one is stored, in a single
    // transaction, so Discover keeps its picks for the whole download.
    if (stale) {
        vlog::line("Catalog",
                   "catalog is " + std::to_string(static_cast<int>(m_catalogAgeDays)) +
                   " days old; refreshing in the background");
    } else {
        vlog::line("Catalog",
                   "empty on this install; starting the one-time download "
                   "in the background");
    }
    refreshCatalog();
}

void VortexBridge::setCatalogProgress(const QString &phase, int fetched) {
    if (m_catalogPhase == phase && (fetched < 0 || m_catalogFetched == fetched))
        return;
    m_catalogPhase = phase;
    if (fetched >= 0) m_catalogFetched = fetched;
    emit catalogProgressChanged();
}

// Callable from the fetch thread; marshals onto the main thread.
void VortexBridge::reportCatalogProgress(const QString &phase, int fetched) {
    QMetaObject::invokeMethod(this, [this, phase, fetched]() {
        setCatalogProgress(phase, fetched);
    }, Qt::QueuedConnection);
}
