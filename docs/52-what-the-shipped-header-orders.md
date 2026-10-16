# What the shipped header orders

**Status: `Sound/sound_data.sadl` is fully read — all 1,311 defines, and the
`SEQARC`/`SE` pairing the file never states.** Eight new verification checks;
50 of 50 pass.

`docs/16` found this file and named it correctly: it is not audio, it is the
developers' own C header, 1,311 `#define` lines, shipped in the NitroFS beside
the 60 MB `sound_data.sdat`. What `docs/16` did not do was read the *structure*
of the numbering, and `lb_progress` went on filing it as **partial — "symbol
header only, audio body not decoded"**.

**There is no audio body.** The file is 1,311 lines and every one of them is a
`#define`. The partial tier was wrong about this file for eight passes; it is
corrected here rather than quietly dropped.

## The seven namespaces

```
namespace    count  range      contiguous  unique
BANK           200  0..199     yes         yes
PLAYER           2  0..1       yes         yes
SE             784  0..23      NO          NO
SEQARC         217  0..216     yes         yes
STRM            98  0..97      yes         yes
STRMPLAYER       2  0..1       yes         yes
WAVE             8  0..7       yes         yes
```

Six of the seven are a contiguous bijection onto `0..N-1`. `SE` is neither, and
that is the whole finding: **`SE` numbering restarts at zero for each sequence
archive**, so a sound's address is the pair `(SEQARC id, SE index)` and not an
`SE` value alone. Anything that treats `SE_*` as a flat 784-entry enum will
collide 567 times.

## The pairing the file does not state

The header never says which `SE` belongs to which `SEQARC`. It does not have to.
Cut the 784 `SE` lines wherever the value returns to 0:

```
217 runs  --  and there are exactly 217 SEQARC entries
```

and run *k* carries the name of `SEQARC` *k*. Neither fact was put there on
purpose. They hold only if the header really is emitted archive by archive, in
`SEQARC` order, which is what makes this a reading rather than a guess.

Raw name agreement is 103 of 217, because the `SE` side uses its own shorthand.
Six substitutions, each read off the pairing rather than assumed:

| `SE` writes | `SEQARC` writes |
| --- | --- |
| `SPELL_X` | `X` |
| `COLOURFACTION_X` | `X` |
| `PROJECTILE_X` | `X` |
| `_VC_` | `_VEHICLECENTER_` |
| `_BUILDER_` | `_ENGINEER_` (Earth and Alien) |
| `_TRANSPORT_` | `_TRANSPORTSHIP_` (Imperial, Earth, Alien) |

That takes agreement to **205 of 217**. The twelve left over are named below,
and a second, independent measurement finds the same twelve.

## The tower tiers share everything

> **Refined by `docs/54`.** This section originally called the duplicate
> `#define`s "a bug in the shipped source". They are not a slip. `docs/54` read
> the `.sdat` and found that each faction's `TOWER`, `TOWER2` and `TOWER3`
> **point at the same sequence-archive file and the same bank file** — 18 names
> over 6 files, on both sides. The names were never renamed because the *data*
> is deliberately shared. The original wording is corrected here rather than
> elsewhere.

**36 `SE` names are `#define`d more than once**, 18 names at three definitions
each. They are exactly the tower tiers: six factions x `ONSELECT`, `ONDEATH`,
`ONHIT` x the two extra tiers.

```
SE_KING_TOWER_ONSELECT   0      belongs to SEQARC__KING_TOWER
SE_KING_TOWER_ONSELECT   0      belongs to SEQARC__KING_TOWER2
SE_KING_TOWER_ONSELECT   0      belongs to SEQARC__KING_TOWER3
```

`TOWER2` and `TOWER3` were copy-pasted from `TOWER` and never renamed. The
values are identical so a C compiler emits a warning rather than an error, which
is presumably why it shipped. 6 x 3 x 2 = 36, exactly the redefine count.

The two measurements agree: the twelve archives whose names disagree after
shorthand *are* the twelve archives holding the 36 redefines. Neither
calculation knows about the other. **A third measurement, from the `.sdat`,
agrees with both** — see `docs/54`. The three tower tiers of a faction are one
sound, one bank and one archive, addressed by three names.

## The bank space is a partition

The 200 banks are not a flat list. Eight of them are **group banks** with no
`__` in the name, and each is followed immediately by the banks it groups:

```
BANK_INTERFACE        0
BANK_COMMON           1
BANK_UNITS            2  +  60 units          -> 63
BANK_STRUCTURES      63  +  60 structures     -> 124
BANK_COLOURFACTIONS 124  +  34 colourfactions -> 159
BANK_PROJECTILES    159  +   4 projectiles    -> 164
BANK_SPELLS         164  +  34 spells         -> 199
BANK_COLLECTIBLES   199                       -> 200
```

`group index + 1 + member count == the next group index`, five times, closing
exactly on 200. Nothing declares the member counts — they are counted from the
names — so the chain closing is a check on the whole layout at once.

The eight group banks carry **the same eight names as the eight `WAVE`
archives**: `INTERFACE`, `COMMON`, `UNITS`, `STRUCTURES`, `COLOURFACTIONS`,
`PROJECTILES`, `SPELLS`, `COLLECTIBLES`. The indices differ (bank indices are
positional), but the eight-way split of the game's audio is the same on both
sides.

## The event vocabulary, and what it says about behaviour

Every archive's `SE` list is a set of events. Sorted by that set:

| archives | events raised | what they are |
| ---: | --- | --- |
| 60 | `ONSELECT` `ONDEATH` `ONSTRIKE` `ONHIT` `ONSPECIAL` | the six factions' units |
| 63 | `ONSELECT` `ONDEATH` `ONHIT` | the 60 structures, plus `WALL`, `GATE`, `BRIDGE` |
| 53 | `ONSELECT` `ONDEATH` `ONSTRIKE` `ONHIT` | colour-faction units and neutral structures |
| 38 | `ONSELECT` | 33 spells, 4 projectiles, `COLLECTIBLES` |
| 1 | `ONSELECT` `ONSTRIKE` | `CRABSWARM` |
| 1 | 17 front-end sounds | `FE` |
| 1 | 24 in-game UI sounds | `UI` |

`60 + 63 + 53 + 38 + 1 + 1 + 1 = 217`, and the effects sum to 784.

Three things fall out of that table, and all three are statements about
**behaviour**, not audio:

1. **`ONSPECIAL` occurs 60 times and only on the six playable factions' units.**
   No structure has one, and no colour-faction unit has one. The special
   ability is a property of playable units alone.
2. **No structure raises `ONSTRIKE`** — not even `TOWER`, `TOWER2` or `TOWER3`,
   which certainly do attack. Their attack sound is not theirs; it belongs to
   the projectile. The four projectile archives (`ARROWSTRIKE`,
   `BOULDERSTRIKE`, `CANNONSTRIKE`, `FIREBALLSTRIKE`) are where it lives, which
   is consistent with `docs/20` finding the damage event on the projectile.
3. **`WALL`, `GATE` and `BRIDGE` carry the structure event set** while sitting
   outside the `BANK_STRUCTURE` namespace. They are selectable and destructible
   but are not a faction's build slot — matching `docs/12`, which found them
   among the 62 neutral `UnitBlueprint` records rather than the 120 faction ones.

`ONSTRIKE` is *dealing* a blow and `ONHIT` is *taking* one, which is the only
reading consistent with 114 strikes against 176 hits: everything that can be
damaged has `ONHIT`, and only things that attack have `ONSTRIKE`.

## Typos, and why they are evidence

`MARSFALVOUR` (for *flavour*), `ABANDONNEDIMPERIALOUTPOST` (double N),
`WHIPWRECKBEACH` (for *shipwreck*, and the file has `SHIPWRECKWATER` spelled
correctly two lines away). These are not corruption — they round-trip byte for
byte. They are hand-typed names, which is corroboration that this file is source
rather than a generated artefact, and a caution: **the header's spelling is
authoritative for the symbol and not for the entity.**

## Tips

* Do not build an enum from `SE_*`. Build `(archive, index)` pairs.
* `lb_sound_symbols.py` now emits both tables:
  `docs/data/sound_symbols.tsv` (1,311 symbols) and
  `docs/data/sound_events.tsv` (784 effects, each with its archive).
* A "documented already" tier entry is not the same as a *read* one. This file
  sat at partial for eight passes because `docs/16` had described it and the
  tier said "body not decoded" — about a file with no body. Re-read tier
  assignments, not just the tier totals.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| ARM9 annotated | 42.6% | 2,501 listed + 1,465 derived |
| data decoded | **99.1%** | 3,878 of 3,915 files |
| decompilation | 0.157% | 377 functions byte-verified |
