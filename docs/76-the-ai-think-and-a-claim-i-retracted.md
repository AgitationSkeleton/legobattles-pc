# The AI think, and a claim the access width refuted

**Status: `func_0208B460` is the AI's top-level think — 1252 bytes, 543
instructions, dispatching to every one of the six job kinds. Its most-used helper
counts by a key, and reading that helper properly **refutes a claim `docs/75`
made one pass earlier**, using the exact rule `docs/75` itself recorded.**
Annotation 54.3% → 54.3%; **107 of 107 checks pass**.

## The think

`func_0208B460` is the only site that builds two job kinds, which made it the AI's
main loop. It is more than that — it calls the *other* decision sites too:

```
func_0208B460   AI_Think, 1252 B, 543 instructions, 39 distinct callees
  -> New_JobScout                x1
  -> New_JobAttackTaskForce      x1
  -> AI_DecideDefend             x1
  -> AI_DecideAttack  (0208B9D4) x2
  -> Random_Below                x2
  -> func_0209DE9C  (the counter) x6
```

So the AI is **one think that owns everything**: it raises scouting and
coordinated attacks itself, and delegates the defend and single-attack decisions
to the small functions `docs/75` read. `JobRepair` and `JobContractBuild` are
raised from elsewhere (`0x02094938`, `0x02095104`), which fits `docs/73`'s
finding that the economy jobs are the smallest and least connected.

Its comparison immediates are the AI's thresholds, and there are very few:

```
0 (x28)   1 (x1)   2 (x3)   3 (x2)   4 (x2)   10 (x2)   15 (x1)
```

Twenty-eight null checks and **seven real thresholds**: 1, 2, 3, 4, 10, 15. The
opponent's entire strategy is gated on half a dozen small integers.

## The counter, read exactly

`func_0209DE9C(obj, key)` is called six times inside the think and eleven times
in total. It is 52 bytes and worth quoting in full, because two details in it are
easy to lose:

```c
u8 count = 0;
vec = obj->[4];
n   = vec->count_8;
if (n <= 1) return 0;

p = vec->data_4 + 4;            /* <-- starts at element ONE */
for (i = 1; i < n; i++, p++) {
    e = *p;
    if (e->[0x38] && e->[0x38]->[0x24] == key)
        count = (u8)(count + 1);   /* <-- truncated every iteration */
}
return count;
```

* **Element 0 is never examined.** The loop is set up at index 1, and with a
  single element the function returns 0 without looking at it. Index 0 of that
  vector is reserved for something the AI does not count.
* **The accumulator is masked to 8 bits on every increment** (`lsl #24`,
  `lsr #24`), so 256 matches would report 0. With the build limits in `docs/68`
  capping at 20 that cannot happen in play, but it is what the code says and a
  port that widens the counter is not reproducing it.

## The claim I retracted

`docs/75` read `func_0209DE9C(ai->obj_4C, 6)` in the defend decision and glossed
it as *"category 6 = siege/special"*, citing `docs/72`'s taxonomy. The key range
across all eleven call sites is **0, 1, 3, 4, 5, 6** — which sits neatly inside
`docs/72`'s 0–6 unit categories, and that coincidence is what made the gloss feel
safe.

It is wrong, and the reason is one line of the disassembly:

```asm
ldr  r5, [r5, #0x24]      ; a WORD
cmp  r1, r5
```

**A blueprint category is a byte** (`docs/72`: `blueprint+0x5C`, read with
`ldrb`). This field is a word, and it is reached by a completely different path —
`element->[0x38]->[0x24]` rather than `entity->[8]->+0x5C`. A search for any
function storing a `+0x5C` byte read into a `+0x24` field found **two hits, both
word-sized and neither a category**.

So the count is over some 0–6 *kind* enum on a nullable sub-object at `+0x38`,
not over unit categories. `docs/75` is corrected in place.

**The rule that caught it is the one `docs/75` wrote down in the same pass:**
*access width disambiguates a shared offset*. I used it there to separate two
classes and failed to apply it to my own inference three paragraphs earlier.
A rule is only worth what you spend re-applying it.

### What it probably is, stated as unproven

The elements have a **nullable pointer at `+0x38`** whose `+0x24` is a small
word-sized enum with seven values, and the count gates job creation. The AI has
exactly **six job kinds** (`docs/73`), which with a "none" value is seven. The
natural reading is *"how many of my squads are currently running job kind K"* —
`Squad` and `ArmySquad` are `SListItem` subclasses (`docs/71`) and the AI keeps a
vector of them.

> **Confirmed by `docs/77`.** `squad+0x38` is the job, and each of the six job
> constructors writes a distinct constant to `job+0x24`: Scout 0, Repair 1,
> ContractBuild 3, AttackTaskForce 4, Attack 5, Defend 6. Those are exactly the
> six keys observed at the call sites. The counter counts **squads by the kind of
> job they are running**, and `kind 6` in the defend decision is `JobDefend`.

**That is a hypothesis, not a finding.** Confirming it needs the writer of
`+0x24`, and the next step is the `Squad` constructor rather than more offset
matching — the same restart note that made `docs/69` short.

## Two functions with one body

`func_0208C1C0` and `func_02096578` (found in `docs/72`) are **byte-identical**:

```asm
ldr r0, [r0, #4] ; ldr r0, [r0, #0x44] ; bx lr
```

Two addresses, one accessor, emitted into two translation units — an inline
getter the compiler did not fold. `func_0208C1C8` is its sibling for `+0x48`.
Worth knowing when counting "distinct functions": the corpus has duplicates that
are duplicates by construction, not by coincidence.

## Tips and pitfalls

* **Re-apply your own rules to your own inferences.** The width check that
  `docs/75` established would have caught `docs/75`'s error in the same pass.
* **A plausible key range is not evidence.** 0–6 matching a known 0–6 taxonomy is
  exactly the kind of coincidence that survives scrutiny it never gets.
* **Read loop initialisation, not just the loop body.** Starting at index 1 is
  invisible unless you look at the setup, and it changes every result this
  function returns.
* **Truncation inside an accumulator is behaviour.** `lsl #24 / lsr #24` on every
  increment is a `u8` counter, whatever the return type looks like.
* **Count the thresholds.** Twenty-eight null checks and seven real constants in
  1252 bytes says the AI is mostly plumbing over a handful of decisions.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | **54.3%** | 2,501 listed + 2558 derived, 114 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
