# Documentation index

Everything this project knows, and how it came to know it.

**Start here.** [65-how-the-game-works.md](65-how-the-game-works.md) is the
consolidated reference: every gameplay constant in one place, each re-derived
from the cartridge on every run, with the open questions listed rather than
omitted. [30-how-complete-is-this.md](30-how-complete-is-this.md) is the honest
scoreboard. Everything else is how those two were arrived at.

## How this is organised

- **Numbered documents** are findings and decisions. Each is about one thing.
  They are append-only in spirit: when a conclusion turns out to be wrong it is
  **corrected in place, with the wrong version named**, because a silently
  edited document teaches nothing and a reader who remembers the old claim needs
  to see it retracted.
- **`reference/`** digests material this project depends on but does not own.
- **`data/`** holds tables that are generated but tracked, because they are
  small, they are the answer rather than the input, and a diff on them is
  meaningful.
- **`notes/`** (a sibling of `docs/`) is for working material too raw to file —
  hex dumps being stared at, half-formed structure guesses. Also tracked.

## Assessment and decisions

| doc | what it is |
| --- | --- |
| [01-inventory.md](01-inventory.md) | every source, tool and reference available, measured rather than assumed |
| [02-existing-decomp-assessment.md](02-existing-decomp-assessment.md) | the third-party `ds-decomp` project: what to adopt, what it does not have, two defects found |
| [03-tooling-decision.md](03-tooling-decision.md) | Ghidra **and** ndsrecomp **and** the matching decomp, as three tracks that feed each other |

## The cartridge and its formats

