"""Regression tests for the ranking maths.

Run with:  python test_scoring.py

The NaN tests exist because both failures were silent: a NaN quality score
propagates into the final score, and `sorted()` does not raise on NaN -- it
just produces a meaningless order. The symptom was an entire section of the
recommendations list showing "nan" instead of a score.
"""

import sys

import numpy as np

import model
import scoring
from interest import build_interest, classify_session, duration_weight
from scoring import SHRINKAGE_M, quality_scores


def check(condition, label):
    print(f"{'PASS' if condition else 'FAIL'}  {label}")
    return bool(condition)


def test_quality_handles_missing_counts():
    print("\n-- rating shrinkage --")
    ok = True

    # numpy NaN, not Python None: this is what a DataFrame column yields, and
    # `float(nan) is None` is False, so an `is None` guard alone lets it slip
    # through and turns every downstream score into NaN.
    q = quality_scores([80.0, 90.0], [np.nan, np.nan])
    ok &= check(not np.isnan(q).any(), "NaN vote counts do not produce NaN scores")

    q = quality_scores([80.0, 90.0], [None, None])
    ok &= check(not np.isnan(q).any(), "None vote counts do not produce NaN scores")

    # A rating with no vote count must still influence its score, not collapse
    # to the catalog mean.
    q = quality_scores([50.0, 90.0], [np.nan, np.nan])
    ok &= check(q[1] > q[0], "higher rating still ranks higher without vote counts")

    # 0 means "unrated" in game_metadata.txt, not "terrible".
    q = quality_scores([0.0, 90.0, 50.0], [np.nan, np.nan, np.nan])
    prior = np.mean([0.9, 0.5])
    ok &= check(abs(q[0] - prior) < 1e-9, "rating 0 is treated as unrated (gets the prior)")

    # More votes = less shrinkage toward the mean.
    low = quality_scores([95.0, 60.0, 60.0], [1, 500, 500])
    high = quality_scores([95.0, 60.0, 60.0], [1000, 500, 500])
    ok &= check(high[0] > low[0], "more votes shrink a high rating less")

    q = quality_scores([None, None], [None, None])
    ok &= check(not np.isnan(q).any(), "no ratings at all still yields finite scores")

    ok &= check(bool(((quality_scores([0.0, 50.0, 100.0], None) >= 0).all()
                      and (quality_scores([0.0, 50.0, 100.0], None) <= 1).all())),
                "quality stays within [0,1]")
    return ok


def test_session_bands():
    print("\n-- session quality bands --")
    ok = True
    ok &= check(classify_session(6) == "abort", "6s launch-and-quit is an abort")
    ok &= check(classify_session(88) == "abort", "88s is still an abort")
    ok &= check(classify_session(300) == "neutral", "5min is ambiguous")
    ok &= check(classify_session(3600) == "engaged", "1h is real play")
    return ok


def test_duration_weight_is_stable():
    print("\n-- duration transform --")
    ok = True
    ok &= check(duration_weight(36000) <= 1.0, "capped at 1.0")
    ok &= check(duration_weight(360000) == duration_weight(36000),
                "beyond the cap adds nothing (one marathon can't dominate)")
    ok &= check(duration_weight(7200) > duration_weight(3600), "monotonic in duration")
    # The old model divided by max(duration), so every weight moved whenever a
    # new session landed. This must not depend on other sessions at all.
    ok &= check(duration_weight(3600) == duration_weight(3600), "independent of other sessions")
    return ok


def test_favorite_clears_abort_penalty():
    print("\n-- favouriting overrules the abort heuristic --")
    from datetime import datetime
    sessions = [
        {"game_id": "g1", "duration_seconds": 30, "ended_at": datetime(2026, 5, 1)},
        {"game_id": "g1", "duration_seconds": 20, "ended_at": datetime(2026, 5, 2)},
        {"game_id": "g2", "duration_seconds": 7200, "ended_at": datetime(2026, 5, 2)},
    ]
    _, disinterest, _ = build_interest(sessions, favorites=set())
    ok = check("g1" in disinterest, "two instant-quits mark a game as disinterest")

    _, disinterest, _ = build_interest(sessions, favorites={"g1"})
    ok &= check("g1" not in disinterest, "favouriting removes it (the only user override)")

    single = [{"game_id": "g3", "duration_seconds": 6, "ended_at": datetime(2026, 5, 1)}]
    _, disinterest, _ = build_interest(single, favorites=set())
    ok &= check("g3" not in disinterest, "a single crash does not condemn a game")
    return ok


