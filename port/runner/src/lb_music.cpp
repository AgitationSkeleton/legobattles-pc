// lb_music.cpp - choose which faction's music a match plays.
//
// Part of legobattles-decomp. See port/runner/README.md.
//
// GOAL THIS SERVES. The music ImGui settings must be wired and work properly;
// the Free Play music choice also applies to multiplayer game music, and the
// faction-based setting is determined by the builder. The Music tab has had a four-way Mode combo since docs/98 and it
// was marked READ -- the setting was stored, saved and reloaded, and nothing
// consumed it, because the one place that chooses a track had not been found.
//
// IT IS FOUND NOW, and this file is the whole of what that took.
//
// WHERE THE GAME CHOOSES ITS MUSIC
//
// docs/98 enumerated all 98 STRM entries and their faction sets, then failed to
// find the selection site through six separate searches: no STRM_ name strings
// in the ARM9 or any overlay; no table of the six faction base ids (4, 18, 32,
// 49, 66, 80) as u8, u16 or u32; not at any stride from 1 to 64; not in the map
// or mission tables; the most likely play entry point never executing; and the
// FIFO carrying a pointer rather than a value.
//
// Every one of those is consistent with what is actually there, which is why
// they all failed. The bases are IMMEDIATES INSIDE A NESTED JUMP TABLE:
//
//     func_02089c38(r0 = side, r1 = kind)         a pure function, returns r0
//         side 0..5, kind 0..4, and every arm is  `movs r0, #imm; bx lr`
//
//         side 0   4  10  16  17        KING
//         side 1  18  24  30  31        WIZARD
//         side 2  66  72  78  79        PIRATE
//         side 3  80  87  96  97        IMPERIAL
//         side 4  32  38  47  48        EARTH
//         side 5  49  55  64  65        ALIEN
//
//     kind 0 = campaign (the calm track), 1 = battle, 2 = victory, 3 = defeat
//
// Every base matches docs/98's SDAT table exactly, including its two
// irregularities: IMPERIAL has seven campaign tracks (80..86) and nine battle
// (87..95), which is why its victory lands on 96 rather than 92. Two readings
// taken from different parts of the ROM for different reasons agreeing on all
// twenty-four numbers is what makes this a reading rather than a guess.
//
// A search for the bases as data could never have found them: `movs r0, #0x42`
// puts 0x42 in the low byte of a 16-bit instruction, next to an opcode, not in
// a table.
//
// AND WHERE THE SIDE COMES FROM
//
//     0x0214B41C + 0x0C   the SIDE, 0..5     written by func_020898e8
//     0x0214B41C + 0x08   the KIND, 0..4     written by func_02089908
//
// func_02089974 reads both and calls func_02089c38 with them. So the single
// number that decides which faction's music plays is one word in main RAM, and
// the mode switch is a write to it.
//
// func_020898e8 is six instructions and this file mirrors it exactly rather
// than approximating it:
//
//     if (side != state[0x0C]) {
//         state[0x0C] = side;
//         rt[0x0C] = rt[0x10] = rt[0x18] = 0;   // force a re-pick
//     }
//
// Without those three clears the state block still holds the previous side's
// bookkeeping and the track already playing runs to its end before anything
// changes. With them the next update picks up the new side.
//
// WHY WRITING RAM IS SAFE HERE, given external-poke-loses-the-race
//
// That note is about fields the guest rewrites every frame. This one is not:
// the only writer is func_020898e8, and its only caller chain runs from scene
// transitions -- lobbies, faction select, match start. Between transitions the
// word is untouched, so a write from here stands. The tick below is idempotent
// and re-asserts every frame anyway, so a transition that overwrites it is
// corrected on the next.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "runtime_arm.h"
}

#include "bus.h"
#include "state.h"
#include "lb_overlay.h"

extern "C" bool lb_field_entity_at(int index, int* owner, int* category,
                                   int* map_x, int* map_y);
extern "C" bool lb_field_entity_blueprint(int index, uint32_t* record);
extern "C" bool lb_field_camera(int* map_x, int* map_y);
extern "C" unsigned int lb_patches_blueprint_base(void);
extern "C" int lb_demo_play_music(void);   /* src/lb_demo.cpp, docs/388 */

