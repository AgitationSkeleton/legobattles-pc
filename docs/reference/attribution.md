# Attribution

Where every borrowed fact, tool and idea in this project came from. Recorded
because knowing the provenance of a fact is worth more than any licence
requires — when a borrowed number turns out to be wrong, this is the list that
says what else to re-check.

## Facts and configuration

| what | from | licence | how it is used |
| --- | --- | --- | --- |
| ARM9 delinking configuration, section boundaries, 91-overlay layout, 9,522 function extents with ARM/Thumb modes, 45 function names, map chunk tags (`TERR`/`EVNT`/`TRIG`/`MINE`/`MARK`/`PAM`) | [LiruJ/Lego-Battles-Decomp](https://github.com/LiruJ/Lego-Battles-Decomp) (snapshot `Lego-Battles-Decomp-master`, 2025-10-20), itself templated from [zeldaret/ph](https://github.com/zeldaret/ph) | CC0 1.0 | adopted; section boundaries independently re-derived and confirmed — see `docs/02` |
| overlay → mission mapping for 64 of 91 overlays, with map names, acts and mission numbers | the same project's `docs/overlays.md` | CC0 1.0 | reproduced in `docs/06-overlays-are-missions.md`; the 26 it could not recover are left blank, not inferred |
| `BP/Entities.ebp` as an editable unit table, and a second sample of the file | [LegoBattlesRebalancePatch](https://github.com/) by Ryncat, with Alpha47; uses opiter09's Entities Editor | see repo | evidence and a diff target only |
| method, bring-up order, DS hardware gotchas, the no-hand-edits rule, the Ghidra failure catalogue | `LSW2DS-RE/playbook/` — the LEGO Star Wars II DS project, whose playbook names LEGO Battles as its intended reader | same author | digested in `reference/lsw2-playbook-digest.md` |

## Tools

| tool | from | licence | role |
| --- | --- | --- | --- |
| [OpenLegoBattles](https://github.com/LiruJ/OpenLegoBattles) | LiruJ | | an open reimplementation with ARM emulation code, named by the decomp's `docs/overlays.md`. Not present locally; noted as a lead |
| [ds-decomp (`dsd`)](https://github.com/AetiasHax/ds-decomp) | AetiasHax | | delinking and the matching-decomp track |
| [objdiff](https://github.com/encounter/objdiff) | encounter | | function-level diffing for the matching track |
| [ds-rom](https://github.com/AetiasHax/ds-rom) | AetiasHax | | ROM extract/rebuild for that track |
| [ndsrecomp](https://github.com/mstan/ndsrecomp) | mstan | | static recompilation; the execution engine |
| [Ghidra](https://ghidra-sre.org/) 12.1.2 | NSA | Apache-2.0 | the analysis database |
| [melonDS](https://melonds.kuribo64.net/) | Arisotura et al. | GPL-3.0 | the oracle |
| [DeSmuME](https://desmume.org/) | DeSmuME team | GPL-2.0 | second opinion, scripted capture |
| [NitroPaint](https://github.com/Garhoogin/NitroPaint) | Garhoogin | | NCGR/NCLR/NCER/NANR/NSCR |
| [apicula](https://github.com/scurest/apicula) | scurest | 0BSD | NSBMD/NSBTX/NSBCA |
| [libnds](https://github.com/blocksds/libnds) | devkitPro / BlocksDS | | naming what the ARM9 calls |
| Metrowerks `mwccarm` | Freescale/NXP | proprietary | the matching-decomp track only |

## Formats

The `COMP` container in `tools/lb_comp.py` was reverse engineered here, from the
ROM. Its LZ11 inner streams are Nintendo's standard
compression type `0x11`, which is publicly documented; the block table,
the four-byte slot padding and the negative-size stored-raw convention are this
project's findings and are written up in `docs/05-the-comp-container.md`.

The BLZ (backwards LZ) ARM9 decompressor in `tools/nds_extract.py` implements
the algorithm from CUE's public-domain `blz.c`, rewritten from its description
rather than transliterated.

## The game

LEGO Battles is © Warner Bros. Interactive Entertainment and The LEGO Group,
developed by Hellbent Games. This project is unaffiliated and non-commercial,
and redistributes no part of it — see `.gitignore`.
