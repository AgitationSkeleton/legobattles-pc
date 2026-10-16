# Status and roadmap

Last updated 2026-08-28, session 001 (second pass).

## Where the project is

| track | state | evidence |
| --- | --- | --- |
| ROM extraction | **done**, both regions | `docs/04`, `tools/nds_extract.py` |
| `COMP` container | **solved** | `docs/05` — 2,178/2,178 decode |
| asset census | **done** | `tools/asset_census.py` |
| `LANG` string tables | **solved** | `tools/lb_lang.py` — 3,064 ids × 8 languages |
| C++ RTTI | **recovered** | `docs/07` — 512 classes, 2,454 virtual functions |
| campaign table | **complete**, 88/90 corroborated | `docs/08`, `docs/data/campaign_missions.tsv` |
| function corpus | **9,312 with modes**, cross-checked | `docs/09` |
| Ghidra headless | **confirmed working** | `docs/09` |
| `BP/Entities.ebp` | **solved** | `docs/11` — 551 records, stat fields named |
| units and buildings | **analysed** | `docs/12`, `docs/data/entities.tsv` |
| `BP/Factions.fbp` | **solved** | `docs/13` — 130 rosters, 26-slot template |
| code reading tools | **built** | `tools/lb_code.py` — xref index in 3.5 s |
| `.map` container + terrain | **solved**, 122/122 | `docs/15`, `docs/data/maps.tsv` |
| `.map` record chunks | **solved**, 122/122 | `docs/15` |
| `BP/*.tbp` tile blueprints | **solved**, 136/136 | `docs/21` |
| `BP/Animations.abp` | not referenced by name in the ARM9 | `docs/20` |
| sound symbol header | **read** | `docs/16` - 1,311 names |
| ndsrecomp bring-up | not started | |
| matching decompilation | third-party config adopted | `docs/02` |
| ARM7 sections | **found** — 3 programs, 99.7% relocated | `docs/14` |
| ARM7 function corpus | **334 functions, 69.5% coverage** | `docs/17`, `docs/data/arm7_corpus.tsv` |
| ARM9 SDK/game split | **measured** — 1,354 functions are library | `docs/19` |
| combat model | **solved** — ranges, 30 Hz tick, DPS identity | `docs/20` |
| collectables and the unlock space | **solved** | `docs/22` |
| save bit-stream layout | **recovered**, loader-validated | `docs/23` |
| spell and buff architecture | **mapped** (parameters still unknown) | `docs/24` |
| the `Sim::` runtime and unit behaviour | **mapped**; `+0x60` named | `docs/25` |

## The blueprint files — superseded

This section previously read *"This is not enough to write a parser, and no
parser will be guessed. The right source is the ARM9 loader, which the RTTI has
already named. That is the first thing to do once Ghidra analysis is running."*

Both halves turned out to be right and one was too pessimistic. The loader was
the right source — and reading it needed no Ghidra analysis at all, only a
cross-reference index and a disassembler (`tools/lb_code.py`). `Entities.ebp` is
solved in `docs/11`, its roster analysed in `docs/12`, and `Factions.fbp` in
`docs/13`.

The observations that section recorded all held up: the string pool at `0x0FA08`
with 1,102 entries, the variable-length records, and the rebalance-patch diff
localising the stat fields to clusters ~124 bytes apart — 124 being the
`UnitBlueprint` stride, as the loader later confirmed.

## Order of work

### 1. Ghidra, properly

The gate for nearly everything else.

1. Import all programs at their runtime addresses: ARM9 static `0x02000000`,
   ITCM `0x01FF8000`, DTCM `0x027E0000`, ARM7 `0x02380000`, and the 91 overlays
   at `0x0216A2E0`.
2. Seed from `out/eur/corpus.tsv` — 9,312 functions with modes. Set the `TMode`
   context register from the `mode` column; do not let analysis guess it.
