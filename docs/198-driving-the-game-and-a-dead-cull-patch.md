# 198 — Driving the game, verbose logging, and a cull patch that does nothing

Status: **harness work, one user request delivered, one negative result.**

## Verbose debug output, with a build stamp

Asked for directly: *"the game needs a bunch of verbose debug info that can help
you find what you're looking for, with version and timestamp labels."*

Two hooks. A banner at startup:

    [lb] LEGO Battles port | built Sep  1 2026 11:54:56 | adaptive_width=448 | enhanced=1

`__DATE__`/`__TIME__` are the BUILD's, so the stamp identifies the binary rather
than the run — which is the thing that was ambiguous. Several
`screenshots/exp-*/stderr.log` files sit side by side here and nothing in them
said which build produced which, and at least one conclusion in this project was
drawn from a log written before the change it was supposed to be testing.

And `LB_VERBOSE=1`, once a second, the geometry that every remaining display
question is really about:

    [lb_geom] t=1025ms window=896x384 guest=448x192 field=0,0 896x384 scale=2.000

`field` is the rect the guest surface is drawn into, and it is also exactly what
goes to `lb_overlay_set_field_rect` for the touch mapping. So one line answers
both "does the view fill the window" and "where will a click land":
`field=0,0 896x384` in an `896x384` window is a complete fill, and a single
`scale` for both axes is proof nothing is being stretched.

## The route works now, and it was established by driving, not guessing

Six route runs were spent changing one guessed coordinate per boot, each costing
a full boot and route. `tools/lb_drive.py` drives a session that is already up —
keys or touches, a capture after every press — and settled the whole route in
one session.

    python tools/lb_drive.py TOUCH:127,75 TOUCH:205,169 A DOWN --shot out/

Three things that cost runs to learn:

* **These menus need two taps.** The first selects, the second confirms. The
  profile screen needs the slot ACTIVELY selected before "Use" does anything,
  and the old route tapped four pixels above the "Tester" bar — so the screen
  looked selected (it is highlighted by default) and the button stayed inert.
* **Touch does work.** That was in real doubt after the profile-name keyboard
  ignored it completely. Tapping the "Empty" slot and watching the highlight
  move proved the mapping was right and only the coordinates were wrong.
* **The name keyboard is NOT touch driven.** The highlight stays on Q however
  precisely the key rectangles are hit. Rather than fight it, every run now
  seeds `saves/lego_battles.sav` — an existing cartridge save — so the route
  never creates a profile, and gets a fully unlocked profile to test with.

The verified route, all measured:

    title           TOUCH 128, 96
    profile "Tester"   TOUCH 127, 75      select
    profile Use     TOUCH 205,169
    Single Player   TOUCH  89, 52
    Free Play       TOUCH 171, 91      twice: select, then enter
    Continue x4     TOUCH 205,169      map, rules, army, opponent

## WITHDRAWN — the cull result below was measured on an unreliable rig

The section that follows concluded that widening the entity-cull immediate
changes nothing. **That conclusion is withdrawn.** It is not replaced by the
opposite claim; it is simply not supported by the measurements that were taken.

Two faults in the rig were found afterwards, and both were invisible at the
time:

* **Two emulator instances were running at once.** The executable is deployed
  as `LegoBattles.exe`, and the kill between runs was
  `taskkill /F /IM nds_runner.exe` — which does not match it. So instances
  accumulated, both bound the debug port in turn, and commands could reach one
  process while `lb_capture.ps1` photographed the other. A capture taken to
  confirm "we are in a battle" showed the TITLE SCREEN while polygon counts
  were being read as gameplay.
* **The same two polygons — z 4267520 and z 4718080 — were later read at the
  title screen**, at identical coordinates to the ones recorded here as an
  in-battle reading. Whatever those two are, they are not evidence about
  culling.

`tools/dbg.py` now has `assert_single_runner()`, called by `lb_probe.py` and
`lb_drive.py`, which refuses to run while more than one instance is alive and
prints the PIDs and the kill command that actually works. The same guard would
have caught another emulator process squatting the shared default port
earlier (docs/192), which is the second time this exact class of
fault has cost measurements.

**What still stands** from this document: the verbose logging, the driving tool,
and the route — those were verified by looking at the screen, not by reading
counters. What needs re-running under the guard: the cull A/B, and the polygon
counts docs/197 draws its culling conclusion from.

The original text follows, unedited.

## Negative result: the entity-cull immediate changes nothing

docs/197 located `func_02038F28`, which builds the entity gather rectangle as
`(camx, camy) .. (camx + 0x100, camy + 0xC0)` and hands it to `func_020882BC`.
The width is one ARM immediate at `0x02038FD0`, stock `E2822C01`
(`add r2, r2, #0x100`), so it can be tested by writing 448 over it —
`E2822D07`, since 448 is `0x07 ROR 26`.

First reading looked decisive: with the camera parked so the base sat just off
the right edge, the polygon count went from **0 to 2** the moment the patch
landed, one of them outside the native band.

**It was not the patch.** The A/B/A/B control says so:

    width=256 -> 2 polygons, 1 outside the native band
    width=448 -> 2 polygons, 1 outside the native band
    width=256 -> 2 polygons, 1 outside the native band
    width=448 -> 2 polygons, 1 outside the native band

Identical in all four states. Something else changed between the first two
samples — the scene is live and units move. The instruction really is part of
that rectangle, but the rectangle is not what limits what gets DRAWN in the
widened columns.

The likely reason is visible in the caller: `func_0203942C` runs `func_0203D538`,
`func_0203D3D8` and `func_0203D488` in sequence, three separate draw passes, and
only the middle one was traced. The two polygons that do survive sit at z
4267520 and z 4718080 — the HUD and health-bar depths from docs/195 — which is
consistent with `func_0203D3D8` drawing overlays rather than the units
themselves. **The unit draw has not been found yet, and the cull is not one
constant.**

The control almost did not run: the guard refused to write the stock word back,
so the "revert" silently did nothing and the second reading would have been
reported as a before. It now accepts any `add r2, r2, #imm`.

## A caution about measuring right after the route

`verify2` reached gameplay and reported 15.65 fps, against 40-43 everywhere
else. The size captures taken moments later show the TITLE SCREEN: the match had
already ended, so that number describes a transition, not a battle. `in_gameplay`
was true when the measurement started and false by the time it finished. Any
figure taken immediately after the route needs a state check at BOTH ends, not
just the start.
