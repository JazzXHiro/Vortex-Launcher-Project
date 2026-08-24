"""Fetch a catalog of candidate games from IGDB for discovery.

    python igdb_catalog.py --refresh
    python igdb_catalog.py --refresh --limit 1000   # smaller run, for testing

Without this, `games` only ever contains titles the user already owns, so the
Recommendations tab can never suggest anything new -- it can only reorder the
library. These rows are stored with source='IGDB_Catalog'.

Deliberately NOT part of the launcher's critical path. `recommend.py` runs
inside a bounded QProcess; a network call there is a hang risk. Run this by
hand (or on a schedule); recommend.py prints a warning when the catalog goes
stale.

This is a separate implementation from the C++ IGDB client on purpose. That one
resolves a single title at a time, has no throttling of any kind, rewrites its
whole metadata file per insert, and hand-parses JSON in a way that mangles
\\uXXXX escapes (which is why the committed caches contain "u0027" where an
apostrophe belongs). None of that survives bulk fetching.
"""

import argparse
import json
import os
import sys
import time
import uuid
from datetime import datetime, date

import requests

from config import IGDB_CLIENT_ID, IGDB_CLIENT_SECRET
from db import encode_list, get_connection
from sync_local_data import make_canonical

BASE_DIR = os.path.dirname(__file__)
TOKEN_PATH = os.path.join(BASE_DIR, ".igdb_token.json")

TOKEN_URL = "https://id.twitch.tv/oauth2/token"
GAMES_URL = "https://api.igdb.com/v4/games"
TTB_URL = "https://api.igdb.com/v4/game_time_to_beats"

# IGDB allows 4 requests/second. 0.30s leaves ~20% headroom; the C++ client has
# no throttling at all and only gets away with it because it never bulk-fetches.
MIN_REQUEST_INTERVAL = 0.30
PAGE_SIZE = 500          # IGDB's maximum
MAX_RETRIES = 3

# IGDB platform 6 is PC (Microsoft Windows). Without this the catalog happily
# fetched Wii, 3DS, GBA and PSP exclusives, which then competed for Discover
# slots on a Windows-only launcher -- that is how Pokemon Rumble Blast and
# Mario Tennis: Power Tour ended up in the recommendations. A third of the pool
# was unplayable: of 5,842 games at the old threshold, only 3,932 ran on PC.
#
# Mac and Linux ids are deliberately absent: the launcher itself is Windows-only
# (WinHTTP, XInput, app_paths), so they would only re-add games that cannot run.
PC_PLATFORM_ID = 6

# Below this many ratings a title is too obscure to be a useful suggestion.
# Also keeps the Bayesian shrinkage prior meaningful.
#
# Measured against IGDB (main games, no version parents), PC-only:
#     >= 20   3,932      >= 10   6,333      >= 5   9,386      >= 3   11,014
# (303,713 main games exist in total, so nothing here is a structural limit.)
#
# 10 buys 61% more genuinely playable candidates than the old 20 did, while
# still excluding titles with almost no ratings. The risk is smaller than it
# looks: SHRINKAGE_M = 30 in scoring.py pulls a game with ten ratings most of
# the way to the catalog mean, so thinly-rated games cannot win on quality --
# they surface only on real genre similarity. If Discover ever starts feeling
# like shovelware, this is the one dial to turn.
MIN_RATING_COUNT = 10

