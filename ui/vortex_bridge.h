#ifndef VORTEX_BRIDGE_H
#define VORTEX_BRIDGE_H

#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Internal game record — holds real paths needed for launching / uninstalling.
// Not exposed to QML directly; QML gets a QVariantMap built from this.
struct BridgeGame {
    std::string name;
    std::string source;      // "Steam" or "Local"
    int         appid    = 0;
    long long   igdb_id  = 0;
    fs::path    installDir;
    fs::path    gamePath;    // local exe path only (empty for Steam games)
    std::string scannedName; // the name pass 1 found, before any rename
    std::string customName;  // the user's own title, "" when not renamed
};

class VortexBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList gameList    READ gameList    NOTIFY gameListChanged)
    Q_PROPERTY(QVariantList recommendationList READ recommendationList NOTIFY recommendationListChanged)
    Q_PROPERTY(bool         isLoading   READ isLoading   NOTIFY loadingChanged)
    Q_PROPERTY(bool         isRecommendationLoading READ isRecommendationLoading NOTIFY recommendationLoadingChanged)
    Q_PROPERTY(int          currentMood READ currentMood NOTIFY moodChanged)
    Q_PROPERTY(QString      recommendationStatus READ recommendationStatus NOTIFY recommendationStatusChanged)
    Q_PROPERTY(QVariantList localDirectories READ localDirectories NOTIFY localDirectoriesChanged)
    // Owned games the user hearted, and saved unowned games. Separate concepts:
    // a favourite is taste (and feeds the recommender), a wishlist entry is
    // intent to acquire (and deliberately does not).
    // Deliberately NOT notified by gameListChanged. The library grid binds to
    // gameList as a plain JS array, so that signal hands GridView a new array
    // and throws the scroll position away -- which is what sent the library
    // back to the top every time a game was hearted. Bulk paths still emit
    // both (refreshGameList); updatePreference() edits the one row in place
    // and emits this alone.
    Q_PROPERTY(QVariantList favoriteGames READ favoriteGames NOTIFY favoritesChanged)
    Q_PROPERTY(QVariantList wishlistGames READ wishlistGames NOTIFY wishlistChanged)
    // Everything ever played, whether or not it is still on the machine. Backed
    // by a ledger rather than by gameList, because the whole point is the games
    // that are no longer there -- see loadPlayedLedger().
    Q_PROPERTY(QVariantList playedGames READ playedGames NOTIFY playedGamesChanged)
    // Games the user took out of the launcher without uninstalling them. The
    // scan honours this list, so a removed game does not come back on the next
    // rescan; Settings lists them so one can be put back.
    Q_PROPERTY(QVariantList removedGames READ removedGames NOTIFY removedGamesChanged)
    // "Only well-known games" in Settings. Restricts the DISCOVER section to
    // titles that are popular, very well rated or newly released; the library
    // section is untouched, because owned rows carry no rating counts at all.
    Q_PROPERTY(bool         curatedOnly READ curatedOnly NOTIFY curatedOnlyChanged)
    // "Ignore games you've played" in Settings. Drops the played history from
    // the profile the three moods are ranked with, leaving the mood itself and
    // the hearted games. Neutral is excluded: it has no mood vector to fall
    // back on, so it would be left ranking on the quality term alone.
    Q_PROPERTY(bool         ignorePlayedGames READ ignorePlayedGames NOTIFY ignorePlayedGamesChanged)
    // "Ignore games you've liked", the counterpart. Drops the hearted games
    // from the same profile. A game that was played AND hearted counts as
    // played and survives this one; both settings together leave the mood
    // alone to rank with.
    Q_PROPERTY(bool         ignoreLikedGames READ ignoreLikedGames NOTIFY ignoreLikedGamesChanged)
    // "Use Steam's own playtime" in Settings. Off, Vortex shows the total it
    // keeps itself: Steam's lifetime figure imported once when a game is first
    // seen, plus every session since, with idle time deducted. On, Steam games
    // fall back to Steam's live figure, which counts play outside the launcher
    // but can carry no idle breakdown -- so it is shown exactly as Steam
    // reports it, with nothing taken out.
    //
    // Display only. The import and the session tracking run either way, or
    // turning this off later would show a total missing everything that
    // happened while it was on.
    Q_PROPERTY(bool         useSteamPlaytime READ useSteamPlaytime NOTIFY useSteamPlaytimeChanged)

    // ---- Scan progress ---------------------------------------------------
    // The library scan used to hide behind a full-screen modal overlay with an
    // indeterminate spinner, so a cold-cache machine looked hung for as long as
    // it took to resolve every title and download every cover. These drive a
    // small non-blocking indicator instead, and the app stays usable.
    Q_PROPERTY(bool    scanActive READ scanActive NOTIFY scanProgressChanged)
    Q_PROPERTY(QString scanPhase  READ scanPhase  NOTIFY scanProgressChanged)
    Q_PROPERTY(int     scanDone   READ scanDone   NOTIFY scanProgressChanged)
    Q_PROPERTY(int     scanTotal  READ scanTotal  NOTIFY scanProgressChanged)

    // Bumped when artwork or metadata for an already-published game lands.
    //
    // The library grid's model is a plain JS array; re-emitting gameListChanged
    // for each downloaded cover would hand GridView a NEW array every time,
    // rebuilding every delegate and throwing away scroll position and
    // controller focus. Instead the maps inside m_gameList are mutated in place
    // and this counter changes, so only the bindings that mention it
    // re-evaluate. See gameDetailsFor().
    Q_PROPERTY(int     artRevision READ artRevision NOTIFY artRevisionChanged)

    // ---- Discovery catalog download --------------------------------------
    // Discover needs a one-time ~5,700-game fetch from IGDB. It runs for
    // minutes, so it reports a running count rather than a bare busy flag --
    // the total is not knowable in advance.
    Q_PROPERTY(QString catalogPhase   READ catalogPhase   NOTIFY catalogProgressChanged)
    Q_PROPERTY(int     catalogFetched READ catalogFetched NOTIFY catalogProgressChanged)
    Q_PROPERTY(bool    catalogRefreshing READ isCatalogRefreshing NOTIFY catalogRefreshingChanged)

    // One sentence naming what is actually broken, or empty when nothing is.
    // Rendered on the Recommendations header because that line is always
    // visible, unlike the sections beneath it.
    Q_PROPERTY(QString authBlocker READ authBlocker NOTIFY credentialsChanged)

    // ---- Browse ----------------------------------------------------------
    // Free search over the whole IGDB catalog, not just the ~5,700 titles the
    // recommender downloaded. Results are covers and a few facts; the details
    // page asks for everything else when it opens, and Steam's reviews after
    // that once IGDB has said which Steam app the game is.
    Q_PROPERTY(QVariantList browseResults   READ browseResults   NOTIFY browseResultsChanged)
    Q_PROPERTY(bool         browseSearching READ browseSearching NOTIFY browseResultsChanged)
    // Why the result list is empty, or "" when it is not (or nothing was asked).
    Q_PROPERTY(QString      browseStatus    READ browseStatus    NOTIFY browseResultsChanged)
    Q_PROPERTY(QVariantMap  browseDetails        READ browseDetails        NOTIFY browseDetailsChanged)
    Q_PROPERTY(bool         browseDetailsLoading READ browseDetailsLoading NOTIFY browseDetailsChanged)
    Q_PROPERTY(QVariantMap  browseReviews        READ browseReviews        NOTIFY browseReviewsChanged)
    Q_PROPERTY(bool         browseReviewsLoading READ browseReviewsLoading NOTIFY browseReviewsChanged)
    // What the tab shows before anything is typed: recent releases, newest
    // first, in the same row shape as browseResults.
    Q_PROPERTY(QVariantList browseNewReleases        READ browseNewReleases        NOTIFY browseNewReleasesChanged)
    Q_PROPERTY(bool         browseNewReleasesLoading READ browseNewReleasesLoading NOTIFY browseNewReleasesChanged)
    Q_PROPERTY(QString      browseNewReleasesStatus  READ browseNewReleasesStatus  NOTIFY browseNewReleasesChanged)

    // Where each launch is, by game name, for the Play button: launching from
    // the press until the game is up, running until it exits. A name is in at
    // most one of the two.
    Q_PROPERTY(QStringList launchingGames READ launchingGames NOTIFY launchStateChanged)
    Q_PROPERTY(QStringList runningGames   READ runningGames   NOTIFY launchStateChanged)

