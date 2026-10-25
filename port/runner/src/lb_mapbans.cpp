// legobattles-decomp: map bans, per-map unit bans, random maps and ban presets
// (docs/429).
//
// What the Ban Maps page, the per-map half of Ban Units and the Random entries
// of the map lists stand on:
//
//   * a map's identity: a stock map by its number (1..30, the cartridge's own
//     map table at 0x02147DDC), a mod map by its content hash (lb_mods.cpp), so
//     a ban means the same map on every machine;
//   * the ban lists as the settings keep them (comma lists of tokens "s5" and
//     "h<16 hex>") and the unit-on-these-maps list ("bp:tok+tok;bp:tok"),
//     parsed here, and the host's copy of them for a joiner (the 'M' page of
//     the rules datagram, lb_teams.cpp);
//   * the two bundled presets, resolved against the game's own names: the
//     English text of LOC/American_English.lng read from the player's ROM, so
//     the names match in every language and no id is typed by hand;
//   * the draw for the Random entries (lb_patches.cpp puts them in the list).
//
// Nothing here is cartridge DATA: the names are read from the player's ROM and
// the map table from guest RAM at run time.

#include "lb_overlay.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "runtime_arm.h"
}

extern "C" int lb_mods_players(int k);   /* lb_mods.cpp */
extern "C" int lb_rules_fresh(void);     /* lb_teams.cpp */

namespace {

constexpr uint32_t kMapTable = 0x02147DDCu;    // 30 entries of 28 bytes
constexpr uint32_t kMapEntry = 28u;
constexpr uint32_t kModNameBase = 0xBF8u;      // lb_mods.cpp: 0xBF8 + 2k
constexpr int kStockMaps = 30;
constexpr size_t kMaxModBans = 16;             // on the wire
constexpr size_t kMaxUnitBans = 12;            // on the wire
constexpr size_t kMaxUnitMods = 2;

std::mutex g_mu;

struct MapKey {
    int stock = 0;                    // 1..30, or 0 for a mod map
    unsigned long long hash = 0;
};

struct UnitMap {
    int bp = 0;
    uint32_t stock = 0;
    std::vector<unsigned long long> mods;
};

struct BanSet {
    uint32_t stock = 0;               // bit n-1: stock map n is banned
    std::vector<unsigned long long> mods;
    std::vector<UnitMap> um;          // a unit banned on these maps only

    bool map_banned(const MapKey& k) const {
        if (k.stock) return (stock >> (k.stock - 1)) & 1u;
        return std::find(mods.begin(), mods.end(), k.hash) != mods.end();
    }
    bool unit_banned_on(int bp, const MapKey& k) const {
        for (const UnitMap& u : um) {
            if (u.bp != bp) continue;
            if (k.stock) return (u.stock >> (k.stock - 1)) & 1u;
            return std::find(u.mods.begin(), u.mods.end(), k.hash) != u.mods.end();
        }
        return false;
    }
    bool empty() const { return !stock && mods.empty() && um.empty(); }
};

// ---- tokens ------------------------------------------------------------------

std::string token_of(const MapKey& k) {
    char b[32];
    if (k.stock) std::snprintf(b, sizeof b, "s%d", k.stock);
    else std::snprintf(b, sizeof b, "h%016llx", k.hash);
    return b;
}

bool parse_token(const std::string& t, MapKey* k) {
    if (t.size() >= 2 && t[0] == 's') {
        const int n = std::atoi(t.c_str() + 1);
        if (n < 1 || n > kStockMaps) return false;
        k->stock = n;
        k->hash = 0;
        return true;
    }
    if (t.size() >= 2 && t[0] == 'h') {
        char* end = nullptr;
        const unsigned long long h = std::strtoull(t.c_str() + 1, &end, 16);
        if (end == t.c_str() + 1 || h == 0) return false;
        k->stock = 0;
        k->hash = h;
        return true;
    }
    return false;
}

void split(const std::string& s, char sep, std::vector<std::string>* out) {
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos) b = s.size();
        if (b > a) out->push_back(s.substr(a, b - a));
        a = b + 1;
    }
}

void add_key(const MapKey& k, uint32_t* stock, std::vector<unsigned long long>* mods) {
    if (k.stock) *stock |= 1u << (k.stock - 1);
    else if (std::find(mods->begin(), mods->end(), k.hash) == mods->end())
        mods->push_back(k.hash);
}

