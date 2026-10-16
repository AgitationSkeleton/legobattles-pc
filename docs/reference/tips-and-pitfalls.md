# Tips and pitfalls, from this project

Written while doing it. Every entry is here because it cost time or nearly
produced a wrong answer on **LEGO Battles specifically**. The general DS advice
inherited from the previous project is in
[`lsw2-playbook-digest.md`](lsw2-playbook-digest.md); this file is what this
game added.

## The method that has worked

**Find the string, follow it two hops, read the parser.** Every format cracked
so far went the same way, and it took minutes rather than sessions:

1. `lb_code.py strings --pattern <something>` — every string with the functions
   that reference it from a literal pool.
2. The referencing function is usually a *request*, not the work. Follow its
   callback or its callee.
3. The parser itself is small and says everything: a count, a stride, a tag.

`BP/Entities.ebp` went from "not enough to write a parser" to 551 records with
named fields in about ten minutes this way. The prerequisite is the cross-
reference index, which takes 3.5 seconds to build over 9,160 functions.

**Build the index before you need it.** The instinct is to disassemble the
function you are curious about. The higher-yield move is to ask *who references
this address*, and that needs an index that already exists.

## Let the format prove itself

A parser that walks variable-length records either lands exactly where the next
section begins or it does not. Design every format tool so that it has such a
check and so that the check is *reported*:

- `COMP`: `header + table + Σ block sizes == file size`, and the sum of decoded
  block lengths equals the declared size. 2,178 of 2,178.
- `Entities.ebp`: the record walk starts at `0x04` and must land exactly on the
  string pool at `0x0FA08` — and 551 records × 2 string fields must reference
  exactly the 1,102 pool entries with none left over. Both hold.

Two independent invariants passing at once is very hard to achieve with a wrong
record size. One invariant is not enough: an off-by-one in a stride can still
sum correctly if a compensating error exists elsewhere.

## Pitfalls that actually bit

**Do not generalise from the first five files.** The `COMP` container's blocks
looked like a fixed 4 KB split in every file examined by hand. They are not:
2,125 of 2,178 use some other split. The block length is in each LZ11 header —
read it. *A sample chosen because it was convenient is not a sample.*

**A header field that is nearly always consistent is not an invariant.** `COMP`'s
fourth word tracks the largest packed block — except in two files where it is
four low. Enforcing it rejected two perfectly good files. Fields the decoder does
not *need* should be checked and reported, never enforced.

**Third-party writers do not follow the original packer's conventions.** The
community rebalance patch gives a 1,196-byte stream a 1,200-byte slot; the retail
packer would have given it 1,196. The invariant that actually holds is "the slot
is big enough", and the padding is slack to be reported.

**Adjacency in `.rodata` has a direction, and the natural guess is backwards.**
Each mission class's map filename sits *after* its typeinfo and vtable, so from
the next class's point of view it is *before*. Reading it as "the string before a
class belongs to that class" shifts all 90 rows by one and every row still looks
plausible. The check that catches it is a row you can verify independently.

**Class short names and display names are different name spaces, and they
collide.** A mission *titled* "Swarm" and a class named `…SwarmAct1` are not the
same mission. Only 18 of 90 class names match a title verbatim. Treating a name
match as positional evidence nearly produced a false contradiction between two
correct derivations.

**A category enum is not an index into the string list it displays.**
`UnitBlueprint+0x5C` has 21 values and `LOC` has 21 category strings, in a
different order. The tempting `LANG[242 + category]` reads plausibly for the
first entry (`0 → "Close Combat"`, and heroes do fight in melee) and is wrong for
every other. What settled it was the *roster*: six factions contributing exactly
one member each to a category is what a category means.

**Windows paths through a shell heredoc are destroyed.** `out\eur\arm9` became
`out\eur` + BEL + `rm9`, because `\a` and `\r` are escapes. This is the same trap
as C string literals and it is worse here because the result still looks like a
path. Write files with the editor tools; if a script must patch text, open with
`newline=''` and scan the result for control characters afterwards.

**Force UTF-8 on stdout before printing game text.** A `LOC` dump died on
`\x80` at line 15 of 3,064 under the Windows console codepage. One
`reconfigure(encoding="utf-8", errors="replace")` at the top of every tool.

**"Does it parse" is not the test. "Does it parse everything" is.** Records in
the map's chunks begin with `0x4C` and the chunk ends at `0x21`, so framing them
by scanning for the next `0x4C` is the obvious move. It works on 10 of 122 maps
by coincidence and fails on 112, because `EVNT` records are variable-length and
their payloads contain both bytes. Had the parser been tried on one file it
would have looked correct. **Run every format tool over the whole corpus on the
first run, not the tenth** — the failure rate is the finding.

