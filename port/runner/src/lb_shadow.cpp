// lb_shadow.cpp - the bridge between src/battles/ and ndsrecomp's bus, the
// arm selector, and the main-RAM hash that grades an A/B replay.
//
// See lb_shadow.h for why the metric is a trace hash rather than a per-call
// comparison. This file holds only mechanism.

#include "lb_shadow.h"

#include "runtime_arm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// --- the address space src/battles/mem.h reads through -----------------------
//
// NOT INLINE, and not a raw pointer into the bus window. bus_read_u32 has a
// fast path that memcpys out of a classified window and a slow path for
// everything else, and which one applies depends on the address; taking a
// pointer to the window would be correct until the first read that is not in
// one. The reimplementation pays a call per load in this build and pays
// nothing in the native build, where the same macro is a plain load.
extern "C" uint32_t battles_read32(uint32_t addr) { return bus_read_u32(addr); }
extern "C" uint16_t battles_read16(uint32_t addr) { return bus_read_u16(addr); }
extern "C" uint8_t  battles_read8(uint32_t addr)  { return bus_read_u8(addr); }

namespace {

// Per-function call counts, one row per registered reimplementation. Small and
// fixed: a table that can grow is a table that needs a lock, and this is only
// ever written from the emulation thread.
struct ShadowRow {
    const char* name;
    unsigned guest;
    unsigned native;
    // verify mode
    unsigned graded;      // calls compared against the cartridge
    unsigned disagreed;
    unsigned refused;     // the out-of-turn call did not complete
    // IS THE ORACLE VACUOUS? 603 agreements mean nothing if every one of them
    // was `0 == 0`: a stub that returned 0 unconditionally would score the
    // same. `nonzero` counts graded calls where the cartridge found a node,
    // and `keys`/`results` count how many distinct arguments and answers the
    // sample contained. A pass needs all three to be substantial.
    unsigned nonzero;
    // A0 AND A1 SEPARATELY, NOT `a0 ^ a1`. The XOR collapses a pair and
    // collides: Battles_FAdd reported `keys>=5` over 11,358 graded calls,
    // which is either "the game adds the same five pairs all session" or "the
    // instrument cannot tell those pairs apart", and one of those is a pass
    // and the other is five test cases wearing a big number.
    uint32_t key_set[32];
    unsigned key_n;
    uint32_t key1_set[32];
    unsigned key1_n;
    uint32_t res_set[32];
    unsigned res_n;
    bool     have_first;
    uint32_t first_arg, first_guest, first_native;
    uint32_t first_arg1, first_guest1, first_native1;
};
// 24, not 8: each swept function gets a SECOND row under a "/sweep" name, so
// synthetic coverage is reported beside live coverage instead of being added
// to it. A sweep's thousands of chosen pairs would otherwise drown the handful
// the route actually produced, and "graded" would stop meaning "the game did
// this".
ShadowRow g_rows[24];
unsigned g_row_count = 0;

ShadowRow* row_for(const char* name) {
    for (unsigned i = 0; i < g_row_count; ++i)
        if (g_rows[i].name == name) return &g_rows[i];
    if (g_row_count >= (sizeof g_rows / sizeof g_rows[0])) return nullptr;
    ShadowRow* r = &g_rows[g_row_count++];
    r->name = name;
    r->guest = r->native = 0u;
    r->graded = r->disagreed = r->refused = 0u;
    r->nonzero = r->key_n = r->key1_n = r->res_n = 0u;
    r->have_first = false;
    r->first_arg = r->first_guest = r->first_native = 0u;
    r->first_arg1 = r->first_guest1 = r->first_native1 = 0u;
    return r;
}

// Distinct-value tally, capped. Linear because the cap is 32 and this runs on
// a path already doing a guest dispatch; a set would cost more than it saves.
void tally(uint32_t* set, unsigned* n, unsigned cap, uint32_t v) {
    for (unsigned i = 0; i < *n; ++i)
        if (set[i] == v) return;
    if (*n < cap) set[(*n)++] = v;
}

}  // namespace

int lb_shadow_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char* v = std::getenv("LB_SHADOW");
        if (!v || !v[0] || std::strcmp(v, "0") == 0 ||
            std::strcmp(v, "guest") == 0) {
            mode = kLbShadowGuest;
        } else if (std::strcmp(v, "native") == 0 ||
                   std::strcmp(v, "1") == 0) {
            mode = kLbShadowNative;
            std::fprintf(stderr, "[lb_shadow] NATIVE arm: reimplementations in "
                                 "src/battles/ stand in for the cartridge\n");
            std::fflush(stderr);
        } else if (std::strcmp(v, "verify") == 0) {
            mode = kLbShadowVerify;
            std::fprintf(stderr, "[lb_shadow] VERIFY: the cartridge runs and "
                                 "is compared; nothing is replaced\n");
            std::fflush(stderr);
        } else {
            // REFUSED, NOT DEFAULTED. An unrecognised value silently selecting
            // the control arm would make an A/B compare one arm with itself,
            // and the two runs would agree perfectly.
            std::fprintf(stderr, "[lb_shadow] LB_SHADOW=\"%s\" is not "
                                 "\"guest\", \"native\" or \"verify\"; "
                                 "refusing to guess\n",
                         v);
            std::fflush(stderr);
            std::exit(2);
        }
    }
    return mode;
}

void lb_shadow_note(const char* name, int native) {
    ShadowRow* r = row_for(name);
    if (!r) return;
    if (native) ++r->native; else ++r->guest;
}

