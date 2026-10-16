# Squads, jobs, and the kind enum

**Status: the three-pass chain closes. `squad+0x38` is the job, each job
constructor stamps a distinct kind at `job+0x24`, and those six constants are
exactly the six keys the AI counter is called with. `docs/76`'s hypothesis is
confirmed and `docs/75`'s retracted claim is fully replaced.** Annotation 54.3%
→ 54.3%; **109 of 109 checks pass**.

## The restart note paid again

`docs/76` ended with a stated next step — *the `Squad` constructor, not more
offset matching*. That is the fourth pass in a row where a written-down restart
made the next one short (`docs/63` → `docs/69`, `docs/70` → `docs/71`, `docs/75`
→ `docs/76`, now `docs/76` → here).

The vtable-reference method from `docs/75` found the class's functions
immediately. The **destructor** turned out to be the informative one:

```c
void Squad::~Squad(void) {
    this->vtable = Squad_vtable;
    ...
    if (this->job_38) {
        if (this->job_38->state_18 != 4) {
            this->job_38->state_18 = 3;      /* cancel */
            this->job_38->vtable[1](job);
        }
        func_0208FD78(this->job_38);
        Heap_Delete(this->job_38);           /* the squad OWNS its job */
    }
    this->job_38 = 0;
    while (!Empty(this->c_24)) { e = At(this->c_24, 0); e->vtable[1](e); }
    ...
}
```

**A squad owns exactly one job and deletes it.** `job+0x18` is the state field
`docs/74` found slot 5 setting to 1 or 4 — and here it is set to **3 on
cancellation**, giving a three-value state machine:

```
1   active        (slot 5, when the job has a valid subject)
3   cancelled     (the squad was destroyed under it)
4   dead/invalid  (slot 5, when the subject is missing; never overwritten)
```

The `!= 4` guard means **a job that is already dead is not re-cancelled** — the
same idempotence `docs/71` found in `Objective_MarkComplete`. Twice now this
codebase guards a state transition rather than letting it repeat.

## The kind enum, from the constructors

Each of the six job constructors writes one constant to `+0x24`:

| kind | class |
| ---: | --- |
| 0 | `JobScout` |
| 1 | `JobRepair` |
| **2** | **unused — no class writes it** |
| 3 | `JobContractBuild` |
| 4 | `JobAttackTaskForce` |
| 5 | `JobAttack` |
| 6 | `JobDefend` |

`docs/76` recorded the keys `AI_CountByKind` is called with across all eleven
call sites as **0, 1, 3, 4, 5, 6**. That is **exactly this set, with 2 absent
from both** — the enum has a hole and the call sites have the same hole.

Two independent lists agreeing including their gap is about as strong as this
kind of inference gets. `docs/72` made the same argument when the build-limit
classes matched the category names.

## What that makes the defend decision

`docs/75` read the urgency flag as `count(...) < 1` and glossed it wrongly;
`docs/76` retracted the gloss and left the meaning open. It is now readable:

```c
urgent = (AI_CountByKind(ai->squads, 6) < 1);   /* kind 6 = JobDefend */
```

**"Raise this defend job as urgent if no squad is already defending."** The AI
escalates only the *first* defensive response and treats later ones as routine
while one is in flight.

Combined with `docs/75`'s other finding — that the random pick happens only when
there is more than one candidate — the whole 132-byte decision now reads
end to end with nothing left unexplained.

## The offset that nearly caught me again

`squad+0x24` is **a container the squad owns and destroys**. `job+0x24` is **the
kind enum**. I was chasing `+0x24` across this pass and the two are three lines
apart in the same destructor.

That is the fourth record type in this project to use `+0x24` for something
different, and the disambiguation was the same as always: **the access
width and the object**. The squad's is a pointer passed to a container API; the
job's is a word compared against a small constant.

`docs/60`'s rule has now been the answer four separate times. It is not a
curiosity — it is the single most reliable pitfall in this codebase, and the
check that guards it (`docs/76`'s width assertion) should probably be the
template for any future offset claim.

## Tips and pitfalls

* **Read the destructor.** For an ownership question it beats the constructor:
  what a class deletes is what it owns, stated unambiguously. The constructor
  showed the vtable; the destructor showed the whole object graph.
* **A gap that appears in two independent lists is evidence.** Kind 2 is unused
  by the classes *and* unused by the call sites.
* **Guarded state transitions are a pattern here, not an accident.** `!= 4`
  before cancelling, `!complete` before completing.
* **Four passes, four restart notes, four short passes.** Writing down where a
  search stopped and what to try instead has now paid four times running; it is
  the highest-leverage habit in this project.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling |
| data decoded, by file | **99.4%** | 3,891 of 3,915 |
| ARM9 annotated | **54.3%** | 2,501 listed + 2561 derived, 118 read by hand |
| decompilation | 0.271% | 789 functions, ceiling measured |
