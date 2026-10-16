# 354 — The widened field's pixel loop, specialised

Written for: whoever works on the compositor next.

Status: **built, verified, deployed** 2026-09-24 (`4266a2e5`, runner built
23:44, `build/LegoBattles.exe` sha256 `ec215198...`, settings kept).

docs/353 found that at 3440x1440 the widened composite costs ~36 ns a pixel
on each of its ten threads, ~36 ms of CPU a frame, and that the per-pixel
loop, not the decode, is most of it: 10.6% of the main thread against 4.9%
for the decode (the last 60 s of the reference replay, 85,677 samples). This
is the first of the levers that doc ranked.

## 1. Why the general loop is slow

It is correct and fully general -- every layer, OBJ, the HD surfaces, the
host-terrain base, the HUD anchor -- and two things in it cost more than the
work they do:

- **Every captured value is reloaded on every pixel.** The row body is a `[&]`
  lambda, so `output_width`, `hud_bg_count`, `mbmode` and `mb` live behind
  the closure. A store through `dst` (a `uint32_t*`) may legally alias any
  of them (`int` and `unsigned` alias), so the compiler reloads each one,
  through two dependent loads, after every pixel it writes. The disassembly
  shows `mov 0x10(%r13),%rax; mov (%rax),%r15d; cmp %r12d,%r15d` closing the
  pixel loop, and the same shape for the others.
- **Every opaque layer is pushed through an insertion** into top/below, a
  `Pixel` built and copied for each, even when two opaque layers in front of
  it have already decided the pixel.

## 2. The specialised loop

`lb_fast_row` (src/lb_wide2d.inc) takes every input as a plain value and
computes the same pixel. The argument that it is the same:

- The general loop keeps, of everything it pushes, the two pixels with the
  smallest (priority, order), the base pixel filling in below them. Every
  key is distinct -- BG1..BG3 are orders 2..4, the 3D layer order 1, the base
  priority 4 order 5 -- so walking the layers in (priority, order) order and
  stopping at the second opaque one yields the same top and below.
- top and below carry exactly what the general loop's `Pixel`s would:
  colour, BLDCNT target bit, 3D alpha, order. `compose()`'s logic then
  finishes the pixel, inlined, with the same `blend()`, `blend5()`,
  `brighten()`/`darken()` and `to_rgb32()`. Inlined, it is short: every
  pixel this path sees has `Pixel::alpha` 0 (BG and 3D pixels always do, and
  so does the backdrop), so `compose()`'s semi-transparent-OBJ branch never
  applies, and what is left reads four registers fixed for the row. The HUD
  cover, `hud_x` (both ways it can be set) and the layer map are reproduced
  line for line.

A first version kept `Pixel`s and called `compose()` per pixel. It was
identical and only ~8-10% faster by band time. The per-pixel call, with both
structs passed through memory, was most of what was left.

It is taken only when nothing it leaves out is in play: OBJ off (it is, in
the field), the HD surfaces not being written (the field is screen 1), and
no host-terrain base. Otherwise the general loop runs, unchanged. The stanza
keeps the general loop as the `else` branch.

`LB_FASTROW=0` turns it off. `LB_FASTROW=check` runs both on every widened
row and counts every pixel that differs. That is the proof below, and it is
the guard for the day the general loop gains a feature this one lacks.

## 3. Verified

**Every row, both ways.** `LB_FASTROW=check` on the reference replay composites
each widened row with both loops, the fast one into a scratch row, and
compares every pixel:

| window | surface | rows compared | pixels that differ | fast ns/px | general ns/px |
|---|---|---:|---:|---:|---:|
| 1280x720 | 640x360 | 3,400,000 | 0 | 18.04 | 23.31 |
| 3440x1440 | 1536x642 | 4,800,000 | 0 | 19.04 | 25.49 |
| 1080x1350 | 540x674 | 5,800,000 | 0 | 23.00 | 29.99 |
| 1473x830 | 736x414 | 3,400,000 | 0 | 17.51 | 22.85 |

The two loops run back to back on the same thread for the same row, so the
times compare like with like whatever else the machine was doing (it was
busy: fps swung 30-56 between runs tonight). They cover the pixel loop only,
not the decode, and the general loop runs second on warm data, which favours
it. **The pixel loop is 23-25% faster at every size.** At 3440x1440 that is
about 6 ms of CPU a frame across the ten row threads.

**The layer map it writes itself** (check mode leaves that to the general
loop): with the fast path on, `LB_LAYERDUMP=1285` at 1280x720 against the
pre-change build is identical in composite and layer map at every dump
inside the replay (1285-5140). 6425 and 7710 are past its end (docs/352 s4).
`lb_thinholes.py`: 0.

**The exactness gate:** identical at all six frames (host-only change).

## 4. What it does not do

The decode that feeds the loop is untouched: `decode_text_line`, three layers
across the full width, with an out-of-line table call per 8-px tile. With the
band's other per-row work, it is roughly a third of each band: about 11 of
its ~30 ns a pixel at 3440x1440, derived as band time less loop time. The
row workers still start late under load (docs/353 s3). Both are next.
