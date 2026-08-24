"""Profile construction, ranking and diversification.

Kept separate from recommend.py (which owns database and file I/O) so the whole
ranking pipeline can be exercised offline.

Final score, both terms on [0, 1]:

    score(g) = 0.80*sim(g) + 0.20*qual(g)

`sim` is cosine against the mood-blended taste profile. The previous model
divided a 0-100 IGDB rating by 10, so the rating term spanned 0-1.84 while
similarity spanned 0-0.8 -- rating had ~2.3x the dynamic range of the
personalisation signal and dominated the ranking. Capping quality at 0.20
against similarity's 0.80 is the numeric statement of that fix.

There is no novelty term. Owned and unowned games are ranked in two separate
lists, so a bonus for being unowned would only bias one list against itself.
An earlier single-list version needed it; separating the lists made it
redundant, and its weight went back to similarity where it does work.

Mood does not appear as a term. It is blended into the profile instead, so it
changes what "similar" *means* and propagates through both the cosine ranking
and the diversity pass.
"""

import sys

import numpy as np
from sklearn.metrics.pairwise import cosine_similarity

# --- Score weights ---------------------------------------------------------
W_SIMILARITY = 0.80
W_QUALITY = 0.20

# --- Rocchio ---------------------------------------------------------------
# Higher than classical Rocchio's 0.15-0.25 because this negative set is
# behavioural and precise, not pseudo-relevance-derived.
BETA_NEGATIVE = 0.35

# --- Mood blending ---------------------------------------------------------
# Steady-state weight of the mood centroid against the play-history profile.
#
# Lowered back to 0.30 when the taste profile itself became mood-conditioned
# (see MOOD_RELEVANCE_GAMMA). Before that the profile was mood-blind, so the
# centroid was the only thing carrying mood and it had to be worth half the
# signal -- the overlap numbers that justified 0.50 were measured under exactly
# that regime and no longer describe this code:
#     mood-BLIND profile, discover overlap:  0.30 -> 0.50   0.50 -> 0.20
# Now that the history is itself filtered by mood, 0.50 applies mood twice. The
# measured effect is not subtle: at 0.50 Competitive leaned on whatever carried
# the rarest labels (Off The Grid, Blood Strike) rather than on games resembling
# the user's own, and only 8% of its top 12 survived dropping the centroid
# entirely. At 0.30 the centroid nudges an already mood-shaped profile.
# Raised from 0.30 after the Weights.md revision, because at 0.30 the mood could
# not outvote the play history: Relaxed returned Darksiders III, Nioh 3 and Black
# Myth: Wukong, and its Discover list overlapped Neutral's by 0.58 -- picking a
# mood barely changed anything. The cause was structural rather than any single
# weight, since 70% of the profile was taste and the taste is action-heavy.
#
# Measured sweep on the live profile (Discover section, deterministic):
#     blend   hack-and-slash picks   Relaxed/Neutral   mean inter-mood   sim to own games
#      0.30          8/12                 0.58             0.125              0.384
#      0.45          5/12                 0.33             0.056              0.314
#      0.50          4/12                 0.25             0.042              0.295
#      0.55          2/12                 0.17             0.042              0.274
#      0.60          2/12                 0.08             0.014              0.239
#
# 0.55 buys the whole of the hack-and-slash fix -- 2/12, the same as 0.60 -- and
# separates the moods, while giving up less personalisation. Going on to 0.60
# costs another 13% of similarity to the user's own games to move Relaxed/Neutral
# from 0.17 to 0.08, which is the worse half of the trade.
MOOD_BLEND = 0.55
MOOD_COLD_START_GAMES = 5  # engaged games needed before mood stops dominating

# --- Mood conditioning of the play history ---------------------------------
# Exponent applied to a played game's mood fit when it is reweighted below.
#
# 1.0 would be a plain linear gate, and measurement showed that is too weak: a
# game with a large interest weight still dominated a mood it barely matched.
# Under Relaxed, Stellar Blade held 39.9% of the profile at 1.0 on playtime
# alone. At 2.0 the gate is quadratic and Life is Strange leads at 44.7%, which
# is the behaviour this exists to produce -- fit decides, playtime breaks ties
# among things that fit.
#
# This is the dial to turn if a mood feels too narrow: lower toward 1.0 to
# broaden, since squaring a cosine in [0,1] pushes weak matches toward zero far
# faster than strong ones.
MOOD_RELEVANCE_GAMMA = 2.0

# --- "Only well-known games" filter ----------------------------------------
# Thresholds for the opt-in Settings toggle. Off by default; when on, a Discover
# candidate must clear at least one of the three arms in curated_mask().
#
# Read off the live catalog (6,288 PC rows), not chosen by feel:
#     total_rating_count  p70 53   p80 93   p90 206   p95 415
#     rating              p70 78.6 p80 80.7 p90 84.4  p95 86.9
# p80 keeps 1,258 games on the popularity arm and 1,642 across the union, which
# is ~100x the 12 Discover slots -- the pool stays far wider than MMR_POOL, so
# the diversity work of the previous round is unaffected.
#
# These are absolutes standing in for percentiles of TODAY'S catalog. After a
# large `igdb_catalog.py --refresh` they may no longer sit at p80; recompute the
# percentile rather than guessing a replacement constant.
CURATED_MIN_VOTES = 93.0
CURATED_MIN_RATING = 80.0
CURATED_RATING_MIN_VOTES = 30.0

# There is no live trending signal in the schema -- IGDB `hypes`/follows are not
# fetched -- so recency plus traction is the only available proxy, and it is a
# weak one: just 67 games qualify. Popularity and rating do nearly all the work
# of this filter. Kept because "trending" was asked for, and because a strong
# new release should not have to wait to accumulate votes.
CURATED_TRENDING_DAYS = 730
CURATED_TRENDING_MIN_VOTES = 50.0

# --- Rating shrinkage ------------------------------------------------------
# A title needs roughly this many ratings before its own score outweighs the
# catalog mean. Also removes the old hard-coded 50.0 default for unrated games.
SHRINKAGE_M = 30.0