// THE ONE COMPARISON. `guest` came from the cartridge's own code, called out of
// turn on the same argument; `native` from src/battles/. Both are read AFTER
// the cartridge has already served the game its answer, so nothing here can
// change what the game does.
void lb_shadow_compare(const char* name, unsigned resc,
                       uint32_t a0, uint32_t a1,
                       uint32_t g0, uint32_t g1,
                       uint32_t n0, uint32_t n1) {
    ShadowRow* r = row_for(name);
    if (!r) return;
    // Words the function does not return are not compared and do not count
    // towards "found something" or the distinct-value tallies either.
    if (resc < 2u) { g1 = n1 = 0u; }
    ++r->graded;
    if (g0 != 0u || g1 != 0u) ++r->nonzero;
    tally(r->key_set, &r->key_n, 32u, a0);
    tally(r->key1_set, &r->key1_n, 32u, a1);
    tally(r->res_set, &r->res_n, 32u, g0 ^ g1);
    if (g0 == n0 && g1 == n1) return;
    ++r->disagreed;
    // THE FIRST EIGHT, NOT THE FIRST. One sample cannot tell "my
    // reimplementation is wrong" from "the probe returned something that is
    // not the function's result": Battles_ListFindByKey disagreed on 19 of
    // 3,742 calls and the one printed value, 00E8BD80, is an ARM `pop
    // {...,pc}` encoding rather than any plausible main-RAM node. Whether that
    // is the pattern or the exception decides which of the two it is, and it
    // takes more than one line to see.
    if (r->disagreed <= 8u) {
        std::fprintf(stderr,
                     "[lb_shadow] DISAGREE #%u %s(%08X,%08X): cartridge "
                     "%08X,%08X  native %08X,%08X\n",
                     r->disagreed, name, a0, a1, g0, g1, n0, n1);
        std::fflush(stderr);
    }
    if (!r->have_first) {
        r->have_first = true;
        r->first_arg = a0;   r->first_arg1 = a1;
        r->first_guest = g0; r->first_guest1 = g1;
        r->first_native = n0; r->first_native1 = n1;
    }
}

void lb_shadow_note_refused(const char* name) {
    ShadowRow* r = row_for(name);
    if (r) ++r->refused;
}

void lb_shadow_report(void) {
    if (g_row_count == 0u) {
        std::fprintf(stderr, "[lb_shadow] no registered function was called\n");
        std::fflush(stderr);
        return;
    }
    for (unsigned i = 0; i < g_row_count; ++i) {
        const ShadowRow& r = g_rows[i];
        std::fprintf(stderr,
                     "[lb_shadow] %-24s guest %u  native %u  graded %u  "
                     "disagreed %u  refused %u  | found %u  a0>=%u  "
                     "a1>=%u  results>=%u\n",
                     r.name, r.guest, r.native, r.graded, r.disagreed,
                     r.refused, r.nonzero, r.key_n, r.key1_n, r.res_n);
        // `keys>=` and `results>=` are capped at 32; reaching the cap means
        // "at least 32 distinct", not "exactly 32".
        if (r.have_first) {
            // BOTH WORDS. This line used to print only R0, so the one run
            // where the harness itself was wrong reported "cartridge 00000000
            // native 00000000" as a disagreement and read as nonsense. The
            // difference was in R1 and the line could not show it.
            std::fprintf(stderr,
                         "[lb_shadow]   first disagreement: %s(%08X,%08X) "
                         "cartridge %08X,%08X  native %08X,%08X\n",
                         r.name, r.first_arg, r.first_arg1,
                         r.first_guest, r.first_guest1,
                         r.first_native, r.first_native1);
        }
    }
    std::fflush(stderr);
}

// --- the trace hash ----------------------------------------------------------
//
// FNV-1a over main RAM, every LB_RAMSUM frames. Two runs of the same recorded
// route are deterministic, so a matching sequence of hashes says the two
// implementations left 4 MB byte-identical at every sampled frame.
//
// STRIDE 4 AND NOT COARSER. A stride that skips words would make the hash
// insensitive to exactly the small scattered writes a wrong lookup causes --
// one pointer stored in the wrong object -- which is the failure this is meant
// to catch. Whole RAM at a low sample rate is the right trade: at N=120 it is
// one megaword every two seconds.
extern "C" void lb_shadow_on_vblank(void) {
    static unsigned frame = 0u;
    const unsigned f = frame++;

    // THE REPORT IS NOT THE HASH'S PASSENGER. It used to print only from
    // inside the LB_RAMSUM branch, so a verify run -- which has no reason to
    // set LB_RAMSUM -- produced the banner and then nothing at all: no graded
    // count, no refusal count, no disagreement. A whole run's measurement was
    // invisible because an unrelated variable was unset.
    if ((f % 3600u) == 0u && f != 0u && lb_shadow_mode() != kLbShadowGuest)
        lb_shadow_report();

    static int period = -1;
    if (period < 0) {
        const char* v = std::getenv("LB_RAMSUM");
        period = (v && v[0]) ? std::atoi(v) : 0;
        if (period < 0) period = 0;
    }
    if (period == 0) return;
    if ((f % (unsigned)period) != 0u) return;

    uint64_t h = 1469598103934665603ull;
    for (uint32_t a = 0x02000000u; a < 0x02400000u; a += 4u) {
        h ^= (uint64_t)bus_read_u32(a);
        h *= 1099511628211ull;
    }
    std::fprintf(stderr, "[lb_ramsum] f=%u %016llx\n", f,
                 (unsigned long long)h);
    std::fflush(stderr);

    if ((f % 3600u) == 0u) lb_shadow_report();
}
