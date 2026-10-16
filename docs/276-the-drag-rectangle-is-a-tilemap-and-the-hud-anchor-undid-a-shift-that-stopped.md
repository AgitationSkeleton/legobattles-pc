# 276 — The drag rectangle is a tilemap, and the HUD anchor undid a shift that had stopped

Two defects: the drag rectangle, and the in-gameplay HUD (which appears while a
unit or building is selected) not constraining to the edges and corners. They
turned out to share a cause with docs/275 — the centred split
used where the render band belongs — and to need very different fixes.

## The HUD anchored nothing at any ordinary play size

`lb_hud_scan` converts a HUD quad's surface x back to the guest's own
coordinates so it can ask "which edge is this against". It did that with the
centred half-overhang, `(output_width - 256) / 2`, while its caller passes the
real render band and says why in its own comment:

> Given the SAME band the rows are about to composite with: the in-field HUD is
> 3D geometry, so it was shifted by the band when it was transformed, and the
> anchor that undoes that has to use the same number or it mis-locates every
> quad by the difference.

The line discarded it. The comment above the line argued for that at length —
horizontal 3D geometry is not band-biased, only the Y path carries the band —
and measured it on a 946x516 surface. **That measurement was right when it was
taken.** docs/268 then gave `SubmitVertex` the matching horizontal term, adding
`lb_band_left(RenderWidth) - centre` to every vertex, and horizontal geometry
has been band-biased ever since. The anchor kept undoing a shift the renderer
had stopped applying.

Measured with a unit selected while the window grew:

| surface  | real band | the line used | quads anchored | rejected offscreen |
|----------|-----------|---------------|----------------|--------------------|
| 772x550  | 258       | 258           | 9              | 0                  |
| 996x718  | 370       | 370           | 9              | 0                  |
| 1252x670 | 380       | 498           | 0              | 9                  |
| 1572x768 | 398       | 658           | 0              | 9                  |
| 1728x768 | 476       | 736           | 0              | 9                  |

The two that worked are the two where the camera happened to be far enough from
a map edge for the clamped band to equal the centred split — the same
coincidence the touch mapping had been living on. At 1728x768 the nine quads
that anchor correctly at 996 report native x −256..−193, fail `nx0 >= 0`, and
are dropped before the classifier ever sees them. So at every size worth
playing at, **nothing** was anchored and the whole HUD sat where the 256-px
screen put it.

After: 9 of 9 at every size, 0 rejected.

Two supporting changes. The offscreen rejects now log their surface and native
coordinates and which of the four bounds failed — the counter alone said
`offscreen=9` and left the rest to be inferred, which is what made this cost a
build to find rather than a glance. And the early-out now asks the surface's
size rather than testing `extra`: those were the same question while `extra` was
half the overhang, but the real band is legitimately 0 with the camera hard
against a map edge, which would have disabled anchoring exactly there.

## The drag rectangle is 8x8 tiles on a 32x24 grid

docs/275 measured the box's drawn anchor tracking `extended x mod 256` exactly
and left the mechanism open. `func_0203b7fc` is the mechanism, and it is not
geometry:

1. it clears a buffer to tile `0x1f`;
2. it reads the recogniser's press and current positions, subtracts a camera,
   and **clamps x into 0..255 and y into 0..191** with "clipped" flags;
3. it shifts both corners right by three — pixels to 8x8 tiles;
4. it walks the edges writing halfword tile indices `0x0d 0x0e 0x1d 0x1e 0x2d
   0x2e` through `func_0203d8e8`, a nine-slice border.

So the box is a **tilemap on the guest's own 32x24 screen grid**. It cannot
reach the widened columns for the same reason the DS cannot: there are only 32
cells across. That closes the option docs/275 left open — patching the four
byte-truncating loads would feed it better numbers and change nothing, because
the clamp and the grid are downstream of them.

The port therefore draws the box itself, beside the debug aspect box, straight
into the finished surface where there is no band to get wrong. `--touchtruth`,
which presses at a chosen pixel and measures where the box lands:

    press client (208,172)   ANCHOR client (208,172)   MISS (+0,+0)

and out of band the port's box anchors at the press with its right edge on the
drag's end (584 for a press at 464, 840 for 720, drag +120) — where before the
whole box sat at 208 whatever was pressed.

Two things make the timing honest rather than a reimplementation of the gesture
rules. `func_0203b7fc` runs once per frame and only while the box is up — its
first four instructions test a flag and branch elsewhere when set — so reading
that same flag makes the port's box appear on exactly the frames the
cartridge's would. And the corners are published from the frontend in surface
coordinates, before the HUD unmapper touches them, so the box is drawn where the
cursor is.

### Suppressing the guest's box: the hook had to go on the transfer

Two attempts failed before the right one. Both cleared the staging tilemap
after `func_0203b7fc` returned — first at `0x021530A0` for the `0x680` bytes its
own head clears, then through the row-pointer table the tile writes use, which
resolves to that same buffer (measured live: `row0=021530A0`, `row1=021530E0`,
a `0x40` stride for 32 halfword cells). Clearing all 832 cells changed nothing,
on the guarded path and on both paths.

The function's own tail says why:

    0203be34  ldr r0, =0x021530a0     the staging buffer
    0203be40  mov r1, #0x680          size
    0203be48  bl  func_0210eaf4       flush
    0203be7c  ldr r2, =0x021530a0     source
    0203be84  mov r3, #0x680          size
    0203be8c  bl  func_0204e0e0       queue the VRAM transfer

It draws, flushes and **sends**, all before an exit hook can run. Clearing the
buffer afterwards tidied memory the hardware had already copied out of.

An OAM dump taken mid-drag ruled out the other hypothesis on the way: all 128
sprites sit parked at y=192, off the bottom of the screen, so the box is not
sprites and the table at `0x02152160` is not OAM entries. That guess is
retracted.

So the hook goes on the transfer, gated on exactly this source and length —
`func_0204e0e0` is generic and everything else that uses it is passed through
untouched — and the buffer is blanked immediately before it is handed over. The
blank value is sampled rather than guessed: the map is 32x26 while the box is
clamped into the screen's 32x24, so the last row can never hold any part of it
and its final cell is always whatever "empty" is. (`mov r0,#0x1f` at the
function's head could land as `0x001F` or `0x1F1F` depending on whether the
helper fills bytes or halfwords, and picking wrong paints 832 cells of garbage.)

Result — one box, at the press, at every position:

    press client (208,172)   ANCHOR (208,172)   MISS (+0,+0)   v-edges 2
    press client (464,172)   ANCHOR (464,172)   MISS (+0,+0)   v-edges 2
    press client (720,172)   ANCHOR (720,172)   MISS (+0,+0)   v-edges 2

`v-edges 2` is the check that matters: a single rectangle has two vertical
edges, and the doubled state reported four. Confirmed on the captures too.

`LB_MARQUEE_KEEPGUEST=1` leaves the cartridge's box in place for A/B.

### Why the function is not simply skipped

It does considerably more than draw a rectangle — past the tile loops it
converts the corners back to pixels, runs a three-state machine, indexes a
table at `0x02152160`, and ends by flushing and transferring. Refusing to call
it would take all of that with it. Blanking one transfer's source is the
smallest intervention that works.

The colour is measured, not chosen: captured while both boxes were on screen,
the cartridge's is yellow, so the port's is drawn yellow rather than white.