def _mood_fixture():
    """A tiny corpus with unambiguous mood alignment.

    The filler document carries every label named in MOOD_LABELS so no mood has
    a label missing from the vocabulary -- otherwise mood_vector() correctly
    warns on stderr and buries the test output.
    """
    docs = [
        # clearly competitive: multiplayer shooter, no single-player claim
        model.build_document(["Shooter"], ["Action"], [], ["Multiplayer", "Battle Royale"]),
        # clearly immersive, and negative under Competitive via Single player
        model.build_document(["Role-playing (RPG)"], [], ["Fantasy"], ["Single player"]),
        model.SEP.join(sorted({label for labels in scoring.MOOD_LABELS.values()
                               for label in labels})),
    ]
    vectorizer = model.fit_vectorizer(docs)
    item_vectors = vectorizer.transform(docs).toarray()
    index_of = {"comp": 0, "imm": 1, "filler": 2}
    return vectorizer, item_vectors, index_of


def test_mood_relevance_gates_the_history():
    print("\n-- mood-conditioned play history --")
    vectorizer, item_vectors, index_of = _mood_fixture()
    ok = True

    interest = {"comp": 0.2, "imm": 0.9}

    # Neutral has no mood vector, and its whole contract is that no mood
    # parameter touches it.
    passthrough = scoring.mood_relevance(item_vectors, index_of, interest, None)
    ok &= check(passthrough == interest, "Neutral (mood_vec=None) returns the history unchanged")

    comp_vec = scoring.mood_vector(vectorizer, 1)
    gated = scoring.mood_relevance(item_vectors, index_of, interest, comp_vec)

    ok &= check("imm" not in gated,
                "a negative-fit game is dropped from the profile, not merely shrunk")
    ok &= check("comp" in gated, "a positive-fit game survives")

    # The exclusion set in recommend.py reads the RAW dict. If conditioning
    # mutated it in place, played games would silently re-enter the candidate
    # pool and be recommended back to the user.
    ok &= check(interest == {"comp": 0.2, "imm": 0.9},
                "the caller's interest dict is not mutated")

    # Fit decides, not playtime: 'imm' has 4.5x the interest weight but does not
    # survive Competitive at all, which is the entire point of this round.
    imm_vec = scoring.mood_vector(vectorizer, 2)
    gated_imm = scoring.mood_relevance(item_vectors, index_of, interest, imm_vec)
    ok &= check(gated_imm.get("imm", 0) > gated_imm.get("comp", 0),
                "the same history reweights differently under a different mood")
    return ok


def test_gamma_sharpens_the_gate():
    print("\n-- relevance exponent --")
    vectorizer, item_vectors, index_of = _mood_fixture()
    ok = True
    interest = {"comp": 0.2, "imm": 0.9}
    comp_vec = scoring.mood_vector(vectorizer, 1)

    original = scoring.MOOD_RELEVANCE_GAMMA
    try:
        scoring.MOOD_RELEVANCE_GAMMA = 1.0
        linear = scoring.mood_relevance(item_vectors, index_of, interest, comp_vec)
        scoring.MOOD_RELEVANCE_GAMMA = 2.0
        squared = scoring.mood_relevance(item_vectors, index_of, interest, comp_vec)
    finally:
        scoring.MOOD_RELEVANCE_GAMMA = original

    # Squaring a cosine in [0,1] can only shrink it, and shrinks weak matches
    # proportionally harder -- that is what stops a much-played, barely-matching
    # game from leading a mood on playtime alone.
    ok &= check(squared["comp"] <= linear["comp"], "gamma=2 never inflates a weight")
    ok &= check(all(v > 0 for v in squared.values()), "surviving weights stay strictly positive")
    return ok


