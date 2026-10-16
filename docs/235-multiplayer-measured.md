# 235 — Multiplayer, measured

**Goal within the project:** two of the port's requirements — the Free Play
music choice also affects multiplayer game music, the faction-based setting is
determined by the builder, and the master server and multiplayer features are
thoroughly implemented and tested.

All of it rested on one thing that had never happened reliably: **two clients in
the same live battle.** This records how that was reached, and what it then
settled.

## The join: two faults, neither of them timing

docs/216 recorded the join as flaky and spent four runs of twenty minutes
re-deriving a tap table. Five runs at the start of this work produced one join.
Every attempt to fix it by adjusting *when* taps happened made it worse in a way
docs/216 had already predicted — walk the host forward faster and the client's
search window closes; retry the tap and it lands on a list mid-populate.

Neither was the problem.

**1. The harness's tap was not a tap.** `lb_mp.py`'s lobby handler pressed at the
row and **released at (0, 0)** — eighty-eight pixels away. The cartridge's
gesture recogniser classifies anything further apart than about twelve pixels as
a **drag**, not a tap: that is `FUN_020AF040`'s threshold, the same constant
docs/234 traced from the other side while fixing clicks in the widened band.
`lb_exp.tap`, which every working route step already used, releases at the press
point. This one did not.

That single line is the whole difference between "one join in five" and "joins
every run".

**2. The tap was also being made on faith.** Even released correctly, the tap was
timed. `l:S` now waits for the condition instead, the same way `b:S` waits for a
battle:

