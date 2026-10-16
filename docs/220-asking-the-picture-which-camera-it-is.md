# 220 — Asking the picture which camera it is

**Goal within the project:** adaptive window size — the requirement that the
widened field has to be *right*, not merely present. This closes two defects:
the side views break at times and become inconsistent; and (the first half of)
on the extended view the fog sometimes does not show, or the sides or even the
original-aspect field corrupt.

## The problem with "at times"

Every previous attempt to chase this failed the same way. A defect that is not
on screen when the probe looks is a defect the probe reports as absent, and this
port has already published two conclusions that were wrong for exactly that
reason (docs/214, docs/217). Hunting for an intermittent fault by taking
screenshots is not a method.

There is a continuous measurement available instead, and it falls out of how
`lb_widemap_fill` was already written. The fill computes the terrain table for
**every** cell in its 42×64 window — native columns included — and then serves
only the cells outside the guest's own 32×24 rectangle. So for the cells it does
*not* serve there is an independent answer sitting in VRAM: the guest's own BG1
tilemap, which is the cartridge drawing the same map through the same camera.

Where the two agree, the band is built from a correct model. Where they
disagree, this port's model has drifted from the game's — and the band comes
from that same model, so the band is wrong at the same instant, in the same way,
in the columns nobody can check by eye.

`LB_MAPAUDIT=1` compares all 768 native cells every frame and prints every
disagreement with its camera position. "Sometimes" becomes a number.

## What it found, immediately

A 2400-frame session with 24 pan steps: **116 bad frames**, in two populations.

The interesting one was a hit on *every third frame while panning*, and each hit
was not a subtle edge artefact but 750 of 768 cells — the entire screen:

```
[lb_audit] frame 1623: 760/768 cells DISAGREE, first r0 c0 ours 012 theirs 000, cam 26,18
[lb_audit] frame 1626: 760/768 cells DISAGREE, first r0 c0 ours 000 theirs 1E0, cam 30,18
[lb_audit] frame 1629: 760/768 cells DISAGREE, first r0 c0 ours 1E0 theirs 012, cam 34,18
```

The camera moves four tiles per step, and 4 mod 3 = 1, so a one-step lag shifts
the phase within every metatile by one and changes essentially every tile. The
values cycle with period 3, which is the signature of exactly that.

## The cause

`lb_widemap_fill` runs on vblank and reads the camera from the view struct. By
then the guest's frame logic has already advanced it. The guest's **tilemap** —
the thing the native columns are actually drawn from — is rewritten separately
and lags by one to two frames.

So on the frame the camera crosses a tile, the band was built from the new
camera while the field beside it still showed the old one. The widened columns
jumped a whole pan step ahead of the native picture, once every three frames,
for the whole duration of any pan. That is the side views breaking at times and becoming
inconsistent, and it is not subtle once you know to look for it.

## The fix: measure, don't assume

The port already has the answer in front of it. So:

- `lb_widemap_fill` no longer builds the table. It records the camera reading
  into a four-deep history, newest first, along with the metatile tables.
- `choose_camera`, at render time, tries the newest reading first — right
  whenever the camera is still, which is most of the time — and walks back
  through the history only if it disagrees. Agreement is scored by
  `sample_mismatch`, which compares 48 native cells (every 4th column by every
  4th row) against the guest's own BG1 tilemap.
- `fill_terrain` then builds the table for the camera that was *chosen*, and the
  fog scan uses the same pair, so terrain and fog cannot shear apart.

This cannot drift: it is re-decided every frame against VRAM, so a wrong pick
lasts one frame and only if the map offers two alignments that both match 48
sampled cells — in which case the two bands are indistinguishable anyway.

**Result: pan-time disagreements go to zero.** 3480 frames, 24 pan steps, no
post-boot mismatch at all.

## A wrong turn, recorded

With the pan fixed, 24 cells still disagreed — one per row, all in hardware
column 31, only when panned hard against the right edge, `ours 000 theirs 023`.

The map is 64 metatiles wide (192 tile columns) and the camera reads 161, so
column 31 is absolute column 192: one past the end. This port calls that
off-map and blanks it; the guest draws a real tile.

Two ways to read past the edge were tested by printing both beside the guest's
value — clamp `mx` to the last column, or read the row entry unclamped. **Both
give 021. The guest gives 023.** So the disagreement was never about how to read
past the edge.

The attempted conclusion was that the camera must be clamped to
`columns - 32 = 160`, since a window starting at 161 cannot fit on the map. That
was implemented and **refuted by its own measurement**: clamping shifted the
whole picture by a tile and took the disagreement from 24 cells to 221. The
camera reading of 161 is correct.

What actually explains 023 is that the guest writes its tilemap a whole
**metatile** at a time, three columns per metatile. Starting at absolute column
161, the last metatile it writes covers columns 189..191 — hardware 28..30 — and
hardware column 31 is never written that pass. It keeps what an earlier camera
left there. **The cartridge is showing a stale column at the map edge**, and it
is visible in the surface dump as a faint vertical seam just inside the right
native boundary.

That is vanilla behaviour in a column this port never serves, so it is excluded
from the audit count rather than reproduced. Blanking an off-map cell stays the
honest answer.

## The second population: gating on the measurement

The remaining 73 bad frames are all at boot, before the field exists, where the
map pointer is stale but still looks like main RAM. About 1.2 seconds at the
start of a run — which is exactly the kind of window that produces a report of
the fog not showing, or the sides corrupting, at the start of a run.

Since `choose_camera` has to compute the agreement score anyway, the band is now
gated on it. `g_model_ok` is true only when the chosen camera disagrees on
**zero** of the 48 sampled cells. While it is false, all three suppliers —
terrain, fog and shroud — return blank.

Blank rather than declined: a declined cell falls through to the hardware wrap
and draws whatever the tilemap happens to hold, which is the garbage being
avoided.

No tolerance on the score, deliberately. Every failure mode seen disagrees on
hundreds of cells out of 768, so a tolerance could only let a genuinely wrong
model through on a map uniform enough to hide it — and on such a map the band it
drew would look the same either way.

## The invariant this establishes

**The widened band never contradicts the field.** Either it is built from a
camera validated against the guest's own picture that frame, or it is blank.
That is checked 48 cells per frame in normal running, and all 768 with
`LB_MAPAUDIT=1`.

## The fog edge variants heal, and reach exact

docs/226 left the fog audit at 7 tile mismatches of 768, all of them
neighbourhoods whose mask the edge table had never observed, with the claim that
they fall as ground is explored. Measured over a 40-step pan:

```
[lb_fogaudit] frame 3180: 0 verdict + 2 tile of 768, missing 0BF:1 0C4:1
[lb_fogaudit] frame 3300: EXACT on 768 fogged cells,
                          running 0 verdict + 175 tile over 105984 cells
```

7, then 2, then **exact on every one of 768 fogged cells** — and the cumulative
figure is 175 mismatched tiles in 105,984 cells, 0.17%, all of them in the early
frames before the table had seen those neighbourhoods.

The 24-cell reading that returns at the far right of the map is the cartridge's
own stale column, explained above: the guest writes its tilemap a metatile at a
time and leaves hardware column 31 holding what an earlier camera left there. It
is the guest's column, never served from here.

So the fog clause is closed: the band's fog is exact once the neighbourhoods
have been seen, and conservative before that.