**Prefer an invariant that holds by construction over one you scan for.** The
fix above was not a better scan: it was locating each chunk's end from the *next
chunk's tag* and asserting the byte before it is the terminator. That cannot be
fooled by payload bytes because it never looks at them.

**A format with two different terminators usually has a reason.** The map's
`TERR` chunk closes with its own tag reversed; the four record chunks close with
a `0x21` sentinel. That looks arbitrary until you read the loader: `TERR` is a
fixed size the loader computes and steps over, while the others are streams of
unknown length whose loop must test for an end each iteration. Reading the
inconsistency as a mistake would have meant missing the structure.

**Two address spaces can overlap, and a tool that merges them lies silently.**
The ARM9's DTCM and a 96 KB block the ARM7 copies both live at `0x027E0000`.
Nothing is wrong on hardware — DTCM is ARM9-private — but a single address-space
object answers ARM9 questions with ARM7 bytes and reports success. Map one core
at a time, and name sections per-CPU: calling `0x027E0000` "dtcm" in the ARM7's
table would be a comment that lies.

**A missing magic is not an absent structure.** The ARM7 has no `MODULE_PARAMS`
nitrocode anywhere, and the natural conclusion — "no autoload table, it all runs
where the header says" — is wrong by 99.7%. When a magic-based search comes back
empty, find the structure by its *shape* instead, and make the shape test strict
enough to be conclusive: for an autoload table, the entry sizes must sum to land
exactly on the table itself.

**A record's size is how many times its reader advances the cursor.** For a
straight-line deserialiser this is exact, takes seconds to count from the
disassembly, and needs no understanding of what the bytes mean. Check that the
advances all precede the first branch; if they do, the size is unconditional.
Five sub-record sizes in the map format came out this way, every one right first
time.

**Two subsystems can share a word and have nothing to do with each other.** The
map's `EVNT` ids are 24 even values and `Battles::EventBase` has exactly 22
subclasses, which reads as "the id selects the class". It does not: `EventBase`
is the UI and netplay event bus (`ButtonEvent`, `PenEvent`, `WiFiErrorEvent`)
and the map's records are `Battles::User::Map::OnEventData`. The constructor
stores one fixed vptr and the id is just an enum. **When a count matches, check
the mechanism, not the count** - one lookup settled it.

**Read the cheap open questions first.** `Sound/sound_data.sadl` sat on the
open-questions list for three working passes, already annotated as "49 KB of
text beginning `#def`". It is a C header of 1,311 developer symbol names, and
reading it took one `head -c 1400`. It then corrected four names this project
had inferred, confirmed the roster structure from a fourth independent
direction, and handed over the complete behaviour-event vocabulary. **An open
question whose cost is bounded and known should be spent before any question
that needs a disassembler.**

**A game that ships a build artefact will name things for you.** Look for
headers, `.h`, `#define`, symbol dumps and debug strings before inferring a
vocabulary. Names you derive from structure are usually right about the
*grouping* and wrong about the *words* - this project had the right twenty
roles and called four of them by names the developers did not use.

**A field that satisfies your discriminator is not thereby the field you
wanted.** `UnitBlueprint+0x15` is non-zero for all 90 mobile units and zero for
all 90 structures, and *no other single byte offset* has that property - which
is a real measurement and was taken as proof it was the movement speed. It is
not. `+0x0C` has the same property as a `u16`, and it is the one the speed
values actually live in. **"Non-zero exactly when a unit can move" is satisfied
by every per-unit attribute a structure does not have** - the discriminator
narrows the search, it does not finish it.

**An external table can be aligned by anchoring on what you already proved.**
The rebalance sheet's column order could not be read off its header row. It
could be pinned by matching two columns against fields already confirmed from
the ROM - Health at `+0x62`, Atk Interval at `+0x6D`, Range at `+0x6F` - and
then counting. That is a use of third-party data that stays honest: it never
introduces a fact, it only orders facts already established.

**A correlation over mostly-constant values is not evidence.** Matching an
external stat sheet against record offsets scored the `Size` column at 20 of 22
against `u8 +0x0D` - which is the *high byte of the u16 speed at +0x0C*. Speeds
run 273-956 so that byte is 1 for most units, and `Size` is 1.0 for most units.
Two unrelated mostly-ones columns agree at 91%% and mean nothing. The `mana`
column scored **lower**, 16 of 22, and is certain, because its values are varied
(405, 605, 700, 1000, 1200, 1750, 2000) and land on a category that makes sense.
**Score the variety of the matched values, not just the hit rate** - and be
suspicious when a match lands on a byte that is half of a wider field you have
already identified.

