# The cartridge, as measured

Every number here was produced by `tools/nds_extract.py` reading the ROM. None
of it is assumed or carried over from another project.

Reproduce with:

```powershell
python tools\nds_extract.py "<your EU ROM>"  out\eur
python tools\nds_extract.py "<your USA ROM>" out\usa
```

## Header

| field | EUR | USA |
| --- | --- | --- |
| title | `LEGO BATTLES` | `LEGO BATTLES` |
| game code | `C5SP` | `C5SE` |
| maker | `WR` (Warner Bros.) | `WR` |
| ROM version | 0 | 0 |
| device capacity | `0x0A` → 1 Gbit (128 MB) | same |
| used ROM size | `0x075FBF68` | `0x075FBF68` |
| ARM9 | offset `0x004000`, `0x0C9E68` packed, RAM `0x02000000`, entry `0x02000800` | offset `0x004000`, `0x0C9DBC` packed |
| ARM7 | offset `0x0E7200`, `0x026F28`, RAM `0x02380000`, entry `0x02380000` | identical |
| FNT | `0x10E200`, `0xFEDF` | identical |
| FAT | `0x11E200`, `0x7D30` → 4,006 entries | identical |
| ARM9 overlay table | `0x0CE000`, `0xB60` → **91 overlays** | `0x0CDE00`, `0xB60` |
| ARM7 overlay table | none | none |
| ARM9 autoload hook | `0x02000AAC` | `0x02000AAC` |

The two regions differ only in the ARM9 image and its overlay-table offset. FNT
and FAT are byte-for-byte identically placed and sized, so **the filesystem
layout is region-independent** and file-level work done on EUR transfers to USA
unchanged.

## ARM9 — compressed, and the header understates it

