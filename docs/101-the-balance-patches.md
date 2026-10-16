# The balance patches

**Status: the Balance Patch dropdown exists with the four requested states, the
mods are fully characterised, and applying one is not implemented. The design
for applying it is settled and the reason it is not shipped is deliberate.**

## Three romhacks, all pure data

The local mod ROM folder (`<your mods folder>`) holds four ROMs in two variants
each — `MapBase` and `MapDump` — for VANILLA, REbalance, FactionWars and
FUNLite. `tools/lb_moddiff.py` established the essential fact:

> **ARM9, ARM7 and all 91 overlays are byte-identical to vanilla in every mod.**

They change `BP/Entities.ebp` and nothing else. That is why a Balance Patch
selector is possible at all: no code differs, so nothing has to be re-lifted and
no override is involved. It is the same kind of change as *Make All Builders The
Same*, only larger.

Recorded diffs (`docs/data/mods/`):

| mod | field changes |
| --- | ---: |
| FUNLite | 679 |
| REbalance | 637 |
| FactionWars | 574 |

The most-changed fields are `health` (226), `cost` (225), `build_time` (222),
`attack_interval` (184) and `attack_range` (164) — a wholesale stat rebalance,
exactly as Ryncat's README describes: *"This patch modifies every single
statistic that a unit possesses, not only its cost, speed, health points, and
damage as depicted in the in-game overview screen."*

## Correction: the on-disk magic is `PMOC`, not `BPNZ`

`docs/95`'s builder patch searches main RAM for `BPNZ`, and that is right — but
**the file on disk does not start with `BPNZ`**. Both the shipped
`BP/Entities.ebp` (17,760 bytes) and the rebalance patch's `1432.ebp` (18,148
bytes) begin `PMOC`, which is `COMP` little-endian.

So `Entities.ebp` is a **COMP/LZ11 container** (`tools/lb_comp.py`), and `BPNZ`
is what it decompresses to in RAM. Anyone looking for the blueprint magic in a
file rather than in memory will not find it, and that is worth stating plainly
because this project has already spent time on exactly that class of confusion.

The size difference is also the first hint that a patched blueprint is not a
drop-in of equal length: 18,148 against 17,760.

## Why applying it is not shipped yet

The three mods are **other people's work** — Ryncat's Rebalance and the
FactionWars and FUNLite hacks. Reproducing their balance inside this port would
redistribute their design, and the recorded diffs in `docs/data/mods/` are
analysis rather than a licence to reimplement.

**The port should read the blueprint from the player's own copy**, exactly as it
already treats BIOS dumps and the cartridge: the user supplies the content, the
port supplies the machinery. That means, at selection time:

1. open the mod ROM the user points at;
2. find `BP/Entities.ebp` in its NitroFS;
3. decompress the COMP container;
4. overwrite the in-RAM `BPNZ` block, which `docs/95` already locates and
   fingerprints at `0x0226A130`.

Steps 3 and 4 are done work — `lb_comp.py` decodes the container and the builder
patch already writes into that block. Step 2 is the missing piece: a NitroFS
lookup inside the runner.

Until then the dropdown stores the choice, is badged `[read, not wired]`, and
says in the panel that it will read the player's own romhack.

## Multiplayer consequence

Ryncat's README is explicit: *"all players in multiplayer must be using this
patch in order for the game to function properly."* That makes the balance patch
**lobby-carried**, like every other simulation-affecting setting in `docs/86` —
a host on Rebalance and a client on vanilla would desync on the first differing
unit stat. Nothing enforces that yet because there is no join path.

---

# CORRECTION: they are not "BP and nothing else"

**This document asserted, repeatedly, that the three mods "change
`BP/Entities.ebp` and nothing else". That is false and is withdrawn.**

The claim came from `lb_moddiff`'s real finding — ARM9, ARM7 and all 91
overlays are byte-identical in every mod — which establishes that **no code
differs**. Generalising that to "no other *file* differs" was an unforced error;
nobody had counted the data files.

Counted now, against `VANILLA_MapBase.nds`:

| mod | files changed | what changed |
| --- | ---: | --- |
| **REbalance** | **32** | **14 `Maps/*.map`**, 15 tileset graphics (`NCGR`/`NCBR`/`NCLR`), `LOC`, `BP/Entities.ebp`, `Sound` |
| FactionWars | 6 | 3 tileset palettes, `LOC`, `BP/Entities.ebp`, `Sound` |
| FUNLite | 3 | `LOC`, `BP/Entities.ebp`, `Sound` |

No mod adds or removes a file; all three only replace existing ones.

## What this changes, and what it does not

**It does not invalidate the bundled balance tables.** Those are generated from
`BP/Entities.ebp` alone and reproduce the *stat* changes, which is what a
"Balance Patch" selector should mean. Rebalance is 835 bytes of stat deltas,
FactionWars 827, FUNLite 893.

**It does mean the selector is not the whole mod.** Selecting Rebalance in the
port gives its balance, not its maps, tilesets, strings or audio. The panel says
so rather than implying a complete conversion.

## The custom maps are a separate feature

REbalance replaces **14 existing map files**:

```
Maps/ck1_1.map  ck1_3  ck1_4  ck1_6  ck2_1  ck2_2  ... (14 total)
```

plus the `ck3_*mini` minimap graphics and three faction tilesets that go with
them. These are **map design, not numeric tweaks**, and they are deliberately
not bundled.

They are also the concrete case for the requested mods folder: a port that can
load `.map` files from a directory and append them to the map list would let a
player supply these from their own copy, alongside any other community maps.
That feature needs:

* the `.map` format read well enough to validate a file before offering it —
  `tools/lb_map.py` and `docs/data/maps.tsv` already cover the header, tileset
  id and the `EVNT`/`TRIG`/`MARK`/`MINE` sections;
* the tilesets a custom map depends on, which REbalance shows can differ;
* an agreed ordering, since every connected player must have
  the same mods in the same order for multiplayer to stay in lockstep.

None of that is implemented. What is now known is exactly which files a mod
touches and why the maps cannot be folded into the balance tables.

## Lesson

`lb_moddiff` answered "does any *code* differ" and answered it well. This
document then used that answer for a different question — "does anything else
differ" — without re-checking. The two questions have different answers, and the
second one had never been asked.
