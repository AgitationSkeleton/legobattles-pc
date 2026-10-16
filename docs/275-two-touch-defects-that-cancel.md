# 275 — Two touch defects that partly cancel, and the control that separated them

docs/274 ended with a question it could not answer: the touch mapping subtracts
the centred band while the compositor draws at the clamped one, but every probe
that tried to demonstrate it came back circular — the harness aimed through a
band and the port mapped back through the same band, so a wrong band satisfied
both halves. It closed by naming the control that would settle it: press at a
chosen pixel and measure where the game **draws** the box, because the box is
drawn by the cartridge from the coordinate it received and is therefore outside
the round trip.

That control now exists (`lb_exp.py --touchtruth`), and it found not one defect
but two. They have opposite signs over part of the screen, which is why three
rounds of probing kept producing results that half-agreed.

## The control

`--touchtruth` presses at a client pixel **it chooses**, drags, and measures the
rectangle in the capture. No band arithmetic enters the aiming or the judging.
Three things had to be made honest before the numbers meant anything:

* **The chrome is measured, not assumed.** `capture()` photographs the outer
  window rect (`GetWindowRect`, because that is the bitmap `PrintWindow` wants)
  while `frontend_input` takes client pixels. The offset between them was being
  inferred from the capture size minus the `[lb_geom]` window size and then
  split in half on the assumption the borders match. `lb_capture.ps1 -Info` now
  reports `GetClientRect` and `ClientToScreen`, and the answer here was (12, 34)
  — not symmetric, and not something to guess when the quantity under test is a
  pixel offset.

* **A stale `[lb_geom]` is refused.** The probe suite resizes the window, and an
  earlier measurement compared a 1712-wide window against a `[lb_geom]` line
  describing a 3412-wide one. That produced what looked like a 500 px error in
  the renderer and was an error in the ruler. The probe now compares the live
  client size with the last logged one and refuses to measure if they differ.

* **The box is found by SHAPE.** A frame-diff bounding box is not evidence about
  a rectangle: units walk, water animates, the HUD ticks, and the box that comes
  back is the union of all of it. A marquee is a one-pixel outline, so long thin
  runs find it and find nothing else. (The earlier ad-hoc bounding box happened
  to agree here, but only by luck.)

And one confound had to be removed. The first run scrolled the camera during two
of its three drags, which makes the x column unreadable — a box anchored in the
world moves when the camera does. The scroll was not incidental: see defect 2.

## Defect 1 — touch subtracts the centred band, the picture is drawn at the clamped one

`frontend.cpp` had:

    const float band = (float)(gw - kScreenWidth) / 2.0f;

while the compositor draws the guest's 256 columns at `lb_band_left()`, which is
clamped by the map edges and equals the centred split only when the camera is
far from both. Measured on a 1024-wide surface with the camera near the left
edge: `[lb_band] left=132`, centred split 384. Every click arrived 252 px from
where it was aimed.

`band` now comes from `lb_band_left((int)gw)` and `bandy` from
`lb_band_top((int)gh)`, in the **owning** apply_hooks stanza (0i, the last one
that touches that line — 0i anchors on the line the earlier stanza emits, so the
change has to go in what 0i re-emits or the hook fails).

`lb_band_left()` returns the latch, `g_band_left`, while the compositor composites
with the promoted `g_band_render`. They agree at rest and can differ by a frame
while the camera moves. That is a smaller error than the one being fixed and is
noted here rather than papered over.

**Verified.** `[lb_map]` now reports `band=132.0` and `band=108.0` — the render
band — where it reported `384.0` before. An in-band press lands exactly:

    press client (208,172)  ext=100  ANCHOR client (208,172)  MISS (+0,+0)

The comparable in-band press before the fix missed by −252. The out-of-band
presses in the same run miss by −256 and −512 — exact multiples of 256, with no
band error left in them, which is defect 2 on its own and is the cleanest
statement of it yet:

| press | ext | TRUE err | BYTE err |
|-------|-----|----------|----------|
| 208 (in band) | 100 | 0 | 0 |
| 464 | 356 | −256 | **0** |
| 720 | 612 | −512 | **0** |

The fourth press of that run scrolled the camera and the probe refused to score
it, which is the guard working rather than a gap in the result.

## Defect 2 — the drawn box truncates x to a byte

With the camera held still, the probe stated two predictions **before** looking,
one of which had to be wrong:

* TRUE — a correct port draws the extended coordinate at the band the picture
  uses: `left + ext`.
* BYTE — the cartridge builds the rectangle from single-byte fields, so the
  extended x arrives modulo 256: `left + (ext % 256)`.

Camera static, `renderx=132` throughout:

| ext  | TRUE predicts | BYTE predicts | observed anchor |
|------|---------------|---------------|-----------------|
| −180 | −48 (err **+256**) | 208 (err 0) | 208 |
| +100 | 232 (err 0)   | 232 (err 0)   | 232 |
| +360 | 492 (err **−256**) | 236 (err 0) | 236 |

Three for three on BYTE, with TRUE wrong by exactly ±256 in both cases where the
two differ. The middle row is in-band, where the two predictions coincide and
nothing is being distinguished — it is there so a mapping that is right in the
centre and wrong at the edges cannot pass by luck.

**Why this hid for so long.** With the centred band subtracted, a press at client
`p` in [128, 384) produces `ext = p − 384`, whose low byte is `p − 128`, drawn at
`132 + p − 128 = p + 4`. The two defects cancel to four pixels over exactly the
strip where a casual probe aims. Outside it they add.

