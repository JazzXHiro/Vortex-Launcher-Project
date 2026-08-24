-- Vortex analytics schema v3 (SQLite).
--
-- Rebuild with:  python reset_db.py --yes
--
-- `games` holds two kinds of row, distinguished by `source`:
--   'Local' / 'Steam'  games the user owns, synced from the launcher's caches
--   'IGDB_Catalog'     discovery candidates fetched by igdb_catalog.py
--
-- `canonical_name` is lowercase-alphanumeric-only and UNIQUE. It is what stops
-- one game becoming two rows: session logs and igdb_cache.txt disagreed about
-- IGDB ids for the same title, so keying on the id meant a heavily played game
-- could still be recommended back to the user.
--
-- v3 moved this schema from PostgreSQL to SQLite so the launcher can ship as a
-- self-contained install with no database server to set up. Two type changes
-- carry the whole difference:
--
--   * UUID    -> TEXT. Every id was already generated in Python as str(uuid...)
--                and read back through a defensive str(), so nothing outside
--                this file had to change.
--
--   * TEXT[]  -> TEXT holding a JSON array. SQLite has no array type. db.py
--                encodes on write and decodes on read, so callers still see
--                real Python lists exactly as psycopg2 delivered them, and
--                model.py's _labels() is untouched.
--
-- Timestamps are ISO-8601 text. pandas parses them via pd.to_datetime, which
-- recommend.py already called with errors="coerce".

CREATE TABLE users (
    user_id TEXT PRIMARY KEY,
    username TEXT UNIQUE NOT NULL
);

CREATE TABLE games (
    game_id TEXT PRIMARY KEY,
    source TEXT NOT NULL,
    external_id TEXT,
    name TEXT NOT NULL,
    canonical_name TEXT,
    executable_path TEXT,
    appid INTEGER,
    steam_appid INTEGER,
    developer TEXT,
    -- The five JSON-array columns. Always written through db.encode_list() and
    -- read back as lists by db.decode_row(); never bind a bare Python list.
    genres TEXT,
    tags TEXT,
    themes TEXT,
    -- How the game is played. Six values in IGDB: Single player,
    -- Multiplayer, Co-operative, Split screen, MMO, Battle Royale.
    game_modes TEXT,
    -- IGDB keywords: the fine-grained descriptors genres cannot express --
    -- 'soulslike', 'survival horror', 'squad tactics', 'hand-drawn'. This is
    -- what separates games whose genre/theme sets are identical; before it,
    -- Pillars of Eternity II and Final Fantasy XIII scored exactly 1.00
    -- similarity because the model had no way to tell them apart.
    --
    -- Stored already filtered: igdb_catalog.py strips storefront noise and
    -- keeps only the 1.5%-15% frequency band, so this is ~450 distinct values,
    -- not IGDB's full 7,418.
    keywords TEXT,
    -- Whether the game is on the machine RIGHT NOW, per installed_games.txt
    -- which the launcher rewrites after every scan. `source` cannot answer
    -- this: games arrive from igdb_cache.txt, which is append-only and is
    -- never pruned when a game is uninstalled, so a title the user removed
    -- years ago still looks 'Local' forever.
    installed INTEGER NOT NULL DEFAULT 0,
    game_length REAL,
    rating REAL,
    review_count INTEGER,
    total_rating_count INTEGER,
    released_at TEXT,
    cover_url TEXT,
    fetched_at TEXT,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP
);

CREATE UNIQUE INDEX games_canonical_idx ON games(canonical_name);
CREATE INDEX games_source_idx ON games(source);

CREATE TABLE sessions (
    session_id TEXT PRIMARY KEY,
    user_id TEXT REFERENCES users(user_id),
    game_id TEXT REFERENCES games(game_id),

    started_at TEXT,
    ended_at TEXT,

    duration_seconds INTEGER,
    active_seconds INTEGER,
    idle_seconds INTEGER,

    activity_ratio REAL,

    -- 1 when this row was DERIVED rather than observed.
    --
    -- Vortex only ever watches sessions it launched itself, so a library
    -- played through Steam produced no history at all. Steam's lifetime total
    -- and last-played date are split into plausible sessions by
    -- sync_local_data.py so that history can shape recommendations -- but they
    -- are inferred, not recorded, and anything measuring real behaviour has to
    -- be able to exclude them. evaluate.py in particular must, or it would be
    -- scoring the recommender against invented data.
    synthetic INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX sessions_user_idx ON sessions(user_id);

-- What was actually served, so a recommendation can be evaluated after the
-- fact instead of only inspected live.
CREATE TABLE recommendations_cache (
    id TEXT PRIMARY KEY,
    user_id TEXT REFERENCES users(user_id),
    recommended_game TEXT REFERENCES games(game_id),
    score REAL,
    rank INTEGER,
    mood INTEGER,
    run_id TEXT,
    model_version TEXT,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP
);

-- Impressions, clicks and launches attributed to the surface that produced
-- them, so click-through can be measured rather than assumed.
CREATE TABLE recommendation_events (
    event_id TEXT PRIMARY KEY,
    run_id TEXT,
    user_id TEXT REFERENCES users(user_id),
    game_id TEXT REFERENCES games(game_id),
    event_type TEXT CHECK (event_type IN ('impression', 'click', 'launch')),
    origin TEXT,
    created_at TEXT DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX recommendation_events_type_idx ON recommendation_events(event_type);
