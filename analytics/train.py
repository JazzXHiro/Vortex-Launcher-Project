"""Fit and persist the item vectorizer.

This is a thin CLI wrapper: the feature construction itself lives in `model.py`
so that `recommend.py` builds vectors exactly the same way. Keeping one
implementation is what prevents train/serve skew — the vectorizer is otherwise
fitted in one process and used in another with no guarantee they agree.

Note that `recommend.py` refits in-process anyway (it costs well under 100ms at
catalog size), so the .pkl written here is a cache, never a source of truth.

Collaborative filtering was deliberately removed. The user x game pivot has
exactly one row — every CF family learns from co-occurrence *across* users, so
with n_users = 1 there is nothing to learn and TruncatedSVD was fitting noise
on a rank-<=1 matrix. See the plan's "Decision: drop collaborative filtering".
"""

import sys

import pandas as pd

from db import get_connection
from model import fit_vectorizer, save_vectorizer, build_documents

conn = get_connection()
games = pd.read_sql("SELECT * FROM games", conn)
conn.close()

documents = build_documents(games)

# An empty catalog otherwise reaches TfidfVectorizer and comes back as
# "ValueError: empty vocabulary; perhaps the documents only contain stop
# words" -- true, but it says nothing about the actual cause, which is that
# nothing has been synced yet. The launcher never calls this script (see the
# comment in VortexBridge about skipping a retrain), so the only way to get
# here is by hand, and a sentence is more use than a stack trace.
if not any(document.strip() for document in documents):
    print("No games with genre or tag data in the database, so there is "
          "nothing to fit.\n"
          "Run the launcher once to scan your library, then:\n"
          "    python reset_db.py --yes\n"
          "    python igdb_catalog.py --refresh   (needs IGDB credentials)",
          file=sys.stderr)
    sys.exit(1)
vectorizer = fit_vectorizer(documents)
save_vectorizer(vectorizer, n_rows=len(games), documents=documents)

print(f"Vectorizer trained on {len(games)} games, "
      f"{len(vectorizer.get_feature_names_out())} features.")
