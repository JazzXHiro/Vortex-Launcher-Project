"""Offline evaluation of the recommender.

    python evaluate.py                # full report
    python evaluate.py --sweeps       # also run the parameter sweeps

Read the honesty note below before quoting any number from this.

WHAT THIS CAN AND CANNOT SHOW
-----------------------------
This user has one profile, five positively-engaged games and thirty raw
sessions. At that size **no accuracy difference between two models can be
statistically significant.** Precision@K, NDCG and MRR are reported with
bootstrap confidence intervals precisely so the reader can see how wide they
are -- they are there to demonstrate correctness, direction and
reproducibility, not superiority.

The mood-sensitivity metrics are the exception and are worth quoting. Inter-mood
overlap and mood-label precision are computed over full ranked lists and need
no held-out labels, so they are genuinely measurable at this data size.
"""

import argparse
import itertools
import math
import os
import sys
from collections import defaultdict

import numpy as np
import pandas as pd

import model
import scoring
from baselines.legacy_score import LEGACY_MOOD_GENRES, legacy_scores
from interest import build_interest
from recommend import (DISCOVER_N, LIBRARY_N, TEMPERATURE_BASIS_N,
                       load_frames, load_favorites)

K_VALUES = (5, 10)

# Driven off scoring.MOOD_LABELS so a new mood needs no edit here. Neutral has
# no labels by design, so it takes part in overlap (as the no-mood baseline) but
# not in any metric that asks "how many picks carry a label of this mood".
MOODS = sorted(scoring.MOOD_LABELS)
LABELLED_MOODS = [m for m in MOODS if scoring.MOOD_LABELS[m]]
BOOTSTRAP_ROUNDS = 1000
RNG = np.random.default_rng(20260816)


# --------------------------------------------------------------------------
# Metrics
# --------------------------------------------------------------------------

def dcg(relevances):
    return sum(rel / math.log2(i + 2) for i, rel in enumerate(relevances))


def ndcg_at_k(ranked_ids, relevant, k):
    gains = [1.0 if gid in relevant else 0.0 for gid in ranked_ids[:k]]
    ideal = sorted([1.0] * min(len(relevant), k), reverse=True)
    denom = dcg(ideal)
    return dcg(gains) / denom if denom > 0 else 0.0


def precision_at_k(ranked_ids, relevant, k):
    if k == 0:
        return 0.0
    return sum(1 for gid in ranked_ids[:k] if gid in relevant) / k


def recall_at_k(ranked_ids, relevant, k):
    if not relevant:
        return 0.0
    return sum(1 for gid in ranked_ids[:k] if gid in relevant) / len(relevant)


def reciprocal_rank(ranked_ids, relevant):
    for i, gid in enumerate(ranked_ids):
        if gid in relevant:
            return 1.0 / (i + 1)
    return 0.0


def percentile_rank(ranked_ids, target):
    """0.0 = ranked first, 1.0 = ranked last. Interpretable at n = 5."""
    if target not in ranked_ids or len(ranked_ids) <= 1:
        return 1.0
    return ranked_ids.index(target) / (len(ranked_ids) - 1)


def intra_list_diversity(rows, item_vectors):
    if len(rows) < 2:
        return 0.0
    sims = []
    for i in range(len(rows)):
        for j in range(i + 1, len(rows)):
            a, b = item_vectors[rows[i]], item_vectors[rows[j]]
            denom = np.linalg.norm(a) * np.linalg.norm(b)
            sims.append(float(np.dot(a, b) / denom) if denom > 0 else 0.0)
    return 1.0 - float(np.mean(sims))


def bootstrap_ci(values, rounds=BOOTSTRAP_ROUNDS):
    if not values:
        return (0.0, 0.0, 0.0)
    arr = np.asarray(values, dtype=float)
    means = [RNG.choice(arr, size=len(arr), replace=True).mean() for _ in range(rounds)]
    return float(arr.mean()), float(np.percentile(means, 2.5)), float(np.percentile(means, 97.5))


# --------------------------------------------------------------------------
# Harness
# --------------------------------------------------------------------------