3. Apply the RTTI names: typeinfo and vtable symbols, and `Class::virtual_NN`
   for every slot.
4. **Export the function/mode list and commit it before any bulk mutation.** On
   LSW2, running 2,027 coverage addresses through `CreateEntryPoints -Analyze`
   destroyed 275 existing functions and 8% of instruction coverage while the
   function *count* rose 31%. The committed export was the only undo.
5. Export data ranges; run the void-return retyping pass.

### 2. ~~The ARM7, before profiling anything~~ — done

It does relocate, and by more than LSW2's did: **488 of 159,528 bytes stay at
the header address**, 62,640 go to `0x037F8000` and 96,376 to `0x027E0000`.
There is no `MODULE_PARAMS` magic, so a structural scan finds the table
instead. `docs/14`.

What remains is that **the ARM7 has no function corpus** — 0 of its ~62 KB of
code is catalogued, and the third-party config covers only the ARM9. That is
now the largest single gap, and it is where the sound system lives.

### 3. ~~The blueprint loader~~ — done, and it did not need Ghidra

`Entities.ebp` and `Factions.fbp` are solved (`docs/11`, `docs/13`), read
straight out of the ARM9 with `tools/lb_code.py`. `Animations.abp` and the 136
`.tbp` tile blueprints remain, and the same route should reach them.

The next data question is **what builds what** — the producer→product mapping,
which neither file carries. `Battles::ProduceEntityCommand` and
`Battles::CreateUnitFactionCommand` are named in the RTTI and are where to look.

### 4. ~~The map format~~ — container and terrain done

`docs/15`. What remains is the record chunks: `func_020A4B90` is the event
factory, and `Battles::EventBase` has 22 subclasses in the RTTI. Decoding those
is decoding the mission-scripting system, and it is the same job as the 91
overlays.

### 5. ndsrecomp bring-up

Program configs from the corpus, then a first boot, then runtime coverage fed
back to Ghidra as *seeds* — never as declared entry points, because a coverage
address is where interpretation started and is often mid-function.

Prerequisites not yet present: a 32-bit `i686-w64-mingw32` toolchain, and DS
BIOS/firmware dumps for the faithful boot path (there is a FreeBIOS fallback).

### 6. Freeze a closure

Write down the set of functions the project needs and report against it. Until
that exists, "progress" is a feeling.

## Open questions, carried forward

