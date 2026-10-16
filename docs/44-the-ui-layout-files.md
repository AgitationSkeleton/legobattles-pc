# The UI layout files: owners found, format not

**Status: the 134 `UI/*.bin` files are identified as screen layouts, 64 are
matched to the exact class that loads them, and the shared parser is located at
`func_020E16C8`. The byte format is NOT decoded, so the data-decoded figure
stays at 6.8%.**

This pass went after the data segment. `.bin` is the largest opaque group by
file count — 134 files, 3.4% of the cartridge — so decoding it would take data
from 6.8% to 10.3%. It did not get there, and the route that failed is as
instructive as the one that worked.

## Guessing at the layout: three attempts, three failures

All 134 files live under `UI/` and are named after screens — `Options.bin`,
`Achievements.bin`, `CampaignBriefing.bin`, `WiFiScreen.bin`. Hex dumps showed a
16-byte header beginning `06`, then what looked like a table, then records
containing plausible screen coordinates.

Three structural hypotheses were tested against all 134 files:

| hypothesis | result |
| --- | --- |
| `size == 16 + 36 × byte1` | 41 of 134 |
| first u32 is the offset to the first record, with `byte1` entries | 41 of 134 |
| same, with `byte1 - 1` entries | 47 of 134 |

**The 41 that "passed" the first two were the 41 empty files** — those with
`byte1 == 0`, where the rule reduces to `size == 16` and is trivially true. The
apparent 31% agreement was entirely the degenerate case.

That is the trap worth recording: **a rule tested on a population containing
many trivial members will report the trivial members as successes.** The check
that exposes it is to score only the non-degenerate cases, which here would have
read 0 of 93.

One real fact did fall out: **byte 0 is a version — 6 in 107 files, 4 in 25**,
with two odd files at `0xFD`. Mixing two formats is part of why no single rule
fitted, and any future attempt should split on it first.

## Reading the loader instead

The method that has worked all project is to stop guessing and find the code.
Searching for functions that reference a `.bin` path found 64, and 54 of them
already carry a class name from the RTTI:

```
Battles::User::Options::virtual_06                    UI/Common/Options.bin
Battles::User::QuickDialog::virtual_06                UI\Common\QuickDialog.bin
Battles::User::Achievements::virtual_06               UI\FE\Achievements.bin
Battles::User::CampaignBriefing::virtual_06           UI/FE/CampaignBriefing.bin
Battles::User::CheatKeyboardScreen::virtual_06        UI\FE\CheatKeyboardScreen.bin
Battles::User::ClientLobby::virtual_06                UI\FE\ClientLobby.bin
```

**`User::<Screen>::virtual_06` is the layout loader**, one per screen class.
That is a slot on `User::ScreenType` that `docs/07` recovered without knowing
what it did, and it is now identified across the whole hierarchy — every screen
in the game loads its layout through the same virtual.

Note the path separators: some are `/` and some are `\`. The same build uses
both, which is a detail a reimplementation has to tolerate rather than
normalise.

`docs/data/ui_screens.tsv` carries the 64 pairs with each file's size and
version byte.

**64 of 134 files are claimed by a loader.** The remaining 70 are referenced
some other way — most likely from a table rather than an inline literal, which
is the obvious next thread.

## The parser

Of the functions called by the 64, three dominate: `func_020F1710` and
`func_020F187C` at 65 calls each, and **`func_020E16C8` at 63** — called by
almost every screen that loads a layout. That is the shared parser, and reading
it is what would settle the format.

It was not read this pass. Saying so plainly matters more than the omission: the
data-decoded figure stays at **6.8%**, because a format is decoded when a parser
in this repository reads it, and none does.

## What this is worth anyway

Even without the byte format, the mapping is useful for the port scope. A
reimplementation needs to know that **screen layout is data, not code** — 134
files describing the UI of every screen, loaded through one virtual, in two
format versions. `docs/31` established the opposite for missions, which are
compiled C++ with no data representation at all. The two halves of the game are
built on opposite principles, and that is worth knowing before either is
reimplemented.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling; no second source exists |
| ARM9 annotated | 42.6% | 2,501 listed + 1,465 derived |
| data decoded | **6.8%** | unchanged — the UI format is not decoded |
| decompilation | 0.143% | 351 functions byte-verified |

The next step on this segment is `func_020E16C8`, and it is a much better lead
than any hex dump: a parser tells you the format exactly, and the three failed
hypotheses above are what guessing costs.

> **Followed in `docs/45`.** `func_020E16C8` is a load-once guard;
> `func_020E4438` registers a callback; `func_020E484C` looks the resource up
> and then **calls a virtual through `[[r0]+0x20]`**. The parse is a virtual
> dispatch per screen class, so it is a larger job than reading one function.
