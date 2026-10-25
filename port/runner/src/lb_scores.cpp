// SG-0018: a match's post-match statistics, per team, for the high-score table.
//
// OBSERVE ONLY: this file only reads guest memory (bus_read_*), once per vblank,
// from lb_patches.cpp's vblank hook. Nothing is written and no guest code is
// called, so the emulation is untouched, and as the simulation is lockstep every
// machine of an online match reads identical numbers.
//
// The game keeps every statistic itself, for its own Stats screen after a match
// (Battles::User::StatsScreen, FUN_020C9FC8 builds the rows). One 0xA4-byte record
// per owner starts at 0x021571FC (the session at 0x021571A4, record 0 at +0x58);
// the match's time is a u64 at session +0xC. Rows, in the Stats screen's order
// (LOC strings 0x144..0x14F), and the record offsets they read:
//   Bricks Collected   u32 +0x14        Minifigures Built       u16 +0x4C
//   Specials Built     u16 +0x4E        Buildings Built         u16 +0x50
//   Minifigures Lost   u16 +0x52        Specials Lost           u16 +0x54
//   Buildings Lost     u16 +0x56        Minifigures Destroyed   u16 +0x58
//   Specials Destroyed u16 +0x5A        Buildings Destroyed     u16 +0x3E
// (FUN_020A7A58 counts built, FUN_020A7E78 lost, FUN_020A7C88 destroyed, each
// keyed by the unit category [[e+0x160]+8]+0x5C: 0-4 minifigures, 5/6 specials,
// 7+ buildings.) "Units" below are minifigures + specials.
//
// A record is indexed by the team's owner byte (team +0x14). The team pool
// (*0x0215540C: +8 team array, +0xC count) is gone once the level closes, so
// every frame of a level copies the records into a snapshot, and the snapshot
// of the last frame stays readable until the next match begins. Won / lost is
// the team's state word, team +0x9C (2 won, 1 / 3 lost, 4 allied win).
#include "lb_overlay.h"

extern "C" {
#include "runtime_arm.h"   // bus_read_u8 / u16 / u32
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kTeamPoolPtr = 0x0215540Cu;
constexpr uint32_t kRecords = 0x021571FCu;
constexpr uint32_t kGameSingleton = 0x021553D4u;
constexpr int kTeams = 4;

inline bool gp(uint32_t p) { return p >= 0x02000000u && p < 0x02400000u; }

struct Rec {
    int state;                          // team +0x9C's last non-zero value
    uint32_t bricks;
    uint32_t built_u, built_s, built_b;
    uint32_t lost_u, lost_s, lost_b;
    uint32_t kill_u, kill_s, kill_b;
};

Rec g_rec[kTeams];
int g_teams = 0;           // teams in the snapshot (0: none)
bool g_have = false;       // a level has been seen since the last begin
bool g_begin_pending = true;
uint32_t g_pool_seen = 0;
uint32_t g_frames = 0;     // vblanks in this level
uint32_t g_tick = 0;       // the game's simulation tick counter ([[0x021553D4+4]]+0x8B4)
bool g_frozen = false;     // the match is decided; the snapshot stays as it was
uint32_t g_decided_at = 0;
int g_log_mode = -1;

uint32_t team_at(uint32_t pool, uint32_t k) {
    const uint32_t arr = bus_read_u32(pool + 8u);
    if (!gp(arr)) return 0;
    const uint32_t t = bus_read_u32(arr + 4u * k);
    return gp(t) ? t : 0;
}

void reset_all() {
    std::memset(g_rec, 0, sizeof g_rec);
    g_teams = 0;
    g_have = true;
    g_frames = 0;
    g_tick = 0;
    g_frozen = false;
    g_decided_at = 0;
}

void print_stats(const char* tag) {
    for (int k = 0; k < g_teams; ++k) {
        const Rec& r = g_rec[k];
        std::fprintf(stderr,
                     "[lb_scores] %s team %d state %d: units built %u lost %u destroyed %u | "
                     "buildings built %u lost %u destroyed %u | bricks %u | %u s "
                     "(frames %u, ticks %u)\n",
                     tag, k, r.state, r.built_u + r.built_s, r.lost_u + r.lost_s,
                     r.kill_u + r.kill_s, r.built_b, r.lost_b, r.kill_b, r.bricks, g_tick / 30u,
                     g_frames, g_tick);
    }
    std::fflush(stderr);
}

}  // namespace

