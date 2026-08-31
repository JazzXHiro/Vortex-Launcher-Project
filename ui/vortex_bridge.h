#ifndef VORTEX_BRIDGE_H
#define VORTEX_BRIDGE_H

#include <QObject>
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
    // gameListChanged is sufficient: every path that alters a favourite goes
    // through updatePreference() or resetPreferences(), both of which call
    // refreshGameList().
    Q_PROPERTY(QVariantList favoriteGames READ favoriteGames NOTIFY gameListChanged)
    Q_PROPERTY(QVariantList wishlistGames READ wishlistGames NOTIFY wishlistChanged)
    // Everything ever played, whether or not it is still on the machine. Backed
    // by a ledger rather than by gameList, because the whole point is the games
    // that are no longer there -- see loadPlayedLedger().
    Q_PROPERTY(QVariantList playedGames READ playedGames NOTIFY playedGamesChanged)
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

    // One sentence naming what is actually broken, or empty when nothing is.
    // Rendered on the Recommendations header because that line is always
    // visible, unlike the sections beneath it.
    Q_PROPERTY(QString authBlocker READ authBlocker NOTIFY credentialsChanged)

public:
    explicit VortexBridge(QObject *parent = nullptr);

    QVariantList gameList()    const { return m_gameList;    }
    QVariantList recommendationList() const { return m_recommendationList; }
    QVariantList favoriteGames() const;
    QVariantList wishlistGames() const { return m_wishlist; }
    QVariantList playedGames() const;
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
    Q_INVOKABLE void   logRecommendationClick(QString name, int rank);
    Q_INVOKABLE void   uninstallGame(QString name);
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

    // Hero and logo art for one unowned pick, fetched when its details page
    // opens rather than alongside the list. Covers are drawn on every Discover
    // card and so are fetched for the whole list; hero and logo are only ever
    // visible inside GameDetails, so doing them at list time would download two
    // images per candidate to show at most one pair. No-op for owned games,
    // which already have SteamGridDB art under Images/.
    Q_INVOKABLE void   ensureArtwork(QString name);

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
    void wishlistChanged();
    void playedGamesChanged();
    void gameListChanged();
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
    void catalogRefreshingChanged();
    void catalogProgressChanged();
    // ok=false carries the script's stderr in `details`; the wizard shows it
    // rather than a generic failure, because the usual cause is a mistyped
    // client secret and the message says so.
    void catalogRefreshFinished(bool ok, QString details);

private:
    QVariantList            m_gameList;
    QVariantList            m_recommendationList;
    QVariantList            m_wishlist;
    QVariantList            m_favoriteSnapshots;
    // Persisted play history. Keyed by the same playtime key stats_manager uses,
    // and only ever added to, so an uninstalled game keeps its row.
    QVariantList            m_playedLedger;
    std::vector<BridgeGame> m_internalGames;
    bool                    m_isLoading   = false;
    bool                    m_rescanQueued = false;  // scan requested while one was running
    bool                    m_isRecommendationLoading = false;
    bool                    m_catalogRefreshing = false;
    bool                    m_catalogAutoFetchTried = false;
    QString                 m_catalogPhase;
    int                     m_catalogFetched = 0;
    // How many unowned candidates the last run had to choose from. Zero means
    // the catalog was never fetched, which is what maybeAutoFetchCatalog() acts
    // on.
    int                     m_discoverCandidateCount = 0;
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

    // Downloads cover art for unowned picks after the list is already visible,
    // fetching only cache misses.
    void        fetchCandidateCovers();

    // Backs ensureArtwork(). fetchArtworkFrom() walks one kind's URL fallback
    // chain, one entry per call, re-entering itself from the reply handler;
    // allDefinitive carries whether every attempt so far failed for a reason
    // upstream actually gave (a 404), so a run of connection errors never ends
    // up stamping the negative cache.
    void        fetchArtworkFrom(const QString &name, const QString &kind,
                                 const QStringList &urls, int index,
                                 bool allDefinitive);
    void        recordArtworkMiss(const QString &name, const QString &kind);

    // Points heroPath/logoPath at whatever is now on disk for one game, in
    // every list that carries it, and emits so the open details page re-reads.
    void        rebindArtwork(const QString &name);

    // "<stem>|<kind>" of each download in flight, so reopening a details page
    // mid-download does not start a second request for the same image.
    QSet<QString>           m_artworkInFlight;
    class QNetworkAccessManager *m_network = nullptr;
    // Coalesces bursts of refresh requests. A member, not a function-local
    // static: the static was parented to `this` and would dangle if a second
    // bridge were ever constructed.
    class QTimer *m_debounce = nullptr;

    // Runs recommend.py immediately. Public callers go through
    // loadRecommendations(), which debounces -- every heart click used to spawn
    // its own Python interpreter.
    void        startRecommendationRun();
};

#endif // VORTEX_BRIDGE_H
