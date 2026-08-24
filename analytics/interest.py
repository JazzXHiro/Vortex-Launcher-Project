"""Turning play sessions into a taste profile.

Pure functions over plain dicts -- no database access -- so the behaviour can
be verified directly against the flat files.

The headline change from the previous model is that raw session time is no
longer trusted. The duration histogram is cleanly bimodal: a cluster under 90
seconds (launch, look, quit) and a cluster from ~3 minutes upward. Counting the
first cluster as interest meant a game the user bounced off four times was
pushing the profile *toward* similar games.
"""

import math
from datetime import timedelta

# --- Session quality bands -------------------------------------------------
# Read off the gap in the observed duration histogram, not invented:
#   1 6 7 15 15 18 20 39 41 46 55 57 75 88 | 170 216 298 472 705 1347 ...
ABORT_BELOW_SECONDS = 120      # never reached interactive play -> negative
ENGAGED_FROM_SECONDS = 600     # real play -> positive
# Anything between the two is ambiguous and is excluded from both profiles.

# --- Weighting -------------------------------------------------------------
RECENCY_HALF_LIFE_DAYS = 30.0
DURATION_CAP_HOURS = 10.0      # fixed, so weights don't rescale run-to-run

W_ENGAGEMENT = 0.40
W_FREQUENCY = 0.20
W_COMPLETION = 0.20
W_FAVORITE = 0.20

# A heart on a game that has never been played is a weaker signal than a heart
# on one that has, and much weaker than hours of actual play. It says "this
# looks interesting", not "I like this" -- most of these are hearted straight
# off a Discover card, sight unseen.
#
# It needs its own weight because the two cases were indistinguishable, and the
# result was upside down: an unplayed heart scored 0.250 while Life is Strange,
# genuinely played, scored 0.147. Measured on the live profile, 55 of 63 entries
# had never been played and held 83% of the profile mass.
#
# A heart on a game you HAVE played keeps the full W_FAVORITE -- that is two
# independent signals agreeing, and it should count for more, not less.
W_FAVORITE_UNPLAYED = 0.08

ABORT_PENALTY = 0.5            # multiplicative, so interest can't go negative
MIN_ABORTS_FOR_DISINTEREST = 2  # one crash must not condemn a game

BAND_ABORT = "abort"
BAND_NEUTRAL = "neutral"
BAND_ENGAGED = "engaged"


def classify_session(duration_seconds):
    if duration_seconds < ABORT_BELOW_SECONDS:
        return BAND_ABORT
    if duration_seconds < ENGAGED_FROM_SECONDS:
        return BAND_NEUTRAL
    return BAND_ENGAGED


def duration_weight(seconds):
    """log1p-compressed, capped at DURATION_CAP_HOURS.

    Normalising by max(duration) let a single long session crush every other
    game, and re-scaled every weight whenever a new session landed.
    """
    if seconds <= 0:
        return 0.0
    return min(1.0, math.log1p(seconds / 3600.0) / math.log1p(DURATION_CAP_HOURS))


def recency_weight(ended_at, reference):
    """Exponential decay with a 30-day half-life."""
    age_days = max(0.0, (reference - ended_at).total_seconds() / 86400.0)
    return 0.5 ** (age_days / RECENCY_HALF_LIFE_DAYS)


def reference_time(sessions, use_now=False, now=None):
    """The instant recency is measured from.

    Defaults to the most recent session rather than wall-clock time. The
    bundled session log is static academic data dated in the future relative to
    nothing in particular; measuring against `now()` makes every weight
    underflow toward zero and the ranking becomes numerically unstable. Pass
    use_now=True in a genuinely live deployment.
    """
    if use_now or not sessions:
        return now or _utcnow()
    return max(s["ended_at"] for s in sessions)


def _utcnow():
    from datetime import datetime
    return datetime.now()


def _normalize(values):
    top = max(values.values()) if values else 0.0
    if top <= 0:
        return {k: 0.0 for k in values}
    return {k: v / top for k, v in values.items()}