# --- Diversity -------------------------------------------------------------
MMR_LAMBDA = 0.75
# How deep into the ranking MMR is allowed to look. This bounds the O(pool*k)
# redundancy comparisons; it was never meant as a quality judgement, but at 50
# it acted as one. With ~5,000 mood-eligible candidates it meant everything
# below rank 50 was unreachable no matter how often the user refreshed -- the
# catalog was functionally 50 games, and measured Discover output held only 65
# distinct titles across 25 refreshes.
MMR_POOL = 150

# Titles one studio may contribute to a single section.
#
# Needed because MMR cannot break these clusters up. After the mood filter every
# surviving candidate is label-identical -- measured pairwise similarity inside
# the Immersive top 12 ran 0.87 to 1.00, with Darksiders and Darksiders II at
# exactly 1.000 against God of War Ragnarok. When everything is equally
# redundant the redundancy term is a constant and MMR degenerates into plain
# score ranking, which is why LOWERING MMR_LAMBDA made it worse (4 -> 6
# Yakuza-family titles at 0.50). Diversity has to come from an axis that is not
# labels, and `developer` is one the catalog already carries in full.
#
# Two entries from one studio is a series; four is the same game four times.
MAX_PER_DEVELOPER = 2

# --- Exploration -----------------------------------------------------------
# The ranking pipeline is otherwise fully deterministic and takes no wall-clock
# input, so the same database state always produced a byte-identical list --
# pressing refresh could not change anything. Worse, replaying a game you
# already play moves its interest weight by ~2% and leaves the candidate set
# identical, because a played game is already excluded. The model had nothing
# left to learn; what was missing was a reason to show you something else.
#
# Two mechanisms, both applied to the score just before MMR runs:
#
#   fatigue  demotes games that were served in recent runs, using the
#            impression rows recommend.py already writes and never read
#   gumbel   perturbs scores so near-equal candidates trade places
#
# Adding Gumbel noise and taking the top k is exactly sampling k items without
# replacement from a softmax over the scores, so this needs no separate
# sampling loop and leaves MMR untouched -- MMR simply re-ranks the perturbed
# scores and still enforces diversity.
# Temperature is expressed as a FRACTION of each section's own score range,
# not as an absolute number. The two sections have wildly different score
# geometry -- measured on real data, the gap between rank 10 and rank 15 is
# 0.1076 in the library (25 candidates, well spread) but 0.0057 in discovery
# (5,757 candidates, very tightly packed), a ~19x difference.
#
# A single absolute temperature therefore cannot work: 0.06 left the library
# almost untouched (100% of its deterministic quality retained, so no
# freshness at all) while scrambling discovery to 72.8% quality -- it surfaced
# Pepsiman, at score 0.340, in a list whose deterministic floor was 0.753.
#
# Scaling to the local range gives one dimensionless knob that behaves the same
# way in both sections. At 0.25, measured: library 25% churn / 96% quality
# kept, discovery 37% churn / 99% quality kept.
EXPLORE_ALPHA = 0.25
# How many recent runs are remembered, and how many appearances within that
# window earn the full penalty. These were ONE constant, and that was a trap:
# the same value set the window and divided the count, so widening the memory
# diluted the penalty by exactly as much. Measured, window 5 -> 15 with nothing
# else changed made the worst repeat WORSE, 52% -> 64%. Separating the two roles
# is what makes the window usable as a dial at all.
FATIGUE_RUNS = 10
FATIGUE_SATURATES_AT = 5

# Raised from 0.15. Note this is now larger than the entire visible score range
# of the Discover section (rank 1 to rank 10 is ~0.048), so fatigue is the
# dominant term for anything served recently. That is deliberate and is what
# moves the numbers -- measured 52% -> 24% worst repeat, 65 -> 142 distinct
# titles, at a 4% cost in mean score.
FATIGUE_WEIGHT = 0.30


def fatigue_penalties(impression_counts, index_of, n_items):
    """Per-item demotion for having been shown recently.

    impression_counts maps game_id -> times served across the last
    FATIGUE_RUNS distinct runs. A game shown FATIGUE_SATURATES_AT times within
    that window takes the full FATIGUE_WEIGHT; one shown once takes a fifth of
    it. Saturating short of the window length is what lets the memory be long
    without weakening the penalty.
    """
    penalties = np.zeros(n_items)
    if not impression_counts:
        return penalties
    for game_id, count in impression_counts.items():
        row = index_of.get(game_id)
        if row is not None:
            penalties[row] = FATIGUE_WEIGHT * min(1.0, count / FATIGUE_SATURATES_AT)
    return penalties


def section_temperature(scores, rows, k=10, alpha=EXPLORE_ALPHA):
    """Absolute temperature for one section, from its own visible score range.

    The range is rank 1 to rank k -- the spread the user actually sees. Using
    the whole pool's range instead would be dominated by the long tail of poor
    matches and would barely perturb the top at all.
    """
    if alpha <= 0 or len(rows) < 2:
        return 0.0
    ordered = np.sort(scores[np.asarray(rows)])[::-1]
    visible = ordered[0] - ordered[min(k, len(ordered)) - 1]
    return alpha * float(visible)


def gumbel_perturb(scores, rng, temperature):
    """scores + T * Gumbel(0,1), i.e. Gumbel top-k sampling.

    Clipped away from 0 and 1 because -log(-log(u)) is infinite at both ends
    and a single infinity would pin one game to the top of every draw.
    """
    if temperature <= 0:
        return scores
    u = np.clip(rng.random(len(scores)), 1e-12, 1.0 - 1e-12)
    return scores + temperature * (-np.log(-np.log(u)))

# Mood label sets, defined over the vocabulary that actually exists in the
# data. The previous sets referenced Sandbox, Simulation, Horror and
# "Story Rich": IGDB's genre is `Simulator` (not Simulation), and Sandbox and
# Horror are *themes*, which were never fetched -- so mood 0 collapsed to
# "Adventure" and mood 2 to "RPG". Theme entries below light up once
# igdb_catalog.py populates the themes column.
# Weights, not membership. A flat set could only ever pull toward a label; it
# had no way to say "a Relaxed list should actively avoid horror". Signed weights
# do, and the mood vector below is built to carry them.
#
#   +++ = 3   ++ = 2   + = 1   - = -1   -- = -2   --- = -3
#
# Genres sit at ++ throughout: a genre says what a game IS rather than what it
# is about, so it is a solid positive, but it should not outrank an explicit
# +++ theme choice.
#
# `Casual` was dropped from Relaxed -- it matches nothing in the data, the third
# such dead label this project has hit after `Simulation` and `Story Rich`.
# mood_vector() now warns rather than letting the next one vanish silently.
#
# Neutral is id 3 rather than 0 so the existing ids never renumber and a stored
# mood value stays meaningful. Display order is a separate concern, handled in
# the QML picker.
NEUTRAL_MOOD = 3