class Corpus:
    def __init__(self):
        self.games, self.sessions = load_frames()

        # Observed sessions only. sync_local_data.py derives sessions from
        # Steam's lifetime totals so the recommender has something to learn
        # from when a user plays outside Vortex -- but those rows are inferred,
        # and every number this file reports is an accuracy claim about real
        # behaviour. Scoring the recommender against history it invented would
        # measure nothing.
        if "synthetic" in self.sessions.columns:
            derived = int((self.sessions["synthetic"] == 1).sum())
            if derived:
                print(f"[eval] excluding {derived} Steam-derived sessions; "
                      f"metrics below use observed play only.")
            self.sessions = self.sessions[self.sessions["synthetic"] != 1]

        self.games = self.games.reset_index(drop=True)
        self.favorites = load_favorites()

        self.names = self.games["name"].tolist()
        self.ids = self.games["game_id"].astype(str).tolist()
        self.index_of = {gid: i for i, gid in enumerate(self.ids)}

        documents = model.build_documents(self.games)
        vectorizer = model.fit_vectorizer(documents)
        self.vectorizer = vectorizer
        self.item_vectors = vectorizer.transform(documents).toarray()

        counts = (self.games["total_rating_count"].tolist()
                  if "total_rating_count" in self.games.columns else None)
        self.quality = scoring.quality_scores(self.games["rating"].tolist(), counts)

        self.lengths = {}
        if "game_length" in self.games.columns:
            self.lengths = {self.ids[i]: (self.games["game_length"].iloc[i] or 0)
                            for i in range(len(self.games))}

        self.records = [
            {"game_id": str(r.game_id),
             "duration_seconds": int(r.duration_seconds or 0),
             "ended_at": r.ended_at}
            for r in self.sessions.itertuples() if r.ended_at is not None
        ]

        interest, _, stats = build_interest(self.records, self.favorites, self.lengths)
        # Everything in the profile: played games AND hearted ones. The name
        # matters -- these are NOT all "engaged" games. On the live profile 55
        # of 63 have never been launched, so reporting this count as a sample of
        # engaged games overstates the evidence eightfold, in the same report
        # that warns against overstating it.
        self.profile_games = list(interest)
        self.engaged_ids = set(stats.get("engaged_ids", ()))
        self.stats = stats

        # Every label class a mood can name. Game modes were added to
        # MOOD_LABELS but not here, so the coverage report called `Single
        # player`, `Multiplayer` and `Battle Royale` dead labels that steer
        # nothing -- while they were in fact steering the moods. A harness that
        # reports a working feature as broken is worse than no harness.
        self.labels = []
        for i in range(len(self.games)):
            collected = []
            for column in ("genres", "themes", "game_modes", "keywords"):
                if column in self.games.columns:
                    collected += list(self.games[column].iloc[i] or [])
            self.labels.append({str(x).lower() for x in collected})

    def rank_new_model(self, records, favorites, mood, exclude=()):
        interest, disinterest, _ = build_interest(records, favorites, self.lengths)
        profile, _, _, _ = scoring.build_profile(
            self.item_vectors, self.index_of, interest, disinterest, self.vectorizer, mood)
        scores, _ = scoring.score_games(profile, self.item_vectors, self.quality)
        return self._order(scores, set(exclude) | set(disinterest))

    def _order(self, scores, excluded):
        rows = [i for i in range(len(self.games)) if self.ids[i] not in excluded]
        rows.sort(key=lambda r: -scores[r])
        return rows


# --------------------------------------------------------------------------
# Protocols
# --------------------------------------------------------------------------

def leave_one_game_out(corpus, mood=1, owned_only=True):
    """Hide one engaged game and see whether the model recovers it.

    owned_only restricts the candidate pool to the user's library, and is the
    meaningful setting. The held-out item is always a game the user owns and
    played, so scoring it against ~5,800 unowned catalog titles asks a question
    the recommender is not answering -- every model scores 0 at K=10 simply
    because one relevant item among thousands cannot land in a top ten. Run
    with owned_only=False to see that degenerate case for yourself.
    """
    results = defaultdict(list)

    catalog_ids = set()
    if owned_only and "source" in corpus.games.columns:
        catalog_ids = {corpus.ids[i] for i in range(len(corpus.games))
                       if corpus.games["source"].iloc[i] == "IGDB_Catalog"}

    for held in corpus.profile_games:
        kept = [r for r in corpus.records if r["game_id"] != held]
        # Other engaged games are removed so the task is "recover the held-out
        # game", not "recover any game they played".
        others = (set(corpus.profile_games) - {held}) | catalog_ids
        relevant = {held}

        rankings = {
            "new model": corpus.rank_new_model(
                kept, corpus.favorites - {held}, mood, exclude=others),
            "legacy (frozen)": _legacy_ranking(corpus, kept, others),
            "most popular": _popular_ranking(corpus, others),
            "rating only": _rating_ranking(corpus, others),
            "random": _random_ranking(corpus, others),
        }

        for label, rows in rankings.items():
            ids = [corpus.ids[r] for r in rows]
            results[label].append({
                "mrr": reciprocal_rank(ids, relevant),
                "pct": percentile_rank(ids, held),
                **{f"p@{k}": precision_at_k(ids, relevant, k) for k in K_VALUES},
                **{f"ndcg@{k}": ndcg_at_k(ids, relevant, k) for k in K_VALUES},
            })
    return results


