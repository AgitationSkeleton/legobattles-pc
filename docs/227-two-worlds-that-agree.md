# 227 — Two worlds that agree

**Goal within the project:** a thoroughly implemented and tested master server
and multiplayer. The audit has carried *desync over a
long match* as **not measured** since it was written, for the plain reason that
nothing existed to measure it with.

## What a desync test should compare

The cartridge has its own `SyncCheckCommand` (docs/87), carrying two 32-bit
fields. Reading it means parsing the game's command stream out of the wireless
payload, and what it yields is **what the game believes about itself** — a
second thing to trust rather than an independent check. A port whose sync check
agrees while the two screens show different battles has learned nothing.

Desync means the two machines' worlds have diverged, so compare the worlds. Both
instances already expose a debug server, so `lb_mp.py --desync` samples the
entity pool from each — the pool's count, and **every entity's position** — and
reports the first sample where they differ, naming the entity and both values.

Positions and not merely the count, deliberately: two sides can hold the same
number of units while one has walked somewhere the other has not, which is the
ordinary way an RTS desyncs and is exactly what a count cannot see.

## The first run reported success without measuring anything

```
desync: sample 36 -- one side not in a battle yet
desync: 36 sample(s), IN SYNC throughout
```

Thirty-six consecutive samples each said one side was not in a battle, and the
summary announced sync. Nothing had been compared. That is the same shape as
every measurement this project has had to throw away, and it is why the tool now
counts the samples that actually compared two live battles and says

```
desync: N sample(s) but NONE compared two battles -- this run measured nothing.
```

when that count is zero. A test that cannot fail is not a test.

## Getting two instances into one battle

The cause was ordinary: the `mp` route reaches the multiplayer **menu**, not a
match. docs/216 had got two instances into a live battle but recorded which
screens they traverse rather than the coordinates, and its captures are numbered
rather than coordinate-named, so the sequence was not recoverable. Re-deriving
it cost four runs, and two things went wrong that are worth keeping:

**The client must search for about a minute before tapping.** Given fourteen
seconds, the capture at that step shows the list still reading "Searching for
Games...", so the tap hit an empty box and the next landed on a list
mid-populate. The game answered *"Could not join this game, please try
another."* — which reads exactly like joining being broken.

**The lobby entry row is at y = 47, not 60.** 60 is inside the list box but
below the first row. The same class of error as docs/216's own note about the
Join icon being at x 173 rather than the 143 the screenshot suggests.

And one step was simply missing: **`212,174` is Continue**, offered on the
host's Game Lobby only once a second player is listed. Without it both sides sit
ready in the lobby for ever. The full lockstep table is now in docs/216 so this
is not re-derived a third time.

## Measured

```
desync: sample 1 in sync, 16 entities
...
desync: sample 18 in sync, 16 entities
desync: 18 sample(s), 18 compared two live battles, IN SYNC throughout
```

`screenshots/mp-hj9/i0-final.png` is the host in a live battle with a red base
and red units; `i1-final.png` is the client with a blue base on the same map.
Sixteen entities on both sides, identical positions, every sample.

`[local_mp]` line counts tell the same story from the transport's side: 17 and 5
on the run whose join failed, **302 and 444** on the run that reached a battle.

## Twelve minutes

```
desync: sample 75 in sync, 16 entities
desync: 75 sample(s), 75 compared two live battles, IN SYNC throughout
```

Seventy-five samples over a twelve-minute match, every one comparing two live
battles, every one identical. That is the audit's "long match" answered.

## The weakness that result still has, stated before anyone else finds it

Sixteen entities, identical positions, seventy-five times running. **If nothing
MOVED, that is two static snapshots matching, and a desync test that cannot see
motion cannot see a desync.** Both sides were idle for the whole twelve minutes,
because the tap sequence gets them into a battle and then stops.

The tool now says so itself. It counts the samples whose signature differs from
the one before, and reports either

    the world changed in N of M comparisons, so this was a moving match

or a warning that the world never changed and the run is weak evidence. A
measurement that can be read as stronger than it is will be, so it has to carry
its own caveat rather than rely on the reader remembering this paragraph.

## What this does and does not establish

**Does:** two instances reach one battle through the cartridge's own lobby, and
their worlds agree — same entity count, same positions — across every sample of
a **twelve-minute** match, checked independently of anything the game says about
itself. That answers the audit's "long match" on duration.

**Does not:** say anything about a desync under LOAD. Both sides sat idle, and
units moving and fighting is where an RTS diverges — an idle match exercises the
lockstep transport but not the simulation that has to stay in step. Idle
agreement is the floor, not the ceiling, and the next run orders units on both
sides and reads the tool's own "the world changed in N of M comparisons" line to
confirm the match was actually moving before believing the result.

Nor does it establish three players (`Play3PlayerMP`, docs/97 — `lb_mp.py` runs
exactly two instances and would need extending), or an actual second machine,
which this environment cannot provide.

---

## Under load: measured

The idle result above is the floor. This is the one that matters.

`--load` orders units to move on both sides throughout the linger. It aims at
where the units actually are — both instances print `[lb_builder]` and
`[lb_hero]` with `LB_ENTLOG=1`, so each one's own log says where its own units
stand — taps the unit, then taps a patch of ground away from it, alternating the
destination so the match keeps moving instead of settling at an edge.