# IGDB's six game modes -- how a game is played, as opposed to what it is
# (genres) or what it is about (themes).
#
# These are scored on a FLAT idf, unlike every other label. Measured coverage in
# the catalog runs from Single player at 94.6% down to Battle Royale at 0.8%, so
# real idf would have handed Battle Royale a weight of 17.4 against Single
# player's 3.2 -- a `+++` outweighing a `---` more than fivefold, and Competitive
# collapsing into a 51-game battle-royale niche that drowned out its genres.
#
# Rarity means much less here than it does for a genre: Battle Royale being
# uncommon does not make it six times more informative than Multiplayer. With
# only six values, a flat scale is the honest reading of the weights.
GAME_MODES = {
    "single player",
    "multiplayer",
    "co-operative",
    "split screen",
    "massively multiplayer online (mmo)",
    "battle royale",
}

# IGDB keywords that steer a mood, DERIVED rather than chosen.
#
# Method: 2,000 PC games were sampled with their keywords, genres and themes.
# Each game got a mood score from the existing MOOD_LABELS over its genres and
# themes; each keyword was then scored by the mean lift of the games carrying it
# against the corpus mean. The comment on every line is that measured lift and
# the number of sampled games supporting it. Bands are relative to each mood's
# own maximum lift, because the moods have different weight ranges and an
# absolute cutoff would not compare across them.
#
# A handful that scored well were dropped on judgement, because they are narrow
# proxies for something the genres already capture and would read as bizarre on
# a card: `fascism` and `united states army` (Relaxed negatives, really just
# WWII shooters), `rail shooting segment` (Competitive positive, actually a
# single-player mechanic), `anti-cheat system` (a storefront fact).
#
# One honest limitation: lift was computed against the CURRENT mood weights, so
# this reinforces the existing definition of each mood rather than discovering a
# dimension the genres are missing.
MOOD_KEYWORDS = {
    0: {  # Relaxed
        "cute": 3, "kid friendly": 3, "graphic adventure": 3,
        "funny": 3, "relaxing": 3, "casual": 3, "cartoony": 3,
        "family friendly": 3,
        # `hand drawn` was requested with a space; IGDB spells it hyphenated and
        # the space form matches nothing at all.
        "hand-drawn": 2,
        "colorful": 2, "episodic": 2, "jrpg": 2,
        # Cosy-indie texture: how a relaxing game tends to look and play.
        "turn-based rpg": 3, "exploration": 3, "puzzle platformer": 3,
        "roguelike": 2, "pixel art": 2, "pixel graphics": 2, "2d": 2, "2.5d": 2,
        "atmospheric": 2, "beautiful": 2,
        "anime": 1, "walking simulator": 1,
        # Stands in for the requested `--horror`, which is not an IGDB keyword --
        # horror exists only as a theme, and Relaxed already carries it at -3.
        "psychological horror": -2,
        "objective-based team gameplay": -3, "special forces": -3,
        "camouflage": -3, "team deathmatch": -3, "guerilla warfare": -3,
    },
    1: {  # Competitive
        "squad tactics": 3, "squad based shooter": 3, "guerilla warfare": 3,
        "special forces": 3, "squad": 3, "bots": 3, "close quarters combat": 3,
        "weapon modification": 3, "objective-based team gameplay": 3,
        "enemy tagging": 3, "griefing": 3, "sniping": 3,
        # 11 games in the whole of IGDB. Below the frequency floor, so it exists
        # here only because filter_keywords() force-keeps mood keywords -- it
        # will not move a list on its own.
        "arena shooter": 3,
        "graphic adventure": -3, "relaxing": -3, "walking simulator": -3,
        "hand-drawn": -2, "casual": -2, "family friendly": -2,
    },
    2: {  # Immersive
        "hallucination": 3, "radiation": 3, "apocalypse": 3, "amnesia": 3,
        "detective": 3, "survival horror": 3, "attributes": 3, "immersive": 3,
        "pickpocketing": 3, "psychological horror": 3, "soulslike": 3,
        "two-handed weapons": 3,
        # Narrative and atmosphere, the other half of what makes a world absorbing.
        "story driven": 3, "story rich": 3, "narrative": 3, "jrpg": 3,
        "atmospheric": 3, "realistic": 3,
        "graphic adventure": 2, "exploration": 2, "beautiful": 2,
        "dungeon crawler": -3,
        "kid friendly": -3, "funny": -3, "cartoony": -3,
        "motorsports": -2, "drifting": -2,
    },
    # Neutral takes none, like every other label class.
}

# Membership set for the IDF damping below, built from the maps above so the two
# can never drift apart.
MOOD_KEYWORD_NAMES = {k.lower() for weights in MOOD_KEYWORDS.values() for k in weights}