| doc | what it is |
| --- | --- |
| [04-rom-layout.md](04-rom-layout.md) | header, ARM9 sections, overlays, the whole NitroFS, both regions |
| [05-the-comp-container.md](05-the-comp-container.md) | the `COMP` compression wrapper — **solved**, 2,178/2,178 |
| [06-overlays-are-missions.md](06-overlays-are-missions.md) | 91 overlays, 90 missions, one each. Its mission table is **superseded by 08**, which recovers all 90 from the ROM |
| [07-the-arm9-ships-its-rtti.md](07-the-arm9-ships-its-rtti.md) | 512 C++ class names, the inheritance graph and 2,454 virtual functions, read straight out of the image |
| [08-the-campaign-table.md](08-the-campaign-table.md) | all 90 missions: class, map, world, side, act, mission — and the two rows that are not corroborated |
| [09-the-function-corpus.md](09-the-function-corpus.md) | 9,312 functions with ARM/Thumb modes, and two independent derivations agreeing on 2,454 of them |
| [11-the-blueprint-format.md](11-the-blueprint-format.md) | `BP/Entities.ebp` solved from its ARM9 loader: 551 records, three classes, named stat fields |
| [12-units-and-buildings.md](12-units-and-buildings.md) | the roster analysed — 20 units per faction, and every building identical across all six |
| [13-the-faction-table.md](13-the-faction-table.md) | `BP/Factions.fbp`: a 26-slot role template, and 124 per-mission variants that are the campaign's tech tree |
| [14-the-arm7-runs-somewhere-else.md](14-the-arm7-runs-somewhere-else.md) | 99.7% of the ARM7 runs at an address the header never mentions — found and handled |
| [15-the-map-format.md](15-the-map-format.md) | `Maps/*.map`: container, terrain and all four record chunks, 122/122 |
| [16-the-game-names-its-own-roles.md](16-the-game-names-its-own-roles.md) | the cartridge ships a C header naming every role, event and spell - and it corrects four of ours |
| [17-the-arm7-corpus.md](17-the-arm7-corpus.md) | 334 ARM7 functions built from three seeds, with modes derived rather than guessed |
| [18-the-ipc-protocol.md](18-the-ipc-protocol.md) | the ARM9-to-ARM7 contract: 32 channels, a 26-bit payload, and the bitmask both cores read |
| [19-a-sixth-of-the-arm9-is-library.md](19-a-sixth-of-the-arm9-is-library.md) | 1,354 contiguous functions are Nintendo SDK, not the game - the decomp target is a sixth smaller |
| [20-the-combat-model.md](20-the-combat-model.md) | damage is a range, the attack interval is in 30 Hz ticks, and a DPS identity proves both |
| [21-the-tile-blueprints.md](21-the-tile-blueprints.md) | `BP/*.tbp` solved 136/136, and the terrain rendering chain from map plane to NCGR |
| [22-collectables-and-the-unlock-space.md](22-collectables-and-the-unlock-space.md) | 342 collectables, 11 categories, and a contiguous 289-slot unlock space |
| [23-the-save-format.md](23-the-save-format.md) | the save is a bit stream - 220 fixed bits plus one per unlockable, validated by the loader |
| [24-spells-and-buffs.md](24-spells-and-buffs.md) | 24 spell names over nine behaviours, and buffs as attached influence objects |
| [25-the-simulation.md](25-the-simulation.md) | the 96-class runtime: entity states, the pathfinder escalation chain, production, and build time |
| [26-reading-a-field-correctly.md](26-reading-a-field-correctly.md) | the blueprint access idiom as a search key, the `u32` width correction, speed as tiles-per-tick, and why the simulation reads derived stats |
| [27-the-collectable-reference.md](27-the-collectable-reference.md) | one field with six meanings: the minifig and minikit links, the 18 red bricks, and the six bonus races |
| [28-the-game-rules.md](28-the-game-rules.md) | the 30-rule toggle set, the code each of the 18 red bricks changes, the owner gate that keeps them off the AI, and the shipped debug overlay |
| [29-the-derived-stats-block.md](29-the-derived-stats-block.md) | `MagicInfluencedUnitStats`: the blueprint copy site, how a buff is applied in fixed point, and the resource yield |
| [30-how-complete-is-this.md](30-how-complete-is-this.md) | measured project completion, and why it is four numbers rather than one |
| [31-the-overlays-and-the-mission-api.md](31-the-overlays-and-the-mission-api.md) | all 91 missions scanned, and the scripting interface the campaign is written against |
| [32-the-objectives.md](32-the-objectives.md) | the 13 objective classes, what every mission is made of, and a coverage figure corrected |
| [33-objectives-have-no-logic-and-the-arm7-plateau.md](33-objectives-have-no-logic-and-the-arm7-plateau.md) | two negatives: an objective's vtable holds no completion logic, and the ARM7 is at its scanning ceiling |
| [34-the-first-matching-bytes.md](34-the-first-matching-bytes.md) | the build-and-compare harness, and the first 351 functions that reproduce the ROM exactly |
| [35-verifying-the-hundred-percents.md](35-verifying-the-hundred-percents.md) | re-checking every completeness claim against the ROM; the ARM7 static section taken to a verified 100% |
| [36-the-arm9-was-never-87-percent.md](36-the-arm9-was-never-87-percent.md) | three whole-section denominators, three wrong coverage figures, and the ARM9 corrected to 99.997% |
| [37-the-arm9-is-complete.md](37-the-arm9-is-complete.md) | the last 36 bytes were alternate entry points; the ARM9's `.text` reaches a verified 100% |
| [38-labelling-the-arm7s-dead-code.md](38-labelling-the-arm7s-dead-code.md) | no strings to label with, so the unreached ARM7 is fingerprinted by the hardware registers it drives |
| [39-the-overlays-were-measuring-the-walk.md](39-the-overlays-were-measuring-the-walk.md) | two published overlay figures both measured this project's own walk; the config had the answer all along |
| [40-earning-annotation.md](40-earning-annotation.md) | 489 functions annotated from strings they reference and from who calls them |
| [41-the-static-initialisers.md](41-the-static-initialisers.md) | strided function families as a lead, and the 209 C++ static-object initialisers they exposed |
| [42-thunks-and-a-detector-that-was-too-specific.md](42-thunks-and-a-detector-that-was-too-specific.md) | 334 tail-call thunks, and the static-initialiser rule corrected for matching an accident |
| [43-constructors-from-the-vptr.md](43-constructors-from-the-vptr.md) | 448 more functions from the vtable pointer they assign, split by how many make the same claim |
| [44-the-ui-layout-files.md](44-the-ui-layout-files.md) | 134 UI layout files, 54 matched to the screen class that loads them, format still open |
| [45-the-no-temporary-rule.md](45-the-no-temporary-rule.md) | which trivial shapes GCC will reproduce exactly, and why the UI parser chase stopped |
| [46-the-screen-layout-container.md](46-the-screen-layout-container.md) | the `UI/*.bin` container decoded on 132 of 132, and why it counts as partial |
| [47-ten-widget-types.md](47-ten-widget-types.md) | the widget type nibble, ten classes named, and the format promoted to decoded |
| [48-the-widget-header.md](48-the-widget-header.md) | the 32-byte widget header, its bounding rectangle, and a denominator that shrank to fit |
| [49-the-nitro-container.md](49-the-nitro-container.md) | 3,475 Nitro files parsed and the graphics payloads decoded; data 10.3% to 78.0% |
| [50-cells-and-animations.md](50-cells-and-animations.md) | NCER and NANR payloads decoded by their own offset arithmetic; data 78.0% to 96.8% |
| [51-models-fonts-and-the-scale-pair.md](51-models-fonts-and-the-scale-pair.md) | NSBMD, NSBCA and NFTR decoded; the partial tier empties; data 96.8% to 99.1% |
| [52-what-the-shipped-header-orders.md](52-what-the-shipped-header-orders.md) | sound_data.sadl read in full: the SEQARC/SE pairing, a bank partition, a shipped bug |
| [53-the-twenty-slots.md](53-the-twenty-slots.md) | the blueprint roster joined to the sound slots, 120 cells; the buildings are one row six times |
| [54-the-archive-agrees-with-the-header.md](54-the-archive-agrees-with-the-header.md) | the 62.9 MB SDAT decoded; its 1,311 symbols equal the header's 1,311 defines |
| [55-nothing-is-opaque.md](55-nothing-is-opaque.md) | banner CRC, the MODS keyframe index, the particle table, the .abp walk; the opaque and known tiers empty |
| [56-follow-the-loader-not-the-bytes.md](56-follow-the-loader-not-the-bytes.md) | the .hps format read out of the ARM9's seek loop; data 99.4% by file |
| [57-the-translation-unit-tier.md](57-the-translation-unit-tier.md) | tier L: 995 attributions from function layout, 96.7% on a hold-out; annotation 42.6% to 53.3% |
| [58-what-a-farm-does.md](58-what-a-farm-does.md) | the unit allowance: min(20, 4 + 4 x Farms); Specials are categories 5 and 6; structures count only at 100% built |
| [59-how-a-hit-resolves.md](59-how-a-hit-resolves.md) | the damage formula and its rounding; +0x6B is "explodes"; the second damage pair is impact vs direct hit |
| [60-building-and-training.md](60-building-and-training.md) | cost, build time, health while building, the six produce preconditions; two unnamed rules named |
| [61-the-empty-function.md](61-the-empty-function.md) | 405 bx-lr functions matched, decompilation 0.157% to 0.264%; a shape tried and removed; tier R |
| [62-what-stops-the-matching.md](62-what-stops-the-matching.md) | four measured blockers and the ceiling: at most 96 of 1,724 small functions remain reachable |
| [63-fog-of-war.md](63-fog-of-war.md) | two bitmaps, a midpoint-circle rasteriser, and a footprint that makes a building's reveal a capsule |
| [64-the-cheats-are-all-x1-5.md](64-the-cheats-are-all-x1-5.md) | both spell red bricks are x1.5 via x+(x>>1); two dead ends recorded |
| [65-how-the-game-works.md](65-how-the-game-works.md) | **the consolidated reference** -- 34 gameplay constants, each re-derived from the ROM, plus the five open questions |
| [66-the-effect-id-is-a-sprite-set.md](66-the-effect-id-is-a-sprite-set.md) | +0x5D is an animation-set id; four of docs/20's five questions are now closed |
| [67-what-a-unit-shoots-first.md](67-what-a-unit-shoots-first.md) | +0x70 is target priority; the last of docs/20's five questions closes |
| [68-a-table-i-read-wrong.md](68-a-table-i-read-wrong.md) | the allowance table is indexed by build-limit class, not category; docs/58 corrected |
| [69-vision-is-authored-per-mission.md](69-vision-is-authored-per-mission.md) | the sight radius is a mission-script literal, 53 call sites in the overlays |