def build_interest(sessions, favorites=(), game_lengths=None, use_now=False):
    """Compute per-game interest and the behavioural disinterest set.

    sessions      -- dicts with game_id, duration_seconds, ended_at
    favorites     -- iterable of favourited game_ids
    game_lengths  -- game_id -> time-to-beat seconds (for the completion term)

    Returns (interest, disinterest, stats).
    """
    favorites = set(favorites)
    game_lengths = game_lengths or {}

    # No early return for an empty session list. Everything below already
    # handles it: reference_time() falls back to now, the loop does nothing,
    # _normalize({}) is {}, and `candidates` reduces to the favourites -- which
    # is exactly what that line was written to express.
    #
    # The guard that used to sit here returned before `favorites` was ever
    # read, so a user who had hearted games but never launched one through
    # Vortex got an empty profile: no "inspired by" sources, every card falling
    # back to the mood reason, and the taste half of the ranking contributing
    # nothing. engaged_counts stays empty here, so those games correctly take
    # W_FAVORITE_UNPLAYED and `engaged_ids` stays empty -- no card can claim
    # the user PLAYED something they only liked.
    reference = reference_time(sessions, use_now=use_now)

    engagement, frequency, engaged_secs = {}, {}, {}
    aborts, engaged_counts = {}, {}

    for s in sessions:
        gid = s["game_id"]
        band = classify_session(s["duration_seconds"])

        if band == BAND_ABORT:
            aborts[gid] = aborts.get(gid, 0) + 1
            continue
        if band == BAND_NEUTRAL:
            continue

        r = recency_weight(s["ended_at"], reference)
        engagement[gid] = engagement.get(gid, 0.0) + r * duration_weight(s["duration_seconds"])
        frequency[gid] = frequency.get(gid, 0.0) + r
        engaged_secs[gid] = engaged_secs.get(gid, 0) + s["duration_seconds"]
        engaged_counts[gid] = engaged_counts.get(gid, 0) + 1

    norm_engagement = _normalize(engagement)
    norm_frequency = _normalize(frequency)

    interest = {}
    # Sorted, not raw set order. Python randomises string hashing per process,
    # so iterating the set unsorted gave this dict a different insertion order
    # on every run -- which propagated into anything that iterated it, and made
    # evaluate.py produce different confidence intervals and a different random
    # baseline each time it ran. Score ties in recommend.py were free to break
    # differently between runs for the same reason.
    candidates = sorted(set(engagement) | favorites)
    for gid in candidates:
        length = game_lengths.get(gid) or 0
        completion = min(1.0, engaged_secs.get(gid, 0) / length) if length > 0 else None
        favorite = 1.0 if gid in favorites else 0.0

        # Hearted-but-never-played carries the reduced weight; hearted AND
        # played keeps the full one.
        favorite_weight = (W_FAVORITE if engaged_counts.get(gid, 0) > 0
                           else W_FAVORITE_UNPLAYED)

        terms = [
            (W_ENGAGEMENT, norm_engagement.get(gid, 0.0)),
            (W_FREQUENCY, norm_frequency.get(gid, 0.0)),
            (favorite_weight, favorite),
        ]
        if completion is None:
            # Redistribute the completion weight across the known terms so the
            # scale is preserved for games with no time-to-beat data.
            known = sum(w for w, _ in terms)
            terms = [(w + W_COMPLETION * w / known, v) for w, v in terms]
        else:
            terms.append((W_COMPLETION, completion))

        raw = sum(w * v for w, v in terms)

        # Favouriting a game clears its abort penalty. With no dislike button,
        # this is the user's only way to overrule a bad inference -- a game
        # that crashes on launch, or that you keep opening just to check a mod.
        abort_rate = 0.0
        if gid not in favorites:
            n_abort = aborts.get(gid, 0)
            n_engaged = engaged_counts.get(gid, 0)
            if n_abort + n_engaged > 0:
                abort_rate = n_abort / (n_abort + n_engaged)

        interest[gid] = raw * (1.0 - ABORT_PENALTY * abort_rate)

    # Behavioural negatives: launched repeatedly, never actually played, and
    # not rescued by a favourite.
    disinterest = {}
    for gid, n_abort in aborts.items():
        if gid in favorites:
            continue
        if engaged_counts.get(gid, 0) > 0:
            continue
        if n_abort >= MIN_ABORTS_FOR_DISINTEREST:
            # More aborts is stronger evidence, saturating so one pathological
            # game cannot dominate the negative profile.
            disinterest[gid] = min(1.0, n_abort / 4.0)

    stats = {
        "reference_time": reference,
        "engaged_sessions": sum(engaged_counts.values()),
        "engaged_games": len(engaged_counts),
        "abort_sessions": sum(aborts.values()),
        "neutral_sessions": len(sessions) - sum(engaged_counts.values()) - sum(aborts.values()),
        # Which games were actually PLAYED, as opposed to merely hearted. The
        # interest dict deliberately merges the two, so without this a caller
        # cannot tell them apart -- and explain() was telling users "Because you
        # played Mabinogi" about a game with zero sessions.
        "engaged_ids": set(engaged_counts),
    }
    return interest, disinterest, stats
