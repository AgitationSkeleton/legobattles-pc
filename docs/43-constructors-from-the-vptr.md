# Constructors, from the pointer they assign

**Status: annotation reaches 42.6% — 2,501 listed plus 1,465 derived, of which
948 are real names. Two new tiers come from a single observation: the first
thing a C++ constructor does is store its class's vtable pointer.**

## The signal

`docs/07` recovered 512 typeinfos. In the Itanium ABI a vtable is preceded by a
pointer to its typeinfo, so inverting that gives **959 vtable addresses mapped
to class names** — and a function that *stores* one of those addresses is
constructing an instance of that class.

This had already been used once, narrowly: `docs/32` named all thirteen
objective classes by following each factory's constructor to its vtable literal.
That was a hand-aimed use of a signal that generalises.

Applied across the ARM9: **982 functions store a known vtable pointer, 845 store
exactly one, and 546 of those were unnamed.**

## Splitting the strong claim from the weak one

The temptation is to call all 546 `Class::ctor`. That would be wrong, and the
data says so plainly:

```
24  Battles::Sim::Continent
 4  Battles::CastSpellCommand
 4  Battles::Sim::ConstructProgressAction
 4  Battles::Sim::EntityCommandBase
```

**There are not 24 constructors for `Sim::Continent`.** Those are 24 sites where
a Continent is constructed *inline* — the compiler emitted the vptr assignment
into each caller rather than calling a shared constructor. Naming all 24
`Continent::ctor` would be 23 false names.

So the set splits by how many unnamed functions assign a given class's vptr:

- **exactly one → tier V.** That function is the class's constructor, and
  `Class::ctor` is a **name**. 341 classes qualify; 244 survive after earlier
  tiers claim their overlaps.
- **more than one → tier A.** Construction is inlined, so `constructs_Class` is
  an **association** — it says what the function makes, not what it is. 204
  functions across 79 classes.

The split is not a judgement call applied afterwards; it is the rule, and it
falls out of the same count that would otherwise have produced the false names.

## Six tiers, and the ratio that matters

```
2,501 of 9,314 functions  -> 26.9%
plus 1,465 derived -> 42.6%
   S  135  named from a sole string reference
   F  235  C++ static-object initialisers
   T  334  tail-call thunks
   V  244  constructors, by the vptr they assign
   A  204  constructs a class (inlined at several sites)
   C  313  attributed to a class: every caller is a method
948 are NAMES; 517 are associations.
```

Tiers are claimed strongest-first — S, F, T, V, A, C — so a function that
qualifies for two gets the more informative label. That is why tier V reports
244 rather than the 341 classes that qualify: nearly a hundred of those
constructors are also thunks or static initialisers, and those labels say more.

**The ratio is the number to watch.** Four passes ago the derived set was 489
functions of which 72% were bare associations. It is now 1,465 of which **65%
are names**. Annotation has grown three-fold and got *better* per entry, which
is the opposite of what usually happens when a metric is pushed.

## The pitfall this tier nearly walked into

Every derivation in `docs/40`–`docs/42` was safe because its rule was strict:
*exactly one* string, *every* caller, *no* return-to-caller. This one had no
such guard available — "stores a vtable pointer" is simply true of 546
functions.

The guard had to be constructed from the distribution instead: **count how many
functions make the same claim, and demote the claim when several do.** A class
with one constructor is unremarkable; a class with 24 is telling you the
compiler inlined it, and that fact is visible only in aggregate.

Generalised: *when a signal cannot be made strict, make it comparative.* A claim
that many functions satisfy equally is weaker than one only a single function
satisfies, and the count is available for free.

## Where the segments stand

| segment | figure | verified |
| --- | ---: | --- |
| ARM7 static code | **100%** | 404 of 404 |
| ARM9 `.text` code | **100%** | 1,201,170 of 1,201,170 |
| overlay code | **100%** | 91,540 of 91,540 |
| ARM7 wram | 71.5% | at its ceiling; no second source exists |
| ARM9 annotated | **42.6%** | 2,501 listed + 1,465 derived (948 names) |
| data decoded | 6.8% | |
| decompilation | 0.143% | 351 functions byte-verified |

Annotation has moved 26.9% → 42.6% across four passes without reading a single
function for its behaviour. Every gain came from a structural property the image
already carried: what a function references, what shape it has, what it calls,
who calls it, and now what pointer it assigns. Those are close to spent; the
remaining 5,300 unnamed functions will need to be read.
