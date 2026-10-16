# The engines were inverted: the field is engine A, and docs/109 was right

## The register that settles it

    POWCNT1 = 020F   bit 15 = 0

POWCNT1 bit 15 routes engine A to the **top** screen when set and to the
**bottom** when clear. It is clear, so **engine A drives the bottom screen**,
which is the game field.

The debug server's `framebuffer` command takes an argument spelled `engine`, but
it indexes `g_fb[front][screen & 1]` -- a **screen** index, not an engine. So
capturing `engine="A"` returns the physical **top** screen regardless of which
engine drew it. That is how `docs/111` and `docs/113` came to state the mapping
backwards, and both are corrected here:

| | drawn by | contents |
| --- | --- | --- |
| screen 0, physical TOP | **engine B** | unit portrait, minimap, gold, population |
| screen 1, physical BOTTOM | **engine A** | the game field |

Independently confirmed: engine A's `DISPCNT = 00012F08` has bit 3 set, which
selects 3D for BG0, and only engine A has 3D at all. The field is where the
units are, and the units are the eight GX quads `docs/107` counted. Engine B
cannot have produced them.

## What this reinstates

`docs/113` claimed the field's layers are all 256x256 and concluded that a wider
view has no tilemap to live in. That was measured on engine B -- the top screen.
The field's real configuration is:

```
engine A  DISPCNT=00012F08   BG0 is 3D
    BG0  BGCNT=0002  256x256  (3D layer, screenbase unused)
    BG1  BGCNT=4187  512x256  screenbase 1  (06000800)  charbase 1  prio 3
    BG2  BGCNT=4081  512x256  screenbase 0  (06000000)  charbase 0  prio 1
    BG3  BGCNT=0284  256x256  screenbase 2  (06001000)  charbase 1  prio 0
```

**BG1 and BG2 -- the field's terrain layers -- are 512x256.** Sixty-four tile
columns each, of which the streamer keeps thirty-two current.

So `docs/109`'s original conclusion stands and is reinstated in full:

* the tilemap is already wide enough for roughly double the view;
* the extra columns live in the **second screen block**;
* that block is **never written**, so it holds stale data, which is exactly the
  repeating garbage `lb_wide2d.inc` saw when it decoded past column 31;
* widening therefore requires **filling block 1**, and nothing else about the
  BG configuration has to change.

`docs/113`'s proposed work -- enlarge the layers to 512x256 and find VRAM for a
second screen block -- is unnecessary. Both are already true. Its VRAM analysis
was also wrong: engine A's BG banks are VRAM_E and VRAM_F, not the engine B
banks it examined.

## Correction record

* `docs/111`: the table naming engine A as the top screen is wrong. Its finding
  that the D-pad scrolls the view, measured in pixels, is unaffected -- that was
  measured on screen 1, the field, whichever engine draws it.
* `docs/113`: the central claim is inverted and the conclusion drawn from it does
  not hold. Superseded by this document.
* `docs/109`: its 512-wide analysis was on the correct screen after all. The
  banner added to it during this session is withdrawn.
* `docs/112`: unaffected. The camera was found by watching an address and
  correlating it against pixel motion on screen 1; no engine assumption entered.

## The lesson, which is the same one twice

`docs/113` closed by saying "read the configuration before measuring the
consequences of it". The configuration was read -- and then attributed to the
wrong engine, because an argument named `engine` was believed instead of
checked. Reading a register is not the same as knowing what it describes. The
check that settles which engine is which is POWCNT1 bit 15, and it costs one
read.
