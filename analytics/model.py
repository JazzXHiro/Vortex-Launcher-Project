"""Shared item-feature construction.

Both `train.py` and `recommend.py` import from here so the vectorizer is built
from identical documents in both processes. Duplicated feature code was the
root cause of train/serve skew, not the persistence format.

The important detail is the tokenizer. Genres and tags are multi-word
categorical labels -- "Role-playing (RPG)", "Hack and slash/Beat 'em up",
"Card & Board Game". scikit-learn's default token_pattern splits on
whitespace and punctuation, shattering those into meaningless unigrams
("role", "playing", "rpg") and computing similarity over noise. We join labels
with "|" and tokenize on `[^|]+` so each label stays one atomic feature.

`[^|]+` is a plain regex string and therefore picklable. Do not replace it with
`analyzer=lambda` -- that breaks joblib.dump.
"""

import hashlib
import json
import os
import re

import joblib
import sklearn
from sklearn.feature_extraction.text import TfidfVectorizer

BASE_DIR = os.path.dirname(__file__)
MODEL_DIR = os.path.join(BASE_DIR, "model")
VECTORIZER_PATH = os.path.join(MODEL_DIR, "vectorizer.pkl")
MANIFEST_PATH = os.path.join(MODEL_DIR, "model_manifest.json")

MODEL_VERSION = "2.0"

# The label separator. Chosen because it does not occur in IGDB genre or theme
# names, unlike "," and "/" which both appear inside real labels.
SEP = "|"

_MOJIBAKE = re.compile(r"u00([0-9a-fA-F]{2})")


def repair_mojibake(text):
    """Undo the C++ JSON parser's mangling of \\uXXXX escapes.

    `source/igdb_manager.cpp` drops the backslash and keeps the following
    characters, so an apostrophe arrives as "u0027" and an ampersand as
    "u0026" -- visible in the committed caches as "Assassinu0027s Creed III"
    and "Card u0026 Board Game". Catalog rows fetched by igdb_catalog.py parse
    JSON properly and are unaffected; this only repairs flat-file-derived data.
    """
    if not isinstance(text, str):
        return text
    return _MOJIBAKE.sub(lambda m: chr(int(m.group(1), 16)), text)


def _labels(value):
    """Normalize a genres/tags cell into a clean list of labels.

    db.decode_row() turns the JSON-array columns back into lists, but the
    same columns are sometimes handled as comma-joined strings elsewhere
    (the C++ flat-file caches), so accept both.
    """
    if value is None:
        return []

    if isinstance(value, (list, tuple)):
        parts = list(value)
    else:
        parts = str(value).split(",")

    out = []
    for part in parts:
        label = repair_mojibake(str(part)).strip()
        # Guard against the "Unknown" placeholder that sync_local_data.py
        # writes for games with no IGDB metadata; it carries no information
        # and would otherwise become a shared feature linking unrelated games.
        if label and label.lower() != "unknown":
            out.append(label)
    return out


def build_document(genres, tags, themes=None, modes=None, keywords=None):
    """Build one game's feature document.

    Main genres are emitted twice so they carry roughly double the term
    frequency of any single keyword or tag -- salience for free, without a
    second weighting mechanism. A genre says what a game IS and should outweigh
    any one descriptor, but not the eleven of them a game typically carries.

    `tags` is still emitted, despite being measured as exactly genres|themes on
    all 6,288 CATALOG rows. It is not redundant for the user's OWN games: those
    rows take `tags` from the AllGenres field of game_metadata.txt, which
    carries genres the main list omits -- Stick Fight has indie, simulator and
    strategy there and nowhere else. Dropping it cost 29 owned rows real labels,
    and owned rows are what the taste profile is built from.
    """
    genre_labels = _labels(genres)
    parts = (_labels(keywords) + _labels(tags)
             + genre_labels + genre_labels
             + _labels(themes) + _labels(modes))
    return SEP.join(parts)


def build_documents(games):
    """Build feature documents for a games DataFrame (row order preserved)."""
    themes = games["themes"] if "themes" in games.columns else None
    modes = games["game_modes"] if "game_modes" in games.columns else None
    keywords = games["keywords"] if "keywords" in games.columns else None
    return [
        build_document(
            games["genres"].iloc[i],
            games["tags"].iloc[i],
            None if themes is None else themes.iloc[i],
            None if modes is None else modes.iloc[i],
            None if keywords is None else keywords.iloc[i],
        )
        for i in range(len(games))
    ]


def make_vectorizer():
    return TfidfVectorizer(
        token_pattern=r"[^|]+",
        lowercase=True,
        sublinear_tf=True,
    )


def fit_vectorizer(documents):
    """Fit over the full catalog so vocabulary is stable.

    Fitting on the whole catalog rather than the owned library means installing
    a game does not shift the feature space underneath the model.
    """
    vectorizer = make_vectorizer()
    vectorizer.fit(documents)
    return vectorizer


def documents_hash(documents):
    """Fingerprint of the corpus the vectorizer was fitted on.

    Row count alone is not enough: refreshing the catalog in place can leave
    the count unchanged while replacing which games -- and therefore which
    labels -- are present, so a stale vocabulary would be reused silently.

    Hashing the input rather than the fitted vocabulary is deliberate. Checking
    the output would mean fitting a probe vectorizer to compare against, which
    costs exactly what this cache exists to avoid; identical documents
    necessarily produce an identical fit.
    """
    digest = hashlib.sha256()
    for document in documents:
        # Length-prefixed so that ["ab", "c"] and ["a", "bc"] cannot collide.
        digest.update(str(len(document)).encode("ascii"))
        digest.update(b":")
        digest.update(document.encode("utf-8"))
    return digest.hexdigest()[:32]


def save_vectorizer(vectorizer, n_rows, documents=None):
    os.makedirs(MODEL_DIR, exist_ok=True)
    joblib.dump(vectorizer, VECTORIZER_PATH)
    manifest = {
        "model_version": MODEL_VERSION,
        "sklearn_version": sklearn.__version__,
        "n_features": len(vectorizer.get_feature_names_out()),
        "catalog_row_count": int(n_rows),
        "documents_hash": documents_hash(documents) if documents is not None else None,
    }

    with open(MANIFEST_PATH, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=4)


def load_vectorizer(expected_rows=None, documents=None):
    """Load the cached vectorizer, or None if it is missing or stale.

    Returning None on any mismatch is deliberate: a refit costs well under
    100ms at catalog size, which is far cheaper than silently serving
    recommendations from a vocabulary that no longer matches the data.
    """
    if not (os.path.exists(VECTORIZER_PATH) and os.path.exists(MANIFEST_PATH)):
        return None

    try:
        with open(MANIFEST_PATH, encoding="utf-8") as handle:
            manifest = json.load(handle)
    except (OSError, ValueError):
        return None

    if manifest.get("model_version") != MODEL_VERSION:
        return None
    if manifest.get("sklearn_version") != sklearn.__version__:
        return None
    if expected_rows is not None and manifest.get("catalog_row_count") != int(expected_rows):
        return None

    # Content check. Everything above compares only metadata, which a catalog
    # refresh can leave identical while the corpus underneath has changed.
    if documents is not None:
        expected = manifest.get("documents_hash")
        if not expected or expected != documents_hash(documents):
            return None

    try:
        return joblib.load(VECTORIZER_PATH)
    except Exception:
        return None