**Run a new instrument on a case where you already know the answer.** A
recursive-descent function scanner was written for the ARM7, which has no corpus
to check against. Running the *same* scanner over the ARM9, which has 9,312
known functions, measured it: tier 1 (direct calls only) is **99.6%% precise**;
adding tier 2 (code pointers in literal pools) drops it to **83.7%%**. The
tier-2 additions turn out to be epilogues and switch-table targets - only 22 of
388 unknown addresses even decode to a plausible opening instruction. Without
the ARM9 run, the ARM7 corpus would have looked like 334 solid functions.

**A metric that cannot exceed 100%% tells you when it is broken; one that can
does not.** The same scanner computed coverage by summing function sizes. On the
ARM9 that printed 188.3%% - impossible, so instantly visible. The identical bug
on the ARM7 had printed 77.8%% for a true 69.5%%, and nothing about that number
looked wrong. Prefer metrics with a hard ceiling, and compute set coverage from
merged intervals rather than a sum.

**Tag every result with the rule that produced it.** Recording the discovery
tier per function costs one column and means a consumer can take only the tier
they trust. It also means an unsound tier can be dropped later instead of
invalidating the whole corpus.

**Before counting how much is left to understand, find out how much is somebody
else's.** One SDK signature string - `[SDK+NINTENDO:BACKUP]` - was enough to ask
whether part of the ARM9 was library rather than game. Two histogram columns
answered it: a contiguous span of 1,354 functions with **0.0%% RTTI attribution
and 0.4%% Thumb**, against 31.5%% and 47.1%% everywhere else. Sixteen percent of
the ARM9 was never going to be interesting. Both signals collapsing at the same
boundary is what makes it solid - either alone would be suggestive.

**Look for the boundary between hand-written and prebuilt code early.** Game
code compiled for size looks different from a library compiled for speed:
instruction set, calling-convention density, the presence of C++ types. Those
differences are measurable in a histogram and they partition a binary for free.

**An external table's DPS column can prove three things at once.** The community
sheet's DPS reproduced exactly as `average_damage * 30 / attack_interval` on 19
of 22 units. That single identity settled which of two equally-plausible
readings of the melee damage pair was right, confirmed the projectile pair, and
fixed the attack interval's unit at 30 Hz - which is the game's simulation tick.
**A derived quantity constrains its inputs**; when a sheet gives you a computed
column, solve for the formula rather than trying to match the raw ones.

**A field whose valid range includes zero must be gated on whether it applies,
not on its value.** `"unlock_slot": u16(0x6C) or None` reported 288 unlockables
instead of 289, because slot 0 is a real slot. It was caught only because a
contiguity check had already established the answer was 289. Write the check
before the convenience.

## Things about this game specifically

**The ARM9 ships its RTTI.** 512 class names, the inheritance graph, and 2,454
virtual functions with their ARM/Thumb bit. Check for this on any DS title before
doing anything else — scan for `0xDEC00621`/`0x2106C0DE` for `MODULE_PARAMS` and
for nested mangled names (`N…E`) for RTTI. It reframes the whole project.

**Almost all of the game's data is behind one container.** Nothing could be read
until `COMP` was decoded, and everything could be read afterwards. Find the
wrapper first.

**The overlays are not engine code.** All 91 load at the same address,
`0x0216A2E0`, so they are mutually exclusive, and each is one mission's script.
The ARM9 static module holds the entire engine. Do not spend time on overlays
early.

**The overlays do not reference the mission classes.** An attempt to identify
overlay↔mission by scanning overlays for the mission vtable addresses found zero
matches. The direction is ARM9 → overlay, not the reverse. Worth knowing before
running the same search again.

**The two regions share a filesystem.** EUR and USA have byte-identical FNT and
FAT offsets and sizes, so any file-level work transfers between them unchanged.
Only the ARM9 differs.

## Token and session economy

- **Write the tool, not the answer.** Every question asked more than once —
  "what references X", "what is at address Y", "what does this record say" —
  became a subcommand. `lb_code.py`, `lb_blueprints.py` and `lb_lang.py` all
  started as one-off scripts that were asked twice.
- **Print the check, not the data.** `2178 decoded, 0 failed` is one line and
  more informative than any listing.
- **A histogram before a hexdump.** Profiling every byte offset across 182
  records (distinct/min/max per offset) located the whole stat block in one
  command; reading records by hand would have taken many.
- **Diff two versions of a file when one exists.** The rebalance patch localised
  every tunable field in `Entities.ebp` to 152 clusters before any code was read,
  and later named four fields at once by *how they changed together*.