# --- Keywords ---------------------------------------------------------------
# IGDB's keyword vocabulary is 7,418 entries and averages 29 per game, against
# the ~10 genre/theme/mode labels a game carries. Stored raw they would be ~3x
# the feature document and would simply overwrite the existing signal, so two
# filters run before anything is stored.
#
# 1. The blocklist. Measured on 2,000 PC games, roughly half of the most common
#    keywords are storefront and platform metadata, not anything about the game:
#    'digital distribution', 'steam', 'achievements', 'bink video', 'playstation
#    trophies', 'xbox controller support for pc', 'auto-saving', 'wasd movement'.
#    At 1%+ frequency there are 89 of them. Substring match, lowercased, because
#    these appear in many near-identical variants ('steam cloud', 'steam trading
#    cards', 'steam achievements').
KEYWORD_BLOCKLIST = (
    "steam", "xbox", "playstation", "psn", "ea app", "origin", "uplay", "gog",
    "epic", "digital distribution", "achievement", "trophies", "trading card",
    "cloud", "dlc", "downloadable content", "games on demand", "backwards compat",
    "controller support", "bink video", "polygonal", "original soundtrack",
    "auto-saving", "wasd", "widescreen", "60 fps", "4k", "ray tracing", "denuvo",
    "drm", "steamworks", "remote play", "leaderboard", "cross-platform",
    "game engine", "unreal engine", "unity", "havok", "patch", "pre-order",
    "season pass", "microtransaction", "early access", "kickstarter", "greenlight",
    "workshop", "mod support", "level editor", "save game", "checkpoint",
    "scummvm", "the game awards", "based on -", "year in the title", "nominee",
    "winner", "compatible", "sequel", "prequel", "spiritual successor", "remake",
    "remaster", "license",
    # Trade-show and storefront-promotion tags. Same class as the rest: they
    # record where a game was shown or sold, not what it is. Caught late,
    # because 'previously on - stadia pro' was the ONLY keyword Life is Strange
    # Remastered carried -- so it would have become that game's entire
    # contribution to the feature space.
    "previously on -", "pax ", "e3 ", "gamescom", "indiecade", "igf ",
    "showcase", "demo day", "game pass", "humble", "itch.io",
)

# 2. The frequency band. Below the floor a keyword is on so few games it cannot
#    generalise. Above the ceiling it is too common to discriminate.
#
# The floor was first set from a 2,000-game sample taken in rating-count order,
# and that was WRONG: popular games share keywords far more than the catalog at
# large does, so 1.5% of the sample (30 games) became 1.5% of the real corpus
# (94 games) and the filter turned out roughly three times stricter than
# intended. Shipped once at 1.5% it kept 197 keywords at 5.5 per game and killed
# 34 of the 41 mood keywords outright.
#
# Recalibrated against all 6,345 catalog games:
#        floor    vocab   kw/game   mood keywords surviving
#        1.5%       197      5.5      7/41
#        0.8%       446      8.2     25/41
#        0.5%       703      9.8     35/41
#        0.3%     1,013     11.0     41/41   <- chosen
#        0.2%     1,339     11.8     41/41
#
# 0.3% is the point where the load stops climbing much (11.0 of a possible 12.2
# at any floor) and every mood keyword clears on its own merit. Note 11 per game
# against ~10 existing labels is about 1.1x, below the 1.4x the sample predicted
# -- the real catalog simply carries fewer in-band keywords per game than its
# most popular corner does.
#
# These are shares of the CORPUS, so they cannot be applied while normalising a
# single row -- see filter_keywords(), which runs once over the whole fetch.
KEYWORD_MIN_SHARE = 0.003
KEYWORD_MAX_SHARE = 0.15

_NS = uuid.NAMESPACE_OID
_last_request = 0.0


def throttle():
    global _last_request
    elapsed = time.monotonic() - _last_request
    if elapsed < MIN_REQUEST_INTERVAL:
        time.sleep(MIN_REQUEST_INTERVAL - elapsed)
    _last_request = time.monotonic()


# --------------------------------------------------------------------------
# Auth
# --------------------------------------------------------------------------

def load_cached_token():
    try:
        with open(TOKEN_PATH, encoding="utf-8") as handle:
            token = json.load(handle)
    except (OSError, ValueError):
        return None
    # 60s of slack so a token doesn't expire mid-run.
    if token.get("expires_at", 0) > time.time() + 60:
        return token.get("access_token")
    return None


def fetch_token():
    cached = load_cached_token()
    if cached:
        return cached

    if not IGDB_CLIENT_ID or not IGDB_CLIENT_SECRET:
        raise SystemExit(
            "IGDB_CLIENT_ID / IGDB_CLIENT_SECRET are not set.\n"
            "Add them to analytics/.env — see .env.example.")

    # data=, never params=. As query parameters the secret ends up in the URL,
    # and requests puts the URL into the HTTPError message -- so a rejected key
    # produced a traceback containing the live client_secret, which the launcher
    # then wrote to vortex.log and the user mailed to someone. Twitch documents
    # the POST body form anyway.
    response = requests.post(TOKEN_URL, data={
        "client_id": IGDB_CLIENT_ID,
        "client_secret": IGDB_CLIENT_SECRET,
        "grant_type": "client_credentials",
    }, timeout=20)

    if response.status_code >= 400:
        # raise_for_status() would print the URL; say what happened without it.
        raise RuntimeError(
            f"Twitch rejected the IGDB credentials (HTTP {response.status_code}). "
            "Check IGDB_CLIENT_ID and IGDB_CLIENT_SECRET in analytics/.env -- "
            "generating a new secret in the Twitch console invalidates the old one.")
    payload = response.json()

    with open(TOKEN_PATH, "w", encoding="utf-8") as handle:
        json.dump({
            "access_token": payload["access_token"],
            "expires_at": time.time() + payload.get("expires_in", 0),
        }, handle)

    return payload["access_token"]


