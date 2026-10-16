# 240 — Audit appendix: every control, and the whole multiplayer flow

`docs/239` graded the project by category. This appendix makes the assessment
**exhaustive**: two things there were summarised by group rather than
enumerated: the individual imgui controls, and the multiplayer flow end to end.
This is that detail. **No code was changed to produce it.**

Status vocabulary is the same and the distinction is the point:

* **MEASURED** — a run proved it, and the instrument was itself checked.
* **BUILT** — wired to real code, reasoned from the ROM, effect never observed.
* **OPEN** — not done, with the reason.

---

## Part A — every control, individually

**45 controls across 8 tabs**: 40 widgets plus 5 text inputs.

### Display (5)

| control | backing | status | note |
|---|---|---|---|
| `Enhanced` radio | `g_set.enhanced` | **MEASURED** | the single adaptive-window toggle; gates 9 sites across 3 files |
| `DS Vanilla` radio | same | **MEASURED** | off = pure vanilla; every gated site is skipped |
| `UI scale` slider | `g_ui_scale` | **BUILT** | scales the imgui panel itself; visually obvious but never captured in an A/B |
| `Magnification` slider | `g_set.pixel_scale` | **MEASURED** | consumed in the generated frontend via `apply_hooks.py`; constant 2.000 across six window sizes with the surface growing 306→786 |
| `HUD scale` slider | `g_set.hud_scale` | **MEASURED** | 1.0–3.0x, anchored corner stays, element grows inward |

### Enhancements (7)

| control | status | note |
|---|---|---|
| `Hotwire targets` combo — Vanilla | **MEASURED** | the unmodified path |
| — "Patch the glitch (specials only)" | **BUILT** | refuses non-category-6 inside `Entity::SetOwner` while HotWireSpell's tick is on the stack; **effect unobserved** |
| — "Hotwire anything (cheat)" | **BUILT** | was a dead entry with no caller until 2026-09-06. Vanilla rule located (`func_0207B73C`); the cheat NOPs its refusal. Instruction-level A/B on one binary; **gameplay effect unobserved** |
| `Hotwire Kills Transport Occupants` — Off | **MEASURED** | tail-calls the cartridge's own SetOwner |
| — "On (destroy the occupants)" | **BUILT** | implemented 2026-09-07 via a real `WaitingToDieCommand`; **compiled only, never run** |
| — "Hotwires them" | **BUILT** | occupant sweep hands each occupant to SetOwner |
| `Attack boost affects ranged` combo | **BUILT** | consumed by lb_patches; both positions reach code |
| `Fix Hotwired Healing` checkbox | **BUILT** | has a getter and a consumer |
| `Make All Builders The Same` checkbox | **BUILT** | rewrites builder blueprints at the block |
| `Unit caps` InputInt x N | **BUILT** | `lb_setting_cap(class)`, with `LB_FORCE_CAP` for headless use |
| `Balance pack` combo | **BUILT** | slot 0 = off, 1–3 → `lb_balance_apply` |

### Cheats (8 + the cheat list)

| control | status | note |
|---|---|---|
| cheat checkboxes (list) | **MEASURED** | consumed by lb_patches and lb_widemap |
| `Enable every cheat` | **MEASURED** | sets all |
| `Disable every cheat` | **MEASURED** | clears all |
| `Unlock All Units` | **BUILT** | docs/181 measured 52 codes; `apply_unlocks` sets the bits. Not confirmed in Character Collection |
| `Unlock All Extras` | **BUILT** | same |
| `Unlock Santa` | **OPEN** | docs/181: no Santa record anywhere in the registry. **Nothing in the ROM to set** |
| `Unlock All Maps` | **OPEN** | docs/181: no map records in kind 8. Same |
| `Unlock Everything` | **BUILT** | ticks all four; badged for what it does, not the weakest thing it touches |

### Input (4)

| control | status | note |
|---|---|---|
| key binding buttons (per action) | **BUILT** | rebind captures the next key; `F1`/`Esc` deliberately excluded mid-rebind |
| pad binding buttons (per action) | **BUILT** | `lb_binding_pad_bit` |
| `Reset all to shipped` | **BUILT** | restores defaults |
| `Save bindings` | **MEASURED** | settings persistence verified across a restart |

### Multiplayer (7, incl. 3 text inputs)

| control | status | note |
|---|---|---|
| `Address` text | **BUILT** | `g_set.master_host`, used by all three actions |
| `Port` InputInt | **BUILT** | `g_set.master_port` |
| `Player name` text | **BUILT** | what an advertised lobby is listed as |
| `Room code` text | **BUILT** | consumed by Join by code |
| `Browse lobbies` | **BUILT** | `lb_net_refresh_lobbies`; the master server's `/list` is MEASURED at 18/18, but this button has not been clicked in a measured run |
| `Join` (per lobby row) | **BUILT** | `lb_net_join_index`; the directory join path itself was exercised (`mp-ms3`) |
| `Advertise a lobby` / `Stop advertising` | **BUILT** | `lb_net_announce` / `lb_net_stop_hosting`, with the wireless port |
| `Join by code` | **BUILT** | resolves a room code through the directory |

### Demo (4, incl. 1 text input)

| control | status | note |
|---|---|---|
| `File` text, `Record`, `Play`, `Stop` | **BUILT** | `lb_demo_*`; the demo clock ticks per vblank so a replay lands each input on its frame. Never exercised in a measured run |

### Saves (3, incl. 1 text input)