### Where the truncation is

The port already moved seven byte reads of the recogniser's position onto the
16-bit copies it publishes at +0x0E (current) and +0x10 (press) — docs/234 and
the `kTouchPatches` table. Every one of those sites is in the 020AFxxx overlay,
and all twenty patches apply cleanly in these runs (`[lb_touch]` confirms it).

`func_0203b7fc` is not in that overlay and was never covered. It loads the same
recogniser object from `*0x02157AF4` and reads the position as bytes:

    0203B894  E5D08006  ldrb r8, [r0, #6]    press x     <- unpatched
    0203B898  E5D04007  ldrb r4, [r0, #7]    press y     (y is 0..191; a byte holds it)
    0203B8CC  E5D10008  ldrb r0, [r1, #8]    current x   <- unpatched
    0203B8D4  E5D16009  ldrb r6, [r1, #9]    current y
    0203B8DC  E20050FF  and  r5, r0, #0xff   <- and truncates it again

It then subtracts a camera and clamps to 0..255 / 0..191 with "clipped" flags,
which is the shape of the on-screen positioning of the box. The addresses sit in
the always-resident static region (0x02000000..0x02150880), so unlike the
overlay sites there is no readiness question.

This is the located cause, not yet the proven one: that these are the only
unpatched byte reads of those fields, and that the measured error is exactly
modulo 256, is strong — but the confirmation is to patch them and watch the box
move, which is the next experiment.

### …but patching those four instructions is NOT the fix

The same function then clamps x into 0..255 and y into 0..191 and sets "clipped"
flags. The cartridge draws its marquee inside the DS screen it believes it has,
and the compositor places that 256-column screen at the band — so the box moves
with the band, which is exactly what the measurements show, and it cannot
escape those columns however correct the coordinates fed to it are.

The captures say the same thing without reading any code. Every box in the
verification run anchored at client 208 with its right edge at 340, inside the
band's 108..363, for presses at 208, 464 **and** 720. Widening the clamp is not
available either: the constants are immediates, but the guest's drawing surface
really is 256 columns wide.

So "the selection box should expand across the whole window" cannot be bought
with an instruction patch. It needs the port to draw the rectangle itself, from
the extended coordinates it already has, the way it already draws the corrected
minimap view box — and to keep the cartridge's clamped one from showing through.
Fixing the four byte reads is still worth doing for everything *else* that reads
those fields, but it will not put the box under the cursor out of band.

That is the next piece of work, and it is a feature rather than a patch: it
needs the extended **y** as well (the port publishes only x today), and it needs
to draw only when the cartridge is actually running a drag-select gesture, or it
will paint a rectangle over drags that mean something else.

### A hazard found on the way

The recogniser object is allocated in **three** places, and only one was widened
when the port gave the press x its own halfword at +0x10:

    020A87A8  2010491F  mov r0,#0x10   -> patched to 0x18
    0203B8A4  E3A00010  mov r0,#0x10   -> NOT patched
    0203B8E8  E3A00010  mov r0,#0x10   -> NOT patched

When the object comes from either of the latter two, the port's
`bus_write_u16(obj + 0x10, ...)` writes two bytes past a sixteen-byte
allocation. Nothing has been observed to break, which is the worst way for a
heap overwrite to behave. Both sites need the same widening.

### And it explains the runaway camera

The cartridge's edge test reads the touch x through the same byte, so an
extended x of 288 arrives as 32 — hard against the left edge — and the map
scrolls. In the first run the presses whose low byte landed near an edge
scrolled and the one whose low byte landed mid-screen did not, three for three.
The probe now chooses offsets whose low byte is mid-screen so the map cannot
move under the measurement; when it moves anyway, the probe says so instead of
printing a number.

## What the player gets from defect 1 alone

Selection, which is the visible symptom: the selection box could not be
reliably clicked and dragged, and clicks outside the original aspect did not
land. `--bandselect` once the probes aim through the same band the port maps
back through:

|            | aiming with the centred split | aiming with the render band |
|------------|-------------------------------|-----------------------------|
| in band    | 0 of 4                        | 2 of 4                      |
| left band  | 2 of 4                        | 4 of 4                      |
| right band | 0 of 4                        | 4 of 4                      |

Clicks outside the original aspect select, 8 of 8. **In band at 2 of 4 is worse
than either widened column and is not explained.** It is recorded here rather
than left out of the table; the widened columns were the question and the middle
is now the anomaly.

It took three runs to get an honest version of that table, and the two failures
are the same fault twice. The first fell back to the centred split without
saying so, because `--bandselect` never armed `LB_BANDLOG`; the second still
read 384 because `--bandselect` kept its own second copy of the centred split
that the first pass had not matched. Both printed four confident counts that
meant nothing. `render_band()` now says out loud when it falls back — a silent
fallback is exactly how `port_camx()` lost a whole experiment, and its docstring
already says so.

## Retraction

docs/274 says the failing in-band control proved the clamped band wrong for
touch. It proved no such thing: that control aimed through the port's own
reported `sx`, which is a native screen coordinate, so it passed whenever the
two halves agreed and failed whenever they disagreed, whichever was correct.
The band the touch must use is the one the picture is drawn at, and it is
`lb_band_left()`. The earlier reverted change was right, and was reverted on the
strength of a probe that could not see the question.
