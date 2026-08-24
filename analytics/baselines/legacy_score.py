"""The original scoring model, frozen for comparison.

This reproduces recommend.py as it was before the overhaul, so the before/after
stays reproducible after the real implementation was rewritten. Do not "fix"
anything in here -- the bugs are the point.

Reproduced faithfully:

  * `rating / 10.0` applied to a 0-100 IGDB rating, so the rating term spans
    0-1.84 while cosine similarity spans 0-0.8. Rating has ~2.3x the dynamic
    range of the personalisation signal and dominates the ranking.
  * A flat +0.15 mood bonus on any substring match, over a genre vocabulary
    that largely does not exist in the data ("Simulation" when IGDB says
    "Simulator"; Sandbox/Horror are themes, which were never fetched).
  * Default TF-IDF tokenisation, which shatters "Role-playing (RPG)" into
    "role", "playing", "rpg".
  * interest = 0.5*activity_ratio + 0.3*(duration/max) + 0.2*(count/max),
    where activity_ratio was synthesised from the like button.
"""

import numpy as np
from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.metrics.pairwise import cosine_similarity

LEGACY_MOOD_GENRES = {
    0: ["Sandbox", "Adventure", "Simulation"],
    1: ["Shooter", "Strategy", "Fighting"],
    2: ["RPG", "Horror", "Story Rich"],
}


def _legacy_documents(games):
    """Whitespace-tokenised genre+tag strings, as the original built them."""
    out = []
    for i in range(len(games)):
        genres = games["genres"].iloc[i]
        tags = games["tags"].iloc[i]
        genres = ", ".join(genres) if isinstance(genres, (list, tuple)) else str(genres)
        tags = ", ".join(tags) if isinstance(tags, (list, tuple)) else str(tags)
        out.append(f"{genres} {tags}")
    return out


def legacy_scores(games, sessions, mood=0, activity_ratio=0.8):
    """Return (scores, excluded_game_ids) using the original formula."""
    documents = _legacy_documents(games)
    vectorizer = TfidfVectorizer()
    item_vectors = vectorizer.fit_transform(documents).toarray()

    index_of = {gid: i for i, gid in enumerate(games["game_id"].astype(str))}

    totals, counts = {}, {}
    for row in sessions.itertuples():
        gid = str(row.game_id)
        totals[gid] = totals.get(gid, 0) + int(row.duration_seconds or 0)
        counts[gid] = counts.get(gid, 0) + 1

    if not totals:
        return np.zeros(len(games)), set()

    max_duration = max(totals.values()) or 1
    max_count = max(counts.values()) or 1

    interest = {}
    for gid in totals:
        # activity_ratio was a 3-valued restatement of the like button, and
        # carried the single heaviest weight.
        interest[gid] = (activity_ratio * 0.5
                         + (totals[gid] / max_duration) * 0.3
                         + (counts[gid] / max_count) * 0.2)

    weighted = []
    for gid, weight in interest.items():
        row = index_of.get(gid)
        if row is not None:
            weighted.append(item_vectors[row] * weight)
    if not weighted:
        return np.zeros(len(games)), set()

    profile = np.mean(weighted, axis=0).reshape(1, -1)
    scores = cosine_similarity(profile, item_vectors)[0]

    ratings = np.array([0.0 if r is None or (isinstance(r, float) and np.isnan(r)) else float(r)
                        for r in games["rating"]], dtype=float)
    # The scale bug, reproduced exactly.
    scores = scores * 0.8 + (ratings / 10.0) * 0.2

    preferred = LEGACY_MOOD_GENRES.get(mood, [])
    for i in range(len(games)):
        genres = games["genres"].iloc[i]
        text = ", ".join(genres) if isinstance(genres, (list, tuple)) else str(genres)
        for label in preferred:
            if label.lower() in text.lower():
                scores[i] += 0.15
                break

    return scores, set(interest)