def post(url, body, token):
    """One apicalypse POST, with throttling and 429 backoff."""
    headers = {
        "Client-ID": IGDB_CLIENT_ID,
        "Authorization": f"Bearer {token}",
        "Accept": "application/json",
    }

    for attempt in range(MAX_RETRIES):
        throttle()
        response = requests.post(url, headers=headers, data=body, timeout=30)

        if response.status_code == 429:
            wait = float(response.headers.get("Retry-After", 2 ** attempt))
            print(f"  rate limited, waiting {wait:.1f}s", file=sys.stderr)
            time.sleep(wait)
            continue

        response.raise_for_status()
        return response.json()

    raise RuntimeError(f"giving up after {MAX_RETRIES} rate-limited attempts")


# --------------------------------------------------------------------------
# Fetch
# --------------------------------------------------------------------------

FIELDS = (
    "fields id,name,total_rating,total_rating_count,first_release_date,"
    "genres.name,themes.name,game_modes.name,keywords.name,cover.url,"
    "involved_companies.company.name,involved_companies.developer,"
    "external_games.uid,external_games.external_game_source;"
)


def is_blocked_keyword(name):
    return any(term in name.lower() for term in KEYWORD_BLOCKLIST)


def filter_keywords(records):
    """Apply the frequency band across the whole fetch, in place.

    Deliberately a second pass rather than part of normalise(). The band is a
    property of the corpus -- "on between 0.3% and 15% of games" -- and a
    per-row filter has no idea how common a keyword is, so it would silently
    keep everything and the vocabulary would land near 3,500.

    Keywords named in scoring.MOOD_KEYWORDS are kept whatever their frequency.
    They are the hand-checked list that steers the moods, and a band tuned for
    something else has no business deleting them -- which is exactly what
    happened on the first run here, when a mis-set floor silently removed 34 of
    41 and left the mood weights pointing at labels no game carried. The band
    controls how much keyword mass enters the model; it must not also decide
    whether a deliberate choice survives.

    Returns the surviving vocabulary size, for the caller to report.
    """
    if not records:
        return 0

    frequency = {}
    for record in records:
        for keyword in set(record["keywords"]):
            frequency[keyword] = frequency.get(keyword, 0) + 1

    total = len(records)
    keep = {k for k, n in frequency.items()
            if KEYWORD_MIN_SHARE <= n / total <= KEYWORD_MAX_SHARE}

    # Imported here rather than at module scope: this is the only place the
    # fetcher needs the ranking model, and a top-level import would drag numpy
    # and scikit-learn into a script that is otherwise pure network and SQL.
    from scoring import MOOD_KEYWORD_NAMES
    keep |= {k for k in frequency if k.lower() in MOOD_KEYWORD_NAMES}

    for record in records:
        record["keywords"] = sorted(k for k in set(record["keywords"]) if k in keep)

    return len(keep)


def fetch_catalog(token, max_rows=None):
    """Page through IGDB using keyset pagination.

    Keyset (`where id > last`) rather than `offset`: IGDB caps offset at 5000,
    and keyset stays stable if the catalog changes underneath a long run.
    """
    rows = []
    last_id = 0

    while True:
        # game_type = 0 is "main game" (excludes DLC, expansions, bundles).
        # NB: this used to be `category`, which IGDB has deprecated -- the old
        # field is still accepted but silently matches nothing, so a query
        # using it returns zero rows rather than an error.
        # version_parent = null drops regional re-releases and ports.
        body = (
            f"{FIELDS} "
            f"where game_type = 0 & version_parent = null "
            f"& platforms = ({PC_PLATFORM_ID}) "
            f"& total_rating_count >= {MIN_RATING_COUNT} & id > {last_id}; "
            f"sort id asc; limit {PAGE_SIZE};"
        )
        page = post(GAMES_URL, body, token)
        if not page:
            break

        rows.extend(page)
        last_id = page[-1]["id"]
        print(f"  fetched {len(rows)} games (last id {last_id})")

        if len(page) < PAGE_SIZE:
            break
        if max_rows and len(rows) >= max_rows:
            rows = rows[:max_rows]
            break

    return rows


