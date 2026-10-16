# Port progress

> **RETRACTED IN PART (docs/105).** This page calls `func_020872DC`
> `HotWire_ResolveTarget` and `func_02087350` `EntityAtTile`. Both names are
> wrong. `func_02087350` resolves a two-byte **entity handle** against the pool
> at `0x02155028`; `func_020872DC` adds a **liveness** test. They are the
> engine-wide handle resolver (80 and 264 callers), not hotwire code and not a
> tile lookup. The override the port built on them is removed. Read anything
> below that rests on those names with that correction in mind.


**Percented progress rates against the port's seven feature items, updated
every pass.**

Two columns, deliberately. **Understanding** is whether the ROM has been read
well enough to build the feature. **Built** is whether it exists.

**The overall row is the plain mean of the seven items and nothing else.** An
earlier version of this table showed `~54% / ~36%`, which matched neither
column: the built figure was silently crediting infrastructure — the lift, the
runner, the boot — that is not one of the seven feature items. That inflated the
headline roughly fourfold, more than once, before being caught. Infrastructure is now recorded below the table, where it cannot
contaminate the score.

## Progress — 2026-08-29

| # | feature item | understanding | built |
| ---: | --- | ---: | ---: |
| 1 | Rendering: Vanilla / Enhanced, FOV, zoom, ImGui overlay | **55%** | **75%** |
| 2 | Rebalance patch + lbmods support | **85%** | **55%** |
| 3 | Enhancement toggles (hotwire, healing, builders) | **80%** | **55%** |
| 4 | Rebindable inputs (controller + KB/M) | **70%** | **80%** |
| 5 | Master server, netcode, room codes, miniupnpc | **80%** | **70%** |
| 6 | Cheat tab (hotwire anything, unlocks) | **65%** | **10%** |
| 7 | Music modes (vanilla/random/faction/map) | **85%** | 0% |
| | **overall** (mean of the seven) | **67%** | **~42%** |

**The static lift runs** (`docs/90`). All 95 programs audit clean with verified
SHA-1s, zero undefined functions and no codegen gaps; the ARM9's 9,156 functions
are emitted as C. All 205 translation units then **compile to native objects with zero errors**
(`port/lift.sh compile`).

**The runner now configures against those banks** (`port/RUNNER.md`). Three
requirements had to be met first, each found by being refused: title bank names
must carry `_arm9_`/`_arm7_` so the CPU can be inferred (the overlays were
renamed `lb_arm9_ov###`); the four BIOS banks are not generated for you and had
to be recompiled from `ndsrecomp/bios/`'s TOMLs; and FreeBIOS ships as vendored
C arrays rather than images, so both were extracted and hash-verified. **The executable boots** (`docs/94`). `nds_runner.exe` loads the cartridge,
registers the recompiled title banks against the exact ROM hash, initialises the
OpenGL 4.3 compute renderer, and **both CPUs run**: the ARM9 clears
`Boot_WaitLoop`, the ARM7 reaches its normal `SWI 6` / `SVC_Halt` idle, and
executed coverage reaches **107 code pages** with zero dispatch misses. The
FreeBIOS build is identical, so no BIOS dumps are needed to run it.

*(Superseding `docs/91`, which recorded a hang in `Boot_WaitLoop` at 6 code
pages. That was real at the time; `docs/94` found the cause — ROM cannot earn
write provenance — and fixed it.)*

It is a bring-up milestone, **not a playable game**: no menu has been reached
and no frame has been confirmed.

`docs/92` diagnoses that stall precisely: the loop polls **`REG_IPCSYNC`**, and
the ARM7 never answers because 96 KB of its code — the `mainram` autoload — was
excluded from every scan by a set literal in `lb_scan`. Fixed, that section
yields **153 functions (41.9%)** and the lift now spans **96 programs and 10,003
functions**. Whether it completes the handshake is untested.

Underlying it: **9,314 ARM9 functions mapped** with modes and a verified call
graph, **54.4% annotated**, **99.4% of data files decoded**, **121 verification
checks passing**.

## Item by item

### 1. Rendering — 35%

**Known.** The simulation cannot see the view (`docs/85`) — proven twice — so
Enhanced's wider field of view and zoom cannot desync against a Vanilla client,
and rendering style is **client-local**, needing no lobby enforcement. The two
screens are `User::WorldViewScreen` (touch, the game field, and therefore the
Enhanced main window) and `User::WorldViewTopScreen` (what the ImGui overlay
replaces). 64 UI screens are mapped to their layout files.

**Not known.** The rendering path itself — BG layers, tile streaming, sprite
budget — which is what actually limits how much extra field of view can be
drawn. `WorldViewScreen`'s slot 8 is `func_020DAD5C`, **3,580 bytes with four
interior entry points**, the largest and most-entered function in the corpus;
unread, and a static lift must get its entry points right. Touch input handling
is unread.

### 2. Rebalance and mods — 70%, the most advanced item

**All three mods diffed against the VANILLA build in the same folder** (which is
the right baseline — same toolchain, so no build noise). `tools/lb_moddiff.py`
is new and does this by NitroFS path.

