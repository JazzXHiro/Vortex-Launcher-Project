"""Why one game scores the way it does, under every mood, printed for the CLI.

    python explain_game.py "Hades"

Run by the launcher whenever a recommendation card is clicked (see
VortexBridge::logRecommendationClick), and by hand when a pick looks wrong.

Two questions get answered side by side:

  1. WHICH LABELS THE MOODS CARE ABOUT. Every genre, theme, game mode and
     keyword the game carries, checked against MOOD_LABELS for Relaxed,
     Competitive and Immersive, with the weight each contributes. Showing all
     three at once is the point -- it is what explains why the same game ranks
     near the top under one mood and vanishes under another, which reading a
     single mood in isolation cannot tell you.

  2. WHICH OF YOUR GAMES IT RESEMBLES. The ranked similarity list that
     scoring.explain_section() compresses into one "Because you played X"
     sentence, shown whole so the sentence can be checked rather than trusted.

Read-only: nothing here writes to the database or to recommendations.json.
"""

import sys

import numpy as np
import pandas as pd

import model
import scoring
from interest import build_interest
from recommend import load_frames, load_favorites

# Neutral is excluded deliberately: it applies no label weights at all
# (MOOD_LABELS has no entry for it), so a column of zeros would say nothing.
MOODS = [0, 1, 2]

LABEL_FIELDS = [
    ("genres", "Genres"),
    ("themes", "Themes"),
    ("game_modes", "Game modes"),
    ("keywords", "Keywords"),
    ("tags", "Tags"),
]


def find_row(games, name):
    """Locate a game by name, canonically, so casing and punctuation do not matter."""
    target = model.repair_mojibake(name).strip().lower()
    canonical = "".join(ch for ch in target if ch.isalnum())

    for i in range(len(games)):
        candidate = str(games["name"].iloc[i])
        if candidate.strip().lower() == target:
            return i
        if "".join(ch for ch in candidate.lower() if ch.isalnum()) == canonical:
            return i
    return None


def game_labels(games, row):
    """Every label the game carries, grouped by the field it came from."""
    out = []
    for field, heading in LABEL_FIELDS:
        if field not in games.columns:
            continue
        values = model._labels(games[field].iloc[row])
        if values:
            out.append((heading, values))
    return out


def print_labels(grouped):
    print("  Labels")
    if not grouped:
        print("    (none -- this game has no metadata, so no mood can match it)")
        return

    present = {heading: values for heading, values in grouped}

    # Every expected kind is listed, empty or not. Omitting a line leaves the
    # reader unable to tell "this game has no keywords" from "keywords were
    # never fetched", which are very different problems.
    for _field, heading in LABEL_FIELDS:
        values = present.get(heading)
        print(f"    {heading:<12} " + (", ".join(values) if values else "(none)"))


def print_mood_table(grouped):
    """Per mood: which labels matched, their weights, and the total."""
    all_labels = []
    for _, values in grouped:
        for value in values:
            if value not in all_labels:
                all_labels.append(value)

    print()
    print("  Mood weights")

    if not all_labels:
        print("    (no labels to match)")
        return

    for mood in MOODS:
        weights = scoring.MOOD_LABELS.get(mood, {})
        matched = [(label, weights[label]) for label in all_labels if label in weights]

        name = scoring.MOOD_NAMES.get(mood, str(mood))
        total = sum(weight for _, weight in matched)

        print()
        print(f"    {name}  (total {total:+g})")

        if not matched:
            # Not the same as scoring zero on purpose: it means the mood has
            # no opinion about this game either way.
            print("      no matching labels -- this mood neither favours nor "
                  "penalises it")
            continue

        positive = [m for m in matched if m[1] > 0]
        negative = [m for m in matched if m[1] < 0]

        for label, weight in sorted(positive, key=lambda m: -m[1]):
            print(f"      +{weight:<4g} {label}")
        for label, weight in sorted(negative, key=lambda m: m[1]):
            print(f"      {weight:<5g} {label}")


