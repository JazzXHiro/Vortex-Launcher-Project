"""Tests for sessions derived from play the session log does not have.

Run with:  python test_derived_sessions.py

The launcher exports, per installed game, the playtime Vortex never watched --
Steam's imported history, or hours the user entered by hand -- and the sync
turns it into synthetic sessions. These run the whole sync against a scratch
folder and database, because the rules that matter live in sync_data(): the
derived rows must sit beside real sessions rather than be dropped for them, and
a changed total must replace the old rows rather than pile up on top of them.
"""

import os
import sys
import tempfile
from datetime import datetime

import sync_local_data
from db import get_connection

GAME = "Hollow Knight"
LAST_PLAYED = int(datetime(2026, 9, 1, 20, 0).timestamp())


def check(condition, label):
    print(f"{'PASS' if condition else 'FAIL'}  {label}")
    return bool(condition)


def _write(folder, name, text):
    with open(os.path.join(folder, name), "w", encoding="utf-8") as f:
        f.write(text)


def _sync(folder, db_path, exported_seconds):
    _write(folder, "installed_games.txt",
           "# Format: NAME|SOURCE|IGDB_ID|PLAYTIME_SECONDS|LAST_PLAYED|INSTALL_DIR\n"
           f"{GAME}|Local|0|{exported_seconds}|{LAST_PLAYED}|C:\\Games\\HK\n")
    sync_local_data.sync_data()

    conn = get_connection(db_path)
    try:
        rows = conn.execute("""
            SELECT s.synthetic, s.duration_seconds
              FROM sessions s JOIN games g ON g.game_id = s.game_id
             WHERE g.canonical_name = ?
        """, (sync_local_data.make_canonical(GAME),)).fetchall()
    finally:
        conn.close()
    synthetic = [d for flag, d in rows if flag == 1]
    observed = [d for flag, d in rows if flag == 0]
    return synthetic, observed


def test_entered_playtime():
    print("\n-- entered playtime through a full sync --")
    ok = True
    folder = tempfile.mkdtemp()
    db_path = os.path.join(folder, "test.sqlite3")
    original_dir = sync_local_data.BASE_DIR
    original_conn = sync_local_data.get_connection
    sync_local_data.BASE_DIR = folder
    sync_local_data.get_connection = lambda path=None: get_connection(db_path)
    try:
        # One session Vortex watched, so the game has real history too.
        _write(folder, "playtime_sessions.log",
               "# Format: GAME_KEY | GAME_NAME | DURATION_SEC | START_DATE | END_DATE | IDLE_SEC\n"
               f"local_hollowknight | {GAME} | 3600 | 2026-09-05 Sat 18:00:00"
               " | 2026-09-05 Sat 19:00:00 | 0\n")

        synthetic, observed = _sync(folder, db_path, 20 * 3600)
        ok &= check(observed == [3600], "the watched session is synced as observed")
        ok &= check(sum(synthetic) == 20 * 3600,
                    "derived rows sit beside it and sum to the exported total")

        synthetic, observed = _sync(folder, db_path, 50 * 3600)
        ok &= check(sum(synthetic) == 50 * 3600,
                    "a changed total replaces the old rows instead of adding to them")
        ok &= check(observed == [3600], "the watched session is untouched")

        synthetic, _ = _sync(folder, db_path, 0)
        ok &= check(synthetic == [], "a total taken back to zero clears them")
    finally:
        sync_local_data.BASE_DIR = original_dir
        sync_local_data.get_connection = original_conn
    return ok


if __name__ == "__main__":
    results = [test_entered_playtime()]
    print(f"\n{'ALL PASSED' if all(results) else 'FAILURES PRESENT'}")
    sys.exit(0 if all(results) else 1)
