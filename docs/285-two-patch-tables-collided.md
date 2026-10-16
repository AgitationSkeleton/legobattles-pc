# 285 — Two patch tables shared a word, and the all-or-none one lost

The third of three symptoms from one build: clicking outside the region of
original aspect just moved to the edge of the original aspect.

This was **a regression, and it was mine**. Nothing was wrong with the touch
widening. It had been switched off — completely, all thirty sites — by an
unrelated patch table, and had been off since `kOrderFix` group 128 went to
default-on the change before.

## The collision

`kTouchPatches` is written as 32-bit words, because ARM instructions are. But
several of its entries sit on THUMB code and change only ONE of the two
halfwords in the word they name. `kOrderFix` is written as halfwords, and owns
the other half of two of them:

| address | table | change |
|---|---|---|
| `020AF29C` | kTouchPatches | `7A29` → `89E9` (pick reads x) |
| `020AF29E` | kOrderFix g128 | `7A6A` → `8A6A` (pick reads y) |
| `020AF220` | kTouchPatches | `79A9` → … (press x) |
| `020AF222` | kOrderFix g128 | `79EA` → `8AAA` (press y) |

`kOrderFix` applies first. `apply_touch_extension`'s readiness test then compared
the **full word** against a stock value that no longer described it, so `ok`
could never reach `total` — and because that table is deliberately all-or-none,
every site was refused, forever:

```
[lb_touch] STILL WAITING: 28 of 30 sites match the stock encoding after 10s
[lb_touch]   020AF29C reads 8A6A7A29, expected 7A6A7A29
[lb_touch]   020AF220 reads 8AAA79A9, expected 79EA79A9
```

## Why the blast radius was so much larger than two sites

`0200A3C4` is in that table. It is the `movhi r5,#0xFB` in `FUN_0200A350` that
pins every out-of-range click to column **251** — the patch that removes it is
what lets a click outside the native panel exist at all. With the set refused,
every such click landed at the native edge.

That is the reported symptom exactly. The two sites that actually
collided were in the *pick*; the symptom came from a third site that had nothing
to do with either and was only refused because it shared a table with them.

**An all-or-none table converts any single mismatch into a total outage.** That
is the right default for "is this the build I was compiled against" and the
wrong default for "did another patch touch a neighbouring halfword".

## Why it was not caught

The verification run for group 128 checked `[lb_orderfix]` and reported
`groups=255, 37 site(s) moved off the byte path`. That line was true. Nothing in
that run read `[lb_touch]`, and the two tables were treated as independent
because they had been written independently.

The general rule: **when a change adds sites to one patch table, the evidence
has to include every other table that writes the same region**, not just the one
that changed. Neither table's own log can report a collision, because each one
sees only its own addresses.

## The fix, and why it is order-independent

Each entry now owns exactly the halfwords it modifies. `stock ^ want` names the
bits that differ, widened to halfword granularity because that is the unit the
other table works in. The readiness test checks only those bits; the write is
read-modify-write so the halfword the entry does not own survives.

Applying `kTouchPatches` before `kOrderFix` would also have worked **today**,
and would break the next time a table is added or an apply order changes. The
masked form cannot collide with anything, in either direction.

**What this gives up, stated plainly:** the readiness test no longer proves the
untouched halfword belongs to the expected build. It still proves every bit the
entry changes is exactly the stock encoding, which is the part that makes the
patch safe to apply — and it is precisely what `kOrderFix` has always checked
for its own sites.

## Verified

All thirty sites apply, including the new Y writer repoint:

```
[lb_touch] 0203DD14 writer sources X from the port
[lb_touch] 0203DD20 writer sources Y from the port
... 28 more ...
[lb_touch] clicks past the native 256 columns now reach the game, through 02153778
```

And the behaviour that depends on them is back. Band selection, on a 786-wide
surface with 261 px bands:

```
bandselect: left band  target at x -245..-16 -> SELECTED 4 of 4 clean attempts
      selected handles: 000D x4
```

A selection at x = −245 requires the extended negative x to reach the pick,
which is what the refused `020AF29C` redirect provides. The in-band and
right-band rows of that run print `COULD NOT PARK … not counted` — the probe
could not move the target to those columns and declines to score them, which is
the instrument behaving correctly rather than a second failure.

## Not established

That `+0x18` and `+0x1A`, the two port-owned touch slots, are unwritten on
*every* route. docs/284's measurement is one battle and one input pattern; a
field written only on a path that route never takes would read as still. The
same caveat has applied to `+0x18` for a long time without trouble, which is
evidence rather than proof.
