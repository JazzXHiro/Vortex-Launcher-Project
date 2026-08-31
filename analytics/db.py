"""SQLite connection factory and the JSON-array bridge.

Vortex used PostgreSQL until v3. It moved to SQLite so the launcher could ship
as a self-contained install: a database server is the one dependency an
installer cannot set up silently -- it needs a service, a port, a data cluster
and a superuser password, and every one of those is a way for someone else's
machine to fail.

`get_connection()` keeps its original name and signature, so callers did not
change. Two things here do the compatibility work:

  * `decode_row` turns the five JSON-array columns back into Python lists, so
    callers see exactly what psycopg2 used to hand them and `model._labels()`
    is untouched.

  * `encode_list` is the write half. Bind through it for those columns --
    sqlite3 rejects a bare Python list.

The one thing that did NOT survive is the paramstyle: sqlite3 uses `?`, not
`%s`. Those were converted at every call site rather than rewritten inside a
cursor wrapper, because a blanket `%s` -> `?` substitution would also corrupt
LIKE patterns and the `'{}'` literals in the upserts.
"""

import datetime
import json
import os
import sqlite3

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

# Beside the scripts, matching how config.py locates .env and how the C++ side
# resolves every data file against the executable's directory (app_paths.h).
_DEFAULT_DB_PATH = os.path.join(BASE_DIR, "vortex.sqlite3")

try:
    # config owns the DB_PATH override so a user can relocate the database.
    # Guarded because config imports python-dotenv: a partially installed
    # environment should still be able to open an existing database rather
    # than failing at import with a bare ModuleNotFoundError.
    from config import DB_PATH
except Exception:  # pragma: no cover - depends on the install, not the code
    DB_PATH = _DEFAULT_DB_PATH

# The columns schema.sql stores as JSON arrays. Keep this in sync with the
# CREATE TABLE for `games` -- a column missing here comes back as a raw JSON
# string, and _labels() would then split it on commas into garbage tokens
# rather than failing loudly.
ARRAY_COLUMNS = frozenset({"genres", "tags", "themes", "game_modes", "keywords"})

# Columns the v2 schema declared TIMESTAMP. psycopg2 returned real datetime
# objects for these and callers rely on it -- interest.recency_weight() does
# arithmetic straight on sessions.ended_at, which raises
# "unsupported operand type(s) for -: 'str' and 'str'" if it arrives as text.
# Decoding here keeps that contract instead of pushing a parse into every
# caller.
TIMESTAMP_COLUMNS = frozenset({"started_at", "ended_at", "fetched_at", "created_at"})

# released_at was a DATE. scoring._days_since() already accepts str, date and
# datetime, so this one is decoded purely for parity with what psycopg2 handed
# back -- str() of a date is the same ISO text either way.
DATE_COLUMNS = frozenset({"released_at"})


# sqlite3's built-in date/datetime adapters were deprecated in Python 3.12 and
# emit a DeprecationWarning on every bind. The caller here (sync_local_data.py)
# holds real datetime objects, and the schema stores ISO-8601
# text, so register the conversion explicitly rather than relying on a default
# that is on its way out.
sqlite3.register_adapter(datetime.datetime, lambda v: v.isoformat(sep=" "))
sqlite3.register_adapter(datetime.date, lambda v: v.isoformat())


def encode_list(value):
    """Python list -> JSON text for storage.

    Accepts what the callers actually hold: a list, a comma-joined string from
    the flat-file caches, or None. Returns '[]' rather than NULL for the empty
    case so the `NULLIF(x, '[]')` guards in the upserts keep working -- they
    are what stops a metadata-free sync from wiping a backfilled set.
    """
    if value is None:
        return "[]"
    if isinstance(value, str):
        # Already JSON? Leave it. Otherwise treat it as the comma-joined form
        # the C++ caches emit.
        stripped = value.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            return stripped
        value = [part for part in (p.strip() for p in value.split(",")) if part]
    return json.dumps(list(value), ensure_ascii=False)


def decode_list(value):
    """JSON text -> Python list. Tolerates NULL and pre-v3 comma-joined rows."""
    if value is None:
        return []
    if isinstance(value, (list, tuple)):
        return list(value)
    text = str(value).strip()
    if not text:
        return []
    if text.startswith("["):
        try:
            decoded = json.loads(text)
        except json.JSONDecodeError:
            return [text]
        return decoded if isinstance(decoded, list) else [decoded]
    return [part for part in (p.strip() for p in text.split(",")) if part]


