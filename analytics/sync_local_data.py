"""ETL: flat files written by the C++ launcher -> the analytics database.

Sources (all next to the executable / repo root):
    igdb_cache.txt        SEARCH_QUERY=IGDB_ID|CANONICAL_NAME
    game_metadata.txt     IGDB_ID=Developer|Rating|TimeToBeat|AllGenres|MainGenres
    playtime_sessions.log GAME_KEY | NAME | DURATION | START | END
    preferences.json      {"Game Name": 1}     (favourites only)

Two things here are load-bearing and easy to undo by accident:

1. `game_id` is derived from the *canonical name*, not from the game key.
   Session keys and cache keys disagree about IGDB ids for the same title --
   Forza Horizon 5 is igdb_144186 in the session log but 141503 in the cache --
   and keying on the raw key created two rows per game. The recommender then
   filtered played games by game_id and missed the twin, which is why a game
   with 45 hours on it was being recommended back to the user.

2. Unknown ratings are written as NULL, not 50.0. A hard-coded midpoint is a
   strong arbitrary prior; the scorer applies Bayesian shrinkage toward the
   catalog mean instead, which is the correct uninformative default.
"""

import json
import os
import uuid
from datetime import datetime, timedelta

from db import encode_list, get_connection
from interest import (ABORT_BELOW_SECONDS, ENGAGED_FROM_SECONDS,
                      MIN_ABORTS_FOR_DISINTEREST)
from model import repair_mojibake

BASE_DIR = os.path.dirname(os.path.dirname(__file__))

# Namespace-stable ids so re-running this script is idempotent.
_NS = uuid.NAMESPACE_OID


def get_file_path(filename):
    return os.path.join(BASE_DIR, filename)


def make_canonical(name):
    """Lowercase alphanumerics only.

    Ported verbatim from make_canonical() in source/game_manager.cpp:33 so the
    C++ and Python sides agree on when two titles are the same game.
    """
    return "".join(ch.lower() for ch in name if ch.isalnum())


def game_uuid(name):
    return str(uuid.uuid5(_NS, "canon_" + make_canonical(name)))


def session_uuid(game_key, start, end, duration):
    """Deterministic, so re-syncing does not duplicate or churn session rows."""
    return str(uuid.uuid5(_NS, f"{game_key}|{start}|{end}|{duration}"))


# --- Steam playtime -> derived sessions -------------------------------------
#
# Vortex only records sessions it launched itself, so a library played through
# Steam produced no history at all and the recommender had nothing to learn
# from. Steam knows the lifetime total and when the game was last played; both
# are needed, because interest decays with recency and a lifetime figure alone
# cannot be weighted.
#
# These rows are INFERRED, not observed. They are marked synthetic=1 in the
# database and are never written to playtime_sessions.log, which is a record of
# sessions Vortex actually watched -- mixing invented rows into it would
# corrupt the one honest source. Anything measuring real behaviour must filter
# them out; evaluate.py does.

# Tuned against the bands in interest.py: comfortably above
# ENGAGED_FROM_SECONDS (600) so each derived session counts as real play, and
# well under DURATION_CAP_HOURS (10) so none of them wastes the duration
# weight by saturating it.
SYNTHETIC_SESSION_SECONDS = 2 * 60 * 60

# A 2,000-hour game would otherwise generate a thousand rows and drown every
# other signal in the profile. The frequency term saturates long before this.
MAX_SYNTHETIC_SESSIONS = 40

# Spacing between derived sessions, working backwards from the last-played
# date. One week means RECENCY_HALF_LIFE_DAYS (30) decays the tail, so a game
# abandoned three years ago does not read as current play.
SYNTHETIC_SESSION_SPACING = timedelta(days=7)


