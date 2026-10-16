# Widescreen menus, verified

## It works

Interactive launch, `adaptive_widescreen = "bottom"` and
`adaptive_width = 448` in `port/game.toml`, captured through the debug server:

```
fix_s0.png   448x192   margin=96   left-margin black:   0/4608   filled
fix_s1.png   448x192   margin=96   left-margin black:   5/4608   filled
```

Looking at them:

* **the LEGO Battles title screen** renders at 448 wide with its sky and
  starfield continuing across the full width, the logo centred, characters at
  their own positions;
* **"Touch the Screen to Start"** renders with its blue gradient extending to
  both edges and the text centred and undistorted.

No black bars, no visible stretch artefacts on the centred content. This is the
first verified widescreen result in this thread.

## The bug that hid it, and a caution about this session's captures

`docs/126` reported the buffer as "still 256 wide" and concluded the config key
had not taken effect. **That was a measurement error, not a fact about the
runner.**

The debug server's `framebuffer` reply is:

```
{"w": 448, "h": 192, "rgb": "..."}
```

The field is **`w`**, not `width`. Every capture helper in this session read
`r.get("width", 256)` and silently fell back to 256, so a 448x192 image was
reshaped as 256x336 -- which is where that odd height came from, and it was
visible in the output the whole time: `448 * 192 == 86016 == 256 * 336`.

`adaptive_width = 448` had been working from the moment it was added.

**Which earlier results this does and does not affect.** Every field capture in
this session was taken in headless `--serve` mode, where `output_width` really
is 256, so `w == 256`, the fallback happened to be correct, and those images and
their conclusions stand. Only the two interactive captures were misshaped, and
`docs/126` had already recorded them as inconclusive rather than drawing a
conclusion from them. Nothing needs retracting beyond `docs/126`'s own claim.

## The pattern, for the third time

This is the same failure shape as the constant scroll registers and the
saturated watch ring: **an instrument reporting something other than what it was
read as saying.** A default argument is exactly as dangerous as a saturated
buffer, and quieter -- `r.get("width", 256)` cannot fail, it just answers the
wrong question.

The check that would have caught all three is the same one: when a measurement
disagrees with a plausible model, verify the measurement before revising the
model. Here the arithmetic was sitting in the output.

## Status

* **Menus at 448 wide: working and verified.**
* `lb_menu_fill` is built and wired, but these menus are being widened by
  `lb_wide2d` itself -- the log shows `engine 1: WIDENING` -- so the fill path
  is not what produced these images and remains unexercised. It is the fallback
  for scenes the widener refuses, and a scene that reaches it has not yet been
  identified.
* The **game field** is still not widened: it is engine A with 3D, handled by
  the upstream path, and `docs/117` and `docs/120` explain why its tilemap
  cannot currently be extended.