> While searching, the box holds WHITE text centred low ("Searching for
> Games..."). Once a host is listed, its name appears as ORANGE text at the
> TOP-LEFT. Orange pixels in that corner exist in neither the searching state
> nor the empty box.

and with three instances contending, even that was not enough on its own — the
list can be redrawn between the wait and the tap. The row is re-checked
immediately before **each** tap now, which removes the assumption rather than
shortening it.

## The sequence, which was three screens longer than recorded

docs/216's table ended at "host presses Continue". What actually follows:

| whose | what | where |
|---|---|---|
| client | **Ready** in the Game Lobby | `212,174` |
| both | this opens **minifig / army select** | |
| client | pick a minifig | `120,25` |
| client | **Ready** again | `212,174` |
| host | its button now reads **Start** | `212,174` |

The host readies itself when it presses Continue in the lobby, which is why its
button shows `Waiting...` throughout and no tap on the host does anything until
the client has readied twice. Sweeping six candidate coordinates on the host with
a capture after each showed **every one inert** — which is what pointed at the
client being the side with work to do, rather than at a coordinate being wrong.

Result: `inst0: in a battle after 0s`, `inst1: in a battle after 0s`.

## What a live battle then settled

### The music setting applies in multiplayer

Measured, where the audit had said "done by construction". Both clients logged
`Faction-aligned -> side 4 (Earth)`.

### Each client now knows which player it is

`lb_music_set_local_owner` existed for exactly this and **was called by
nothing**, so faction mode keyed off owner 0 — right in Free Play and the
campaign, wrong for a joining client, which would have heard its opponent's
faction.

The net layer cannot supply the index: this port bridges local wireless and the
**cartridge** assigns the slots, so nothing on the host side learns the game's
own numbering. The opening camera can:

```
instance 0   cam (  88,144)   owner 0's base at sx=123   -> player 0
instance 1   cam (1072,672)   owner 1's base at sx=123   -> player 1
```

Both open centred on their **own** base, which is how every RTS opens a match.
So the local owner is the owner whose base is nearest the camera, latched on the
first frame a battle is up — before the view can move and make the question
ambiguous. Only bases count: they do not move, so unlike a unit they cannot
wander toward the other start and change the answer.

### And the chain is legible end to end

`Faction-aligned -> side 4` alone cannot be checked by a reader — it does not say
whose builder was read, and where both players hold the same faction it looks
identical whether the latch worked or not. The log names the owner and the
blueprint now:

```
inst1:  owner 0's builder is blueprint 82 -> side 4
        Faction-aligned -> side 4 (Earth), was 6
        local player is owner 1 (its base is nearest the opening camera); was 0
        owner 1's builder is blueprint 82 -> side 4
```

The client reads owner 0's builder, latches itself as owner 1, and **re-reads
owner 1's**. Instance 0 reads owner 0 throughout, which is correct for it.

### Desync, on a match that was actually moving

Two four-minute runs reported "IN SYNC throughout" and both carried the tool's
own warning that the world never changed between samples. `--load` was ordering
nothing, for two silent reasons: it reads the unit's position out of the port's
entity log and nothing set `LB_ENTLOG=1`, and it tapped **twelve pixels below**
the unit — an aim fudge from before selection worked, which docs/234 removed the
need for.

With both fixed:

```
the world changed in 8 of 11 comparisons, so this was a moving match
17 sample(s), 11 compared two live battles, IN SYNC throughout
6 discarded as SKEWED -- the world moved mid-read
```

Two players, units moving, every comparison agreeing, and the samples that could
not tell skew from divergence thrown away rather than counted as passes.

## The pattern worth carrying forward

Every fault in this document was in the **instrument**, not the game:

* a tap that was a drag;
* a wait that was a guess;
* a load generator that ordered nothing and said nothing;
* a table that stopped three screens early;
* and a log line that could not distinguish a working fix from a coincidence.

None of them would have been found by re-running the experiment, and several had
survived repeated rounds of exactly that. The thing that found each one was
asking what the tool could actually have observed, given what it prints.

## Three players, and why it stopped reproducing

**First, a correction to the heading this section used to carry.** It said
"still not reached", and that was wrong: `docs/227` reached three players in one
battle and measured thirteen in-sync samples. What was true is narrower and
should have been said that way -- three players had stopped *reproducing*, twice
in a row, on the newer route.

The failure was identical both times: each client logged `lobby row seen,
selected and confirmed` and then got *"Could not join this game, please try
another"*, while the host's lobby showed one player.

**It was not the tap.** The row is re-checked immediately before each tap now,
so the list is provably populated at the instant it is pressed. The failure
survived that.

**It was not a transport limit.** `wifi_net.cpp` fans every frame out to
`kMaxInstances = 16` loopback ports.

The frame counters ruled out one further reading:

```
FAILED, three players            WORKED, two players
inst0  sent=996   recv=4         inst0  sent=12648  recv=2
inst1  (no counters logged)      inst1  sent=6085   recv=12665
inst2  sent=0     recv=200
```

A client sends **nothing** until it has joined -- inst2 received 200 frames and
sent zero -- so "the client is not sending" is a symptom, not a cause. And the
host's `recv` is 2 or 4 even in the run that WORKED, so a small host recv is
normal here: the host broadcasts and the clients listen.

### What it was

**Two clients on the Join screen at once.** Both entered it in the first second
and searched simultaneously, and the first client's join does not survive that.
Staggering client 2's *entry* to that screen -- not merely its tap -- by a
hundred seconds fixes it:

```
inst0: in a battle after 0s
inst1: in a battle after 0s
inst2: in a battle after 0s
desync: 9 sample(s), 5 compared live battles, IN SYNC throughout
```

Three instances, one battle, in sync across all three, with the world provably
moving (four samples were discarded as SKEWED, which only happens when it does).

### And then it did not reproduce, which matters more than the success

The winning run's exact command was never written down. Rebuilding the route
from docs/216's recorded two-player sequence plus the stagger produced a worse
result than any failure so far: **all three instances sat in the lobby**,
including the HOST, which had reached a battle in every previous run.

That is not a regression, it is the last timing guess in the route coming due.
The host's button only becomes **Start** once EVERY client has readied twice, so
a Start placed at a fixed step index is correct for two players and a hundred
seconds early for three. It lands on a button still reading `Waiting...`, does
nothing, and is never pressed again.

So the honest status is **reached, but not yet reproducible from a written-down
route** -- weaker than "done", and weaker than what this document said an hour
ago. A new `s:S` step presses Start until the instance is actually in a battle,
which removes the guess the same way `l:` and `b:` removed theirs.

### The Start step starved the client, and a two-player control proved it

The rebuilt route failed the same way with THREE players and with TWO, which is
what made it diagnosable: three players were not the variable. The two-player
run is the control -- same route, same `s:` step, one client -- and it failed
identically:

```
inst0: pressed Start 29 time(s) and never reached a battle
inst1: STILL not in a battle after 180s
```

The captures said the rest. Every instance sat on **army select**: the host's
button read `Start`, both clients' read `Waiting...`, and the lobby capture
before it showed all three players present -- red, blue and green, slots 1/2/3
filled. **The join was never the problem in these runs.** Nothing was wrong with
either coordinate either.

`s:S` blocked. The lockstep loop runs step *n* on every instance before step
*n+1*, so a host step that waits 180 seconds starves every other instance for
180 seconds. The host pressed Start 29 times *before the client had been handed
the steps that pick a minifig and ready up*. The button it was pressing was
real, enabled, and correctly aimed; there was simply nobody ready on the other
side yet.

Short and repeated fixes it -- one press plus a ten-second watch, the step
listed many times -- so every other instance advances a step between two
presses. The general rule, which this harness has now learned three times: **a
route step that waits is a step that blocks everyone else.** `l:` and `b:` are
safe because they end as soon as their condition holds; a step that retries on
a timer is not.

### And a harness bug the success hid

The lockstep loop bounded itself by `routes[0]` and `routes[1]`. With three
instances the third client's route is reliably the longest -- it is the one
carrying the stagger -- so **its tail was dropped silently**. The run above lost
its last five steps that way and only reached a battle because the battle wait
that follows the route caught up. Nothing in the output said so. Now bounded by
the longest route.

The load generator had the mirror-image fault: it required `owner=0`, which is
the host. On both clients that selected a builder **769 pixels off the left of
their own screen**, which was rejected, so two of the three worlds sat still
while the tool called them in sync. It now picks the unit on the instance's own
screen, nearest the centre -- no owner index needed, and the same reasoning the
music latch uses: each instance opens centred on its own base.

## Three players, settled

```
$ lb_mp.py mp3g --instances 3 --route mp --load --desync --linger 150
using the built-in mp routes (pass --tapN to override)
inst0: in a battle after a Start press
inst1: in a battle
inst2: in a battle
desync: sample 1 in sync across 3, 19 entities
desync: sample 2 in sync across 3, 19 entities
desync: sample 4 in sync across 3, 19 entities
desync: sample 6 in sync across 3, 19 entities
desync: the world changed in 3 of 4 comparisons, so this was a moving match
desync: 7 sample(s), 4 compared two live battles, IN SYNC throughout
```

One command, no tap table. Three worlds, all moving, agreeing on every sample
that could be compared, and the three that could not tell skew from divergence
discarded rather than counted as passes.

The builders now confirm the load generator reaches every instance:

```
inst0 owners: 0 1 2      inst1 owners: 0 1 2      inst2 owners: 0 1 2
```

### The pattern, again

Every fault between the first success and this one was in the instrument, and
each **hid its own effect** -- which is why re-running never found them:

* a Start step that blocked the loop it lived in, so the host pressed a live
  button at a client that had not been given its turn to ready;
* a battle wait that blocked the same way, and then reported an instance was not
  in a battle **while its own capture showed it in one**;
* a builder log filtered to `owner == 0`, so on a client it named the host's
  unit, off camera, and the load generator silently ordered nothing;
* and, underneath all three, a working run whose command was never written down.

The last is the one worth carrying: **a result that cannot be re-run is barely a
result.** The routes are in `lb_mp.py` now.