def _legacy_ranking(corpus, records, exclude):
    frame = pd.DataFrame(records)
    if frame.empty:
        return list(range(len(corpus.games)))
    frame = frame.rename(columns={"duration_seconds": "duration_seconds"})
    scores, played = legacy_scores(corpus.games, frame, mood=1)
    return corpus._order(scores, set(exclude) | played)


def _popular_ranking(corpus, exclude):
    counts = (corpus.games["total_rating_count"].fillna(0).to_numpy()
              if "total_rating_count" in corpus.games.columns
              else np.zeros(len(corpus.games)))
    return corpus._order(counts, set(exclude))


def _rating_ranking(corpus, exclude):
    return corpus._order(corpus.quality, set(exclude))


def _random_ranking(corpus, exclude):
    return corpus._order(RNG.random(len(corpus.games)), set(exclude))


# --------------------------------------------------------------------------
# Mood sensitivity -- no held-out labels needed, so genuinely measurable here
# --------------------------------------------------------------------------

def mood_sensitivity(corpus, mu=None):
    original = scoring.MOOD_BLEND
    if mu is not None:
        scoring.MOOD_BLEND = mu

    tops, label_hits = {}, {}
    try:
        for mood in MOODS:
            rows = corpus.rank_new_model(corpus.records, corpus.favorites, mood,
                                         exclude=corpus.profile_games)[:10]
            tops[mood] = rows
            if mood in LABELLED_MOODS:
                # Positive labels only: a negatively weighted label means the
                # opposite of a match, so counting it would invert the metric.
                wanted = {m.lower() for m in scoring.mood_positive_labels(mood)}
                label_hits[mood] = sum(1 for r in rows if corpus.labels[r] & wanted) / max(1, len(rows))
    finally:
        scoring.MOOD_BLEND = original

    # Neutral is included here on purpose: overlap against it measures how far
    # each mood actually pulls away from pure play history, which is the
    # reference line the others should be read against.
    overlaps = {}
    for a, b in itertools.combinations(MOODS, 2):
        sa, sb = set(tops[a]), set(tops[b])
        overlaps[(a, b)] = len(sa & sb) / len(sa | sb) if sa | sb else 1.0

    return overlaps, label_hits, tops


def vocabulary_coverage(corpus, label_sets, substring=False):
    """How many of each mood's configured labels exist in the data at all.

    This is the direct measure of the mood bug. A label that matches nothing
    cannot steer anything, so a mood built mostly from such labels silently
    collapses to whichever one or two labels are real.
    """
    universe = set()
    for labels in corpus.labels:
        universe |= labels

    out = {}
    for mood, labels in label_sets.items():
        live = []
        for label in labels:
            needle = label.lower()
            hit = (any(needle in known for known in universe) if substring
                   else needle in universe)
            if hit:
                live.append(label)
        out[mood] = (live, [l for l in labels if l not in live])
    return out


# --------------------------------------------------------------------------
# Report
# --------------------------------------------------------------------------

def header(title):
    print("\n" + "=" * 74)
    print(title)
    print("=" * 74)