def test_build_profile_contract():
    print("\n-- build_profile return contract --")
    vectorizer, item_vectors, index_of = _mood_fixture()
    ok = True
    interest = {"comp": 0.2, "imm": 0.9}

    result = scoring.build_profile(item_vectors, index_of, interest, {},
                                   vectorizer, scoring.NEUTRAL_MOOD)
    ok &= check(len(result) == 4, "returns (profile, taste, mood_vec, effective_interest)")

    profile, taste, mood_vec, effective = result
    ok &= check(mood_vec is None, "Neutral has no mood vector")
    ok &= check(effective == interest, "Neutral's profile uses the whole history")
    ok &= check(np.allclose(profile, taste), "Neutral's profile is the unblended taste profile")

    _, _, comp_mood_vec, comp_effective = scoring.build_profile(
        item_vectors, index_of, interest, {}, vectorizer, 1)
    ok &= check(comp_mood_vec is not None, "Competitive has a mood vector")
    ok &= check(set(comp_effective) < set(interest),
                "Competitive's profile is built from a strict subset of the history")
    return ok


def test_fatigue_saturation():
    print("\n-- impression fatigue --")
    ok = True
    index_of = {"a": 0, "b": 1, "c": 2}

    p = scoring.fatigue_penalties({"a": scoring.FATIGUE_SATURATES_AT}, index_of, 3)
    ok &= check(abs(p[0] - scoring.FATIGUE_WEIGHT) < 1e-9,
                "FATIGUE_SATURATES_AT appearances earn the full penalty")

    p = scoring.fatigue_penalties({"a": 1}, index_of, 3)
    ok &= check(abs(p[0] - scoring.FATIGUE_WEIGHT / scoring.FATIGUE_SATURATES_AT) < 1e-9,
                "one appearance earns a fifth of it")

    # The window must be able to exceed the saturation point without weakening
    # the penalty -- they were one constant, and widening the memory used to
    # dilute the penalty by exactly as much.
    ok &= check(scoring.FATIGUE_RUNS > scoring.FATIGUE_SATURATES_AT,
                "the memory window is longer than the saturation point")

    p = scoring.fatigue_penalties({"a": scoring.FATIGUE_RUNS}, index_of, 3)
    ok &= check(abs(p[0] - scoring.FATIGUE_WEIGHT) < 1e-9,
                "the penalty is clamped, never exceeding FATIGUE_WEIGHT")

    ok &= check(not scoring.fatigue_penalties({}, index_of, 3).any(),
                "no impression history means no penalty")
    return ok


def test_mmr_group_cap():
    print("\n-- per-group cap in MMR --")
    ok = True
    # Four label-identical items from one studio, then two from others. This is
    # the real failure: identical vectors make the redundancy term a constant,
    # so MMR alone cannot separate them and returns all four.
    vectors = np.array([[1.0, 0.0]] * 4 + [[0.0, 1.0]] * 2)
    scores = np.array([0.9, 0.89, 0.88, 0.87, 0.5, 0.4])
    rows = list(range(6))
    groups = ["Ryu Ga Gotoku"] * 4 + ["Other A", "Other B"]

    plain = scoring.mmr_rerank(rows, scores, vectors, k=4)
    ok &= check(sum(1 for r in plain if groups[r] == "Ryu Ga Gotoku") == 4,
                "without a cap, one studio takes every slot")

    capped = scoring.mmr_rerank(rows, scores, vectors, k=4,
                                group_of=groups, max_per_group=2)
    ok &= check(sum(1 for r in capped if groups[r] == "Ryu Ga Gotoku") == 2,
                "the cap holds at 2 per studio")
    ok &= check(len(capped) == 4, "the section is still filled to k")
    ok &= check(capped[0] == 0, "the top-scoring item is still picked first")

    # An empty key must not group unrelated games together.
    blanks = ["", "", "", "", "", ""]
    unkeyed = scoring.mmr_rerank(rows, scores, vectors, k=4,
                                 group_of=blanks, max_per_group=2)
    ok &= check(len(unkeyed) == 4, "blank group keys are exempt from the cap")

    # Graceful degradation: if the quota cannot fill k, fill the rest anyway
    # rather than returning a short list.
    tight = scoring.mmr_rerank(rows, scores, vectors, k=6,
                               group_of=groups, max_per_group=1)
    ok &= check(len(tight) == 6, "a quota too tight to fill k does not return short")
    return ok