MOOD_LABELS = {
    # Relaxed -- light, social, warm. Actively pushes away from tension.
    #
    # `Action` is deliberately absent rather than negative. It sits on 62% of
    # the catalog, so as a positive it dragged Relaxed toward generic blockbusters
    # (Elden Ring, God of War) regardless of the other weights; as a negative it
    # would suppress two thirds of everything. Omitting it lets the remaining
    # labels do the steering, and an action game can still qualify on Comedy or
    # Racing rather than on being an action game.
    0: {
        "Comedy": 3, "Party": 3, "Romance": 3,
        "Fantasy": 2, "Drama": 2,
        "Simulator": 2, "Racing": 2,                          # genres
        # Adventure dropped from ++ to +. At 53% of the catalog it is one of the
        # two broadest labels in this mood, and the same breadth argument that
        # keeps Action out applies to it in weaker form.
        "Adventure": 1,
        "Platform": 2, "Puzzle": 2, "Arcade": 2, "Card & Board Game": 2,
        # Lowered from ++ to +. At ++ it was the single label selecting the mood:
        # 9 of the 12 Relaxed picks matched on it and nothing else -- Darksiders,
        # Nioh 3, Black Myth: Wukong, Path of Exile 2. It is the one Relaxed
        # label that agrees with an action-heavy play history, so it won every
        # slot while the cosy keywords never fired.
        "Hack and slash/Beat 'em up": 1,
        # 43% of the catalog -- the broadest positive in any mood. Watch this
        # one: Action was left out at 62% precisely because a label that wide
        # steers toward whatever is popular rather than toward the mood.
        "Indie": 2,
        "Point-and-click": 1,
        "Kids": 1,                                            # theme
        "Mystery": 1, "Erotic": 1,
        "Horror": -3, "Stealth": -3, "Warfare": -3,
        # game modes
        "Single player": 2, "Massively Multiplayer Online (MMO)": 2,
        "Co-operative": 1, "Battle Royale": -3,
    },
    # Competitive -- skill and opposition.
    #
    # `++ Multiplayer` and a merely `-` Single player are a deliberate revision
    # of the requested `--- Single player`: 90% of Multiplayer games ALSO list
    # Single player, so a hard negative there would have pushed down the very
    # titles this mood exists to surface (only ~278 games are multiplayer-only).
    # Stating it positively via Multiplayer is what actually marks a
    # competitive game.
    1: {
        "Shooter": 2, "Fighting": 2, "Strategy": 2, "Sport": 2,   # genres
        # `Action` removed by request. It was this mood's only theme, so the
        # theme class now contributes nothing here -- Competitive rests on its
        # genres, modes and keywords.
        # game modes
        "Battle Royale": 3, "Multiplayer": 2, "Single player": -1,
    },
    # Neutral -- deliberately empty, and the emptiness IS the feature.
    #
    # Do not "fix" this by adding labels. An empty map makes mood_vector()
    # return None, which makes build_profile() return the unblended taste
    # profile, which is exactly the request: rank purely on play logs,
    # playtime, favourites and similarity to what has actually been played,
    # with no theme or genre thumb on the scale. Expressing it as a mood with
    # no labels rather than an `if mood == NEUTRAL` branch means it cannot
    # drift out of step with the real scoring path.
    NEUTRAL_MOOD: {},
    # Immersive -- absorbing worlds and atmosphere. Pushes away from levity.
    2: {
        "Fantasy": 3, "Science fiction": 3,
        "Open world": 2, "Horror": 2, "Thriller": 2,
        "Drama": 2, "Mystery": 2,
        "Role-playing (RPG)": 2, "Visual Novel": 2,           # genres
        "Stealth": 1,
        "Romance": -2, "Sandbox": -2,
        "Comedy": -3, "Party": -3, "Erotic": -3,
        # Survival flipped from +2 to -3 by request. Note it now pulls against
        # the `survival horror` keyword, still at +3: a survival-horror game
        # gets a strong push and a strong pull at once, and its net position is
        # not readable from either weight alone.
        "Survival": -3,
        # Arcade-y and mechanical genres, as the opposite of an absorbing world.
        "Hack and slash/Beat 'em up": -3, "Point-and-click": -3,
        "Arcade": -3, "Platform": -3,
        "Puzzle": -1,
        # game modes
        "Single player": 3, "Massively Multiplayer Online (MMO)": 1,
        "Co-operative": -2, "Split screen": -2,
        "Multiplayer": -3, "Battle Royale": -3,
    },
}

# Merged after the fact rather than written inline, so the derived keyword
# weights stay visibly separate from the hand-specified genre/theme/mode ones.
# A keyword never silently overwrites a label of another class: the two
# vocabularies are checked for collision at import rather than left to chance.
for _mood, _keywords in MOOD_KEYWORDS.items():
    _clash = {k for k in _keywords if k in MOOD_LABELS[_mood]}
    if _clash:
        raise ValueError(f"mood {_mood}: keyword(s) {sorted(_clash)} collide with "
                         f"an existing genre/theme/mode label of the same name")
    MOOD_LABELS[_mood].update(_keywords)

# These must match the card labels in ui/main.qml's mood picker. They are not
# internal names: explain() puts them straight onto recommendation cards as
# "Fits your <name> mood", so a mismatch means the user picks one word and is
# told another.
MOOD_NAMES = {0: "Relaxed", 1: "Competitive", 2: "Immersive", NEUTRAL_MOOD: "Neutral"}


def mood_positive_labels(mood):
    """Labels a game can match to be considered "in" this mood.

    Negatively weighted labels are the opposite of a match, so anything
    measuring mood coverage or precision must exclude them or the measurement
    inverts.
    """
    return [label for label, weight in MOOD_LABELS.get(mood, {}).items() if weight > 0]


def as_float(value):
    """Coerce a cell to float, mapping anything unusable to NaN.

    Values arrive as Python None from some paths and as numpy NaN from a
    DataFrame column, and `float(nan) is None` is False -- so an `is None` check
    alone silently lets NaN through and poisons the arithmetic downstream. That
    was a real bug in quality_scores(); curated_mask() reads the same two
    columns and would hit it identically, which is why this is shared rather
    than duplicated.
    """
    if value is None:
        return np.nan
    try:
        return float(value)
    except (TypeError, ValueError):
        return np.nan


