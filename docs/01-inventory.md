# Inventory of everything available to this project

Taken 2026-08-28. Every path below was opened and read, not assumed.

## The cartridge images

`<the ROM folder>`

| region | game code | SHA-1 | bytes |
| --- | --- | --- | ---: |
| Europe (En,Fr,De,Es,It,Da) | `C5SP` | `da60469bd143eb8f47ecf0d7951181b4783c91b4` | 134,217,728 |
| USA (En,Fr,Es) | `C5SE` | `f25d090feb840e9b4b07546b6929d897e3fb3965` | 134,217,728 |

Both are genuine retail dumps at the cartridge's natural 1 Gbit capacity, not
padded overdumps: the header's used-size field is `0x075FBF68` (123.7 MB) in
both, and the tail is real ROM padding rather than a resized image.

The EUR SHA-1 matches the third-party decomp's `lb_eur.sha1` exactly, so that
project's configuration applies to this image without adjustment. Its
`lb_usa.sha1` does **not** describe LEGO Battles — it still carries Phantom
Hourglass's `4c8f52dd719918bbcd46e73a8bae8628139c1b85 ph_usa.nds` from the
template it was forked from. The USA hash above is this project's, measured.

**EUR is the primary target**, because it is the region the existing
configuration and symbol work already describes. USA is extracted and tracked in
parallel so region-specific work can be diffed rather than guessed at.

## Third-party sources

`<the third-party sources folder>`

| item | what it is | value here |
| --- | --- | --- |
| `Lego-Battles-Decomp-master` | a `ds-decomp` + `mwccarm` matching decompilation, EUR | scaffolding is excellent, decompilation is ~0.5% done — see `02-existing-decomp-assessment.md` |
| `LegoBattlesRebalancePatch-main` | a community balance patch: one replacement `BP/Entities.ebp` plus a unit stat sheet | proves the unit table is a located, editable structure, and gives a second sample of the file to diff against |
| `NitroPaint-main` | Nitro graphics tooling (NCGR/NCLR/NCER/NANR/NSCR) | directly applicable — this ROM's graphics are stock Nitro formats |

The rebalance patch's `1432.ebp` is 18,148 bytes against the cartridge's 17,760.
That size difference is itself a finding: the container's size table is rebuilt
on write, so replacements are not constrained to the original length.

## The LEGO Star Wars II project

`LSW2DS-RE` — the same author's previous DS title, taken from
ROM to a running ndsrecomp-based PC port.

- `playbook/` — thirteen documents written *during* that project and addressed
  to this one; the README says so in its first paragraph. Digested in
  `docs/reference/lsw2-playbook-digest.md`.
- `docs/` — 100+ numbered research documents. Mostly LSW2-specific findings, but
  the shape of the documentation system is worth copying and is copied here.
- `tools/` — around 200 scripts. `07-starting-a-new-ds-project.md` names the
  nine that transfer essentially unchanged.
- `port/`, `recomp/` — a working PC port over ndsrecomp, and the generated-bank
  build around it.
- `tools/vendor/` — a machine-local toolchain this project reuses rather than
  re-downloading: Ghidra 12.1.2 (build 2026-06-05), JDK 21, melonDS, DeSmuME,
  GitHub CLI 2.94.0, apicula, FluidSynth.

## DS platform material

`<a local DS SDK and reference folder>`

| item | relevance |
| --- | --- |
| `ndsrecomp-main` | the static recompiler. Lifts guest ARM to C ahead of time; a thin runtime supplies the memory bus, dual-CPU scheduling, hardware models and SDL presentation. Pre-alpha, but it is what LSW2's port runs on |
| `MetroidPrimeHuntersRecomp-main` | a full DS recomp/port to read as a worked example |
| `melonDS-master`, `desmume-master` | emulator sources; melonDS is the oracle, and LSW2 keeps a patched instrumentation build |
| `libnds-master`, `sdk-master` | headers and SDK reference for naming what the ARM9 is calling |
| `dwc_network_server_emulator`, `GameSpy-master`, `UniSpySDK`, `WfcPatcher`, `Wiimmfi-App` | Nintendo WFC / DWC replacement stack, for the online-play scope |
| `MKDS-decomp-headers`, `mkds-re-main`, `mkds-asm` | Mario Kart DS RE material; useful for Nitro format and SDK cross-reference |

The DS BIOS dumps the LSW2 port used came from a local share of DS Wi-Fi
connector material. Not yet needed; noted so it is not rediscovered.

## Tooling confirmed working

| tool | version | notes |
| --- | --- | --- |
| Python | 3.13.3 | the extraction tools in `tools/` are dependency-free |
| Ghidra | 12.1.2 | vendored under the LSW2 tree, with JDK 21 beside it |
| GitHub CLI | 2.94.0 | |
| git | 2.49.0 | |
| CMake / Ninja | 4.0.2 / 1.12.1 | |
| GCC | 15.2.0 | 64-bit; a 32-bit `i686-w64-mingw32` toolchain is required for the recomp and is not yet present |
| melonDS, DeSmuME | vendored | the oracle |

## What is not present yet, and will be needed

- A **32-bit mingw toolchain** (`i686-w64-mingw32`). DS pointers are four bytes;
  the LSW2 playbook is explicit that building 64-bit costs a day.
- **DS BIOS and firmware dumps**, for the faithful ndsrecomp boot path. There is
  an opt-in FreeBIOS path that does not need them.
- **`mwccarm`**, the Metrowerks ARM compiler the matching-decomp track needs.
  The third-party repo's `tools/setup.py` downloads it; it is not vendored here.