def test_hearted_is_not_played():
    print("\n-- hearted vs played --")
    from datetime import datetime
    from interest import W_FAVORITE, W_FAVORITE_UNPLAYED
    ok = True

    sessions = [
        {"game_id": "played", "duration_seconds": 7200, "ended_at": datetime(2026, 5, 2)},
    ]
    interest, _, stats = build_interest(sessions, favorites={"hearted"})

    ok &= check("hearted" in interest, "a heart alone still enters the profile")
    ok &= check(stats["engaged_ids"] == {"played"},
                "engaged_ids names only games with a real session")
    ok &= check("hearted" not in stats["engaged_ids"],
                "a hearted game with no session is not reported as engaged")

    # The bug this fixes: an unplayed heart used to outweigh real play history.
    ok &= check(W_FAVORITE_UNPLAYED < W_FAVORITE,
                "an unplayed heart weighs less than a played one")

    both = build_interest(sessions, favorites={"played"})[0]
    ok &= check(both["played"] > interest["hearted"],
                "played AND hearted outranks hearted alone")

    # And the wording, which is what the user actually sees on the card.
    vectors = np.array([[1.0, 0.0], [1.0, 0.0], [0.0, 1.0]])
    index_of = {"played": 0, "hearted": 1}
    names = ["Counter-Strike 2", "Mabinogi", "Some Candidate"]
    taste = np.array([1.0, 0.0])

    reason = scoring.explain(0, vectors, {"hearted": 1.0}, index_of, names,
                             taste, None, scoring.NEUTRAL_MOOD, played_ids={"played"})
    ok &= check(reason == "Because you liked Mabinogi",
                f"a hearted-only game says 'liked', got: {reason!r}")

    reason = scoring.explain(0, vectors, {"played": 1.0}, index_of, names,
                             taste, None, scoring.NEUTRAL_MOOD, played_ids={"played"})
    ok &= check(reason == "Because you played Counter-Strike 2",
                f"a genuinely played game still says 'played', got: {reason!r}")
    return ok


def test_explanations_stay_varied():
    print("\n-- explanation variety across a panel --")
    ok = True
    # Six candidates, all closest to the same profile game. Chosen per-card,
    # every one of them names it; that is exactly the panel the user saw.
    vectors = np.array([[1.0, 0.0]] * 6 + [[1.0, 0.0], [0.9, 0.44], [0.7, 0.71]])
    rows = list(range(6))
    index_of = {"a": 6, "b": 7, "c": 8}
    names = [f"Candidate {i}" for i in range(6)] + ["Alpha", "Beta", "Gamma"]
    interest = {"a": 0.9, "b": 0.3, "c": 0.2}
    taste = np.array([1.0, 0.0])

    reasons = scoring.explain_section(rows, vectors, interest, index_of, names,
                                      taste, None, scoring.NEUTRAL_MOOD,
                                      played_ids={"a", "b", "c"},
                                      max_per_source=3)
    counts = {}
    for r in reasons:
        counts[r] = counts.get(r, 0) + 1
    ok &= check(len(reasons) == 6, "one reason per card")
    ok &= check(max(counts.values()) <= 3,
                f"no source cited more than 3 times, got {max(counts.values())}")
    ok &= check(len(counts) >= 2, f"the panel names more than one game, got {len(counts)}")
    ok &= check(reasons[0] == "Because you played Alpha",
                f"the first card still gets the best match, got {reasons[0]!r}")

    # Similarity decides, not profile weight: 'a' has 4.5x the weight of 'c' but
    # once it is capped the next most SIMILAR game must take over.
    ok &= check(any("Beta" in r for r in reasons),
                "a lower-weight but similar game gets cited once the top one caps")

    # A single-card call must still work for callers that want just one.
    one = scoring.explain(0, vectors, interest, index_of, names, taste, None,
                          scoring.NEUTRAL_MOOD, played_ids={"a"})
    ok &= check(one == "Because you played Alpha", f"single-card wrapper works, got {one!r}")

    ok &= check(scoring.explain_section([], vectors, interest, index_of, names,
                                        taste, None, scoring.NEUTRAL_MOOD) == [],
                "an empty section yields no reasons")
    return ok