namespace {

// The music state block and its runtime companion, both named above.
const uint32_t kMusicState = 0x0214B41Cu;   // +0x08 kind, +0x0C side
const uint32_t kMusicRt    = 0x02155494u;   // +0x0C, +0x10, +0x18

// Blueprint records: base + 4 + index * 0x7C, twenty per faction in the order
// King, Wizard, Pirate, Imperial, Earth, Alien -- which is the SAME order as
// func_02089c38's sides. That is the second independent confirmation of the
// order: out/eur/entities.json labels every record with a `faction`, and the
// six builders (category 1) sit at indices 2, 22, 42, 62, 82 and 102, one per
// faction, twenty apart.
const uint32_t kRecordStride = 0x7Cu;
const int kBlueprintsPerFaction = 20;
const int kSides = 6;
const int kCategoryBuilder = 1;

// WHOSE builder. Measured rather than assumed: in a Free Play match the port's
// own entity log reports the player's hero as owner=0 and the opponent's as
// owner=1, and a tap on the owner=0 unit selects it while a tap on the other
// does not.
//
// 0 is only the starting value now. In multiplayer the joining client is a
// different index, and `latch_local_owner` below works out which from the
// opening camera -- verified in a real two-player match, where instance 1
// logged "local player is owner 1" and instance 0 stayed at 0.
int g_local_owner = 0;

int g_forced_side = -1;      // what this file last wrote, -1 = nothing
unsigned g_random_seed = 0;  // stable within a match, see pick_random

// The WORLD, from the tileset the level actually loaded.
//
// Castle, pirate and space are the three worlds of the world setting, and
// docs/data/maps.tsv says every one of the 122 maps uses exactly one of three
// tilesets: KingTileset (41 maps), PirateTileset (41) and MarsTileset (40).
// So the world IS the tileset, and the tileset is loaded by name.
//
// The names appear three times over in main RAM: in a static string table at
// about 0x0214BA20, in the front end's asset lists around 0x02176000, and --
// once, for the one that is actually loaded -- in the heap above 0x02300000.
// Only the last of those moves with the level, so only the last answers the
// question. Measured in a Free Play match on a castle map: "KingTileset" at
// 0x023DDB24 and 0x023DFD50, with neither of the other two anywhere above
// 0x02300000.
//
// Scanned once per level, not per frame.
const uint32_t kHeapLo = 0x02300000u;
const uint32_t kHeapHi = 0x02400000u;
const char* const kTilesetNames[3] = {
    "KingTileset", "PirateTileset", "MarsTileset",
};
// World -> the side whose music that world's campaign uses. The pairs follow
// func_02089c38's own order: sides 0,1 are the castle factions, 2,3 the pirate
// ones and 4,5 the space ones, which is also the order entities.json lists
// them in. Taking the FIRST of each pair is a choice, not a reading -- the
// cartridge has no "the castle world's music" to read, only two castle
// factions -- and it is the choice that keeps a castle map sounding like the
// castle campaign.
const int kWorldSide[3] = {0, 2, 4};

int g_world = -1;            // 0 castle, 1 pirate, 2 space; -1 unknown
uint32_t g_world_map = 0;    // the map pointer the world was scanned for

bool main_ram(uint32_t p) { return p >= 0x02000000u && p < 0x02400000u; }

// docs/388: a replay plays the recorder's Music mode when it has one.
int mode() {
    const int played = lb_demo_play_music();
    return played >= 0 ? played : lb_setting_music_mode();
}

// IS A BATTLE ACTUALLY UP?
//
// Not "does the map pointer look like a main-RAM address". lb_widemap.cpp
// records that in the menus "the view and map pointers are stale but still look
// like main-RAM addresses", and that mattered here: the front end runs with
// side 6, which is outside func_02089c38's 0..5 and is its own music path.
// Overriding a menu's side would replace the front-end theme with a faction
// track, which is not what any of these modes offers.
//
// The entity pool is the honest test. A menu has no entities; a battle has
// dozens. It is also the pool this file already walks to find the builder, so
// it costs two reads and no new address.
bool battle_up() {
    const uint32_t pool = bus_read_u32(0x02155028u);
    if (!main_ram(pool)) return false;
    const uint32_t arr = bus_read_u32(pool + 8u);
    const int32_t count = (int32_t)bus_read_u32(pool + 0xCu);
    return main_ram(arr) && count > 0 && count <= 4096;
}

// Find the local player's BUILDER and read its faction off its blueprint.
//
// The faction-based setting is determined by the builder, and the builder is
// the right unit to key off: there is exactly
// one builder blueprint per faction (category 1, indices 2/22/42/62/82/102),
// every side has one, and a match always has one on the field. A hero would
// work equally well in Free Play and not in a campaign mission that starts
// without one.
// WHICH PLAYER IS THIS CLIENT? Latched once per match from the opening camera.
//
// `lb_music_set_local_owner` exists for exactly this and was called by nothing,
// so faction mode keyed off owner 0 -- correct in Free Play and the campaign,
// and wrong for a joining client, which would hear its OPPONENT's faction.
//
// The net layer cannot supply it: this port bridges local wireless and the
// CARTRIDGE assigns the slots, so nothing on the host side ever learns the
// game's own index.
//
// The opening camera does. Measured in a real two-player match:
//
//   instance 0   cam (  88,144)   owner 0's base at sx=123   -> player 0
//   instance 1   cam (1072,672)   owner 1's base at sx=123   -> player 1
//
// Both start centred on their OWN base, which is how every RTS opens a match.
// So the local owner is the owner whose base is nearest the camera, and it is
// latched on the FIRST frame a battle is up -- before the player can move the
// view and make the question ambiguous.
//
// Only bases (category 7) count. They do not move, so unlike a unit they
// cannot wander toward the other player's start and change the answer.
// THE GAME'S OWN ANSWER FIRST (docs/385). The camera heuristic was wrong in
// a Free Play run, twice: "local player is owner 1 (its base is
// nearest the opening camera)", and faction-aligned music played the ENEMY's
// Wizard, then Alien, until a later latch put it back to Earth. The cartridge
// knows which player is local: its session block (0x02156A24) holds the local
// player's id at +0x64, and the player list (*0x0215540C: +8 array, +0xC
// count) holds each player's id at +0x10 -- FUN_02034B44's own gate reads
// them (lb_widemap.cpp, gate_open). The list index is the owner. It is
// accepted only when that owner has a base on the field; otherwise the camera
// decides as before.
int game_local_owner() {
    constexpr uint32_t kSession = 0x02156A24u;
    const uint32_t mgr = bus_read_u32(0x0215540Cu);
    if (!main_ram(mgr)) return -1;
    const uint32_t id = bus_read_u32(kSession + 0x64u);
    const int32_t count = (int32_t)bus_read_u32(mgr + 0xCu);
    const uint32_t arr = bus_read_u32(mgr + 8u);
    if (!main_ram(arr) || count <= 0 || count > 8) return -1;
    int found = -1;
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t p = bus_read_u32(arr + 4u * (uint32_t)i);
        if (main_ram(p) && bus_read_u32(p + 0x10u) == id) {
            if (found >= 0) return -1;   // two players with one id: no answer
            found = (int)i;
        }
    }
    if (found < 0) return -1;
    int owner = 0, cat = 0;
    for (int i = 0; lb_field_entity_at(i, &owner, &cat, nullptr, nullptr); ++i)
        if (cat == 7 && owner == found) return found;
    return -1;
}