The header's ARM9 size is the **packed** size. The module is BLZ-compressed
(Nintendo's backwards LZ) and unpacks substantially larger:

| | EUR | USA |
| --- | ---: | ---: |
| packed | `0x0C9E68` (827,496) | `0x0C9DBC` |
| unpacked | `0x150C18` (1,379,352) | `0x150B38` |

That is **552 KB the header does not account for**. Any tool that maps a
function address back to its bytes — a mode classifier, a disassembler, a lifter
config — must read the unpacked image, not `rom[off : off+size]`. The symptom of
getting this wrong is loud and easy to misread: a large fraction of known-good
functions report as "outside the image", and the instinct is to doubt the
function list. Doubt the extent first.

## ARM9 sections, from `MODULE_PARAMS`

The SDK plants a `MODULE_PARAMS` structure identified by the magic pair
`0xDEC00621` / `0x2106C0DE`. For EUR it sits at `0x02000BA0`:

| field | value |
| --- | --- |
| autoload list | `0x02150C40` … `0x02150C58` (two entries) |
| autoload data start | `0x02150880` |
| static `.bss` | `0x02150880` … `0x0216C068` |
| SDK version | `0x04027531` |

Giving three programs:

| section | RAM address | size | bss | SHA-1 (EUR) |
| --- | --- | ---: | ---: | --- |
| static | `0x02000000` | `0x150880` | `0x1B7E8` | `dd6d06b7603d6e0f5dca51fbacfc7d710acf4f79` |
| ITCM | `0x01FF8000` | `0x320` | 0 | `d8f52b3ec34b2b0852546298f47b60f8e73fc2b1` |
| DTCM | `0x027E0000` | `0x60` | `0x20` | `11b49efd46d57f51a4d29aad0848a465ea393d17` |

These reproduce the third-party dsd configuration's boundaries exactly, from an
independent derivation. See `02-existing-decomp-assessment.md`.

**Still to check:** whether the ARM7 relocates a block of itself into shared
WRAM. On LSW2 it did, the block was in no header, and until it was found every
instruction of it ran interpreted — the difference between 21 and 60 fps. The
ARM7 here is uncompressed and only `0x26F28` bytes, so this is a cheap check and
it is on the list.

## Overlays

91 ARM9 overlays, `ov000`–`ov090`, all small. The third-party config catalogues
364 functions across all of them combined, against 9,158 in the static module.
The static ARM9 holds essentially all of the game's logic; the overlays are, on
present evidence, mission scripts and similar leaf content.

## NitroFS

4,006 FAT entries: 3,915 named files in 28 directories, plus the 91 overlays
which have FAT entries but no path.

| directory | files | size | contents |
| --- | ---: | ---: | --- |
| `UI/` | 2,642 | 18.60 MB | interface graphics, by far the largest count |
| `(root)` | 541 | 1.03 MB | tilesets, error text, banner, some models |
| `Sprites/` | 357 | 0.56 MB | |
| `BP/` | 139 | 0.38 MB | **the game's data tables** — see below |
| `Maps/` | 122 | 0.91 MB | every battlefield |
| `Models/` | 62 | 0.31 MB | `.nsbmd` + `.nsbca` pairs, mostly vehicles |
| `Movies/` | 24 | 33.05 MB | `.mods` full-motion video |
| `Particles/` | 10 | 0.06 MB | `.hps` |
| `Font/` | 8 | 0.02 MB | `.NFTR`, one per language |
| `LOC/` | 8 | 0.65 MB | `.lng`, one per language |
| `Sound/` | 2 | 60.04 MB | `sound_data.sdat` (60 MB) + `sound_data.sadl` |

### Formats

Counted by magic, not by extension — see `tools/asset_census.py`.

**Stock Nintendo Nitro, readable with existing tooling:**

| magic | count | what |
| --- | ---: | --- |
| NCLR | 694 | palettes |
| NANR | 368 | sprite animation |
| NCER | 368 | cells / OAM assembly |
| NSBCA | 39 | 3D joint animation |
| NSBMD | 39 | 3D models |
| NSCR | 32 | tilemaps |
| NFTR | 8 | fonts |
| SDAT | 1 | the 60 MB sound archive |
| NCGR | 10 raw, 1,917 inside `COMP` | tile graphics |

**This game's own formats:**

| stamp | count | what |
| --- | ---: | --- |
| `COMP` | 2,178 | the compression container — **solved**, `docs/05-the-comp-container.md` |
| `MAPT`…`PAM` | 122 | maps — chunked, tags known |
| `BPNZ` | 1 | `BP/Entities.ebp`, the unit and building table |
| `FANZ` | 1 | `BP/Factions.fbp`, the faction table |
| `MODS` | 24 | video |
| `LANG` | 8 | localised text |
| `.hps` | 10 | particle systems; a version word, not a stamp |
| `.tbp` | 136 | tile blueprints, inside `COMP` |
| `.abp` | 1 | `BP/Animations.abp`, inside `COMP`, no inner stamp |
| `.bin` | 37+ | small structured records, root and `UI/` |
| `.sadl` | 1 | begins `#def` — it is **text** |

### Maps

122 files, named by a two-letter campaign prefix:

| prefix | count | almost certainly |
| --- | ---: | --- |
| `ck` | 15 | Castle — King |
| `cw` | 15 | Castle — Wizard |
| `pi` | 16 | Pirates — one side |
| `pp` | 15 | Pirates — the other |
| `ma` | 15 | Space — one side |
| `mh` | 15 | Space — the other |
| `mp` | 30 | multiplayer / skirmish |
| `test` | 1 | `test1.map` |

Which side of each pair is which is not yet established from the ROM and is
marked as such rather than guessed. The `LOC/*.lng` text will settle it.

## Video and audio

- `Movies/` holds 24 `.mods` files: four per world-side (`Intro`, `Battle`,
  `Flavour`, `Outro`) across six prefixes `A E I K P W`. Those initials do not
  match the map prefixes and the mapping between them is an open question.
- `Sound/sound_data.sdat` is a stock Nitro SDAT, 60 MB — 52% of the cartridge's
  used space. Standard tooling reads it.
- `Sound/sound_data.sadl` is 49 KB of text beginning `#def`. Read it before
  assuming anything about the streamed-audio path.
