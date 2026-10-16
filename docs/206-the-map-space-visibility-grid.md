# 206 — The map-space visibility grid

Status: **found, adopted at runtime, and holding.** `LB_FOGSRC=map` locates the
grid per battle, verifies it across three views, uses it, and polices it every
frame. Not yet the default — see the end.

The history below is kept in full. Two grids were found before the right one,
and the difference between them is the interesting part.

## The problem

The widened columns took visibility from a cache accumulated out of the guest's
own 32-column window, so anything the window had never covered defaulted to
shrouded. Safe, but visibly wrong: the fog came out as a dead-straight vertical
wall exactly at the native boundary while fog inside the window followed the
terrain.

## The wrong grid, and why it was wrong

The first search adopted `0227DEB0` and the frame-by-frame check rejected it
within moments:

    [lb_findfog] ADOPTED 0227DEB0 stride 64 after 3 views
    [lb_findfog] DROPPED 0227DEB0: 68/132 disagree with the guest's own fog

That was not a coincidental match — 108 yes/no samples agreeing by chance is
about 2^-108 per offset. The buffer encodes visibility, just not the kind the
shroud is drawn from: **currently-visible** (what the player's units can see
now) rather than **explored** (what they have ever seen).

**The search's own filter is what let it through.** Samples were restricted to
metatiles whose six tiles all agreed. Near the player's base a unanimous
metatile is either fully explored *and* currently occupied, or wholly
unexplored — exactly the cells where the two records coincide. The search
literally could not distinguish them.

## The fix: sample where the records differ

Classify each metatile by the **majority** of its six tiles instead of
demanding unanimity. That admits the cells that separate the two records:
ground the player has explored but has no unit near. Because a metatile
straddling a fog edge now carries a slightly noisy label, the match tolerates
one miss in twenty rather than demanding perfection — with both classes still
required to be well represented, so a zero-filled or constant region cannot
pass.

With that change the search finds a different buffer, and it holds:

    [lb_findfog] ADOPTED 0215E6ED stride 64 after 3 views      (no DROPPED)

Reproduced at the same address across separate runs. `0215E6ED` also sits just
above the game's static globals — the map pointer is at `02156CB0` — where a
persistent per-map record would be expected, unlike `0227DEB0` which was far up
in the heap.

## Layout

    byte per METATILE, row stride 64, non-zero = revealed
    revealed(mx, my) = *(u8*)(base + my * 64 + mx) != 0

## The mechanism, which matters more than the address

* **Discovery** by correlation against known-true visibility, on both classes.
* **Verification** across three distinct views before adoption.
* **Policing** every frame against `g_seen`, which is exact for the 32 columns
  the guest actually draws, with automatic drop and rediscovery.

Nothing here trusts a hard-coded address, and a wrong answer was caught by the
port itself in the same session it was proposed.

## Result

The left widened band carries revealed terrain continuing naturally out of the
native region, with an organic fog boundary following the map. The straight wall
is gone, and so are the blocky metatile patches the rejected candidate produced.

## Agreement is not enough, and that took a third wrong candidate to learn

A later run adopted `0227C5AB` instead, and it agreed with the guest's own fog
on **92.4%** of the window -- comfortably past any agreement threshold -- and
was still the wrong buffer:

    [lb_vis] 0227C5AB revealed 2206/4096  SHRANK  agrees with guest fog 92.4%

Ground once explored is explored forever, so an **explored record's revealed
count can only rise**. A single decrease rules it out. Current visibility rises
and falls as units move, which is exactly what a shrink looks like.

Monotonicity is therefore an **adoption criterion**, not a line in the log: a
shrink disqualifies the address immediately, blacklists it for the session, and
restarts discovery. Agreement alone provably cannot separate "explored" from
"currently visible" -- three candidates have now passed it and been wrong.

## The search is view-dependent, which is its real weakness

Different runs adopt different addresses, because what the search can
distinguish depends on the view it happens to sample:

* a panned view came back **107 revealed against 9 shrouded**, and on nine
  samples of a class the search matched an unrelated family;
* balanced views (around 38/82) found `0215E6ED` run after run.

So the sample set now has to be balanced -- each class at least a fifth of the
total -- rather than merely "at least 8 of each". The caller simply retries
until a view with a real fog edge across it comes along, which costs nothing.

## Where it stands

`0215E6ED` survives all three checks: it is found from balanced views, agrees
with the guest's own fog on **93.9%** of the window every frame, and never
shrinks.

**The monotonic evidence for it is weak, and should be called weak.** The
harness never moves units, so the revealed count is *constant* for a whole run
rather than growing. "Never shrank" is a genuine test that disqualified another
candidate; "never grew" is not yet confirmation. Watching the count rise as
ground is actually explored is the test still owed.

## PARKED. The map-grid source is experimental and stays off.

Two further results ended this line of work for now.

**The growth test could not be made to run.** An explored record's count must
rise, and confirming that needs ground to actually be explored. `gx_polygons`
returns an empty list at the point the harness queries it, so units cannot be
located that way, and an 18-point tap sweep with move orders left the count at
exactly 515/4096 throughout. That is not evidence either way: it is equally
consistent with "nothing moved" and with "this is not the explored record",
and the two cannot be separated without confirming a unit actually walked.

**Worse, the source produces a visible artefact.** With `LB_FOGSRC=map`, after
the camera has moved, the widened band breaks into vertical stripes of
alternating terrain and fog roughly a metatile wide
(`screenshots/exp-explore2/explore-after.png`). The default window-derived path
does not do this. Whatever the grid is, the way it is being indexed is wrong at
some camera positions.

So: **the switch stays off and is labelled experimental.** The conservative
window-derived cache is the default, is artefact-free, and never reveals ground
the game is hiding. The fog wall it produces at the native boundary is a
cosmetic conservatism, which is a far better failure than striping.

What was actually worth having from this line of work is the **method**, and it
stands on its own: correlate against known-true state, verify across several
independent views, require a balanced sample, and police the result forever with
an invariant the right answer cannot violate. It caught three wrong answers.

## Why it is still not the default



* Verified on one map (`The Pond`) only. A second map would confirm both the
  address and the layout.
* Which of several surviving candidates this is — the player's own record, a
  shared one, or a copy — is not established. It agrees with what the player
  sees on every view sampled, which is the property that matters, but that is
  not the same as knowing what it is.
* Discovery costs a 4 MB scan per battle. Fine as an opt-in; worth measuring
  before it runs for everyone.

The conservative window-derived cache remains the default until those are
settled.
