"""Produce the recommendation list consumed by the launcher.

    python recommend.py <mood> [run_id]

Writes two files next to this script:

    recommendations.json       top-level JSON array -- the bridge rejects
                               anything else, so metadata goes in the sidecar
    recommendations_meta.json  {run_id, generated_at, status, ...}

The run_id is a nonce passed in by the bridge. The bridge only accepts the
JSON if the sidecar echoes the nonce it generated, which is what stops a stale
file being displayed as a fresh result when this script fails.

No network access happens here. Cover art is downloaded by the caller, after
the list is already on screen -- this script runs inside a bounded QProcess and
must not block on I/O.
"""

import hashlib
import json
import os
import sys
from datetime import datetime, timedelta

import numpy as np
import pandas as pd

import model
import scoring
from db import get_connection
from interest import build_interest
from model import MODEL_VERSION

BASE_DIR = os.path.dirname(__file__)
OUTPUT_PATH = os.path.join(BASE_DIR, "recommendations.json")
META_PATH = os.path.join(BASE_DIR, "recommendations_meta.json")

# Slot counts per section. Discovery gets more because its pool is ~200x
# larger, so there is far more worth surfacing; the library section is capped
# by how many unplayed games the user actually owns.
LIBRARY_N = 10
DISCOVER_N = 12

# The basis for the exploration temperature, deliberately NOT the slot count.
# section_temperature() measures the score range over ranks 1..k, so raising k
# widens that range and quietly heats up exploration. EXPLORE_ALPHA was
# calibrated at 10 (25% churn / 96% quality library, 37% / 99% discover), so
# the basis stays fixed and only the number of slots changes.
TEMPERATURE_BASIS_N = 10
CATALOG_STALE_DAYS = 30

OWNED_SOURCES = ("Local", "Steam")

# Mood id 1, named here rather than written as a bare 1 at the filter below.
COMPETITIVE_MOOD = 1

# Competitive means playing against people. A game with no multiplayer mode at
# all cannot deliver that however well its genres score, so this is an
# eligibility rule rather than a weight -- no amount of Shooter and Sniping
# makes a single-player campaign competitive.
MULTIPLAYER_MODES = {"multiplayer", "co-operative", "split screen",
                     "massively multiplayer online (mmo)", "battle royale"}


def playable_against_others(value):
    """True unless the game's modes are known and none of them involve others.

    No modes listed at all is UNKNOWN, not known-single-player, so it passes.
    26 catalog candidates are in that state, and dropping them would assert
    something the data does not say.
    """
    modes = ({str(m).strip().lower() for m in value}
             if isinstance(value, (list, tuple)) else set())
    return not modes or bool(modes & MULTIPLAYER_MODES)


