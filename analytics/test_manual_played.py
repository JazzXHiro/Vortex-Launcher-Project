"""Tests for games marked played by hand ("Add to Played" on the Browse page).

Run with:  python test_manual_played.py

These rows are invented evidence, so the rules around them matter more than
for anything Vortex actually observed: they must count as real play, they must
never stack on top of real play, and they must be findable again so that
unmarking a game takes it back out of the profile.
"""

import os
import sys
import tempfile
from datetime import datetime

import sync_local_data
from interest import ENGAGED_FROM_SECONDS, build_interest
from sync_local_data import (make_canonical, manual_played_sessions,
                             manual_session_uuid, parse_manual_played)


def check(condition, label):
    print(f"{'PASS' if condition else 'FAIL'}  {label}")
    return bool(condition)


def _with_manual_file(text):
    """Point get_file_path() at a temp dir holding manual_played.txt."""
    folder = tempfile.mkdtemp()
    with open(os.path.join(folder, "manual_played.txt"), "w", encoding="utf-8") as f:
        f.write(text)
    sync_local_data.BASE_DIR = folder
    return folder


def test_parse():
    print("\n-- parse_manual_played --")
    ok = True
    original = sync_local_data.BASE_DIR
    try:
        _with_manual_file(
            "# header\n"
            "Hades|113112|1759500000\n"
            "\n"
            "No Id Game|0|1759500100\n"
            "Short Line\n")
        rows = parse_manual_played()
        ok &= check(len(rows) == 3, "comments and blank lines are skipped")
        ok &= check(rows[0] == ("Hades", "113112", 1759500000),
                    "name, id as a string, and epoch are read")
        ok &= check(rows[1][1] == "0", "an unknown id stays the string '0'")
        ok &= check(rows[2] == ("Short Line", "0", 0),
                    "missing trailing fields default instead of raising")

        sync_local_data.BASE_DIR = tempfile.mkdtemp()
        ok &= check(parse_manual_played() == [], "no file means no games")
    finally:
        sync_local_data.BASE_DIR = original
    return ok


def test_sessions():
    print("\n-- manual_played_sessions --")
    ok = True
    ids = {"hades": "g-hades", "celeste": "g-celeste", "tunic": "g-tunic"}
    manual = [("Hades", "113112", 1759500000),
              ("Celeste", "26226", 1759500000),
              ("Tunic", "23733", 1759500000),
              ("Unknown Game", "0", 1759500000)]

    rows = manual_played_sessions(manual, ids.get, skip_game_ids={"g-celeste"})
    by_game = {r["game_id"]: r for r in rows}

    ok &= check(set(by_game) == {"g-hades", "g-tunic"},
                "a game with real sessions and an unknown game are skipped")
    hades = by_game["g-hades"]
    ok &= check(hades["synthetic"] is True, "rows are flagged synthetic")
    ok &= check(hades["duration"] >= ENGAGED_FROM_SECONDS,
                "the session is long enough to count as engaged")
    ok &= check(hades["end"] == datetime.fromtimestamp(1759500000),
                "the session ends when the game was marked")
    ok &= check(hades["session_id"] == manual_session_uuid(make_canonical("Hades")),
                "the id depends on the game alone, so it can be retracted")

    again = manual_played_sessions([("Hades", "113112", 1800000000)], ids.get, set())
    ok &= check(again[0]["session_id"] == hades["session_id"],
                "re-marking later reuses the same id instead of stacking a row")
    return ok


def test_counts_as_played():
    print("\n-- feeds the profile --")
    rows = manual_played_sessions([("Hades", "113112", 1759500000)],
                                  {"hades": "g-hades"}.get, set())
    records = [{"game_id": r["game_id"], "duration_seconds": r["duration"],
                "ended_at": r["end"]} for r in rows]
    interest, _, stats = build_interest(records)
    ok = check("g-hades" in stats["engaged_ids"],
               "a marked game is in engaged_ids, so it is excluded from candidates")
    ok &= check(interest.get("g-hades", 0) > 0, "and it carries positive interest")
    return ok


if __name__ == "__main__":
    results = [test_parse(), test_sessions(), test_counts_as_played()]
    print(f"\n{'ALL PASSED' if all(results) else 'FAILURES PRESENT'}")
    sys.exit(0 if all(results) else 1)