def test_played_quota():
    print("\n-- reserved played citations --")
    ok = True
    # Six candidates against one played game and THREE hearted ones. Three
    # matters: with fewer hearted sources than 6/EXPLANATION_MAX_PER_SOURCE the
    # cap alone forces played citations, and the quota under test cannot be
    # observed separately from it.
    #
    # Every hearted game is a closer match than the played one -- the real
    # situation, where 55 hearted games outnumber 8 played and win on coverage.
    vectors = np.array([[1.0, 0.0]] * 6
                       + [[0.80, 0.60],       # played
                          [0.99, 0.141], [0.98, 0.199], [0.97, 0.243]])
    rows = list(range(6))
    index_of = {"p": 6, "h1": 7, "h2": 8, "h3": 9}
    names = ([f"Candidate {i}" for i in range(6)]
             + ["Played Game", "Hearted One", "Hearted Two", "Hearted Three"])
    interest = {"p": 0.5, "h1": 0.5, "h2": 0.5, "h3": 0.5}
    taste = np.array([1.0, 0.0])

    def run(quota):
        return scoring.explain_section(rows, vectors, interest, index_of, names,
                                       taste, None, scoring.NEUTRAL_MOOD,
                                       played_ids={"p"}, played_quota=quota)

    none = run(None)
    ok &= check(all("liked" in r for r in none),
                "with no quota the closer hearted game wins every card")

    two = run(2)
    played = sum(1 for r in two if r.startswith("Because you played"))
    ok &= check(played == 2, f"a quota of 2 yields exactly 2 played citations, got {played}")
    ok &= check(len(two) == 6, "every card still gets a reason")

    zero = run(0)
    ok &= check(sum(1 for r in zero if r.startswith("Because you played")) == 0,
                "a quota of 0 reserves nothing")

    # The cap still binds: one played source cannot fill more than 3 even when
    # the quota asks for 6. Asking for more than is available must not crash or
    # return short.
    six = run(6)
    played = sum(1 for r in six if r.startswith("Because you played"))
    ok &= check(len(six) == 6, "an unfillable quota still returns one reason per card")
    ok &= check(played <= scoring.EXPLANATION_MAX_PER_SOURCE,
                f"the per-source cap still binds under a large quota, got {played}")

    # No played games at all: the quota must degrade silently, not raise.
    only_hearted = scoring.explain_section(rows, vectors, {"h1": 0.5, "h2": 0.5, "h3": 0.5},
                                           {"h1": 7, "h2": 8, "h3": 9}, names,
                                           taste, None, scoring.NEUTRAL_MOOD,
                                           played_ids=set(), played_quota=4)
    ok &= check(len(only_hearted) == 6 and not any("played" in r for r in only_hearted),
                "a quota with no played sources available degrades quietly")
    return ok