def report(sweeps=False):
    corpus = Corpus()

    header("CORPUS")
    print(f"  games in catalog        {len(corpus.games)}")
    print(f"  owned                   {int((corpus.games['source'] != 'IGDB_Catalog').sum())}")
    print(f"  raw sessions            {len(corpus.records)}")
    print(f"  engaged sessions        {corpus.stats.get('engaged_sessions', 0)}")
    print(f"  engaged games (n)       {corpus.stats.get('engaged_games', 0)}")
    print(f"  aborted sessions        {corpus.stats.get('abort_sessions', 0)}")
    print(f"  favourites              {len(corpus.favorites)}")
    print(f"\n  n = {len(corpus.profile_games)} is the sample size for every accuracy number below.")

    header("LEAVE-ONE-GAME-OUT  (library pool; mean [95% bootstrap CI])")
    print("  Candidates are restricted to the owned library. The held-out item is")
    print("  always a game the user owns and played, so ranking it against ~5,800")
    print("  unowned catalog titles measures nothing -- every model scores 0 at")
    print("  K=10 because one relevant item cannot surface from thousands.\n")
    results = leave_one_game_out(corpus)
    metrics = ["mrr", "pct", "p@10", "ndcg@10"]
    print(f"  {'model':18} " + " ".join(f"{m:>22}" for m in metrics))
    for label in ["new model", "legacy (frozen)", "rating only", "most popular", "random"]:
        cells = []
        for metric in metrics:
            mean, lo, hi = bootstrap_ci([r[metric] for r in results[label]])
            cells.append(f"{mean:.3f} [{lo:.2f},{hi:.2f}]".rjust(22))
        print(f"  {label:18} " + " ".join(cells))
    print("\n  pct = mean percentile rank of the held-out game (lower is better).")
    print("  Those intervals overlap heavily. That is the honest result at n = "
          f"{len(corpus.profile_games)}, not a")
    print("  formatting artefact -- do not report these as evidence one model beats another.")

    header("MOOD SENSITIVITY  (no held-out labels needed -- measurable at this size)")
    overlaps, label_hits, tops = mood_sensitivity(corpus)

    print("  Inter-mood top-10 overlap (Jaccard). 1.00 = the picker does nothing:")
    for (a, b), value in overlaps.items():
        print(f"    {scoring.MOOD_NAMES[a]:12} vs {scoring.MOOD_NAMES[b]:12} {value:.2f}")

    print("\n  Mood-label precision -- fraction of the top 10 carrying a label of")
    print("  the selected mood:")
    for mood in LABELLED_MOODS:
        print(f"    mood {mood} {scoring.MOOD_NAMES[mood]:14} {label_hits[mood]:.2f}")
    print("    (Neutral is absent by design -- it has no labels to match.)")

    print("\n  Vocabulary coverage -- labels that match nothing in the data cannot")
    print("  steer anything, so a mood built from them silently collapses:")
    legacy_cov = vocabulary_coverage(corpus, LEGACY_MOOD_GENRES, substring=True)
    new_cov = vocabulary_coverage(
        corpus, {m: scoring.mood_positive_labels(m) for m in LABELLED_MOODS},
        substring=False)
    for mood in sorted(LEGACY_MOOD_GENRES):
        live, dead = legacy_cov[mood]
        print(f"    legacy mood {mood}: {len(live)}/{len(live) + len(dead)} usable"
              f"   live={live}  dead={dead}")
    for mood in LABELLED_MOODS:
        live, dead = new_cov[mood]
        print(f"    new    mood {mood}: {len(live)}/{len(live) + len(dead)} usable"
              + (f"   dead={dead}" if dead else ""))

    header("DIVERSITY AND COVERAGE")
    seen = set()
    for mood in MOODS:
        rows = corpus.rank_new_model(corpus.records, corpus.favorites, mood,
                                     exclude=corpus.profile_games)
        selected = scoring.mmr_rerank(rows, _scores_for(corpus, mood), corpus.item_vectors, k=10)
        plain = rows[:10]
        seen.update(corpus.names[r] for r in selected)
        print(f"  mood {mood} intra-list diversity   "
              f"MMR {intra_list_diversity(selected, corpus.item_vectors):.3f}   "
              f"vs plain top-10 {intra_list_diversity(plain, corpus.item_vectors):.3f}")
    print(f"  catalog coverage across moods: {len(seen)} distinct titles in {len(MOODS) * 10} slots")

    header("COLD START")
    for mood in MOODS:
        rows = corpus.rank_new_model([], set(), mood)
        print(f"  mood {mood}: {len(rows[:10])} picks with no play history at all "
              f"(old model returned 0 -- the tab was empty on first run)")

    if sweeps:
        header("PARAMETER SWEEPS")
        print("  mu (mood blend) vs inter-mood overlap:")
        for mu in (0.0, 0.15, 0.30, 0.50, 0.75, 1.0):
            ov, hits, _ = mood_sensitivity(corpus, mu=mu)
            mean_ov = float(np.mean(list(ov.values())))
            mean_hit = float(np.mean(list(hits.values())))
            marker = "  <- current" if abs(mu - 0.30) < 1e-9 else ""
            print(f"    mu={mu:.2f}   overlap {mean_ov:.2f}   mood-label precision {mean_hit:.2f}{marker}")

        print(f"\n  exploration alpha vs refresh churn and quality retention:")
        print(f"    (churn = how many of the section's slots change between refreshes,")
        print(f"     library={LIBRARY_N} discover={DISCOVER_N}; quality = mean score")
        print(f"     kept vs that section's deterministic top picks)")
        base = _scores_for(corpus, 1)
        owned = (corpus.games["source"].isin(["Local", "Steam"]).to_numpy()
                 if "source" in corpus.games.columns else np.ones(len(corpus.games), bool))
        pools = {
            "library": [i for i in range(len(corpus.games))
                        if corpus.ids[i] not in corpus.profile_games and owned[i]
                        and np.linalg.norm(corpus.item_vectors[i]) > 0],
            "discover": [i for i in range(len(corpus.games))
                         if corpus.ids[i] not in corpus.profile_games and not owned[i]
                         and np.linalg.norm(corpus.item_vectors[i]) > 0],
        }
        # Use the real per-section slot counts so these numbers describe the
        # list that actually ships, not a uniform 10.
        slots = {"library": LIBRARY_N, "discover": DISCOVER_N}
        for alpha in (0.0, 0.10, 0.25, 0.40, 0.60):
            cells = []
            for label, rows in pools.items():
                if len(rows) < 2:
                    cells.append(f"{label} n/a")
                    continue
                arr = np.asarray(rows)
                n = slots[label]
                # Temperature basis stays at TEMPERATURE_BASIS_N regardless of
                # slot count, matching what recommend.py does — otherwise a
                # wider section would silently get hotter exploration.
                temp = scoring.section_temperature(
                    base, rows, k=TEMPERATURE_BASIS_N, alpha=alpha)
                det = arr[np.argsort(-base[arr])[:n]]
                det_mean = base[det].mean()
                churn, keep = [], []
                for trial in range(30):
                    a = arr[np.argsort(-scoring.gumbel_perturb(
                        base, np.random.default_rng(trial), temp)[arr])[:n]]
                    b = arr[np.argsort(-scoring.gumbel_perturb(
                        base, np.random.default_rng(500 + trial), temp)[arr])[:n]]
                    churn.append(n - len(set(a) & set(b)))
                    keep.append(base[a].mean() / det_mean)
                cells.append(f"{label} {np.mean(churn):.1f}/{n} @ {100*np.mean(keep):.0f}%")
            marker = "  <- current" if abs(alpha - scoring.EXPLORE_ALPHA) < 1e-9 else ""
            print(f"    alpha={alpha:.2f}   " + "   ".join(cells) + marker)

        print("\n  lambda (MMR) vs diversity:")
        base_scores = _scores_for(corpus, 1)
        rows = corpus.rank_new_model(corpus.records, corpus.favorites, 1, exclude=corpus.profile_games)
        for lam in (0.5, 0.65, 0.75, 0.9, 1.0):
            selected = scoring.mmr_rerank(rows, base_scores, corpus.item_vectors, k=10, lam=lam)
            marker = "  <- current" if abs(lam - 0.75) < 1e-9 else ""
            print(f"    lambda={lam:.2f}  intra-list diversity "
                  f"{intra_list_diversity(selected, corpus.item_vectors):.3f}"
                  f"   mean score {np.mean([base_scores[r] for r in selected]):.3f}{marker}")

    header("READ THIS BEFORE QUOTING ANY NUMBER")
    print(f"  With one user, {len(corpus.engaged_ids)} played games "
          f"({len(corpus.profile_games)} counting hearted ones) and "
          f"{len(corpus.records)} sessions,")
    print("  no accuracy difference above is statistically significant. This harness")
    print("  validates implementation correctness, directional behaviour and")
    print("  reproducibility -- not superiority.")
    print()
    print("  The mood-sensitivity section is the exception: it needs no held-out")
    print("  labels and is measured over full ranked lists, so it is the strongest")
    print("  quantitative claim this dataset supports.")


def _scores_for(corpus, mood):
    interest, disinterest, _ = build_interest(corpus.records, corpus.favorites, corpus.lengths)
    profile, _, _, _ = scoring.build_profile(
        corpus.item_vectors, corpus.index_of, interest, disinterest, corpus.vectorizer, mood)
    scores, _ = scoring.score_games(profile, corpus.item_vectors, corpus.quality)
    return scores


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sweeps", action="store_true", help="also run parameter sweeps")
    args = parser.parse_args()
    sys.exit(report(sweeps=args.sweeps) or 0)
