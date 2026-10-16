# 282 — Where the frame budget goes, and why the ARM7 is the suspect

The target: a stable 60 fps minimum while preserving visual and audio fidelity.

Measured on the deployed build at a wide surface (1706x696 client,
pixel_scale 2 -> 852x348 guest surface), deployed settings, a full combat route.

## The budget

```
57.66 fps  (347 frames in 6.02 s)
   emu_ticks         14.200 ms/frame
   present_ticks      2.922
   adaptive_ticks     1.204
   upload_ticks       0.101
   draw_ticks         0.057
   swap_ticks         0.381
   drain_ticks        0.157
                     ------
                     19.022 ms      vs 16.67 ms for 60 fps
```

**2.4 ms must come out**, and `emu_ticks` is 75% of the total. Even if the whole
compositor went to zero the remainder is 14.9 ms, or 67 fps -- so the target is
reachable, but only by touching guest execution, not by tuning presentation
alone.

A candidate was checked and eliminated first: the run emits thousands of
diagnostic lines (2704 `[lb_bares]`, 792 `[lb_row12]`, and a dozen other tags)
with no env var arming them. Over ~16,000 frames that is about 0.17 lines per
frame, which cannot account for milliseconds. Noisy, but not the cost.

## The asymmetry

`dispatch_stats.json` (note: it is a PYTHON REPR, not JSON -- `json.load` rejects
it, use `ast.literal_eval`):

| | ARM9 | ARM7 |
|---|---:|---:|
| `dispatch_total` | 302,674,892 | 18,161,763 |
| `cache_hit_absent` | **1.0%** | **93.4%** |
| `crs_miss` | 0.2% | 33.5% |
| `crs_scan_iters` per lookup | 1.02 | 5.1 |

`cache_hit_absent` is a lookup that resolved to NO STATIC ENTRY, which is the
Tier-3 interpreter path (`runtime_arm.cpp`: "Copied RAM code has no static entry
and is handled by Tier 3"). So the ARM9 is running lifted code essentially
always, and **the ARM7 is running interpreted essentially always**.

The entry-point counts say why:

| program | entry points |
|---|---:|
| `lb_arm9.toml` | 9156 |
| `lb_arm7_wram.toml` | 313 |
| `lb_arm7_mainram.toml` | 153 |
| **`lb_arm7_static.toml`** | **3** |

Three. The ARM7's static section is effectively not lifted at all.

## Why this is the suspect and NOT yet the answer

The ARM7 is the sound CPU and it runs continuously, so a fully interpreted ARM7
is a plausible home for milliseconds. But this is **NOT ESTABLISHED as the
cost**, and the arithmetic does not prove it: ARM7 is only 6% of dispatches, and
`dispatch_total` counts dispatches rather than instructions, so it cannot be
converted into time without measuring.

**The measurement that would settle it:** run the same route with audio disabled
and compare `emu_ticks`. If it falls substantially, the ARM7 is the cost and
lifting it is the lever. If it barely moves, the cost is ARM9-side and the ARM7's
interpretation is cheap because it is idle-looping.

That experiment must run on a quiet machine -- fps readings swing widely under
load, so it cannot be run while other heavy work is running.

## The constraint that shapes any fix here

The target is "while preserving visual and audio fidelity". The ARM7 IS the
audio. Disabling sound is a MEASUREMENT, never a fix, and any change to ARM7
lifting has to be verified against audio output rather than only against frame
time.