BanSet parse_sets(const char* maps, const char* umb) {
    BanSet b;
    std::vector<std::string> v;
    if (maps) split(maps, ',', &v);
    for (const std::string& t : v) {
        MapKey k;
        if (parse_token(t, &k)) add_key(k, &b.stock, &b.mods);
    }
    v.clear();
    if (umb) split(umb, ';', &v);
    for (const std::string& e : v) {
        const size_t c = e.find(':');
        if (c == std::string::npos) continue;
        UnitMap u;
        u.bp = std::atoi(e.c_str());
        if (u.bp < 0 || u.bp >= 192) continue;
        std::vector<std::string> toks;
        split(e.substr(c + 1), '+', &toks);
        for (const std::string& t : toks) {
            MapKey k;
            if (parse_token(t, &k)) add_key(k, &u.stock, &u.mods);
        }
        if (u.stock || !u.mods.empty()) b.um.push_back(std::move(u));
    }
    return b;
}

// ---- the cartridge's maps -----------------------------------------------------

bool key_of_name_id(uint32_t id, MapKey* k) {
    if (id >= kModNameBase) {
        const int n = lb_mods_count();
        for (int i = 0; i < n; ++i)
            if (lb_mods_name_id(i) == id) {
                k->stock = 0;
                k->hash = lb_mods_hash(i);
                return k->hash != 0;
            }
        return false;
    }
    for (int n = 0; n < kStockMaps; ++n)
        if (bus_read_u32(kMapTable + kMapEntry * (uint32_t)n + 8u) == id) {
            k->stock = n + 1;
            k->hash = 0;
            return true;
        }
    return false;
}

bool key_of_mission(uint32_t mission, MapKey* k) {
    if (mission < 0x02000000u || mission >= 0x02400000u) return false;
    return key_of_name_id(bus_read_u32(mission + 0xF0u), k);
}

// ---- the English names, from the ROM -------------------------------------------

std::vector<uint8_t> g_eng;
bool g_eng_tried = false;

bool eng_load() {
    if (g_eng_tried) return !g_eng.empty();
    const int sz = lb_rom_read("LOC/American_English.lng", nullptr, 0);
    if (sz < 0x1000) return false;   // no ROM yet: try again later
    g_eng_tried = true;
    std::vector<uint8_t> raw((size_t)sz);
    lb_rom_read("LOC/American_English.lng", raw.data(), sz);
    if (raw.size() >= 4 && std::memcmp(raw.data(), "LANG", 4) != 0) {
        const int usz = lb_rom_read_unwrapped("LOC/American_English.lng", nullptr, 0);
        if (usz > 0) {
            raw.resize((size_t)usz);
            lb_rom_read_unwrapped("LOC/American_English.lng", raw.data(), usz);
        }
    }
    if (raw.size() < 0x3000 || std::memcmp(raw.data(), "LANG", 4) != 0) return false;
    g_eng.swap(raw);
    return true;
}

std::string eng_text(uint32_t id) {
    if (!eng_load() || id >= 3064u) return std::string();
    const uint8_t* b = g_eng.data();
    const uint32_t at = (uint32_t)b[0x0C + 4 * id] | ((uint32_t)b[0x0D + 4 * id] << 8) |
                        ((uint32_t)b[0x0E + 4 * id] << 16) |
                        ((uint32_t)b[0x0F + 4 * id] << 24);
    if (at >= g_eng.size()) return std::string();
    std::string s;
    for (uint32_t i = at; i < g_eng.size() && b[i] && s.size() < 96; ++i)
        s.push_back((char)b[i]);
    return s;
}