def _write_json(path, payload):
    """Atomic replace so a reader never sees a half-written file."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=4)
    os.replace(tmp, path)


def write_output(items, run_id, mood, status, candidate_count=0, curated=False,
                 ignore_played=False, ignore_liked=False):
    _write_json(OUTPUT_PATH, items)
    _write_json(META_PATH, {
        "run_id": run_id,
        "generated_at": datetime.now().isoformat(timespec="seconds"),
        "model_version": MODEL_VERSION,
        "mood": mood,
        # Recorded for the same reason `mood` is: without it a served list
        # cannot be attributed after the fact, and two runs that differ only by
        # one of these flags look like unexplained churn.
        "curated": bool(curated),
        "ignore_played": bool(ignore_played),
        "ignore_liked": bool(ignore_liked),
        "status": status,
        "candidate_count": candidate_count,
        "returned": len(items),
    })


def _read_sql(cur, query):
    """Small DataFrame reader over a raw DB-API cursor.

    pandas.read_sql warns on anything that isn't a SQLAlchemy connectable, and
    that warning goes to stderr -- which the launcher captures and reports as
    the reason the ML run "failed". Reading the cursor directly keeps stderr
    clean without pulling in SQLAlchemy for two queries.
    """
    cur.execute(query)
    columns = [d[0] for d in cur.description]
    return pd.DataFrame(cur.fetchall(), columns=columns)


def load_frames():
    conn = get_connection()
    try:
        cur = conn.cursor()
        games = _read_sql(cur, "SELECT * FROM games")
        sessions = _read_sql(
            cur,
            "SELECT s.* FROM sessions s "
            "JOIN users u ON u.user_id = s.user_id AND u.username = 'local_user'")
        cur.close()
    finally:
        conn.close()
    return games, sessions


def load_recent_impressions(runs=scoring.FATIGUE_RUNS):
    """game_id -> times served across the last N distinct runs.

    Counted over run_ids rather than a time window: a window behaves very
    differently for someone who opens the launcher daily versus weekly, whereas
    "the last ten things you were shown" means the same thing to both.

    The count is deliberately not the same number that saturates the penalty --
    see FATIGUE_SATURATES_AT. A long memory with a short saturation point is what
    lets a game stay demoted for several runs instead of bouncing straight back.

    recommend.py has been writing these impression rows since feedback capture
    landed; nothing read them until now.
    """
    try:
        conn = get_connection()
        cur = conn.cursor()
        cur.execute("""
            SELECT game_id, count(*)
            FROM recommendation_events
            WHERE event_type = 'impression'
              AND run_id IN (
                  SELECT run_id FROM recommendation_events
                  WHERE event_type = 'impression' AND run_id IS NOT NULL
                  GROUP BY run_id
                  ORDER BY max(created_at) DESC
                  LIMIT ?
              )
            GROUP BY game_id
        """, (runs,))
        counts = {str(gid): int(n) for gid, n in cur.fetchall()}
        cur.close()
        conn.close()
        return counts
    except Exception as exc:
        # Fatigue is an enhancement, not a correctness requirement -- degrade
        # to "nothing has been shown yet" rather than failing the run.
        print(f"[warn] could not read impression history: {exc}", file=sys.stderr)
        return {}


def _rng_for(run_id):
    """Seeded from the run nonce, so each refresh differs but any single run
    can be reproduced exactly from its id when debugging."""
    if run_id:
        seed = int(hashlib.sha256(run_id.encode("utf-8")).hexdigest()[:16], 16)
        return np.random.default_rng(seed)
    return np.random.default_rng()


def _explanation_rng(run_id, section):
    """A SEPARATE random stream for the explanation quota.

    Deliberately not the `_rng_for(run_id)` stream. That one is already consumed
    by gumbel_perturb, and drawing from it here would shift every subsequent
    Gumbel value -- silently changing WHICH GAMES get recommended, not just how
    they are described. Mixing the section name in also keeps the library and
    discover draws independent of each other and of their ordering.

    Seeded from the run nonce like everything else here, so a refresh is
    reproducible from its id.
    """
    seed = hashlib.sha256(f"explain:{section}:{run_id}".encode("utf-8")).hexdigest()[:16]
    return np.random.default_rng(int(seed, 16))


def load_favorites():
    """Favourites come straight from the launcher's preferences file.

    Read here rather than from the database so a favourite takes effect on the very
    next run, without waiting for a sync.
    """
    from sync_local_data import game_uuid, parse_preferences
    return {game_uuid(name) for name in parse_preferences()}


def warn_if_catalog_stale(games):
    """Report the state of the discovery catalog: missing, stale, or current.

    Three states, not two. The previous version returned early when there were
    no timestamps at all, which is exactly the case that most needs saying:
    a catalog that was never fetched produces an empty Discover section and
    said nothing about why, so it looked like the feature was broken rather
    than unpopulated.
    """
    catalog_rows = 0
    if "source" in games.columns:
        catalog_rows = int((games["source"].astype(str) == "IGDB_Catalog").sum())

    if catalog_rows == 0:
        print("[warn] the discovery catalog is empty, so Discover has nothing to "
              "suggest.\n"
              "       Fetch it with: python igdb_catalog.py --refresh "
              "(needs IGDB credentials, takes a few minutes).",
              file=sys.stderr)
        return

    if "fetched_at" not in games.columns:
        return
    fetched = pd.to_datetime(games["fetched_at"], errors="coerce").dropna()
    if fetched.empty:
        return
    age = datetime.now() - fetched.max().to_pydatetime()
    if age > timedelta(days=CATALOG_STALE_DAYS):
        print(f"[warn] candidate catalog is {age.days} days old ({catalog_rows} "
              f"games); run: python igdb_catalog.py --refresh", file=sys.stderr)


def build_recommendations(games, sessions, favorites, mood,
                          explore=True, impressions=None, run_id="", curated=False,
                          ignore_played=False, ignore_liked=False):
    """Rank candidates.

    explore=False disables fatigue and Gumbel noise entirely, which is what
    evaluate.py uses: metrics must be reproducible, and fatigue depends on live
    impression history that would otherwise leak into offline scoring.

    curated=True restricts the DISCOVER section to games that are popular,
    very well rated or newly released -- the Settings toggle. The library
    section is never affected; see the filter below for why.

    ignore_played=True and ignore_liked=True drop, respectively, the games the
    user has PLAYED and the ones they merely HEARTED from the profile -- the
    other two Settings toggles. Either way the games stay excluded from the
    candidates, and both together leave the mood alone to rank with; see below.
    """
    if games.empty:
        return [], 0

    games = games.reset_index(drop=True)
    names = games["name"].tolist()
    index_of = {gid: row for row, gid in enumerate(games["game_id"].astype(str))}

    documents = model.build_documents(games)

    # Every document empty means no game carries a single genre, theme, mode or
    # keyword. TfidfVectorizer answers that with "ValueError: empty vocabulary;
    # perhaps the documents only contain stop words", which is true and useless
    # -- it names a symptom of the vectoriser, not the cause.
    #
    # The real cause is upstream: IGDB never resolved, so sync_local_data.py
    # wrote the "Unknown" placeholder for every label and model._labels()
    # correctly strips it. Say that instead, and return empty rather than
    # raising, so the launcher reports "ML returned no results" (the recommender
    # ran and had nothing to work with) rather than "ML unavailable" (it did not
    # run at all). The difference is the whole diagnosis.
    if not any(document.strip() for document in documents):
        print(f"[warn] none of the {len(games)} games have genre or theme data, "
              "so similarity cannot be computed.\n"
              "       IGDB metadata was never fetched -- check IGDB_CLIENT_ID "
              "and IGDB_CLIENT_SECRET in analytics/.env.",
              file=sys.stderr)
        return [], 0

    vectorizer = model.load_vectorizer(expected_rows=len(games), documents=documents)
    if vectorizer is None:
        # Refitting costs well under 100ms at catalog size, which is far
        # cheaper than the correctness risk of a stale vocabulary.
        vectorizer = model.fit_vectorizer(documents)
        model.save_vectorizer(vectorizer, n_rows=len(games), documents=documents)
    item_vectors = vectorizer.transform(documents).toarray()

    session_records = [
        {
            "game_id": str(row.game_id),
            "duration_seconds": int(row.duration_seconds or 0),
            "ended_at": row.ended_at,
        }
        for row in sessions.itertuples()
        if row.ended_at is not None
    ]

    lengths = {}
    if "game_length" in games.columns:
        lengths = {
            str(games["game_id"].iloc[i]): (games["game_length"].iloc[i] or 0)
            for i in range(len(games))
        }

    interest, disinterest, stats = build_interest(session_records, favorites, lengths)

    # The two "ignore" toggles, applied to a COPY. `interest` itself has to stay
    # whole because it is what decides exclusion further down; filtering it in
    # place would put every dropped game straight back into the candidate pool,
    # which is the one thing these toggles must not do.
    #
    # Moods only. Neutral has no mood vector to fall back on, so stripping its
    # history would leave the quality term alone to rank with, which is not a
    # recommendation, it is a popularity chart.
    played = set(stats.get("engaged_ids", ()))
    played_ignored = bool(ignore_played) and mood != scoring.NEUTRAL_MOOD
    liked_ignored = bool(ignore_liked) and mood != scoring.NEUTRAL_MOOD

    dropped = set()
    if played_ignored:
        dropped |= played
    if liked_ignored:
        # Hearts only. A game that was BOTH played and hearted counts as played
        # -- the same call _explanation_sources makes when it decides between
        # "Because you played" and "Because you liked" -- so the liked filter
        # leaves it alone and real playtime survives this toggle.
        dropped |= {game_id for game_id in favorites if game_id not in played}

    # Both on empties the profile, since `interest` holds nothing but played and
    # hearted games. That needs no special case: build_profile() answers a zero
    # taste vector with the mood alone, which is exactly what was asked for.
    profile_interest = interest
    if dropped:
        profile_interest = {game_id: weight for game_id, weight in interest.items()
                            if game_id not in dropped}

    # `interest` stays the full play history and is what decides what the user
    # has already played. `effective_interest` is that history reweighted by how
    # well each game fits the chosen mood, and is what shaped the profile -- so
    # it is the honest basis for an explanation, but NOT for exclusion.
    profile, taste, mood_vec, effective_interest = scoring.build_profile(
        item_vectors, index_of, profile_interest, disinterest, vectorizer, mood)

    counts = games["total_rating_count"].tolist() if "total_rating_count" in games.columns else None
    quality = scoring.quality_scores(games["rating"].tolist(), counts)

    source = games["source"].astype(str) if "source" in games.columns else pd.Series(["Local"] * len(games))
    owned_mask = source.isin(OWNED_SOURCES).to_numpy()

    # Only games that are on the machine right now belong in the library
    # section. `source` alone cannot say: igdb_cache.txt is append-only and is
    # never pruned on uninstall, so removed games stayed 'Local' forever and,
    # being stale but high-scoring, took the top slots — the UI drew them as
    # "NOT IN LIBRARY" because it could not match them to a real game.
    #
    # Fall back to source when nothing is marked installed, so a database
    # synced before installed_games.txt existed still shows a library section
    # rather than an empty one.
    if "installed" in games.columns:
        installed_mask = games["installed"].fillna(False).astype(bool).to_numpy()
        if installed_mask.any():
            owned_mask = owned_mask & installed_mask

    scores, similarity = scoring.score_games(profile, item_vectors, quality)

    # `scores` stays the honest model output and is what gets reported; only
    # `ranking_scores` carries the exploration adjustments, so a card never
    # displays a number that includes random noise. Fatigue is global (it is
    # already in score units); the Gumbel noise is applied per section, since
    # each has its own score scale.
    ranking_scores = scores
    if explore:
        ranking_scores = scores - scoring.fatigue_penalties(
            impressions or {}, index_of, len(games))
    rng = _rng_for(run_id)

    # Candidate pool: everything except games already engaged with and games
    # the behaviour says were bounced off. Note the wishlist is deliberately
    # absent -- it is a saved list and has no influence on ranking.
    #
    # RAW `interest`, never `effective_interest`. Mood conditioning drops played
    # games out of the effective dict -- under Competitive it drops Stellar
    # Blade, Life is Strange, Absolute Drift and Wuthering Waves -- and reading
    # this line from that dict would put all four straight back into the
    # candidate pool and recommend games the user has already played. Same class
    # of bug as Forza Horizon 5 once recommending itself.
    excluded = set(interest) | set(disinterest)
    eligible = [
        row for row in range(len(games))
        if str(games["game_id"].iloc[row]) not in excluded
        and np.linalg.norm(item_vectors[row]) > 0
    ]
    if not eligible:
        return [], 0

    # Rank owned and unowned separately. Ranked together, discovery wins every
    # slot purely on numbers -- the catalog is ~200x larger than the unplayed
    # library, so the best of 5,700 beats the best of 26 essentially always,
    # and games the user could actually launch right now vanish from the tab.
    library_rows = [r for r in eligible if owned_mask[r]]
    discover_rows = [r for r in eligible if not owned_mask[r]]

    def drop_off_mood(rows, k):
        """Remove candidates the mood actively points away from.

        This changes the deterministic top-k in no mood and no section --
        measured, not assumed; those games were never going to rank. Its whole
        job is exploration: EXPLORE_ALPHA is 25% of the visible score range,
        which is easily enough Gumbel noise to lift a horror game into a Relaxed
        list from further down the pool. A game with negative mood fit is not a
        fresh pick, it is a wrong one.

        The floor matters. Immersive's installed-library pool is 18 games and 10
        survive the filter -- exactly LIBRARY_N -- which would leave MMR nothing
        to choose between and no room for exploration at all. Below 2x the slot
        count the filter is skipped rather than allowed to starve the section.
        """
        if mood_vec is None or not rows:
            return rows
        fits = scoring.mood_fits(item_vectors, mood_vec)
        kept = [r for r in rows if fits[r] >= 0]
        return kept if len(kept) >= 2 * k else rows

    def drop_single_player_only(rows, k):
        if mood != COMPETITIVE_MOOD or not rows or "game_modes" not in games.columns:
            return rows

        kept = [r for r in rows
                if playable_against_others(games["game_modes"].iloc[r])]
        # Same starvation guard as drop_off_mood. It matters here: the installed
        # library has 17 Competitive candidates and 11 survive, against 10 slots,
        # so the guard skips this filter for that section and leaves it whole.
        # A library list with one spare candidate cannot rotate at all.
        return kept if len(kept) >= 2 * k else rows

    library_rows = drop_off_mood(library_rows, LIBRARY_N)
    discover_rows = drop_off_mood(discover_rows, DISCOVER_N)
    library_rows = drop_single_player_only(library_rows, LIBRARY_N)
    discover_rows = drop_single_player_only(discover_rows, DISCOVER_N)

    if curated:
        # Discover only, and not by preference -- by what the data can answer.
        # Owned rows carry NO total_rating_count at all (0 of 34 measured), so
        # "popular" and "trending" are unanswerable there; filtering the library
        # would mean filtering on rating alone, over an 18-game pool that has to
        # fill 10 slots.
        keep = scoring.curated_mask(
            games["rating"].tolist(),
            games["total_rating_count"].tolist() if "total_rating_count" in games.columns else [None] * len(games),
            games["released_at"].tolist() if "released_at" in games.columns else None)
        kept = [r for r in discover_rows if keep[r]]
        # Same starvation guard as drop_off_mood. Measured pools are ~100x the
        # slot count so this should never fire, but a smaller or fresher catalog
        # is exactly the case where it would.
        if len(kept) >= 2 * DISCOVER_N:
            discover_rows = kept
        else:
            print(f"[warn] curated filter left only {len(kept)} discover candidates; "
                  f"showing the unfiltered pool instead", file=sys.stderr)

    def column(row, name, default=None):
        if name not in games.columns:
            return default
        value = games[name].iloc[row]
        return default if value is None or (isinstance(value, float) and np.isnan(value)) else value

    # Studio is the diversity axis MMR cannot supply for itself. Discover only:
    # the library section is 18 games the user already owns, and capping it by
    # studio would just shrink an already thin pool for no benefit.
    developers = None
    if "developer" in games.columns:
        developers = [str(games["developer"].iloc[r] or "").strip()
                      for r in range(len(games))]

    def build(rows, section, k):
        section_scores = ranking_scores
        if explore and rows:
            # Temperature from this section's own visible range: the library
            # and the catalog differ by ~19x in how tightly packed their top
            # scores are, so one absolute value cannot serve both.
            section_scores = scoring.gumbel_perturb(
                ranking_scores, rng,
                scoring.section_temperature(ranking_scores, rows,
                                            k=TEMPERATURE_BASIS_N))
        selected = scoring.mmr_rerank(
            rows, section_scores, item_vectors, k=k,
            group_of=developers if section == "discover" else None,
            max_per_group=scoring.MAX_PER_DEVELOPER if section == "discover" else None)

        # How many cards this refresh reserves for games actually played, drawn
        # fresh each time. Ranking sources on similarity alone makes every card
        # say "liked" -- 55 hearted games against 8 played ones -- so without a
        # reservation the panel never mentions anything the user has played.
        #
        # Only when exploring, for the same reason fatigue and Gumbel are gated:
        # evaluate.py and the tests must stay reproducible.
        #
        # Nothing to reserve when played games were dropped from the profile:
        # they are absent from effective_interest, so no card can cite one and
        # a non-zero quota would just be a request that can never be filled.
        # Tied to the played toggle specifically -- dropping hearts leaves the
        # played sources in place and the reservation still has work to do.
        played_quota = None
        if explore and selected and not played_ignored:
            ceiling = scoring.PLAYED_QUOTA_MAX
            ceiling = len(selected) if ceiling is None else min(ceiling, len(selected))
            low = min(scoring.PLAYED_QUOTA_MIN, ceiling)
            played_quota = int(_explanation_rng(run_id, section).integers(low, ceiling + 1))
        # Reasons for the whole section in one pass, so no single game from the
        # profile can end up citing every card. Chosen per-card, the panel came
        # back twelve-for-twelve on one title.
        #
        # effective_interest, so the named game is one that actually shaped this
        # profile. Explaining a Competitive pick with "Because you played
        # Stellar Blade" would be false -- under Competitive that game
        # contributes exactly zero.
        reasons = scoring.explain_section(
            selected, item_vectors, effective_interest, index_of, names,
            taste, mood_vec, mood, played_ids=stats.get("engaged_ids", ()),
            played_quota=played_quota)

        # The evidence behind each reason string. explain_section() picks one
        # source per card and spreads citations across the panel; this keeps
        # the full ranked list so the card can show how strong the match
        # actually is instead of only asserting that one exists.
        inspirations = scoring.inspiration_sources(
            selected, item_vectors, effective_interest, index_of, names,
            played_ids=stats.get("engaged_ids", ()))

        items = []
        for position, row in enumerate(selected):
            item = {
                "name": names[row],
                "score": round(float(scores[row]), 4),
                "reason": reasons[position],
                "similarity": round(float(similarity[row]), 4),
                "inspiredBy": inspirations[position],
                "owned": section == "library",
                "section": section,
            }
            if section == "discover":
                # Unowned games are absent from the launcher's game list, so
                # everything the details page renders has to travel with them.
                # Keys mirror buildGameMap() in vortex_bridge.cpp so the same
                # QML renders both kinds of row.
                length = column(row, "game_length")
                rating = column(row, "rating")
                item.update({
                    "developer": column(row, "developer", "Unknown") or "Unknown",
                    "rating": float(rating) if rating is not None else 0.0,
                    "genres": ", ".join(column(row, "genres") or []) or "Unknown",
                    "tags": ", ".join(column(row, "tags") or []) or "Unknown",
                    "timeToBeat": f"{int(length // 3600)} Hours" if length else "N/A",
                    "coverUrl": column(row, "cover_url", "") or "",
                    "steamAppId": int(column(row, "steam_appid", 0) or 0),
                    "releasedAt": str(column(row, "released_at", "") or ""),
                })
            items.append(item)
        return items

    # One flat array, each item tagged with its section. The bridge rejects
    # anything that isn't a top-level array (vortex_bridge.cpp:382), so the
    # split is expressed per-item and the UI groups on it.
    items = (build(library_rows, "library", LIBRARY_N)
             + build(discover_rows, "discover", DISCOVER_N))
    return items, len(eligible)


def record_served(items, run_id, mood):
    """Persist what was served, plus one impression per item.

    recommendations_cache and recommendation_events existed in the schema from
    the start but nothing ever wrote to them, so there was no record of what
    the user was actually shown and no way to evaluate it after the fact.
    """
    if not items:
        return

    import uuid

    try:
        conn = get_connection()
        cur = conn.cursor()

        cur.execute("SELECT user_id FROM users WHERE username='local_user'")
        row = cur.fetchone()
        if not row:
            cur.close(); conn.close()
            return
        user_id = row[0]

        cur.execute("SELECT canonical_name, game_id FROM games")
        by_canonical = {c: g for c, g in cur.fetchall() if c}

        from sync_local_data import make_canonical

        for rank, item in enumerate(items, 1):
            game_id = by_canonical.get(make_canonical(item["name"]))
            if not game_id:
                continue
            cur.execute("""
                INSERT INTO recommendations_cache
                    (id, user_id, recommended_game, score, rank, mood, run_id, model_version)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """, (str(uuid.uuid4()), user_id, game_id, item["score"], rank,
                  mood, run_id, MODEL_VERSION))
            cur.execute("""
                INSERT INTO recommendation_events
                    (event_id, run_id, user_id, game_id, event_type, origin)
                VALUES (?, ?, ?, ?, 'impression', ?)
            """, (str(uuid.uuid4()), run_id, user_id, game_id, item.get("section", "library")))

        conn.commit()
        cur.close()
        conn.close()
    except Exception as exc:
        # Never let bookkeeping break the actual recommendation run.
        print(f"[warn] could not record served set: {exc}", file=sys.stderr)


def main():
    # Flags are stripped BEFORE the positional arguments are read, so they can
    # appear anywhere in argv and `recommend.py <mood> <run_id>` keeps working
    # untouched. The bridge's existing call site is positional; adding a flag to
    # a positional CLI is the classic way to break one.
    #
    # Split once on the leading dashes rather than once per flag: the second
    # toggle is where a chain of per-flag comprehensions starts going wrong.
    raw = sys.argv[1:]
    flags = {a for a in raw if a.startswith("--")}
    argv = [a for a in raw if not a.startswith("--")]
    curated = "--curated" in flags
    ignore_played = "--ignore-played" in flags
    ignore_liked = "--ignore-liked" in flags

    # Defaults to Neutral, matching the launcher's pre-picker default: with no
    # mood stated, assume none rather than silently applying Relaxed's weights.
    mood = int(argv[0]) if argv else scoring.NEUTRAL_MOOD
    run_id = argv[1] if len(argv) > 1 else ""

    try:
        games, sessions = load_frames()
    except Exception as exc:
        # Leave any previous recommendations.json alone; the bridge will reject
        # it on the nonce mismatch and fall back to its own ranking.
        print(f"[error] database unavailable: {exc}", file=sys.stderr)
        return 1

    warn_if_catalog_stale(games)

    items, candidates = build_recommendations(
        games, sessions, load_favorites(), mood,
        explore=True, impressions=load_recent_impressions(), run_id=run_id,
        curated=curated, ignore_played=ignore_played, ignore_liked=ignore_liked)
    status = "ok" if items else "no candidates"
    write_output(items, run_id, mood, status, candidates, curated=curated,
                 ignore_played=ignore_played, ignore_liked=ignore_liked)
    if run_id:
        record_served(items, run_id, mood)
    notes = "".join([", curated" if curated else "",
                     ", ignoring played" if ignore_played else "",
                     ", ignoring liked" if ignore_liked else ""])
    print(f"{len(items)} recommendations from {candidates} candidates "
          f"(mood {mood}{notes}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