def fetch_time_to_beat(token, game_ids):
    """Batch the time-to-beat lookup; it is a separate endpoint."""
    lengths = {}
    for start in range(0, len(game_ids), PAGE_SIZE):
        chunk = game_ids[start:start + PAGE_SIZE]
        body = (f"fields game_id,normally; "
                f"where game_id = ({','.join(str(g) for g in chunk)}); "
                f"limit {PAGE_SIZE};")
        try:
            for row in post(TTB_URL, body, token):
                if row.get("normally"):
                    lengths[row["game_id"]] = float(row["normally"])
        except Exception as exc:
            # Optional enrichment: a failure here must not lose the catalog.
            print(f"  [warn] time-to-beat batch failed: {exc}", file=sys.stderr)
    return lengths


# --------------------------------------------------------------------------
# Normalise
# --------------------------------------------------------------------------

def developer_of(row):
    for company in row.get("involved_companies") or []:
        if company.get("developer"):
            name = (company.get("company") or {}).get("name")
            if name:
                return name
    return "Unknown"


def steam_appid_of(row):
    for external in row.get("external_games") or []:
        # external_game_source 1 == Steam
        if external.get("external_game_source") == 1:
            uid = external.get("uid")
            if uid and str(uid).isdigit():
                return int(uid)
    return None


def cover_of(row):
    url = (row.get("cover") or {}).get("url")
    if not url:
        return None
    if url.startswith("//"):
        url = "https:" + url
    return url.replace("t_thumb", "t_cover_big")


def normalise(row, lengths):
    genres = [g["name"] for g in row.get("genres") or [] if g.get("name")]
    themes = [t["name"] for t in row.get("themes") or [] if t.get("name")]
    modes = [m["name"] for m in row.get("game_modes") or [] if m.get("name")]
    # Blocklist here, frequency band later in filter_keywords() -- the band
    # needs the whole corpus and cannot be decided from one row.
    keywords = [k["name"] for k in row.get("keywords") or []
                if k.get("name") and not is_blocked_keyword(k["name"])]
    if not genres and not themes:
        return None  # nothing to compute similarity from

    name = row.get("name")
    if not name:
        return None

    released = None
    if row.get("first_release_date"):
        released = date.fromtimestamp(row["first_release_date"])

    return {
        "game_id": str(uuid.uuid5(_NS, "canon_" + make_canonical(name))),
        "canonical_name": make_canonical(name),
        "name": name,
        "external_id": str(row["id"]),
        "developer": developer_of(row),
        "genres": genres,
        "themes": themes,
        "game_modes": modes,
        "keywords": keywords,
        # `tags` is no longer read by the vectorizer -- model.build_document()
        # takes keywords instead. Kept populated so older tooling and any
        # hand-written query against the column still behave, but note it has
        # always been exactly genres|themes and never carried anything of its
        # own; that redundancy is what keywords replaced.
        "tags": sorted(set(genres) | set(themes)),
        "rating": row.get("total_rating"),
        "total_rating_count": row.get("total_rating_count"),
        "game_length": lengths.get(row["id"]),
        "steam_appid": steam_appid_of(row),
        "released_at": released,
        "cover_url": cover_of(row),
    }


# --------------------------------------------------------------------------
# Store
# --------------------------------------------------------------------------

def clear_catalog(cur):
    """Drop the previous catalog so a refresh REPLACES rather than appends.

    Without this the insert below (ON CONFLICT DO NOTHING, no delete path) only
    ever grows the table, so tightening the query changes nothing that is
    already stored -- the console-only games this filter now excludes would sit
    in the database forever and keep taking Discover slots.

    Order matters: the foreign keys are ON DELETE NO ACTION, so anything
    pointing at a catalog row has to go first.

    Games referenced by `sessions` are deliberately spared. A row with play
    history is a game the user actually ran, and a catalog refresh must never
    destroy that.
    """
    cur.execute("""
        DELETE FROM recommendation_events
         WHERE game_id IN (SELECT game_id FROM games WHERE source = 'IGDB_Catalog')
    """)
    events = cur.rowcount

    cur.execute("""
        DELETE FROM recommendations_cache
         WHERE recommended_game IN (SELECT game_id FROM games WHERE source = 'IGDB_Catalog')
    """)

    cur.execute("""
        DELETE FROM games
         WHERE source = 'IGDB_Catalog'
           AND game_id NOT IN (SELECT game_id FROM sessions WHERE game_id IS NOT NULL)
    """)
    return cur.rowcount, events


