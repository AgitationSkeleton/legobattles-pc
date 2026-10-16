# A sixth of the ARM9 is library code, and it is contiguous

**1,354 of the ARM9's 9,154 functions — 191,384 bytes, 16% of the code — are
Nintendo SDK library, not LEGO Battles.** They sit in one unbroken span,
`0x020F2D18`–`0x021218E8`, and they do not need reverse engineering.

This is a scoping result rather than a discovery about the game, and it is the
kind that changes what "how much is left" means.

## How it was found

By accident, while trying to name the IPC channels (`docs/18`). Walking up the
ARM9 call graph from the FIFO sender produced no names and no strings until one
caller turned out to reference `[SDK+NINTENDO:BACKUP]` — the signature the
Nintendo SDK's backup (save) library stamps into a binary.

That reframed the question. If the IPC and save code is SDK, how much else is?

## Two independent signals, one boundary

Bucketing every ARM9 function by address and measuring two things that have
nothing to do with each other:

| | functions | Thumb | RTTI-attributed | bytes |
| --- | ---: | ---: | ---: | ---: |
| inside `0x020F2D18`–`0x021218E8` | 1,354 | **0.4%** | **0.0%** | 191,384 |
| the rest of the ARM9 main module | 7,800 | 47.1% | 31.5% | 1,009,770 |

**Neither signal is a little different — both collapse.** Not one of the 1,354
functions in that span appears in any vtable of the 512 C++ classes
(`docs/07`), and five of them are Thumb against 47% outside.

Both are explained by the same fact. The game is C++ compiled for size, so it is
half Thumb and its polymorphic types leave RTTI everywhere. The SDK is a
prebuilt C library compiled for speed, so it is ARM and has no C++ types at all.

## The spot checks agree

| function | in the span? |
| --- | --- |
| `0x02111CD4` — the IPC FIFO send (`docs/18`) | inside |
| `0x02116E24` — the `[SDK+NINTENDO:BACKUP]` user | inside |
| `0x02111B64` — an `IPCFIFOCNT` user | inside |
| `0x0210FAA8` — IRQ disable | inside |
| `0x020A3A08` — `Battles::Map::LoadTerrain` | outside |
| `0x02001F3C` — the blueprint parser | outside |

Every function this project has identified as game logic falls outside; every
one identified as platform plumbing falls inside.

## What it changes

- **The decompilation target shrinks by a sixth.** 7,800 functions of game code,
  not 9,154. The SDK half is a documented, widely-understood library that other
  DS projects have already named — `MI_`, `OS_`, `FS_`, `PXI_`, `CARD_`, `SND_`.
- **A recompiler should still lift it.** It is real code the game calls; the
  point is only that nobody needs to *understand* it.
- **It explains the ARM7 too.** `docs/17` noted the ARM7 is 322-of-330 ARM code
  and called that unusual. It is not unusual for a *library*: the ARM7 side of
  a DS title is almost entirely SDK, which is exactly why so little of it is
  Thumb and why none of it carries RTTI.
- **It gives the naming route for the IPC channels.** They are not this game's
  channels, they are the SDK's `PXI` channels, and their meanings are a property
  of the SDK rather than of LEGO Battles. `docs/18` records them by number; the
  names should come from SDK material, not from more disassembly of this ROM.

## The boundary is a measurement, not a guess

The span's ends are the last RTTI-attributed function below `0x02120000`
(`0x020F2D18`) and the next one after the gap (`0x021218E8`). Everything strictly
between them is unattributed. The one caveat worth stating: **absence of RTTI is
evidence, not proof** — a game function that happens to be non-polymorphic and
ARM-compiled would look the same. The Thumb collapse is the second signal that
makes the reading solid, and the spot checks are the third, but a handful of the
1,354 could still be game code.

## The general lesson

**Before counting how much of a binary is left to understand, find out how much
of it is somebody else's.** One SDK signature string was enough to ask the
question, and two histogram columns were enough to answer it. Sixteen percent of
this ARM9 was never going to be interesting, and knowing that up front is worth
more than the sixteen percent.
