# ARM9 RTTI extraction — class and vtable map

Source: `out/eur/arm9/section_static_02000000.bin` (1,378,432 bytes, loaded at
`0x02000000`), read byte-for-byte. Ghidra output was used only to cross-check,
never as input.

Outputs:

- `rtti_classes.tsv` — 517 rows, one per `type_info` object.
- `vtable_methods.tsv` — 3,811 rows, one per vtable slot.

## Headline numbers

| | |
|---|---|
| `type_info` objects found | **517** (513 game classes + 3 `__cxxabiv1` + `std::type_info`) |
| Classes with a vtable | 515 |
| Vtable slots total | 3,811 (3,790 with a target, 21 null pure-virtuals) |
| Distinct functions given a ground-truth name | **2,466** |
| …of those, real inventory functions | 1,178 → **157,378 bytes = 15.54 %** of 1,012,921 |
| …of those, thunk/external (excluded from coverage) | 2 |
| …of those, in *neither* the inventory nor Ghidra | 1,286 → ~64,548 bytes (gap estimate) |
| Combined estimated coverage | **~21.9 %** of 1,012,921 ARM9 bytes |

The denominator is the corpus README's own: 6,838 real functions / 1,012,921
bytes, after thunks and externals. Two RTTI-named functions are thunks and are
excluded from the numerator to match.

The 15.54 % figure is the defensible one: it uses measured function sizes. The
~21.9 % figure adds a next-start-minus-this-start estimate for the 1,286
functions no prior tool knew about, and is therefore softer.

## Verified layout

Standard Itanium C++ ABI (devkitARM/GCC), confirmed against the bytes rather
than assumed. The three ABI `type_info` vtables live at the very end of the
image and are the anchors for the whole scan:

| ABI class | vtable address | `type_info` shape |
|---|---|---|
| `__cxxabiv1::__class_type_info` | `0x021507e0` | `[vtable][name_ptr]` |
| `__cxxabiv1::__si_class_type_info` | `0x021507d0` | `[vtable][name_ptr][base_ti]` |
| `__cxxabiv1::__vmi_class_type_info` | `0x021507c0` | `[vtable][name_ptr][flags][n][(base_ti,offset_flags)×n]` |

A vtable group is `[offset_to_top][type_info_ptr][slot0][slot1]…`; the pointer
stored in an object is `&slot0`, i.e. two words past the group start. The type
name is a null-terminated Itanium-mangled string (`N7Battles15EntityBlueprintE`)
reachable through `name_ptr`; the strings are interleaved with the `type_info`
and vtable objects in `0x0213aa00…0x021507e0`, not pooled separately.

`rtti_classes.tsv` records the name string address per class, so the
"type_info pointer sits immediately before the first vtable slot" assumption in
the brief is confirmed — but note it is the *pointer* that precedes slot 0, and
`offset_to_top` precedes that.

## Layout surprises

Four things would have produced wrong answers if taken on faith:

1. **Half the vtable slots are Thumb.** 2,171 of 3,790 slot values have bit 0
   set. The real function address is `slot & ~1`. A scan that demands 4-byte
   alignment silently drops 222 vtables to zero slots — that was the first
   result here, and it looked like a plausible "many abstract classes" answer.
2. **Pure virtuals are encoded as literal `0`, not `__cxa_pure_virtual`.** The
   string `__cxa_` does not occur anywhere in the image. Nulls appear
   *mid-vtable*: `Battles::Sim::EntityCommandBase` has four consecutive nulls at
   slots 8–11 followed by real slots at 12 and 13. Stopping at the first null
   truncates those tables.
3. **A range check of `0x02000000..0x02200000` is not enough.** Code ends at
   `0x021350f0`; RTTI/rodata begins at `0x0213aa00`. One vtable
   (`Battles::User::WorldViewScreen`, `0x0214f0ec`) is immediately followed by a
   spell-name string table, and 31 string pointers were being walked in as
   method slots — they passed the wide range check the brief suggests.
   Bounding slot targets to `< 0x0213a000` removes exactly those 31 and loses no
   real slot (zero slot targets fall in the `0x021350f0..0x0213aa00` gap). This
   one is a trap for a fresh scan rather than a bug that was in the shipped
   data: `lb_rtti.py`'s documented `--code-end` already excluded that region.
4. **No secondary vtables and only one `vmi` class.** `Battles::Texture` is the
   single `__vmi_class_type_info`, and it still has one base
   (`Battles::SListItem`) and one vtable. Every other class is single
   inheritance. `offset_to_top` is 0 everywhere — there is no multiple
   inheritance thunk layer to model in the port.

