# 177 — The cheat-code validator, `func_02009158`

Status: **located and read.** Closes the question docs/97 left open twice ("the
validator is therefore still unlocated", "until it is found neither *which code
does what* nor *how to trigger one* is established").

## How it was found

docs/97 looked for it around `CheatKeyboardScreen` and concluded
`CheatKeyboardScreen::virtual_08` was not it (correctly — that is the ordinary
widget walk). The validator is not in the screen classes at all. It is one of the
six functions that reference the record table from a literal pool:

    xref 0x02125FF4 -> func_02000d18  func_02007d80  func_02007f10
                       func_02009158  func_020c2ec4  func_020d2500

`func_020c2ec4` and `func_020d2500` were already read as table walkers.
`func_02009158` is the one that takes a string.

## What it does

```c
bool func_02009158(void* screen /*r0*/, const char* typed /*r1*/, bool r2)
{
    record = 0x02125FF4;
    for (i = 0; i < 0x9B; ++i, record += 0x10) {     /* all 155 records */
        func_020f17a4(&tmp, record[0x04], 0x0213B684, 0x69E);   /* wrap the code */
        if (func_020f1958(typed, &tmp)) {                       /* compare */
            if (record[0x0C] == screen[0x54]) {                 /* kind vs mask */
                func_020088D4(screen, record);                  /* apply it */
                if (r2) func_0203E1D4(record[0x08]);            /* the LOC id */
                result = 1;
                break;
            }
        }
        func_020f187c(&tmp);                          /* destroy the temporary */
    }
    return result;
}
```

Thumb, 0x74 bytes. The loop bound `cmp r4, #0x9b` is the 155-record count, and
`adds r5, #0x10` the 16-byte stride — both matching docs/170.

**It independently confirms the corrected record layout.** The loop reads the
code string from `record[0x04]` and the kind from `record[0x0C]`, which is
exactly the layout docs/170/171 settled on and *not* the one docs/97 first
recorded. A second, independent witness to a field order that had been wrong.

`record[0x0C] == screen[0x54]` is the same mask test `func_020088D4` performs on
entry, so the screen carries which category of code it will accept.

## Why this matters beyond cheats

docs/176 located the live profile record at `0x02168500` and established that the
only thing still missing for the four Unlock options is **what the bits mean** —
and that the way to learn that without guessing is to change one unlock and diff
the record.

This is the missing half of that experiment. With the validator located, a code
can be submitted programmatically rather than typed on the in-game keyboard, and
the profile record diffed across it. Two of the 57 codes name themselves usefully
for that: `SKQMXPL` (Reveal Map) and `LJYQRAC` (Show Minikit).

## Not yet done

Calling it needs the string in whatever form `func_020f1958` compares — the
temporary is built by `func_020f17a4` from a pointer, a second pointer
(`0x0213B684`) and a length (`0x69E`), which looks like a string-table lookup
rather than a plain `strcmp`, so the argument is probably an id or a wrapped
object rather than a bare `char*`. That has to be read before the call can be
made, and guessing the calling convention would be exactly the kind of invention
the project does not do.