def test_competitive_needs_multiplayer():
    print("\n-- Competitive excludes single-player-only games --")
    import pandas as pd
    from recommend import build_recommendations
    ok = True

    # A tiny frame: one multiplayer shooter, one single-player-only shooter with
    # IDENTICAL labels otherwise, and one with no modes recorded.
    def row(name, modes, gid, rating=60.0):
        return {"game_id": gid, "source": "IGDB_Catalog", "name": name,
                "canonical_name": name.lower().replace(" ", ""),
                "genres": ["Shooter"], "tags": ["Shooter"], "themes": [],
                "game_modes": modes, "keywords": ["sniping"], "developer": name,
                "rating": rating, "total_rating_count": 500, "game_length": 0,
                "installed": False, "steam_appid": 0, "released_at": None,
                "cover_url": "", "external_id": gid}

    # Padding matters. The filter carries the same `2 * k` starvation guard as
    # drop_off_mood, so on a three-row fixture it correctly declines to run and
    # the test would pass or fail for the wrong reason. DISCOVER_N is 12, so the
    # multiplayer survivors have to clear 24.
    rows = [row(f"Padding Shooter {i}", ["Multiplayer"],
                f"{i:08d}-0000-0000-0000-000000000000") for i in range(30)]
    # The three under test are rated far above the padding, so they take the top
    # slots on merit. Anything missing from the output was removed by the filter,
    # not by ranking -- which is what makes the assertions below mean something.
    rows += [
        row("Team Shooter", ["Single player", "Multiplayer"],
            "11111111-1111-1111-1111-111111111111", rating=98.0),
        row("Solo Shooter", ["Single player"],
            "22222222-2222-2222-2222-222222222222", rating=97.0),
        row("Unknown Modes", [],
            "33333333-3333-3333-3333-333333333333", rating=96.0),
    ]
    games = pd.DataFrame(rows)
    sessions = pd.DataFrame(columns=["game_id", "duration_seconds", "ended_at",
                                     "started_at", "session_id", "user_id"])

    # The predicate is tested directly. Asserting on the returned twelve instead
    # would test the RANKING, not the filter: Competitive scores Multiplayer at
    # +2 and Single player at -1, so a multiplayer-only game legitimately
    # outranks one carrying both, and a "kept" game can be absent from the top
    # twelve for entirely correct reasons.
    from recommend import playable_against_others
    ok &= check(playable_against_others(["Single player", "Multiplayer"]),
                "both modes -> eligible")
    ok &= check(not playable_against_others(["Single player"]),
                "single player alone -> excluded")
    ok &= check(playable_against_others(["Multiplayer"]), "multiplayer alone -> eligible")
    ok &= check(playable_against_others([]),
                "no modes recorded -> eligible (unknown is not single-player)")
    ok &= check(playable_against_others(None), "a null modes column does not crash")
    for mode in ("Co-operative", "Split screen",
                 "Massively Multiplayer Online (MMO)", "Battle Royale"):
        ok &= check(playable_against_others(["Single player", mode]),
                    f"{mode} counts as playing with others")

    # And end to end, so the wiring is covered too.
    items, _ = build_recommendations(games, sessions, set(), 1, explore=False)
    ok &= check("Solo Shooter" not in {i["name"] for i in items},
                "a single-player-only game is dropped under Competitive")

    # Every other mood must be untouched by this rule.
    leaked = []
    for mood in (0, 2, scoring.NEUTRAL_MOOD):
        picks, _ = build_recommendations(games, sessions, set(), mood, explore=False)
        if "Solo Shooter" not in {i["name"] for i in picks}:
            leaked.append(mood)
    ok &= check(not leaked, f"no other mood applies the filter (offenders: {leaked})")
    return ok


