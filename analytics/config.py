"""Configuration loaded from the environment.

Values come from `analytics/.env` (gitignored) -- see `.env.example` for the
template. Every name here is optional: the recommender must run on a fresh
install where the user has not entered anything yet.

That is a change from v2, which called `_require("DB_PASSWORD")` and raised at
import. The intent was to fail loudly instead of handing psycopg2 an empty
password, but the failure surfaced deep inside the launcher's QProcess as
"ML unavailable" with no hint that a missing .env was the cause. SQLite has no
password, so the whole class of failure is gone with it.
"""

import os

from dotenv import load_dotenv

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
load_dotenv(os.path.join(BASE_DIR, ".env"))

# Where the analytics database lives. db.py owns the default; this only exists
# so an advanced user can relocate it.
DB_PATH = os.getenv("DB_PATH", os.path.join(BASE_DIR, "vortex.sqlite3"))

# IGDB / Twitch credentials for the Python catalog fetcher (igdb_catalog.py).
# Optional: only the catalog refresh needs them, so a missing value must not
# stop the recommender from running.
IGDB_CLIENT_ID = os.getenv("IGDB_CLIENT_ID", "")
IGDB_CLIENT_SECRET = os.getenv("IGDB_CLIENT_SECRET", "")

# Used by the launcher (C++ side, via secrets.cpp) to download artwork. Read
# here too so Python-side tooling can report whether it is set.
STEAMGRIDDB_API_KEY = os.getenv("STEAMGRIDDB_API_KEY", "")


def has_igdb_credentials():
    """Whether igdb_catalog.py can authenticate.

    Callers use this to skip the catalog refresh with a clear message rather
    than letting the fetch fail with an opaque 401.
    """
    return bool(IGDB_CLIENT_ID and IGDB_CLIENT_SECRET)
