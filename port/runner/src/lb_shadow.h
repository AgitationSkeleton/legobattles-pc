// lb_shadow.h - grade a reimplemented function against the cartridge's own.
//
// THE PROBLEM THIS SOLVES, stated in docs/332: `tools/lb_match.py` marks a
// function done when GCC reproduces the ROM's bytes, and the ROM was built
// with mwccarm, which is not on this machine. Every function that has ever
// matched is <= 12 bytes. A native port does not need byte equality -- but the
// moment byte equality stops being the goal the project has no progress metric
// at all, which is how "11.5% decompiled" came to be quoted for months.
//
// THE REPLACEMENT METRIC IS BEHAVIOURAL EQUIVALENCE ON A REPLAYED SESSION.
// The runner already replays a recorded route deterministically. Run it twice
// -- once with the cartridge's function, once with the C reimplementation
// standing in its place -- and hash main RAM at fixed frames in both. If every
// hash agrees, the reimplementation is indistinguishable from the original on
// every path that run took, for every value it wrote anywhere in 4 MB.
//
// WHY NOT CALL BOTH AND COMPARE THE RETURN VALUE, which is the obvious design.
// Because a recompiled body does not return: its epilogue sets R[15] from LR
// and dispatches onward, so an override may call `original()` exactly once, as
// a true tail call, and anything written after it runs at slice end with the
// CPU somewhere else. Two implementations of an earlier feature died on that
// and the symptoms read as memory corruption. `runtime_call_push_return` can
// call a leaf out of turn -- func_02016A00 is a leaf -- but doing it from the
// same address the dispatcher is already inside lands on the body's entry
// guard. Trace-level differencing needs neither.
//
// WHAT IT DOES NOT PROVE, said once so it is not claimed later: only the paths
// the replay took are covered; a hash that never diverges says nothing about
// an input the run never supplied; and two implementations wrong in the same
// way agree.

#ifndef LB_SHADOW_H
#define LB_SHADOW_H

#include <cstdint>

// LB_SHADOW selects which implementation runs.
//
//   unset / 0 / "guest"   the cartridge's own code (the control arm)
//   "native"              the reimplementation in src/battles/ stands in
//   "verify"              the cartridge runs AND is compared (see below)
//
// Anything else is refused loudly rather than silently treated as one of them:
// a typo that quietly selects the control arm makes an A/B compare a run with
// itself, which is the shape of several of this project's retracted results.
//
// WHY "verify" EXISTS AND "native" IS NOT THE GRADING ARM. docs/333: ndsrecomp
// accounts emulated cycles per LIFTED INSTRUCTION, so a host replacement costs
// zero cycles where the cartridge's costs sixty-odd. The emulated clock runs
// ahead, the scheduler crosses timer boundaries at different instants, and a
// trace hash taken at a fixed frame catches counters mid-update. The first
// grading pass diverged at 4 of 136 samples for exactly that reason and it
// read like a failed reimplementation -- it was not: the divergences healed
// and the call counts were identical.
//
// "verify" removes the confound by never replacing anything. The override
// records the guest's own argument and tail-calls the cartridge, so both the
// behaviour and the cycle cost are the shipped ones; the comparison happens
// afterwards, out of turn, from the vblank hook.
enum LbShadowMode {
    kLbShadowGuest = 0,
    kLbShadowNative = 1,
    kLbShadowVerify = 2,
};
int lb_shadow_mode(void);

// One graded call: the argument, what the cartridge returned for it, and what
// src/battles/ returned for the same argument. Counted per function, with the
// first disagreement kept whole.
// TWO WORDS IN, AND `resc` WORDS OUT. func_0211FDB4 is the signed divide and
// returns the quotient in R0 and the remainder in R1; comparing only R0 would
// pass a reimplementation whose remainder was wrong at every call.
//
// `resc` IS NOT OPTIONAL AND ZERO-FILLING IS NOT A SUBSTITUTE. Comparing two
// words unconditionally reported Battles_RegistryFind as disagreeing on 635 of
// 635 calls: R0 matched every time and R1 held 0215111C -- the singleton
// address the cartridge loads into r1 as SCRATCH -- against the zero the
// reimplementation had not written. A register the function does not return is
// not a result, and reading one is how a correct implementation fails a test.
void lb_shadow_compare(const char* name, unsigned resc,
                       uint32_t a0, uint32_t a1,
                       uint32_t g0, uint32_t g1,
                       uint32_t n0, uint32_t n1);

// The out-of-turn call did not complete, so there is nothing to compare. This
// is counted rather than ignored: a verify run whose every call was refused
// reports zero disagreements, which would read as a pass.
void lb_shadow_note_refused(const char* name);

// Counted per registered function so a run can say whether the arm it claims
// to be testing actually executed. A behavioural A/B where the native arm was
// never entered produces identical hashes and looks like a pass.
void lb_shadow_note(const char* name, int native);

// LB_RAMSUM=N hashes main RAM every N frames and prints it. Off when unset.
// Called from lb_patches_on_vblank, on the emulation thread, with the guest
// between frames.
extern "C" void lb_shadow_on_vblank(void);

// Printed at exit and every 3,600 frames: which functions ran, on which arm,
// how many times.
void lb_shadow_report(void);

// --- the reimplementations, compiled from src/battles/ -----------------------
extern "C" {
uint32_t Battles_RegistryFind(uint32_t key);   // func_02016A00
// func_0211FDB4. Quotient and remainder both, because the ROM returns both.
void Battles_DivMod(uint32_t a, uint32_t b, uint32_t* q, uint32_t* r);
// src/battles/leaf.c
uint32_t Battles_SplitArrayElem(uint32_t obj, uint32_t index);  // func_020EF1E0
uint32_t Battles_ListFindByKey(uint32_t obj, uint32_t key);     // func_0206AC2C
uint32_t Battles_IndirectU16IsSet(uint32_t obj);                // func_0205E638
// src/battles/softfloat.c -- raw binary32 words in and out.
uint32_t Battles_FAdd(uint32_t a, uint32_t b);   // func_0211F324
uint32_t Battles_FSub(uint32_t a, uint32_t b);   // func_0211F934
uint32_t Battles_FMul(uint32_t a, uint32_t b);   // func_0211F748
}

#endif  // LB_SHADOW_H
