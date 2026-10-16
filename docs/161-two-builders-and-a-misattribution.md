# Two builders, and docs/119's constants belong to the wrong one

## The correction

`docs/119` located the field-of-view constants -- `cmp sb, #0xa` at `0203A114`
and the `0x40` row stride -- and `docs/158` proved patching them changes the
picture. Both are true. But they drive the **wrong layer**.

Watching writes into the **terrain** buffer `021523A0` names its writers:

```
pc=020A34C2  x780     pc=020A34B2  x770
pc=020A34C8  x780     pc=020A34B6  x770
                      pc=020A34BA  x770
                      pc=020A34BE  x770
```

All **Thumb**, all in `020A34B2..020A34C8` -- the six-halfword 3x2 metatile
stamp `docs/118` found *alongside* the ARM one. The ARM builder at `0203A47C`,
whose loop carries `#0xa`, writes the buffer at `02152A20`, which `docs/118`
matched to **block 0, BG2**.

So there are **two builders**, one per layer, and `docs/119` read the ARM one.

## Which explains the striping

`docs/158` patched the ARM bound and got grey stone stripes across 78.6% of the
field -- while `docs/156` measured BG2 as contributing only **1.3%** when
disabled. Those look contradictory and are not.

BG2's tiles are mostly **transparent** in normal play, so switching the layer
off changes almost nothing. Corrupting its map replaces those transparent tiles
with opaque ones, which then cover the terrain. A layer can be nearly invisible
and still ruin the picture when its data is wrong -- "contributes 1.3%" and
"can damage 78.6%" are both true of the same layer.

That reconciles every measurement taken so far, and it means `docs/158`'s
striping screenshot was **BG2 being corrupted**, not the terrain narrowing.

## Where this leaves the widescreen work

Still standing:

* `docs/157`'s display path -- `LB_BG_EXTEND=1`, terrain in block 2, BG3 off --
  renders the field at 448 with terrain to both edges;
* `docs/158`'s finding that guest code in RAM is patchable, which is what makes
  any of this possible;
* the camera-shift double-run, **validated this iteration**: jumping the camera
  +256 and letting the streamer rebuild leaves block 1 holding genuinely
  different terrain, only 256 of 1024 entries shared with the pre-jump map.

Newly known and needed:

* the terrain builder is the **Thumb** stamp at `020A34B2`, and its loop bound
  lives in a caller not yet identified -- the stamp itself is a leaf ending at
  `020A34E0`;
* `func_02039FB4`, which reads the camera and looked like the streamer, takes
  its destination from the literal `02152A20` and is therefore **BG2's**
  builder. Its single caller is `020393F8`, which passes the view in `r5` and
  sets the dirty byte `[r5+0x67]` afterwards;
* it has **zero dispatching call sites**, so it cannot be overridden -- though
  it can still be *called*, which is what the double-run needs.

## The honest state of the double-run plan

The mechanism is validated but not yet implementable from the vblank hook,
because the hook has no robust way to obtain the **view object pointer**. It was
found empirically at `0224B42C` (`docs/112`) and the same address has held all
session, but a hardcoded heap address is not something to ship.

Restoring by re-running is also ruled out: after jumping the camera and jumping
back, block 1 did **not** return to its previous contents, because the rebuild
is incremental. A real implementation must save and restore the 2 KB directly.