extern "C" void lb_scores_match_begin(void) {
    reset_all();
    g_begin_pending = false;
}

extern "C" int lb_scores_player_stats(int player, LbMatchStats* out) {
    if (!out) return 0;
    out->valid = 0;
    out->won = out->units_built = out->units_lost = out->units_killed = -1;
    out->buildings_built = out->buildings_lost = out->buildings_killed = -1;
    out->bricks = -1;
    if (!g_have || player < 0 || player >= g_teams) return 0;
    const Rec& r = g_rec[player];
    out->valid = 1;
    out->won = (r.state == 2 || r.state == 4) ? 1 : (r.state == 1 || r.state == 3) ? 0 : -1;
    out->units_built = (int)(r.built_u + r.built_s);
    out->units_lost = (int)(r.lost_u + r.lost_s);
    out->units_killed = (int)(r.kill_u + r.kill_s);
    out->buildings_built = (int)r.built_b;
    out->buildings_lost = (int)r.lost_b;
    out->buildings_killed = (int)r.kill_b;
    out->bricks = (int)r.bricks;
    return 1;
}

// The match length in seconds, or -1 when unknown. The game's logic runs 30
// ticks a second and a pause stops the counter. (The Stats screen's own time,
// session +0xC, is the OS clock summed at pause events; it stays 0 in these
// matches, so it is not used.)
extern "C" int lb_scores_match_seconds(void) {
    if (!g_have || !g_tick) return -1;
    return (int)(g_tick / 30u);
}

extern "C" void lb_scores_tick(void) {
    if (g_log_mode < 0) {
        const char* v = std::getenv("LB_SCORES_LOG");
        g_log_mode = v ? (std::atoi(v) ? std::atoi(v) : 1) : 0;
    }
    const uint32_t pool = bus_read_u32(kTeamPoolPtr);
    const uint32_t cnt = gp(pool) ? bus_read_u32(pool + 0xCu) : 0u;
    if (cnt < 1u || cnt > 8u || !gp(bus_read_u32(pool + 8u))) {   // no level
        g_pool_seen = 0;
        g_begin_pending = true;
        return;
    }
    if (g_begin_pending || pool != g_pool_seen) {   // a level came up: a new match
        reset_all();
        g_begin_pending = false;
    }
    g_pool_seen = pool;
    if (g_frozen) return;
    ++g_frames;
    g_teams = (int)(cnt > (uint32_t)kTeams ? (uint32_t)kTeams : cnt);

    bool decided = false;
    for (int k = 0; k < g_teams; ++k) {
        const uint32_t t = team_at(pool, (uint32_t)k);
        if (!t) continue;
        Rec& r = g_rec[k];
        const uint32_t st = bus_read_u32(t + 0x9Cu);
        if (st != 0u && st <= 4u) r.state = (int)st;
        if (r.state == 2 || r.state == 4) decided = true;
        const uint32_t owner = bus_read_u8(t + 0x14u);
        if (owner >= 4u) continue;
        const uint32_t a = kRecords + owner * 0xA4u;
        r.bricks = bus_read_u32(a + 0x14u);
        r.built_u = bus_read_u16(a + 0x4Cu);
        r.built_s = bus_read_u16(a + 0x4Eu);
        r.built_b = bus_read_u16(a + 0x50u);
        r.lost_u = bus_read_u16(a + 0x52u);
        r.lost_s = bus_read_u16(a + 0x54u);
        r.lost_b = bus_read_u16(a + 0x56u);
        r.kill_u = bus_read_u16(a + 0x58u);
        r.kill_s = bus_read_u16(a + 0x5Au);
        r.kill_b = bus_read_u16(a + 0x3Eu);
    }
    const uint32_t g = bus_read_u32(kGameSingleton + 4u);
    g_tick = gp(g) ? bus_read_u32(g + 0x8B4u) : 0u;

    // A team won (2 / 4), or every team is decided: the match is over. The
    // cartridge keeps counting (the AI keeps building after the win), so 30
    // frames on, for the other teams' words to settle, the snapshot is frozen.
    bool all_set = true;
    for (int k = 0; k < g_teams; ++k) all_set = all_set && g_rec[k].state != 0;
    if ((decided || all_set) && !g_decided_at) g_decided_at = g_frames;
    if (g_decided_at && g_frames - g_decided_at >= 30u) {
        g_frozen = true;
        if (g_log_mode) print_stats("FINAL");
        return;
    }
    if (g_log_mode && g_frames % 600u == 0u) print_stats("battle");
}
