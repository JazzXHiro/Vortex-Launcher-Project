#include "vortex_bridge.h"
#include "app_paths.h"
#include "game_manager.h"
#include "idle_tracker.h"
#include "igdb_manager.h"
#include "json_text.h"
#include "metadata_manager.h"
#include "preference_manager.h"
#include "stats_manager.h"
#include "steam_manager.h"
#include "secrets.h"
#include "steamgriddb_manager.h"
#include "vortex_log.h"

#include <QMetaObject>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <functional>
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

// Artwork for unowned recommendations lives in its own root, deliberately not
// inside Images/. That namespace is keyed by game name and
// delete_steamgriddb_images() runs over it whenever a local folder is removed,
// which would silently delete a candidate's cover on a name collision.
static fs::path candidateImagesDir(const fs::path &baseDir) {
    return baseDir / "CandidateImages";
}

static std::string candidateCoverStem(const QString &name) {
    std::string stem = name.toStdString();
    for (char &c : stem)
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*')
            c = '_';
    return stem;
}

// Covers stay flat at CandidateImages/<stem>.<ext> -- that is where the ones
// already downloaded live, and moving them would re-fetch every one. Hero and
// logo, added later, get a subdirectory each rather than a suffix on the stem:
// candidateCoverStem() folds nine characters onto '_', so "<stem>_hero" can
// collide with a real title that sanitises to the same thing, while a directory
// never can.
static fs::path candidateArtDir(const fs::path &root, const QString &kind) {
    return kind.isEmpty() ? root : root / kind.toStdString();
}

static QString findCandidateArt(const fs::path &root, const QString &name,
                                const QString &kind) {
    const std::string stem = candidateCoverStem(name);
    const fs::path dir = candidateArtDir(root, kind);
    for (const std::string &ext : {".jpg", ".png", ".jpeg"}) {
        fs::path full = dir / (stem + ext);
        std::error_code ec;
        if (fs::exists(full, ec) && !ec) {
            QString abs = QString::fromStdString(fs::absolute(full).string());
            return QUrl::fromLocalFile(abs).toString();
        }
    }
    return "";
}

static QString findCandidateCover(const fs::path &root, const QString &name) {
    return findCandidateArt(root, name, QString());
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

// Ordered; each entry is tried until one returns bytes.
static QStringList artworkUrlChain(const QVariantMap &item, const QString &kind) {
    const int appId = item.value("steamAppId").toInt();
    QStringList urls;

    if (kind == QLatin1String("hero")) {
        // library_hero.jpg is 1920x620 and is what the Steam client itself
        // draws behind a library page. Apps older than that layout 404 on it,
        // which is why IGDB follows rather than replaces it.
        if (appId > 0)
            urls << steamArtUrl(appId, QStringLiteral("library_hero.jpg"));
        const QString igdb = igdbHeroUrl(item.value("coverUrl").toString());
        if (!igdb.isEmpty())
            urls << igdb;
    } else if (kind == QLatin1String("logo")) {
        // No IGDB equivalent: it has no transparent logo art at all. Without an
        // appid this slot stays empty and GameDetails centres the cover.
        if (appId > 0)
            urls << steamArtUrl(appId, QStringLiteral("logo.png"));
    }

    return urls;
}

// ─────────────────────────────────────────────────────────────────────────────
// Negative cache, mirroring the .sgdb_state convention in
// steamgriddb_manager.cpp: a game with nothing upstream would otherwise
// re-request every time its details page is opened, forever. One file for the
// whole root rather than one per game, because candidates are flat files and
// have no per-game directory to hold it.
//
// Only definitive answers are stamped. Being offline must never write a game
// off, so fetchArtworkFrom() stamps only when every attempt in the chain failed
// for a reason the server actually gave.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr std::time_t kArtworkNegativeCacheSeconds = 7 * 24 * 60 * 60;

static fs::path artworkStatePath(const fs::path &root) {
    return root / ".artwork_state";
}

// Keyed on the sanitised stem, so the separator cannot appear in it: '|' is one
// of the characters candidateCoverStem() replaces. '=' is legal in a Windows
// filename, hence rfind below rather than find.
static QString artworkStateKey(const QString &name, const QString &kind) {
    return QString::fromStdString(candidateCoverStem(name)) + QLatin1Char('|') + kind;
}

static QHash<QString, std::time_t> loadArtworkState(const fs::path &root) {
    QHash<QString, std::time_t> state;
    std::ifstream in(artworkStatePath(root));
    if (!in)
        return state;

    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.rfind('=');
        if (eq == std::string::npos)
            continue;
        try {
            state.insert(QString::fromStdString(line.substr(0, eq)),
                         static_cast<std::time_t>(std::stoll(line.substr(eq + 1))));
        } catch (...) {
            continue; // unparsable stamp: treat as never attempted
        }
    }
    return state;
}