std::string fold(const std::string& in) {   // case-insensitive, without "The "
    std::string s;
    for (char c : in) s.push_back((c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c);
    if (s.compare(0, 4, "the ") == 0) s.erase(0, 4);
    return s;
}

// A stock map named `name` in English: its number, or 0.
int stock_by_name(const char* name) {
    const std::string want = fold(name);
    for (int n = 0; n < kStockMaps; ++n) {
        const uint32_t id = bus_read_u32(kMapTable + kMapEntry * (uint32_t)n + 8u);
        if (fold(eng_text(id)) == want) return n + 1;
    }
    return 0;
}

// ---- local and received state ----------------------------------------------------

BanSet g_local;                     // the settings' lists, parsed
BanSet g_recv;                      // the host's, as its 'M' page gave them
bool g_recv_ok = false;
bool g_local_tourney_team = false, g_recv_tourney_team = false;
unsigned g_recv_seed = 0;
bool g_random_maps = false;         // the setting
bool g_preset_active = false;       // a preset (not None) was applied
BanSet g_preset_maps;               // that preset's map bans (stock/mods only)
unsigned g_host_seed = 0;           // this machine's, while it hosts
bool g_was_hosting = false;

MapKey g_chosen;                    // the map the match will be on
bool g_chosen_ok = false;

unsigned fresh_seed() {
    static std::mt19937 rng(std::random_device{}() ^
                            (unsigned)std::chrono::steady_clock::now()
                                .time_since_epoch().count());
    unsigned s;
    do { s = (unsigned)rng(); } while (!s);
    return s;
}

struct Rng {   // splitmix64: the same draw on every machine
    unsigned long long s;
    unsigned long long next() {
        unsigned long long z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    size_t pick(size_t n) { return n ? (size_t)(next() % n) : 0; }
};

bool net_hosting_now() {
    const int s = lb_net_guest_state();
    return s == 10 || s == 2 || s == 3 || s == 4 || s == 11;
}
bool net_joined_now() {
    const int s = lb_net_guest_state();
    return s == 9 || s == 6 || s == 7 || s == 5 || s == 12;
}

bool demo_playing() { return lb_demo_state() == 2; }

// The list a match uses: a joiner's is the host's, anything else this machine's.
// Called with g_mu held.
// (`fresh` is lb_rules_fresh(), asked BEFORE taking g_mu: the rules lock is
// taken first when the host's page arrives.)
const BanSet* effective_locked(bool* known, bool fresh) {
    if (net_joined_now()) {
        *known = g_recv_ok && fresh;
        return &g_recv;
    }
    *known = true;
    return &g_local;
}

// Pool of the list being built: lb_patches.cpp feeds the missions the game's
// own unlock test passed, the first filter-site append closes it.
std::vector<uint32_t> g_pool;
bool g_pool_open = false;
int g_nx = 0;
int g_xkind[2] = {0, 0};
unsigned g_draws = 0;
// Every map the list could hold is banned (set when the pool closes): the bans
// are left out of that list, so it is never empty (the game's map select has
// no empty state) and Random still draws.
bool g_all_banned = false;

bool eligible(uint32_t m) {   // FUN_0204AFF8's own test
    // Two starts or more: the port lowered the game's own minimum of three
    // (apply_two_player_maps in lb_patches.cpp, docs/439 era), so the 2-player
    // Ninjago maps are drawn too.
    return bus_read_u32(m + 0x10u) < 3u && bus_read_u8(m + 0x158u) > 1u;
}

}  // namespace

// ---- the C API -----------------------------------------------------------------

extern "C" const char* lb_teams_loc_text(unsigned id);   /* lb_teams.cpp */

extern "C" void lb_maps_set_local(const char* banned_maps, const char* unit_map_bans,
                                  const char* preset_maps, int preset_active,
                                  int tourney_team, int random_maps) {
    BanSet b = parse_sets(banned_maps, unit_map_bans);
    // What the rules datagram can carry is all that counts, on the host too, so
    // every machine builds the same rosters.
    if (b.mods.size() > kMaxModBans) b.mods.resize(kMaxModBans);
    if (b.um.size() > kMaxUnitBans) b.um.resize(kMaxUnitBans);
    for (UnitMap& u : b.um)
        if (u.mods.size() > kMaxUnitMods) u.mods.resize(kMaxUnitMods);
    BanSet p = parse_sets(preset_maps, nullptr);
    std::lock_guard<std::mutex> lk(g_mu);
    g_local = std::move(b);
    g_preset_maps = std::move(p);
    g_preset_active = preset_active != 0;
    g_local_tourney_team = tourney_team != 0;
    g_random_maps = random_maps != 0;
}

// The list of maps the Ban Maps page shows: 30 stock, then the mod maps.
extern "C" int lb_maps_count(void) { return kStockMaps + lb_mods_count(); }

// Entry i: its token, its name (the game's language), its player count.
extern "C" int lb_maps_entry(int i, char* token, int tcap, char* name, int ncap,
                             int* players) {
    if (i < 0 || i >= lb_maps_count()) return 0;
    MapKey k;
    std::string nm;
    int pl = 0;
    if (i < kStockMaps) {
        k.stock = i + 1;
        const uint32_t id = bus_read_u32(kMapTable + kMapEntry * (uint32_t)i + 8u);
        nm = lb_teams_loc_text(id);
        pl = (int)bus_read_u32(kMapTable + kMapEntry * (uint32_t)i + 4u);
    } else {
        const int m = i - kStockMaps;
        k.hash = lb_mods_hash(m);
        nm = lb_mods_name(m) ? lb_mods_name(m) : "?";
        pl = lb_mods_players(m);
    }
    if (nm.empty()) nm = "Map " + std::to_string(i + 1);
    if (token && tcap > 0) std::snprintf(token, (size_t)tcap, "%s", token_of(k).c_str());
    if (name && ncap > 0) std::snprintf(name, (size_t)ncap, "%s", nm.c_str());
    if (players) *players = pl;
    return 1;
}

extern "C" int lb_maps_list_has(const char* list, const char* token) {
    std::vector<std::string> v;
    split(list ? list : "", ',', &v);
    return std::find(v.begin(), v.end(), std::string(token)) != v.end();
}

extern "C" void lb_maps_list_toggle(char* list, int cap, const char* token) {
    std::vector<std::string> v;
    split(list, ',', &v);
    auto it = std::find(v.begin(), v.end(), std::string(token));
    if (it != v.end()) v.erase(it);
    else v.push_back(token);
    std::string o;
    for (const std::string& t : v) {
        if (!o.empty()) o += ',';
        o += t;
    }
    std::snprintf(list, (size_t)cap, "%s", o.c_str());
}

// The maps a unit is banned on, as a '+' list; 1 when the unit has an entry.
extern "C" int lb_maps_umb_get(const char* umb, int bp, char* toks, int cap) {
    std::vector<std::string> v;
    split(umb ? umb : "", ';', &v);
    if (toks && cap > 0) toks[0] = '\0';
    for (const std::string& e : v) {
        const size_t c = e.find(':');
        if (c == std::string::npos || std::atoi(e.c_str()) != bp) continue;
        if (toks && cap > 0) std::snprintf(toks, (size_t)cap, "%s", e.c_str() + c + 1);
        return 1;
    }
    return 0;
}

// Replace (or, with empty `toks`, remove) a unit's entry.
extern "C" void lb_maps_umb_set(char* umb, int cap, int bp, const char* toks) {
    std::vector<std::string> v;
    split(umb, ';', &v);
    std::string o;
    for (const std::string& e : v) {
        const size_t c = e.find(':');
        if (c != std::string::npos && std::atoi(e.c_str()) == bp) continue;
        if (!o.empty()) o += ';';
        o += e;
    }
    if (toks && toks[0]) {
        if (!o.empty()) o += ';';
        o += std::to_string(bp) + ":" + toks;
    }
    std::snprintf(umb, (size_t)cap, "%s", o.c_str());
}

// A '+' or ',' list of tokens as names, for the page and the export.
extern "C" void lb_maps_names_of(const char* toks, char* out, int cap) {
    std::vector<std::string> v;
    std::string t = toks ? toks : "";
    for (char& c : t) if (c == '+') c = ',';
    split(t, ',', &v);
    std::string o;
    const int n = lb_maps_count();
    for (const std::string& tk : v) {
        std::string nm = tk;
        for (int i = 0; i < n; ++i) {
            char tok[32], name[64];
            if (lb_maps_entry(i, tok, sizeof tok, name, sizeof name, nullptr) && tk == tok) {
                nm = name;
                break;
            }
        }
        if (!o.empty()) o += ", ";
        o += nm;
    }
    std::snprintf(out, (size_t)cap, "%s", o.c_str());
}

// ---- presets ---------------------------------------------------------------------

namespace {

struct PerMap { const char* unit; std::vector<const char*> maps; };
struct PresetDef {
    const char* name;
    std::vector<const char*> units;
    std::vector<const char*> maps;
    std::vector<PerMap> per_map;
};

const std::vector<PresetDef>& presets() {
    static const std::vector<PresetDef> k = {
        {"None", {}, {}, {}},
        // The LEGO Battles Discord's competitive rules (2026-10), docs/429.
        {"LEGO Battles Discord: competitive (2026-10)",
         {"Space Criminal Leader", "Agent Chase", "Biff", "Gemma", "Classic Space",
          "Space Police Cruiser", "Space Criminal Hotrod"},
         {"Pond", "Avast", "Dead End", "Le Poisson", "Lost Lagoon", "Dark Waters",
          "Ruthless"},
         {{"Sheriff", {"River Crossing", "Exile", "Omega Quadrant", "Black Seas",
                       "Outbreak", "Kahuka's Strait"}}}},
    };
    return k;
}

}  // namespace

extern "C" int lb_maps_preset_count(void) { return (int)presets().size(); }
extern "C" const char* lb_maps_preset_name(int i) {
    return i >= 0 && i < (int)presets().size() ? presets()[(size_t)i].name : "";
}

// Resolve preset i against the game's own names. 1 and the three lists, or 0
// with the reason in `msg` (the game is not far enough along to name units).
extern "C" int lb_maps_preset_resolve(int i, char* units, int ucap, char* maps,
                                      int mcap, char* umb, int bcap, char* msg,
                                      int gcap) {
    if (i < 0 || i >= (int)presets().size()) return 0;
    const PresetDef& d = presets()[(size_t)i];
    units[0] = maps[0] = umb[0] = '\0';
    if (d.units.empty() && d.maps.empty() && d.per_map.empty()) {
        std::snprintf(msg, (size_t)gcap, "Cleared: no bans.");
        return 1;
    }
    if (!eng_load()) {
        std::snprintf(msg, (size_t)gcap, "The game's names are not readable yet.");
        return 0;
    }
    // The units: every candidate whose display name (blueprint +0x06) reads so
    // in English.
    struct Cand { int bp; std::string en; };
    std::vector<Cand> cands;
    for (int bp = 0; bp < 182; ++bp) {
        if (lb_teams_unit_slot(bp) < 0) continue;
        const int id = lb_teams_unit_name_id(bp);
        if (id < 0) {
            std::snprintf(msg, (size_t)gcap, "The game's units are not loaded yet.");
            return 0;
        }
        cands.push_back({bp, eng_text((uint32_t)id)});
    }
    auto unit_ids = [&](const char* name, std::vector<int>* out) {
        for (const Cand& c : cands)
            if (fold(c.en) == fold(name)) out->push_back(c.bp);
    };
    std::string us, ms, bs, log;
    auto add_tok = [](std::string* s, const std::string& t, char sep) {
        if (!s->empty()) *s += sep;
        *s += t;
    };
    for (const char* name : d.units) {
        std::vector<int> ids;
        unit_ids(name, &ids);
        log += std::string(" ") + name + "=";
        if (ids.empty()) log += "?";
        for (size_t k = 0; k < ids.size(); ++k) {
            if (lb_teams_is_king_default(ids[k])) { log += "king(skipped)"; continue; }
            add_tok(&us, std::to_string(ids[k]), ',');
            log += (k ? "/" : "") + std::to_string(ids[k]);
        }
    }
    log += " |";
    for (const char* name : d.maps) {
        const int n = stock_by_name(name);
        log += std::string(" ") + name + "=" + (n ? std::to_string(n) : "?");
        if (n) add_tok(&ms, "s" + std::to_string(n), ',');
    }
    log += " |";
    for (const PerMap& pm : d.per_map) {
        std::vector<int> ids;
        unit_ids(pm.unit, &ids);
        std::string toks;
        log += std::string(" ") + pm.unit + "=";
        for (size_t k = 0; k < ids.size(); ++k)
            log += (k ? "/" : "") + std::to_string(ids[k]);
        if (ids.empty()) log += "?";
        log += " on";
        for (const char* mn : pm.maps) {
            const int n = stock_by_name(mn);
            log += std::string(" ") + mn + "=" + (n ? std::to_string(n) : "?");
            if (n) add_tok(&toks, "s" + std::to_string(n), '+');
        }
        if (!toks.empty())
            for (int bp : ids) add_tok(&bs, std::to_string(bp) + ":" + toks, ';');
    }
    std::fprintf(stderr, "[lb_maps] preset \"%s\" resolved:%s\n", d.name, log.c_str());
    std::fflush(stderr);
    if (log.find("=?") != std::string::npos) {
        std::snprintf(msg, (size_t)gcap, "Some names in that preset are not in this "
                      "game; see the log.");
        // still applied: what resolved
    } else {
        std::snprintf(msg, (size_t)gcap, "Applied.");
    }
    std::snprintf(units, (size_t)ucap, "%s", us.c_str());
    std::snprintf(maps, (size_t)mcap, "%s", ms.c_str());
    std::snprintf(umb, (size_t)bcap, "%s", bs.c_str());
    return 1;
}

// ---- the list, the draw -----------------------------------------------------------

// A map is left out of the list: banned by this machine's lists.
extern "C" int lb_maps_mission_banned(unsigned mission) {
    if (demo_playing()) return 0;
    MapKey k;
    if (!key_of_mission(mission, &k)) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_all_banned) return 0;   // nothing would be left: the bans are set aside
    return g_local.map_banned(k) ? 1 : 0;
}

