# It boots

**Status: `nds_runner.exe` exists — 66.7 MB, zero undefined references — and it
boots LEGO Battles, registers the recompiled title banks, initialises the GPU
and executes 13.7 million instructions of lifted code before hanging in an early
spin loop.** That is a bring-up milestone, not a playable game, and the
distinction matters.

## What the boot log says

```
[load] arm9 bios: 4096 bytes, SHA-1 ok
[load] arm7 bios: 16384 bytes, SHA-1 ok
[load] cartridge: 134217728 bytes, SHA-1 da60469bd143eb8f47ecf0d7951181b4783c91b4
[dispatch] registered configured title banks for da60469bd143eb8f47ecf0d7951181b4783c91b4
[boot] direct boot (opt-in): cartridge entry state established
[gpu3d] OpenGL 4.3.0 NVIDIA / GeForce RTX 3060 Ti
[gpu3d] renderer: OpenGL 4.3 compute
```

**`registered configured title banks`** is the line that matters: the 9,867
functions this project lifted are live and dispatching, gated on the exact
cartridge hash.

## Four link failures, each invisible until the one before was fixed

| # | undefined | cause |
| ---: | ---: | --- |
| 1 | many (truncated) | **overlay bodies globbed out** — the runner globs `*_[0-9][0-9].c`, so unsharded banks contributed only their dispatch object |
| 2 | 14 | **firmware banks absent** — the firmware-boot path needs banks recompiled from `firmware.bin` |
| 3 | 1 | **`nds_texture_upscale_set_factor`** — an upstream defect |
| 4 | **0** | — |

Failure 1 is the instructive one. `--shards 1` does not fix it; one shard still
emits the bare name. **Two is the minimum** that produces `_00.c`. And a CMake
glob matching nothing is not an error — it is an empty list, so the emit step and
the compile step both reported success.

Failure 3 is a bug in ndsrecomp rather than in this configuration: `frontend.cpp`
and `main.cpp` call `nds_texture_upscale_set_factor` unconditionally, but its
definition in `src/melonds_compute/TextureUpscale.cpp` compiles only when
`NDS_ENABLE_COMPUTE_RENDERER=ON`. So the renderer-off configuration does not
link. Turning it on is not a workaround here — it is the configuration the port
wants, and SDL2 2.32.10 is present.

## The firmware, and why generated is correct here

The runner refused to start on a firmware SHA-1 mismatch. That is not a fault:
**a firmware dump carries per-console user settings**, so two dumps of the same
revision differ. `--generated-firmware` synthesises one and the runner says so
plainly — *"GENERATED image (opt-in, no dump)"*. Combined with `--boot direct`,
which establishes cartridge entry state without executing firmware, the firmware
is not on the path a title needs.

## Where it stops

```
#13733678 branch pc=0x02111C30 addr=0x02111C14 sp=0x027E3750 lr=0x00000008
[coverage] 6 code pages, 24576 bytes
```

The spin is inside **`func_02111B64`**, 256 bytes of ARM in the `0x0211xxxx`
runtime region — the same neighbourhood as `__aeabi_idivmod` (`docs/78`). It
initialises a 32-entry table, writes `[r0, #0x388]`, then loops on a halfword it
never sees change.

**Six code pages of coverage means it never leaves early boot.** `lr=0x00000008`
is an exception-vector value, which fits a wait that is expecting an interrupt.

The likely shape is a **dual-CPU synchronisation wait**: the ARM9 spinning for
something the ARM7 should produce. That is the standard next problem for a DS
recomp, and ndsrecomp's own README describes per-title bring-up as exactly this
kind of work.

## What is honestly true

* **Built and runnable**: the executable exists, starts, and accepts the flags
  the port needs — `--adaptive-widescreen bottom`, `--screen-layout`,
  `--config game.toml`, `--player-name`, `--local-wireless`.
* **The lift is live**: title banks registered against the exact ROM hash.
* **Not playable**: it hangs before rendering a frame of the game.
* **Not verified beyond boot**: 6 code pages is a fraction of a percent of the
  1.38 MB image.