def synthesize_steam_sessions(game_id, game_key, total_seconds, last_played_epoch):
    """Split a Steam lifetime total into plausible sessions.

    Durations sum back to the original total exactly: the remainder that does
    not fill a whole session is folded into the oldest one rather than dropped,
    so the profile reflects the hours actually played.

    Returns [] when there is nothing to derive -- no playtime, or no
    last-played date to anchor recency to. Inventing a date would be worse than
    skipping the game: it would place the play at "now" and outrank real
    recent sessions.
    """
    if total_seconds <= 0 or last_played_epoch <= 0:
        return []

    count = int(round(total_seconds / SYNTHETIC_SESSION_SECONDS))
    count = max(1, min(count, MAX_SYNTHETIC_SESSIONS))

    per_session = total_seconds // count
    remainder = total_seconds - per_session * count

    try:
        anchor = datetime.fromtimestamp(last_played_epoch)
    except (OverflowError, OSError, ValueError):
        return []

    out = []
    for index in range(count):
        # index 0 is the most recent and lands exactly on Steam's last-played
        # date; the rest step backwards.
        end = anchor - SYNTHETIC_SESSION_SPACING * index
        duration = per_session + (remainder if index == count - 1 else 0)
        if duration <= 0:
            continue
        start = end - timedelta(seconds=duration)

        out.append({
            "session_id": session_uuid(f"steam-derived|{game_key}", start, end, duration),
            "game_id": game_id,
            "start": start,
            "end": end,
            "duration": duration,
            "synthetic": True,
        })
    return out

def parse_igdb_cache():
    cache = {}
    path = get_file_path("igdb_cache.txt")
    if not os.path.exists(path):
        return cache
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("=", 1)
            if len(parts) == 2:
                cache[repair_mojibake(parts[0])] = parts[1].split("|", 1)[0]
    return cache


def _split_labels(raw):
    return [repair_mojibake(g.strip()) for g in raw.split(",") if g.strip()]


def parse_metadata():
    meta = {}
    path = get_file_path("game_metadata.txt")
    if not os.path.exists(path):
        return meta
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("=", 1)
            if len(parts) != 2:
                continue
            igdb_id = parts[0]
            vals = parts[1].split("|")
            # Developer|Rating|TimeToBeat|AllGenres|MainGenres[|Themes[|GameModes]]
            # Themes are a later addition, so five-field lines written by older
            # builds stay valid and simply carry no themes.
            if len(vals) >= 5:
                try:
                    rating = float(vals[1]) if vals[1] else None
                except ValueError:
                    rating = None
                try:
                    ttb = float(vals[2]) if vals[2] else 0.0
                except ValueError:
                    ttb = 0.0
                meta[igdb_id] = {
                    "developer": repair_mojibake(vals[0]) or "Unknown",
                    "rating": rating,
                    "game_length": ttb,
                    "tags": _split_labels(vals[3]),
                    "genres": _split_labels(vals[4]),
                    "themes": _split_labels(vals[5]) if len(vals) >= 6 else [],
                    "game_modes": _split_labels(vals[6]) if len(vals) >= 7 else [],
                    "external_id": igdb_id,
                }
    return meta


def _int_or_zero(text):
    try:
        return int(str(text).strip())
    except (TypeError, ValueError):
        return 0


def parse_installed_games():
    """canonical_name -> (display_name, source), as the launcher sees them.

    Written by VortexBridge::loadGames() after every successful scan. It is the
    only authoritative answer to two questions the analytics side cannot
    otherwise get right:

    1. What is actually installed. igdb_cache.txt is append-only and is never
       pruned on uninstall, so games the user removed keep looking owned and
       kept taking the top slots in the "from your library" section.

    2. What each game is CALLED. The launcher shows the IGDB canonical name for
       local games (game_manager.cpp sets g.name = info.name) but the Steam
       manifest name for Steam games, whereas this script only ever saw the
       cache *key* -- the folder name that was searched. So a game could be
       installed and still fail to match: the folder "Neverness To Everness"
       resolves to "Neverness to Everness: Dreamwalk Corridor", and no amount
       of canonicalisation bridges that.

    Returns (by_canonical, by_igdb_id), or (None, None) when the file is
    absent, so callers can tell "nothing installed" apart from "we don't know".

    by_igdb_id is what actually closes the gap described above: the launcher
    writes NAME|SOURCE|IGDB_ID and igdb_cache.txt stores the same id against
    the folder name, so the id joins the two spellings exactly. It was being
    parsed and dropped on the floor, leaving the canonical name -- a heuristic
    that fails precisely when IGDB renamed the game -- as the only key.
    """
    path = get_file_path("installed_games.txt")
    if not os.path.exists(path):
        return None, None

    by_canonical = {}
    by_igdb_id = {}
    steam_playtime = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("|")
            if not parts[0]:
                continue
            name = repair_mojibake(parts[0])
            source = parts[1] if len(parts) > 1 and parts[1] else "Local"
            by_canonical[make_canonical(name)] = (name, source)

            # Steam's lifetime total and last-played date, appended to the
            # format after IGDB_ID. Older files simply lack them, which is why
            # every field is read defensively rather than by unpacking.
            playtime = _int_or_zero(parts[3]) if len(parts) > 3 else 0
            last_played = _int_or_zero(parts[4]) if len(parts) > 4 else 0

            # id 0 means "IGDB never resolved this one", which is not a key --
            # every unresolved game carries it, so indexing on it would collide
            # them all into one entry.
            if len(parts) > 2:
                igdb_id = _int_or_zero(parts[2])
                if igdb_id > 0:
                    by_igdb_id[str(igdb_id)] = (name, source)

            if playtime > 0:
                steam_playtime[make_canonical(name)] = (playtime, last_played)

    return by_canonical, by_igdb_id, steam_playtime