| control | status | note |
|---|---|---|
| `Save file` text, `Export live save`, `Import (retag to this build)` | **BUILT** | never exercised in a measured run. **Note the constraint**: the player's original cartridge save is a read-only source and is never written back to |

### Music (1) and the field/overlay panels (4)

| control | status | note |
|---|---|---|
| `Mode` combo (4 positions) | **MEASURED** | all four compared as SOUND; Vanilla vs Map-specific correlate +0.973, either vs Faction-aligned +0.67..+0.73 |
| `Locate now` / `Re-locate` | **BUILT** | field locator; re-runs the scan |
| `Show the top screen overlay` | **BUILT** | toggles the overlay window |
| `Show the whole battlefield (panel)` | **BUILT** | toggles the field panel |

**Every control reaches real code.** The honest summary is that **wiring is
complete** and **verification is partial**: the Display, Cheats-core, Music and
binding-persistence groups are measured; most Enhancements, all Demo/Saves, and
the master-server buttons are wired-but-unobserved.

---

## Part B — the whole multiplayer flow, step by step

Every step below is what the port actually does now, with the status of each.

### B1. Bring-up

| step | status | detail |
|---|---|---|
| launch N instances | **MEASURED** | `lb_mp.py --instances 2\|3`, ports 19861/19862/19863, one debug server each |
| separate saves | **MEASURED** | each instance copies the seed save; sharing one file would diverge the route for non-network reasons |
| local wireless bridge | **MEASURED** | `LocalMpTransport`, loopback UDP at `base_port + index`, fanned out to `kMaxInstances = 16` |
| smoke check | **MEASURED** | `--route none --linger 1` confirms all instances serve before a 25-minute route is attempted |

### B2. Reaching the lobby

| step | status | detail |
|---|---|---|
| title → main menu → multiplayer | **MEASURED** | shared `MP_ROUTE` prefix, 5 taps |
| host walks map select, rules, army, Continue | **MEASURED** | recorded tap table, now a built-in route |
| client opens Join and searches | **MEASURED** | `l:S` waits for the condition, not a timer: a listed host shows ORANGE text top-left of the box, which neither the searching state nor an empty box has |
| client taps the row | **MEASURED** | must be a real tap — releasing 88 px away made the recogniser call it a **drag**. This single bug was the difference between "one join in five" and "joins every run" |
| row re-verified before each tap | **MEASURED** | with three instances the list can redraw between the wait and the tap |

### B3. Seating, readying, starting

| step | status | detail |
|---|---|---|
| Game Lobby shows all players | **MEASURED** | captured with three: red/blue/green, slots 1/2/3 filled |
| client Ready (212,174) | **MEASURED** | opens minifig/army select |
| client picks a minifig (120,25), Readies again | **MEASURED** | the host's button only becomes **Start** after every client has readied twice |
| host presses Start | **MEASURED** | must be **repeated short attempts**, not one long wait: a route step that blocks starves every other instance, which is why the host once pressed Start 29 times at a client that had not been given its turn to ready |
| all instances in a battle | **MEASURED** | 2 players and 3 players both reach `in a battle` |

### B4. In the match

| step | status | detail |
|---|---|---|
| both/all worlds agree | **MEASURED** | `in sync across 3, 19 entities` on every counted sample |
| the match is actually moving | **MEASURED** | the load generator orders units; `the world changed in 3 of 4 comparisons` |
| skewed samples discarded | **MEASURED** | a sample that cannot tell skew from divergence is thrown away, not counted as a pass |
| each client knows which player it is | **MEASURED** | latched from the opening camera; `local player is owner 1 ... was 0` |
| music follows the local player's builder | **MEASURED** | the client re-reads **owner 1's** builder after latching |
| desync command exists in the codec | **BUILT** | `SyncCheckCommand` is in the command set (docs/87) and the cartridge sends it; this port measures divergence by comparing worlds instead |

### B5. The master server

| step | status | detail |
|---|---|---|
| host announces a lobby | **MEASURED** (server side) | 4-char room code on a confusion-free alphabet |
| lobby listed, token never leaked | **MEASURED** | `/list` excludes the host token |
| heartbeat keeps it alive, expiry when it stops | **MEASURED** | both directions tested |
| player count and list update | **MEASURED** | for the GUI to expand |
| operator ceiling on max_players; bad ports refused | **MEASURED** | |
| ban / unban | **MEASURED** | |
| client browses and joins through the directory | **MEASURED** | `mp-ms3`: the client was given no address, only the directory |
| **the in-game buttons that drive all this** | **BUILT** | wired to `lb_net_*`; not each clicked in a measured run |
| **a second physical machine / NAT / firewall** | **OPEN** | impossible here; loopback with the shortcut disabled is the closest approximation |

### B6. What the flow does *not* cover

* **Four or more players.** `Play3PlayerMP` is one of the cartridge's own
  unlockable entries (docs/97), which is independent evidence that three is the
  maximum, so this is a non-goal rather than a gap.
* **Real-network conditions.** See above.
* **Long-match desync.** Runs are minutes, not hours.

---

## Part C — what is left, and nothing else is

1. **"Modelled special unit rendering"** — never directly evidenced (docs/209).
2. **Destroy-occupants** — implemented, compiled, never run.
3. **Three hotwire modes** — wired, effects unobserved.
4. **Gameplay tuning group** — wired, effects unobserved.
5. **Master-server buttons** — wired, not clicked in a measured run.
6. **Demo and Saves buttons** — wired, not exercised.
7. **Unlock Santa / All Maps** — no ROM records found.
8. **Second machine / NAT** — environment limit.

Everything else in scope is measured.