## Where things stand

| doc | what it is |
| --- | --- |
| [10-status-and-roadmap.md](10-status-and-roadmap.md) | what is done, what is next, and every open question carried forward |

## Tools

Everything in `tools/` is standalone, reads only from `out/`, and writes nothing
back into the cartridge tree. Each carries its reasoning in its module
docstring, including what it deliberately does *not* do.

| tool | what it does |
| --- | --- |
| `nds_extract.py` | header, BLZ-decompressed ARM9, autoload split, overlays, NitroFS |
| `lb_comp.py` | the game's own `COMP` container — decode, encode, verify |
| `lb_blueprints.py` | `BP/Entities.ebp` — the 551 unit, projectile and item records |
| `lb_factions.py` | `BP/Factions.fbp` — 130 faction records and their role slots |
| `lb_map.py` | `Maps/*.map` — every chunk, on all 122 maps |
| `lb_lang.py` | `LOC/*.lng` — 3,064 strings across 8 languages |
| `lb_rtti.py` | Itanium C++ RTTI: 517 classes, 2,466 virtual functions |
| `lb_campaign.py` | the 90-mission campaign table |
| `lb_sound_symbols.py` | `Sound/*.sadl` symbol headers |
| `build_corpus.py` | merges every function source into one corpus |
| `lb_code.py` | address space, disassembly, xrefs, callers, strings |
| `lb_scan.py` | recursive-descent function discovery with mode propagation |
| `lb_field.py` | finds the code that reads a blueprint field, by access *idiom* rather than displacement — see [26](26-reading-a-field-correctly.md) |
| `lb_gamerules.py` | the game-rule toggle set: jump table, flags, and readers — see [28](28-the-game-rules.md) |
| `lb_overlays.py` | scans all 91 overlays, one address space each, and ranks the mission API — see [31](31-the-overlays-and-the-mission-api.md) |
| `lb_nitro.py` | parses the Nitro container and the NCLR/NCGR/NSCR payloads — see [49](49-the-nitro-container.md) |
| `lb_ui.py` | parses the `UI/*.bin` screen layout container — see [46](46-the-screen-layout-container.md) |
| `lb_annotate.py` | derives annotations from string references and caller classes — see [40](40-earning-annotation.md) |
| `lb_arm7map.py` | labels the ARM7's unreached runs by subsystem — see [38](38-labelling-the-arm7s-dead-code.md) |
| `lb_verify.py` | re-runs every "100%" claim in the docs against the cartridge — see [35](35-verifying-the-hundred-percents.md) |
| `lb_match.py` | compiles `src/` and byte-compares against the ROM — the decompilation figure — see [34](34-the-first-matching-bytes.md) |
| `lb_stubs.py` | generates C for the ROM's two-instruction functions and manifests them |
| `lb_progress.py` | measured project completion: decompilation, mapping, annotation and format coverage, kept apart — see [30](30-how-complete-is-this.md) |
| `asset_census.py` | what is in the cartridge, by format and size |