def print_inspirations(games, row, sessions, favorites):
    """The user's own games ranked by resemblance to this one."""
    print()
    print("  Inspired by (your games, most similar first)")

    documents = model.build_documents(games)
    vectorizer = model.fit_vectorizer(documents)
    item_vectors = vectorizer.transform(documents).toarray()

    ids = [str(games["game_id"].iloc[i]) for i in range(len(games))]
    names = [str(games["name"].iloc[i]) for i in range(len(games))]
    index_of = {game_id: i for i, game_id in enumerate(ids)}

    # Same shape recommend.py builds, so the similarities printed here are the
    # ones that actually ranked the card rather than an approximation of them.
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

    interest, _disinterest, stats = build_interest(session_records, favorites, lengths)

    # Say what the profile actually contains. The old message claimed "no play
    # history or favourites yet" and printed it to a user who had both -- it
    # described the symptom (an empty interest dict) as though it were the
    # cause, which sent the investigation in the wrong direction entirely.
    played = len(stats.get("engaged_ids", ()) or ())
    liked = len(favorites)
    derived = sum(1 for row in sessions.itertuples()
                  if getattr(row, "synthetic", 0) == 1)

    print(f"    profile: {played} played, {liked} liked"
          + (f", {derived} sessions derived from Steam playtime" if derived else ""))

    if not interest:
        missing = []
        if not played:
            missing.append("no play history (Vortex only records games it "
                           "launched itself)")
        if not liked:
            missing.append("no favourites (heart a game to teach it your taste)")
        print("    nothing to compare against -- " + "; ".join(missing))
        return

    sources = scoring.inspiration_sources(
        [row], item_vectors, interest, index_of, names,
        played_ids=stats.get("engaged_ids", ()), top_n=8)[0]

    if not sources:
        print("    (no comparable games in your library)")
        return

    for entry in sources:
        origin = "played" if entry["played"] else "liked"
        bar = "#" * int(round(entry["similarity"] * 20))
        print(f"    {entry['similarity']:.4f}  {origin:<6} {bar:<20} {entry['name']}")


def main():
    # --labels prints the header and the label block only. Opening a game
    # profile asks "what is this game", which the labels answer; the mood
    # tables and the similarity list answer "why was this recommended", which
    # is a question about a recommendation. Printing all of it on every profile
    # open would put roughly forty lines in the console per click while
    # browsing a library.
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    labels_only = "--labels" in sys.argv[1:]

    if not argv:
        print(__doc__)
        return 1

    name = argv[0]

    try:
        games, sessions = load_frames()
    except Exception as exc:
        print(f"Could not open the analytics database: {exc}", file=sys.stderr)
        print("Genres, themes and game modes still come from the launcher's "
              "own cache; only keywords need this database.", file=sys.stderr)
        return 1

    if games.empty:
        print("The database is empty -- run the launcher once to scan your library.",
              file=sys.stderr)
        return 1

    row = find_row(games, name)
    if row is None:
        print(f"No game named {name!r} in the database.", file=sys.stderr)
        return 1

    print()
    print("=" * 70)
    print(f"  {games['name'].iloc[row]}")
    source = str(games["source"].iloc[row]) if "source" in games.columns else "?"
    rating = scoring.as_float(games["rating"].iloc[row]) if "rating" in games.columns else float("nan")
    print(f"  source {source}"
          + (f"   rating {rating:.1f}" if rating == rating else "   rating n/a"))
    print("=" * 70)

    grouped = game_labels(games, row)
    print_labels(grouped)

    if labels_only:
        print()
        return 0

    print_mood_table(grouped)

    try:
        print_inspirations(games, row, sessions, load_favorites())
    except Exception as exc:  # diagnostics must never take the launcher down
        print(f"    (could not compute similarities: {exc})")

    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