extern "C" void lb_maps_fill_begin(void) {
    g_pool.clear();
    g_pool_open = true;
    g_nx = 0;
}

extern "C" void lb_maps_pool_add(unsigned mission) {
    if (g_pool_open && g_pool.size() < 256) g_pool.push_back(mission);
}

namespace {

unsigned long long draw_seed(bool tourney) {
    // A tourney draw comes from the seed the host announces; the other from
    // the clock. Either way the draw number moves it on.
    unsigned s = 0;
    if (tourney) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (net_hosting_now()) {
            if (!g_host_seed) g_host_seed = fresh_seed();
            s = g_host_seed;
        }
    }
    if (!s) s = fresh_seed();
    return ((unsigned long long)s << 20) ^ (unsigned long long)(++g_draws);
}

// The mission for Random entry `kind` (0 Random, 1 Random (tourney)); 0 if none.
uint32_t draw_mission(int kind) {
    std::vector<uint32_t> pool;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (uint32_t m : g_pool) {
            if (!eligible(m)) continue;
            MapKey k;
            if (!key_of_mission(m, &k)) continue;
            if (!g_all_banned && g_local.map_banned(k)) continue;
            if (kind == 1 && g_preset_maps.map_banned(k)) continue;
            pool.push_back(m);
        }
    }
    if (pool.empty()) return 0u;
    Rng r{draw_seed(kind == 1)};
    const uint32_t m = pool[r.pick(pool.size())];
    MapKey k;
    key_of_mission(m, &k);
    char name[64] = "?";
    const uint32_t id = bus_read_u32(m + 0xF0u);
    for (int i = 0; i < lb_maps_count(); ++i) {
        char tok[32], nm[64];
        if (lb_maps_entry(i, tok, sizeof tok, nm, sizeof nm, nullptr) &&
            token_of(k) == tok) {
            std::snprintf(name, sizeof name, "%s", nm);
            break;
        }
    }
    std::fprintf(stderr, "[lb_maps] %s drew %s (%s, name id %u) from %zu map(s)\n",
                 kind == 1 ? "Random (tourney)" : "Random", name, token_of(k).c_str(),
                 (unsigned)id, pool.size());
    std::fflush(stderr);
    return m;
}

}  // namespace

