#include "vortex_bridge.h"
#include "app_paths.h"
#include "game_manager.h"
#include "igdb_manager.h"
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
    loadWishlist();
    loadFavoriteSnapshots();
    loadPlayedLedger();
    seedPlayedLedgerFromStats();
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

static QString getLastPlayedDate(const std::string &gameKey,
                                 const fs::path &baseDir) {
    std::ifstream file(baseDir / "playtime_sessions.log");
    if (!file.is_open()) return "Never";
    // The key is the first field of "KEY | NAME | SECONDS | START | END", so it
    // is matched as a prefix rather than searched for anywhere in the line -- a
    // plain find() makes "igdb_1877" match every "igdb_18770" session too.
    const std::string prefix = gameKey + " |";
    std::string line, lastDate = "Never";
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.compare(0, prefix.size(), prefix) == 0) {
            size_t lastPipe = line.find_last_of('|');
            if (lastPipe != std::string::npos)
                lastDate = line.substr(lastPipe + 2, 10);
        }
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
static QString formatPlaytimeLabel(long long seconds) {
    if (seconds <= 0)
        return QStringLiteral("0 Hours");
    if (seconds < 3600) {
        const long long mins = std::max(1LL, seconds / 60);
        return QString::number(mins) + (mins == 1 ? " Minute" : " Minutes");
    }
    return QString::number(seconds / 3600.0, 'f', 1) + " Hours";
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
        long long playtimeSeconds = 0;
        long long lastPlayed = 0;
        if (game.source == "Steam" && game.appid > 0) {
            playtimeSeconds = get_steam_playtime_seconds(game.appid);
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

    // Steam's own total wins for Steam games — it includes sessions launched
    // outside Vortex, which our log never sees. Ours covers everything else.
    long long ptSec = 0;
    if (bg.source == "Steam")
        ptSec = get_steam_playtime_seconds(bg.appid);
    if (ptSec <= 0)
        ptSec = get_playtime(ptKey);

    game["playtime"]   = formatPlaytimeLabel(ptSec);
    game["lastPlayed"] = getLastPlayedDate(ptKey, m_baseDir);

    // The same three facts in a form something other than a label can use: the
    // Played tab keys its ledger on playKey, sorts on lastPlayedAt and sums
    // playtimeSeconds. Derived here rather than recomputed there, so the tab and
    // the details page can never disagree about how long you played something.
    game["playKey"]         = QString::fromStdString(ptKey);
    game["playtimeSeconds"] = static_cast<qlonglong>(ptSec);

    // Steam knows the exact second; everything else is pinned to the day its
    // last session ended.
    long long lastPlayedAt = 0;
    if (bg.source == "Steam")
        lastPlayedAt = get_steam_last_played(bg.appid);
    if (lastPlayedAt <= 0)
        lastPlayedAt = parseDateToEpoch(game["lastPlayed"].toString());
    game["lastPlayedAt"] = static_cast<qlonglong>(lastPlayedAt);

    if (bg.igdb_id > 0) {
        GameMetadata meta = get_game_metadata(bg.igdb_id);
        game["developer"]  = QString::fromStdString(meta.developer);
        game["rating"]     = meta.rating;
        game["timeToBeat"] = (meta.time_to_beat_seconds > 0)
                             ? QString::number(meta.time_to_beat_seconds / 3600) + " Hours"
                             : "N/A";
        
        QString genresStr = "";
        for (size_t i = 0; i < meta.main_genres.size(); ++i) {
            genresStr += QString::fromStdString(meta.main_genres[i]);
            if (i < meta.main_genres.size() - 1) genresStr += ", ";
        }
        game["genres"] = genresStr.isEmpty() ? "Unknown" : genresStr;

        QString tagsStr = "";
        for (size_t i = 0; i < meta.all_genres.size(); ++i) {
            tagsStr += QString::fromStdString(meta.all_genres[i]);
            if (i < meta.all_genres.size() - 1) tagsStr += ", ";
        }
        game["tags"] = tagsStr.isEmpty() ? "Unknown" : tagsStr;
    } else {
        game["developer"]  = QString("Unknown");
        game["rating"]     = 0.0;
        game["timeToBeat"] = QString("N/A");
        game["genres"]     = QString("Unknown");
        game["tags"]       = QString("Unknown");
    }

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
        emit gameListChanged();   // favoriteGames is notified by this one
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
    for (const QVariant &entry : m_favoriteSnapshots) {
        QVariantMap snapshot = entry.toMap();
        const QString name = snapshot.value("name").toString();
        if (name.isEmpty() || seen.contains(name.toLower()))
            continue;
        // The preference file remains the source of truth; a snapshot whose
        // heart was removed elsewhere must not linger.
        if (get_game_preference(name.toStdString()) <= 0.0)
            continue;

        // A game that was played and then uninstalled already has a full row in
        // the played ledger -- artwork, playtime, developer and genres. That row
        // is both richer and fresher than a snapshot frozen at the moment of the
        // heart click, so it wins here for the same reason the live library row
        // wins above. Without this, hearting from the Played tab replaced the
        // page with the bare snapshot and the art and metadata vanished.
        QVariantMap played = findGameByName(m_playedLedger, name);
        if (!played.isEmpty()) {
            played["installed"] = false;
            played["matched"]   = false;
            played["status"]    = 1.0;
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

void VortexBridge::loadFavoriteSnapshots() {
    m_favoriteSnapshots.clear();

    QFile file(pathToQString(favoriteSnapshotPath()));
    if (!file.open(QIODevice::ReadOnly)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) return;

    for (const QJsonValue &value : doc.array()) {
        if (value.isObject())
            m_favoriteSnapshots << value.toObject().toVariantMap();
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
    for (int i = 0; i < m_favoriteSnapshots.size(); ++i) {
        if (QString::compare(m_favoriteSnapshots[i].toMap().value("name").toString(),
                             name, Qt::CaseInsensitive) == 0) {
            if (!favorited) {
                m_favoriteSnapshots.removeAt(i);
                saveFavoriteSnapshots();
            }
            return;
        }
    }

    if (!favorited || !findGameByName(m_gameList, name).isEmpty())
        return;   // un-favourited, or owned and therefore already renderable

    QVariantMap snapshot = findGameByName(m_recommendationList, name);
    if (snapshot.isEmpty())
        snapshot = findGameByName(m_wishlist, name);
    // The played ledger last: a game hearted from the Played tab is in none of
    // the lists above, and a snapshot holding nothing but a name is not
    // renderable -- which is exactly what emptied the details page.
    if (snapshot.isEmpty())
        snapshot = findGameByName(m_playedLedger, name);
    if (snapshot.isEmpty())
        snapshot["name"] = name;

    snapshot.remove("score");
    snapshot.remove("reason");
    snapshot.remove("inspiredBy");
    snapshot.remove("section");
    snapshot.remove("similarity");
    snapshot["status"] = 1.0;
    snapshot["addedAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);

    m_favoriteSnapshots << snapshot;
    saveFavoriteSnapshots();
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

    for (const QJsonValue &value : doc.array()) {
        if (value.isObject())
            m_wishlist << value.toObject().toVariantMap();
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

        entry["playtimeSeconds"] = static_cast<qlonglong>(stat.seconds);
        entry["playtime"]        = formatPlaytimeLabel(stat.seconds);

        const QString lastPlayed = getLastPlayedDate(stat.key, m_baseDir);
        entry["lastPlayed"]   = lastPlayed;
        entry["lastPlayedAt"] = static_cast<qlonglong>(parseDateToEpoch(lastPlayed));

        const fs::path gameDir =
            imageRoot / steamgriddb_image_folder_name(name.toStdString());
        for (const auto &slot : kPlayedArtSlots)
            entry[slot.first] = findImagePath(gameDir, slot.second);

        entry["developer"]  = QStringLiteral("Unknown");
        entry["genres"]     = QStringLiteral("Unknown");
        entry["tags"]       = QStringLiteral("Unknown");
        entry["rating"]     = 0.0;
        entry["timeToBeat"] = QStringLiteral("N/A");
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
}

void VortexBridge::saveSettings() const {
    QJsonObject object;
    object["curatedOnly"] = m_curatedOnly;
    object["ignorePlayedGames"] = m_ignorePlayedGames;
    object["ignoreLikedGames"] = m_ignoreLikedGames;

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

bool VortexBridge::isWishlisted(QString name) const {
    for (const QVariant &entry : m_wishlist) {
        if (QString::compare(entry.toMap().value("name").toString(), name,
                             Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

bool VortexBridge::toggleWishlist(QString name) {
    for (int i = 0; i < m_wishlist.size(); ++i) {
        if (QString::compare(m_wishlist[i].toMap().value("name").toString(), name,
                             Qt::CaseInsensitive) == 0) {
            m_wishlist.removeAt(i);
            saveWishlist();
            emit wishlistChanged();
            return false;
        }
    }

    // Snapshot the metadata: wishlist entries are unowned, so they are absent
    // from gameList and there would be nothing to render them from later. It
    // also keeps the tab working with Postgres stopped and no network.
    QVariantMap snapshot;
    for (const QVariant &entry : m_recommendationList) {
        const QVariantMap item = entry.toMap();
        if (QString::compare(item.value("name").toString(), name, Qt::CaseInsensitive) == 0) {
            snapshot = item;
            break;
        }
    }
    if (snapshot.isEmpty())
        snapshot["name"] = name;

    snapshot.remove("score");
    snapshot.remove("reason");
    snapshot.remove("inspiredBy");
    snapshot.remove("section");
    snapshot.remove("similarity");
    snapshot["addedAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);

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

    QThread *thread = QThread::create([this, baseDir, imagesRoot]() {
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

        std::vector<BridgeGame> internalGames;
        for (const SteamGame &g : steamGames) {
            BridgeGame bg;
            bg.name       = g.name;
            bg.source     = "Steam";
            bg.appid      = g.appid;
            bg.igdb_id    = g.igdb_id;
            bg.installDir = g.installDir;
            internalGames.push_back(std::move(bg));
        }
        for (const temp_GameEntry &g : localEntries) {
            BridgeGame bg;
            bg.name       = g.name;
            bg.source     = "Local";
            bg.igdb_id    = g.igdb_id;
            bg.installDir = g.installDir;
            bg.gamePath   = g.gamePath;
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

        if (found.source == "Steam") {
            launch_steam_game_by_appid(found.appid);

            // Steam's protocol launch hands back no process to wait on, so the
            // session is observed: wait for the game to appear, then to go away.
            played = monitor_steam_session(found.appid, found.installDir,
                                           &sessionStart, &sessionEnd);
        } else {
            sessionStart = std::time(nullptr);
            // launchGame() in game_manager blocks until the process exits.
            ::launchGame(found.gamePath);
            sessionEnd = std::time(nullptr);
            played     = sessionEnd > sessionStart;
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
            vlog::item("Play", found.name, vlog::Status::Ok,
                       "played for " + formatDuration(durationSeconds));

            record_play_session(ptKey, found.name, sessionStart, sessionEnd);

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
// updatePreference — updates preferences.json via the engine, then does a
// lightweight refresh so the favourite state updates immediately.
// ─────────────────────────────────────────────────────────────────────────────
double VortexBridge::updatePreference(QString name, double score) {
    double newStatus = toggle_game_preference(name.toStdString(), score);
    updateFavoriteSnapshot(name, newStatus > 0.0);
    refreshGameList();   // emits gameListChanged, which favoriteGames binds to
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
