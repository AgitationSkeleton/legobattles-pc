# The field-of-view constants

## The real builder loop

`func_0203A460` is the leaf that stamps one **3x2 metatile**: six halfword
stores, three through `r1` and three through `r2`, with its six tile indices
read as bytes from a table. It has three callers -- `0203A104`, `0203A1A0`,
`0203A28C` -- each paired with a call to `func_0203A318` immediately before.

The clearest is `0203A104`:

```
0203A0CC  mov   r7, #0xc         ; 12 metatile ROWS ...
0203A0D0  tst   r4, #0xf
0203A0D4  addne r7, r7, #1       ; ... or 13
0203A0E8  mov   sb, #0
0203A0EC  mov   r0, r4           <- loop head
0203A0F0  bl    0203A318         ; pick the tile variant
0203A0FC  mov   r0, sl
0203A100  add   r2, r6, #0x40    ; second row pointer = first + 0x40
0203A104  bl    0203A460         ; stamp the 3x2 metatile
0203A108  add   r6, r6, #6       ; advance 6 bytes = 3 tile entries
0203A110  add   sb, sb, #1
0203A114  cmp   sb, #0xa         ; 10 metatile COLUMNS
0203A120  blt   0203A0EC
```

**The two constants that bound the field of view:**

| constant | at | meaning |
| --- | --- | --- |
| `#0xa` = 10 | `0203A114` | metatile columns, each **3 tiles** wide |
| `#0xc` = 12 (+1) | `0203A0CC` | metatile rows, each **2 tiles** tall |

## They reconcile exactly with the DMA

* 13 rows x 2 = **26 tile rows**
* row stride is **`0x40` bytes = 32 entries**, visible three ways: `add r2, r6,
  #0x40` for the second row of a stamp, and in the sibling caller
  `strh r0,[r8]` / `strh r0,[r8,#0x40]` followed by `add r8, r8, #0x80` for a
  two-row advance
* 26 rows x 32 entries = **832 entries**

832 is precisely the DMA length captured in `docs/118`
(`cnt=80000340`, 832 halfwords, to both `06000000` and `06000800`). Every number
in the chain now agrees: 10 columns x 3 tiles = 30 tiles drawn into a 32-entry
stride, 13 rows x 2 = 26 rows, 26 x 32 = 832, DMA'd once per layer.

The 30-of-32 is the scroll margin: the visible screen is 32 tiles wide and the
builder fills 30 columns of metatiles plus the partial column the scroll offset
exposes.

## What widening requires, concretely

Doubling the visible width means:

1. **`#0xa` -> about 21** at `0203A114`, so the builder stamps enough 3-wide
   metatiles to cover 64 columns;
2. **the row stride `0x40` -> `0x80`** everywhere it appears, because the
   staging buffer's rows must hold 64 entries rather than 32 -- this is the
   invasive part, as the stride is baked into `add r2, r6, #0x40`,
   `strh r0,[r8,#0x40]` and `add r8, r8, #0x80`;
3. **the DMA length 832 -> 1664**, and a destination that is a genuine
   64-column map rather than two packed 32-column maps;
4. **the packing undone** -- per `docs/117`, BG2's second block *is* BG1's
   first, so a 64-column BG2 requires relocating BG1's and BG3's maps first.

Steps 1 and 3 are single constants. Step 2 is a handful of related constants.
Step 4 is the structural one and remains the gate.

## Correction to docs/118

`docs/118` analysed the loop at `0203A438` and reported that its outer pass
"runs exactly twice, matching the two layers" and its inner bound is a register
so the span is "data-driven, not a hardcoded 32". **Both statements are wrong.**

That loop belongs to `func_0203A318`, which is not the builder: it runs
`r8 = -1, 0, 1` and `sl = -1..1`, calls two predicates, and returns a packed
bitfield of 2-bit codes in `r6`. It is a **3x3 neighbourhood scan** that selects
which tile variant to stamp -- an autotiler, not a column loop. Its `cmp r8, #2`
is the neighbourhood extent, not a layer count, and it runs three times, not
twice.

The real bound is the immediate `#0xa` above. So the span is **not**
data-driven; it is a constant in the instruction stream, which is more
tractable, not less.
