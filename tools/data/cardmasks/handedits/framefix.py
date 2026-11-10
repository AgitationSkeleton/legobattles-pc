"""Replace the full-width frame-row cut (rows 86-87) with a cut only where no figure column continues
from row 85 (so legs that pass the frame stay coloured). py -3.13 framefix.py NAME..."""
import sys, json
import numpy as np
import edit, render

edits = edit.load_edits()
for n in sys.argv[1:]:
    lst = [e for e in edits.get(n, []) if e.get("rect") != [0, 86, 95, 87]]
    edits[n] = lst
    mask, alpha, R = edit.apply(n, edits=edits)
    legs = mask[84] | mask[85]
    pix = [[x, y] for y in (86, 87) for x in range(96) if mask[y, x] and not legs[x]]
    lst.append({"op": "sub", "pixels": pix, "why": "the frame line above the HP strip, between the legs only"})
    print(n, len(pix), "frame pixels removed; leg columns kept:", int(legs.sum()))
with open(edit.EDITS_PATH, "w", encoding="utf-8", newline="\n") as fh:
    json.dump(edits, fh, indent=1)