static void saveArtworkState(const fs::path &root,
                             const QHash<QString, std::time_t> &state) {
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec)
        return;

    std::ofstream out(artworkStatePath(root), std::ios::trunc);
    if (!out)
        return;
    for (auto it = state.constBegin(); it != state.constEnd(); ++it)
        out << it.key().toStdString() << "="
            << static_cast<long long>(it.value()) << "\n";
}

// A stamp in the future (the clock moved backwards between runs) counts as
// stale, so a bad clock cannot block a game's artwork indefinitely.
static bool artworkAttemptIsFresh(std::time_t stamp, std::time_t now) {
    return stamp > 0 && now >= stamp
        && (now - stamp) < kArtworkNegativeCacheSeconds;
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
static bool recommendationRunMatches(const fs::path &metaPath, const QString &runId) {
    QFile file(pathToQString(metaPath));
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return false;

    return doc.object().value("run_id").toString() == runId;
}

static QVariantList readRecommendationJson(const fs::path &jsonPath, const QVariantList &games,
                                           const fs::path &candidateImages) {
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

        // Cover art is cached under a sibling root, never inside Images/:
        // that namespace is keyed by name and delete_steamgriddb_images()
        // would happily remove a candidate's artwork on a folder removal.
        const QString cached = findCandidateCover(candidateImages, name);
        const QString hero = findCandidateArt(candidateImages, name, "hero");
        const QString logo = findCandidateArt(candidateImages, name, "logo");
        item["coverPath"] = cached;
        // Wide art and logo arrive through ensureArtwork(), lazily, when the
        // details page opens on this game -- so on a first sighting there is
        // nothing here yet and the cover stands in for the hero. GameDetails
        // blurs a stand-in deliberately rather than stretching it sharp. What
        // a previous session already fetched is picked up right here.
        item["heroPath"] = hero.isEmpty() ? cached : hero;
        item["logoPath"] = logo;
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
    game["appid"]      = bg.appid;
    game["installDir"] = QString::fromStdString(bg.installDir.string());

    game["coverPath"]  = findImagePath(gameDir, "grid");
    game["heroPath"]   = findImagePath(gameDir, "hero");
    game["logoPath"]   = findImagePath(gameDir, "logo");

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
        if (jsonPath && recommendationRunMatches(metaPath, runId)) {
            recommendations = readRecommendationJson(*jsonPath, gameListSnapshot,
                                                     candidateImagesDir(baseDir));
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

        QMetaObject::invokeMethod(this, [this, recommendations, status, discoverCount]() {
            m_recommendationList = recommendations;
            m_discoverCandidateCount = discoverCount;
            setRecommendationLoading(false);
            setRecommendationStatus(status);

            // Nothing to discover from means the catalog was never fetched.
            // Runs once per session and only with credentials present.
            maybeAutoFetchCatalog();
            emit recommendationListChanged();

            // Covers download only after the list is on screen, and only for
            // cache misses. Doing it inside recommend.py would add seconds of
            // network I/O to a bounded QProcess on a path that fires whenever
            // the user hearts a game.
            fetchCandidateCovers();

            if (m_recommendationQueued) {
                m_recommendationQueued = false;
                loadRecommendations();
            }
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// ─────────────────────────────────────────────────────────────────────────────
// Candidate cover art — asynchronous, cache-miss only.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::fetchCandidateCovers() {
    const fs::path root = candidateImagesDir(m_baseDir);

    QList<QPair<QString, QString>> wanted;  // name -> url
    for (const QVariant &entry : m_recommendationList) {
        const QVariantMap item = entry.toMap();
        if (item.value("matched").toBool()) continue;
        if (!item.value("coverPath").toString().isEmpty()) continue;

        const QString url = item.value("coverUrl").toString();
        if (!url.isEmpty())
            wanted.append({ item.value("name").toString(), url });
    }
    if (wanted.isEmpty()) return;

    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) return;

    if (!m_network) m_network = new QNetworkAccessManager(this);

    auto *pending = new int(wanted.size());
    for (const auto &[name, url] : wanted) {
        QNetworkReply *reply = m_network->get(QNetworkRequest(QUrl(url)));
        connect(reply, &QNetworkReply::finished, this, [this, reply, name, root, pending]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                const QByteArray data = reply->readAll();
                if (!data.isEmpty()) {
                    QString ext = QFileInfo(reply->url().path()).suffix().toLower();
                    if (ext != "png" && ext != "jpeg") ext = "jpg";
                    QFile out(pathToQString(root / (candidateCoverStem(name) + "." + ext.toStdString())));
                    if (out.open(QIODevice::WriteOnly))
                        out.write(data);
                }
            }
            // Rebind paths once, after the last download settles, rather than
            // emitting a full list change per image.
            if (--(*pending) == 0) {
                delete pending;
                for (QVariant &entry : m_recommendationList) {
                    QVariantMap item = entry.toMap();
                    if (item.value("matched").toBool()) continue;
                    const QString found =
                        findCandidateCover(root, item.value("name").toString());
                    if (!found.isEmpty()) {
                        item["coverPath"] = found;
                        // Stand in for a hero only while there isn't one:
                        // ensureArtwork() may already have fetched real wide
                        // art for this game, and it outranks the cover.
                        const QString hero = findCandidateArt(
                            root, item.value("name").toString(), "hero");
                        item["heroPath"] = hero.isEmpty() ? found : hero;
                        entry = item;
                    }
                }
                emit recommendationListChanged();
            }
        });
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Hero / logo art for one unowned pick — asynchronous, on demand.
//
// Split from fetchCandidateCovers() on purpose. A cover is drawn on every card
// in the Discover grid, so it is fetched for the whole list; hero and logo are
// only ever visible inside GameDetails, so fetching them at list time would
// download two images per candidate to show at most one pair.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::ensureArtwork(QString name) {
    name = name.trimmed();
    if (name.isEmpty())
        return;

    // Same order GameDetails resolves gameData in, minus the library: an owned
    // game already has SteamGridDB art under Images/ and wants none of this.
    QVariantMap item = findGameByName(m_recommendationList, name);
    if (item.isEmpty())
        item = findGameByName(m_wishlist, name);
    if (item.isEmpty())
        item = findGameByName(m_favoriteSnapshots, name);
    if (item.isEmpty())
        item = findGameByName(m_playedLedger, name);
    if (item.isEmpty() || item.value("matched").toBool())
        return;

    // Everything downstream -- the filename stem, the state key, the rebind --
    // keys off the item's own name rather than the argument. findGameByName()
    // also matches case-insensitively and canonically, so a caller passing
    // "stick fight the game" would otherwise write art under a stem that
    // readRecommendationJson() never looks up, and it would download again on
    // every open and never appear.
    name = item.value("name").toString();

    const fs::path root = candidateImagesDir(m_baseDir);
    const QHash<QString, std::time_t> state = loadArtworkState(root);
    const std::time_t now = std::time(nullptr);

    for (const QString &kind : { QStringLiteral("hero"), QStringLiteral("logo") }) {
        if (!findCandidateArt(root, name, kind).isEmpty())
            continue;                                     // already on disk

        const QString key = artworkStateKey(name, kind);
        if (artworkAttemptIsFresh(state.value(key), now))
            continue;                                     // known to have none
        if (m_artworkInFlight.contains(key))
            continue;                                     // page reopened mid-download

        const QStringList urls = artworkUrlChain(item, kind);
        if (urls.isEmpty()) {
            // Nothing addressable at all — no Steam appid, and for a hero no
            // usable cover URL to rewrite either. Stamp it: re-deriving the
            // same empty chain on every open costs nothing and returns nothing.
            recordArtworkMiss(name, kind);
            continue;
        }

        m_artworkInFlight.insert(key);
        fetchArtworkFrom(name, kind, urls, 0, true);
    }
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
// emits so an open details page re-reads it. Shaped like rebindArtwork() and
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

void VortexBridge::fetchArtworkFrom(const QString &name, const QString &kind,
                                    const QStringList &urls, int index,
                                    bool allDefinitive) {
    const QString key = artworkStateKey(name, kind);

    if (index >= urls.size()) {
        m_artworkInFlight.remove(key);
        // Only a chain that ran out of *answers* is a real miss. If any attempt
        // failed to reach its host, the game keeps its chance next time — the
        // same rule ensure_steamgriddb_images() follows, so launching offline
        // never writes artwork off for a week.
        if (allDefinitive)
            recordArtworkMiss(name, kind);
        return;
    }

    if (!m_network)
        m_network = new QNetworkAccessManager(this);

    QNetworkRequest request { QUrl(urls.at(index)) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_network->get(request);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, name, kind, urls, index, allDefinitive]() {
        reply->deleteLater();

        const QNetworkReply::NetworkError error = reply->error();
        const QByteArray data =
            error == QNetworkReply::NoError ? reply->readAll() : QByteArray();

        if (data.isEmpty()) {
            // Steam 404s library_hero.jpg for apps older than that layout, so a
            // miss has to advance the chain rather than end it — IGDB is still
            // worth asking. A transport failure lands here too and advances the
            // same way; all it changes is whether the exhausted chain is
            // allowed to stamp the negative cache.
            const bool definitive =
                error == QNetworkReply::NoError                     // 200, no body
                || error == QNetworkReply::ContentNotFoundError
                || error == QNetworkReply::ContentAccessDenied
                || error == QNetworkReply::ContentGoneError
                || error == QNetworkReply::ContentOperationNotPermittedError;
            fetchArtworkFrom(name, kind, urls, index + 1,
                             allDefinitive && definitive);
            return;
        }

        const fs::path dir = candidateArtDir(candidateImagesDir(m_baseDir), kind);
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (!ec) {
            QString ext = QFileInfo(reply->url().path()).suffix().toLower();
            if (ext != "png" && ext != "jpeg")
                ext = "jpg";
            QFile out(pathToQString(
                dir / (candidateCoverStem(name) + "." + ext.toStdString())));
            if (out.open(QIODevice::WriteOnly))
                out.write(data);
        }

        m_artworkInFlight.remove(artworkStateKey(name, kind));
        rebindArtwork(name);
    });
}

void VortexBridge::recordArtworkMiss(const QString &name, const QString &kind) {
    const fs::path root = candidateImagesDir(m_baseDir);
    QHash<QString, std::time_t> state = loadArtworkState(root);
    state.insert(artworkStateKey(name, kind), std::time(nullptr));
    saveArtworkState(root, state);
}

void VortexBridge::rebindArtwork(const QString &name) {
    const fs::path root = candidateImagesDir(m_baseDir);
    const QString hero = findCandidateArt(root, name, "hero");
    const QString logo = findCandidateArt(root, name, "logo");
    if (hero.isEmpty() && logo.isEmpty())
        return;

    auto apply = [&](QVariantList &list) {
        bool changed = false;
        for (QVariant &entry : list) {
            QVariantMap item = entry.toMap();
            if (item.value("matched").toBool())
                continue;
            if (QString::compare(item.value("name").toString(), name,
                                 Qt::CaseInsensitive) != 0)
                continue;

            if (!hero.isEmpty())
                item["heroPath"] = hero;   // outranks the cover standing in
            if (!logo.isEmpty())
                item["logoPath"] = logo;
            entry = item;
            changed = true;
        }
        return changed;
    };

    // GameDetails re-runs findGameData() on each of these (GameDetails.qml:191),
    // so emitting is all an already-open page needs to pick the art up.
    if (apply(m_recommendationList))
        emit recommendationListChanged();

    // These two serialise the whole item map, so saving here is what keeps a
    // saved game rendering after a restart with no network — the entire reason
    // the snapshots exist.
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
}

// ─────────────────────────────────────────────────────────────────────────────
// Favourites / wishlist
// ─────────────────────────────────────────────────────────────────────────────
QVariantList VortexBridge::favoriteGames() const {
    // Owned favourites come from the live game list — buildGameMap already
    // sets `status` from get_game_preference().
    QVariantList list;
    QSet<QString> seen;
    for (const QVariant &entry : m_gameList) {
        const QVariantMap game = entry.toMap();
        if (game.value("status").toDouble() > 0.0) {
            list << entry;
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
            played["name"]   = name;
            played["status"] = 1.0;
            list << played;
            continue;
        }

        list << snapshot;
    }
    return list;
}

fs::path VortexBridge::favoriteSnapshotPath() const {
    return m_baseDir / "favorite_snapshots.json";
}

// Points a snapshot's three art slots at whatever is on disk for that name.
//
// Both art roots are keyed by name, so a game's artwork can always be found
// again even when no list still carries a row for it. That is the case the
// snapshot fallback below could not handle: un-hearting a Discover pick deletes
// its snapshot, and if the recommendations have been reshuffled since, hearting
// it again rebuilt the snapshot from a map holding nothing but a name -- the
// card lost its cover while the cached JPEG sat there untouched. Only the
// details page recovered, because ensureArtwork() rebinds hero and logo but
// never the cover.
//
// Re-resolving rather than trusting a stored value also matters on load: the
// saved paths are absolute file:// URLs and stop working the moment the app is
// moved to another folder. Same reasoning as loadPlayedLedger().
static void bindArtworkFromCache(const fs::path &baseDir, const QString &name,
                                 QVariantMap &item) {
    if (name.isEmpty()) return;

    // "grid" is the cover. In Images/ each kind is a subfolder; in the
    // candidate root the cover sits at the top level and only hero and logo
    // get a subfolder, which is what the empty kind means to findCandidateArt.
    static const std::pair<const char *, const char *> kArtSlots[] = {
        { "coverPath", "grid" }, { "heroPath", "hero" }, { "logoPath", "logo" }
    };

    const fs::path gameDir =
        baseDir / "Images" / steamgriddb_image_folder_name(name.toStdString());
    const fs::path candidates = candidateImagesDir(baseDir);

    for (const auto &slot : kArtSlots) {
        // Images/ first: a favourite that is installed, or was played and then
        // uninstalled, has its full-size art there rather than in the cache of
        // covers downloaded for discovery picks.
        QString found = findImagePath(gameDir, slot.second);
        if (found.isEmpty()) {
            const QString kind = qstrcmp(slot.second, "grid") == 0
                                 ? QString() : QString::fromLatin1(slot.second);
            found = findCandidateArt(candidates, name, kind);
        }
        // Nothing on disk leaves whatever the row already had -- for an unowned
        // pick heroPath legitimately holds the cover standing in for a hero.
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
// Two things are wrong with them. Rows the stats file seeded carry "Unknown" in
// every metadata slot -- that is all playtime_stats.txt knows -- and nothing
// ever went back over them, so a game uninstalled long ago showed a blank
// details page forever. And rows of any age can carry the dropped-escape
// spelling of a name or a genre. Rows that already hold real metadata keep it:
// those came from a live scan, which knows more than the caches do.
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
    std::sort(merged.begin(), merged.end(),
              [](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap();
        const QVariantMap y = b.toMap();

        const qlonglong xAt = x.value("lastPlayedAt").toLongLong();
        const qlonglong yAt = y.value("lastPlayedAt").toLongLong();
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
}

void VortexBridge::saveSettings() const {
    QJsonObject object;
    object["curatedOnly"] = m_curatedOnly;
    object["ignorePlayedGames"] = m_ignorePlayedGames;
    object["ignoreLikedGames"] = m_ignoreLikedGames;
    // stats_manager::use_steam_playtime() reads this same key straight off disk,
    // so the CLI shows whichever total the launcher is showing.
    object["useSteamPlaytime"] = m_useSteamPlaytime;

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

    m_wishlist << snapshot;
    saveWishlist();
    emit wishlistChanged();
    // Deliberately no loadRecommendations() here: the wishlist is a saved list
    // and has no influence on ranking, so there is nothing to recompute.
    return true;
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
    if (m_isLoading) {
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

        // Games the user removed from the library are dropped HERE, before
        // either vector exists, rather than when the QVariantList is built:
        // updateGameRow() writes m_gameList by position, so m_internalGames and
        // m_gameList have to stay index-parallel, and they only do if both are
        // built from the same filtered set.
        std::vector<BridgeGame> internalGames;
        for (const SteamGame &g : steamGames) {
            BridgeGame bg;
            bg.name       = g.name;
            bg.source     = "Steam";
            bg.appid      = g.appid;
            bg.igdb_id    = g.igdb_id;
            bg.installDir = g.installDir;
            if (isRemovedIn(removedSnapshot, bg)) continue;
            internalGames.push_back(std::move(bg));
        }
        for (const temp_GameEntry &g : localEntries) {
            BridgeGame bg;
            bg.name       = g.name;
            bg.source     = "Local";
            bg.igdb_id    = g.igdb_id;
            bg.installDir = g.installDir;
            bg.gamePath   = g.gamePath;
            if (isRemovedIn(removedSnapshot, bg)) continue;
            internalGames.push_back(std::move(bg));
        }

        const int total = static_cast<int>(internalGames.size());
        vlog::line("Scan", std::to_string(steamGames.size()) + " Steam + " +
                           std::to_string(localEntries.size()) + " local = " +
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

            IgdbGameInfo info =
                (bg.source == "Steam")
                    ? igdb_resolve_game(bg.name, false, bg.appid)
                    : igdb_resolve_game(bg.installDir.filename().string());

            if (info.id > 0) {
                bg.igdb_id = info.id;
                // The canonical IGDB title replaces the folder name for local
                // games. Steam store names are already presentable, so those
                // are left alone rather than swapping in a subtly different
                // spelling under the user mid-scan.
                if (bg.source != "Steam" && !info.name.empty())
                    bg.name = info.name;
            }

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
                               syncOut.trimmed().replace('\n', " | ").toStdString());
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

QVariantMap VortexBridge::gameDetailsFor(QString name) const {
    for (const QVariant &entry : m_gameList) {
        const QVariantMap map = entry.toMap();
        if (map.value("name").toString() == name)
            return map;
    }
    return {};
}

// Keyed on install directory, which -- unlike the title -- is decided in pass 1
// and never changes afterwards. Delegates hold the pass-1 snapshot of a row
// (the grid is deliberately not rebuilt when artwork lands, see updateGameRow),
// so a lookup by modelData.name misses for every local game whose folder name
// differs from the canonical IGDB title the resolve pass swaps in.
QVariantMap VortexBridge::gameDetailsForInstallDir(QString installDir) const {
    if (installDir.isEmpty())
        return {};

    for (const QVariant &entry : m_gameList) {
        const QVariantMap map = entry.toMap();
        if (map.value("installDir").toString() == installDir)
            return map;
    }
    return {};
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

// Human-readable session length. The log is read by someone asking "did that
// count", and 4980 does not answer that as directly as 1h 23m.
static std::string formatDuration(long long seconds) {
    if (seconds < 0) seconds = 0;
    const long long hours = seconds / 3600;
    const long long minutes = (seconds % 3600) / 60;
    const long long secs = seconds % 60;

    if (hours > 0)
        return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
    if (minutes > 0)
        return std::to_string(minutes) + "m " + std::to_string(secs) + "s";
    return std::to_string(secs) + "s";
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

    QThread *thread = QThread::create([this, found, ptKey]() {
        std::time_t sessionStart = 0;
        std::time_t sessionEnd   = 0;
        bool        played       = false;
        bool        syncOk       = true;
        bool        material     = false;

        long long idleSeconds = 0;

        if (found.source == "Steam") {
            launch_steam_game_by_appid(found.appid);

            // Steam's protocol launch hands back no process to wait on, so the
            // session is observed: wait for the game to appear, then to go away.
            // Idle sampling belongs inside that call -- it must not start until
            // the game is actually up, or the wait for it counts as idle.
            played = monitor_steam_session(found.appid, found.installDir,
                                           &sessionStart, &sessionEnd,
                                           &idleSeconds);
        } else {
            sessionStart = std::time(nullptr);
            // launchGame() in game_manager blocks until the process exits, so
            // the tracker running its own thread is what lets the local path
            // measure idle the same way the Steam one does.
            IdleTracker idle;
            idle.start();
            ::launchGame(found.gamePath);
            idleSeconds = idle.stop();
            sessionEnd  = std::time(nullptr);
            played      = sessionEnd > sessionStart;
        }

        if (!played) {
            // A Steam launch that never started, or a process that exited
            // immediately. Nothing downstream can change, and silence here is
            // what makes "I played it and nothing happened" unanswerable.
            vlog::item("Play", found.name, vlog::Status::Skipped,
                       "no session detected -- the game never ran, or exited "
                       "immediately");
        }

        if (played) {
            const long long durationSeconds =
                static_cast<long long>(sessionEnd - sessionStart);

            // The duration is reported unaltered, with idle beside it rather
            // than taken out of it -- that is what goes in the log, and a
            // figure quietly missing its idle would not match what is on disk.
            std::string played_for = "played for " + formatDuration(durationSeconds);
            if (idleSeconds > 0)
                played_for += " (" + formatDuration(idleSeconds) + " idle)";
            vlog::item("Play", found.name, vlog::Status::Ok, played_for);

            record_play_session(ptKey, found.name, sessionStart, sessionEnd,
                                idleSeconds);

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
                           syncOutput.trimmed().replace('\n', " | ").toStdString());

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

// ─────────────────────────────────────────────────────────────────────────────
// uninstallGame — uses real installDir from m_internalGames.
// ─────────────────────────────────────────────────────────────────────────────
void VortexBridge::uninstallGame(QString name) {
    for (const BridgeGame &bg : m_internalGames) {
        if (QString::fromStdString(bg.name) == name) {
            if (bg.source == "Steam") {
                uninstall_steam_game_by_appid(bg.appid);
            } else {
                int possibleSteamAppId = get_steam_appid_for_install_dir(bg.installDir);
                if (possibleSteamAppId > 0) {
                    uninstall_steam_game_by_appid(possibleSteamAppId);
                } else {
                    std::error_code ec;
                    fs::remove_all(bg.installDir, ec);
                }
            }
            // Re-scan so the removed game disappears from the grid.
            loadGames();
            return;
        }
    }
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
                m_catalogAutoFetchTried = false;
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

// Start the one-time catalog download if this install has never had one.
//
// Discover is empty without it, and the first-run wizard's offer is easy to
// skip -- which is exactly what happened on the machine this was written for:
// the log showed a working library, working artwork, and a Discover section
// with nothing in it and no explanation.
//
// Deliberately silent when credentials are missing. That is not a failure, it
// is a user who has not entered keys yet, and the wizard and the Discover
// empty state both already say so.
void VortexBridge::maybeAutoFetchCatalog() {
    if (m_catalogRefreshing || m_catalogAutoFetchTried) return;
    m_catalogAutoFetchTried = true;

    if (!has_secret(kIgdbId) || !has_secret(kIgdbSecret)) {
        vlog::line("Catalog",
                   "skipped: no IGDB credentials, so Discover stays empty "
                   "until they are entered in Settings");
        return;
    }

    // Only when there is nothing to lose. A populated catalog is refreshed on
    // demand from Settings, never automatically -- re-downloading 5,700 games
    // on every launch would be indefensible.
    if (m_discoverCandidateCount > 0) return;

    vlog::line("Catalog",
               "empty on this install; starting the one-time download "
               "in the background");
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