// At the first append of the filter: the Random entries' missions. `out[i]`
// with kind i; returns the count.
extern "C" int lb_maps_fill_extras(unsigned out[2]) {
    g_pool_open = false;
    g_nx = 0;
    {
        // Would the bans leave this list empty? Then they are set aside for it.
        std::lock_guard<std::mutex> lk(g_mu);
        bool any_eligible = false, any_left = false;
        for (uint32_t m : g_pool) {
            if (!eligible(m)) continue;
            any_eligible = true;
            MapKey k;
            if (!key_of_mission(m, &k) || !g_local.map_banned(k)) {
                any_left = true;
                break;
            }
        }
        const bool was = g_all_banned;
        g_all_banned = any_eligible && !any_left && !demo_playing();
        if (g_all_banned && !was) {
            std::fprintf(stderr, "[lb_maps] every map is banned: the list shows them all\n");
            std::fflush(stderr);
            lb_overlay_notice("Every map is banned, so the bans are set aside and every map "
                              "is listed. Unban at least one map (Balance > Ban Maps).");
        }
    }
    if (demo_playing()) return 0;
    bool on, tourney;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        on = g_random_maps;
        tourney = g_preset_active;
    }
    if (!on) return 0;
    const uint32_t a = draw_mission(0);
    if (!a) return 0;
    out[g_nx] = a;
    g_xkind[g_nx++] = 0;
    if (tourney) {
        const uint32_t b = draw_mission(1);
        if (b) {
            out[g_nx] = b;
            g_xkind[g_nx++] = 1;
        }
    }
    return g_nx;
}

