"""Rebuild the SQLite schema from scratch, then re-sync from the flat files.

    python reset_db.py --yes

DESTRUCTIVE: drops every Vortex table. That is safe here only because the
database holds nothing original -- every row is derived from igdb_cache.txt,
game_metadata.txt, playtime_sessions.log and preferences.json, and
sync_local_data.py is the only writer. Confirm that still holds before running.

You need this after changing how `game_id` is derived: ids are now a uuid5 of
the canonical game name rather than of the IGDB key, so rows written by an
older build will not match and duplicates would linger.

NOTE: this also drops the ~5,700-row IGDB discovery catalog, which is NOT
rebuilt from the flat files. Re-fetch it afterwards or the Discover section
will be nearly empty:

    python igdb_catalog.py --refresh
"""

import os
import sqlite3
import sys

from db import DB_PATH

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SCHEMA_PATH = os.path.join(BASE_DIR, "schema.sql")


def reset():
    with open(SCHEMA_PATH, encoding="utf-8") as handle:
        schema = handle.read()

    # Deleting the file is cleaner than dropping tables one by one: it also
    # clears the WAL sidecars and any index or table an older schema version
    # created that the current one no longer names. There is nothing to
    # preserve -- see the docstring.
    for suffix in ("", "-wal", "-shm"):
        path = DB_PATH + suffix
        if os.path.exists(path):
            os.remove(path)
            print(f"Removed {os.path.basename(path)}.")

    # executescript, not execute: sqlite3 runs exactly one statement per
    # execute() call and schema.sql is a dozen of them.
    conn = sqlite3.connect(DB_PATH)
    conn.executescript(schema)
    conn.commit()
    conn.close()
    print("Schema recreated.")


if __name__ == "__main__":
    if "--yes" not in sys.argv:
        print(__doc__)
        print("Refusing to run without --yes.")
        sys.exit(1)

    reset()

    from sync_local_data import sync_data
    sync_data()