## Validation

- **97.8 % of slot targets (2,434 / 2,489 before the bound fix) begin with a
  plausible prologue** — `stmfd sp!`, Thumb `push`, `bx lr`, or a one-instruction
  `ldr` accessor. After the code-region bound, the only remaining "odd"
  prologues are `ldr rX,[rY,#imm]` and `cmp` — single-expression virtual getters,
  which is what a 1-instruction virtual should look like.
- **No vtable is truncated early.** For every one of the 515 vtables, the word
  immediately past the last recorded slot (and past any trailing zeros) is a
  string, a pointer into the RTTI/rodata region, or the `offset_to_top` of the
  next group. Not one terminates on something that could have been another
  method pointer.
- **35 of 35 identity claims in `subsystem_map.tsv` agree, 0 mismatches.** The
  three apparent misses (`0202e624`, `020d0cf8`, `020dad9a`) are `caller
  X::virtual_NN` notes, not claims that the address *is* that method.
- Spot disassembly of functions the inventory lacks confirms them: `0x02000c3c`
  / `0x02000c50` are a textbook D1/D0 destructor pair, `0x02002260` is a
  destructor that stores its vtable pointer (`ldr r1,[pc,#80]; str r1,[r5]`).

## Namespace breakdown

| Namespace | Classes | Slots | Bytes (inventory-listed) |
|---|---:|---:|---:|
| `Battles` | 303 | 2,067 | 58,754 |
| `Battles::User` | 100 | 977 | 34,256 |
| `Battles::Sim` | 94 | 735 | 63,212 |
| `Battles::Sim::OccupationGrid` | 5 | 4 | 820 |
| `__cxxabiv1` | 3 | 6 | 144 |
| `Battles::SaveGameEvent` | 2 | 4 | 96 |
| `Battles::ScriptingMacros` | 2 | 4 | 0 |
| `Battles::User::Map` | 2 | 4 | 0 |
| `Battles::CreateGameEvent` | 1 | 2 | 44 |
| `Battles::Palette` | 1 | 2 | 20 |
| `Battles::ScriptingMacros::Timer` | 1 | 2 | 0 |
| `Battles::Sim::PathFinding` | 1 | 2 | 20 |
| `Battles::User::ActionMap` | 1 | 2 | 12 |
| `std` | 1 | 0 | 0 |

By class count `Battles` dominates, but 92 of its 303 classes are
`CampaignMission*_*Act*` scenario scripts with 11–14 slots each and tiny bodies.
By *bytes*, `Battles::Sim` leads (63,212) despite having a third as many
classes — the simulation is where the large virtuals live. `Battles::Sim` and
`Battles::User` together account for 62 % of the named bytes.

## Inheritance shape

- 70 root classes (no base); 132 at depth 2, 285 at depth 3, 29 at depth 4, 1 at
  depth 5 (`Battles::Sim::AstarPlotter`).
- Biggest base-class hubs: `Battles::CampaignMissionBase` (90 direct subclasses),
  `Battles::User::ScreenType` (63), `Battles::Effect` (30),
  `Battles::CommandBase` (27), `Battles::Sim::EntityCommandBase` (27),
  `Battles::SListItem` (25), `Battles::EventBase` (22), `Battles::DListItem` (21).
- Vtable sizes: 146 classes have 2 slots (destructor pair only); the common
  behavioural sizes are 12 (90 classes), 5 (66), 13 (65), 11 (41), 14 (27).

## What this buys the port

- 1,482 functions sit at slot ≥ 2 — real behaviour, now named by class and slot.
- 984 functions are destructor pairs only (slots 0/1) — cheap to stub correctly.
- The RTTI-named functions are *small*: median 41 bytes vs. 68 for the inventory
  as a whole. RTTI names many leaves, not the giants. Only 20.3 % of the bytes in
  `subsystem_map.tsv`'s 200 largest functions get an RTTI name, so this does not
  replace the subsystem classification — it complements it.
- The 12 largest RTTI-named functions, for orientation:
  `Battles::Sim::ConstructStructureEntityCommand::virtual_09` (3,232 B),
  `Battles::User::JobAttackTaskForce::virtual_01` (2,892),
  `Battles::Sim::ConstructMultipleEntityCommand::virtual_09` (2,456),
  `Battles::SpriteUIEngine::virtual_02` (2,252),
  `Battles::Sim::DamageSpell::virtual_02` (1,840),
  `Battles::User::SplashScreen::virtual_09` (1,822).