The `+12` in y on the unit tap is not a guess. docs/226's calibration sweep
measured `dy 0` missing the builder and `dy 6` and `dy 12` selecting it; the
sprite is drawn below the position the entity reports. That measurement came out
of the out-of-band selection investigation, which has not closed its own
question — but it is what makes this test aim correctly.

```
desync: the world changed in 13 of 26 comparisons, so this was a moving match,
        not a frozen one
desync: 26 sample(s), 26 compared two live battles, IN SYNC throughout
```

**Half the comparisons saw the world change, and every one of them saw the two
worlds agree.** That is a moving two-player match staying in lockstep, checked
against the entity pool of each instance rather than against anything the game
says about itself.

### The failed attempt before it, which is why the warning exists

The first load run tapped guessed coordinates:

```
desync: WARNING -- the world never changed between samples. Two IDLE worlds
        agreeing is weak evidence; a desync shows up when units move.
desync: 32 sample(s), 32 compared, 1 DIVERGED (first at sample 1, ...)
desync: one divergence, at the very first sample -- that is the two instances
        entering the battle a moment apart, not a desync.
```

The taps selected nothing, both worlds sat frozen, and without that warning the
run would have read as a clean pass under load. It was not; it was the idle test
again with extra steps.

The single divergence is worth keeping too. At sample 1 entity 10 read
`004A619C:002E6013` on one side and `00498B6E:002D1301` on the other — close but
not equal, which is one instance a few frames ahead of the other on entry, and
it converged immediately. That is why the summary now counts every divergence
and names the "one divergence, at the very first sample" case rather than
reporting a bare "first divergence at sample 1", which reads like a desync and
is not one.

---

## Three players

The audit carried "three players" as **never tried**, and the honest reason was
that the harness could only run two instances. `Play3PlayerMP` is one of the
cartridge's own unlockable-tracking entries (docs/97), which is independent
confirmation that three is the maximum, so it should be reachable.

`lb_mp.py --instances 3` launches a third with `--tap2`/`--env2`. Two changes
were needed beyond the count:

**The sampler compares every instance against the first**, not instance 0
against instance 1. With three players two can agree while the third has
drifted, and a pairwise check would call that in sync — the same shape of hole
as a summary reporting success for samples it never compared.

**One wireless base per instance.** `bases` was a 2-tuple, so the third indexed
past the end and the run died with `IndexError` before launching anything. The
caller never saw it, because the command sent stderr to `/dev/null` and then
grepped a log the crash had prevented from being written: a twenty-five minute
slot, an empty file, no explanation. The launch path now gets a one-minute
check first —

```
$ lb_mp.py smoke3 --instances 3 --route none --linger 1
3 instances serving on (19861, 19862, 19863)
inst0/1/2: [local_mp] configured instance=0/1/2 base_port=28900
```

— which is worth the minute.

### Measured

```
desync: sample 13 in sync across 3, 19 entities
desync: 13 sample(s), 13 compared two live battles, IN SYNC throughout
```

`screenshots/mp-three2/i{0,1,2}-final.png` are three views of one battle: **red,
blue and green**, each instance showing its own base and units, on the same map.
Nineteen entities against the sixteen a two-player match carries, which is the
third player's base and units and is a second reason to believe all three are
really in it.

Both clients join the same host lobby with the same sequence as the two-player
case; the third simply searches a moment longer before tapping.

**Caveat, and the tool says it too:** this run was idle — "the world never
changed between samples" — so it establishes that three instances reach one
match and agree, not that they stay in step under load. Three players plus
`--load` is the next run.

---

## The divergence that was my own sampler

A three-player run under load reported **7 of 18 samples diverged**. That would
have been the first desync this project ever measured. It was not one.

```
desync: DIVERGED at sample 2 (28s in)
desync:   inst0 vs inst1  entity 11  0014B601:0011BE51 vs 0014E427:0011E74F
desync:   inst0 vs inst2  entity 11  0014B601:0011BE51 vs 0014E427:0011E74F
```

Read the two lines together: **inst1 and inst2 agree with each other, and inst0
disagrees with both.** inst0 is the instance read FIRST. The difference is about
three map units on a single entity — one that was walking.

The sampler reads each instance in turn over a debug socket. With a moving world
the instance read first is simply older than the ones read after it, and with
three emulators contending for one machine the spread is wide enough to show.
That is a stale read, not two worlds disagreeing.

**The gate.** After reading all instances the sampler now re-reads the first. If
it changed while the others were being read, the sample straddles a move and
cannot tell skew from divergence, so it is discarded and counted as neither. The
summary reports how many were dropped.

Under load that throws away real samples. It is the correct trade: a test that
reports motion as divergence is worse than one that waits.

**And it does not weaken the two-player results.** Skew manufactures false
DIVERGENCES, never false agreement — so a run that agreed across 26 samples with
the world changing in 13 of them agreed *despite* whatever skew it had. That
result stands, and is if anything conservative.

## Three players under load: not established

Five attempts, one usable run, and that one turned out to be measuring skew.

The failures are not in the game. Every attempt reached the lobby, and one
capture set shows all three sitting on the **minifig select** screen — host
"Waiting...", both clients listed — so discovery, joining and readiness work
with three. What fails is *starting* the match: three emulators on one machine
make the fixed-index tap timing unreliable, and the run either taps a screen
that has not appeared yet or arrives after the moment has passed.

`b:S` (wait until this instance is in a battle) was added for exactly this and
is the right direction, but the flakiness is upstream of it, in the menu walk
itself. Recorded as **not established** rather than passed or failed, because
neither is what the evidence says.