def decode_datetime(value):
    """ISO text -> datetime. Returns the input unchanged if it will not parse.

    Tolerant of both separators because the rows come from two eras: the
    adapter above writes "YYYY-MM-DD HH:MM:SS", while anything migrated from
    the old Postgres database arrived as isoformat() with a "T".
    """
    if value is None or isinstance(value, datetime.datetime):
        return value
    text = str(value).strip()
    if not text:
        return None
    try:
        return datetime.datetime.fromisoformat(text.replace("T", " "))
    except ValueError:
        return value


def decode_date(value):
    """ISO text -> date. Returns the input unchanged if it will not parse."""
    if value is None or isinstance(value, datetime.date):
        return value
    text = str(value).strip()
    if not text:
        return None
    try:
        return datetime.date.fromisoformat(text[:10])
    except ValueError:
        return value


def decode_value(name, value):
    if name in ARRAY_COLUMNS:
        return decode_list(value)
    if name in TIMESTAMP_COLUMNS:
        return decode_datetime(value)
    if name in DATE_COLUMNS:
        return decode_date(value)
    return value


def decode_row(cursor, row):
    """sqlite3 row_factory: restore the column types psycopg2 used to return.

    A tuple, not a Row or a dict, because callers index positionally
    (`row[0]`) and recommend.py builds DataFrames from `cursor.description`.
    Returning anything richer would change behaviour at those call sites.
    """
    names = [d[0] for d in cursor.description]
    return tuple(decode_value(name, value) for name, value in zip(names, row))


SCHEMA_PATH = os.path.join(BASE_DIR, "schema.sql")


def _migrate(conn):
    """Bring an existing database up to the current schema.

    _ensure_schema() only runs schema.sql when there are no tables at all, so
    a column added later would never reach a database that already exists --
    which is every install that has been launched once. Additive and
    idempotent: it only ever adds a missing column, never drops or rewrites,
    so running it on an up-to-date database does nothing.
    """
    columns = {row[1] for row in conn.execute("PRAGMA table_info(sessions)")}
    if "synthetic" not in columns:
        conn.execute(
            "ALTER TABLE sessions ADD COLUMN synthetic INTEGER NOT NULL DEFAULT 0")
        conn.commit()


def _ensure_schema(conn):
    """Create the tables if this database has none yet.

    sqlite3.connect() happily creates an empty FILE for a path that does not
    exist, so a fresh install had a database with no tables in it and every
    script died on "no such table: games" -- surfacing in the launcher as
    "ML unavailable" with nothing to say why.

    Under PostgreSQL the schema was applied by hand as a documented setup step
    (`python reset_db.py --yes`). Moving to SQLite so the app could ship as a
    one-click install removed the step from the instructions without replacing
    it, which broke recommendations on every fresh install.

    Done here rather than in a bootstrap script so that every entry point --
    recommend.py, sync_local_data.py, igdb_catalog.py, explain_game.py, the
    tests, and anyone running them by hand -- is covered by one implementation.

    Idempotent and non-destructive, unlike reset_db.py, which drops every table
    and must never be run automatically.
    """
    exists = conn.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='games'"
    ).fetchone()
    if exists:
        _migrate(conn)
        return

    with open(SCHEMA_PATH, encoding="utf-8") as handle:
        schema = handle.read()

    # executescript, not execute: sqlite3 runs exactly one statement per
    # execute() call and schema.sql is a dozen of them.
    conn.executescript(schema)
    conn.commit()


def get_connection(path=None):
    """Open the analytics database, creating it from schema.sql if it is new.

    Callers close their own connections, as they did with psycopg2.
    """
    conn = sqlite3.connect(path or DB_PATH)
    conn.row_factory = decode_row

    # Foreign keys are OFF by default in SQLite and must be set per connection.
    # sessions and recommendation_events both reference games(game_id); without
    # this an orphaned row inserts silently.
    conn.execute("PRAGMA foreign_keys = ON")

    # The launcher runs sync_local_data.py and recommend.py back to back after
    # a play session (see VortexBridge). WAL lets the second one read while the
    # first is still committing instead of failing on a locked database.
    conn.execute("PRAGMA journal_mode = WAL")

    # A sync during an open read otherwise raises "database is locked"
    # immediately rather than waiting for the writer to finish.
    conn.execute("PRAGMA busy_timeout = 10000")

    # After the pragmas: journal_mode = WAL has to be set before the first
    # write, and this may be the first write.
    _ensure_schema(conn)

    return conn