def curated_mask(ratings, rating_counts, released_at, today=None):
    """Popular OR very well rated OR trending. Returns a boolean array.

    Backs the "only well-known games" setting. Ranking alone cannot express
    this: the model scores fit, and a perfect-fit game with 14 ratings beats a
    great-fit game with 1,300 on similarity every time. Restricting the
    candidate pool is the honest way to say "not that one".

    The three arms are a union, not a conjunction -- a beloved niche game with
    few votes still qualifies on rating, and a brand-new release qualifies
    before it has accumulated either.
    """
    ratings = np.asarray([as_float(r) for r in ratings], dtype=float)
    counts = np.asarray([as_float(c) for c in rating_counts], dtype=float)

    # Missing means unproven, which is exactly what this filter excludes, so
    # NaN becomes 0 rather than being treated as unknown-and-therefore-allowed.
    ratings = np.nan_to_num(ratings, nan=0.0)
    counts = np.nan_to_num(counts, nan=0.0)

    popular = counts >= CURATED_MIN_VOTES

    # The vote floor is what stops a single 100/100 review from qualifying a
    # game nobody has played -- without it this arm admits the exact long tail
    # the setting exists to remove.
    well_rated = (ratings >= CURATED_MIN_RATING) & (counts >= CURATED_RATING_MIN_VOTES)

    trending = np.zeros(len(ratings), dtype=bool)
    if released_at is not None:
        days = _days_since(released_at, today)
        trending = (days >= 0) & (days <= CURATED_TRENDING_DAYS) \
            & (counts >= CURATED_TRENDING_MIN_VOTES)

    return popular | well_rated | trending


def _days_since(released_at, today=None):
    """Age in days per row; NaN (which compares False) where undated."""
    import datetime as _dt

    reference = today or _dt.date.today()
    if isinstance(reference, _dt.datetime):
        reference = reference.date()

    out = np.full(len(released_at), np.nan)
    for i, value in enumerate(released_at):
        if value is None:
            continue
        try:
            if hasattr(value, "to_pydatetime"):
                value = value.to_pydatetime()
            if isinstance(value, _dt.datetime):
                value = value.date()
            elif isinstance(value, str):
                value = _dt.date.fromisoformat(value[:10])
            elif not isinstance(value, _dt.date):
                continue
        except (ValueError, TypeError):
            continue
        out[i] = (reference - value).days
    return out


def _l2(vector):
    norm = np.linalg.norm(vector)
    return vector / norm if norm > 0 else vector


def quality_scores(ratings, rating_counts=None):
    """Bayesian shrinkage toward the catalog mean.

    An unrated game gets exactly the catalog mean -- the correct uninformative
    prior -- rather than an arbitrary constant. Where a rating exists but its
    vote count is unknown (library rows, before the catalog fetch adds
    total_rating_count), confidence is set equal to the prior strength, i.e. a
    50/50 blend of the observed rating and the mean.
    """
    ratings = np.asarray([as_float(r) for r in ratings], dtype=float)

    # A rating of 0 means "not rated" in game_metadata.txt, not "terrible".
    # Treating it as an observation would bury every unrated game.
    known = ~np.isnan(ratings) & (ratings > 0)

    if not known.any():
        return np.full(len(ratings), 0.5)

    scaled = np.where(known, ratings / 100.0, np.nan)
    prior = float(np.nanmean(scaled))

    if rating_counts is None:
        counts = np.where(known, SHRINKAGE_M, 0.0)
    else:
        raw = np.asarray([as_float(c) for c in rating_counts], dtype=float)
        # Unknown vote count (library rows, which IGDB never gave us a count
        # for) is not the same as a count of zero. Zero would shrink the game
        # all the way to the catalog mean and discard its rating entirely;
        # SHRINKAGE_M weights the observed rating and the prior equally, which
        # is the honest reading of "we have a rating but no confidence in it".
        counts = np.where(np.isnan(raw), SHRINKAGE_M, raw)
        counts = np.where(known, counts, 0.0)

    observed = np.where(known, scaled, 0.0)
    return (counts * observed + SHRINKAGE_M * prior) / (counts + SHRINKAGE_M)


def mood_vector(vectorizer, mood):
    """Weighted mood vector in the item feature space, or None if unsupported.

    Built from the vocabulary and IDF directly rather than by transforming a
    pseudo-document. That matters: TfidfVectorizer L2-normalises every document,
    so transforming a single label yields 1.0 no matter how rare the label is --
    the IDF weighting that makes `Visual Novel` more distinguishing than
    `Adventure` would be silently thrown away. Multiplying the caller's weight
    by idf keeps both signals.

    Returns None when no label resolves (e.g. theme labels before the catalog
    has been fetched). Blending a zero vector would produce NaNs, so callers
    fall back to the unblended profile.
    """
    # Returns before the missing-label warning below, so Neutral's empty map is
    # silent rather than reported as a mood whose labels all failed to resolve.
    weights = MOOD_LABELS.get(mood)
    if not weights:
        return None

    vector = np.zeros(len(vectorizer.vocabulary_))
    # One shared idf for all six game modes, so a `+++` carries the same force
    # whichever mode it names. Median rather than a constant so it tracks the
    # corpus instead of drifting as the catalog grows.
    # The reference scale for damped labels: the median idf of THIS mood's own
    # genres and themes, not of the whole vocabulary.
    #
    # It used to be median(vectorizer.idf_), which was 3.75 back when the
    # vocabulary held ~200 genre/theme/mode terms. Keywords then took it to
    # 1,101 terms of which 1,050 are keywords, so that median became 5.95 -- the
    # keywords' own median -- and every damped label inflated with it. A `+++`
    # keyword reached 17.84 against a `+++` theme at 6.97, and `Single player`,
    # on 94% of games, went from 3.75 to 17.84 too. The constant meant to stop
    # rare labels dominating had started handing them the win.
    #
    # Anchoring to the mood's own undamped labels is stable under any future
    # vocabulary change, because it asks the question that actually matters:
    # what does a typical genre or theme weigh in this mood?
    undamped = [vectorizer.idf_[vectorizer.vocabulary_[label.lower()]]
                for label in weights
                if label.lower() in vectorizer.vocabulary_
                and label.lower() not in GAME_MODES
                and label.lower() not in MOOD_KEYWORD_NAMES]
    mode_idf = float(np.median(undamped)) if undamped else float(np.median(vectorizer.idf_))
    missing = []
    for label, weight in weights.items():
        # vocabulary_ keys are lowercased, so a label whose case differs from
        # the stored name still resolves ("Open World" vs "Open world").
        lower = label.lower()
        index = vectorizer.vocabulary_.get(lower)
        if index is None:
            missing.append(label)
            continue
        # Game modes AND keywords take the flat median idf; genres and themes
        # keep their real one. Keywords are held to 1.5-15% of the catalog by
        # igdb_catalog.py while genres run 20-60%, so on real idf a `+++`
        # keyword would outweigh a `+++` theme several times over and Immersive
        # would collapse into whatever `soulslike` happens to match. Exactly the
        # failure game modes produced before they were damped, for exactly the
        # same reason: rarity here is an artefact of how the vocabulary was
        # built, not a measure of how much the label means.
        damped = lower in GAME_MODES or lower in MOOD_KEYWORD_NAMES
        scale = mode_idf if damped else vectorizer.idf_[index]
        vector[index] = weight * scale

    if missing:
        # Loud on purpose. A mood label that matches nothing steers nothing,
        # and silent ones have cost this project three separate rounds.
        print(f"[warn] mood {mood} ({MOOD_NAMES.get(mood, '?')}): no games carry "
              f"{', '.join(sorted(missing))} -- these labels do nothing",
              file=sys.stderr)

    return _l2(vector) if np.linalg.norm(vector) > 0 else None


