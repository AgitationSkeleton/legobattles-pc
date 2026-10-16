# 357 — The widened gather scans the whole map

Written for: whoever works on emulation cost or game pacing at wide windows
next.

Status: **measured; an observer added, nothing changed in the game.**

docs/353 found `FUN_0207E9B4` the hottest guest function at 3440x1440. It is
a tile-rectangle entity gather: x from rect+0x08 to +0x10, y from +0x09 to
+0x11 (inclusive bytes, rect in R2). Per tile it looks up the entities in
three grid slots, de-duplicates them with a flag at entity+0x15A and appends
them to a list, so its cost is the rectangle's area. The host stack cannot
say which guest function calls it (lifted calls tail-dispatch), so
`LB_GATHERLOG=1` adds an observer on 0x0207E9B4. It reads the rectangle and
LR, tallies per VBlank, and then runs the cartridge's function unchanged.

## 1. What it scans

The reference replay, per VBlank, twelve windows of 300 VBlanks each:

| window | tiles scanned per VBlank | from the render gather (LR 02088308) | rectangle often |
|---|---:|---:|---|
| 3440x1440 (1536x642) | 5,700 - 8,900 | 5,580 - 8,770 | x 0..63, y 0..56: the whole map |
| 1280x720 (640x360) | 2,600 - 4,250 | 2,490 - 4,040 | ~30x40 tiles |

Nearly all of it is one caller: 02088308, inside `020882BC`, the render
gather that docs/353 s5 traced (the grid through 0207E9B4, then the
projectile vector). It runs about 2.5 times a VBlank. Its rectangle is the
one `apply_cull_width` widens to the surface, so that units and buildings
draw in the bands, and at 3440x1440 the surface covers the map's full width.
So the rectangle is the whole map. The other callers (0207AF28, 0207ED10,
0207F038) scan 50-200 tiles a VBlank at either size.

The exactness gate is identical with the observer installed.

## 2. What it costs, and where

- **Host time.** Most of the scanned tiles are empty and fail the gather's
  first mask test in a few instructions. At ~20 guest instructions a tile,
  ~8,500 tiles is ~170k guest instructions a VBlank at the widest window
  against ~60k at 1280x720. That is an estimate, not a measurement. In host
  time it is a share of emulation's 11-13 ms, which inclusive profile shares
  understate (docs/353 s4).
- **Guest time, which matters more.** Those instructions run on the emulated
  67 MHz ARM9 and count against its frame budget (~1.1M cycles a VBlank).
  docs/351 found that at the widest surfaces more game passes overran two
  VBlanks ("more of the map is gathered and drawn"). Every three-VBlank
  pass is a slower game step, at any host fps. So at a wide window the game
  runs slightly slower, because it is doing the widened gather's work.

**Measured in guest instructions** (`LB_CULLAB=1`: the widened rectangle and
the cartridge's own in turn, 300 VBlanks each, first 10 after a switch left
out, ARM9 instructions retired per VBlank):

| window | widened over stock, per battle period | median |
|---|---|---:|
| 3440x1440 | +5.0, +13.9, +9.0, +4.8, +23.2, +45.2% | ~11% |
| 1280x720 | +3.7, +8.5, +5.3, +1.0, +9.3, +22.2% | ~7% |

(~270k-345k instructions a VBlank in the battle. The two periods before the
battle are identical at both sizes, as they should be: the gather is not
running yet.) Adjacent periods are different moments of the battle, so the
spread is partly content. The direction and the growth with width are
consistent, and the heaviest period costs most. The figure includes drawing
the extra units the wider rectangle finds, not only the scan. In stock
periods the bands draw no units; this is a measurement mode.

## 3. What could be done, not done here

- **A host implementation of the gather** returning the identical list
  (same order: rows, then columns, then the three slots, de-duplicated at
  first encounter) and the same flag side effects, and charging the guest
  what the unwidened rectangle would have cost. That would bring both the
  host time and the game's pass timing back to native levels at every width.
  It changes the exactness gate by design. So it needs its own proof: a
  check mode that runs both and compares the lists, on every call. And it
  must hold for multiplayer: the render gather only feeds drawing, and each
  peer already widens its own.
  **The obstacle, found on the way:** a host implementation cannot call the
  cartridge's per-tile function (`0207EB78`) and resume, because lifted guest
  calls tail-dispatch and never return to host code (see the memory "LB
  lifted body never returns"). So a host gather means reimplementing the
  per-tile lookup too: `0207E3F4`, `0207D790`, `0207D6BC`, `0207D678`,
  `02087350`, `02087634`, `020EF244`. And a check mode cannot run both and
  compare synchronously. Patching a faster empty-tile test into the guest
  loop instead would run that function in the Tier-3 interpreter, slower on
  the host for fewer guest instructions. None of this is small; whether game
  pacing at the widest window is worth it is an open decision.
- **Narrowing the render gather to what is on the surface.** It already is:
  at 3440x1440 the surface spans the map. Nothing to gain there.

Recorded here so the next pass on emulation cost starts from the measured
number, not from the profile's inclusive share.
