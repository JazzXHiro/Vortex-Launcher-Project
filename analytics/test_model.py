"""Regression tests for feature construction.

Run with:  python test_model.py

The tokenization test is the guard for the defect where TfidfVectorizer's
default token_pattern shattered multi-word genre labels into meaningless
unigrams, so similarity was computed over noise. If it fails, the recommender
is silently back to comparing games on fragments like "role" and "playing".
"""

import sys

from model import build_document, fit_vectorizer, repair_mojibake


def check(condition, label):
    print(f"{'PASS' if condition else 'FAIL'}  {label}")
    return condition


def test_atomic_labels():
    print("\n-- multi-word labels stay atomic --")
    docs = [
        build_document(["Role-playing (RPG)"], ["Role-playing (RPG)", "Adventure"]),
        build_document(["Racing"], ["Racing", "Sport", "Arcade"]),
        build_document(["Fighting"], ["Hack and slash/Beat 'em up", "Card & Board Game"]),
    ]
    features = set(fit_vectorizer(docs).get_feature_names_out())

    ok = True
    for want in ["role-playing (rpg)", "hack and slash/beat 'em up", "card & board game"]:
        ok &= check(want in features, f"present: {want!r}")
    for bad in ["rpg", "role", "playing", "slash", "board", "em"]:
        ok &= check(bad not in features, f"absent:  {bad!r}")
    return ok


def test_mojibake_repair():
    print("\n-- C++ parser's \\uXXXX mangling is repaired --")
    ok = True
    ok &= check(repair_mojibake("Assassinu0027s Creed") == "Assassin's Creed",
                "u0027 -> apostrophe")
    ok &= check(repair_mojibake("Card u0026 Board Game") == "Card & Board Game",
                "u0026 -> ampersand")
    ok &= check(repair_mojibake("Terraria") == "Terraria",
                "clean text untouched")
    return ok


def test_main_genres_weighted():
    print("\n-- main genres outweigh a single keyword --")
    doc = build_document(["Racing"], ["Racing", "Sport"], themes=["Action"],
                         keywords=["drifting"])
    parts = doc.split("|")
    ok = check(parts.count("Racing") == 3, "a main genre appears 3x (1 tag + 2 genre)")
    ok &= check(parts.count("drifting") == 1, "a keyword appears 1x")
    ok &= check(parts.count("Action") == 1, "a theme appears 1x")
    # tags still carry information for OWNED rows, where they come from the
    # AllGenres field and list genres the main set omits. Catalog rows get
    # genres|themes there and lose nothing by the duplication.
    ok &= check(parts.count("Sport") == 1, "a tag-only label still reaches the document")
    return ok


def test_keywords_in_document():
    print("\n-- keywords reach the feature space --")
    docs = [
        build_document(["Role-playing (RPG)"], [], themes=["Fantasy"],
                       keywords=["soulslike", "survival horror"]),
        build_document(["Role-playing (RPG)"], [], themes=["Fantasy"],
                       keywords=["cute", "hand-drawn"]),
    ]
    features = set(fit_vectorizer(docs).get_feature_names_out())
    ok = True
    for want in ["soulslike", "survival horror", "cute", "hand-drawn"]:
        ok &= check(want in features, f"present: {want!r}")

    # The whole point: two games with IDENTICAL genres and themes must stop
    # being indistinguishable. Before keywords, pairs like Pillars of Eternity II
    # and Final Fantasy XIII scored exactly 1.00 against each other.
    from sklearn.metrics.pairwise import cosine_similarity
    vectors = fit_vectorizer(docs).transform(docs).toarray()
    similarity = float(cosine_similarity(vectors[0:1], vectors[1:2])[0][0])
    ok &= check(similarity < 0.999,
                f"identical genres+themes no longer means identical games ({similarity:.3f})")
    return ok


def test_unknown_dropped():
    print("\n-- 'Unknown' placeholder is not a feature --")
    doc = build_document(["Unknown"], ["Unknown"])
    return check(doc == "", "all-Unknown game yields an empty document")


def test_keyword_filters():
    print("\n-- keyword blocklist and frequency band --")
    from igdb_catalog import (is_blocked_keyword, filter_keywords,
                              KEYWORD_MIN_SHARE, KEYWORD_MAX_SHARE)
    ok = True

    # Storefront and platform metadata, which was roughly half of the most
    # common keywords IGDB returns.
    for junk in ["steam achievements", "Bink Video", "auto-saving",
                 "Xbox controller support for PC", "PlayStation Trophies",
                 "digital distribution"]:
        ok &= check(is_blocked_keyword(junk), f"blocked: {junk!r}")
    for keep in ["soulslike", "survival horror", "squad tactics", "hand-drawn"]:
        ok &= check(not is_blocked_keyword(keep), f"kept:    {keep!r}")

    # The band is a property of the CORPUS, so the fixture needs enough records
    # for a below-floor keyword to actually be below the floor. 1,000 records:
    # 'common' on all of them (above the ceiling), 'rare' on one (0.1%, below
    # the floor), 'good' on fifty (5%, inside the band).
    def corpus():
        records = []
        for i in range(1000):
            kws = ["common"]
            if i < 50:
                kws.append("good")
            if i == 0:
                kws.append("rare")
            records.append({"keywords": kws})
        return records

    records = corpus()
    vocab = filter_keywords(records)
    kept = {k for r in records for k in r["keywords"]}
    ok &= check(kept == {"good"}, f"only the in-band keyword survives, got {sorted(kept)}")
    ok &= check(vocab == 1, f"reported vocabulary size is 1, got {vocab}")
    ok &= check(KEYWORD_MIN_SHARE <= 0.05 <= KEYWORD_MAX_SHARE,
                "the 5% fixture really does sit inside the configured band")
    ok &= check(0.001 < KEYWORD_MIN_SHARE, "the 0.1% fixture really is below the floor")

    # A mood keyword must survive whatever its frequency. A mis-set floor once
    # deleted 34 of the 41 and left every mood weight pointing at a label no
    # game carried -- silently, because a filtered-out keyword looks identical
    # to one IGDB never had.
    import scoring
    rare_mood = sorted(scoring.MOOD_KEYWORD_NAMES)[0]
    records = corpus()
    records[0]["keywords"].append(rare_mood)
    filter_keywords(records)
    ok &= check(rare_mood in records[0]["keywords"],
                f"a mood keyword survives below the floor ({rare_mood!r})")

    ok &= check(filter_keywords([]) == 0, "an empty fetch does not divide by zero")
    return ok


if __name__ == "__main__":
    results = [
        test_atomic_labels(),
        test_mojibake_repair(),
        test_main_genres_weighted(),
        test_keywords_in_document(),
        test_unknown_dropped(),
        test_keyword_filters(),
    ]
    print(f"\n{'ALL PASSED' if all(results) else 'FAILURES PRESENT'}")
    sys.exit(0 if all(results) else 1)