## Reference

| doc | what it is |
| --- | --- |
| [reference/lsw2-playbook-digest.md](reference/lsw2-playbook-digest.md) | the LEGO Star Wars II playbook, digested — what carries over and where this project stands against its day-one list |
| [reference/tips-and-pitfalls.md](reference/tips-and-pitfalls.md) | what this game taught, and the mistakes that nearly stuck |
| [reference/attribution.md](reference/attribution.md) | where every borrowed fact and tool came from |

## Conventions

**Addresses** are written as they appear at runtime (`0x02000000`…), not as ROM
file offsets, unless a document says otherwise at the point of use.

**Every number is reproducible.** A document that states a measurement names the
tool and the command that produced it. If a value cannot be read from the ROM,
it is marked as unknown rather than estimated — the project's first rule.

**Retractions stay visible.** Two already:

- `05-the-comp-container.md` — the container's block size was assumed to be a
  fixed 4 KB, which was true of the first five files examined by hand and false
  of 2,125 of the 2,178.
- `08-the-campaign-table.md` — the `mh`/`ma` side assignment that the mission
  titles suggested was the wrong way round, and `06` had correctly declined to
  guess it. `08` settles it from the game's own dialogue.

**Superseded documents are not deleted.** `06` still records how the overlay
count was established and what a third party had recovered; `08` says at the top
that it supersedes `06`'s table, and `06`'s table is the check `08` passes.