public:
    explicit VortexBridge(QObject *parent = nullptr);

    QVariantList gameList()    const { return m_gameList;    }
    QStringList  launchingGames() const { return m_launchingGames; }
    QStringList  runningGames()   const { return m_runningGames;   }
    QVariantList recommendationList() const { return m_recommendationList; }
    QVariantList favoriteGames() const;
    // The saved entries only. m_wishlist also holds rows whose entry has been
    // taken off the list -- see toggleWishlist() -- which this filters out.
    QVariantList wishlistGames() const;
    QVariantList playedGames() const;
    QVariantList removedGames() const { return m_removed; }
    bool         isLoading()   const { return m_isLoading;   }
    bool         isRecommendationLoading() const { return m_isRecommendationLoading; }
    int          currentMood() const { return m_currentMood; }
    bool         curatedOnly() const { return m_curatedOnly; }
    bool         ignorePlayedGames() const { return m_ignorePlayedGames; }
    bool         ignoreLikedGames() const { return m_ignoreLikedGames; }
    bool         useSteamPlaytime() const { return m_useSteamPlaytime; }
    QString      recommendationStatus() const { return m_recommendationStatus; }

    bool         scanActive() const { return m_scanActive; }
    QString      scanPhase()  const { return m_scanPhase;  }
    int          scanDone()   const { return m_scanDone;   }
    int          scanTotal()  const { return m_scanTotal;  }
    int          artRevision() const { return m_artRevision; }
    QString      catalogPhase()   const { return m_catalogPhase; }
    int          catalogFetched() const { return m_catalogFetched; }
    QString      authBlocker() const;
    QVariantList browseResults()   const { return m_browseResults; }
    bool         browseSearching() const { return m_browseSearching; }
    QString      browseStatus()    const { return m_browseStatus; }
    QVariantMap  browseDetails()        const { return m_browseDetails; }
    bool         browseDetailsLoading() const { return m_browseDetailsLoading; }
    QVariantMap  browseReviews()        const { return m_browseReviews; }
    bool         browseReviewsLoading() const { return m_browseReviewsLoading; }
    QVariantList browseNewReleases()        const { return m_browseNewReleases; }
    bool         browseNewReleasesLoading() const { return m_browseNewReleasesLoading; }
    QString      browseNewReleasesStatus()  const { return m_browseNewReleasesStatus; }

    // The current map for one game by name, including artwork and metadata
    // that landed after the list was published. Reference artRevision in the
    // same binding to make QML re-read it as results arrive.
    Q_INVOKABLE QVariantMap gameDetailsFor(QString name) const;

    // Same, but keyed on the install directory instead of the title.
    //
    // The name is NOT a stable identity: pass 1 publishes local games under
    // their folder name and pass 2 replaces it with the canonical IGDB title,
    // so a delegate holding the pass-1 snapshot asks gameDetailsFor() for a
    // name the live row no longer has and gets nothing back -- which is why a
    // game whose folder disagrees with its IGDB title showed NO ART forever.
    // installDir is fixed at scan time and survives the rename.
    Q_INVOKABLE QVariantMap gameDetailsForInstallDir(QString installDir) const;

    // Configured local game folders, as plain path strings for the settings panel.
    QVariantList localDirectories() const;

    // Called by the QML mood picker on startup; triggers a full scan.
    Q_INVOKABLE void   initialize(int mood);

    // Changes the mood after startup, from the settings panel. Re-ranks only:
    // the installed set has not changed, so rescanning the disk here would cost
    // seconds for a result identical to what is already loaded.
    Q_INVOKABLE void   setMood(int mood);

    // Toggles the "only well-known games" filter from the settings panel.
    // Persists to settings.json and re-ranks; like setMood() it does not
    // rescan the disk, since the installed set has not changed.
    Q_INVOKABLE void   setCuratedOnly(bool enabled);

    // Toggles "ignore games you've played" from the settings panel. Same
    // contract as setCuratedOnly(): persist, notify, re-rank, no rescan.
    Q_INVOKABLE void   setIgnorePlayedGames(bool enabled);

    // Toggles "ignore games you've liked". Same contract again.
    Q_INVOKABLE void   setIgnoreLikedGames(bool enabled);

    // Switches where Steam games' playtime is read from. Persists and rebuilds
    // the list; unlike the three above it does not re-rank, because it changes
    // which number is displayed, not what the recommender was given.
    Q_INVOKABLE void   setUseSteamPlaytime(bool enabled);

    // Full scan: Steam + local dirs + SteamGridDB artwork. Runs on a background thread.
    Q_INVOKABLE void   loadGames();

    // Lightweight refresh: rebuilds QVariantList from the cached m_internalGames
    // without re-scanning or hitting the network. Safe to call on main thread.
    Q_INVOKABLE void   refreshGameList();
    Q_INVOKABLE void   loadRecommendations();

    Q_INVOKABLE double updatePreference(QString name, double score);

    // Clears every favourite in one go, for a fresh recommendation profile.
    // Returns how many were removed so the UI can report it. Unlike
    // updatePreference() this does reload the recommendations: a deliberate
    // "start over" is exactly the case where leaving the taste-derived list on
    // screen would contradict what was asked for.
    Q_INVOKABLE int    resetPreferences();
    // Takes the origin explicitly rather than defaulting it: Q_INVOKABLE default
    // parameters are unreliable across moc versions.
    Q_INVOKABLE void   launchGameFrom(QString name, QString origin);
    // Closes a running game: asks its windows to close, then terminates what
    // is left a few seconds later. The launch thread sees it exit and records
    // the session as usual.
    Q_INVOKABLE void   quitGame(QString name);
    Q_INVOKABLE void   logRecommendationClick(QString name, int rank);
    Q_INVOKABLE void   uninstallGame(QString name);

    // Hides a game from the launcher WITHOUT touching its files -- the counter
    // part to uninstallGame(), for a title that is installed and staying that
    // way but has no business being in the library. installDir is passed
    // alongside the name because it is the only identity that survives pass 2
    // renaming a local game (see gameDetailsForInstallDir).
    Q_INVOKABLE void   removeFromLibrary(QString name, QString installDir);

    // Opens a game's install folder in Explorer. False if the folder is gone.
    Q_INVOKABLE bool   openInstallFolder(QString path);

    // Points a local game at a different .exe and records it in exe_cache.txt
    // so the choice survives rescans. Returns the new path, "" if refused.
    Q_INVOKABLE QString setGameExecutable(QString installDir, QString exeUrl);

    // Gives an owned game the user's own title, recorded in name_overrides.txt
    // so it survives rescans, and makes it the name IGDB and SteamGridDB are
    // searched by from then on. An empty name puts the scanned one back.
    // Refused (false) while a scan is running.
    Q_INVOKABLE bool   renameGame(QString installDir, QString newName);

    // Puts one back. Needs a full rescan: the game is gone from
    // m_internalGames, and only the scan can rebuild it from the disk.
    Q_INVOKABLE void   restoreToLibrary(QString name);
    // Returns true only when a new directory was actually stored, so the UI can
    // tell "added" apart from "already in the list".
    Q_INVOKABLE bool   addLocalGameDirectory(QString folderUrl);

    // Drops a configured folder and deletes the artwork of every game that lived
    // only there. Reports the outcome through directoryRemoved().
    Q_INVOKABLE void   removeLocalGameDirectory(QString folderPath);
    Q_INVOKABLE void   refreshLocalDirectories();
    Q_INVOKABLE void   logGameClick(QString name, QString genres, QString tags);

    // Wishlist. Entries store a metadata snapshot rather than just a name:
    // wishlisted games are unowned, so they are absent from gameList and there
    // would otherwise be nothing to render the card or details page from. It
    // also means the tab still works with Postgres down and no network.
    Q_INVOKABLE bool   toggleWishlist(QString name);
    Q_INVOKABLE bool   isWishlisted(QString name) const;

    // Called when a details page opens on an unowned game: moves its live-art
    // lookup to the front of the queue, so the page's banner and logo URLs are
    // the next answer. No-op for owned games, which already have SteamGridDB
    // art under Images/, and for games whose URLs are already cached.
    Q_INVOKABLE void   ensureArtwork(QString name);

    // Developer, genres, rating and time-to-beat for one unowned pick, on the
    // same lazy terms as ensureArtwork(). A row copied out of a live list
    // already carries them and this no-ops; the case it exists for is a
    // favourite that outlived every list and has only a name to go on.
    Q_INVOKABLE void   ensureMetadata(QString name);

    // ---- Browse ----------------------------------------------------------
    // Runs one IGDB search off the UI thread. A newer call supersedes an older
    // one still in flight, whose answer is then dropped -- typing fast must
    // never leave the results of an earlier prefix on screen.
    Q_INVOKABLE void   searchCatalog(QString query);
    Q_INVOKABLE void   clearBrowse();
    // Fetches recent releases for the empty search box. Cheap to call often:
    // it does nothing while a fetch is running or within an hour of a good one.
    Q_INVOKABLE void   loadNewReleases();

    // Everything the Browse details page shows for one IGDB id, then Steam's
    // reviews for it when IGDB knows its Steam app. Also keeps a snapshot of
    // the game, so wishlisting, hearting or marking it played from that page
    // stores full metadata rather than a bare name.
    Q_INVOKABLE void   loadBrowseDetails(qlonglong igdbId);
    // The same page for a recommendation, which knows only its name. The row
    // seeds the page at once; the IGDB id is resolved from the library, the
    // offline cache or a lookup, and the full answer replaces the seed when it
    // lands. A name IGDB has no answer for keeps the seed.
    Q_INVOKABLE void   loadBrowseDetailsForGame(QString name);

    // Whether preferences.json holds a heart for exactly this name.
    Q_INVOKABLE bool   isFavorite(QString name) const;

    // "Add to Played", for a game played somewhere Vortex never saw. Adds a
    // manual row to the played ledger, or takes one back off; a game with
    // real recorded playtime cannot be un-played, and returns false.
    // Exported to manual_played.txt so the recommender counts it as played.
    Q_INVOKABLE bool   togglePlayed(QString name);

    // "tracked" (real playtime), "manual" (added by hand), or "none".
    Q_INVOKABLE QString playedState(QString name) const;

    // ---- First-run credentials -------------------------------------------
    // Vortex runs with no credentials at all: the Steam library, playtime and
    // the local recommender never needed them. These only gate artwork
    // (SteamGridDB) and the Discover catalog (IGDB), so the wizard that calls
    // them is skippable by design and reachable again from Settings.

    // True when both IGDB values and the SteamGridDB key are present, i.e. the
    // wizard has nothing left to ask for. Drives whether it is shown at all.
    Q_INVOKABLE bool   hasCredentials() const;

    // Reports which individual keys are set, so Settings can show the state of
    // each rather than one all-or-nothing flag.
    Q_INVOKABLE QVariantMap credentialStatus() const;

    // Writes analytics/.env and re-reads it in place (see reload_secrets in
    // secrets.h -- without that the new keys do nothing until a restart).
    // Empty arguments leave the corresponding existing value untouched, so
    // Settings can update one key without clearing the others.
    Q_INVOKABLE bool   saveCredentials(QString igdbClientId,
                                       QString igdbClientSecret,
                                       QString steamGridDbKey);

    // Runs igdb_catalog.py --refresh in the background. Several minutes on a
    // full fetch, hence the signals rather than a blocking return.
    // Checks the entered keys against the live providers and reports through
    // credentialsValidated(). Does not block saving -- an offline machine must
    // still be able to store a key the user knows is good.
    Q_INVOKABLE void   validateCredentials(QString igdbClientId,
                                           QString igdbClientSecret,
                                           QString steamGridDbKey);

    Q_INVOKABLE void   refreshCatalog();
    Q_INVOKABLE bool   isCatalogRefreshing() const { return m_catalogRefreshing; }

