# 376 — The cartridge hears only its own screen

Status: **fixed.** The defect: unit and building sounds did not play, and
construction sounds were inconsistent.

The ROM facts below were checked by disassembly, and the runtime facts were
measured here.

## Why entity sounds are silent

* **Every entity sound passes one audibility test first.** That test is
  `func_0208938C` (Thumb; r0 is the sound manager `0x02155464`, r1 the
  entity). Its callers are:
  * the entity's own sequence archive (`func_02089270`): selection and
    command acknowledgements, a newly produced unit, construction, death,
    harvesting and attacks;
  * spells (`func_02089230`);
  * collectibles (`func_020890FC`).

  Interface sounds (archives 0/1) have no position test, and the music is a
  stream. That is the observed split exactly.
* **The window is the DS's own screen plus about two cells.**
  * It turns the guest camera into a map cell (24x16 px).
  * It caches a window `cx-2..cx+13` by `cy-2..cy+14` in the manager
    (`+0x18/+0x19` to `+0x20/+0x21`, valid byte `+0x2C`), clamped to the
    map. The bytes are at 02089424..02089456.
  * An entity whose cell (`+0x128/+0x129`) is outside is silent.
* **The port never widened it**, as it did the gather rect and the effect
  test (docs/350).
  * Measured at 736x414 with `LB_SNDLOG=1`: the cached window was cells
    8..23 by 7..23 against a picture of cells 0..31 by 2..27. **67% of the
    picture was silent** for unit and building sounds.
  * The same arithmetic gives 82% at 974x600 and 89% at 1536x616.
  * Construction is "inconsistent" because it is heard only while the
    builder stands inside the guest's own 256x192.

## The fix

`apply_snd_cull` (lb_patches.cpp):

* **What it writes.** The six Thumb immediates grow each margin by that
  side's band, rounded up to whole cells. The cartridge's two cells stay
  outside the band.
* **Rebuild.** It clears the cached window's valid byte when the margins
  change.
* **Safety.** All six or none, and only over the stock halfword or one of
  its own.
* **Off.** Vanilla, `LB_WIDEMAP=0` and `LB_SNDCULL=0` restore the stock
  halfwords. So does a recording made before this: demos gain `snd=1`,
  because more sounds starting is more guest work per pass, and the pass
  length is what the game's pacing reads (docs/351).

Measured with the widened window:

* `LB_SNDLOG`: cells 0..33 by 0..30 against the same picture, **0% silent**.

## Instrument: LB_SPULOG

apply_hooks stanza 0zzzzzzz counts SPU key-ons (SOUNDxCNT bit 31, 0 to 1)
per channel and prints them every ten seconds of mixed audio.

* **The music stream** keys only channel pairs 6/7 and 8/9, two a window.
* **Any other key-on is an effect.** Selecting and ordering the hero in the
  native columns keyed channels 2 and 4 (3 and 1 in one window), so effects
  play on this build.

## Verification

* **A/B:** the base in the left band (native x about -80, cells outside the
  stock window) is selected and then three builders are queued, all inside
  the first minute, before any combat (a one-off drive script).
  `LB_SNDSTART=1` (below) lists every sound the cartridge started:

  | | stock window (`LB_SNDCULL=0`) | widened |
  |---|---|---|
  | interface sounds (archives 0/1) | 5 | 5 |
  | entity sounds | 2 | 4 |

  * **The two extra** are from the band: archive 102 index 0, the base's
    selection sound, and archive 44 index 0, the new builder's voice.
  * **The two common ones** started before the pan, while the camera was on
    the base.
  * **Key-ons agree:** channel 4 reads 0 stock against 2 widened.
  * **A key-on count alone could not separate the two**, because the five
    interface clicks swamp it. That is why the start log exists.
* **`LB_SNDSTART=1`** reads the cartridge's own "started this pass" array
  (`0x02155878`, 217 archives by 20 indices; `func_02089AAC` sets a byte, and
  task 0x21 clears the array once a pass). It prints each newly set byte with
  its class: archives 0/1 are the interface, 182..215 spells, 216
  collectibles, and the rest entities.
* **Gate** (exp-gate377, which also carries docs/377):
  * Counts are identical to gate370b, and the eight pictures under 5972 to
    gate375.
  * The gate demo has no `snd=` key, so the window stayed stock for the
    whole replay.
  * The log shows it engaging only after `[lb_demo] finished at frame 8188`.
    From then on it follows each size of the harness's sweep: cells
    -4..+15 at 306x216, up to -18..+29 at 996x546.
* **Vanilla** (exp-van377): 240/240 identical.

## Not the cause (the other candidates, now measured)

* **The profile's effect volume:** `0x0214B41C+0x10` reads 127.
* **The manager's state:** reads 2 (effects start above 1).
* **The ARM7:** lifted and 0% interpreted in the gate runs.
