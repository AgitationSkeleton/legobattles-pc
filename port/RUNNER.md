# Building the runner

The lift produces C; the **runner** is what turns it into an executable. It
supplies the memory bus, the two-CPU scheduler, the hardware models and optional
SDL presentation. `port/lift.sh` handles the lift; this is the rest.

```
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DNDS_TITLE_BANK_DIR=<abs>/port/generated \
  -DNDS_TITLE_ROM_SHA1=da60469bd143eb8f47ecf0d7951181b4783c91b4 \
  -S <ndsrecomp>/runner -B <build>
cmake --build <build> --target nds_runner -j 6
```

`NDS_TITLE_ROM_SHA1` is not decoration — the runner refuses to register a title
bank directory that is not gated by an exact ROM hash.

## Three requirements discovered by being refused

**1. Bank names must contain `_arm9_` or `_arm7_`.** The runner infers a bank's
CPU from its filename and aborts otherwise:

```
Cannot infer title-bank CPU from source name: .../lb_ov000_dispatch.c
Include _arm9_ or _arm7_ in every bank name.
```

The overlays were `lb_ov000`. They are ARM9 code — they load into ARM9 space at
`0x0216A2E0` — so they are now `lb_arm9_ov000`, and `port/lift.sh` names them
that way.

**2. The BIOS banks are not generated for you.** `nds_banks` requires four
banks — `arm9_bios`, `arm7_bios`, `freebios_arm9`, `freebios_arm7` — and the
runner's CMake does not build them. They come from running `nds_recompile`
against the TOMLs in `<ndsrecomp>/bios/`:

```
arm9_bios      167 functions      from bios/biosnds9.rom
arm7_bios      637 functions      from bios/biosnds7.rom
freebios_arm9   70 functions      from third_party/freebios/drastic_bios_arm9.bin
freebios_arm7   72 functions      from third_party/freebios/drastic_bios_arm7.bin
```

**3. FreeBIOS ships vendored as C arrays, not as images.** The
`third_party/freebios` submodule is empty in this checkout, but
`vendor/freebios/freebios_images.cpp` embeds both images. Extracting them gives
byte-exact files:

```
drastic_bios_arm9.bin   4096 bytes  b85d6afdbf65d87c9ab11d4e7fb5ecfd6192ccf8  VERIFIED
drastic_bios_arm7.bin  16384 bytes  eaab9b21610978f05a0eb14a9c8ff345698f56a2  VERIFIED
```

Both hashes match the values `tools/gen_freebios_embed.py` records, so the
extraction is checked rather than assumed. FreeBIOS is BSD-2-Clause and
redistributable; the real BIOS dumps are not.

## The link failure, and why it was invisible

The first full link failed with undefined references like
`lb_arm9_ov090_tfunc_0216A4FC`, and only five distinct symbols were reported —
`ld` truncates with "more undefined references follow", so the visible tail was
not the shape of the problem.

The archive told the truth:

```
ar t libnds_banks.a | grep -c "ov[0-9]"     ->  91
ar t libnds_banks.a | grep "ov089|ov090"    ->  ..._dispatch.c.obj  (only)
```

**91 overlay members, every one a dispatch object and not a single body.** The
runner globs bank bodies as

```cmake
file(GLOB _group_bodies "${_group_dir}/*_[0-9][0-9].c")
```

An unsharded bank emits `lb_arm9_ov000.c` with no numeric suffix, so CMake
**silently globs it out**. The dispatch object still links, and it references
every function in a bank whose bodies were never compiled.

The ARM9 was sharded 16 ways and matched the pattern, which is why it linked
clean and hid the problem for every other bank.

`--shards 1` does **not** fix it — one shard still emits the bare name. Two is
the minimum that produces `_00.c`/`_01.c`, so `port/lift.sh` now shards every
bank with a floor of 2.

**The lesson is about silence.** A glob that matches nothing is not an error in
CMake; it is an empty list. Both the emit step and the compile step reported
success, and the defect only surfaced 11 minutes into a link.

## On the BIOS dumps

`biosnds9.rom` and `biosnds7.rom` verify against the hashes in
`<ndsrecomp>/bios/README.md`:

```
biosnds9.rom  bfaac75f101c135e32e2aaf541de6b1be4c8c62d  MATCHES
biosnds7.rom  24f67bdea115a2c847c8813a262502ee1607b7df  MATCHES
firmware.bin  87dae2500e889737af51f4a5b5845770a62482f5  DIFFERS from ae22de59fb...
```

The firmware differing is expected — a firmware image carries per-console user
settings, so two dumps of the same revision hash differently. It matters only
for the firmware-boot path, not for running a title.

**These dumps are staged outside the repository** (`tools/ndsrecomp/bios/`) and
are never committed. `.gitignore` enforces it, but the physical separation is the real guarantee.