## Known gaps — explicitly not determined

- **Method names are not in the ROM.** RTTI ships *class* names only. The
  `proposed_symbol` column is `Class::virtual_NN`, a positional label. The
  original method names are not recoverable from this data.
- **1,286 named functions have no measured size.** They are absent from both
  `arm9_inventory.tsv` and Ghidra's `decomp_all` (only vtable slots reference
  them, so auto-analysis never created them). Their ~64,548-byte contribution is
  a next-start-minus-start estimate, not a measurement. These addresses are worth
  feeding back into the function finder — see the note on dropped tails.
- **A vtable whose *last* virtual is pure is under-counted.** Nulls are committed
  as slots only when a later real slot proves the walk is still inside the table
  (`pure_virtual_slots` counts the 21 that qualified). Zeros still pending when
  the walk ends are dropped as padding — correctly for most classes
  (`Battles::EntityBlueprint` is followed by 23 zero words of section padding),
  but a trailing pure-virtual is indistinguishable from padding in the bytes
  alone. 90 vtables end on at least one zero word, so for those `num_slots` is a
  lower bound. A derived class's vtable length would settle it per class; that
  cross-check was not used to adjust any number here.
- **The "~1,023 vtables" figure in the earlier analysis is wrong.** The ARM9
  static image contains 517 `type_info` objects and 515 vtable groups. 1,023 is
  very close to the 512 `typeinfo_*` + 511 `vtable_*` symbol *lines* in
  `out/eur/rtti_symbols.txt`, which is almost certainly where it came from. The
  class count is ~513, not ~1,023.
- **Overlays carry no RTTI.** All 92 `out/eur/overlays/ov*.bin` were checked for
  both the three ABI vtable pointers and the literal string `N7Battles`: zero
  hits in any overlay. The ARM9 static image is the complete set. (An overlay
  compiled with its own ABI vtable copies would be missed by the pointer check,
  but the string check rules that out.)
- **No non-class `type_info`.** No `__pbase_type_info`, `__pointer_type_info` or
  `__fundamental_type_info` strings exist in the image, so `typeid` is never
  applied to a non-class type. Nothing is being missed on that axis.
- Coverage is expressed against the 1,012,921-byte figure in the brief; the
  inventory itself sums to 1,013,373 bytes over 6,903 functions (a 452-byte
  discrepancy, not chased down). The full code span `0x02000430..0x021350f0` is
  1,264,832 bytes, against which the combined estimate is 17.55 %.

## Reproducing

    python tools/lb_rtti.py out/eur/arm9/section_static_02000000.bin \
        --code-end 0x0213a000 \
        --classes-tsv docs/data/rtti_classes.tsv \
        --methods-tsv docs/data/vtable_methods.tsv \
        --inventory   docs/data/arm9_inventory.tsv

`--code-end` is not optional in practice: left out, the tool defaults to the
image end and walks the `Battles::User::WorldViewScreen` vtable into the spell
name table (surprise 3 above). `0x02125fc4`, the bound
`docs/07-the-arm9-ships-its-rtti.md` has always used, gives byte-identical
output — the highest real slot target is `0x02121970`, so any bound between
that and `0x0214eed4` behaves the same. `0x0213a000` is preferred only because
it is derived from where code actually ends (`0x021350f0`) rather than chosen.

Three fixes went into `tools/lb_rtti.py` for this pass; the first two changed
the answer:

- the mangled-name regex now accepts `$` and `.` inside an identifier, which
  recovers the four internal-linkage `OccupationGrid::…MatchFunc$NNNNN_cpp`
  classes, and accepts the `St<len><id>` form, which recovers `std::type_info`
  (512 classes → 517);
- the slot walk buffers nulls instead of stopping at the first one, which
  restores the truncated interfaces of eight abstract bases — including
  `Battles::Effect` (2 slots → 11, 30 subclasses) and
  `Battles::Sim::EntityCommandBase` (5 → 14, 27 subclasses);
- null slots are skipped in the symbol list rather than emitted as address 0.

This extraction was cross-checked by writing a second, independently anchored
extractor (it takes the three ABI vtable addresses as fixed constants and
demangles via `c++filt` fed `_ZTS` + the raw name, where `lb_rtti.py` *measures*
the ABI vtables from a histogram of name-pointer referrers and demangles by
hand). The two agree byte-for-byte on every shared column of both TSVs.