signals:
    void launchStateChanged();
    void wishlistChanged();
    void playedGamesChanged();
    void removedGamesChanged();
    void gameListChanged();
    void favoritesChanged();
    void recommendationListChanged();
    void loadingChanged();
    void recommendationLoadingChanged();
    void moodChanged();
    void curatedOnlyChanged();
    void ignorePlayedGamesChanged();
    void ignoreLikedGamesChanged();
    void useSteamPlaytimeChanged();
    void recommendationStatusChanged();
    void localDirectoriesChanged();
    void directoryRemoved(QString folder, int gamesRemoved, int artworkDeleted);
    void credentialsChanged();
    // Per-key outcome of validateCredentials(): igdbOk, igdbDetail,
    // sgdbOk, sgdbDetail, and the *Rejected flags separating "refused" from
    // "could not reach".
    void credentialsValidated(QVariantMap result);
    void scanProgressChanged();
    void artRevisionChanged();
    // One owned row was rewritten in place (a rename, and again once its new
    // art and metadata land). The grid follows through artRevision; this is
    // for a details page, which holds a copy of the row.
    void gameRowChanged(QString installDir);
    void catalogRefreshingChanged();
    void catalogProgressChanged();
    // ok=false carries the script's stderr in `details`; the wizard shows it
    // rather than a generic failure, because the usual cause is a mistyped
    // client secret and the message says so.
    void catalogRefreshFinished(bool ok, QString details);
    void browseResultsChanged();
    void browseDetailsChanged();
    void browseReviewsChanged();
    void browseNewReleasesChanged();