def parse_preferences():
    """Favourites only. Values <= 0 are ignored so preference files written by
    older builds (which had a dislike button) load without a migration."""
    path = get_file_path("preferences.json")
    if not os.path.exists(path):
        return {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            raw = json.load(f)
    except (OSError, ValueError):
        return {}
    return {k: v for k, v in raw.items() if isinstance(v, (int, float)) and v > 0}


# Games with no IGDB match get no metadata rather than invented metadata.
_EMPTY_META = {
    "developer": "Unknown",
    "rating": None,
    "game_length": 0.0,
    "genres": [],
    "tags": [],
    "themes": [],
    "game_modes": [],
    "external_id": None,
}


def collect_games_and_sessions():
    """Build the rows to upsert. Pure -- no DB access, so it can be tested
    offline (see verify_phase1.py)."""
    igdb_cache = parse_igdb_cache()
    metadata_cache = parse_metadata()

    games = {}

    def upsert_game(name, igdb_id):
        gid = game_uuid(name)
        meta = metadata_cache.get(igdb_id, _EMPTY_META)
        existing = games.get(gid)
        # Two keys can map to one canonical game; keep the richer record.
        if existing and existing["rating"] is not None and meta["rating"] is None:
            return gid
        games[gid] = {
            "name": name,
            "developer": meta["developer"],
            "genres": meta["genres"],
            "tags": meta["tags"],
            "rating": meta["rating"],
            "game_length": meta["game_length"],
            "themes": meta.get("themes") or [],
            "game_modes": meta.get("game_modes") or [],
            # Needed so igdb_catalog.py can backfill themes by IGDB id for any
            # owned game the discovery catalog does not cover.
            "external_id": meta.get("external_id") or (igdb_id if igdb_id != "0" else None),
        }
        return gid

    for game_name, igdb_id in igdb_cache.items():
        upsert_game(game_name, igdb_id)

    sessions = []
    path = get_file_path("playtime_sessions.log")
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = [p.strip() for p in line.split("|")]
                if len(parts) < 5:
                    continue

                game_key, game_name, duration_raw = parts[0], repair_mojibake(parts[1]), parts[2]
                try:
                    duration = int(duration_raw)
                except ValueError:
                    continue

                # Field 5 is idle seconds, appended after the dates so the four
                # fields above keep their indices. Rows written before idle was
                # measured simply do not have it, and stay unattributed rather
                # than being credited as fully active -- nobody looked.
                idle = None
                if len(parts) >= 6:
                    try:
                        idle = max(0, min(int(parts[5]), duration))
                    except ValueError:
                        idle = None

                try:
                    # "2026-05-01 Mon 10:00:00" -> "2026-05-01 10:00:00"
                    start_dt = datetime.strptime(
                        f"{parts[3].split()[0]} {parts[3].split()[2]}", "%Y-%m-%d %H:%M:%S")
                    end_dt = datetime.strptime(
                        f"{parts[4].split()[0]} {parts[4].split()[2]}", "%Y-%m-%d %H:%M:%S")
                except (ValueError, IndexError):
                    continue

                igdb_id = game_key[5:] if game_key.startswith("igdb_") else igdb_cache.get(game_name, "0")
                gid = upsert_game(game_name, igdb_id)

                sessions.append({
                    "session_id": session_uuid(game_key, start_dt, end_dt, duration),
                    "game_id": gid,
                    "duration": duration,
                    "idle": idle,
                    "start": start_dt,
                    "end": end_dt,
                })

    return games, sessions


def is_material(cur, user_id, added_sessions):
    """Could these newly inserted sessions actually move the ranking?

    Derived from the session bands rather than a fresh threshold, so there is
    only one definition of what counts as playing a game:

      * engaged (>= ENGAGED_FROM_SECONDS) enters the taste profile
      * an abort reaching MIN_ABORTS_FOR_DISINTEREST with no engaged session
        enters the disinterest set
      * neutral (120-600s) is excluded from BOTH profiles by construction, so
        it provably cannot change the output

    The launcher uses this to avoid re-running the recommender — and therefore
    reshuffling the list — after a session that cannot have changed anything.
    """
    if not added_sessions:
        return False

    for s in added_sessions:
        if s["duration"] >= ENGAGED_FROM_SECONDS:
            return True

    # Only aborts left. One is never enough, so ask the database whether this
    # game has now accumulated enough of them with no real play in between.
    for s in {x["game_id"] for x in added_sessions}:
        cur.execute("""
            SELECT count(*) FILTER (WHERE duration_seconds < ?),
                   count(*) FILTER (WHERE duration_seconds >= ?)
            FROM sessions WHERE user_id = ? AND game_id = ?
        """, (ABORT_BELOW_SECONDS, ENGAGED_FROM_SECONDS, user_id, s))
        aborts, engaged = cur.fetchone()
        if engaged == 0 and aborts >= MIN_ABORTS_FOR_DISINTEREST:
            return True

    return False


def sync_data():
    games, sessions = collect_games_and_sessions()

    conn = get_connection()
    cur = conn.cursor()

    user_id = str(uuid.uuid5(_NS, "local_user"))
    cur.execute(
        "INSERT INTO users (user_id, username) VALUES (?, ?) "
        "ON CONFLICT (username) DO NOTHING",
        (user_id, "local_user"))
    cur.execute("SELECT user_id FROM users WHERE username=?", ("local_user",))
    user_id = cur.fetchone()[0]

    installed, installed_by_id, steam_playtime = parse_installed_games()
    if installed is None:
        print("[warn] installed_games.txt not found; treating every known game "
              "as installed. Launch the GUI once to generate it.")

    # Canonical names of games matched through their IGDB id, so the backfill
    # loop below does not insert a second row for one that is already handled.
    matched_canonicals = set()
    # Every canonical_name this run marks installed=1, so the sweep below can
    # clear the rest. Not the same set as `installed`: a game matched through
    # its IGDB id is stored under the cache's spelling of the name, not the
    # launcher's.
    installed_canonicals = set()
    id_matches = []

    for gid, g in games.items():
        canonical = make_canonical(g["name"])
        external_id = str(g.get("external_id") or "")

        # The launcher's copy wins on both name and source. Without this the DB
        # keeps its own spelling and the UI cannot match the row back to a real
        # game, so an installed title renders as "NOT IN LIBRARY".
        #
        # The IGDB id is tried first because it is exact. The canonical name is
        # the fallback, and it is wrong in one specific, common case: IGDB
        # renames the game, the launcher displays and records the new name, and
        # this side only ever saw the folder name it searched with. "Neverness
        # To Everness" against "Neverness to Everness: Dreamwalk Corridor" is
        # the worked example -- it used to sync as installed=0 AND spawn a
        # duplicate metadata-free row, which put a game the user owns into the
        # Discover section labelled "NOT IN LIBRARY".
        if installed is None:
            name, source, is_installed = g["name"], "Local", True
        elif external_id and external_id in installed_by_id:
            name, source = installed_by_id[external_id]
            is_installed = True
            matched_canonicals.add(make_canonical(name))
            if make_canonical(name) != canonical:
                id_matches.append((g["name"], name))
        elif canonical in installed:
            name, source = installed[canonical]
            is_installed = True
            matched_canonicals.add(canonical)
        else:
            name, source, is_installed = g["name"], "Local", False

        # Owned rows win over any catalog row for the same canonical name: the
        # user's own library is the better source of truth, and a game must
        # never appear as both owned and a discovery candidate.
        cur.execute("""
            INSERT INTO games (game_id, source, name, canonical_name, developer,
                               genres, tags, rating, game_length, installed,
                               themes, game_modes, external_id)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT (canonical_name) DO UPDATE SET
                game_id     = EXCLUDED.game_id,
                source      = EXCLUDED.source,
                name        = EXCLUDED.name,
                developer   = EXCLUDED.developer,
                genres      = EXCLUDED.genres,
                tags        = EXCLUDED.tags,
                rating      = EXCLUDED.rating,
                game_length = EXCLUDED.game_length,
                installed   = EXCLUDED.installed,
                external_id = COALESCE(EXCLUDED.external_id, games.external_id),
                -- Keep whatever themes we already have if this sync has none:
                -- the C++ only writes them for newly resolved games, so an
                -- older metadata line must not wipe a backfilled set.
                themes      = CASE WHEN json_array_length(EXCLUDED.themes) > 0
                                   THEN EXCLUDED.themes ELSE games.themes END,
                game_modes  = CASE WHEN json_array_length(EXCLUDED.game_modes) > 0
                                   THEN EXCLUDED.game_modes ELSE games.game_modes END
        """, (gid, source, name, canonical, g["developer"],
              encode_list(g["genres"]), encode_list(g["tags"]),
              g["rating"], g["game_length"], is_installed,
              encode_list(g["themes"]), encode_list(g["game_modes"]),
              g["external_id"]))

        if is_installed:
            installed_canonicals.add(canonical)

    # A game the launcher reports as installed but that never appeared in
    # igdb_cache.txt (metadata lookup failed, or it was added since) would
    # otherwise be silently missing from the library section entirely.
    if installed:
        for canonical, (name, source) in installed.items():
            # Already handled above, under the cache's own canonical name and
            # with its metadata attached. Inserting again here would key a
            # SECOND row on the resolved name -- same game, no genres, and
            # invisible to the recommender because a zero vector is filtered
            # out of the candidate pool. That duplicate is what this skip
            # exists to prevent.
            if canonical in matched_canonicals:
                continue

            cur.execute("""
                INSERT INTO games (game_id, source, name, canonical_name, installed)
                VALUES (?, ?, ?, ?, 1)
                ON CONFLICT (canonical_name) DO UPDATE SET
                    installed = 1,
                    name      = EXCLUDED.name,
                    source    = EXCLUDED.source
            """, (game_uuid(name), source, name, canonical))
            installed_canonicals.add(canonical)

    # installed_games.txt is the whole truth about what is on the machine, so
    # anything still flagged installed that this run did not see has been
    # uninstalled or deleted. Without this sweep such a row keeps its flag
    # forever and keeps taking slots in the "from your library" section: the
    # loop above only ever visits games igdb_cache.txt still knows about, and
    # a game that has dropped out of both files is visited by neither.
    if installed_canonicals:
        placeholders = ",".join("?" * len(installed_canonicals))
        cur.execute(f"""
            UPDATE games SET installed = 0
             WHERE installed = 1
               AND canonical_name NOT IN ({placeholders})
        """, tuple(installed_canonicals))
        if cur.rowcount:
            print(f"  cleared installed flag on {cur.rowcount} game(s) that are "
                  f"no longer on disk")

    if id_matches:
        # Worth naming: this is the case that silently failed before, and
        # seeing it in the log is how a future mismatch gets noticed.
        for cache_name, resolved in id_matches:
            print(f"  matched by IGDB id: {cache_name!r} -> {resolved!r}")

    # Remove duplicates left behind by earlier syncs, which created a second
    # metadata-free row whenever IGDB had renamed a game. Only rows with no
    # labels at all are touched, and only when the same game exists elsewhere
    # with real metadata, so nothing carrying information can be deleted.
    #
    # The history attached to a duplicate is re-pointed at the surviving row
    # rather than deleted with it. That is not tidiness: sessions,
    # recommendations_cache and recommendation_events all carry a foreign key
    # into games, so deleting a referenced row raised "FOREIGN KEY constraint
    # failed" and took the WHOLE sync down with it -- every `installed` flag
    # this function had just written rolled back with the transaction. The
    # visible symptom was games the user had uninstalled still holding slots in
    # the "from your library" section, because the flag that would have
    # retired them never reached disk. One stray impression row was enough.
    if installed_by_id:
        duplicates = cur.execute("""
            SELECT dup.game_id,
                   (SELECT keep.game_id FROM games keep
                     WHERE keep.canonical_name <> dup.canonical_name
                       AND keep.name = dup.name
                       AND json_array_length(COALESCE(keep.genres, '[]')) > 0
                     ORDER BY keep.game_id LIMIT 1)
              FROM games AS dup
             WHERE json_array_length(COALESCE(dup.genres, '[]')) = 0
               AND json_array_length(COALESCE(dup.themes, '[]')) = 0
               AND EXISTS (
                   SELECT 1 FROM games keep
                    WHERE keep.canonical_name <> dup.canonical_name
                      AND keep.name = dup.name
                      AND json_array_length(COALESCE(keep.genres, '[]')) > 0
                 )
        """).fetchall()

        removed = 0
        for dup_id, keep_id in duplicates:
            if not keep_id:
                continue
            cur.execute("UPDATE sessions SET game_id = ? WHERE game_id = ?",
                        (keep_id, dup_id))
            cur.execute("UPDATE recommendations_cache SET recommended_game = ? "
                        "WHERE recommended_game = ?", (keep_id, dup_id))
            cur.execute("UPDATE recommendation_events SET game_id = ? "
                        "WHERE game_id = ?", (keep_id, dup_id))
            cur.execute("DELETE FROM games WHERE game_id = ?", (dup_id,))
            removed += 1
        if removed:
            print(f"  removed {removed} duplicate row(s) left by earlier syncs")

    # Games Vortex actually watched. Observed play always wins: once there is
    # real history for a game, derived rows must not compete with it.
    observed_game_ids = {s["game_id"] for s in sessions}

    derived = []
    for canonical, (total_seconds, last_played) in steam_playtime.items():
        cur.execute("SELECT game_id FROM games WHERE canonical_name = ?", (canonical,))
        row = cur.fetchone()
        if not row:
            continue
        game_id = row[0]
        if game_id in observed_game_ids:
            continue
        derived.extend(synthesize_steam_sessions(
            game_id, canonical, total_seconds, last_played))

    # Deterministic session ids let us upsert instead of deleting every session
    # for the user and reinserting on each game exit.
    added = []
    for s in sessions + derived:
        # active_seconds / idle_seconds / activity_ratio have been in the schema
        # all along and were never written; the launcher measures idle now, so
        # they carry a real figure. They stay NULL for anything with no idle
        # reading -- synthetic sessions, and rows logged before the measurement
        # existed -- which is why a reader wanting a number for every row has to
        # COALESCE onto duration_seconds rather than assume these are populated.
        idle = s.get("idle")
        active = None if idle is None else s["duration"] - idle
        ratio = None
        if active is not None and s["duration"] > 0:
            ratio = active / s["duration"]

        cur.execute("""
            INSERT INTO sessions
                (session_id, user_id, game_id, started_at, ended_at,
                 duration_seconds, active_seconds, idle_seconds, activity_ratio,
                 synthetic)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT (session_id) DO NOTHING
        """, (s["session_id"], user_id, s["game_id"], s["start"], s["end"],
              s["duration"], active, idle, ratio,
              1 if s.get("synthetic") else 0))
        if cur.rowcount:
            added.append(s)

    if derived:
        print(f"  derived {len(derived)} sessions from Steam playtime "
              f"for {len({d['game_id'] for d in derived})} games")

    material = is_material(cur, user_id, added)

    events = ingest_feedback_events(cur, user_id)

    conn.commit()
    cur.close()
    conn.close()

    print(f"Synced {len(games)} games, {len(sessions)} sessions, {events} feedback events.")
    # Read by the launcher to decide whether re-running the recommender could
    # possibly change anything. Keep this the last line and the format stable.
    print(f"MATERIAL={1 if material else 0}")


def ingest_feedback_events(cur, user_id):
    """Fold the launcher's NDJSON feedback log into recommendation_events.

    The launcher links only Qt and winhttp -- no libpq -- so it appends events
    to a flat log and this is where they reach the database. The log is truncated
    once its contents are committed.
    """
    path = get_file_path(os.path.join("analytics", "feedback_events.log"))
    if not os.path.exists(path):
        return 0

    inserted = 0
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except ValueError:
                continue

            name = event.get("name")
            event_type = event.get("event_type")
            if not name or event_type not in ("impression", "click", "launch"):
                continue

            cur.execute("SELECT game_id FROM games WHERE canonical_name=?",
                        (make_canonical(name),))
            row = cur.fetchone()
            if not row:
                continue

            cur.execute("""
                INSERT INTO recommendation_events
                    (event_id, run_id, user_id, game_id, event_type, origin)
                VALUES (?, ?, ?, ?, ?, ?)
            """, (str(uuid.uuid4()), event.get("run_id"), user_id, row[0],
                  event_type, event.get("origin")))
            inserted += 1

    # Truncate rather than delete: the launcher may hold the file open.
    if inserted:
        open(path, "w", encoding="utf-8").close()
    return inserted


if __name__ == "__main__":
    sync_data()
