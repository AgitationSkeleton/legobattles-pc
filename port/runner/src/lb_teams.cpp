// legobattles-decomp: Team Builder and Ban Units (docs/384).
//
// The feature: a "Team Builder"
// tab whose grid of unit icons mimics the Free Play army screen, with presets,
// Randomize, Set Team and Set Enemy Team, Custom and Random opponents in Free
// Play, and a "Ban Units" tab whose bans switch an offending online player to
// King as the match starts. The tabs themselves are in lb_overlay.cpp; this
// file is what they stand on:
//
//   * the ROM: NitroFS, the COMP/LZ11 wrapper, NCGR/NCLR, and the army
//     screen's own heads (UI/MiniHeads) with the army screen's own palettes;
//   * the unit table: each slot's candidates, in the order the army screen
//     lists them;
//   * the save's army record, its unlock state, and Set Team's refusals;
//   * the Custom/Random opponent's army, and the ban rules and their journey
//     from the host to the clients over the wireless bridge.
//
// Nothing here is cartridge DATA: images, palettes and names are read from the
// player's own ROM and from guest RAM at run time. What is written down is what
// the code says -- addresses, offsets, and the order the army screen's own rule
// produces -- each with where it was read (docs/384).

#include "lb_overlay.h"

#include <algorithm>   // docs/387: std::min in lb_rom_read
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "runtime_arm.h"
}

extern "C" unsigned int lb_patches_blueprint_base(void);   /* lb_patches.cpp */
extern "C" int lb_patches_blueprint_settled(void);         /* lb_patches.cpp */
extern "C" bool lb_field_entity_at(int index, int* owner, int* category,
                                   int* map_x, int* map_y);   /* lb_field.cpp */