def store(records):
    conn = get_connection()
    cur = conn.cursor()
    now = datetime.now()

    removed, dropped_events = clear_catalog(cur)
    if removed:
        print(f"Cleared {removed} previous catalog rows.")
    if dropped_events:
        # Fatigue in recommend.py is driven by these; it restarts from empty.
        print(f"  ({dropped_events} impression rows went with them, so "
              f"recommendation fatigue starts fresh.)")

    inserted = 0
    enriched = 0
    for r in records:
        # A canonical_name already present belongs to the user's own library.
        # The owned row must never be downgraded to a discovery candidate --
        # hence no name/source/installed in the UPDATE -- but we DO take its
        # themes, which the C++ metadata path never fetched for owned games.
        # Previously this was DO NOTHING and those themes were thrown away.
        cur.execute("""
            INSERT INTO games (game_id, source, external_id, name, canonical_name,
                               developer, genres, tags, themes, game_modes, keywords,
                               rating, total_rating_count, game_length, steam_appid,
                               released_at, cover_url, fetched_at)
            VALUES (?, 'IGDB_Catalog', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
                    ?, ?, ?, ?, ?, ?)
            ON CONFLICT (canonical_name) DO UPDATE
               SET themes     = COALESCE(NULLIF(EXCLUDED.themes, '[]'), games.themes),
                   game_modes = COALESCE(NULLIF(EXCLUDED.game_modes, '[]'), games.game_modes),
                   keywords   = COALESCE(NULLIF(EXCLUDED.keywords, '[]'), games.keywords)
             WHERE games.source <> 'IGDB_Catalog'
               AND (games.themes IS NULL OR json_array_length(games.themes) = 0
                    OR games.game_modes IS NULL OR json_array_length(games.game_modes) = 0
                    OR games.keywords IS NULL OR json_array_length(games.keywords) = 0)
        """, (r["game_id"], r["external_id"], r["name"], r["canonical_name"],
              r["developer"], encode_list(r["genres"]), encode_list(r["tags"]),
              encode_list(r["themes"]), encode_list(r["game_modes"]),
              encode_list(r["keywords"]), r["rating"],
              r["total_rating_count"], r["game_length"], r["steam_appid"],
              r["released_at"], r["cover_url"], now))
        # rowcount is 1 for a fresh insert and 1 for an owned row we enriched;
        # the source tells them apart.
        if cur.rowcount:
            cur.execute("SELECT source FROM games WHERE canonical_name = ?",
                        (r["canonical_name"],))
            row = cur.fetchone()
            if row and row[0] == "IGDB_Catalog":
                inserted += 1
            else:
                enriched += 1

    conn.commit()
    cur.close()
    conn.close()
    return inserted, enriched