**The headline: all three are pure data mods.** ARM9, ARM7 and the overlay table
are **byte-identical** to VANILLA in every case. So "instant application of the
rebalance patch" is a **data-set swap**, not code patching — a far easier
feature than first assumed.

```
                files changed   ARM9      what
REbalance            32         identical  Entities.ebp, 13 maps, text, minimaps
FactionWars           6         identical  Entities.ebp, text, 3 tileset palettes
FUNLite               3         identical  Entities.ebp, text
```

`Sound/sound_data.sdat` shrinks identically in all three — a shared base trim,
not a gameplay change.

**The stat changes are extracted field by field** into
`docs/data/mods/entity_stat_diff.tsv` — **1,890 field changes** across 551
entity records:

```
REbalance    138 of 551 entities  cost(76) build_time(76) attack_interval(68) health(66)
FactionWars  132                  cost(95) health(93) build_time(81) attack_range(41)
FUNLite      120                  attack_interval(79) attack_range(74) health(67) speed(59)
```

No entities added or removed by any mod.

**Not known.** The 13 changed `Maps/ck*.map` files are not diffed at map level
(the format is decoded in `docs/15`, so this is work not risk). The `.lng` text
diffs are not extracted. The `MapDump` family (4,011 FAT entries vs `MapBase`'s
4,006) is not yet compared — that is where "additional levels" likely live.

### 3. Enhancement toggles — 45%

**Patch Hotwire Glitch.** Localised. `HotWire_ResolveTarget` (`func_020872DC`,
44 bytes) resolves the entity at the target tile through `EntityAtTile`
(`func_02087350`) and accepts it only when `entity->[0xE0]->[4]->+8 == 0`. The
lookup underneath uses a **tile map holding one occupant per tile with no
disambiguation for stacked entities** — which is exactly the known exploit: park a special over the target, tap the special's corner, and the
lookup hands back the wrong occupant. Both the fix and the "Hotwire Anything"
cheat are **single-function overrides**.

**Make All Builders The Same.** Category 1 is Builder, six of them, one per
faction (`docs/72`), and every stat field is already decoded, so this is a data
override like item 2. The Imperial builder's record is identifiable.

**Not known.** *Fix Hotwired Healing* — not investigated at all; needs the heal
path and what team-switch does to an in-progress heal. The exploit's input path
(stylus-on-corner hit-testing) is unread, which matters for the fix's precision.

### 4. Rebindable inputs — 5%

**Nothing read.** Input handling has not been touched. Known only in outline:
the ARM7 owns touch (`docs/14`), and the ARM9↔ARM7 protocol is ten channels
(`docs/18`) with meanings partly identified. This is the least-advanced item and
the one most likely to be underestimated.

### 5. Master server and netcode — 80% understood, 30% built

**Known, and better than expected.** The wire protocol is a **closed,
enumerated set**: `CommandBase` has 27 subclasses (`docs/data/commands.tsv`) and
in lockstep these are what peers exchange — `MoveEntitiesCommand`,
`AttackCommand`, `ConstructStructureCommand`, `CastSpellCommand` and the rest.
`Sim::EntityCommandBase`'s 30 subclasses are derived per-entity state and are
never transmitted. **The cartridge already ships desync detection** as
`SyncCheckCommand` and `SyncStatusCommand`, so the port reproduces a scheme
rather than inventing one. The lobby layer is enumerated: `HostLobby`,
`ClientLobby`, `JoinWifiGame`, `LobbySettings`, `MultiplayerMenu`, `WiFiScreen`,
plus `WiFiScanForParentsEvent` — which is the "search for local game" the port
repoints at a master server.

**The encoding is now read** (`docs/87`). `CommandBase`'s slot 3 is serialise and
slot 4 deserialise; the difference is that slot 3 passes fields by value and slot
4 passes their addresses and returns a boolean. `Stream_Write32` writes four
unrolled bytes least-significant first; `Stream_Read32` reads four and
**short-circuits to false on any one failing**, so deserialisation is validated.
`Stream_WriteByte` fills a primary buffer up to a threshold then spills to an
overflow buffer; `Stream_ReadByte` fails on empty. Eighteen primitives form the
whole codec, and most commands write **single bytes** directly rather than words.

**BUILT.** `services/master-server/` — both flavours (console and GUI), and
**18 of 18 tests pass** against the live service. Standard library only. The cmd
flavour shows logs and keeps them on disk; the GUI runs the same server in a
thread and adds the lobby sidepanel, each row expanding to its connected
players, with right-click close/ban. Room codes are 4-character Crockford Base32;
advertisements expire after 90s without a heartbeat; bans persist. Endpoint
shape, TTL and code alphabet match the LSW2 server deliberately, and the default
port 28555 sits beside its 28553 so one host can run both.

**The player cap is deliberately not assumed.** `func_02086E5C` iterates
`world->players_8[i]` for `i < world->count_C`, so the count is **runtime state,
not a compile-time constant**. `SkirmishSetup` and `LobbySettings` both use 2 and
3 as immediates, consistent with a 3-player maximum,
but that is not proof — so the host states `max_players` and the server only
enforces an operator ceiling.

