# The field camera

## Address

    object   0224B42C
    camera X 0224B48A   (object + 0x5E, signed 16-bit)
    camera Y 0224B48C   (object + 0x60, signed 16-bit)

These are the offsets `func_020396E0` loads with `ldrsh` to push a layer scroll,
and the same offsets `func_0203B7FC` subtracts from a world position at
`[r2+0x0A]` / `[r2+0x0C]` -- a world-to-screen transform, which is what makes
the pair a camera origin rather than an arbitrary coordinate.

There is **more than one object with this layout**. An earlier probe caught
`0224B448` at the same offsets and it holds unrelated values (549, 13584);
writing it does nothing. The live one must be identified by watching, not
assumed. `0224B42C` is the one read while the field is scrolling.

## Units: 1:1 with pixels, and it clamps

Holding Left, sampling the camera against the measured pixel shift of the
engine B framebuffer:

| after | camera X | measured dx |
| --- | --- | --- |
| baseline | 73 | 0 |
| +4M cycles | 41 | **-32** |
| +8M cycles | 0 | -68 |
| +12M cycles | 0 | (no further x movement) |
| +16M cycles | 0 | (no further x movement) |

The first step is a decrease of exactly 32 against a measured `dx` of exactly
-32, so the camera is in **pixels, 1:1**, no scaling. Camera Y held at 144
throughout, as it must while only Left is held.

It then **stops at 0 and stays there** while the input is still held. The game
already clamps the camera at the map edge; the "don't scroll into the black
void" constraint is enforced by the guest, not something a port has to add.

## Writing it works, but the redraw is separate

Writing the pair takes -- the value reads back and persists -- but **the view
does not move on its own**, because the field is drawn by rewriting the tilemap
and the streamer only runs when something asks it to. Setting the dirty bytes at
`[obj+0x66]` / `[obj+0x67]` is not enough either; those belong to the layer
scroll path, which is not what draws the field.

What does work is writing the camera and then letting the streamer run:

| test | camera after | measured shift |
| --- | --- | --- |
| write X+96, set both dirty bytes | (96, 144) | (0, 0) -- no redraw |
| write X+96, then a brief Right tap | (98, 144) | **(+86, +12)** |

96 written, 2 added by the tap, 98 read back, and the picture moved by ~86 at a
74% correlation match (the sweep steps by 2 and the scene is not a pure
translation once units animate, so a few pixels of slack are expected). The
write is effective; only the redraw needed provoking.

## How to move the view from the port

Two options, and the difference matters for lockstep:

* **Synthesise input.** Entirely safe -- it is what a player does, it goes
  through the game's own clamping and its own streamer, and it stays
  deterministic across clients.
* **Write the camera directly**, then provoke a redraw. Also safe for lockstep,
  because `docs/85` found no Sim->User calls: the camera is presentation. But it
  bypasses the guest's clamp, so a port doing this must not exceed the bounds
  the guest would have enforced.

## What this unblocks

The widescreen thread needed the view to move before anything about scrolling
could be measured. It can now be moved deliberately and measured in pixels, so
the next questions are answerable:

1. how many tile columns the streamer refreshes per step, now that steps can be
   made one at a time and on demand;
2. what the clamp bound is at each edge -- that bound is the field of view, and
   with the camera writable it can be found by bisection instead of guessed;
3. whether a wider view can be filled by driving the camera and capturing more
   columns than the hardware viewport shows.
