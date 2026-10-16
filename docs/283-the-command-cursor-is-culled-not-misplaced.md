# 283 — The command cursor is CULLED, not misplaced

The defect: the touch UI's contextual cursors (the harvest icon on trees and crystals, the move, patrol and attack arrows) were missing in the widened view.

The contextual command icons are all spawned by one function,
`func_020A8CCC(point, element_id, target_handle, flag)`, with a literal element
id per command:

| id | command |
|---|---|
| `0xAE` | Attack |
| `0xAF`, `0xB0` | Move |
| **`0xB1`** | **Harvest** -- the tree/crystal icon |
| `0xB2` | Patrol |
| `0xB4` | Assist / Repair |
| `0xB5` | Garrison |
| `0xB6` | Rally point |
| `0xC1` | the fall-back marker |

## Why they "break" in the widened view

Not because they land in the wrong place. **Because they are not drawn at all.**
`func_020A8CCC` tests the icon's screen position against the native 256x192
screen and jumps to `020A8D24` -- which finishes the LERP and ENDS the icon --
whenever it is outside:

```
020A8D0C  DB0A  blt 020A8D24    x < -24   -> abort
020A8D18  DB04  blt 020A8D24    y < -16   -> abort
020A8D1E  DC01  bgt 020A8D24    x > 256   -> abort     (0x1E + 0xE2 = 0x100)
020A8D22  DD04  ble 020A8D2E    y <= 192  -> draw
```

So a harvest icon on a crystal out in the band is spawned and immediately
terminated. That is a cull, not a clamp: there is no wrong-place version of it
to see.

## The second defect: the cursor's own byte truncation

The point reaches the spawner through `func_020AA814` (and its twin
`func_020AA874`), both of which mask the click to a byte first:

```
020AA82E  0609  lsls r1,r1,#24
020AA830  0612  lsls r2,r2,#24
020AA832  0E09  lsrs r1,r1,#24     r1 = x & 0xFF
020AA834  0E12  lsrs r2,r2,#24     r2 = y & 0xFF
```

`lsl #24 / lsr #24` is the same `& 0xFF` docs/233 removed at
`020E4994`/`020E4998`, and takes the same fix: shift by 16 and return
ARITHMETICALLY, so a negative or past-256 coordinate survives instead of
wrapping modulo 256.

## What was ALREADY fixed, and a retraction

A research pass reported four unpatched byte reads feeding the recogniser
(`020AF1DA`, `020AF22E`, `020AF32E`, `020AF3B8`) as defect number one. **That is
wrong.** All four are already in `kOrderFix` with exactly the proposed
`0x7A28 -> 0x89E8` rewrite, and that table defaults to every group with
`enhanced` set. Recorded because the rest of that report was accurate and
verified: the remaining twelve halfwords were checked byte-for-byte against
`section_static_02000000.bin` before anything was written, and all twelve
matched.

## The fix, and what it costs

Both defects go into `kOrderFix` as new groups, so they inherit its
stock-verification and its all-or-nothing application -- one wrong halfword and
the whole group is declined rather than half-applied.

* **group 16** -- the eight truncation halfwords.
* **group 32** -- the cull: three aborts to `46C0` (nop) and the final
  conditional to `E004` (unconditional). The bounds are built from 8-bit Thumb
  immediates (`subs r1,#0x34`, `adds r1,#0xE2`, `cmp r5,#0xC0`), so they cannot
  be widened to a band that changes with the window; removing the test is the
  available move.

The default mask goes 15 -> 63.

**What removing the cull costs**, since it is a real behaviour change: the abort
arm was also how an icon ENDED when it scrolled off the native screen. Without
it an icon ends only when its own LERP finishes (alpha 31 -> 0 over 800, set at
`020A8DA8`), which is the normal path anyway.

## Verified, and NOT verified

**Verified:** all 25 sites apply on the reference settings --
`[lb_orderfix] groups=63, 25 site(s) moved off the byte path`, with each of the
twelve new ones logged by address. The combat route completes, 268 s, 55.4 fps,
0 dispatch misses.

**NOT verified:** that a command cursor now APPEARS out in the band. That needs
a capture with an order issued past native column 256, and the replay route is
desynced from the recorded live run (the camera-bounds change moved where a
recorded touch lands), so it cannot be relied on to issue one. The patches
applying is evidence the mechanism is in place, not evidence of the picture.

## Separate from the placement ghost

Confirmed separate: no construct handler calls `func_020A8CCC`, and the
placement subsystem has its own state, its own action ids, and its own
positioning function (`func_0203CA10`) with no cull at all. docs/281's footprint
work and this are two paths, not one.