void latch_local_owner(uint32_t map) {
    static uint32_t latched_for = 0;
    if (map == latched_for || !map) return;
    const int game = game_local_owner();
    if (game >= 0) {
        latched_for = map;
        // Once a match, whether or not it changed: which source decided is
        // what a log reader needs to tell this path from the camera's.
        std::fprintf(stderr, "[lb_music] local player is owner %d (the "
                     "game's own local player id); was %d\n", game,
                     g_local_owner);
        std::fflush(stderr);
        g_local_owner = game;
        return;
    }
    int cx = 0, cy = 0;
    if (!lb_field_camera(&cx, &cy)) return;
    int best = -1;
    long best_d = 0;
    int owner = 0, cat = 0, ex = 0, ey = 0;
    for (int i = 0; lb_field_entity_at(i, &owner, &cat, &ex, &ey); ++i) {
        if (cat != 7 || owner < 0 || owner >= 8) continue;
        const long dx = (long)ex - cx, dy = (long)ey - cy;
        const long d = dx * dx + dy * dy;
        if (best < 0 || d < best_d) { best = owner; best_d = d; }
    }
    if (best < 0) return;              // no base yet; try again next frame
    latched_for = map;
    std::fprintf(stderr,
                 "[lb_music] local player is owner %d (its base is nearest "
                 "the opening camera); was %d\n", best, g_local_owner);
    std::fflush(stderr);
    g_local_owner = best;
}