extern "C" int lb_maps_extra_count(void) { return g_nx; }

// The list's label for Random entry `idx`.
extern "C" const char* lb_maps_extra_label(int idx) {
    if (idx < 0 || idx >= g_nx) return nullptr;
    return g_xkind[idx] == 1 ? "Random (tourney)" : "Random";
}

// A fresh draw for entry `idx`, when the player lands on it. 0 if none.
extern "C" unsigned lb_maps_extra_redraw(int idx) {
    if (idx < 0 || idx >= g_nx) return 0u;
    return draw_mission(g_xkind[idx]);
}

// ---- the chosen map, and the unit bans on it -----------------------------------

extern "C" void lb_maps_note_pick_id(unsigned name_id, int online) {
    MapKey k;
    const bool ok = key_of_name_id(name_id, &k);
    std::lock_guard<std::mutex> lk(g_mu);
    if (online && ok && g_chosen_ok && g_chosen.stock == k.stock &&
        g_chosen.hash == k.hash)
        return;   // the lobby's packet repeats the same map every few frames
    if (!online) g_host_seed = fresh_seed();   // a Free Play match: its own seed
    g_chosen_ok = ok;
    if (ok) g_chosen = k;
    if (ok) {
        std::fprintf(stderr, "[lb_maps] the match is on map %s (name id %u)\n",
                     token_of(k).c_str(), name_id);
        std::fflush(stderr);
    }
}

extern "C" void lb_maps_forget(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_chosen_ok = false;
    g_recv = BanSet();
    g_recv_ok = false;
    g_recv_tourney_team = false;
    g_recv_seed = 0;
}

// Is unit `bp` banned on the map of this match by the lists that count
// (a host's or a joiner's host's)? Unknown map: no.
extern "C" int lb_maps_unit_banned_here(int bp) {
    if (demo_playing()) return 0;
    const bool fresh = lb_rules_fresh();
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_chosen_ok) return 0;
    bool known;
    const BanSet* b = effective_locked(&known, fresh);
    return known && b->unit_banned_on(bp, g_chosen) ? 1 : 0;
}