**The type tag is found** (`docs/88`). It is the first argument to
`CommandBase`'s constructor, stored at **`command+0x0C`**, and the base
constructor lays out the whole header: vtable at +0, 0 at +4, 2 at +8, the type
id at +0x0C, and a `u16` owner at +0x10. **26 classes carry 26 distinct ids**
(`docs/data/command_ids.tsv`), with `SyncCheckCommand` = 5 and `SyncStatusCommand`
= 6. `TeamCommand` is the one exception — its id arrives as a parameter rather
than a literal — and is recorded as unresolved.

**The packet layout is read** (`docs/89`). `Command_Send` installs the buffers,
writes a header and calls slot 3; `Command_Receive` brackets the read, reads the
header, calls slot 4 and measures the packet by cursor delta. The layout is
**`u8` type id, `u16` owner, then the body** — no length field, no magic, no
padding, which is safe precisely because every field read is checked. The raw
vtable offsets (`+0xC` and `+0x10`) confirm `docs/87`'s slot assignments from a
second direction.

**Not known.** The tag *peek* — `Command_ReadHeader` discards the byte, so
something above already chose the class. That is the last small piece of the
receive path. Also unread: what the sync commands hash and how often, what a
lobby holds, and the DS wireless transport that carries these packets.

### 6. Cheat tab — 50%

**Known.** The game-rule / red-brick table is mapped (`docs/28`) and the cheats
are uniformly ×1.5 multipliers (`docs/64`). Critically, `docs/63` showed **Reveal
Map is an OR in the visibility query, not a write to the fog map** — which is
why it is lockstep-safe, and it is the model every cheat here should copy.
Unlock state is the unlock-slot space already decoded (`blueprint+0x6C`, 289
records across five unlockable categories). "Hotwire Anything" is the same single
branch as item 3.

**Not known.** Which slots correspond to Extras, Maps and specifically Santa;
where progression is persisted (the save format is `docs/23`, but the unlock
write path is unread).

### 7. Music modes — 55%

**Known.** 98 `STRM` tracks, **already grouped by faction in their own symbol
names**, and the six factions pair cleanly into the three worlds the map-specific
option needs: King/Wizard = castle, Pirate/Imperial = pirate, Earth/Alien =
space. Front-end themes exist per world. Music is presentation, so all four modes
are **client-local** by `docs/85`'s rule.

**Not known.** Where multiplayer music is *selected* — the code that hardcodes
King for MP has not been found, and that is the hook every mode needs. Whether a
map declares its world, or it must be inferred.

## What this pass did

* Built `tools/lb_moddiff.py` and diffed all three mods.
* Established the mods are **pure data** — the single most useful finding for
  item 2.
* Extracted 1,890 entity stat changes to `docs/data/mods/entity_stat_diff.tsv`.
* Enumerated the music sets against the four music modes.

## Next, in order of what unblocks most

1. **The tag peek** — whatever selects a class from the leading byte. Small,
   and the last gap in the receive path.
2. **Where multiplayer music is chosen** — small, and completes item 7.
3. **`MapDump` vs `MapBase`** — finds the additional levels for item 2.
4. **The heal path** — the one unexamined enhancement in item 3.
5. **Input handling** — item 4 is at 5% and needs a first look before it can be
   estimated honestly.

## Boot milestone (docs/94)

**The port boots.** Both CPUs execute recompiled native code, the ARM9 clears
`Boot_WaitLoop`, the ARM7 reaches its normal `SWI 6` / `SVC_Halt` idle, and
executed coverage went from 6 to **85 code pages**.

The blocker was not the IPC handshake. Every ARM7 BIOS dispatch entry was
inactive because `--validate-live-bytes` requires *write provenance*, and ROM
can never acquire it — `written_for_ptr()` shadows five RAM regions and no BIOS.
Cartridge banks keep the flag; the BIOS banks, already SHA-1-verified at load
and immutable, no longer carry it.

Booting completes none of the seven feature items on its own, so only item 1
moves (0% -> 10%: the configured display profile is now actually applied). Its
value is that every remaining item needed a running executable to be testable at
all, and none of them were until now.

## Item 5: the server half is done, the client half is not

`services/master-server/build_exe.sh` produces both executables (a console
build and a GUI build), and the console one is **verified as a binary**: it serves `/health`,
accepts an announce, issues a room code and token, lists the lobby, and writes
both console and file logs.

**The GUI executable is built but not runtime-verified.** It is a windowed
binary, so exercising it means opening a desktop window, which the
automated tests avoid. Its tkinter sidepanel is covered by the source
tests; the *packaged* build is not.

What remains on item 5 is entirely client-side and is the larger half:

* the in-game *search for local game* menu still queries for nearby DS systems
  rather than the master server, and that overhaul is untouched;
* direct-IP adhoc connection;
* miniupnpc auto-port-forwarding;
* carrying `options` — the host's lobby-carried rule set — into the actual
  simulation, which is what makes items 3 and 6 host-authoritative.

The protocol reading (`docs/87`-`89`) is what makes that client work possible,
but none of it is written yet.