def mood_fits(item_vectors, mood_vec):
    """Signed alignment of every item with the mood, in [-1, 1].

    Both operands are already L2-normalised -- item_vectors by TfidfVectorizer
    and mood_vec by _l2() -- so the dot product IS the cosine and no further
    normalisation is needed or wanted.
    """
    return item_vectors @ mood_vec


def mood_relevance(item_vectors, index_of, interest, mood_vec):
    """Reweight the play history by how well each game fits the mood.

    Without this, the same heavily-played games shape every mood: Stellar Blade
    alone was 42.9% of the profile under Relaxed, Competitive AND Immersive, so
    Competitive returned Mass Effect 3 and Red Dead Redemption 2 while Valorant
    sat at rank 1,866 of 6,289. Mood decided how candidates were scored but
    never which of the user's games did the deciding.

    Membership was tried first and is a dead end: `Single player` sits on 94.6%
    of the catalog and is positive in two moods, `Action` on 62% and positive in
    the third, so "carries a mood label" keeps 8 of 8 played games in Relaxed
    and Competitive. Degree of fit is what discriminates; membership is not.

    Games with zero or negative fit are dropped from the returned dict rather
    than merely shrunk. That is the point -- Immersive genuinely should not be
    shaped by Counter-Strike 2 -- and dropping them keeps len() an honest count
    of how much relevant history exists.

    Returns `interest` unchanged when mood_vec is None, which is what keeps
    Neutral's contract: no mood parameter touches it.
    """
    if mood_vec is None:
        return interest

    fits = mood_fits(item_vectors, mood_vec)
    relevant = {}
    for game_id, weight in interest.items():
        row = index_of.get(game_id)
        if row is None or weight <= 0:
            continue
        fit = max(0.0, float(fits[row]))
        scaled = weight * (fit ** MOOD_RELEVANCE_GAMMA)
        if scaled > 0:
            relevant[game_id] = scaled
    return relevant


def blend_weight(n_engaged_games):
    """How much of the profile the mood should account for.

    Ramps from 1.0 (no history -- mood is all we have, which is also what makes
    the first run return something instead of an empty tab) down to MOOD_BLEND
    once the user has a real play history.

    Not reached for Neutral, which has no mood vector to ramp toward. On a fresh
    profile Neutral therefore has nothing to rank with: the taste vector is all
    zeros, similarity is 0 for every candidate, and the quality term alone
    decides the order. That is the honest reading of "no data, no preference",
    and it is the one case where Neutral does worse than a mood.
    """
    if n_engaged_games <= 0:
        return 1.0
    if n_engaged_games >= MOOD_COLD_START_GAMES:
        return MOOD_BLEND
    span = 1.0 - MOOD_BLEND
    return 1.0 - span * (n_engaged_games / MOOD_COLD_START_GAMES)


def build_profile(item_vectors, index_of, interest, disinterest,
                  vectorizer=None, mood=None):
    """Rocchio profile over the mood-conditioned history, then mood blending.

    Returns (profile, taste_profile, mood_vec, effective_interest).

    `taste_profile` is the history-only vector, kept so explanations can tell
    whether a pick was driven by play history or by the mood selection.
    `effective_interest` is the reweighted history the profile was actually
    built from; callers need it so an explanation names a game that really
    contributed, and MUST NOT use it as the exclusion set (see recommend.py).
    """
    dim = item_vectors.shape[1]

    mood_vec = mood_vector(vectorizer, mood) if vectorizer is not None and mood is not None else None

    # Mood decides which of the user's games count, before any of them are
    # summed. Only the positive term is conditioned: a game the user repeatedly
    # bounced off is evidence against it in every mood, so filtering the
    # negative set by mood would just discard true negatives.
    effective = mood_relevance(item_vectors, index_of, interest, mood_vec)

    positive = np.zeros(dim)
    for game_id, weight in effective.items():
        row = index_of.get(game_id)
        if row is not None and weight > 0:
            positive += weight * item_vectors[row]
    positive = _l2(positive)

    negative = np.zeros(dim)
    for game_id, weight in disinterest.items():
        row = index_of.get(game_id)
        if row is not None and weight > 0:
            negative += weight * item_vectors[row]
    negative = _l2(negative)

    # Non-negative Rocchio: clipping stops a disliked genre from
    # *anti*-contributing, which would perversely inflate unrelated items.
    taste = _l2(np.clip(positive - BETA_NEGATIVE * negative, 0.0, None))

    if mood_vec is None:
        return taste, taste, None, effective

    # RAW len(interest), deliberately, not len(effective). This ramp asks "does
    # this user have a play history at all", and the answer does not depend on
    # the mood. Feeding it the conditioned count would quietly raise mu above
    # its configured value for exactly the moods that filter hardest --
    # Competitive keeps 4 of 8 games, which would ramp mu from 0.30 to 0.44 and
    # silently undo the MOOD_BLEND setting.
    #
    # The genuine "no relevant history for this mood" case needs no special
    # handling: every game was dropped, so `taste` is the zero vector and the
    # branch below already falls back to the mood alone.
    mu = blend_weight(len(interest))
    if np.linalg.norm(taste) == 0:
        return mood_vec, taste, mood_vec, effective
    return _l2((1.0 - mu) * taste + mu * mood_vec), taste, mood_vec, effective


