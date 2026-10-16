# 182 — What Command & Conquer does, and what it says about this port

Source: `CnC_Remastered_Collection-master` (TIBERIANDAWN / REDALERT), the DLL
sources EA released for the Remastered Collection. Worth reading here because
the remaster solves **exactly** the problem this port has: an original game with
a fixed viewport, and a modern host renderer that wants to draw at any
resolution.

The two lessons below both bear on work this port has been getting wrong.

## 1. The shroud is per-CELL state, not a separate array

`DLLExportClass::Get_Shroud_State()` exports fog by walking the map and asking
each cell:

```cpp
shroud_entry.IsVisible = cellptr->Is_Visible(PlayerPtr);
shroud_entry.IsMapped  = cellptr->Is_Mapped(PlayerPtr);
```

Visibility lives **inside the cell structure**, per player, alongside terrain and
occupancy. There is no standalone "explored bitmap" anywhere.

docs/179 recorded three failed attempts to find such a standalone array in LEGO
Battles by diffing main RAM, and concluded the method was at fault. This suggests
the *premise* was at fault too: **there may be no separate array to find.** If
LEGO Battles follows the same shape, visibility is a field in the per-cell map
structure — which this port has already located the terrain half of (the 64x63
grid of u16 metatile indices, docs/168).

That makes the next search a targeted one rather than another 4 MB diff: look for
per-cell data *parallel to the grid already located*, at the same cell count,
and check whether a cell's entry is wider than the two bytes the terrain uses.

## 2. The viewport is state derived from the output buffer, not a constant

`DisplayClass::Set_View_Dimensions()`:

```cpp
if (width == -1) {
    TacLeptonWidth = Pixel_To_Lepton(SeenBuff.Get_Width() - x);
} else {
    TacLeptonWidth = width * CELL_LEPTON_W;
}
...
Confine_Rect(&xx, &yy, TacLeptonWidth, TacLeptonHeight,
             MapCellWidth * CELL_LEPTON_W, MapCellHeight * CELL_LEPTON_H);
Set_Tactical_Position(...);
WindowList[WINDOW_TACTICAL][WINDOWWIDTH]  = Lepton_To_Pixel(TacLeptonWidth);
WindowList[WINDOW_TACTICAL][WINDOWHEIGHT] = Lepton_To_Pixel(TacLeptonHeight);
```

Three things happen together, and that is the whole trick:

1. the view size is **read from the actual output buffer**, not a constant;
2. the tactical position is **re-clamped** so an enlarged view stays on the map;
3. every derived structure — here the clipping window — is **updated from the
   same source**.

Widening is one function that owns the dimensions, and the rest of the engine
follows because it reads them rather than hard-coding them.

## What this says about the approach taken so far

The wide field in this port is a **host-side reconstruction**: terrain redrawn
from the level grid, the guest's own 256x192 pasted over the camera rect, and
off-camera units approximated from learned sprite patches (docs/179). It fills
any window at any aspect with no black bars, and the minimap box reports true
coverage (docs/180). But it is a reconstruction, and the artefacts it keeps
producing — cut-off units, ghost structures, a revealed map with a shrouded
rectangle in the middle — are all symptoms of reconstructing rather than
rendering.

C&C shows the alternative, and this port has already found the pieces for it:

* docs/163 located the viewport dimension object — 32x24 tiles, with the fog
  bitmask sized `(width + 7) / 8` **from that same width**. That is precisely
  C&C's "derived structures read the one source" pattern, in this cartridge.
* docs/163 also found that `32` and `24` are copied from a further object rather
  than being immediates, and said the durable change belongs further up the
  chain. That object is this game's `Set_View_Dimensions`.
* docs/164 flagged the consequence C&C's code makes obvious: widening the view
  without growing the buffers derived from it is a **buffer overrun**. The fog
  mask goes from 96 to 168 bytes at 56 tiles wide, and nothing allocated that.

So the durable fix is not a better sprite learner. It is: find the one place the
view dimensions originate, enlarge them, and enlarge every buffer derived from
them — the fog mask first, since its size is written as a function of the width
right there in `020EEB1C`. Then the game renders the wider field itself, with its
own units, its own fog and its own particles, and the reconstruction can go.

**Recorded as direction, not as done.** Nothing in this document has been
implemented or measured against LEGO Battles; it is a reading of another game's
source and what it implies about where to look next.