namespace {

// ---- the player's ROM -------------------------------------------------------
//
// main.cpp reads the whole cartridge into one vector that lives for the
// session (the frontend loop runs inside main); apply_hooks hands it over the
// moment it is read. Only one ROM can run (port/game.toml's sha1 pin), so the
// EUR addresses below are exact.
const uint8_t* g_rom = nullptr;
size_t g_rom_size = 0;

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool ci_equal(const char* a, size_t na, const char* b, size_t nb) {
    if (na != nb) return false;
    for (size_t i = 0; i < na; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// A NitroFS path, e.g. "UI/MiniHeads.NCGR", to its bytes. FNT at header 0x40,
// FAT at 0x48; directory d's main entry is FNT + (d & 0xFFF) * 8: u32 subtable
// offset, u16 first file id. A subtable entry is u8 t (0 ends), t & 0x7F name
// bytes, and a u16 directory id when t & 0x80. Files number consecutively.
bool nitro_file(const char* path, std::vector<uint8_t>* out) {
    if (!g_rom || g_rom_size < 0x200) return false;
    const uint32_t fnt = rd32(g_rom + 0x40), fat = rd32(g_rom + 0x48);
    const uint32_t fat_size = rd32(g_rom + 0x4C);
    if (fnt >= g_rom_size || fat >= g_rom_size) return false;
    uint32_t dir = 0xF000u;
    const char* part = path;
    for (;;) {
        const char* slash = std::strchr(part, '/');
        const bool last = slash == nullptr;
        const size_t plen = last ? std::strlen(part) : (size_t)(slash - part);
        const uint32_t main_at = fnt + (dir & 0xFFFu) * 8u;
        if (main_at + 8u > g_rom_size) return false;
        uint32_t p = fnt + rd32(g_rom + main_at);
        uint32_t fid = rd16(g_rom + main_at + 4u);
        bool found = false;
        uint32_t found_id = 0;
        while (p < g_rom_size) {
            const uint8_t t = g_rom[p++];
            if (t == 0) break;
            const uint32_t n = t & 0x7Fu;
            if (p + n > g_rom_size) return false;
            const char* name = (const char*)(g_rom + p);
            p += n;
            if (t & 0x80u) {
                if (p + 2u > g_rom_size) return false;
                const uint16_t sub = rd16(g_rom + p);
                p += 2u;
                if (!last && ci_equal(name, n, part, plen)) {
                    found = true;
                    found_id = sub;
                    break;
                }
            } else {
                if (last && ci_equal(name, n, part, plen)) {
                    found = true;
                    found_id = fid;
                    break;
                }
                ++fid;
            }
        }
        if (!found) return false;
        if (!last) {
            dir = found_id;
            part = slash + 1;
            continue;
        }
        if ((found_id + 1u) * 8u > fat_size) return false;
        const uint32_t s = rd32(g_rom + fat + found_id * 8u);
        const uint32_t e = rd32(g_rom + fat + found_id * 8u + 4u);
        if (e < s || e > g_rom_size) return false;
        out->assign(g_rom + s, g_rom + e);
        return true;
    }
}

// Nintendo LZ11 (type byte 0x11). The three token forms by the high nibble.
bool lz11(const uint8_t* b, size_t n, size_t o, std::vector<uint8_t>* out) {
    if (o + 4u > n || b[o] != 0x11) return false;
    size_t size = rd32(b + o) >> 8;
    o += 4u;
    if (size == 0) {
        if (o + 4u > n) return false;
        size = rd32(b + o);
        o += 4u;
    }
    const size_t start = out->size();
    out->reserve(start + size);
    while (out->size() - start < size) {
        if (o >= n) return false;
        const uint8_t flags = b[o++];
        for (int bit = 0; bit < 8 && out->size() - start < size; ++bit) {
            if (!(flags & (0x80 >> bit))) {
                if (o >= n) return false;
                out->push_back(b[o++]);
                continue;
            }
            if (o + 2u > n) return false;
            const uint8_t b0 = b[o];
            size_t len, dist;
            switch (b0 >> 4) {
            case 0:
                if (o + 3u > n) return false;
                len = (size_t)((((b0 & 15) << 4) | (b[o + 1] >> 4)) + 0x11);
                dist = (size_t)((((b[o + 1] & 15) << 8) | b[o + 2]) + 1);
                o += 3u;
                break;
            case 1:
                if (o + 4u > n) return false;
                len = (size_t)((((b0 & 15) << 12) | (b[o + 1] << 4) |
                                (b[o + 2] >> 4)) + 0x111);
                dist = (size_t)((((b[o + 2] & 15) << 8) | b[o + 3]) + 1);
                o += 4u;
                break;
            default:
                len = (size_t)((b0 >> 4) + 1);
                dist = (size_t)((((b0 & 15) << 8) | b[o + 1]) + 1);
                o += 2u;
                break;
            }
            if (dist > out->size() - start) return false;
            for (size_t k = 0; k < len; ++k) {
                const uint8_t v = (*out)[out->size() - dist];   // copy first:
                out->push_back(v);                              // no aliasing
            }
        }
    }
    out->resize(start + size);
    return true;
}

// The game's own wrapper: "PMOC", u32 size, u32 block count N, i32 hint,
// i32 block[N]; a positive block is an LZ11 stream, a negative one |n| raw
// bytes. A file without it is returned as it is.
bool comp_unwrap(std::vector<uint8_t>* b) {
    if (b->size() < 16 || std::memcmp(b->data(), "PMOC", 4) != 0) return true;
    const uint32_t size = rd32(b->data() + 4), cnt = rd32(b->data() + 8);
    size_t p = 16u + 4u * cnt;
    if (p > b->size()) return false;
    std::vector<uint8_t> out;
    out.reserve(size);
    for (uint32_t i = 0; i < cnt; ++i) {
        const int32_t n = (int32_t)rd32(b->data() + 16u + 4u * i);
        if (n < 0) {
            const size_t k = (size_t)(-(int64_t)n);
            if (p + k > b->size()) return false;
            out.insert(out.end(), b->data() + p, b->data() + p + k);
            p += k;
        } else {
            if (!lz11(b->data(), b->size(), p, &out)) return false;
            p += (size_t)n;
        }
    }
    if (out.size() != size) return false;
    b->swap(out);
    return true;
}

// A Nitro section by its (byte-reversed) magic: header size at +0x0C, section
// count at +0x0E, then {magic, u32 size} chained.
long section(const std::vector<uint8_t>& b, const char* magic) {
    if (b.size() < 16) return -1;
    size_t o = rd16(b.data() + 0x0C);
    const unsigned count = rd16(b.data() + 0x0E);
    for (unsigned i = 0; i < count && o + 8u <= b.size(); ++i) {
        if (std::memcmp(b.data() + o, magic, 4) == 0) return (long)o;
        const uint32_t sz = rd32(b.data() + o + 4u);
        if (sz == 0) break;
        o += sz;
    }
    return -1;
}

// ---- the army screen's units --------------------------------------------------
//
// Each slot's candidates, in the order FUN_020B96FC (T) lists them: every
// minifig collectable (blueprints 356..440) whose unit fits the slot -- the
// unit's category for slots 0-4, FUN_020EB21C's tier for the sieges and the
// transport. Evaluated over the ROM's blueprints (docs/384); each entry is
// checked at run time against the minifig record in guest RAM (+0x60 the unit,
// +0x6C its unlock bit), so a table that disagreed with the cartridge would
// say so instead of mislabelling.
struct Cand {
    uint16_t unit;      // blueprint index
    uint16_t minifig;   // blueprint index of its minifig collectable
    uint8_t faction;    // 0 King 1 Wizard 2 Pirate 3 Imperial 4 Earth 5 Alien
                        // 6 Neutral (docs/410: the creatures, no minifig)
};

const Cand kHero[] = {
    {0, 356, 0},   {1, 357, 0},   {161, 366, 0}, {20, 369, 1},  {21, 370, 1},
    {165, 379, 1}, {40, 382, 2},  {41, 383, 2},  {145, 392, 2}, {60, 395, 3},
    {61, 396, 3},  {149, 405, 3}, {80, 408, 4},  {81, 409, 4},  {129, 418, 4},
    {100, 421, 5}, {101, 422, 5}, {133, 431, 5}, {175, 434, 0}, {176, 435, 1},
    {177, 436, 2}, {178, 437, 3}, {179, 438, 5}, {180, 439, 4}, {181, 440, 0},
};
const Cand kBuilder[] = {
    {2, 358, 0}, {22, 371, 1}, {42, 384, 2}, {62, 397, 3}, {82, 410, 4},
    {102, 423, 5},
};
const Cand kMelee[] = {
    {3, 359, 0},  {162, 367, 0}, {23, 372, 1}, {166, 380, 1}, {43, 385, 2},
    {63, 398, 3}, {83, 411, 4},  {103, 424, 5},
    // docs/410: the neutral creatures are not listed here: kCreatures below
    // places them, by default Close Combat (docs/436).
};
const Cand kRanged[] = {
    {4, 360, 0},   {24, 373, 1}, {44, 386, 2},  {146, 393, 2}, {64, 399, 3},
    {150, 406, 3}, {84, 412, 4}, {130, 419, 4}, {104, 425, 5}, {134, 432, 5},
};
const Cand kMounted[] = {
    {5, 361, 0}, {25, 374, 1}, {45, 387, 2}, {65, 400, 3}, {85, 413, 4},
    {105, 426, 5},
};
const Cand kSiege1[] = {
    {6, 362, 0}, {26, 375, 1}, {46, 388, 2}, {66, 401, 3}, {86, 414, 4},
    {106, 427, 5},
};
const Cand kSiege2[] = {
    {7, 363, 0},   {163, 368, 0}, {27, 376, 1},  {167, 381, 1},
    {47, 389, 2},  {147, 394, 2}, {67, 402, 3},  {151, 407, 3},
    {87, 415, 4},  {131, 420, 4}, {107, 428, 5}, {135, 433, 5},
    // docs/410: the Shark (PirateNeutral, category 6, SharkBite) is placed by
    // kCreatures below. The tier function FUN_020EB21C gives it none, but the
    // army record's three special fields only differ in which menu lists them.
};
const Cand kSiege3[] = {
    {8, 364, 0}, {28, 377, 1}, {48, 390, 2}, {68, 403, 3}, {88, 416, 4},
    {108, 429, 5},
};
const Cand kTransport[] = {
    {9, 365, 0}, {29, 378, 1}, {49, 391, 2}, {69, 404, 3}, {89, 417, 4},
    {109, 430, 5},
};

struct Slot {
    const Cand* c;
    int n;
    uint32_t rec_off;    // the army record field (0x02154E40 layout)
    int x, y;            // the button on UI/FE/CustomizeArmy.bin, native px
    uint32_t loc;        // the game's name for the category
    const char* fallback;
};
#define LB_N(a) (int)(sizeof(a) / sizeof((a)[0]))
const Slot kSlots[LB_TEAM_SLOTS] = {
    {kHero, LB_N(kHero), 0x08u, 36, 11, 245u, "Hero"},
    {kBuilder, LB_N(kBuilder), 0x0Au, 20, 40, 246u, "Builder"},
    {kMelee, LB_N(kMelee), 0x0Cu, 52, 40, 242u, "Close Combat"},
    {kRanged, LB_N(kRanged), 0x0Eu, 20, 69, 243u, "Ranged"},
    {kMounted, LB_N(kMounted), 0x10u, 52, 69, 244u, "Mounted"},
    {kSiege1, LB_N(kSiege1), 0x00u, 20, 98, 248u, "Special One"},
    {kSiege2, LB_N(kSiege2), 0x02u, 52, 98, 249u, "Special Two"},
    {kSiege3, LB_N(kSiege3), 0x04u, 20, 127, 250u, "Special Three"},
    {kTransport, LB_N(kTransport), 0x06u, 52, 127, 251u, "Transport"},
};
#undef LB_N

// FUN_020B83D4's rebuild from GetFaction(0x7C) when the record's flag is
// clear: the first unit of categories 0..4, the first three sieges, the
// first transport. That is Free Play's default army.
const uint16_t kKingDefault[LB_TEAM_SLOTS] = {0, 2, 3, 4, 5, 6, 7, 8, 9};

// docs/410: a creature (no minifig) only while the option allows them.
bool cand_on(const Cand& c) { return c.minifig != 0 || lb_creatures_live() != 0; }

// docs/436: THE CREATURES' SLOTS. Each creature sits in the slot the setting
// gives it (lb_creature_slot: by default the Wolf and the Robot in Close
// Combat, the Shark in Special Two), after the slot's minifig units.
const Cand kCreatures[4] = {{169, 0, 6}, {138, 0, 6}, {155, 0, 6}, {153, 0, 6}};   // Wolf, Robot, Shark, Monkey
int creature_slot_of(int i) { return lb_creature_slot(kCreatures[i].unit); }
// How many candidates slot `s` has before the option is looked at, and the
// k-th of them: the static ones, then the creatures that chose this slot.
int slot_total(int s) {
    int n = kSlots[s].n;
    for (int i = 0; i < 4; ++i) n += creature_slot_of(i) == s;
    return n;
}
const Cand& slot_item(int s, int k) {
    if (k < kSlots[s].n) return kSlots[s].c[k];
    k -= kSlots[s].n;
    for (int i = 0; i < 4; ++i)
        if (creature_slot_of(i) == s && k-- == 0) return kCreatures[i];
    return kCreatures[0];   // not reached for k < slot_total(s)
}

const Cand* find_cand(int unit, int* slot_out) {
    for (int s = 0; s < LB_TEAM_SLOTS; ++s)
        for (int k = 0; k < slot_total(s); ++k) {
            const Cand& c = slot_item(s, k);
            if (c.unit == unit && cand_on(c)) {
                if (slot_out) *slot_out = s;
                return &c;
            }
        }
    return nullptr;
}

bool guest_ptr(uint32_t p) { return p >= 0x02000000u && p < 0x02400000u; }

constexpr uint32_t kArmyRecord = 0x02154E40u;   // record 0, the only one
constexpr uint32_t kUnlockMgr = 0x021585A8u;    // Battles::User::UnlockableManager
constexpr uint32_t kViewSlot = 0x02157FACu;     // screen registry 0x5C: the battle view
constexpr uint32_t kArmySlot = 0x02157E7Cu;     // screen registry 16: CustomizeArmy
constexpr uint32_t kHeadCount = 0x0214E41Cu;    // FUN_020B8538's literals:
constexpr uint32_t kHeadKeys = 0x0214E4E0u;     //   166 blueprint keys
constexpr uint32_t kHeadRecBase = 0x252u;       //   UIMgrData record 0x252 + head

// Where blueprint `index` is in RAM. BP/Entities.ebp is "BPNZ" then 551
// records whose size is set by the kind byte at +8: 0x7C for a unit, 0x74 and
// 0x70 for the other kinds -- the minifig collectables are 0x70 -- so only the
// units, which come first, are at base + 4 + index * 0x7C. The loader
// (func_02001F3C) fixes the file up in place without moving records, so the
// walk is made over guest RAM once per block.
constexpr int kBlueprints = 551;
std::vector<uint32_t> g_rec_addr;
uint32_t g_rec_base = 0u;
uint32_t bp_addr(int index) {
    const uint32_t base = lb_patches_blueprint_base();
    if (!base || index < 0 || index >= kBlueprints) return 0u;
    if (base != g_rec_base) {
        g_rec_addr.assign(kBlueprints, 0u);
        g_rec_base = base;
        uint32_t o = base + 4u;
        for (int i = 0; i < kBlueprints; ++i) {
            g_rec_addr[(size_t)i] = o;
            const uint8_t kind = bus_read_u8(o + 8u);
            const uint32_t size = kind == 0 ? 0x7Cu : kind == 1 ? 0x74u
                                  : kind == 2 ? 0x70u : 0u;
            if (!size) {   // not the layout this was read from: use none
                std::fprintf(stderr, "[lb_teams] blueprint %d has kind %u; "
                             "the record walk stops\n", i, (unsigned)kind);
                for (int j = i; j < kBlueprints; ++j) g_rec_addr[(size_t)j] = 0u;
                break;
            }
            o += size;
        }
    }
    return g_rec_addr[(size_t)index];
}

// The runtime check of the table: every minifig record names its unit at +0x60
// and its unlock bit at +0x6C (FUN_02000C6C; = minifig - 260). -1 not checked
// yet (blueprints not in RAM), 0 a mismatch (logged once), 1 agreed.
int g_table_ok = -1;
void check_table() {
    if (g_table_ok >= 0 || !lb_patches_blueprint_base()) return;
    int bad = 0;
    for (int s = 0; s < LB_TEAM_SLOTS; ++s)
        for (int k = 0; k < kSlots[s].n; ++k) {
            const Cand& c = kSlots[s].c[k];
            if (!c.minifig) continue;          // docs/410: a creature
            const uint32_t mf = bp_addr(c.minifig);
            // +0x60 names the unit: its index in the file, or, if the loader
            // fixed it up, the address of its record.
            const uint32_t unit = mf ? bus_read_u32(mf + 0x60u) : 0u;
            const bool names_unit = unit == c.unit || unit == bp_addr(c.unit);
            if (!mf || bus_read_u8(mf + 8u) != 2u || !names_unit ||
                bus_read_u16(mf + 0x6Cu) != (uint32_t)(c.minifig - 260)) {
                if (++bad <= 4)
                    std::fprintf(stderr, "[lb_teams] minifig %u does not name "
                                 "unit %u (kind %u, +0x60 %08X, +0x6C %u)\n",
                                 c.minifig, c.unit,
                                 mf ? (unsigned)bus_read_u8(mf + 8u) : 0u,
                                 (unsigned)unit,
                                 mf ? (unsigned)bus_read_u16(mf + 0x6Cu) : 0u);
            }
        }
    g_table_ok = bad ? 0 : 1;
    std::fprintf(stderr, "[lb_teams] unit table: %s against the blueprints "
                 "in RAM\n", bad ? "DISAGREES" : "agrees");
    std::fflush(stderr);
}

// BitArray::Get (020EE9A0): data = [ba + 0xC], then bit i.
bool bit_get(uint32_t ba, uint32_t i) {
    const uint32_t data = bus_read_u32(ba + 0x0Cu);
    if (!guest_ptr(data)) return false;
    return ((bus_read_u8(data + (i >> 3)) >> (i & 7u)) & 1u) != 0;
}

// ---- names, from the game's live LOC table (docs/356's reader) --------------
constexpr uint32_t kLangManager = 0x0215378Cu;
constexpr uint32_t kLangMagic = 0x474E414Cu;   // "LANG"
constexpr uint32_t kLangIds = 0xBF8u;
constexpr uint32_t kLangStrings = 0x0Cu + kLangIds * 4u;
constexpr uint32_t kLangMaxBytes = 0x20000u;

uint32_t lang_table() {
    if (bus_read_u16(kLangManager) != 0u) return 0u;
    if (bus_read_u32(kLangManager + 4u) != bus_read_u32(kLangManager + 8u))
        return 0u;
    const uint32_t file = bus_read_u32(kLangManager + 0x0Cu);
    if (file < 0x02000000u || file > 0x02400000u - kLangMaxBytes) return 0u;
    if (bus_read_u32(file) != kLangMagic) return 0u;
    if (bus_read_u32(file + 8u) != file + 0x0Cu) return 0u;
    return file;
}

// Text id `id` as UTF-8 (the .lng text is 8-bit; bytes past 0x7F are taken
// as Latin-1). Empty while the table is not settled.
std::string loc_text(uint32_t id) {
    const uint32_t file = lang_table();
    if (!file || id >= kLangIds) return std::string();
    const uint32_t at = bus_read_u32(file + 0x0Cu + id * 4u);
    if (at < file + kLangStrings || at >= file + kLangMaxBytes) return std::string();
    std::string s;
    for (uint32_t i = 0; i < 96u; ++i) {
        const uint8_t c = bus_read_u8(at + i);
        if (c == 0) break;
        if (c < 0x20) { s.push_back(' '); continue; }
        if (c < 0x80) { s.push_back((char)c); continue; }
        s.push_back((char)(0xC0 | (c >> 6)));
        s.push_back((char)(0x80 | (c & 0x3F)));
    }
    return s;
}

struct NameCache {
    uint32_t file = 0;
    std::string unit[192];
    std::string slot[LB_TEAM_SLOTS];
};
NameCache g_names;

void names_refresh() {
    const uint32_t file = lang_table();
    if (file == g_names.file) return;
    g_names = NameCache();
    g_names.file = file;
}

// ---- the head atlas ---------------------------------------------------------
//
// UI/MiniHeads.NCGR: 4bpp, tiled, 48x30 tiles = 16x10 heads of 24x24, loaded by
// FUN_02030DA4 (A) as resource 0x2C with UI/WorldViewTop_Back.NCLR. A unit's
// head position is its blueprint's index in the u32 key table at 0x0214E4E0
// (FUN_020E0784); its sub-palette is byte +0x0F of the first frame of
// UI/FE/UIMgrData.bin record 0x252 + position (FUN_020E06FC).
constexpr int kAtlasW = 384, kAtlasH = 240;
std::vector<uint32_t> g_rgba, g_grey;   // RGBA32, one row after another
int g_pos[192];                         // blueprint -> head position, -1 none
int g_atlas_state = 0;                  // 0 not yet, 1 built, -1 failed
std::string g_atlas_msg = "Waiting for the ROM.";
SDL_Texture* g_tex[2] = {nullptr, nullptr};
SDL_Renderer* g_tex_owner = nullptr;

uint32_t rgb555_to_rgba(uint16_t c) {
    uint32_t out = 0xFF000000u;
    for (int ch = 0; ch < 3; ++ch) {
        const uint32_t v = ((c >> (5 * ch)) & 31u) * 2u;
        out |= (((v << 2) | (v >> 4)) & 0xFFu) << (8 * ch);
    }
    return out;
}

bool build_atlas() {
    if (g_atlas_state != 0) return g_atlas_state > 0;
    if (!g_rom) return false;
    // The key table is in the ARM9's data, valid once the ARM9 is in RAM.
    const uint32_t count = bus_read_u32(kHeadCount);
    if (count == 0 || count > 551u || bus_read_u32(kHeadKeys) != 0u ||
        bus_read_u32(kHeadKeys + 4u) != 2u) {
        g_atlas_msg = "Waiting for the game's head table.";
        return false;
    }
    std::vector<uint8_t> ncgr, nclr, ui;
    if (!nitro_file("UI/MiniHeads.NCGR", &ncgr) || !comp_unwrap(&ncgr) ||
        !nitro_file("UI/WorldViewTop_Back.NCLR", &nclr) || !comp_unwrap(&nclr) ||
        !nitro_file("UI/FE/UIMgrData.bin", &ui) || !comp_unwrap(&ui)) {
        g_atlas_state = -1;
        g_atlas_msg = "Could not read the heads from the ROM.";
        std::fprintf(stderr, "[lb_teams] %s\n", g_atlas_msg.c_str());
        return false;
    }
    const long c = section(ncgr, "RAHC"), pl = section(nclr, "TTLP");
    if (c < 0 || pl < 0 || ui.size() < 8) {
        g_atlas_state = -1;
        g_atlas_msg = "The heads' files are not the expected format.";
        return false;
    }
    const uint8_t* cb = ncgr.data() + c;
    const unsigned th = rd16(cb + 8), tw = rd16(cb + 10);
    const uint32_t dsz = rd32(cb + 24), doff = rd32(cb + 28);
    if (tw * 8u != (unsigned)kAtlasW || th * 8u != (unsigned)kAtlasH ||
        rd32(cb + 12) != 3u || (size_t)c + 8u + doff + dsz > ncgr.size() ||
        dsz < (size_t)tw * th * 32u) {
        g_atlas_state = -1;
        g_atlas_msg = "UI/MiniHeads is not the expected 4bpp sheet.";
        return false;
    }
    const uint8_t* pix = cb + 8 + doff;
    const uint8_t* pb = nclr.data() + pl;
    const uint32_t pdsz = rd32(pb + 16), pdoff = rd32(pb + 20);
    if ((size_t)pl + 8u + pdoff + pdsz > nclr.size()) {
        g_atlas_state = -1;
        g_atlas_msg = "UI/WorldViewTop_Back.NCLR is short.";
        return false;
    }
    const uint8_t* pal = pb + 8 + pdoff;
    const unsigned pal_n = pdsz / 2u;
    const unsigned ui_n = rd16(ui.data());

    for (int& p : g_pos) p = -1;
    for (uint32_t k = 0; k < count && k < 160u; ++k) {
        const uint32_t key = bus_read_u32(kHeadKeys + 4u * k);
        if (key < 192u && g_pos[key] < 0) g_pos[key] = (int)k;
    }
    g_rgba.assign((size_t)kAtlasW * kAtlasH, 0u);
    g_grey.assign((size_t)kAtlasW * kAtlasH, 0u);
    int drawn = 0;
    for (unsigned h = 0; h < 160u; ++h) {
        const unsigned rec = kHeadRecBase + h;
        if (rec >= ui_n || 8u + (rec + 1u) * 0x1Cu > ui.size()) break;
        const uint32_t frame = rd32(ui.data() + 8u + rec * 0x1Cu);
        const uint32_t res = rd32(ui.data() + 8u + rec * 0x1Cu + 24u);
        if (frame == 0 || res != 0x2Cu || frame + 0x10u > ui.size()) continue;
        const unsigned sub = ui[frame + 0x0Fu];
        const int x0 = (int)(h % 16u) * 24, y0 = (int)(h / 16u) * 24;
        for (int y = y0; y < y0 + 24; ++y)
            for (int x = x0; x < x0 + 24; ++x) {
                const uint8_t byte =
                    pix[((y >> 3) * tw + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
                const unsigned v = (x & 1) ? (byte >> 4) : (byte & 15u);
                if (!v || sub * 16u + v >= pal_n) continue;   // 0 is clear
                const uint32_t rgba = rgb555_to_rgba(rd16(pal + 2u * (sub * 16u + v)));
                const size_t at = (size_t)y * kAtlasW + x;
                g_rgba[at] = rgba;
                const uint32_t r = rgba & 0xFFu, g = (rgba >> 8) & 0xFFu,
                               b = (rgba >> 16) & 0xFFu;
                const uint32_t yv = (77u * r + 150u * g + 29u * b) >> 8;
                g_grey[at] = 0xFF000000u | (yv << 16) | (yv << 8) | yv;
            }
        ++drawn;
    }
    g_atlas_state = 1;
    g_atlas_msg.clear();
    std::fprintf(stderr, "[lb_teams] %d heads read from UI/MiniHeads, %u keys\n",
                 drawn, (unsigned)count);
    std::fflush(stderr);
    return true;
}

// ---- the battle HUD's icons (docs/408) --------------------------------------
//
// The battle HUD does not cut the head grid (h%16, h/16) as the front end
// does: FUN_020D85F4(bp, heads, upgrade) picks a UI/Game/UIMgrData.bin record
// -- 0x252 + the blueprint's key position for the heads set, after a remap:
// Tower II and III draw the Tower II "+" cell in an upgrade list (the record
// the front end's file leaves empty), shipyards draw the King's, bridge
// variants the small bridge's -- and that record's frame names a rectangle on
// UI/MiniHeadsGame (u16 x,y,w,h at frame+0x14, FUN_020332E8) with its
// sub-palette at frame+0x0F. One cell per blueprint here, 16 x 12 of 24 px.
// The battle sheet drew a lantern and a sack over the Wolf's and the Shark's
// heads (cells (11,2) and (11,1)); those two are taken from UI/MiniHeads,
// which still has them.
constexpr int kHudW = 384, kHudH = 288;
std::vector<uint32_t> g_hud_rgba;
bool g_hud_has[192];
int g_hud_state = 0;                    // 0 not yet, 1 built, -1 failed
SDL_Texture* g_hud_tex = nullptr;
SDL_Renderer* g_hud_owner = nullptr;

int hud_icon_key(int bp) {
    static const int kTower[6] = {16, 36, 56, 76, 96, 116};
    for (int t : kTower)
        if (bp == t + 1 || bp == t + 2) return t + 1;   // the "+" tower
    if (bp == 39 || bp == 59 || bp == 79 || bp == 99 || bp == 119) return 19;
    if (bp >= 122 && bp <= 126) return 121;
    return bp;
}

bool build_hud_atlas() {
    if (g_hud_state != 0) return g_hud_state > 0;
    if (!g_rom) return false;
    const uint32_t count = bus_read_u32(kHeadCount);
    if (count == 0 || count > 551u || bus_read_u32(kHeadKeys + 4u) != 2u)
        return false;   // the key table is not in RAM yet
    std::vector<uint8_t> game, fe, nclr, ui;
    if (!nitro_file("UI/MiniHeadsGame.NCGR", &game) || !comp_unwrap(&game) ||
        !nitro_file("UI/MiniHeads.NCGR", &fe) || !comp_unwrap(&fe) ||
        !nitro_file("UI/WorldViewTop_Back.NCLR", &nclr) || !comp_unwrap(&nclr) ||
        !nitro_file("UI/Game/UIMgrData.bin", &ui) || !comp_unwrap(&ui)) {
        g_hud_state = -1;
        std::fprintf(stderr, "[lb_teams] could not read the HUD icons\n");
        return false;
    }
    struct Sheet { const uint8_t* pix; unsigned tw; };
    auto sheet = [](const std::vector<uint8_t>& f, Sheet* s) {
        const long c = section(f, "RAHC");
        if (c < 0) return false;
        const uint8_t* cb = f.data() + c;
        const unsigned th = rd16(cb + 8), tw = rd16(cb + 10);
        const uint32_t dsz = rd32(cb + 24), doff = rd32(cb + 28);
        if (tw * 8u != (unsigned)kAtlasW || th * 8u != (unsigned)kAtlasH ||
            rd32(cb + 12) != 3u || (size_t)c + 8u + doff + dsz > f.size())
            return false;
        s->pix = cb + 8 + doff;
        s->tw = tw;
        return true;
    };
    Sheet sg{}, sf{};
    const long pl = section(nclr, "TTLP");
    if (!sheet(game, &sg) || !sheet(fe, &sf) || pl < 0 || ui.size() < 8) {
        g_hud_state = -1;
        return false;
    }
    const uint8_t* pb = nclr.data() + pl;
    const uint32_t pdsz = rd32(pb + 16), pdoff = rd32(pb + 20);
    if ((size_t)pl + 8u + pdoff + pdsz > nclr.size()) { g_hud_state = -1; return false; }
    const uint8_t* pal = pb + 8 + pdoff;
    const unsigned pal_n = pdsz / 2u;
    const unsigned ui_n = rd16(ui.data());

    g_hud_rgba.assign((size_t)kHudW * kHudH, 0u);
    for (bool& h : g_hud_has) h = false;
    int drawn = 0;
    for (int bp = 0; bp < 182; ++bp) {
        const int key = hud_icon_key(bp);
        int pos = -1;
        for (uint32_t k = 0; k < count && k < 166u; ++k)
            if (bus_read_u32(kHeadKeys + 4u * k) == (uint32_t)key) { pos = (int)k; break; }
        if (pos < 0) continue;
        const unsigned rec = kHeadRecBase + (unsigned)pos;
        if (rec >= ui_n || 8u + (rec + 1u) * 0x1Cu > ui.size()) continue;
        const uint32_t frame = rd32(ui.data() + 8u + rec * 0x1Cu);
        const uint32_t res = rd32(ui.data() + 8u + rec * 0x1Cu + 24u);
        if (frame == 0 || res != 0x2Cu || frame + 0x1Cu > ui.size() ||
            ui[frame + 0x0Cu] != 0)
            continue;
        const unsigned sx = rd16(ui.data() + frame + 0x14u);
        const unsigned sy = rd16(ui.data() + frame + 0x16u);
        const unsigned sub = ui[frame + 0x0Fu];
        if (sx + 24u > (unsigned)kAtlasW || sy + 24u > (unsigned)kAtlasH) continue;
        const Sheet& s = (bp == 155 || bp == 169) ? sf : sg;   // Shark, Wolf
        const int x0 = (bp % 16) * 24, y0 = (bp / 16) * 24;
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 24; ++x) {
                const unsigned px = sx + (unsigned)x, py = sy + (unsigned)y;
                const uint8_t byte =
                    s.pix[((py >> 3) * s.tw + (px >> 3)) * 32 + (py & 7) * 4 + ((px & 7) >> 1)];
                const unsigned v = (px & 1) ? (byte >> 4) : (byte & 15u);
                if (!v || sub * 16u + v >= pal_n) continue;
                g_hud_rgba[(size_t)(y0 + y) * kHudW + (x0 + x)] =
                    rgb555_to_rgba(rd16(pal + 2u * (sub * 16u + v)));
            }
        g_hud_has[bp] = true;
        ++drawn;
    }
    g_hud_state = 1;
    std::fprintf(stderr, "[lb_teams] %d HUD icons read from UI/MiniHeadsGame\n",
                 drawn);
    std::fflush(stderr);
    return true;
}

// ---- the creatures on the game's own army screen (BR-0002, SG-0009) ----------
//
// docs/430. The army screen lists a unit through a minifig COLLECTABLE
// blueprint that names it (+0x60 the unit, +0x6C its unlock bit): FUN_020B96FC
// walks all 551 blueprints, keeps kind 2 / category 0x0B whose unit passes
// FUN_020EAC1C (slots 0-4, by category) or FUN_020EB21C's tier (slots 5-8), and
// FUN_020B859C finds the one naming each pick of the army record. The Wolf,
// the Robot and the Shark have no minifig, so the list never offers them and a
// team holding one found no card for its slot (a locked King).
//
// Each creature is given a card by turning one story-only collectable record
// (548 Minikit_I3_5, 549 Ticket, 550 CK1_3_Scroll: pick-ups of campaign
// levels, nothing in Free Play or a lobby) into a minifig for as long as the
// front end is up. The record's +0x6C points at the King hero's unlock bit
// (96), which every profile has unlocked and bought. Two overrides in
// lb_patches.cpp let the creature through the two filters, from that one call
// site. The head is the battle HUD's (the front end's sheet has a "?" at the
// creature's key position): its pixels are written over the game's resident
// copy of UI/MiniHeads while the screen is up, in the same sub-palette (5).
//
// Off: no byte of the game is touched.
struct CardRec { int rec; int unit; };
const CardRec kCardRecs[4] = {{548, 169}, {549, 138}, {550, 155}, {547, 153}};   // Wolf, Robot, Shark, Monkey (547 Minikits_I3_4)
constexpr uint32_t kCardBit = 96u;                                    // minifig 356's slot
struct CardOld { uint8_t cat; uint32_t unit; uint8_t lock; uint16_t bit; };
CardOld g_card_old[4];
bool g_cards_applied = false;
uint32_t g_cards_base = 0u;
std::vector<uint8_t> g_cards_ref;     // the ROM's own sheet, to know it when we see it

bool battle_present() {
    const uint32_t view = bus_read_u32(kViewSlot);
    if (guest_ptr(view) && bus_read_u32(view + 0x1Cu) != 0u) return true;
    return lb_field_entity_at(0, nullptr, nullptr, nullptr, nullptr);
}

// The army screen is alive: its registry slot holds it and its layout is up.
uint32_t army_screen() {
    const uint32_t a = bus_read_u32(kArmySlot);
    return guest_ptr(a) && bus_read_u32(a + 0x1Cu) != 0u ? a : 0u;
}

// 24x24 of a creature's HUD head as 4bpp tiles in the 3x3 order of a head cell
// (tile (tx,ty) at 32 * (ty * 3 + tx)), the key position and the sub-palette
// the Game UIMgrData gives it. The same source and rectangle as build_hud_atlas.
bool head_tiles(int bp, uint8_t out[288], int* pos_out, int* sub_out) {
    const uint32_t count = bus_read_u32(kHeadCount);
    if (count == 0 || count > 551u || bus_read_u32(kHeadKeys + 4u) != 2u) return false;
    std::vector<uint8_t> game, fe, ui;
    if (!nitro_file("UI/MiniHeadsGame.NCGR", &game) || !comp_unwrap(&game) ||
        !nitro_file("UI/MiniHeads.NCGR", &fe) || !comp_unwrap(&fe) ||
        !nitro_file("UI/Game/UIMgrData.bin", &ui) || !comp_unwrap(&ui) ||
        ui.size() < 8)
        return false;
    int pos = -1;
    for (uint32_t k = 0; k < count && k < 160u; ++k)
        if (bus_read_u32(kHeadKeys + 4u * k) == (uint32_t)bp) { pos = (int)k; break; }
    if (pos < 0) return false;
    const unsigned rec = kHeadRecBase + (unsigned)pos;
    if (rec >= rd16(ui.data()) || 8u + (rec + 1u) * 0x1Cu > ui.size()) return false;
    const uint32_t frame = rd32(ui.data() + 8u + rec * 0x1Cu);
    if (frame == 0 || frame + 0x1Cu > ui.size() || ui[frame + 0x0Cu] != 0) return false;
    const unsigned sx = rd16(ui.data() + frame + 0x14u);
    const unsigned sy = rd16(ui.data() + frame + 0x16u);
    const std::vector<uint8_t>& f = (bp == 155 || bp == 169) ? fe : game;   // Shark, Wolf
    const long c = section(f, "RAHC");
    if (c < 0) return false;
    const uint8_t* cb = f.data() + c;
    const unsigned tw = rd16(cb + 10);
    const uint32_t doff = rd32(cb + 28);
    if (tw * 8u != (unsigned)kAtlasW || sx + 24u > (unsigned)kAtlasW ||
        sy + 24u > (unsigned)kAtlasH || (size_t)c + 8u + doff + 46080u > f.size())
        return false;
    const uint8_t* pix = cb + 8 + doff;
    std::memset(out, 0, 288);
    for (unsigned y = 0; y < 24u; ++y)
        for (unsigned x = 0; x < 24u; ++x) {
            const unsigned px = sx + x, py = sy + y;
            const uint8_t byte =
                pix[((py >> 3) * tw + (px >> 3)) * 32 + (py & 7) * 4 + ((px & 7) >> 1)];
            const unsigned v = (px & 1) ? (byte >> 4) : (byte & 15u);
            uint8_t& o = out[((y >> 3) * 3 + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
            o = (uint8_t)(o | ((x & 1) ? (v << 4) : v));
        }
    *pos_out = pos;
    *sub_out = ui[frame + 0x0Fu];
    return true;
}

struct CardHead { bool ok = false; bool tried = false; uint8_t t[288]; int pos = -1, sub = 0; };
CardHead g_card_head[4];

// The game's resident copy of UI/MiniHeads (4bpp, 48 x 30 tiles): the army
// screen's image manager (+0x44) -> its source image -> +0x38 -> +4 -> +0x14.
uint32_t resident_sheet(uint32_t army) {
    const uint32_t o0 = bus_read_u32(army + 0x44u);
    if (!guest_ptr(o0)) return 0u;
    const uint32_t o1 = bus_read_u32(o0 + 0x38u);
    if (!guest_ptr(o1)) return 0u;
    const uint32_t o2 = bus_read_u32(o1 + 4u);
    if (!guest_ptr(o2)) return 0u;
    const uint32_t p = bus_read_u32(o2 + 0x14u);
    return guest_ptr(p) && guest_ptr(p + 46079u) ? p : 0u;
}

// Whether `sheet` is UI/MiniHeads as the ROM has it, outside the three cells.
bool sheet_is_ours(uint32_t sheet) {
    if (g_cards_ref.empty()) {
        std::vector<uint8_t> fe;
        if (!nitro_file("UI/MiniHeads.NCGR", &fe) || !comp_unwrap(&fe)) return false;
        const long c = section(fe, "RAHC");
        if (c < 0) return false;
        const uint32_t doff = rd32(fe.data() + c + 28);
        if ((size_t)c + 8u + doff + 46080u > fe.size()) return false;
        g_cards_ref.assign(fe.begin() + c + 8 + doff, fe.begin() + c + 8 + doff + 46080);
    }
    // Heads 0, 40 and 80 (cells nobody here touches): 288 bytes each.
    for (int h : {0, 40, 80})
        for (int t = 0; t < 9; ++t) {
            const uint32_t off = (uint32_t)(((h / 16) * 3 + t / 3) * 48 + (h % 16) * 3 + t % 3) * 32u;
            for (uint32_t b = 0; b < 32u; b += 4)
                if (bus_read_u32(sheet + off + b) != rd32(&g_cards_ref[off + b])) return false;
        }
    return true;
}

void cards_sheet(uint32_t army) {
    const uint32_t sheet = resident_sheet(army);
    if (!sheet) return;
    static uint32_t checked = 0u;
    static bool ours = false;
    if (sheet != checked) { checked = sheet; ours = sheet_is_ours(sheet); }
    if (!ours) return;
    for (int i = 0; i < 4; ++i) {
        CardHead& h = g_card_head[i];
        if (!h.tried) {
            h.tried = true;
            h.ok = head_tiles(kCardRecs[i].unit, h.t, &h.pos, &h.sub);
            std::fprintf(stderr, "[lb_cards] creature %d head: %s (key position %d, "
                         "sub-palette %d)\n", kCardRecs[i].unit, h.ok ? "read" : "NOT READ",
                         h.pos, h.sub);
            std::fflush(stderr);
        }
        if (!h.ok) continue;
        for (int t = 0; t < 9; ++t) {
            const uint32_t off = (uint32_t)(((h.pos / 16) * 3 + t / 3) * 48 +
                                            (h.pos % 16) * 3 + t % 3) * 32u;
            for (uint32_t b = 0; b < 32u; b += 4) {
                const uint32_t w = rd32(&h.t[t * 32 + b]);
                if (bus_read_u32(sheet + off + b) != w) bus_write_u32(sheet + off + b, w);
            }
        }
    }
}

void cards_tick() {
    const uint32_t base = lb_patches_blueprint_base();
    if (base != g_cards_base) {   // a game reset reloaded the blueprints
        g_cards_base = base;
        g_cards_applied = false;
    }
    const bool want = lb_creatures_live() != 0 && !battle_present();
    if (!g_cards_applied && !want) return;
    if (!base || !lb_patches_blueprint_settled()) return;
    if (want && !g_cards_applied) {
        // The record walk is lb_teams' own (bp_addr): kind 2 records are 0x70.
        for (int i = 0; i < 4; ++i) {
            const uint32_t r = bp_addr(kCardRecs[i].rec);
            if (!r || bus_read_u8(r + 8u) != 2u) return;   // not the layout this knows
        }
        for (int i = 0; i < 4; ++i) {
            const uint32_t r = bp_addr(kCardRecs[i].rec);
            g_card_old[i] = {(uint8_t)bus_read_u8(r + 0x5Cu), bus_read_u32(r + 0x60u),
                             (uint8_t)bus_read_u8(r + 0x6Bu),
                             (uint16_t)bus_read_u16(r + 0x6Cu)};
            bus_write_u8(r + 0x5Cu, 0x0Bu);                       // a minifig
            bus_write_u32(r + 0x60u, (uint32_t)kCardRecs[i].unit);
            bus_write_u8(r + 0x6Bu, 0xFFu);                       // has an unlock bit
            bus_write_u16(r + 0x6Cu, (uint16_t)kCardBit);
        }
        g_cards_applied = true;
        std::fprintf(stderr, "[lb_cards] the creatures have army-screen cards "
                     "(records 547-550)\n");
        std::fflush(stderr);
    } else if (!want && g_cards_applied) {
        if (army_screen()) return;   // its cells hold these records: wait
        for (int i = 0; i < 4; ++i) {
            const uint32_t r = bp_addr(kCardRecs[i].rec);
            if (!r) continue;
            bus_write_u8(r + 0x5Cu, g_card_old[i].cat);
            bus_write_u32(r + 0x60u, g_card_old[i].unit);
            bus_write_u8(r + 0x6Bu, g_card_old[i].lock);
            bus_write_u16(r + 0x6Cu, g_card_old[i].bit);
        }
        g_cards_applied = false;
        std::fprintf(stderr, "[lb_cards] the creature cards are back to the "
                     "campaign's records\n");
        std::fflush(stderr);
        return;
    }
    if (const uint32_t army = army_screen()) cards_sheet(army);
}

// ---- the enemy -------------------------------------------------------------
LbTeam g_enemy = {{0, 2, 3, 4, 5, 6, 7, 8, 9}};
bool g_enemy_set = false;

// ---- bans ------------------------------------------------------------------
constexpr int kBanBytes = 24;              // blueprints 0..191
std::mutex g_rules_mu;                     // the overlay, the bridge, the hook
uint8_t g_bans_local[kBanBytes] = {};      // under g_rules_mu
uint8_t g_bans_recv[kBanBytes] = {};       // under g_rules_mu
bool g_bans_recv_ok = false;               // under g_rules_mu
unsigned g_bans_recv_from = 0;             // under g_rules_mu
uint8_t g_flags_recv = 0;                  // docs/406; under g_rules_mu
int g_cslots_recv = 0;                     // docs/436; under g_rules_mu
int g_gold_recv = 0;                       // SG-0036; under g_rules_mu
// docs/413: the host's mod maps as its rules list them (key, content hash,
// name), and whether its rules carried the list at all (an older build's
// do not).
struct RecvMap { unsigned key; unsigned long long hash; std::string name; };
std::vector<RecvMap> g_maps_recv;          // under g_rules_mu
bool g_maps_recv_has = false;              // under g_rules_mu
// docs/418: the host's balance as it arrives, page by page (under g_rules_mu):
// the seed being assembled, its pack and builders, the values so far and
// which pages have come; `g_bal_done` once every page of that seed has.
// `g_bal_absent`: the host's rules carry no balance (a build before it).
unsigned long long g_bal_seed = 0ull;
int g_bal_slot = 0, g_bal_builders = 0, g_bal_total = 0, g_bal_pages = 0;
std::vector<int> g_bal_values;             // triples
std::vector<bool> g_bal_seen;
bool g_bal_done = false, g_bal_absent = false;
// BR-0011: the host's 'S' page -- the other simulation settings.
LbHostSettings g_set_recv = {};
bool g_set_ok = false;
constexpr int kBalPerPage = 60;
// When they last arrived. A host sends them every 250 ms from opening its
// lobby to the end of the match, so rules older than a few seconds belong to
// a lobby this player has left and are not applied.
std::chrono::steady_clock::time_point g_bans_recv_at{};   // under g_rules_mu
// SG-0013 (docs/430): the host's build as its rules last said it, and the one
// the joiner was last warned about (under g_rules_mu).
std::string g_host_version, g_version_warned;
std::chrono::steady_clock::time_point g_version_warned_at{};
bool rules_fresh() {
    return g_bans_recv_ok &&
           std::chrono::steady_clock::now() - g_bans_recv_at <
               std::chrono::seconds(3);
}

bool ban_bit(const uint8_t* bits, int bp) {
    return bp >= 0 && bp < kBanBytes * 8 && ((bits[bp >> 3] >> (bp & 7)) & 1);
}

bool king_default(int bp) {
    for (uint16_t k : kKingDefault)
        if (k == bp) return true;
    return false;
}

// The cartridge's own network state (lb_net_guest_update, from its task
// object): 10/2/3/4/11 hosting, 9/6/7/5/12 joining, 0/-1 idle.
bool net_hosting(int s) { return s == 10 || s == 2 || s == 3 || s == 4 || s == 11; }
bool net_joined(int s) { return s == 9 || s == 6 || s == 7 || s == 5 || s == 12; }

}  // namespace

// ---- the C API (lb_overlay.h) -----------------------------------------------

extern "C" void lb_rom_attach(const unsigned char* data, size_t size) {
    g_rom = data;
    g_rom_size = size;
    std::fprintf(stderr, "[lb_teams] ROM attached: %zu bytes\n", size);
    std::fflush(stderr);
}

// docs/387: a NitroFS file of the player's own ROM, as stored (not
// decompressed), for lb_palette.cpp. Returns its size, or -1; copies at most
// `cap` bytes into `out`.
extern "C" int lb_rom_read(const char* path, unsigned char* out, int cap) {
    std::vector<uint8_t> v;
    if (!path || !nitro_file(path, &v)) return -1;
    const size_t n = cap > 0 ? std::min(v.size(), (size_t)cap) : 0u;
    if (out && n) std::memcpy(out, v.data(), n);
    return (int)v.size();
}

// docs/428: the same, with the game's COMP wrapper removed (maps' minimaps).
extern "C" int lb_rom_read_unwrapped(const char* path, unsigned char* out, int cap) {
    std::vector<uint8_t> v;
    if (!path || !nitro_file(path, &v) || !comp_unwrap(&v)) return -1;
    const size_t n = cap > 0 ? std::min(v.size(), (size_t)cap) : 0u;
    if (out && n) std::memcpy(out, v.data(), n);
    return (int)v.size();
}

// docs/428: one blueprint's 24x24 battle-HUD icon as RGBA32 (0 = clear), for
// the match picture. 0 when the atlas is not built yet or has no such icon.
extern "C" int lb_teams_hud_icon_rgba(int bp, unsigned int out[576]) {
    if (bp < 0 || bp >= 182 || !build_hud_atlas() || !g_hud_has[bp]) return 0;
    const int x0 = (bp % 16) * 24, y0 = (bp / 16) * 24;
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x)
            out[y * 24 + x] = g_hud_rgba[(size_t)(y0 + y) * kHudW + (x0 + x)];
    return 1;
}

// docs/428: this machine's own ban bits (the host's rules, on the host).
extern "C" int lb_teams_local_bans(unsigned char* bits, int n) {
    if (!bits || n < 1) return 0;
    std::lock_guard<std::mutex> lk(g_rules_mu);
    for (int i = 0; i < n; ++i) bits[i] = i < kBanBytes ? g_bans_local[i] : 0;
    return 1;
}

// docs/429: the game's text for a LOC id in the language it runs in (map names).
extern "C" const char* lb_teams_loc_text(unsigned id) {
    static thread_local std::string s;
    s = loc_text(id);
    return s.c_str();
}

extern "C" int lb_rules_fresh(void) {
    std::lock_guard<std::mutex> lk(g_rules_mu);
    return rules_fresh() ? 1 : 0;
}

extern "C" int lb_teams_slot_count(int slot) {
    if (slot < 0 || slot >= LB_TEAM_SLOTS) return 0;
    int n = 0;
    for (int k = 0; k < slot_total(slot); ++k) n += cand_on(slot_item(slot, k));
    return n;
}

extern "C" int lb_teams_slot_unit(int slot, int k) {
    if (slot < 0 || slot >= LB_TEAM_SLOTS || k < 0) return -1;
    for (int i = 0; i < slot_total(slot); ++i) {
        if (!cand_on(slot_item(slot, i))) continue;
        if (k-- == 0) return slot_item(slot, i).unit;
    }
    return -1;
}

extern "C" void lb_teams_slot_pos(int slot, int* x, int* y) {
    if (slot < 0 || slot >= LB_TEAM_SLOTS) return;
    if (x) *x = kSlots[slot].x;
    if (y) *y = kSlots[slot].y;
}

extern "C" const char* lb_teams_slot_label(int slot) {
    if (slot < 0 || slot >= LB_TEAM_SLOTS) return "";
    names_refresh();
    std::string& s = g_names.slot[slot];
    if (s.empty()) s = loc_text(kSlots[slot].loc);
    return s.empty() ? kSlots[slot].fallback : s.c_str();
}

extern "C" const char* lb_teams_unit_name(int bp) {
    if (bp < 0 || bp >= 192) return "?";
    names_refresh();
    std::string& s = g_names.unit[bp];
    if (s.empty()) {
        const uint32_t rec = bp_addr(bp);
        if (rec) s = loc_text(bus_read_u16(rec + 0x06u));   // display name id
    }
    if (s.empty()) {
        static char buf[24];
        std::snprintf(buf, sizeof buf, "unit %d", bp);
        return buf;
    }
    return s.c_str();
}

// docs/408: a spell's name, as the HUD's tooltip gives it (FUN_020DCD34):
// spell record +1 (0x02126DA0, 0x14 each) picks a button, the button its
// layout element (0x2A5 + k, 4/5 and 6/7 swapped; WorldViewScreen::v08), and
// the {element, text} pairs at 0x021283D0 the text. Read live, so it follows
// the game's language; null when the table is not settled. Ids 0x21/0x22 are
// the Lightning Bolt's and Fireball's impact payloads and are never listed.
extern "C" const char* lb_teams_spell_name(int id) {
    static const uint16_t kText[0x21] = {
        0,     0x126, 0x126, 0x127, 0x120, 0x121, 0x122, 0x123, 0x124, 0x125,
        0x107, 0x11A, 0x110, 0x108, 0x10A, 0x109, 0x10B, 0x10C, 0x10D, 0x10E,
        0x10F, 0x116, 0x115, 0x111, 0x112, 0x114, 0x113, 0x117, 0x118, 0x119,
        0x11B, 0x11E, 0x11D};
    if (id <= 0 || id > 0x20) return nullptr;
    static uint32_t file = 0;
    static std::string cache[0x21];
    const uint32_t now = lang_table();
    if (!now) return nullptr;
    if (now != file) {
        file = now;
        for (std::string& c : cache) c.clear();
    }
    if (cache[id].empty()) cache[id] = loc_text(kText[id]);
    return cache[id].empty() ? nullptr : cache[id].c_str();
}

// docs/385: the unit's display-name id (its blueprint's +0x06), -1 if the
// blueprints are not in RAM.
extern "C" int lb_teams_unit_name_id(int bp) {
    if (bp < 0 || bp >= 192) return -1;
    const uint32_t rec = bp_addr(bp);
    return rec ? (int)bus_read_u16(rec + 0x06u) : -1;
}

extern "C" int lb_teams_unit_faction(int bp) {
    const Cand* c = find_cand(bp, nullptr);
    return c ? c->faction : -1;
}

extern "C" int lb_teams_unit_slot(int bp) {
    int s = -1;
    return find_cand(bp, &s) ? s : -1;
}

extern "C" int lb_teams_is_king_default(int bp) { return king_default(bp) ? 1 : 0; }

// -1 unknown (no profile or blueprints yet), 0 not unlocked, 1 unlocked but
// not bought, 2 fieldable. The army screen's own tests: unlocked is FUN_020EB658
// (the minifig's +0x6B clear, or its bit in mgr+4 or mgr+0x24), bought is
// FUN_020EB5B4 (its bit in mgr+0x34).
extern "C" int lb_teams_unit_state(int bp) {
    const Cand* c = find_cand(bp, nullptr);
    const uint32_t mgr = bus_read_u32(kUnlockMgr);
    if (!c || !guest_ptr(mgr) || !lb_patches_blueprint_base()) return -1;
    if (!c->minifig) return 2;           // docs/410: a creature, always fieldable
    check_table();
    const uint32_t mf = bp_addr(c->minifig);
    const uint32_t bit = bus_read_u16(mf + 0x6Cu);
    const bool unlocked = bus_read_u8(mf + 0x6Bu) == 0 ||
                          bit_get(mgr + 4u, bit) || bit_get(mgr + 0x24u, bit);
    if (!unlocked) return 0;
    return bit_get(mgr + 0x34u, bit) ? 2 : 1;
}

extern "C" SDL_Texture* lb_teams_atlas(SDL_Renderer* r, int grey) {
    if (!r || !build_atlas()) return nullptr;
    if (g_tex_owner != r) {
        for (SDL_Texture*& t : g_tex)
            if (t) { SDL_DestroyTexture(t); t = nullptr; }
        g_tex_owner = r;
    }
    const int which = grey ? 1 : 0;
    if (!g_tex[which]) {
        SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC, kAtlasW,
                                           kAtlasH);
        if (!t) return nullptr;
        SDL_UpdateTexture(t, nullptr, (which ? g_grey : g_rgba).data(),
                          kAtlasW * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
        g_tex[which] = t;
    }
    return g_tex[which];
}

extern "C" int lb_teams_icon_uv(int bp, float* u0, float* v0, float* u1,
                                float* v1) {
    if (g_atlas_state <= 0 || bp < 0 || bp >= 192 || g_pos[bp] < 0) return 0;
    const int p = g_pos[bp];
    *u0 = (float)((p % 16) * 24) / kAtlasW;
    *v0 = (float)((p / 16) * 24) / kAtlasH;
    *u1 = *u0 + 24.0f / kAtlasW;
    *v1 = *v0 + 24.0f / kAtlasH;
    return 1;
}

// docs/408: the battle HUD's icon for a blueprint (build_hud_atlas), as the
// Balance menu shows them.
extern "C" SDL_Texture* lb_teams_hud_atlas(SDL_Renderer* r) {
    if (!r || !build_hud_atlas()) return nullptr;
    if (g_hud_owner != r && g_hud_tex) {
        SDL_DestroyTexture(g_hud_tex);
        g_hud_tex = nullptr;
    }
    g_hud_owner = r;
    if (!g_hud_tex) {
        g_hud_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STATIC, kHudW, kHudH);
        if (!g_hud_tex) return nullptr;
        SDL_UpdateTexture(g_hud_tex, nullptr, g_hud_rgba.data(), kHudW * 4);
        SDL_SetTextureBlendMode(g_hud_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_hud_tex, SDL_ScaleModeNearest);
    }
    return g_hud_tex;
}

extern "C" int lb_teams_hud_icon_uv(int bp, float* u0, float* v0, float* u1,
                                    float* v1) {
    if (g_hud_state <= 0 || bp < 0 || bp >= 182 || !g_hud_has[bp]) return 0;
    *u0 = (float)((bp % 16) * 24) / kHudW;
    *v0 = (float)((bp / 16) * 24) / kHudH;
    *u1 = *u0 + 24.0f / kHudW;
    *v1 = *v0 + 24.0f / kHudH;
    return 1;
}

extern "C" const char* lb_teams_icons_status(void) {
    return g_atlas_msg.c_str();
}

extern "C" void lb_teams_shutdown(void) {
    for (SDL_Texture*& t : g_tex)
        if (t) { SDL_DestroyTexture(t); t = nullptr; }
    g_tex_owner = nullptr;
    if (g_hud_tex) { SDL_DestroyTexture(g_hud_tex); g_hud_tex = nullptr; }
    g_hud_owner = nullptr;
}

extern "C" void lb_teams_king_default(LbTeam* t) {
    if (!t) return;
    for (int s = 0; s < LB_TEAM_SLOTS; ++s) t->pick[s] = kKingDefault[s];
}

extern "C" int lb_teams_valid(const LbTeam* t) {
    if (!t) return 0;
    for (int s = 0; s < LB_TEAM_SLOTS; ++s) {
        int in = -1;
        if (!find_cand(t->pick[s], &in) || in != s) return 0;
    }
    return 1;
}

extern "C" int lb_teams_is_default(const LbTeam* t) {
    for (int s = 0; s < LB_TEAM_SLOTS; ++s)
        if (t->pick[s] != kKingDefault[s]) return 0;
    return 1;
}

// The loaded profile's army, record 0 at 0x02154E40. Returns 1 when a profile
// is loaded and the record holds a valid army; *confirmed is its +0x12 flag,
// which the commit sets on every Continue (docs/384: not "non-default").
extern "C" int lb_teams_read_save(LbTeam* t, int* confirmed) {
    char name[16];
    if (lb_net_profile_name(name, (int)sizeof name) <= 0) return 0;
    LbTeam r;
    for (int s = 0; s < LB_TEAM_SLOTS; ++s)
        r.pick[s] = (unsigned short)bus_read_u16(kArmyRecord + kSlots[s].rec_off);
    if (confirmed) *confirmed = bus_read_u8(kArmyRecord + 0x12u) ? 1 : 0;
    if (!lb_teams_valid(&r)) return 0;
    if (t) *t = r;
    return 1;
}

// Why Set Team may not apply now, or null. The rule: only when the player
// is not in gameplay, the pause menu counting as gameplay, and never while
// connected to a lobby or viewing the master server.
extern "C" const char* lb_teams_set_team_blocked(void) {
    char name[16];
    if (lb_net_profile_name(name, (int)sizeof name) <= 0)
        return "No profile is loaded yet.";
    const int net = lb_net_guest_state();
    if (net != 0 && net != -1)
        return "Not while hosting, in a lobby, or browsing games.";
    // The battle view, registry slot 0x5C: it exists from the battle's first
    // frame to its end, under the pause menu too, and the slot is cleared
    // when its layout goes (docs/384).
    const uint32_t view = bus_read_u32(kViewSlot);
    if (guest_ptr(view) && bus_read_u32(view + 0x1Cu) != 0u)
        return "Not during a battle (the pause menu counts).";
    // The army screen keeps its own two copies of the record (+0x64, +0x78)
    // and writes them back on Continue, which would undo Set Team.
    const uint32_t army = bus_read_u32(kArmySlot);
    if (guest_ptr(army) && bus_read_u32(army + 0x1Cu) != 0u)
        return "Close the game's army screen first.";
    return nullptr;
}

// Write the team into record 0 as the army screen's commit would
// (FUN_020B8EB4 -> FUN_0204FED8: the picks, +0x12 = 1, +0x13 = 0x7C), after
// the army screen's own refusal: a unit must be unlocked and bought.
extern "C" int lb_teams_set_team(const LbTeam* t, char* msg, int cap) {
    auto say = [&](const char* fmt, const char* a) {
        if (msg && cap > 0) std::snprintf(msg, (size_t)cap, fmt, a);
    };
    if (!t || !lb_teams_valid(t)) { say("%s", "That team is not valid."); return 0; }
    if (const char* why = lb_teams_set_team_blocked()) { say("%s", why); return 0; }
    for (int s = 0; s < LB_TEAM_SLOTS; ++s) {
        const int st = lb_teams_unit_state(t->pick[s]);
        if (st < 0) { say("%s", "The game's unlock state is not readable yet."); return 0; }
        if (st < 2) {
            say(st == 0 ? "%s is not unlocked on this save."
                        : "%s is not bought on this save.",
                lb_teams_unit_name(t->pick[s]));
            return 0;
        }
    }
    for (int s = 0; s < LB_TEAM_SLOTS; ++s)
        bus_write_u16(kArmyRecord + kSlots[s].rec_off, t->pick[s]);
    bus_write_u8(kArmyRecord + 0x12u, 1u);
    bus_write_u8(kArmyRecord + 0x13u, 0x7Cu);
    std::fprintf(stderr, "[lb_teams] Set Team: record 0 = %u %u %u %u %u | %u "
                 "%u %u %u\n", t->pick[0], t->pick[1], t->pick[2], t->pick[3],
                 t->pick[4], t->pick[5], t->pick[6], t->pick[7], t->pick[8]);
    std::fflush(stderr);
    say("%s", "Set. The game uses it now; it is saved with the game's next save.");
    return 1;
}

extern "C" void lb_teams_set_enemy(const LbTeam* t) {
    if (!t || !lb_teams_valid(t)) return;
    g_enemy = *t;
    g_enemy_set = true;
}

extern "C" void lb_teams_get_enemy(LbTeam* t) {
    if (t) *t = g_enemy;
}

// Each slot independently: "all categories shuffled eg random hero, random
// builder". fieldable_only keeps to units this save can field.
extern "C" void lb_teams_randomize(LbTeam* t, int fieldable_only) {
    static std::mt19937 rng(std::random_device{}());
    for (int s = 0; s < LB_TEAM_SLOTS; ++s) {
        std::vector<int> pool;
        for (int k = 0; k < slot_total(s); ++k) {
            const int u = slot_item(s, k).unit;
            if (!cand_on(slot_item(s, k))) continue;   // docs/410
            if (fieldable_only && lb_teams_unit_state(u) != 2) continue;
            pool.push_back(u);
        }
        if (pool.empty()) pool.push_back(kKingDefault[s]);
        t->pick[s] = (unsigned short)pool[std::uniform_int_distribution<int>(
            0, (int)pool.size() - 1)(rng)];
    }
}

// ---- the Custom and Random opponents (lb_patches.cpp's hooks) ---------------
//
// OpponentSetup's button frame is the choice. Frames 0x38..0x3D are the six
// factions (FUN_020C0374); the two new choices use frames the screen already
// has loaded from its common atlas: 0x8F, the crown, for Custom, and 0x15,
// the "?", for Random (docs/384).
constexpr int kFrameCustom = 0x8F, kFrameRandom = 0x15;

// docs/390: PORTRAITS OF THEIR OWN, 56x56 like the six factions: existing UI
// elements the same size as the Free Play mugshots -- the settings cog for
// Custom, a question mark for Random.
//
// The six portraits are 56x56 cells of UI/AllInOne/Iconic_FreeplayFaction
// (256x256, 8bpp, resource 0x4F), drawn as 3D quads through frame records
// 0x38..0x3D of the FE UIMgrData. The Options cog (Iconic_MainMenu, record
// 0x4C state 1, cell 0,64) and the LEGO Store "?" (Iconic_LegoStore, 0x50
// state 1, cell 64,0) are the same size in the same style. They are pasted,
// remapped to the faction atlas's palette, into two cells OpponentSetup never
// draws -- the grey Wizard (0,128) and the grey King (64,128) -- in the
// decoded file just before the game uploads it (lb_patches.cpp,
// xfer_override). Two unused records, 0x4A and 0x4B, are given the frame
// blocks of 0x31 and 0x30 state 2 (those same two cells) and resource 0x4F.
// The cartridge then draws them through its own button and quad path.
// Until both are in place the selector keeps the 24x24 crown and "?".
constexpr int kPortraitCustom = 0x4A, kPortraitRandom = 0x4B;
bool g_portrait_records = false;   // 0x4A/0x4B hold the frames now
bool g_portrait_pasted = false;    // the last faction atlas upload was patched

extern "C" int lb_teams_enemy_frame(int choice) {
    if (choice >= 0 && choice < 6) return 0x38 + choice;
    if (g_portrait_records && g_portrait_pasted)
        return choice == 6 ? kPortraitCustom : kPortraitRandom;
    return choice == 6 ? kFrameCustom : kFrameRandom;
}

extern "C" int lb_teams_enemy_choice(int frame) {
    if (frame >= 0x38 && frame <= 0x3D) return frame - 0x38;
    if (frame == kFrameCustom || frame == kPortraitCustom) return 6;
    if (frame == kFrameRandom || frame == kPortraitRandom) return 7;
    return -1;
}

// 1 for the two 24x24 frames, which need the button's rect shrunk.
extern "C" int lb_teams_enemy_frame_small(int frame) {
    return (frame == kFrameCustom || frame == kFrameRandom) ? 1 : 0;
}

namespace {
// The two portraits as indices into the faction atlas's palette, 56x56 each
// (cog first). 0 stays 0: the atlases share transparent index 0.
std::vector<uint8_t> g_cells;
int g_cells_state = 0;   // 0 not tried, 1 built, -1 failed

struct Bitmap { std::vector<uint8_t> px; uint16_t pal[256]; };

bool load_atlas(const char* stem, Bitmap* out) {
    std::string base = std::string("UI/AllInOne/") + stem;
    std::vector<uint8_t> ncbr, nclr;
    if (!nitro_file((base + ".NCBR").c_str(), &ncbr) || !comp_unwrap(&ncbr) ||
        !nitro_file((base + ".NCLR").c_str(), &nclr) || !comp_unwrap(&nclr))
        return false;
    const long c = section(ncbr, "RAHC"), pl = section(nclr, "TTLP");
    if (c < 0 || pl < 0) return false;
    const uint8_t* cb = ncbr.data() + c;
    const uint32_t dsz = rd32(cb + 24), doff = rd32(cb + 28);
    // 32x32 tiles, 8bpp (code 4), linear (+0x14 = 1): a 256x256 bitmap.
    if (rd16(cb + 8) != 32u || rd16(cb + 10) != 32u || rd32(cb + 12) != 4u ||
        rd32(cb + 20) != 1u || dsz < 65536u ||
        (size_t)c + 8u + doff + 65536u > ncbr.size())
        return false;
    out->px.assign(cb + 8 + doff, cb + 8 + doff + 65536);
    const uint8_t* pb = nclr.data() + pl;
    const uint32_t pdsz = rd32(pb + 16), pdoff = rd32(pb + 20);
    if (pdsz < 512u || (size_t)pl + 8u + pdoff + 512u > nclr.size()) return false;
    for (int i = 0; i < 256; ++i) out->pal[i] = rd16(pb + 8 + pdoff + 2 * i);
    return true;
}

bool build_cells() {
    if (g_cells_state != 0) return g_cells_state > 0;
    if (!g_rom) return false;
    Bitmap ff, mm, ls;
    if (!load_atlas("Iconic_FreeplayFaction", &ff) ||
        !load_atlas("Iconic_MainMenu", &mm) ||
        !load_atlas("Iconic_LegoStore", &ls)) {
        g_cells_state = -1;
        std::fprintf(stderr, "[lb_enemy] portraits: the Iconic atlases are "
                     "not the expected format; keeping the 24x24 frames\n");
        std::fflush(stderr);
        return false;
    }
    auto nearest = [&](uint16_t c) -> uint8_t {
        const int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        int best = 1, bd = 1 << 30;
        for (int i = 1; i < 256; ++i) {
            const uint16_t p = ff.pal[i];
            const int dr = r - (p & 31), dg = g - ((p >> 5) & 31),
                      db = b - ((p >> 10) & 31);
            const int d = dr * dr + dg * dg + db * db;
            if (d < bd) { bd = d; best = i; }
        }
        return (uint8_t)best;
    };
    g_cells.assign(2u * 56u * 56u, 0u);
    const struct { const Bitmap* src; int sx, sy; } cell[2] = {
        {&mm, 0, 64},    // the Options cog: Custom
        {&ls, 64, 0},    // the LEGO Store "?": Random
    };
    for (int k = 0; k < 2; ++k)
        for (int y = 0; y < 56; ++y)
            for (int x = 0; x < 56; ++x) {
                const uint8_t i = cell[k].src->px[(size_t)(cell[k].sy + y) * 256u +
                                                  (size_t)(cell[k].sx + x)];
                g_cells[(size_t)k * 3136u + (size_t)y * 56u + (size_t)x] =
                    i ? nearest(cell[k].src->pal[i]) : 0u;
            }
    g_cells_state = 1;
    return true;
}

// The FE UIMgrData's records: u16 count at +0, record i at +8 + i*0x1C, six
// u32 frame pointers (one per button state) and the u32 resource. The loaded
// copy's address is the word at 0x021581C0 (found live at the opponent
// screen: table 022464FC, 1,277 records; docs/390). An unused record has no
// frames and resource 0xFFFF.
constexpr uint32_t kUiTablePtr = 0x021581C0u;
constexpr uint32_t kRecSize = 0x1Cu;
}  // namespace

// docs/390: paste the two portraits into a decoded faction atlas at guest
// address `buf` (65,536 bytes). Returns 1 when done.
extern "C" int lb_teams_paste_portraits(uint32_t buf) {
    if (!build_cells() || !guest_ptr(buf) || !guest_ptr(buf + 65535u)) return 0;
    // Custom at the grey Wizard (0,128), Random at the grey King (64,128):
    // the cells of records 0x31 and 0x30 state 2.
    const uint32_t at[2] = {buf + 128u * 256u + 0u, buf + 128u * 256u + 64u};
    for (int k = 0; k < 2; ++k)
        for (uint32_t y = 0; y < 56u; ++y)
            for (uint32_t x = 0; x < 56u; x += 4u) {
                const uint8_t* s = &g_cells[(size_t)k * 3136u + y * 56u + x];
                bus_write_u32(at[k] + y * 256u + x,
                              (uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                              ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24));
            }
    g_portrait_pasted = true;
    return 1;
}

// The Random opponent's "?" (the LEGO Store cell, Iconic_LegoStore 64,0) in its
// own colours, 56x56 RGBA with alpha 0 where the cell is clear: the map
// select's Random entry draws it in place of the drawn map (lb_matchpic.cpp).
extern "C" int lb_teams_question_rgba(unsigned int out[56 * 56]) {
    static std::vector<unsigned int> cell;
    if (cell.empty()) {
        if (!g_rom) return 0;
        Bitmap ls;
        if (!load_atlas("Iconic_LegoStore", &ls)) return 0;
        cell.assign(56u * 56u, 0u);
        for (int y = 0; y < 56; ++y)
            for (int x = 0; x < 56; ++x) {
                const uint8_t i = ls.px[(size_t)y * 256u + (size_t)(64 + x)];
                if (!i) continue;
                const uint16_t c = ls.pal[i];
                const unsigned r = (c & 31u) << 3, g = ((c >> 5) & 31u) << 3,
                               b = ((c >> 10) & 31u) << 3;
                cell[(size_t)y * 56u + (size_t)x] = 0xFF000000u | (b << 16) | (g << 8) | r;
            }
    }
    std::memcpy(out, cell.data(), 56u * 56u * sizeof(unsigned int));
    return 1;
}

// docs/390: once a frame, make sure records 0x4A/0x4B carry the portraits'
// frames whenever the FE UIMgrData is loaded (it is reloaded after a battle,
// and the load fixup never fills a record the file leaves empty).
// docs/430: the army screen's two filters, for a creature, while its card is
// in (lb_patches.cpp's overrides of FUN_020EAC1C and FUN_020EB21C, from the
// army screen's list call only). -1: not ours, ask the game.
extern "C" int lb_teams_card_listable(int unit) {
    if (!g_cards_applied) return -1;
    // docs/436: any creature; the screen has already matched the slot to the
    // unit's own category, which the creature layer set from the slot.
    return unit == 169 || unit == 138 || unit == 155 || unit == 153 ? 1 : -1;
}
extern "C" int lb_teams_card_tier(int unit) {
    if (!g_cards_applied) return -1;
    const int s = lb_creature_slot(unit);   // Special One/Two/Three = 5/6/7
    return s >= 5 && s <= 7 ? s - 4 : -1;
}

extern "C" void lb_teams_portrait_tick(void) {
    cards_tick();   // docs/430: every frame; it does nothing while the option is off
    static int tick = 0;
    if (++tick < 15) return;
    tick = 0;
    const uint32_t base = bus_read_u32(kUiTablePtr);
    if (!guest_ptr(base) || bus_read_u16(base) <= 0x4Bu) {
        g_portrait_records = false;
        return;
    }
    auto rec = [&](uint32_t i) { return base + 8u + i * kRecSize; };
    // The faction portraits must be what this code expects, or nothing is
    // touched: 0x38 on resource 0x4F, 0x30/0x31 with a state-2 frame.
    const uint32_t f30 = bus_read_u32(rec(0x30) + 8u);
    const uint32_t f31 = bus_read_u32(rec(0x31) + 8u);
    if (bus_read_u32(rec(0x38) + 24u) != 0x4Fu || !guest_ptr(f30) ||
        !guest_ptr(f31)) {
        g_portrait_records = false;
        return;
    }
    const struct { uint32_t r, frame; } put[2] = {
        {(uint32_t)kPortraitCustom, f31}, {(uint32_t)kPortraitRandom, f30}};
    bool wrote = false;
    for (const auto& p : put) {
        const uint32_t a = rec(p.r);
        const uint32_t res = bus_read_u32(a + 24u);
        if (res == 0x4Fu && bus_read_u32(a) == p.frame) continue;
        if (res != 0xFFFFu && res != 0x4Fu) {   // not an empty record: leave it
            g_portrait_records = false;
            return;
        }
        for (uint32_t s = 0; s < 6u; ++s) bus_write_u32(a + 4u * s, p.frame);
        bus_write_u32(a + 24u, 0x4Fu);
        wrote = true;
    }
    if (wrote) {
        std::fprintf(stderr, "[lb_enemy] portrait records 0x4A/0x4B set "
                     "(UIMgrData %08X; frames %08X %08X)\n", base, f31, f30);
        std::fflush(stderr);
    }
    g_portrait_records = true;
}

// docs/429: RANDOM TEAM (TOURNEY). A random army from a seed: each slot takes
// one of its candidates, skipping every unit banned (everywhere in `bits`, or
// on the chosen map). Not limited to what a save has unlocked, so every
// machine -- which knows only the host's seed and rules -- draws the same
// team for a player. splitmix64 over seed and player, never a library's
// distribution.
static void team_generate(unsigned seed, int player, const uint8_t* bits, LbTeam* t) {
    unsigned long long st = ((unsigned long long)seed << 8) ^
                            (0xA5ull + (unsigned long long)player * 0x9E3779B1ull);
    auto next = [&]() {
        unsigned long long z = (st += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    for (int s = 0; s < LB_TEAM_SLOTS; ++s) {
        std::vector<int> pool;
        for (int k = 0; k < slot_total(s); ++k) {
            const Cand& c = slot_item(s, k);
            if (!cand_on(c)) continue;
            if (!king_default(c.unit) &&
                (ban_bit(bits, c.unit) || lb_maps_unit_banned_here(c.unit)))
                continue;
            pool.push_back(c.unit);
        }
        if (pool.empty()) pool.push_back(kKingDefault[s]);
        t->pick[s] = (unsigned short)pool[(size_t)(next() % pool.size())];
    }
}

static void team_to_record(const LbTeam& t, unsigned char rec[20]) {
    std::memset(rec, 0, 20);
    for (int s = 0; s < LB_TEAM_SLOTS; ++s) {
        rec[kSlots[s].rec_off] = (unsigned char)(t.pick[s] & 0xFF);
        rec[kSlots[s].rec_off + 1] = (unsigned char)(t.pick[s] >> 8);
    }
    const Cand* b = find_cand(t.pick[1], nullptr);
    rec[0x12] = 1;
    rec[0x13] = (unsigned char)(0x7C + (b ? b->faction : 0));
}

// The 20-byte record for an opponent: the picks in the army record's layout,
// +0x12 = 1, and +0x13 the builder's faction. FUN_02000E78 takes the buildings
// from the builder's faction (FUN_02002C78), and the Start handler names the
// opponent from +0x13 (GetFaction of the roster's +0x31), so both follow the
// builder. Returns 0 if the team is not valid.
extern "C" int lb_teams_enemy_army(int choice, unsigned char rec[20]) {
    LbTeam t = g_enemy;
    if (choice == 7) {
        unsigned seed = 0;
        if (lb_maps_tourney_team(&seed)) {   // docs/429
            uint8_t bits[kBanBytes];
            {
                std::lock_guard<std::mutex> lk(g_rules_mu);
                std::memcpy(bits, g_bans_local, sizeof bits);
            }
            team_generate(seed, 1, bits, &t);
            std::fprintf(stderr, "[lb_maps] Random team (tourney), seed %u: the "
                         "opponent fields %u %u %u %u %u | %u %u %u %u\n", seed,
                         t.pick[0], t.pick[1], t.pick[2], t.pick[3], t.pick[4],
                         t.pick[5], t.pick[6], t.pick[7], t.pick[8]);
            std::fflush(stderr);
        } else {
            lb_teams_randomize(&t, 0);
        }
    }
    if (!lb_teams_valid(&t)) lb_teams_king_default(&t);
    team_to_record(t, rec);
    return 1;
}

// SG-0033: the same record for a given team (Computer Player 3's saved team).
// An invalid team fields King's default, as the Enemy Team does.
extern "C" int lb_teams_team_army(const LbTeam* team, unsigned char rec[20]) {
    LbTeam t = *team;
    if (!lb_teams_valid(&t)) lb_teams_king_default(&t);
    team_to_record(t, rec);
    return 1;
}

// ---- bans ------------------------------------------------------------------

extern "C" void lb_teams_set_bans(const unsigned char* bits, int nbytes) {
    uint8_t next[kBanBytes] = {};
    if (bits && nbytes > 0)
        std::memcpy(next, bits, (size_t)(nbytes < kBanBytes ? nbytes : kBanBytes));
    for (uint16_t k : kKingDefault)   // "The King team cannot be banned"
        next[k >> 3] &= (uint8_t)~(1u << (k & 7));
    std::lock_guard<std::mutex> lk(g_rules_mu);
    std::memcpy(g_bans_local, next, sizeof next);
}

extern "C" int lb_teams_received_bans(unsigned char* bits, int nbytes,
                                      unsigned* from) {
    std::lock_guard<std::mutex> lk(g_rules_mu);
    if (!rules_fresh()) return 0;
    if (bits && nbytes > 0)
        std::memcpy(bits, g_bans_recv, (size_t)(nbytes < kBanBytes ? nbytes : kBanBytes));
    if (from) *from = g_bans_recv_from;
    return 1;
}

// The rules a HOST sends its players, from the wireless bridge's drain every
// 250 ms (apply_hooks, wifi_net.cpp): "LBR1", a version byte, the byte count,
// then the ban bits for blueprints 0..191. Nothing while not hosting.
// docs/406: then a flags count (1) and the host's rule flags (LB_RULE_*),
// which change the simulation and so must match on every machine. A build
// that predates them reads the bans and ignores the rest.
// docs/413: then the host's mod maps: a count, and for each its lobby key
// (u16), content hash (u64, little-endian) and name (a length byte, at most
// 31 bytes). A joiner translates the lobby's key through them to its own copy.
extern "C" int lb_rules_outgoing(unsigned char* buf, int cap) {
    if (!net_hosting(lb_net_guest_state())) return 0;
    int n = 6 + kBanBytes + 2;
    if (!buf || cap < n + 1) return 0;
    std::memcpy(buf, "LBR1", 4);
    buf[4] = 1;
    buf[5] = (unsigned char)kBanBytes;
    buf[6 + kBanBytes] = 1;
    buf[7 + kBanBytes] = (unsigned char)lb_rules_local_flags();
    const int count_at = n++;
    int maps = 0;
    for (int k = 0; k < lb_mods_count() && maps < 255; ++k) {
        const char* name = lb_mods_name(k);
        const int len = name ? (int)std::min<size_t>(std::strlen(name), 31) : 0;
        if (n + 11 + len > cap) break;
        const unsigned key = lb_mods_name_id(k);
        const unsigned long long h = lb_mods_hash(k);
        buf[n++] = (unsigned char)(key & 0xFF);
        buf[n++] = (unsigned char)(key >> 8);
        for (int b = 0; b < 8; ++b) buf[n++] = (unsigned char)(h >> (8 * b));
        buf[n++] = (unsigned char)len;
        if (len) std::memcpy(buf + n, name, (size_t)len);
        n += len;
        ++maps;
    }
    buf[count_at] = (unsigned char)maps;
    // docs/418: then one page of the host's balance: 'B', a version, the seed
    // (u64), the pack, the Same Builders source, the value count (u16), this
    // page and the page count, then up to 60 values of (record u16, field
    // u8, value u32). The pages go round, one per datagram.
    {
        static std::vector<int> vals(3 * 4096);
        static int page = 0;
        const int slot = lb_balance_effective_slot();
        const int builders = lb_balance_effective_builders();
        const int total = std::min(lb_balance_local_values(vals.data(), 4096), 4096);
        const int pages = total ? (total + kBalPerPage - 1) / kBalPerPage : 1;
        if (pages <= 255) {
            page = (page + 1) % pages;
            const int first = page * kBalPerPage;
            const int cnt = std::min(kBalPerPage, total - first);
            if (n + 18 + 7 * std::max(cnt, 0) <= cap) {
                const unsigned long long seed = lb_balance_local_seed(slot, builders);
                buf[n++] = 'B';
                buf[n++] = 1;
                for (int b = 0; b < 8; ++b) buf[n++] = (unsigned char)(seed >> (8 * b));
                buf[n++] = (unsigned char)slot;
                buf[n++] = (unsigned char)builders;
                buf[n++] = (unsigned char)(total & 0xFF);
                buf[n++] = (unsigned char)(total >> 8);
                buf[n++] = (unsigned char)page;
                buf[n++] = (unsigned char)pages;
                buf[n++] = (unsigned char)std::max(cnt, 0);
                for (int i = 0; i < cnt; ++i) {
                    const int* t = &vals[3 * (first + i)];
                    buf[n++] = (unsigned char)(t[0] & 0xFF);
                    buf[n++] = (unsigned char)(t[0] >> 8);
                    buf[n++] = (unsigned char)t[1];
                    for (int b = 0; b < 4; ++b) buf[n++] = (unsigned char)(t[2] >> (8 * b));
                }
            }
        }
    }
    // BR-0011: then the 'S' page, the other settings that change the
    // simulation: 'S', a version, the byte count (23), hotwire targets,
    // transport occupants, transport release, ranged attack boost, healing
    // fix, the cheats (u32), the ten build limits, and a check (u32).
    if (n + 26 <= cap) {
        unsigned char* const at = buf + n;
        at[0] = 'S';
        at[1] = 1;
        at[2] = 19;
        at[3] = (unsigned char)lb_setting_hotwire_mode();
        at[4] = (unsigned char)lb_setting_hotwire_transport();
        at[5] = (unsigned char)(lb_setting_transport_release() ? 1 : 0);
        // docs/433: 0 off, 1 on, 2 on without the blast. A build that predates
        // 2 reads it as on; the builds differ then and the 'V' page says so.
        at[6] = (unsigned char)std::min(std::max(lb_setting_attack_boost_ranged(), 0), 2);
        at[7] = (unsigned char)(lb_setting_fix_hotwired_healing() ? 1 : 0);
        const unsigned cm = (unsigned)lb_cheat_mask_live();
        for (int b = 0; b < 4; ++b) at[8 + b] = (unsigned char)(cm >> (8 * b));
        for (int i = 0; i < 10; ++i) {
            const int c = lb_setting_cap(i);
            at[12 + i] = (unsigned char)(c < 0 ? 0 : c > 255 ? 255 : c);
        }
        unsigned h = 2166136261u;
        for (int i = 3; i < 22; ++i) h = (h ^ at[i]) * 16777619u;
        for (int b = 0; b < 4; ++b) at[22 + b] = (unsigned char)(h >> (8 * b));
        n += 26;
        // docs/429: then the 'M' page: the map bans, the unit bans on some
        // maps only, and the tourney team flag and seed (lb_mapbans.cpp).
        n += lb_maps_wire_out(buf + n, cap - n);
    }
    // SG-0013 (docs/430): then the host's build, as its own small page: 'V', a
    // page version (1), a length byte and the text (at most 60 bytes), so a joiner
    // can say that the two builds differ. Older builds stop reading before it.
    {
        const char* v = lb_port_version();
        const int vl = (int)std::min<size_t>(std::strlen(v), 60);
        if (n + 3 + vl <= cap) {
            buf[n++] = 'V';
            buf[n++] = 1;
            buf[n++] = (unsigned char)vl;
            std::memcpy(buf + n, v, (size_t)vl);
            n += vl;
            // docs/436: then the 'C' page, after the 'V' page: 'C', a page
            // version (1), the army slot code of the Wolf, the Robot and the
            // Shark (0 the default, else 1 + the slot). Older builds stop
            // reading at the 'V' page; a host without it has every creature in
            // its default slot.
            if (n + 6 <= cap) {
                const int packed = lb_setting_creature_slots();
                buf[n++] = 'C';
                buf[n++] = 1;
                for (int i = 0; i < 4; ++i) buf[n++] = (unsigned char)((packed >> (4 * i)) & 15);
                // SG-0036: then the 'G' page: 'G', a version (1) and the Gold
                // Rush target (u32, 0 the game's 10,000). A host without it
                // plays the stock target.
                if (n + 6 <= cap) {
                    const unsigned g = (unsigned)std::max(lb_setting_gold_rush_target(), 0);
                    buf[n++] = 'G';
                    buf[n++] = 1;
                    for (int b = 0; b < 4; ++b) buf[n++] = (unsigned char)(g >> (8 * b));
                }
            }
        }
    }
    static int largest = 0;
    if (n > largest) {
        largest = n;
        std::fprintf(stderr, "[lb_teams] the largest rules datagram so far: %d "
                     "bytes\n", n);
        std::fflush(stderr);
    }
    std::lock_guard<std::mutex> lk(g_rules_mu);
    std::memcpy(buf + 6, g_bans_local, kBanBytes);
    return n;
}

// BR-0011: the host's other simulation settings for a joined machine: -1 when
// this machine decides (not joined), 0 when joined without an 'S' page (an
// older host, or its rules not yet in: all zero, the game's own), else 1 and
// the values. Not gated on freshness, like the flags: a late datagram mid-
// match must not change a rule on one machine only.
extern "C" int lb_rules_host_settings(LbHostSettings* out) {
    if (!net_joined(lb_net_guest_state())) return -1;
    std::lock_guard<std::mutex> lk(g_rules_mu);
    if (out) {
        if (g_set_ok) *out = g_set_recv;
        else std::memset(out, 0, sizeof *out);
    }
    return g_set_ok ? 1 : 0;
}

// BR-0004: the lobby was left; the rules it sent are not the next one's.
extern "C" void lb_patches_lobby_forget(void);   /* lb_patches.cpp */
extern "C" void lb_rules_on_leave(void) {
    lb_patches_lobby_forget();
    std::lock_guard<std::mutex> lk(g_rules_mu);
    g_bans_recv_ok = false;
    g_bans_recv_from = 0;
    g_flags_recv = 0;
    g_cslots_recv = 0;
    g_gold_recv = 0;
    g_maps_recv.clear();
    g_maps_recv_has = false;
    g_bal_done = false;
    g_bal_absent = false;
    g_bal_seed = 0ull;
    g_bal_total = g_bal_pages = 0;
    g_bal_values.clear();
    g_bal_seen.clear();
    g_set_ok = false;
    g_set_recv = LbHostSettings{};
    lb_maps_forget();   // docs/429
}

// docs/418: the host's balance for a joiner, once every page of one seed has
// arrived: the seed, pack, builders and up to `max` values (triples), and
// the value count is returned. -1 before then. An older host's rules carry
// no balance; that is the game's own (seed 0, nothing else).
extern "C" int lb_rules_host_balance(unsigned long long* seed, int* slot, int* builders,
                                     int* triples, int max) {
    std::lock_guard<std::mutex> lk(g_rules_mu);
    if (!rules_fresh()) return -1;
    if (g_bal_absent) {
        *seed = 0ull;
        *slot = *builders = 0;
        return 0;
    }
    if (!g_bal_done) return -1;
    *seed = g_bal_seed;
    *slot = g_bal_slot;
    *builders = g_bal_builders;
    const int n = std::min(g_bal_total, max);
    if (triples && n > 0)
        std::memcpy(triples, g_bal_values.data(), sizeof(int) * 3u * (size_t)n);
    return g_bal_total;
}

// docs/413: what the host's rules say of the lobby key `key`: 1, with its
// hash and name, when the host lists it; 0 when the host lists mod maps but
// not this one; -1 when the host's rules carry no list (a build before
// docs/413, whose keys can only be taken as they are); -2 before any fresh
// rules have arrived.
extern "C" int lb_rules_host_map(unsigned key, unsigned long long* hash,
                                 char* name, int cap) {
    std::lock_guard<std::mutex> lk(g_rules_mu);
    if (!rules_fresh()) return -2;
    if (!g_maps_recv_has) return -1;
    for (const RecvMap& m : g_maps_recv)
        if (m.key == key) {
            if (hash) *hash = m.hash;
            if (name && cap > 0) std::snprintf(name, (size_t)cap, "%s", m.name.c_str());
            return 1;
        }
    return 0;
}

// docs/406: the rule flags a joined machine runs with: the host's as last
// received (0 before any arrive), or -1 when this machine decides (offline,
// or hosting). Not gated on freshness, so a late datagram mid-match cannot
// switch a rule off on one machine only; every datagram rewrites them, so a
// new lobby's host replaces an old one's long before its match starts.
extern "C" int lb_rules_host_flags(void) {
    if (!net_joined(lb_net_guest_state())) return -1;
    std::lock_guard<std::mutex> lk(g_rules_mu);
    return g_bans_recv_ok ? g_flags_recv : 0;
}

// docs/436: the host's creature slots (packed, see lb_overlay.h): -1 when this
// machine decides, 0 for a host that sends none.
extern "C" int lb_rules_host_gold_target(void) {   // SG-0036
    if (!net_joined(lb_net_guest_state())) return -1;
    std::lock_guard<std::mutex> lk(g_rules_mu);
    return g_bans_recv_ok ? g_gold_recv : 0;
}

extern "C" int lb_rules_host_creature_slots(void) {
    if (!net_joined(lb_net_guest_state())) return -1;
    std::lock_guard<std::mutex> lk(g_rules_mu);
    return g_bans_recv_ok ? g_cslots_recv : 0;
}

extern "C" void lb_rules_incoming(const unsigned char* buf, int len,
                                  unsigned int sender) {
    if (!buf || len < 6 || std::memcmp(buf, "LBR1", 4) != 0) return;
    const int n = buf[5];
    if (n != kBanBytes || len < 6 + n) return;
    // Only a player who is joining or joined takes a host's rules.
    if (!net_joined(lb_net_guest_state())) return;
    std::lock_guard<std::mutex> lk(g_rules_mu);
    const bool changed = !g_bans_recv_ok ||
                         std::memcmp(g_bans_recv, buf + 6, kBanBytes) != 0;
    std::memcpy(g_bans_recv, buf + 6, kBanBytes);
    for (uint16_t k : kKingDefault)
        g_bans_recv[k >> 3] &= (uint8_t)~(1u << (k & 7));
    // docs/406: the flags after the bans; a host that sends none has none on.
    const int fn = len >= 7 + n ? buf[6 + n] : 0;
    const uint8_t flags = (fn >= 1 && len >= 8 + n) ? buf[7 + n] : 0;
    const bool flags_changed = !g_bans_recv_ok || flags != g_flags_recv;
    g_flags_recv = flags;
    // docs/413: the host's mod maps, after the flags.
    std::vector<RecvMap> maps;
    bool has_maps = false;
    int at = 6 + n + 2;   // docs/418: and where the balance page starts
    {
        if (fn >= 1 && len > at) {
            const int count = buf[at++];
            has_maps = true;
            for (int k = 0; k < count; ++k) {
                if (at + 11 > len) { has_maps = false; break; }
                RecvMap m;
                m.key = (unsigned)buf[at] | ((unsigned)buf[at + 1] << 8);
                m.hash = 0;
                for (int b = 0; b < 8; ++b)
                    m.hash |= (unsigned long long)buf[at + 2 + b] << (8 * b);
                const int nl = buf[at + 10];
                at += 11;
                if (nl > 31 || at + nl > len) { has_maps = false; break; }
                m.name.assign((const char*)buf + at, (size_t)nl);
                at += nl;
                maps.push_back(std::move(m));
            }
            if (!has_maps) maps.clear();
        }
    }
    // docs/418: then one page of the host's balance.
    bool bal_done_now = false;
    int s_at = -1;
    // A page is 17 bytes before its values (an empty set is exactly that).
    if (has_maps && at + 17 <= len && buf[at] == 'B' && buf[at + 1] == 1) {
        g_bal_absent = false;
        unsigned long long seed = 0;
        for (int b = 0; b < 8; ++b) seed |= (unsigned long long)buf[at + 2 + b] << (8 * b);
        const int slot = buf[at + 10], builders = buf[at + 11];
        const int total = buf[at + 12] | (buf[at + 13] << 8);
        const int page = buf[at + 14], pages = buf[at + 15], cnt = buf[at + 16];
        at += 17;
        const int page_end = at + 7 * cnt;   // SG-0013: whatever the values below do
        s_at = page_end;   // BR-0011: where the 'S' page would start
        if (seed != g_bal_seed || total != g_bal_total || pages != g_bal_pages) {
            g_bal_seed = seed;
            g_bal_slot = slot;
            g_bal_builders = builders;
            g_bal_total = total;
            g_bal_pages = pages;
            g_bal_values.assign(3u * (size_t)total, 0);
            g_bal_seen.assign((size_t)std::max(pages, 1), false);
            g_bal_done = false;
        }
        const int first = page * kBalPerPage;
        if (pages >= 1 && page < pages && first + cnt <= total && at + 7 * cnt <= len &&
            !g_bal_done) {
            for (int i = 0; i < cnt; ++i, at += 7) {
                int* t = &g_bal_values[3u * (size_t)(first + i)];
                t[0] = buf[at] | (buf[at + 1] << 8);
                t[1] = buf[at + 2];
                t[2] = (int)((uint32_t)buf[at + 3] | ((uint32_t)buf[at + 4] << 8) |
                             ((uint32_t)buf[at + 5] << 16) | ((uint32_t)buf[at + 6] << 24));
            }
            g_bal_seen[(size_t)page] = true;
            if (std::all_of(g_bal_seen.begin(), g_bal_seen.end(), [](bool b) { return b; })) {
                g_bal_done = true;
                bal_done_now = true;
            }
        }
        at = page_end;
    } else if (!has_maps || at >= len) {
        g_bal_absent = true;   // an older host: no balance in its rules
    }
    // BR-0011: then the 'S' page; an older host has none.
    bool set_changed = false;
    if (s_at >= 0 && s_at + 26 <= len && buf[s_at] == 'S' && buf[s_at + 1] == 1 &&
        buf[s_at + 2] == 19) {
        const unsigned char* q = buf + s_at;
        unsigned h = 2166136261u;
        for (int i = 3; i < 22; ++i) h = (h ^ q[i]) * 16777619u;
        const unsigned got = (unsigned)q[22] | ((unsigned)q[23] << 8) |
                             ((unsigned)q[24] << 16) | ((unsigned)q[25] << 24);
        if (h == got) {
            LbHostSettings v = {};
            v.hotwire_mode = q[3] <= 2 ? q[3] : 0;
            v.hotwire_transport = q[4] <= 2 ? q[4] : 0;
            v.transport_release = q[5] ? 1 : 0;
            v.attack_boost = q[6] <= 2 ? q[6] : 1;   // docs/433: 2 = no blast
            v.fix_healing = q[7] ? 1 : 0;
            v.cheats = ((unsigned)q[8] | ((unsigned)q[9] << 8) |
                        ((unsigned)q[10] << 16) | ((unsigned)q[11] << 24)) & 0x3FFFFu;
            for (int i = 0; i < 10; ++i) v.caps[i] = q[12 + i];
            set_changed = !g_set_ok || std::memcmp(&v, &g_set_recv, sizeof v) != 0;
            g_set_recv = v;
            g_set_ok = true;
            at = s_at + 26;   // SG-0013: the 'V' page follows it
        }
    }
    // docs/429: then the 'M' page, after the 'S' page; an older host has none.
    {
        const bool s_here = s_at >= 0 && s_at + 26 <= len && buf[s_at] == 'S' &&
                            buf[s_at + 1] == 1 && buf[s_at + 2] == 19;
        lb_maps_wire_in(s_here ? buf + s_at + 26 : buf, s_here ? len - (s_at + 26) : 0,
                        sender);
        // SG-0013: the 'V' page follows the 'M' page ('M', 1, a u16 length,
        // the payload and a u32 check), so step over it.
        const int m_at = s_at + 26;
        if (s_here && m_at + 4 <= len && buf[m_at] == 'M' && buf[m_at + 1] == 1) {
            const int m_end = m_at + 8 + (buf[m_at + 2] | (buf[m_at + 3] << 8));
            if (m_end <= len) at = m_end;
        }
    }
    if (set_changed) {
        const LbHostSettings& v = g_set_recv;
        std::fprintf(stderr, "[lb_teams] the host's settings from instance %u: "
                     "hotwire %d, occupants %d, release %d, attack boost %d, heal "
                     "fix %d, cheats %05X, caps %d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                     sender, v.hotwire_mode, v.hotwire_transport,
                     v.transport_release, v.attack_boost, v.fix_healing, v.cheats,
                     v.caps[0], v.caps[1], v.caps[2], v.caps[3], v.caps[4],
                     v.caps[5], v.caps[6], v.caps[7], v.caps[8], v.caps[9]);
        std::fflush(stderr);
    }
    // SG-0013 (docs/430): the host's build page ('V'), straight after the
    // balance page (or the maps, when the host sent no balance).
    std::string host_version;
    int c_at = -1;
    if (at + 3 <= len && buf[at] == 'V' && buf[at + 1] == 1 && at + 3 + buf[at + 2] <= len) {
        host_version.assign((const char*)buf + at + 3, buf[at + 2]);
        c_at = at + 3 + buf[at + 2];
    }
    // docs/436: the 'C' page after it: the creatures' army slots. None from an
    // older host: all in their default slots (0). Not gated on freshness, like
    // the flags.
    {
        int packed = 0;
        if (c_at >= 0 && c_at + 6 <= len && buf[c_at] == 'C' && buf[c_at + 1] == 1)
            for (int i = 0; i < 4; ++i) {
                const int c = buf[c_at + 2 + i];
                packed |= ((c >= 0 && c <= 8 && c != 2) ? c : 0) << (4 * i);
            }
        if (!g_bans_recv_ok || packed != g_cslots_recv) {
            std::fprintf(stderr, "[lb_teams] the host's creature slots from instance "
                         "%u: %03X\n", sender, (unsigned)packed);
            std::fflush(stderr);
        }
        g_cslots_recv = packed;
        // SG-0036: the 'G' page after it: the Gold Rush target.
        int gold = 0;
        const int g_at = c_at >= 0 ? c_at + 6 : -1;
        if (g_at >= 0 && g_at + 6 <= len && buf[g_at] == 'G' && buf[g_at + 1] == 1) {
            const unsigned g = (unsigned)buf[g_at + 2] | ((unsigned)buf[g_at + 3] << 8) |
                               ((unsigned)buf[g_at + 4] << 16) | ((unsigned)buf[g_at + 5] << 24);
            gold = g <= 1000000u ? (int)g : 0;
        }
        if (!g_bans_recv_ok || gold != g_gold_recv) {
            std::fprintf(stderr, "[lb_teams] the host's Gold Rush target from instance "
                         "%u: %d%s\n", sender, gold, gold ? "" : " (the game's)");
            std::fflush(stderr);
        }
        g_gold_recv = gold;
    }
    if (bal_done_now) {
        std::fprintf(stderr, "[lb_teams] the host's balance from instance %u: "
                     "seed %016llX, %d value(s) in %d page(s)\n", sender,
                     g_bal_seed, g_bal_total, g_bal_pages);
        std::fflush(stderr);
    }
    const bool maps_changed = !g_bans_recv_ok || has_maps != g_maps_recv_has ||
                              maps.size() != g_maps_recv.size();
    g_maps_recv = std::move(maps);
    g_maps_recv_has = has_maps;
    if (!host_version.empty()) {
        g_host_version = host_version;
        const std::string mine = lb_port_version();
        const auto now = std::chrono::steady_clock::now();
        if (host_version != mine &&
            (host_version != g_version_warned ||
             now - g_version_warned_at > std::chrono::minutes(5))) {
            g_version_warned = host_version;
            g_version_warned_at = now;
            std::fprintf(stderr, "[lb_teams] the host runs \"%s\", this build is \"%s\"\n",
                         host_version.c_str(), mine.c_str());
            std::fflush(stderr);
            const std::string msg = "The host runs version " + host_version +
                                    ", you run " + mine +
                                    ": the match may go out of sync.";
            lb_overlay_notice(msg.c_str());
        }
    }
    g_bans_recv_ok = true;
    g_bans_recv_from = sender;
    g_bans_recv_at = std::chrono::steady_clock::now();
    if (changed) {
        int count = 0;
        for (int b = 0; b < kBanBytes * 8; ++b) count += ban_bit(g_bans_recv, b);
        std::fprintf(stderr, "[lb_teams] rules from instance %u: %d unit(s) "
                     "banned\n", sender, count);
        std::fflush(stderr);
    }
    if (flags_changed) {
        std::fprintf(stderr, "[lb_teams] the host's rule flags from instance "
                     "%u: %02X\n", sender, flags);
        std::fflush(stderr);
    }
    if (maps_changed) {
        std::fprintf(stderr, "[lb_teams] the host's mod maps from instance %u: "
                     "%s%zu\n", sender, g_maps_recv_has ? "" : "no list, ",
                     g_maps_recv.size());
        std::fflush(stderr);
    }
}

// At a multiplayer match's start, for one player's army record (the one
// FUN_020F5168 built on its stack for FUN_02000E78): if any pick is banned,
// the whole army becomes Free Play's default King army -- "they are then
// changed to King once the game starts". Every machine makes this decision
// from the HOST's list (its own on the host, the received one on a client),
// so every machine builds the same rosters. Returns 1 when it replaced.
extern "C" int lb_teams_enforce_bans(unsigned char rec[20], char* why, int cap,
                                     int player) {
    // Test aid: LB_TEST_PLAYER_HERO=<blueprint> makes every player bring that
    // hero, to watch the bans (including those on the chosen map) act on it.
    const char* test_hero = std::getenv("LB_TEST_PLAYER_HERO");
    if (test_hero) {
        const int bp = std::atoi(test_hero);
        rec[kSlots[0].rec_off] = (unsigned char)(bp & 0xFF);
        rec[kSlots[0].rec_off + 1] = (unsigned char)(bp >> 8);
    }
    uint8_t bits[kBanBytes];
    const int net = lb_net_guest_state();
    {
        std::lock_guard<std::mutex> lk(g_rules_mu);
        if (net_hosting(net)) {
            std::memcpy(bits, g_bans_local, sizeof bits);
        } else {
            if (!rules_fresh()) return 0;
            std::memcpy(bits, g_bans_recv, sizeof bits);
        }
    }
    // Test aid: LB_TOURNEY_SELFTEST=1 draws 3000 teams under these bans on this
    // match's map and counts the banned units among them.
    if (std::getenv("LB_TOURNEY_SELFTEST")) {
        static bool done = false;
        if (!done) {
            done = true;
            int bad = 0, invalid = 0, sheriff = 0, teams = 0;
            for (unsigned sd = 1; sd <= 1000; ++sd)
                for (int pl = 0; pl < 3; ++pl) {
                    LbTeam t;
                    team_generate(sd, pl, bits, &t);
                    ++teams;
                    if (!lb_teams_valid(&t)) ++invalid;
                    for (int s = 0; s < LB_TEAM_SLOTS; ++s) {
                        const int u = t.pick[s];
                        if (!king_default(u) && (ban_bit(bits, u) || lb_maps_unit_banned_here(u)))
                            ++bad;
                        if (u == 177) ++sheriff;
                    }
                }
            std::fprintf(stderr, "[lb_maps] tourney self-test: %d teams, %d banned unit "
                         "pick(s), %d invalid team(s), Sheriff (177) drawn %d time(s)\n",
                         teams, bad, invalid, sheriff);
            std::fflush(stderr);
        }
    }
    // docs/429: Random team (tourney) replaces every player's army, from the
    // host's seed, skipping its bans (and the ones on this match's map).
    unsigned seed = 0;
    if (lb_maps_tourney_team(&seed) && player >= 0) {
        LbTeam t;
        team_generate(seed, player, bits, &t);
        team_to_record(t, rec);
        if (why && cap > 0)
            std::snprintf(why, (size_t)cap, "seed %u: %u %u %u %u %u | %u %u %u %u",
                          seed, t.pick[0], t.pick[1], t.pick[2], t.pick[3], t.pick[4],
                          t.pick[5], t.pick[6], t.pick[7], t.pick[8]);
        return 2;
    }
    std::string names;
    for (int s = 0; s < LB_TEAM_SLOTS; ++s) {
        const int bp = rec[kSlots[s].rec_off] | (rec[kSlots[s].rec_off + 1] << 8);
        // docs/410: a creature counts as banned unless the host allows them.
        // docs/436: or sits in a slot other than the host's choice for it.
        const bool creature = (bp == 169 || bp == 138 || bp == 155 || bp == 153) &&
                              (!lb_creatures_live() || lb_creature_slot(bp) != s);
        if (!king_default(bp) &&
            (ban_bit(bits, bp) || creature || lb_maps_unit_banned_here(bp))) {
            if (!names.empty()) names += ", ";
            names += lb_teams_unit_name(bp);
        }
    }
    if (names.empty()) {
        if (test_hero) {
            std::fprintf(stderr, "[lb_bans] test hero %s: player %d may bring it on "
                         "this map\n", test_hero, player);
            std::fflush(stderr);
        }
        return 0;
    }
    for (int s = 0; s < LB_TEAM_SLOTS; ++s) {
        rec[kSlots[s].rec_off] = (unsigned char)(kKingDefault[s] & 0xFF);
        rec[kSlots[s].rec_off + 1] = (unsigned char)(kKingDefault[s] >> 8);
    }
    rec[0x12] = 1;
    rec[0x13] = 0x7C;
    if (why && cap > 0) std::snprintf(why, (size_t)cap, "%s", names.c_str());
    return 1;
}