def score_games(profile, item_vectors, quality):
    similarity = cosine_similarity(profile.reshape(1, -1), item_vectors)[0]
    similarity = np.clip(similarity, 0.0, 1.0)
    return W_SIMILARITY * similarity + W_QUALITY * quality, similarity


def mmr_rerank(candidate_rows, scores, item_vectors, k=10, lam=MMR_LAMBDA,
               group_of=None, max_per_group=None):
    """Maximal Marginal Relevance, with an optional per-group quota.

    Ten games ranked purely by content similarity come back near-identical.
    An item must be more than (1-lam)/lam better to outrank one that duplicates
    something already picked.

    `group_of` maps a row index to a grouping key (the caller passes developer
    names) and `max_per_group` caps how many of each may be selected. This is a
    separate mechanism from the redundancy term above, and it has to be, because
    redundancy is computed over the label vectors -- once a mood filter has run,
    every survivor carries near-identical labels, the redundancy term becomes a
    constant, and MMR silently collapses into plain score ranking. A group quota
    still bites when the vectors cannot tell two games apart.

    A candidate whose group is full is skipped for this round rather than
    dropped, so the quota degrades gracefully: if it would leave fewer than k
    selectable items the remaining slots fill normally instead of returning
    short. An empty or missing group key is exempt -- lumping every unknown
    together would cap unrelated games against each other.
    """
    pool = sorted(candidate_rows, key=lambda r: -scores[r])[:MMR_POOL]
    if not pool:
        return []

    rows = np.asarray(pool)
    vectors = item_vectors[rows]
    # Row-normalise once, then a single gram matrix gives every pairwise cosine.
    # The previous version called cosine_similarity once per (candidate, chosen)
    # pair inside the loop -- O(pool*k) sklearn round trips, which tripling
    # MMR_POOL would have tripled.
    norms = np.linalg.norm(vectors, axis=1, keepdims=True)
    gram = (vectors / np.where(norms > 0, norms, 1.0)) @ (vectors / np.where(norms > 0, norms, 1.0)).T

    pool_scores = np.asarray([scores[r] for r in pool], dtype=float)
    keys = [None] * len(pool)
    if group_of is not None and max_per_group:
        keys = [group_of[r] if group_of[r] else None for r in pool]

    used = {}
    selected = []
    redundancy = np.zeros(len(pool))
    available = np.ones(len(pool), dtype=bool)

    while available.any() and len(selected) < k:
        value = np.where(available, lam * pool_scores - (1.0 - lam) * redundancy, -np.inf)

        if max_per_group:
            allowed = value.copy()
            for i in np.flatnonzero(available):
                key = keys[i]
                if key is not None and used.get(key, 0) >= max_per_group:
                    allowed[i] = -np.inf
            # Only honour the quota while it still leaves something to pick.
            if np.isfinite(allowed).any():
                value = allowed

        if not np.isfinite(value).any():
            break

        i = int(np.argmax(value))
        selected.append(int(pool[i]))
        if keys[i] is not None:
            used[keys[i]] = used.get(keys[i], 0) + 1
        available[i] = False
        redundancy = np.maximum(redundancy, gram[i])

    return selected


# How many cards in one section may cite the same game from the user's profile.
#
# Without a cap, a panel of twelve cards all read "Because you played Stellar
# Blade", which tells the user nothing and looks broken. Three is enough to let a
# genuinely dominant influence show through more than once while forcing the rest
# of the panel to name something else.
EXPLANATION_MAX_PER_SOURCE = 3

# Cards per section reserved for a game the user has actually PLAYED, drawn
# fresh on every refresh rather than fixed.
#
# Needed because ranking sources on similarity alone made every card read
# "Because you liked ...". That is what the data says -- the profile holds 8
# played games against 55 hearted ones, so hearted titles win nearly every match
# on sheer coverage -- but a panel that never mentions anything you played reads
# as broken.
#
# A fixed "prefer played" rule is not the answer either: Wuthering Waves is the
# single best played match for EVERY Relaxed and Immersive card, being the only
# played game carrying fantasy/RPG/open-world labels, so preferring played would
# just swap one monotonous panel for another. EXPLANATION_MAX_PER_SOURCE is what
# saves it -- forced past each card's top played match, 4-6 distinct played
# sources become usable per mood.
#
# The dials, both live:
#   MIN at 0 means some refreshes cite nothing played at all. Raise to 2-3 if
#       that reads badly.
#   MAX at None means a refresh can cite played games on every card, and the
#       weakest of those sit near 0.55 similarity. Lower it to tighten quality.
PLAYED_QUOTA_MIN = 0
PLAYED_QUOTA_MAX = None   # None = the section's card count


def _explanation_sources(rows, item_vectors, interest, index_of, names, played_ids):
    """Per row, the user's own games ranked by similarity, most similar first.

    Ranked on similarity ALONE -- deliberately not weight * similarity, which is
    what the profile is built from. Two different questions: the profile asks
    "how much should this game shape my taste", the card asks "which of my games
    is this one like". Mixing them made the answer wrong in both directions.

    With mood conditioning the effective weights collapse to ~0.01 and sit
    near-tied, so the product was decided by dict order rather than by anything
    meaningful and one arbitrary title won every card in the panel. Meanwhile the
    similarity-only answers were sitting right there and were near-exact --
    measured 1.00 for Monster Hunter: World against Ys VIII, 0.98 for Aion 2
    against Tibia.

    The weight still gates membership: a game with no positive weight is not in
    `interest` at all, so a mood-irrelevant title can never be cited.
    """
    played_ids = set(played_ids)

    sources = []
    for game_id, weight in interest.items():
        source = index_of.get(game_id)
        if source is None or weight <= 0:
            continue
        sources.append((source, names[source], game_id in played_ids))

    if not sources or len(rows) == 0:
        return [[] for _ in rows]

    source_rows = np.asarray([s[0] for s in sources])
    candidates = item_vectors[np.asarray(rows)]
    library = item_vectors[source_rows]

    # One gram matrix instead of a cosine_similarity call per (card, source)
    # pair, which at 12 cards x 63 profile games was 756 sklearn round trips.
    def unit(matrix):
        norms = np.linalg.norm(matrix, axis=1, keepdims=True)
        return matrix / np.where(norms > 0, norms, 1.0)

    similarity = unit(candidates) @ unit(library).T

    ranked = []
    for i in range(len(rows)):
        order = np.argsort(-similarity[i])
        ranked.append([(float(similarity[i][j]), sources[j][1], sources[j][2])
                       for j in order if similarity[i][j] > 0])
    return ranked