// Does the match run on a random team from a seed, and which seed? 1 and the
// seed, else 0.
extern "C" int lb_maps_tourney_team(unsigned* seed) {
    if (demo_playing()) return 0;
    const bool fresh = lb_rules_fresh();
    std::lock_guard<std::mutex> lk(g_mu);
    if (net_joined_now()) {
        if (!(g_recv_ok && g_recv_tourney_team && fresh)) return 0;
        if (seed) *seed = g_recv_seed;
        return 1;
    }
    if (!g_local_tourney_team) return 0;
    if (net_hosting_now()) {
        if (!g_host_seed) g_host_seed = fresh_seed();
        if (seed) *seed = g_host_seed;
    } else {
        if (!g_host_seed) g_host_seed = fresh_seed();   // Free Play: one per run...
        if (seed) *seed = g_host_seed;
    }
    return 1;
}

// A new seed (a new lobby, a new Free Play match).
extern "C" void lb_maps_new_seed(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_host_seed = fresh_seed();
}

extern "C" unsigned lb_maps_current_seed(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (net_joined_now()) return g_recv_ok ? g_recv_seed : 0u;
    return g_host_seed;
}

// The host's map bans as a joiner holds them, for the Ban Maps page: the
// token lists, or 0 when no fresh rules carry them.
extern "C" int lb_maps_received(char* maps, int mcap, char* umb, int ucap) {
    const bool fresh = lb_rules_fresh();
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_recv_ok || !fresh) return 0;
    std::string ms, bs;
    for (int n = 1; n <= kStockMaps; ++n)
        if ((g_recv.stock >> (n - 1)) & 1u) {
            if (!ms.empty()) ms += ',';
            ms += "s" + std::to_string(n);
        }
    for (unsigned long long h : g_recv.mods) {
        if (!ms.empty()) ms += ',';
        ms += token_of(MapKey{0, h});
    }
    for (const UnitMap& u : g_recv.um) {
        std::string t;
        for (int n = 1; n <= kStockMaps; ++n)
            if ((u.stock >> (n - 1)) & 1u) {
                if (!t.empty()) t += '+';
                t += "s" + std::to_string(n);
            }
        for (unsigned long long h : u.mods) {
            if (!t.empty()) t += '+';
            t += token_of(MapKey{0, h});
        }
        if (!bs.empty()) bs += ';';
        bs += std::to_string(u.bp) + ":" + t;
    }
    if (maps && mcap > 0) std::snprintf(maps, (size_t)mcap, "%s", ms.c_str());
    if (umb && ucap > 0) std::snprintf(umb, (size_t)ucap, "%s", bs.c_str());
    return 1;
}

// ---- the 'M' page of the host's rules -----------------------------------------------
//
//   'M', version (1), payload length (u16), payload, check (u32, FNV-1a of the
//   payload); payload = flags (bit 0: every player fields a random team from
//   the seed), seed (u32), banned stock maps (u32, bit n-1), banned mod maps
//   (u8 count, u64 hashes), then the units banned on some maps only (u8
//   count; each: blueprint u8, stock maps u32, u8 count, u64 hashes).
//   Appended after the 'S' page: a build before it reads everything it knows
//   and ignores the rest.

namespace {
unsigned fnv(const unsigned char* p, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}
void put32(unsigned char* b, int* n, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[(*n)++] = (unsigned char)(v >> (8 * i));
}
void put64(unsigned char* b, int* n, unsigned long long v) {
    for (int i = 0; i < 8; ++i) b[(*n)++] = (unsigned char)(v >> (8 * i));
}
}  // namespace

// Appends the page at buf[0..]; returns its size, or 0 if there is no room.
extern "C" int lb_maps_wire_out(unsigned char* buf, int cap) {
    std::lock_guard<std::mutex> lk(g_mu);
    const bool hosting = net_hosting_now();
    if (hosting && !g_was_hosting) {
        g_host_seed = fresh_seed();   // a new lobby, a new announced seed
        std::fprintf(stderr, "[lb_maps] tourney seed for this lobby: %u\n", g_host_seed);
        std::fflush(stderr);
    }
    g_was_hosting = hosting;
    if (!g_host_seed) g_host_seed = fresh_seed();
    unsigned char p[512];
    int n = 0;
    p[n++] = (unsigned char)(g_local_tourney_team ? 1 : 0);
    put32(p, &n, g_host_seed);
    put32(p, &n, g_local.stock);
    const size_t nm = std::min(g_local.mods.size(), kMaxModBans);
    p[n++] = (unsigned char)nm;
    for (size_t i = 0; i < nm; ++i) put64(p, &n, g_local.mods[i]);
    const int count_at = n++;
    int written = 0;
    for (size_t i = 0; i < g_local.um.size() && i < kMaxUnitBans; ++i) {
        const UnitMap& u = g_local.um[i];
        const size_t nh = std::min<size_t>(u.mods.size(), kMaxUnitMods);
        if (n + 6 + 8 * (int)nh > (int)sizeof p) break;
        p[n++] = (unsigned char)u.bp;
        put32(p, &n, u.stock);
        p[n++] = (unsigned char)nh;
        for (size_t h = 0; h < nh; ++h) put64(p, &n, u.mods[h]);
        ++written;
    }
    p[count_at] = (unsigned char)written;
    if (4 + n + 4 > cap) return 0;
    buf[0] = 'M';
    buf[1] = 1;
    buf[2] = (unsigned char)(n & 0xFF);
    buf[3] = (unsigned char)(n >> 8);
    std::memcpy(buf + 4, p, (size_t)n);
    const unsigned h = fnv(p, n);
    for (int i = 0; i < 4; ++i) buf[4 + n + i] = (unsigned char)(h >> (8 * i));
    return 4 + n + 4;
}

