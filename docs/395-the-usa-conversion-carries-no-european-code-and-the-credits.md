# 395 — The USA conversion carries no European code, and the credits

Status: **built, verified, deployed with the next release.** This doc covers
three changes:

- `tools/lb_region_delta.py` and `port/runner/src/lb_setup.cpp`: the second
  conversion format, `LBDELTA2`;
- `publish/legobattles-pc/THIRD_PARTY_NOTICES.md` and `LICENSES/`, copied
  into every release by `tools/lb_dist.py`;
- `tools/lb_publish_pc.py`: the export to the public repository.

## Why the first conversion was the wrong shape

The port runs the European cartridge (`C5SP`). Its lifted code is registered
for that image's SHA-1. docs/394 accepted a USA dump (`C5SE`) by turning it
into the European image. The two releases share every game file. They differ
only in the first `0x126000` bytes: the header, the ARM9 static image, the 91
ARM9 overlays, the overlay table and the FAT entries that place the overlays.

`LBDELTA1` stored the European bytes of every run that differs. That was
894 KB, nearly all of it the European ARM9 in compressed form. It worked, but
the file was a copy of the European program, which a USA owner does not own.

## What the two images actually differ by

Unpacked with `tools/nds_extract.py`, the European ARM9 is 1,379,352 bytes.
bsdiff against the USA ARM9 copies **1,378,941 of them from the USA image**.
45,196 of those bytes are adjusted, about 29,000 words. These are address
relocations: the European build moved code by a few hundred bytes. Only **411
bytes** have no USA counterpart. The 91 overlays behave the same way: same
sizes, relocated addresses.

So the European code can be rebuilt from the player's own USA code if the
compression can be redone exactly.

## Reproducing the packed images

The ARM9 and 78 of the overlays are BLZ-compressed (backwards LZ, the DS SDK
format). Their packed bytes must match exactly, because the SHA-1 covers them.

- **The ARM9:** CUE's `BLZ_Code` in normal mode, with the ARM9 rule (the
  module's first `0x4000` bytes left raw), reproduces the cartridge's 826,984
  packed bytes. The one exception is the footer. The cartridge's `enc_len`
  counts the footer itself (`pak_tmp + hdr_len`); CUE's does not.
- **The overlays:** no CUE mode reproduces them. Normal mode matched 4 of 78
  once the footer was corrected, and "best" mode matched none. They were
  packed by a different encoder.

An encoder is not needed. The cartridge's own LZ parse can be replayed: the
flag bytes and the two-byte back-reference tokens, in decoding order, with the
**literal bytes stripped out**. Given the unpacked module, a literal is just
the next byte of the reversed image, so the parse plus the unpacked bytes
rebuild the packed module exactly:

| | packed | parse kept |
| --- | ---: | ---: |
| ARM9 | 826,984 | 429,981 (380,610 literals stripped) |
| 78 packed overlays | 72,012 | 22,010 |

## LBDELTA2

The layout is documented field by field at the top of
`tools/lb_region_delta.py`. In order:

1. The 92 modules (the ARM9, then the overlays in id order). Each records its
   USA and European offsets and lengths, its European footer, and its literal
   stripped parse.
2. bsdiff's control triples over the **unpacked** modules end to end, the
   per-byte additions stored sparse, and the 411 inserted bytes.
3. The European image's 0xFF fill over `[ARM9 offset, ARM7 offset)`.
4. 16 byte runs, 3,646 bytes in all. These are:
   - the header fields that differ, and its checksums;
   - 128 bytes at `0xF80`;
   - the overlay table at `0xCE000` (2,912 bytes);
   - the FAT entries from `0x11E201` that place the overlays, which sit at
     shifted offsets.

The generator proves the file before writing it. It runs the same
conversion in Python on the USA dump and checks the European SHA-1.

The file is 801 KB. Of that:

- 452 KB is LZ parse (flags and tokens);
- 57 KB is address additions;
- 411 bytes are inserted code;
- 3.6 KB are header and table runs.

It is larger than the arithmetic minimum because the ARM9's parse is stored
rather than recomputed. Recomputing would save about 430 KB but put an encoder
on the critical path. The parse is exact by construction.

`lb_setup.cpp` does the same steps in C++:

1. `blz_unpack` (a port of `nds_extract.blz_decompress`) unpacks the USA
   modules;
2. bsdiff's operations rebuild the European modules;
3. `blz_repack` replays each stored parse;
4. the runs are written;
5. the result is checked against the European SHA-1 before it is kept.

Every read of the file is bounds-checked through one cursor. A damaged file
fails with a message, not a crash.

**Verified:** a fresh `lb_dist.py` folder with only the USA dump in `rom\`
converted it on first start to SHA-1 `da60469bd143eb8f47ecf0d7951181b4783c91b4`
and booted. The log reads `[load] cartridge: 134217728 bytes, SHA-1 da60469b…`.
The original was renamed `.unused`, as before.

The file is still made from two dumps and is never committed (`*.lbdelta` in
`.gitignore`). It goes into releases only.

## Credits and licences

The port is built on **ndsrecomp** (© 2026 Matthew Stanley). The runner vendors
**melonDS**'s 3D engine, rasterisers and Wi-Fi model (GPL-3.0-or-later), so
`LegoBattles.exe` is a combined work under GPL-3.0-or-later. ndsrecomp as a
whole is under PolyForm Noncommercial 1.0.0, with its runner sources under
MIT.

- `publish/legobattles-pc/THIRD_PARTY_NOTICES.md` names every component with
  what it is used for: ndsrecomp, melonDS, FreeBIOS (the built-in BIOS),
  libslirp, xxHash, glad and `khrplatform.h`, Dear ImGui, SDL2, the GCC
  runtime and winpthreads. `LICENSES/` holds each licence text.
- `tools/lb_dist.py` copies both into every release folder. It also writes
  `LegoBattles-PC-<version>-runner-source.zip`, the assembled `port/runner/tree`
  the executable was compiled from: upstream's `cmake/`, `recompiler/`,
  `runner/` and `vendor/` with this port's hooks and sources applied. That is
  the runner's corresponding source. The lifted title code is made from the
  player's cartridge and is not included.
- `.github/workflows/release.yml` attaches both zips to the release.

## The public export

`tools/lb_publish_pc.py` no longer copies `docs/`. The public repository's
documentation is a curated selection, edited for publication. A new document
goes in only when it is named with `--add-doc`. The export also refuses to
publish a file with personal material: local paths, machine names, quoted
feedback, private planning documents or process records. `LEAKS` in the
script is the list it checks.

The build scripts no longer name local paths:

- `port/runner/build.sh`, `port/recomp/build.sh` and `port/lift.sh` find
  ndsrecomp and Dear ImGui through `NDSRECOMP`, `IMGUI` or `LB_TOOLS`, else
  a `tools/` folder three levels above the checkout;
- the Python tools read `LB_ROM_EU`, `LB_ROM_US`, `LB_MODS_DIR` and
  `LB_DSD_DIR`, else `roms/` and `sources/` two levels above;
- the release workflow passes `LB_TOOLS` from a repository variable.
