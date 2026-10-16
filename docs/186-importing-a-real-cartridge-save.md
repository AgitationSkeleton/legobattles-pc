# 186 — Importing a real cartridge save, and a data-loss hazard

Status: **working end to end.** A save pulled from a real DS cartridge now loads
in the port, and the workflow around it will not destroy the original.

## The save did not load, and the reason is one constant

A 512-byte `LEGO Battles.sav` from a real cartridge produced two **Empty**
profile slots. The save was fine; the port rejected it.

The tag is `XXX_BATTLE_SAVES`, and the prefix is not derived from the console's
region at runtime — it is a compile-time constant. `func_0204b1d4` is eleven
instructions and writes it as literal immediates:

```asm
0204B1DA  movs r0, #0x41      'A'   -> [0]
0204B1E6  movs r0, #0x46      'F'   -> [1]
0204B1E0  movs r0, #0x45      'E'   -> [2]
0204B1EA  adds r1, r2, #3
0204B1EE  ldr  r0, =021486BC        '_BATTLE_SAVES'
0204B1F0  movs r2, #0xd             13 bytes
```

(The interleaved `movs r1, #0x3f` / `strb` pairs write `'?'` to each slot first
and are immediately overwritten — scratch, not part of the tag.)

So this ROM expects **`AFE`**. The cartridge save carried **`AFN`**, from a build
whose constant differs. Rewriting those three bytes and changing nothing else
makes the same file load as profile "Max".

**That also proves the record checksum does not cover the tag.** Each record
carries its own checksum at +0x000 (docs/181), and both survived the edit
untouched — `d3e826c3` for record 0, `f8180082` for record 1. A translation is
therefore lossless and reversible, not a forgery.

The save also cross-checks docs/181 nicely: record 0 has **192 of 192 unlock bits
set**, which is what a completed profile should look like, against 9 in the
barely-started second slot.

## The hazard, which is the more important half

**The runner rewrites its save file even when it could not read it.** The first
import attempt pointed it at a copy; the game rejected the save, started fresh,
and wrote an empty save over that copy. Had the copy been the original, the
cartridge save would have been gone.

So the workflow is built to make that impossible rather than to remember not to
do it:

* `tools/lb_save.py` opens the source **read-only**, never writes to it, and
  refuses an existing destination without `--force` (backing it up first).
* `play.bat` keeps saves in `saves/` beside the script and never accepts a path
  outside it.

    python tools/lb_save.py inspect <file.sav>
    python tools/lb_save.py import <cartridge.sav> saves/lego_battles.sav

Verified: after a full import-and-run cycle the source file's MD5 is unchanged.

## Running it

`play.bat` finds the runner and config, creates `saves/`, and takes the ROM from
`LB_ROM` or from a `.nds` in `rom/`. The cartridge is never committed, so the
path has to come from the person running it:

    set LB_ROM=C:\path\to\LEGO Battles.nds
    play.bat