// Reads the page at p[0..len]; 1 if it was one, 0 if absent or damaged.
extern "C" int lb_maps_wire_in(const unsigned char* p, int len, unsigned sender) {
    bool ok = false;
    BanSet b;
    bool team = false;
    unsigned seed = 0;
    if (len >= 4 + 9 + 2 + 4 && p[0] == 'M' && p[1] == 1) {
        const int n = p[2] | (p[3] << 8);
        if (4 + n + 4 <= len) {
            unsigned got = 0;
            for (int i = 0; i < 4; ++i) got |= (unsigned)p[4 + n + i] << (8 * i);
            if (got == fnv(p + 4, n)) {
                const unsigned char* q = p + 4;
                int at = 0;
                auto r32 = [&](uint32_t* v) {
                    if (at + 4 > n) return false;
                    *v = (uint32_t)q[at] | ((uint32_t)q[at + 1] << 8) |
                         ((uint32_t)q[at + 2] << 16) | ((uint32_t)q[at + 3] << 24);
                    at += 4;
                    return true;
                };
                auto r64 = [&](unsigned long long* v) {
                    if (at + 8 > n) return false;
                    *v = 0;
                    for (int i = 0; i < 8; ++i) *v |= (unsigned long long)q[at + i] << (8 * i);
                    at += 8;
                    return true;
                };
                bool good = n >= 1;
                if (good) { team = (q[0] & 1) != 0; at = 1; }
                uint32_t v = 0;
                good = good && r32(&seed) && r32(&v);
                b.stock = v & 0x3FFFFFFFu;
                if (good && at < n) {
                    const int nm = q[at++];
                    for (int i = 0; good && i < nm; ++i) {
                        unsigned long long h = 0;
                        good = r64(&h);
                        if (good && h) b.mods.push_back(h);
                    }
                } else good = false;
                if (good && at < n) {
                    const int nu = q[at++];
                    for (int i = 0; good && i < nu; ++i) {
                        if (at + 6 > n) { good = false; break; }
                        UnitMap u;
                        u.bp = q[at++];
                        r32(&u.stock);
                        u.stock &= 0x3FFFFFFFu;
                        const int nh = q[at++];
                        for (int h = 0; good && h < nh; ++h) {
                            unsigned long long hh = 0;
                            good = r64(&hh);
                            if (good && hh) u.mods.push_back(hh);
                        }
                        if (good && u.bp < 192) b.um.push_back(std::move(u));
                    }
                } else good = false;
                ok = good;
            }
        }
    }
    std::lock_guard<std::mutex> lk(g_mu);
    const bool changed = ok != g_recv_ok || team != g_recv_tourney_team ||
                         seed != g_recv_seed || b.stock != g_recv.stock ||
                         b.mods != g_recv.mods || b.um.size() != g_recv.um.size();
    if (ok) {
        g_recv = std::move(b);
        g_recv_tourney_team = team;
        g_recv_seed = seed;
    } else {
        g_recv = BanSet();
        g_recv_tourney_team = false;
        g_recv_seed = 0;
    }
    g_recv_ok = ok;
    if (changed) {
        std::fprintf(stderr, "[lb_maps] the host's map rules from instance %u: %s%s"
                     "%d stock + %zu mod map(s) banned, %zu unit(s) banned on some "
                     "maps, seed %u%s\n", sender, ok ? "" : "none (",
                     ok ? "" : ") ", ok ? __builtin_popcount(g_recv.stock) : 0,
                     g_recv.mods.size(), g_recv.um.size(), g_recv_seed,
                     g_recv_tourney_team ? ", random teams (tourney)" : "");
        std::fflush(stderr);
    }
    return ok ? 1 : 0;
}
