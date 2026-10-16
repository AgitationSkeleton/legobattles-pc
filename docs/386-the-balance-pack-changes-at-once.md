# 386 — The balance pack changes at once, and reaches every unit

Status: **implemented and verified live** (lb_balance.cpp, lb_patches.cpp,
lb_overlay.cpp). The requirement (2026-09-27): a change to the rebalance
setting takes effect immediately when the player is not in gameplay or a
lobby.

## What it did before

The Enhancements tab's Balance Patch (Rebalance, FactionWars, Funlite) was
applied once, the frame lb_patches.cpp found the blueprint block
(BP/Entities.ebp in RAM). The packs are byte deltas (docs/101) with no
vanilla copy, so nothing could be taken out: "cannot be undone in memory --
changing it back needs a restart". Make All Builders The Same had the same
limit ("Off does not undo it").

## What it does now

* **A snapshot of the loaded bytes.** Every byte any pack touches, and every
  byte of the 14 fields same-builders copies into the five other builders, is
  kept as the game loaded it, before either edit touches it
  (`lb_balance_snapshot`, `snapshot_same_builders`).
* **Any change is applied in the frame after it is made**, outside a battle
  and a lobby: same-builders' fields go back, the old pack's bytes go back
  (`lb_balance_revert`), then the new pack is applied and same-builders
  re-copied from the (patched) Imperial builder -- the order the
  combination needs.
* **"Not in gameplay or a lobby"** (`lb_blueprint_edit_blocked`) is Set
  Team's test (docs/384) plus one: the network idle (not hosting, in a lobby,
  joining or browsing), no battle view (registry slot 0x5C, which stays under
  the pause menu), and no unit on the field (a battle loading or ending).
  Inside one, the change waits, and the panel says why ("in the game:
  Rebalance; the change waits: not during a battle (the pause menu counts)").
  A unit reads its blueprint as it is made and as it acts, so a change
  mid-battle would give one match two rule sets; and nothing carries the pack
  to other machines, so a lobby must not change under its players.

## Found on the way: the pack never reached the late units

The first snapshot, taken the frame the block was found, was not the loaded
block. Putting it back left 319 bytes different from the finished block, all
in records 145..199 (run t386a). The fingerprint (the Imperial builder,
record 62) matches while the loader is still writing later records.

The same race hit the shipped behaviour. A pack applied at discovery wrote
those records before the loader did, and the loader overwrote them. The log's
"830 of 835 bytes changed (5 already matched vanilla)", explained away in
lb_overlay.cpp as bytes the pack and vanilla agree on, was measured on that
unfinished block. That note is retracted in place.

Now nothing is snapshot or applied until the span the edits reach has held
still: an FNV-1a hash of it every 20 frames, three times equal. It settles 80
frames after discovery. A pack chosen before launch then changes all 835
bytes (run t386c). Settings with no pack chosen were never affected by this.

## Verified (runs t386b, t386c; block diffs against the settled vanilla block)

| action (main menu unless said) | bytes differing from vanilla |
|---|---|
| same-builders on / off | 16 / 0 |
| Rebalance | 835 (835 of 835 changed) |
| FactionWars (Rebalance taken out: 835 back) | 827 |
| Off (FactionWars taken out: 827 back) | 0 |
| same-builders on, then Rebalance | 841 |
| then Off, same-builders still on | 16 |
| Rebalance chosen before launch, after settling | all 835 applied |
| in a battle, Off chosen | waits; the log says why |
| that battle ends (Victory) | Rebalance taken out, 0 of its 835 bytes left different |

In a battle the block differs from vanilla in 539 more bytes than the pack's,
all at unit-record offsets +0x2E..+0x59 -- per-battle fixups, likely handles
-- and none of them is a byte any pack or same-builders touches. So taking a
pack out never writes over a byte the game changes.

## Gate

With no pack selected (the harness's settings) nothing is applied, as before.
The results match the reference runs:

* the exactness gate: counts identical to gate370b, and 8 of 8 layer files
  identical to gate381;
* the t384f replays: identical to each other and to rep384c;
* Vanilla: 240 of 240 identical to van-hudmask;
* 59.79 fps.
