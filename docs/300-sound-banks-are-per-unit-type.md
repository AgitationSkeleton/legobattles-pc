# 300 — Sound banks are per unit type, and the silent units are a different family

The defect: unit sounds do not play, except for a couple of buildings or units;
e.g. the astro builder plays but the space criminal leader and the islander do
not.

docs/287 cleared the arena and docs/299 cleared allocation failure. This finds
the structure, and the split in the defect's own description turns out to name it.

## The data

`Sound/sound_data.sdat` is **62,902,976 bytes** — 62.8 MB of it wave data — at
ROM offset 38,691,840, so it ends at 101,594,816. The cartridge is
**134,217,728 bytes**, so the archive is entirely present and nothing is
truncated. At 63 MB against a 686 KB arena it is plainly **streamed**, not
resident.

`Sound/sound_data.sadl` beside it is a **C header**, 1311 `#define`s of name to
id, and it lays the id space out exactly:

```
BANK_INTERFACE              0
BANK_COMMON                 1
BANK_UNITS                  2
BANK_UNIT__<FACTION>_<ROLE>   3 .. 62     60 entries
BANK_STRUCT…                 63 .. 123    61 entries
BANK_COLOURFACTION__<NAME>  125 .. 158    34 entries
```

`BANK_UNIT__` covers the six main factions crossed with roles — HEROM, HEROF,
BUILDER, MELEE, RANGED, MOUNTED, SIEGEONE/TWO/THREE, TRANSPORT.
`BANK_COLOURFACTION__` is a **separate family**: SPACEPOLICE, SPACECRIMINAL,
ISLANDER, AGENTCHASE, CLASSICSPACE, SANTA and the rest — the custom-army
factions, each with HERO, plain, SIEGE and BASE variants.

## The split in the defect is the split between the two families

* **the builder plays** — a builder is `BANK_UNIT__<faction>_BUILDER`, ids 3..62
* **space criminal leader and islander are silent** — both are
  `BANK_COLOURFACTION__…`, ids 125..158

That is not a coincidence of two examples; it is the whole boundary.

## How banks get loaded, and where one can be skipped

`func_02088590` loads `BANK_INTERFACE` and `BANK_COMMON` explicitly by id, each
checked (docs/299). It then runs a **loop over a collection of entities**:

```asm
blx  func_020023B0        ; count
...
blx  func_020023E8        ; element[i]
bl   func_02088A18        ; element -> bank id
ldr  r1, [pc, #0x1dc]     ; = 0x0000FFFF
cmp  r0, r1
beq  <skip>               ; 0xFFFF means "no bank", nothing is loaded
bl   func_02089B0C        ; otherwise load it
```

**`0xFFFF` is the sentinel**, and a unit whose mapping returns it is silently
given no bank at all. Not a failed load — docs/299 measured that failure handler
never firing — but a load never attempted.

`func_02088A18` is the mapping. It reads a **category byte at `+0x5C`**,
switches on it through a jump table for values 0..0x13 with everything above 19
falling to a default arm, sets a structure/unit flag, and then selects a bank id
by comparing an index against thresholds (0x14, 0x28, …), producing values like
2 (`BANK_UNITS`) and 62.

## Status and the next question

**The defect is open, and the search has a target.** The question is now narrow:
**does `func_02088A18` ever produce an id in 125..158?** If it cannot, the
colourfaction banks are never requested, which matches every observation —
allocation never fails because no allocation is attempted, and the units that
work are exactly those in the other family.

**NOT ESTABLISHED:** that it cannot. `func_02088A18` runs to `0x02088C4E`, a
long chain of range comparisons, and only the first few were read. The
alternative is that colourfaction banks are loaded elsewhere entirely — by
whatever sets up a custom army — and that path is the broken one.

**Worth noting for whoever continues:** the `.sadl` is a generated header, so
every bank name and id in this game is already known by name. That is an unusual
luxury in this project and it should be used — a port-side log of "bank id N
requested" can print the actual symbol rather than a number.

## CORRECTION — the colourfaction banks ARE requested

The question this document left open was "does `func_02088A18` ever produce an
id in 125..158?", with the suggestion that it might not. **It does — all of
them.**

Scanning every immediate the function moves into its **return register**:

```
r0 immediates: 122..160, 162..177
in the colourfaction range 125..158: all 34
```

So every colourfaction bank has an explicit return arm. They are mapped, the
sentinel is not returned for them, and the loop therefore calls
`func_02089B0C` on each.

### The near-miss worth recording

The first scan was of **r4**, not r0, and returned `12, 22, 32, 42, 52, 62, 72,
82, 92, 102, 112` — a clean arithmetic sequence of step 10 that matches the id
layout exactly (6 factions x 10 roles), topping out at 112 and **never reaching
125**. That was an elegant, wrong answer, and it was one commit away from being
published as "colourfaction banks are never requested".

`r4` is an intermediate. The function returns in `r0`, which the tail makes
obvious — `movs r0, #0xAF` then `pop {r4, pc}`. Checking what the function
actually *returns* before trusting a scan of what it *computes* is the whole
lesson, and it is the same shape as the project's other instrument
failures.

## Where the defect stands now

Three mechanisms eliminated, each by measurement:

1. **The arena is not exhausted** — about three-quarters used, 150-200 KB free.
2. **No allocation fails** — the failure handler's signature at `0x02143E38`
   is untouched across two full matches on two tilesets.
3. **The banks are not un-mapped** — every colourfaction id has a return arm.

So colourfaction sounds are requested, and requested successfully. **The silence
is downstream of loading**, in what triggers a sound for a unit or what mixes
it, and that is where the next search belongs.

The `.sadl` remains the lever: every bank and wave in this game is named, so an
instrument can report `BANK_COLOURFACTION__ISLANDER` rather than `147`.