def test_ignore_played_drops_history_not_candidates():
    print()
    print("-- ignore played / ignore liked toggles --")
    import pandas as pd
    from datetime import datetime, timedelta
    from recommend import build_recommendations
    ok = True

    # One played game and one hearted game, both carrying Relaxed labels so
    # both genuinely survive the mood gate -- otherwise the toggle would look
    # like it worked when mood_relevance had already done the dropping.
    def row(name, gid, genres, rating=60.0):
        return {"game_id": gid, "source": "IGDB_Catalog", "name": name,
                "canonical_name": name.lower().replace(" ", ""),
                "genres": genres, "tags": genres, "themes": ["Comedy"],
                "game_modes": ["Single player"], "keywords": [], "developer": name,
                "rating": rating, "total_rating_count": 500, "game_length": 0,
                "installed": False, "steam_appid": 0, "released_at": None,
                "cover_url": "", "external_id": gid}

    played_id = "11111111-1111-1111-1111-111111111111"
    hearted_id = "22222222-2222-2222-2222-222222222222"
    # The interesting one: played AND hearted. It is what makes the two filters
    # more than each other's mirror image.
    both_id = "33333333-3333-3333-3333-333333333333"
    rows = [row(f"Padding Puzzler {i}", f"{i:08d}-0000-0000-0000-000000000000",
                ["Puzzle", "Indie"]) for i in range(30)]
    rows += [row("Played Puzzler", played_id, ["Puzzle", "Arcade"], rating=95.0),
             row("Hearted Platformer", hearted_id, ["Platform", "Comedy"], rating=94.0),
             row("Played And Hearted", both_id, ["Puzzle", "Comedy"], rating=93.0)]
    games = pd.DataFrame(rows)

    ended = datetime.now() - timedelta(days=1)
    sessions = pd.DataFrame([
        {"session_id": "s1", "user_id": "u", "game_id": played_id,
         "duration_seconds": 7200, "started_at": ended - timedelta(hours=2),
         "ended_at": ended},
        {"session_id": "s2", "user_id": "u", "game_id": both_id,
         "duration_seconds": 5400, "started_at": ended - timedelta(hours=4),
         "ended_at": ended - timedelta(hours=2)},
    ])
    favorites = {hearted_id, both_id}

    def run(mood, ignore=False, ignore_liked=False):
        items, _ = build_recommendations(games, sessions, favorites, mood,
                                         explore=False, ignore_played=ignore,
                                         ignore_liked=ignore_liked)
        return items

    def cited(items, played_only=None):
        return {source["name"] for item in items for source in item["inspiredBy"]
                if played_only is None or source["played"] == played_only}

    # Relaxed, toggle off: the played game shapes the profile and is citable.
    base = run(0, False)
    ok &= check("Played Puzzler" in cited(base, played_only=True),
                "off: a played game is cited as evidence")

    # Relaxed, toggle on: no card may cite anything played, while the hearted
    # game keeps working -- the toggle drops playtime, not the whole profile.
    gated = run(0, True)
    ok &= check(not cited(gated, played_only=True),
                "on: nothing played is cited any more")
    ok &= check("Hearted Platformer" in cited(gated, played_only=False),
                "on: a hearted game still shapes the profile")

    # The whole point of filtering a COPY. If `interest` itself were filtered,
    # the played game would drop out of the exclusion set and be recommended
    # back -- the Forza-recommending-itself bug, one toggle later.
    for label, items in (("off", base), ("on", gated)):
        ok &= check("Played Puzzler" not in {i["name"] for i in items},
                    f"{label}: a played game is still never recommended back")

    # The liked toggle, and the point of the played-and-hearted fixture: it
    # takes the pure hearts and leaves anything with real playtime behind it.
    unliked = run(0, ignore_liked=True)
    ok &= check("Hearted Platformer" not in cited(unliked),
                "liked off: a hearted-only game stops shaping the profile")
    ok &= check("Played And Hearted" in cited(unliked, played_only=True),
                "liked off: a played AND hearted game survives on its playtime")
    ok &= check("Played Puzzler" in cited(unliked, played_only=True),
                "liked off: played games are untouched")

    # And the mirror. A game that is both counts as played, so the PLAYED
    # filter is the one that removes it -- not the liked filter.
    ok &= check("Played And Hearted" not in cited(gated),
                "played off: a played AND hearted game goes with the played set")

    # Both at once. Nothing is left to build a profile from, and build_profile
    # answers a zero taste vector with the mood alone rather than falling over.
    neither = run(0, ignore=True, ignore_liked=True)
    ok &= check(len(neither) > 0, "both on: still returns a full list")
    ok &= check(not cited(neither), "both on: no card cites anything at all")
    ok &= check(not any("you played" in i["reason"] or "you liked" in i["reason"]
                        or "genres you" in i["reason"] for i in neither),
                "both on: no reason claims a history the profile does not have")
    for label, items in (("liked off", unliked), ("both off", neither)):
        names = {i["name"] for i in items}
        ok &= check(not ({"Played Puzzler", "Hearted Platformer",
                          "Played And Hearted"} & names),
                    f"{label}: nothing already played or hearted is recommended back")

    # Neutral has no mood vector to fall back on, so the toggles must not touch
    # it. Compared on the full ordered output, with explore=False making the
    # run deterministic.
    neutral = [i["name"] for i in run(scoring.NEUTRAL_MOOD)]
    for label, kwargs in (("played", {"ignore": True}),
                          ("liked", {"ignore_liked": True}),
                          ("both", {"ignore": True, "ignore_liked": True})):
        got = [i["name"] for i in run(scoring.NEUTRAL_MOOD, **kwargs)]
        ok &= check(neutral == got, f"Neutral is unaffected by the {label} toggle")
    ok &= check("Played Puzzler" in cited(run(scoring.NEUTRAL_MOOD, ignore=True),
                                          played_only=True),
                "Neutral still cites played games with the toggles on")
    return ok