int side_from_builder() {
    const uint32_t base = lb_patches_blueprint_base();
    if (!main_ram(base)) return -1;
    int owner = 0, cat = 0;
    for (int i = 0; lb_field_entity_at(i, &owner, &cat, nullptr, nullptr); ++i) {
        if (owner != g_local_owner || cat != kCategoryBuilder) continue;
        uint32_t rec = 0;
        if (!lb_field_entity_blueprint(i, &rec) || !main_ram(rec)) continue;
        if (rec < base + 4u) continue;
        const uint32_t idx = (rec - base - 4u) / kRecordStride;
        const int side = (int)idx / kBlueprintsPerFaction;
        // SAY WHOSE BUILDER, AND WHICH ONE.
        //
        // "Faction-aligned -> side 4" alone cannot be checked by a reader: it
        // does not say which player's builder was read, and in a two-player
        // match where both happen to pick the same faction it looks identical
        // whether the local-owner latch worked or not. Printing the owner and
        // the blueprint index makes the chain legible end to end -- owner 1,
        // blueprint 82, side 4 -- and it is what turns "both clients logged
        // Earth" from an ambiguous result into a stated one.
        //
        // Once per match, and only when something changed.
        {
            static uint32_t said_for = 0;
            static int said_owner = -1, said_idx = -1;
            const uint32_t map = bus_read_u32(0x02156CB0u);
            if (map != said_for || g_local_owner != said_owner ||
                (int)idx != said_idx) {
                said_for = map;
                said_owner = g_local_owner;
                said_idx = (int)idx;
                std::fprintf(stderr,
                             "[lb_music] owner %d's builder is blueprint %u "
                             "-> side %d\n",
                             g_local_owner, unsigned(idx), side);
                std::fflush(stderr);
            }
        }
        if (side >= 0 && side < kSides) return side;
    }
    return -1;
}

// Which of the three tilesets is loaded. See the note on kTilesetNames.
int scan_world() {
    BusRegion ram{};
    if (!bus_get_region("mainram", &ram) || !ram.ptr) return -1;
    const uint32_t lo = kHeapLo - 0x02000000u;
    const uint32_t hi = kHeapHi - 0x02000000u;
    if (hi > ram.len) return -1;
    for (int w = 0; w < 3; ++w) {
        const char* const name = kTilesetNames[w];
        const size_t n = std::strlen(name);
        for (uint32_t off = lo; off + n <= hi; ++off)
            if (std::memcmp(ram.ptr + off, name, n) == 0) return w;
    }
    return -1;
}

// A per-match pick that does not move while the match runs.
//
// Two requirements pull against each other. Re-rolling every frame would
// restart the track every frame, so the pick has to be STABLE within a match.
// But seeding it from the map alone makes the same map play the same "random"
// side forever, which is not random at all -- measured, and it is why the
// counter is here.
//
// A match counter satisfies both: it advances once when a level appears, so it
// is constant while that level is up and different for the next one. It is
// also a plain count rather than a clock, so a harness run is still
// reproducible -- the nth match of a run always sounds the same, which is what
// makes a regression visible.
unsigned g_match_seq = 0;
uint32_t g_last_map = 0;

int pick_random(uint32_t map) {
    if (map != g_last_map) {
        g_last_map = map;
        if (map) ++g_match_seq;
        uint32_t h = map ^ (g_match_seq * 0x9E3779B9u);
        h ^= h >> 15; h *= 0x2C1B3C6Du;
        h ^= h >> 12; h *= 0x297A2D39u;
        h ^= h >> 15;
        g_random_seed = h ? h : 1u;
    }
    return (int)(g_random_seed % (unsigned)kSides);
}

}  // namespace