| question | where |
| --- | --- |
| `cw2_1` / `cw2_2` — which class is which. Still open, but there is now a route: each overlay's `OpenMissionDialog` ids land in one contiguous `LOC` block (`ov078` → 1648–1654 under title 1644 "Convenient Alliance", `ov080` → 1661+ under 1657 "Strength in Numbers"), and the blocks run in presentation order | `docs/08`, `docs/data/overlay_survey.md` |
| `pi3_4.map` exists and no mission claims it | `docs/08` |
| the Wizard campaign has 16 mission text blocks for 15 maps | `docs/08` |
| ~~what `ov000` is~~ — resolved: it is `Battles::CampaignBase`'s own overlay, the 91st claimant of the slot-11 `GetOverlayId()`; no `.map`, no mission dialog, and 11 of the 13 objective types constructed exactly once | `docs/data/overlay_survey.md` |
| ~~naming the 13 scripting-object factories~~ — resolved: they are the 13 concrete `ObjectiveBase` subclasses, named from their vtable literals | `docs/32` |
| the ~40 mission-API entry points still unnamed | `docs/31` |
| `LANG` header field `+0x04` = `0x3B20`, constant across all 8 languages | `tools/lb_lang.py` |
| `COMP` `scratch_hint` is 4 low in exactly two files | `docs/05` |
| what builds what — the producer→product mapping. Three candidates now eliminated: `UnitBlueprint+0x20` (faction-invariant), `BuildTab::virtual_08` (button dispatch), and the `User::BuildCommandItem`/`UnitsRequired` list builders (hardcoded UI items, not data-driven) | `docs/13`, `docs/26` |
| what the ARM7's ten IPC channels mean — they are SDK `PXI` channels, so the names are a property of the SDK | `docs/18`, `docs/19` |
| ~~what the ARM7's remaining ~30% is~~ — labelled: 43% of it names its own hardware, dominated by the Wi-Fi stack; the rest loads no registers and is plausibly its crypto | `docs/38` |
| what actually completes an objective — not the vtable; the `EVNT`/`TRIG` event machinery is the untested candidate | `docs/33` |
| what the `EVNT`/`TRIG` id enums mean | `docs/15` |
| the field layout inside the event sub-records | `docs/15` |
| what terrain planes A and C mean (E is solved) | `docs/15`, `docs/21` |
| what indexes `DetailTiles_<map>.tbp` — not plane B | `docs/21` |
| the category enum → `LOC` string permutation table | `docs/12` |
| spell parameters — cost, cooldown, radius; likely compiled in, not a file | `docs/24` |
| armour and population | `docs/12` |
| what the seven scalar save fields mean (widths known, meanings not) | `docs/23` |
| `ProjectileBlueprint` flags at +0x5E..+0x6B and its second damage pair | `docs/20` |
| `Animations.abp` is never referenced by name in the ARM9 | `docs/20` |
| ~~`UnitBlueprint+0x0C..0x13`~~ — resolved: two `u32`s, `+0x0C` is speed in tiles/tick, `+0x10` is an unnamed rate limit | `docs/26` |
| what `UnitBlueprint+0x10` measures — a 20.12 rate limit, `12.0` vs `0.3333`, on nine units only | `docs/26` |
| ~~where `Sim::MagicInfluencedUnitStats` is populated~~ — resolved: `func_0205CC14`, and it copies only four fields, not a block | `docs/29` |
| whether a second pass in `Recompute` modifies the two untouched accumulators (upgrades) — the last 60% of the function is unread | `docs/29` |
| what `MagicInfluencedUnitStats+0x08` is — seeded from the team side, not the blueprint | `docs/29` |
| how movement speed is buffed — it is **not** in the derived stats block | `docs/29` |
| ~~which flag each red brick sets~~ — resolved: all 18, confirmed by a function that reads exactly those flags and no others | `docs/28` |
| ~~the 155-arm rule enum~~ — resolved: 30 handled ids, 125 default | `docs/28` |
| ~~what the twelve non-red-brick rules are~~ — eleven named: rule 78 makes everything cost 1, five drive the shipped debug overlay, four are ungated twins of red bricks | `docs/28` |
| what rule 91 (`+0x19`) displays — it clears two debug buffers but the text is written elsewhere | `docs/28` |
| rule 103 (`+0x1D`) is toggleable and has no reader at all | `docs/28` |
| settings bytes `+0x1B`, `+0x1F`, `+0x20` are read by code but not exposed by the toggle dispatcher | `docs/28` |
| ~~the overlays' 93.9% denominator includes each overlay's literal pools and data, as the ARM7's did~~ — resolved: classified byte-exhaustively as **88.2% code, 10.0% data, 1.5% padding, 0.3% unaccounted**. The walk had also been stopping at the Thumb switch dispatch (`lb_scan` implements the ARM idiom only), which cost 32 points of code coverage | `docs/data/overlay_survey.md` |
| the ds-decomp config has **no ARM7 at all** (`config/eur` and `config/usa` are arm9-only), so the ARM7's 71.5% has no second source to rescue it | `docs/40` |
| the `UI/*.bin` layout format — `func_020E16C8` is the shared parser, and byte 0 is a version (6 in 107 files, 4 in 25) | `docs/44` |
| 70 of the 134 layout files are not referenced by an inline literal, so something loads them from a table | `docs/44` |
| the `.mods` video format (24 files, 33 MB) | `docs/04` |
| `.hps` particle systems (10 files) | `docs/04` |