private:
    QVariantList            m_gameList;
    QVariantList            m_recommendationList;
    QVariantList            m_wishlist;
    QVariantList            m_favoriteSnapshots;
    // Persisted play history. Keyed by the same playtime key stats_manager uses,
    // and only ever added to, so an uninstalled game keeps its row.
    QVariantList            m_playedLedger;
    // Games removed from the library. Records rather than bare names: identity
    // is appid / installDir / canonical name in that order, and only a record
    // can carry all three.
    QVariantList            m_removed;
    std::vector<BridgeGame> m_internalGames;
    bool                    m_isLoading   = false;
    bool                    m_rescanQueued = false;  // scan requested while one was running
    // Renames whose IGDB / artwork pass is still running. A scan waits for
    // them, or its Steam baseline import could race the playtime rekey.
    int                     m_renamesInFlight = 0;
    bool                    m_isRecommendationLoading = false;
    bool                    m_catalogRefreshing = false;
    // When maybeAutoFetchCatalog() last started a fetch. Invalid until the
    // first one, and invalidated again when credentials start working.
    QElapsedTimer           m_catalogAutoFetchClock;
    QString                 m_catalogPhase;
    int                     m_catalogFetched = 0;
    // How many unowned candidates the last run had to choose from. Zero means
    // the catalog was never fetched, which is what maybeAutoFetchCatalog() acts
    // on.
    int                     m_discoverCandidateCount = 0;
    // Days since the catalog was fetched, from the last run's sidecar; -1 when
    // unknown. maybeAutoFetchCatalog() refreshes once this reaches 5.
    double                  m_catalogAgeDays = -1.0;
    // Optimistic until a real call says otherwise, so a launcher that has not
    // contacted IGDB yet does not accuse the user of having bad keys.
    bool                    m_igdbAuthOk = true;
    bool                    m_artworkAuthOk = true;
    QString                 m_authBlocker;

    // Scan progress. Written from the scan thread through queued invocations,
    // so they are only ever touched on the main thread.
    bool                    m_scanActive = false;
    QString                 m_scanPhase;
    int                     m_scanDone  = 0;
    int                     m_scanTotal = 0;
    int                     m_artRevision = 0;
    // Coalesces artwork/metadata updates: without it a fast cache-hit scan
    // would emit a notify per game and re-run every bound expression in the
    // grid dozens of times a second.
    class QTimer           *m_artNotifyTimer = nullptr;
    // Appids handed to Steam's uninstall and not yet seen gone; see
    // watchSteamUninstall(). Stops a second press from starting a second poll.
    QSet<int>               m_pendingSteamUninstalls;
    void watchSteamUninstall(int appid);
    // Main thread only; the launch thread posts its changes over.
    QStringList             m_launchingGames;
    QStringList             m_runningGames;
    void setLaunchState(const QString &name, bool launching, bool running);
    bool                    m_recommendationQueued = false;  // coalesced by the debounce timer
    // Neutral (see MOOD_LABELS in analytics/scoring.py). With no mood chosen
    // yet, apply none rather than silently using Chill's weights.
    int                     m_currentMood = 3;
    // Unlike mood, which is a per-session choice and resets to Neutral each
    // launch, this is a preference about how the app behaves and persists.
    bool                    m_curatedOnly = false;
    bool                    m_ignorePlayedGames = false;
    bool                    m_ignoreLikedGames = false;
    bool                    m_useSteamPlaytime = false;
    QString                 m_recommendationStatus = "Recommendations not loaded";
    fs::path                m_baseDir;    // resolved project root (contains Images/)

    void        setLoading(bool val);

    // Scan progress plumbing. setScanProgress() and updateGameRow() touch
    // member state and so must run on the main thread; reportScanProgress()
    // and updateGameRow() are the thread-safe entry points the scan lambda
    // calls, and marshal internally.
    void        setScanProgress(bool active, const QString &phase, int done, int total);
    void        reportScanProgress(const QString &phase, int done, int total);
    void        updateGameRow(int index, const BridgeGame &game);
    void        scheduleArtNotify();
    void        setCatalogProgress(const QString &phase, int fetched);
    void        reportCatalogProgress(const QString &phase, int fetched);

    // Starts the one-time catalog download when this install has never had
    // one and IGDB credentials are available.
    void        maybeAutoFetchCatalog();

    // Fire-and-forget diagnostics for a clicked recommendation.
    void        explainGameToLog(const QString &name,
                                    const QStringList &extraArgs = QStringList());

    // Everything known about one game, printed when its profile opens.
    void        logGameProfile(const QString &name);
    void        setRecommendationLoading(bool val);
    void        setRecommendationStatus(const QString &status);
    QVariantMap buildGameMap(const BridgeGame &bg) const;

    void        loadWishlist();
    void        saveWishlist() const;
    fs::path    wishlistPath() const;

    // ---- Removed games ----------------------------------------------------
    void        loadRemovedGames();
    void        saveRemovedGames() const;
    fs::path    removedGamesPath() const;

    // Name-only match, for the lists that hold snapshots rather than live rows
    // (favourite snapshots, the played ledger).
    bool        isRemovedName(const QString &name) const;

    // ---- Played ledger ----------------------------------------------------
    // Same reasoning as the wishlist and the favourite snapshots: an entry with
    // no row in gameList has nothing to draw a card or a details page from, so
    // each one carries a renderable copy of its metadata.
    void        loadPlayedLedger();
    void        savePlayedLedger() const;
    fs::path    playedLedgerPath() const;

    // Backfills the ledger from playtime_stats.txt, which has been accumulating
    // since long before this tab existed. Without it a first run shows only the
    // games that happen to be installed right now, which is the opposite of the
    // point.
    void        seedPlayedLedgerFromStats();
    void        backfillPlayedLedgerMetadata();

    // Upserts a snapshot for every live row with playtime on it. This is also
    // the only path that captures a Steam game played entirely outside Vortex:
    // its total comes from localconfig.vdf while it is installed, and the ledger
    // is what keeps it afterwards.
    void        syncPlayedLedger();

    // General app settings, so later toggles have somewhere to live rather
    // than each growing its own file.
    void        loadSettings();
    void        saveSettings() const;
    fs::path    settingsPath() const;

    // Renderable copies of favourited games that are not installed. Without
    // these a hearted Discover pick is stored in preferences.json but has no
    // row anywhere to draw a card from.
    void        loadFavoriteSnapshots();
    void        saveFavoriteSnapshots() const;
    void        updateFavoriteSnapshot(const QString &name, bool favorited);
    fs::path    favoriteSnapshotPath() const;

    // Appends one NDJSON line to analytics/feedback_events.log. The launcher
    // does not link libpq, so Python ingests the log into Postgres.
    void        appendFeedbackEvent(const QVariantMap &fields);

    // Live art for every unowned row -- recommendations, wishlist, favourites,
    // played. None of them keep image files on disk: prepareLiveArtwork()
    // points a row's art slots at remote URLs (from the cache when it has the
    // game) and queues resolveLiveArtwork() for the rest, which looks up
    // SteamGridDB's URLs, falling back to Steam's CDN probed with HEAD.
    // applyLiveArtwork() caches the answer and writes it into every row of
    // that name. `urgent` puts a game at the front of the queue -- a visible
    // Discover pick, or the one a details page just opened on.
    void        prepareLiveArtwork(QVariantMap &row, bool urgent = false);
    void        resolveLiveArtwork(const QString &name, bool urgent = false);
    void        pumpLiveArtwork();
    void        finishLiveArtwork(const QString &name, const QVariantMap &art,
                                  bool definitive);
    void        applyLiveArtwork(const QString &name, const QVariantMap &art,
                                 bool definitive);
    QVariantMap savedRowFor(const QString &name) const;

    // live_artwork.json: the URLs each lookup found, keyed by lower-cased
    // name, so a game is asked about once per machine rather than once per
    // run of the recommender.
    fs::path    liveArtCachePath() const;
    void        loadLiveArtCache();
    void        saveLiveArtCache() const;
    bool        liveArtCacheIsFresh(const QString &name) const;
    QHash<QString, QVariantMap> m_liveArtCache;

    // Names (lower-cased) asked about this session, the ones still waiting,
    // and how many lookups are running. Capped: a played history can hold
    // dozens of unowned rows, and each lookup is four SteamGridDB requests.
    QSet<QString>           m_liveArtAsked;
    QStringList             m_liveArtQueue;
    int                     m_liveArtRunning = 0;
    // Queued or in flight right now -- unlike m_liveArtAsked, which also holds
    // lookups that ended without a definitive answer. A Discover page holds
    // IGDB's banner back only while its game is in here.
    QSet<QString>           m_liveArtPending;

    // The metadata counterpart of applyLiveArtwork(): writes one resolved IGDB
    // id's fields into every list holding that name, and emits for each.
    void        applyResolvedMetadata(const QString &name, long long igdbId);

    // Names this session has asked IGDB about. Entered before the request and
    // cleared only when one comes back with an id, so it covers both "already
    // in flight" and "IGDB has no answer for this" -- the second of which would
    // otherwise repeat the lookup on every page open. Session-lived on purpose:
    // igdb_resolve_game() persists what it learns, so a restart costs at most
    // one more request per unresolvable title.
    QSet<QString>           m_metadataAsked;

    class QNetworkAccessManager *m_network = nullptr;
    // Coalesces bursts of refresh requests. A member, not a function-local
    // static: the static was parented to `this` and would dangle if a second
    // bridge were ever constructed.
    class QTimer *m_debounce = nullptr;

    // ---- Browse ------------------------------------------------------------
    QVariantList            m_browseResults;
    bool                    m_browseSearching = false;
    QString                 m_browseStatus;
    QVariantMap             m_browseDetails;
    bool                    m_browseDetailsLoading = false;
    QVariantMap             m_browseReviews;
    bool                    m_browseReviewsLoading = false;
    QVariantList            m_browseNewReleases;
    bool                    m_browseNewReleasesLoading = false;
    QString                 m_browseNewReleasesStatus;
    // Started on each successful fetch; invalid until the first one.
    QElapsedTimer           m_browseNewReleasesClock;
    // Bumped per request; a reply carrying an older number is stale.
    int                     m_browseSearchSeq  = 0;
    int                     m_browseDetailsSeq = 0;
    // Discover-shaped rows for every details page opened this session, so the
    // wishlist, favourite and played paths have something to snapshot from.
    QVariantList            m_browseSnapshots;
    // Raw IGDB facts keyed by canonical name, for save_game_metadata() when a
    // browse game is marked played: the recommender needs its genres.
    QHash<QString, QVariantMap> m_browseMetadata;

    void        applyBrowseResults(int seq, const QByteArray &json, const QString &error);
    void        applyNewReleases(const QByteArray &json, const QString &error);
    void        applyBrowseDetails(int seq, const QByteArray &game, const QByteArray &ttb,
                                   const QString &error);
    void        fetchSteamReviews(int seq, int appId);
    void        queryBrowseDetails(int seq, qlonglong igdbId);
    void        finishUnresolvedBrowseDetails(int seq);

    // The best renderable row for an unowned game, from whichever list holds
    // one. Shared by the wishlist, favourite and played snapshot paths.
    QVariantMap snapshotFromLists(const QString &name) const;

    // NAME|IGDB_ID|ADDED_AT for every manual played row, for sync_local_data.py.
    void        writeManualPlayed() const;

    // Runs recommend.py immediately. Public callers go through
    // loadRecommendations(), which debounces -- every heart click used to spawn
    // its own Python interpreter.
    void        startRecommendationRun();
};

#endif // VORTEX_BRIDGE_H