// See lb_overlay.h. The multiplayer layer sets this once it knows which player
// the local client is; until then it is 0, which is the local player in Free
// Play and in the campaign.
extern "C" void lb_music_set_local_owner(int owner) {
    if (owner >= 0 && owner < 8) g_local_owner = owner;
}

// Which side this port wants, or -1 to leave the game entirely alone.
extern "C" int lb_music_wanted_side(void) {
    const int m = mode();
    if (m <= 0) return -1;                       // Vanilla: do not interfere

    const uint32_t map = bus_read_u32(0x02156CB0u);
    if (!main_ram(map) || !battle_up()) return -1;

    latch_local_owner(map);
    // docs/387: the music override setting has Wizard/Pirate/Imperial/Earth/
    // Alien options that always pick a preference regardless of the map.
    // Modes 4..9 are the six sides in func_02089c38's order, King
    // included: "Vanilla" plays King's only in Free Play and multiplayer, and
    // a campaign level has its own.
    if (m >= 4 && m < 4 + kSides) return m - 4;
    if (m == 1) return pick_random(map);
    if (m == 2) return side_from_builder();
    if (m == 3) {
        if (g_world < 0 || g_world_map != map) {
            g_world = scan_world();
            g_world_map = map;
        }
        return (g_world >= 0 && g_world < 3) ? kWorldSide[g_world] : -1;
    }
    return -1;
}

// Once per frame, from lb_patches_on_vblank.
extern "C" void lb_music_tick(void) {
    static int last_mode = -1;
    const int m = mode();
    if (m != last_mode) {
        last_mode = m;
        // Leaving a non-vanilla mode does not put the previous side back --
        // there is nothing to put back, because the game's own value was
        // overwritten rather than saved. It re-asserts itself at the next
        // transition, which is the same moment it would have changed anyway.
        g_forced_side = -1;
        g_world = -1;
        g_world_map = 0;
    }
    if (m <= 0) return;

    const int want = lb_music_wanted_side();
    if (want < 0 || want >= kSides) return;

    const uint32_t cur = bus_read_u32(kMusicState + 0x0Cu);
    if ((int)cur == want) return;

    // func_020898e8, exactly: set the side, then clear the three runtime words
    // that make the next update re-pick a track instead of letting the current
    // one play out.
    bus_write_u32(kMusicState + 0x0Cu, (uint32_t)want);
    bus_write_u32(kMusicRt + 0x0Cu, 0u);
    bus_write_u32(kMusicRt + 0x10u, 0u);
    bus_write_u32(kMusicRt + 0x18u, 0u);

    if (g_forced_side != want) {
        g_forced_side = want;
        static const char* const kSideName[kSides] = {
            "King", "Wizard", "Pirate", "Imperial", "Earth", "Alien"};
        static const char* const kModeName[4] = {
            "Vanilla", "Random", "Faction-aligned", "Map-specific"};
        std::fprintf(stderr,
                     "[lb_music] %s -> side %d (%s), was %u\n",
                     m < 4 ? kModeName[m] : "Always this faction", want,
                     kSideName[want], unsigned(cur));
        std::fflush(stderr);
    }
}

// What the game is playing right now, for the UI and for the log. `side` and
// `kind` are the two words func_02089974 reads; `strm` is the base id
// func_02089c38 maps them to, reproduced here from the same table so the panel
// can name the track set without breaking into the guest.
extern "C" void lb_music_state(int* side, int* kind, int* strm) {
    const int s = (int)bus_read_u32(kMusicState + 0x0Cu);
    const int k = (int)bus_read_u32(kMusicState + 0x08u);
    if (side) *side = s;
    if (kind) *kind = k;
    if (!strm) return;
    // func_02089c38's table, read out of the cartridge (see the file header).
    static const int kBase[kSides][4] = {
        {  4,  10, 16, 17 },   // KING
        { 18,  24, 30, 31 },   // WIZARD
        { 66,  72, 78, 79 },   // PIRATE
        { 80,  87, 96, 97 },   // IMPERIAL
        { 32,  38, 47, 48 },   // EARTH
        { 49,  55, 64, 65 },   // ALIEN
    };
    *strm = (s >= 0 && s < kSides && k >= 0 && k < 4) ? kBase[s][k] : -1;
}