def backfill_owned_labels(token):
    """Fetch themes and game modes for owned games the catalog does not cover.

    The catalog only carries PC games with enough ratings, so a chunk of the
    user's library falls outside it -- and the C++ scan path only writes themes
    for games it resolves fresh, never for ones already in igdb_cache.txt. This
    closes that gap directly, by IGDB id, in one batched request.
    """
    conn = get_connection()
    cur = conn.cursor()
    cur.execute("""
        SELECT external_id, canonical_name FROM games
         WHERE source <> 'IGDB_Catalog'
           AND external_id IS NOT NULL AND external_id <> '0'
           AND (themes IS NULL OR json_array_length(themes) = 0
                OR game_modes IS NULL OR json_array_length(game_modes) = 0
                OR keywords IS NULL OR json_array_length(keywords) = 0)
    """)
    pending = {str(e): c for e, c in cur.fetchall()}

    if not pending:
        cur.close(); conn.close()
        return 0

    # The keyword vocabulary the catalog settled on, so owned rows are held to
    # the same filtered set rather than importing IGDB's full 7,418.
    cur.execute("""
        SELECT DISTINCT unnest(keywords) FROM games
         WHERE source = 'IGDB_Catalog' AND keywords IS NOT NULL
    """)
    vocabulary = {row[0] for row in cur.fetchall()}

    updated = 0
    ids = [i for i in pending if i.isdigit()]
    for start in range(0, len(ids), PAGE_SIZE):
        chunk = ids[start:start + PAGE_SIZE]
        # Both label sets in one request rather than two passes.
        body = (f"fields id,themes.name,game_modes.name,keywords.name; "
                f"where id = ({','.join(chunk)}); limit {PAGE_SIZE};")
        try:
            rows = post(GAMES_URL, body, token)
        except Exception as exc:
            print(f"  [warn] label backfill failed: {exc}", file=sys.stderr)
            break

        for row in rows:
            canonical = pending.get(str(row["id"]))
            if not canonical:
                continue
            themes = [t["name"] for t in row.get("themes") or [] if t.get("name")]
            modes = [m["name"] for m in row.get("game_modes") or [] if m.get("name")]
            # Restricted to the vocabulary the catalog already kept. The
            # frequency band cannot be recomputed from a handful of owned games,
            # and a keyword unique to the user's library would enter the
            # vectorizer with a near-maximal IDF while matching no candidate at
            # all -- weight with nothing to connect it to.
            keywords = sorted({k["name"] for k in row.get("keywords") or []
                               if k.get("name") and k["name"] in vocabulary})
            if not themes and not modes and not keywords:
                continue
            # COALESCE/NULLIF so an empty result never wipes a set we already
            # have from the catalog collision path.
            cur.execute("""
                UPDATE games
                   SET themes     = COALESCE(NULLIF(?, '[]'), themes),
                       game_modes = COALESCE(NULLIF(?, '[]'), game_modes),
                       keywords   = COALESCE(NULLIF(?, '[]'), keywords)
                 WHERE canonical_name = ?
            """, (encode_list(themes), encode_list(modes),
                  encode_list(keywords), canonical))
            updated += cur.rowcount

    conn.commit()
    cur.close()
    conn.close()
    return updated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--refresh", action="store_true",
                        help="fetch the catalog (required; network access is never implicit)")
    parser.add_argument("--limit", type=int, default=None,
                        help="stop after roughly this many games")
    args = parser.parse_args()

    if not args.refresh:
        parser.print_help()
        return 1

    started = time.time()
    token = fetch_token()

    print("Fetching catalog...")
    raw = fetch_catalog(token, max_rows=args.limit)
    print(f"Fetched {len(raw)} raw rows in {time.time() - started:.1f}s")

    print("Fetching time-to-beat...")
    lengths = fetch_time_to_beat(token, [r["id"] for r in raw])
    print(f"  got {len(lengths)} lengths")

    records, seen = [], set()
    for row in raw:
        record = normalise(row, lengths)
        # IGDB can return distinct ids that canonicalise identically; keep the
        # first, which sorting by id makes deterministic.
        if record and record["canonical_name"] not in seen:
            seen.add(record["canonical_name"])
            records.append(record)

    print(f"Normalised to {len(records)} usable rows "
          f"({len(raw) - len(records)} dropped: no genres/themes or duplicate)")

    raw_vocab = len({k for r in records for k in r["keywords"]})
    kept_vocab = filter_keywords(records)
    per_game = sum(len(r["keywords"]) for r in records) / max(len(records), 1)
    print(f"Keywords: {raw_vocab} distinct after the blocklist, {kept_vocab} after the "
          f"{KEYWORD_MIN_SHARE:.1%}-{KEYWORD_MAX_SHARE:.0%} frequency band "
          f"({per_game:.1f} per game)")

    inserted, enriched = store(records)
    print(f"Inserted {inserted} new catalog rows "
          f"({len(records) - inserted} already present as owned games).")
    if enriched:
        print(f"Backfilled labels onto {enriched} owned games from the catalog.")

    # The catalog only covers PC games with enough ratings, so the rest of the
    # library needs asking for directly.
    remaining = backfill_owned_labels(token)
    if remaining:
        print(f"Backfilled labels onto {remaining} more owned games by IGDB id.")

    print(f"Total elapsed {time.time() - started:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