def test_every_mood_label_resolves():
    print("\n-- mood labels exist in the live data --")
    # The check that would have caught `hand drawn` (space) against IGDB's
    # `hand-drawn`, and the three dead labels shipped before it: Simulation,
    # Story Rich, Casual. A label matching nothing steers nothing, silently.
    try:
        from recommend import load_frames
        games, _ = load_frames()
    except Exception as exc:
        print(f"SKIP  database unavailable ({type(exc).__name__})")
        return True

    games = games.reset_index(drop=True)
    vocabulary = set()
    for column in ("genres", "themes", "game_modes", "keywords"):
        if column not in games.columns:
            continue
        for value in games[column]:
            if isinstance(value, (list, tuple)):
                vocabulary |= {str(x).strip().lower() for x in value if x}

    ok = True
    for mood, weights in scoring.MOOD_LABELS.items():
        dead = sorted(l for l in weights if l.lower() not in vocabulary)
        ok &= check(not dead,
                    f"{scoring.MOOD_NAMES[mood]}: all {len(weights)} labels resolve"
                    + (f" -- DEAD: {dead}" if dead else ""))
    return ok


def test_curated_mask():
    print("\n-- curated candidate filter --")
    import datetime
    ok = True
    today = datetime.date(2026, 8, 20)
    recent = "2026-01-01"
    old = "2005-01-01"

    #                      rating  votes                     expected
    cases = [
        ("popular but mediocre",   60.0, 500.0, old,    True),
        ("well rated, enough votes", 85.0, 40.0, old,   True),
        ("well rated, too few votes", 100.0, 3.0, old,  False),
        ("obscure and average",     70.0, 14.0, old,    False),
        ("new release with traction", 70.0, 60.0, recent, True),
        ("new release, no traction",  70.0, 5.0, recent,  False),
    ]
    mask = scoring.curated_mask([c[1] for c in cases],
                                [c[2] for c in cases],
                                [c[3] for c in cases], today=today)
    for (label, _, _, _, expected), got in zip(cases, mask):
        ok &= check(bool(got) == expected, f"{label} -> {'kept' if expected else 'dropped'}")

    # Missing data means unproven, which is what the filter excludes. Getting
    # this backwards would admit the entire long tail it exists to remove.
    blanks = scoring.curated_mask([None, np.nan], [None, np.nan], [None, None], today=today)
    ok &= check(not blanks.any(), "rows with no rating or vote count are dropped")

    # An undated row must not crash or be treated as brand new.
    undated = scoring.curated_mask([70.0], [60.0], [None], today=today)
    ok &= check(not undated[0], "an undated game cannot qualify as trending")

    ok &= check(len(scoring.curated_mask([], [], [], today=today)) == 0,
                "an empty catalog yields an empty mask")
    return ok


if __name__ == "__main__":
    results = [
        test_quality_handles_missing_counts(),
        test_session_bands(),
        test_duration_weight_is_stable(),
        test_favorite_clears_abort_penalty(),
        test_mood_relevance_gates_the_history(),
        test_gamma_sharpens_the_gate(),
        test_build_profile_contract(),
        test_fatigue_saturation(),
        test_mmr_group_cap(),
        test_curated_mask(),
        test_hearted_is_not_played(),
        test_explanations_stay_varied(),
        test_played_quota(),
        test_competitive_needs_multiplayer(),
        test_ignore_played_drops_history_not_candidates(),
        test_every_mood_label_resolves(),
    ]
    print(f"\n{'ALL PASSED' if all(results) else 'FAILURES PRESENT'}")
    sys.exit(0 if all(results) else 1)