def explain_section(rows, item_vectors, interest, index_of, names, taste_profile,
                    mood_vec, mood, played_ids=(),
                    max_per_source=EXPLANATION_MAX_PER_SOURCE,
                    played_quota=None):
    """Reasons for a whole section at once, so they can be kept varied.

    Explanations used to be chosen one card at a time, which is why every card
    in a panel could name the same game -- nothing in the per-card decision knew
    what the other eleven cards had already said. Assigning the section together
    is the only way to bound the repetition.

    `played_quota` reserves that many cards for a game the user has actually
    PLAYED. None means no reservation, which is what offline callers get so
    evaluation stays deterministic. The reservation exists because ranking on
    similarity alone makes every card say "liked": there are 55 hearted games
    against 8 played ones, so hearted titles win nearly every match.

    Reserved cards are chosen STRONGEST FIRST -- the cards whose best played
    match is closest -- so the citations the quota forces are the most
    defensible ones available, and a high quota degrades gracefully instead of
    pinning weak matches onto the top of the panel.
    """
    ranked = _explanation_sources(rows, item_vectors, interest, index_of, names,
                                  played_ids)
    used = {}
    reserved = {}

    # "genres you play" is a claim about the user's history exactly as "Because
    # you played" is, and it needs the same guard. Three states, because the
    # profile has three: something played in it, hearts only, or empty. The
    # hearts-only user is the one who has never launched a game through Vortex,
    # or has the "ignore games you've played" setting on; an empty profile is
    # both ignore settings on at once, and a card picked with no profile at all
    # cannot claim either kind of history.
    played_ids = set(played_ids)
    if any(game_id in played_ids for game_id in interest):
        quality_reason = "Highly rated in genres you play"
    elif interest:
        quality_reason = "Highly rated in genres you like"
    else:
        quality_reason = "Highly rated"

    if played_quota:
        # Best played match per card, ignoring hearted sources entirely.
        best_played = []
        for i in range(len(rows)):
            for similarity, name, was_played in ranked[i]:
                if was_played:
                    best_played.append((similarity, i, name))
                    break

        # Strongest first. Ties break on card order, which keeps the whole
        # function reproducible for a given run_id.
        best_played.sort(key=lambda item: (-item[0], item[1]))

        for _, i, _name in best_played:
            if len(reserved) >= played_quota:
                break
            for _, name, was_played in ranked[i]:
                if not was_played:
                    continue
                if used.get(name, 0) < max_per_source:
                    reserved[i] = (name, True)
                    used[name] = used.get(name, 0) + 1
                    break
        # Fewer played matches than the quota asked for is fine and expected --
        # a mood gate can leave no played sources at all. The quota is a target.

    reasons = []
    for i, row in enumerate(rows):
        vector = item_vectors[row]

        mood_fit = float(np.dot(vector, mood_vec)) if mood_vec is not None else 0.0
        taste_fit = (float(np.dot(vector, taste_profile))
                     if np.linalg.norm(taste_profile) else 0.0)

        chosen = reserved.get(i)
        already_counted = chosen is not None
        if chosen is None:
            for _, name, was_played in ranked[i]:
                if used.get(name, 0) < max_per_source:
                    chosen = (name, was_played)
                    break
            # Every source is at its cap: fall back to the single best match
            # rather than inventing a vaguer reason. Better a repeat than a card
            # that explains nothing.
            if chosen is None and ranked[i]:
                chosen = (ranked[i][0][1], ranked[i][0][2])

        if chosen is None:
            reasons.append(quality_reason
                           if mood_fit <= 0 else
                           f"Fits your {MOOD_NAMES.get(mood, 'chosen')} mood")
            continue

        name, was_played = chosen
        # "played" is a claim about the user's history and has to be true. A
        # hearted game they never launched gets "liked" instead.
        verb = "played" if was_played else "liked"

        # A reserved card always states its link. Letting the mood override it
        # would silently shrink the quota the caller asked for.
        if i in reserved or taste_fit >= mood_fit or mood_fit <= 0:
            # Count the source only when it is actually printed. Counting at
            # selection time would let a card that ends up showing the mood
            # reason still burn a slot of that source's cap.
            if not already_counted:
                used[name] = used.get(name, 0) + 1
            reasons.append(f"Because you {verb} {name}")
        else:
            reasons.append(f"Fits your {MOOD_NAMES.get(mood, 'chosen')} mood")

    return reasons


def inspiration_sources(rows, item_vectors, interest, index_of, names,
                        played_ids=(), top_n=3):
    """Per row, the user's own games that most resemble it.

    The same ranking explain_section() picks its one-line reason from, exposed
    whole. That function has to choose a single source and spread citations
    across a panel, so it throws the rest away -- but the discarded part is the
    actual evidence: "Because you played X" is an assertion, and this is what
    makes it checkable.

    Deliberately a separate function rather than a second return value from
    explain_section(): that one is called by evaluate.py and four tests, and
    widening its contract to carry diagnostics would be a change to a
    measurement path for the benefit of a display path.

    Returns one list of {name, similarity, played} dicts per row, most similar
    first, JSON-ready for recommendations.json.
    """
    ranked = _explanation_sources(rows, item_vectors, interest, index_of, names,
                                  played_ids)
    out = []
    for entry in ranked:
        out.append([
            {"name": name,
             "similarity": round(float(similarity), 4),
             "played": bool(was_played)}
            for similarity, name, was_played in entry[:top_n]
        ])
    return out


def explain(row, item_vectors, interest, index_of, names, taste_profile, mood_vec,
            mood, played_ids=()):
    """Single-card convenience wrapper over explain_section()."""
    return explain_section([row], item_vectors, interest, index_of, names,
                           taste_profile, mood_vec, mood, played_ids)[0]
