# First read of the save record's contents

`docs/100` read the save container -- a 16-byte magic and two 242-byte records --
and explicitly left the **record contents** unread. That is what blocks the
Unlock family (`docs/121`, `docs/122`). This is a first read of one, taken from
a save **this port wrote itself**, so no cartridge dump is involved.

## The anchor

`tools/lb_warp.py` types a profile name during its route: `Q`, `W`, `E`. That
name appears in the record, which proves both that the record is the profile and
where its name field begins:

```
+000  59 F5 24 43 51 77 65 00 00 00 00 00 00 00 00 00  |Y.$CQwe.........|
+010  00 00 00 00 00 7F 65 40 00 C0 00 E0 01 00 01 20  |......e@....... |
+020  00 00 00 40 00 60 00 80 00 B7 C0 00 00 00 00 00  |...@.`..........|
...
+0A0  00 00 00 00 00 00 00 00 00 FF FF 0A 00 00 00 00  |................|
+0B0  00 00 00 00 00 BF C0 00 00 00 00 00 00 00 00 00  |................|
+0C0  00 00 00 00 00 00 00 00 00 00 00 00 00 7F FF 85  |................|
+0D0  00 00 00 00 00 00 00 00 00 5F E0 00 00 00 00 00  |........._......|
```

Only **30 of 242 bytes** are non-zero, and the highest is at `0xDA`.

## What can be said

* **`+0x00 .. +0x03` is per-record, not shared.** Records 0 and 1 differ there,
  and `docs/100` observed record 1 being a byte-for-byte copy in a *fresh* save.
  A four-byte field at the front that changes with content is a **checksum or
  id**; which of the two is not established.
* **`+0x04` is the profile name**, ASCII, null-terminated -- `Qwe` here, matching
  what the route typed.
* The populated bytes past the name fall in **four separated clusters**:
  `0x15..0x2A`, `0xA9..0xAB`, `0xB5..0xB6`, `0xCD..0xCF` and `0xD9..0xDA`.
* Several of those values look like **packed bit flags** rather than counts:
  `FF FF 0A`, `7F FF 85`, `BF C0`, `5F E0`. Runs of set bits ending in a partial
  byte is the shape of a bitmap sized to something that is not a multiple of
  eight.

**That last point is an observation about shape, not an identification.** No
claim is made that any of these is the unlock bitmap; `docs/122` already
eliminated one plausible-looking candidate for exactly that kind of reasoning.

## The experiment that would settle it

Two saves differing by **one known change** -- unlock one thing, complete one
match, win one skirmish -- and a byte diff between them. The clusters above give
a short list of places to look, so the diff will be readable rather than
open-ended.

`lb_stage.py` and the debug server make the state side of that reachable; what
is still needed is a way to make the game *commit* a specific unlock, which is
gameplay progress rather than a memory write.

## Why this is worth having

The Unlock family is badged `unread`, and it has been blocked since `docs/97`
first said "where progress is *stored*" was the missing piece. This does not
unblock it, but it moves the question from "what does a save record look like"
to "which of these five clusters changes when X happens", which is a much
smaller question and one that a single controlled experiment answers.
