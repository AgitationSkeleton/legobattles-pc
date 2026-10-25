// legobattles-decomp: mod maps from a mods folder (docs/391).
//
// The goal: non-vanilla maps placed in a mods folder are adapted
// automatically and appended as additional listings in the freeplay/lobby
// map select.
//
// WHAT A MOD MAP IS. The community mods ship as whole ROMs. The MapDump
// family (VANILLA/FUNLite/FactionWars/REbalance _MapDump.nds) replaces 18 of
// the 30 multiplayer slots, byte for byte the same in all four: Maps/mpNN.map,
// its minimap art (mpNNmini.NCGR/.NCBR, mpNNminiNT.NCGR/.NCBR, at the root),
// its BP/DetailTiles_mpNN.tbp, and its name in LOC/American_English.lng (id
// 1242 + 2 * (NN - 1)). The map table and the loader are in docs/391.
//
// WHAT THIS FILE DOES, before the cartridge is handed to the emulator:
//
//  * Scan <exe dir>/mods/*.nds. For every slot whose Maps/mpNN.map differs
//    from the player's own ROM, take that map and its companions from the mod
//    (a missing companion falls back to the player's stock one), its name,
//    its player count (the distinct players of its EVNT records, as the
//    loader reads them) and its world (the tileset it names). Identical maps
//    from several mods are one map.
//
//  * Give each one a stem "z/NN" and add it to the cartridge's file system as
//    Maps/z/NN.map, z/NNmini.NCGR ..., BP/DetailTiles_z/NN.tbp: three NEW
//    directories (docs/439: the 100th map onwards is "y/01", with three more
//    directories; at most 198 maps), whose files take new file ids after the last one. Every
//    stock file keeps its id; only the FNT (a directory is added to three
//    parents) and the FAT (entries appended) are rewritten, into the padding
//    after the last file, and the header points at them.
//
//  * Append two strings per map (its name and description) to every
//    LOC/*.lng, at ids 3064 + 2k and 3065 + 2k, by pointing those files' FAT
//    entries at extended copies. The game's own string lookup then shows them.
//
// lb_patches.cpp registers the missions (register_missions_override) with
// the game's own loop, so the Free Play list shows them after the stock 30.
// Multiplayer is excluded until both machines can be shown to have the same
// maps (docs/391).

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace {

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
void wr16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

bool ci_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
    }
    return true;
}

uint64_t fnv64(const std::vector<uint8_t>& b, uint64_t h = 1469598103934665603ull) {
    for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// ---- NitroFS -----------------------------------------------------------------
//
// FNT: a main table of 8-byte directory records (u32 sub-table offset, u16 first
// file id, u16 parent -- the root's holds the directory count), then each
// directory's sub-table: u8 len|0x80 name u16 dir-id for a directory, u8 len name
// for a file (ids consecutive from the directory's first), 0 to end.
struct Entry { bool dir; std::string name; int sub; };   // sub: directory index
struct Dir { uint16_t first = 0; int parent = -1; std::vector<Entry> entries; };

struct Nitro {
    std::vector<Dir> dirs;
    std::vector<std::pair<uint32_t, uint32_t>> fat;   // start, end
    bool parse(const uint8_t* fnt, size_t fnt_n, const uint8_t* fatb, size_t fat_n) {
        if (fnt_n < 8) return false;
        const unsigned count = rd16(fnt + 6);
        if (count == 0 || count > 4096 || (size_t)count * 8u > fnt_n) return false;
        dirs.assign(count, Dir{});
        for (unsigned d = 0; d < count; ++d) {
            const uint32_t off = rd32(fnt + 8u * d);
            dirs[d].first = rd16(fnt + 8u * d + 4u);
            if (d) dirs[d].parent = (int)(rd16(fnt + 8u * d + 6u) & 0x0FFFu);
            size_t p = off;
            while (p < fnt_n) {
                const uint8_t t = fnt[p++];
                if (!t) break;
                const unsigned len = t & 0x7Fu;
                if (p + len > fnt_n) return false;
                Entry e{(t & 0x80u) != 0, std::string((const char*)fnt + p, len), -1};
                p += len;
                if (e.dir) {
                    if (p + 2 > fnt_n) return false;
                    e.sub = (int)(rd16(fnt + p) & 0x0FFFu);
                    p += 2;
                }
                dirs[d].entries.push_back(e);
            }
        }
        fat.clear();
        for (size_t i = 0; i + 8 <= fat_n; i += 8)
            fat.emplace_back(rd32(fatb + i), rd32(fatb + i + 4));
        return true;
    }
    // A path, looked up as the game's FINDPATH does: case-insensitively, the
    // first match in directory order (docs/391). -1 if absent.
    int find(const std::string& path) const {
        int d = 0;
        size_t p = 0;
        while (true) {
            const size_t slash = path.find('/', p);
            const std::string part = path.substr(p, slash == std::string::npos
                                                        ? std::string::npos
                                                        : slash - p);
            int id = dirs[(size_t)d].first;
            bool found = false;
            for (const Entry& e : dirs[(size_t)d].entries) {
                if (slash != std::string::npos) {
                    if (e.dir && ci_eq(e.name, part)) {
                        d = e.sub;
                        found = true;
                        break;
                    }
                } else if (!e.dir) {
                    if (ci_eq(e.name, part)) return id;
                }
                if (!e.dir) ++id;
            }
            if (slash == std::string::npos || !found ||
                d < 0 || d >= (int)dirs.size())
                return -1;
            p = slash + 1;
        }
    }
    std::vector<uint8_t> serialize_fnt() const {
        std::vector<uint8_t> out(dirs.size() * 8u, 0u);
        for (size_t d = 0; d < dirs.size(); ++d) {
            wr32(&out[d * 8u], (uint32_t)out.size());
            wr16(&out[d * 8u + 4u], dirs[d].first);
            wr16(&out[d * 8u + 6u],
                 d == 0 ? (uint32_t)dirs.size() : 0xF000u + (uint32_t)dirs[d].parent);
            for (const Entry& e : dirs[d].entries) {
                out.push_back((uint8_t)(e.name.size() | (e.dir ? 0x80u : 0u)));
                out.insert(out.end(), e.name.begin(), e.name.end());
                if (e.dir) {
                    const uint32_t id = 0xF000u + (uint32_t)e.sub;
                    out.push_back((uint8_t)id);
                    out.push_back((uint8_t)(id >> 8));
                }
            }
            out.push_back(0u);
        }
        return out;
    }
    int child(int d, const char* name) const {
        for (const Entry& e : dirs[(size_t)d].entries)
            if (e.dir && ci_eq(e.name, name)) return e.sub;
        return -1;
    }
};

// A ROM image on disk, read piecewise: header, FNT and FAT whole, files on
// demand. The mod ROMs are 120 MB and only a few kilobytes of each are needed.
struct RomFile {
    FILE* f = nullptr;
    Nitro fs;
    ~RomFile() { if (f) std::fclose(f); }
    bool open(const std::string& path) {
        f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        uint8_t h[0x50];
        if (std::fread(h, 1, sizeof h, f) != sizeof h) return false;
        const uint32_t fo = rd32(h + 0x40), fs_n = rd32(h + 0x44);
        const uint32_t ao = rd32(h + 0x48), as_n = rd32(h + 0x4C);
        if (fs_n > (4u << 20) || as_n > (1u << 20)) return false;
        std::vector<uint8_t> fnt(fs_n), fat(as_n);
        if (std::fseek(f, (long)fo, SEEK_SET) != 0 ||
            std::fread(fnt.data(), 1, fnt.size(), f) != fnt.size() ||
            std::fseek(f, (long)ao, SEEK_SET) != 0 ||
            std::fread(fat.data(), 1, fat.size(), f) != fat.size())
            return false;
        return fs.parse(fnt.data(), fnt.size(), fat.data(), fat.size());
    }
    bool read(const std::string& path, std::vector<uint8_t>* out) {
        const int id = fs.find(path);
        if (id < 0 || (size_t)id >= fs.fat.size()) return false;
        const auto se = fs.fat[(size_t)id];
        if (se.second < se.first || se.second - se.first > (8u << 20)) return false;
        out->resize(se.second - se.first);
        return std::fseek(f, (long)se.first, SEEK_SET) == 0 &&
               std::fread(out->data(), 1, out->size(), f) == out->size();
    }
};

bool mem_read(const std::vector<uint8_t>& rom, const Nitro& fs,
              const std::string& path, std::vector<uint8_t>* out) {
    const int id = fs.find(path);
    if (id < 0 || (size_t)id >= fs.fat.size()) return false;
    const auto se = fs.fat[(size_t)id];
    if (se.second < se.first || se.second > rom.size()) return false;
    out->assign(rom.begin() + se.first, rom.begin() + se.second);
    return true;
}

// ---- COMP / LZ11: lb_teams.cpp's reader (docs/384), copied -------------------
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

// The map's world (from the tileset name at +0x0B) and its players (the
// distinct player bytes of its EVNT records' 9-byte conditions, lb_map.py's
// grammar). Returns false if the file is not a map the loader would accept.
bool map_facts(std::vector<uint8_t> m, int* world, int* players) {
    if (!comp_unwrap(&m) || m.size() < 0x40 || std::memcmp(m.data(), "MAP", 3) != 0 ||
        std::memcmp(m.data() + 3, "TERR", 4) != 0)
        return false;
    const char* ts = (const char*)m.data() + 0x0B;
    *world = !std::strncmp(ts, "Pirate", 6) ? 1 : !std::strncmp(ts, "Mars", 4) ? 2 : 0;
    const size_t cells = (size_t)m[7] * m[8];
    size_t q = 3 + 0x28 + 3 * cells;
    if (q + 2 > m.size()) return false;
    q += 2 + rd16(m.data() + q);
    q += 2 * cells;
    if (q + 8 > m.size() || std::memcmp(m.data() + q, "RRET", 4) != 0 ||
        std::memcmp(m.data() + q + 4, "EVNT", 4) != 0)
        return false;
    size_t p = q + 8;
    bool seen[256] = {false};
    while (p < m.size() && m[p] == 0x4C) {          // 'L'
        p += 2;                                      // mark, id
        if (p >= m.size()) return false;
        p += 1 + 2u * m[p];                          // points
        if (p >= m.size()) return false;
        const uint8_t area = m[p++];
        if (area) p += 4;
        static const int sizes[3] = {9, 8, 3};
        for (int k = 0; k < 3; ++k) {
            if (p >= m.size()) return false;
            const unsigned n = m[p++];
            if (p + (size_t)n * sizes[k] > m.size()) return false;
            if (k == 0)
                for (unsigned i = 0; i < n; ++i) seen[m[p + 9u * i + 2u]] = true;
            p += (size_t)n * sizes[k];
        }
    }
    int n = 0;
    for (bool s : seen) n += s ? 1 : 0;
    *players = n;
    return p < m.size() && m[p] == 0x21;
}

// String id `id` of a LANG file (Latin-1), or "".
std::string lng_string(const std::vector<uint8_t>& f, uint32_t id) {
    if (f.size() < 0x10 || std::memcmp(f.data(), "LANG", 4) != 0) return "";
    const uint32_t first = rd32(f.data() + 0x0C);
    const uint32_t count = (first - 0x0Cu) / 4u;
    if (id >= count || 0x0Cu + 4u * id + 4u > f.size()) return "";
    const uint32_t off = rd32(f.data() + 0x0C + 4u * id);
    std::string s;
    for (size_t i = off; i < f.size() && f[i]; ++i) s.push_back((char)f[i]);
    return s;
}

// A LANG file with `extra` strings appended as ids count, count+1, ...
std::vector<uint8_t> lng_extend(const std::vector<uint8_t>& f,
                                const std::vector<std::string>& extra,
                                uint32_t* first_new) {
    const uint32_t first = rd32(f.data() + 0x0C);
    const uint32_t count = (first - 0x0Cu) / 4u;
    const uint32_t grow = 4u * (uint32_t)extra.size();
    std::vector<uint8_t> out(f.begin(), f.begin() + 0x0C);
    out.resize(0x0C + 4u * (count + (uint32_t)extra.size()));
    for (uint32_t i = 0; i < count; ++i)
        wr32(&out[0x0C + 4u * i], rd32(f.data() + 0x0C + 4u * i) + grow);
    out.insert(out.end(), f.begin() + first, f.end());
    for (size_t k = 0; k < extra.size(); ++k) {
        wr32(&out[0x0C + 4u * (count + (uint32_t)k)], (uint32_t)out.size());
        out.insert(out.end(), extra[k].begin(), extra[k].end());
        out.push_back(0u);
    }
    *first_new = count;
    return out;
}

// ---- the mod maps ---------------------------------------------------------------
struct ModMap {
    int slot = 0;                        // the mpNN it replaced
    std::string name, desc, source;      // display name, description, mod file
    int players = 0, world = 0;
    std::vector<uint8_t> map, detail, mini[4];
    uint64_t hash = 0;
    uint32_t name_id = 0, desc_id = 0;   // LOC ids, set by the cartridge build
};
const char* const kMiniExt[4] = {"mini.NCGR", "mini.NCBR", "miniNT.NCGR",
                                 "miniNT.NCBR"};
std::vector<ModMap> g_maps;
// docs/439: the stem of mod map k is "<letter>/NN": 99 maps to a letter, 'z'
// first, then 'y'. (The game copies the stem from a 5-byte "mpNN" string, so
// it is four characters.) Two letters are two groups of three directories.
constexpr int kStemGroup = 99;
constexpr int kStemGroups = 2;
// The Free Play list draws no name for its 129th and later items (measured,
// docs/439), so 30 stock + 96 mod maps + 2 Random entries is the most that
// shows names. Stems, ids and registration take kStemGroup * kStemGroups.
constexpr int kMaxMods = 96;
void stem_of(int k, char out[8]) {
    std::snprintf(out, 8, "%c/%02d", 'z' - k / kStemGroup, k % kStemGroup + 1);
}
std::string g_status = "no mods folder";

std::string exe_dir() {
    char* base = SDL_GetBasePath();
    std::string d = base ? base : "";
    if (base) SDL_free(base);
    return d;
}

// docs/413, docs/431: a loose map folder read as the loader reads it. `stock_read`
// supplies a stock ROM file (the slot's detail tiles when the folder has none).
// False when the folder is not a map (exactly one .map and its four minimap
// files, a map the loader accepts).
bool loose_folder_map(const std::filesystem::path& f,
                      const std::function<bool(const std::string&, std::vector<uint8_t>*)>& stock_read,
                      ModMap* out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    ModMap& m = *out;
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    auto slurp = [](const fs::path& p, std::vector<uint8_t>* o) {
        std::error_code e2;
        const auto sz = fs::file_size(p, e2);
        if (e2 || sz == 0 || sz > (8u << 20)) return false;
        FILE* fp = std::fopen(p.string().c_str(), "rb");
        if (!fp) return false;
        o->resize((size_t)sz);
        const bool ok = std::fread(o->data(), 1, o->size(), fp) == o->size();
        std::fclose(fp);
        return ok;
    };
            std::vector<fs::path> maps;
            std::vector<fs::path> files;
            for (const auto& e : fs::directory_iterator(f, ec)) {
                if (!e.is_regular_file(ec)) continue;
                files.push_back(e.path());
                // An empty .map is a leftover, not a second map (the archive's
                // Supply Lines 1 ships a 0-byte spy1.map beside Sup1.map).
                if (lower(e.path().extension().string()) == ".map" &&
                    fs::file_size(e.path(), ec) > 0 && !ec)
                    maps.push_back(e.path());
            }
            if (maps.size() != 1) return false;
            const std::string stem = maps[0].stem().string();
            auto find = [&](const std::string& name) -> fs::path {
                const std::string want = lower(name);
                for (const fs::path& p : files)
                    if (lower(p.filename().string()) == want) return p;
                return fs::path();
            };
            // The stock slot the map stands in for: mpNN.map, else a folder
            // named for it, "18-Better Outbreak" (docs/419). 0 for none.
            int slot = 0;
            if (lower(stem).rfind("mp", 0) == 0) slot = std::atoi(stem.c_str() + 2);
            {
                const std::string fn = f.filename().string();
                if ((slot < 1 || slot > 30) && !fn.empty() && std::isdigit((unsigned char)fn[0])) {
                    size_t d = 0;
                    while (d < fn.size() && std::isdigit((unsigned char)fn[d])) ++d;
                    if (d < fn.size() && (fn[d] == '-' || fn[d] == ' '))
                        slot = std::atoi(fn.c_str());
                }
            }
            if (slot < 1 || slot > 30) slot = 0;
            bool ok = slurp(maps[0], &m.map);
            int own_minis = 0;
            for (int k = 0; ok && k < 4; ++k) {
                const fs::path p = find(stem + kMiniExt[k]);
                if (!p.empty() && slurp(p, &m.mini[k])) ++own_minis;
            }
            if (ok && own_minis != 4) {
                // No minimap of its own (the Better Free Play pack's 29-Better
                // Dead Marsh, a shipyard-only edit of mp29): a slot map takes
                // its stock slot's, as it takes the slot's detail tiles below.
                char nm[40];
                for (int k = 0; ok && k < 4; ++k) {
                    std::snprintf(nm, sizeof nm, "mp%02d%s", slot, kMiniExt[k]);
                    ok = slot && stock_read(nm, &m.mini[k]) && !m.mini[k].empty();
                }
                if (ok)
                    std::fprintf(stderr, "[lb_mods] %s: no minimap of its own; slot mp%02d's\n",
                                 f.filename().string().c_str(), slot);
            }
            if (!ok) return false;
            if (!map_facts(m.map, &m.world, &m.players) || m.players < 2) {
                std::fprintf(stderr, "[lb_mods] %s: %s.map is not a map the game "
                             "reads, skipped\n", f.filename().string().c_str(),
                             stem.c_str());
                return false;
            }
            const fs::path dt = find("DetailTiles_" + stem + ".tbp");
            if (dt.empty() || !slurp(dt, &m.detail)) {
                const int s = slot;   // docs/419: the slot found above
                char stock[40];
                std::snprintf(stock, sizeof stock, "BP/DetailTiles_mp%02d.tbp",
                              s >= 1 && s <= 30 ? s : 1);
                stock_read(stock, &m.detail);
                std::fprintf(stderr, "[lb_mods] %s: no detail tiles of its own; "
                             "slot mp%02d's\n", f.filename().string().c_str(),
                             s >= 1 && s <= 30 ? s : 1);
            }
            m.name = f.filename().string();
            m.hash = fnv64(m.map);
            m.hash = fnv64(m.detail, m.hash);
            for (auto& v : m.mini) m.hash = fnv64(v, m.hash);
    return true;
}


void scan(const std::vector<uint8_t>& rom, const Nitro& own) {
    namespace fs = std::filesystem;
    // docs/413: LB_MODS_DIR=<folder> reads another mods folder, so two
    // instances in one build folder can hold different maps. Test aid only.
    const char* alt = std::getenv("LB_MODS_DIR");
    const fs::path dir = alt && *alt ? fs::path(alt) : fs::path(exe_dir()) / "mods";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<fs::path> roms;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (e.is_regular_file(ec) && ext == ".nds") roms.push_back(e.path());
    }
    std::sort(roms.begin(), roms.end());
    std::vector<uint8_t> stock_lng;
    mem_read(rom, own, "LOC/American_English.lng", &stock_lng);
    int files = 0;
    for (const fs::path& p : roms) {
        RomFile mod;
        if (!mod.open(p.string())) {
            std::fprintf(stderr, "[lb_mods] %s: not a NitroFS image, skipped\n",
                         p.filename().string().c_str());
            continue;
        }
        ++files;
        std::vector<uint8_t> lng;
        mod.read("LOC/American_English.lng", &lng);
        for (int slot = 1; slot <= 30; ++slot) {
            char stem[8];
            std::snprintf(stem, sizeof stem, "mp%02d", slot);
            ModMap m;
            std::vector<uint8_t> stock;
            if (!mod.read(std::string("Maps/") + stem + ".map", &m.map)) continue;
            if (mem_read(rom, own, std::string("Maps/") + stem + ".map", &stock) &&
                stock == m.map)
                continue;                                  // the stock map
            if (!map_facts(m.map, &m.world, &m.players) || m.players < 2) {
                std::fprintf(stderr, "[lb_mods] %s: %s.map is not a map the "
                             "game reads, skipped\n",
                             p.filename().string().c_str(), stem);
                continue;
            }
            const std::string dt = std::string("BP/DetailTiles_") + stem + ".tbp";
            if (!mod.read(dt, &m.detail)) mem_read(rom, own, dt, &m.detail);
            bool ok = true;
            for (int k = 0; k < 4; ++k)
                if (!mod.read(std::string(stem) + kMiniExt[k], &m.mini[k]) &&
                    !mem_read(rom, own, std::string(stem) + kMiniExt[k], &m.mini[k]))
                    ok = false;
            if (!ok) continue;
            m.slot = slot;
            m.source = p.filename().string();
            m.name = lng_string(lng, 1242u + 2u * (uint32_t)(slot - 1));
            const std::string stock_name =
                lng_string(stock_lng, 1242u + 2u * (uint32_t)(slot - 1));
            for (char& c : m.name) if (c == '_') c = ' ';
            if (m.name.empty() || m.name == stock_name)
                m.name = (stock_name.empty() ? std::string(stem) : stock_name) +
                         " (mod)";
            m.hash = fnv64(m.map);
            m.hash = fnv64(m.detail, m.hash);
            for (auto& v : m.mini) m.hash = fnv64(v, m.hash);
            auto same = std::find_if(g_maps.begin(), g_maps.end(),
                                     [&](const ModMap& o) { return o.hash == m.hash; });
            if (same != g_maps.end()) {
                if (same->source.find(m.source) == std::string::npos &&
                    same->source.size() < 160)
                    same->source += ", " + m.source;
                continue;
            }
            g_maps.push_back(std::move(m));
        }
    }
    // docs/413: LOOSE MAP FOLDERS, as the community map archive ships them
    // (github.com/Ryncat2000/Lego-Battles-Custom-Map-Archive): mods/maps/, at
    // any depth, so its zips can simply be extracted there. A folder is a map
    // when it holds exactly one .map and that map's four minimap files
    // (<stem>mini.NCGR/.NCBR, <stem>miniNT.NCGR/.NCBR); a folder of story maps
    // without them (the archive's difficulty patch) is not one. Its detail
    // tiles are DetailTiles_<stem>.tbp when it has them; without, a map made
    // to replace slot mpNN takes that slot's stock ones, any other mp01's
    // (the archive: "if this is included you MUST replace it"). The folder's
    // name is the map's.
    {
        auto lower = [](std::string s) {
            for (char& c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };
        auto slurp = [](const fs::path& p, std::vector<uint8_t>* out) {
            std::error_code e2;
            const auto sz = fs::file_size(p, e2);
            if (e2 || sz == 0 || sz > (8u << 20)) return false;
            FILE* f = std::fopen(p.string().c_str(), "rb");
            if (!f) return false;
            out->resize((size_t)sz);
            const bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
            std::fclose(f);
            return ok;
        };
        // A map folder anywhere under mods/ counts (mods/maps/ is the documented
        // place, but a player dropping the folders straight into mods/ saw no
        // maps); mods/cache/ -- the map browser's downloaded zips -- is skipped.
        const fs::path loose = dir;
        std::vector<fs::path> folders;
        if (fs::is_directory(loose, ec))
            for (auto it = fs::recursive_directory_iterator(loose, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_directory(ec)) continue;
                if (it.depth() == 0 && lower(it->path().filename().string()) == "cache") {
                    it.disable_recursion_pending();
                    continue;
                }
                folders.push_back(it->path());
            }
        std::sort(folders.begin(), folders.end());
        int loose_n = 0;
        for (const fs::path& f : folders) {
            ModMap m;
            if (!loose_folder_map(f, [&](const std::string& p, std::vector<uint8_t>* o) {
                    return mem_read(rom, own, p, o);
                }, &m))
                continue;
            m.slot = 1000 + loose_n;
            m.source = "mods/" + f.lexically_relative(loose).generic_string();
            if (std::any_of(g_maps.begin(), g_maps.end(),
                            [&](const ModMap& o) { return o.hash == m.hash; }))
                continue;
            g_maps.push_back(std::move(m));
            ++loose_n;
        }
        if (loose_n) ++files;   // the folder counts as one source in the status
    }

    // A stable order whatever the folder holds: the slot, then the content.
    std::sort(g_maps.begin(), g_maps.end(), [](const ModMap& a, const ModMap& b) {
        return a.slot != b.slot ? a.slot < b.slot : a.hash < b.hash;
    });
    size_t dropped = 0;
    if (g_maps.size() > (size_t)kMaxMods) {   // docs/439: see kMaxMods
        dropped = g_maps.size() - (size_t)kMaxMods;
        for (size_t k = (size_t)kMaxMods; k < g_maps.size(); ++k)
            std::fprintf(stderr, "[lb_mods] over the %d-map limit, not loaded: %s\n",
                         kMaxMods, g_maps[k].source.c_str());
        g_maps.resize((size_t)kMaxMods);
    }
    for (ModMap& m : g_maps)
        m.desc = "A map from " + m.source + ".";
    char buf[96];
    if (dropped)
        std::snprintf(buf, sizeof buf, "%zu mod map(s) from %d source(s), %zu over "
                      "the limit", g_maps.size(), files, dropped);
    else
        std::snprintf(buf, sizeof buf, "%zu mod map(s) from %d source(s)",
                      g_maps.size(), files);   // docs/413: ROMs and mods/maps
    g_status = buf;
}

// CRC-16 (poly 0xA001, init 0xFFFF) of the header's first 0x15E bytes.
uint16_t header_crc(const uint8_t* h) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < 0x15E; ++i) {
        crc ^= h[i];
        for (int b = 0; b < 8; ++b) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

}  // namespace

// docs/426: a decompressed BP/Entities.ebp ("BPNZ...") from a .nds (any mod) or
// from an Entities.ebp, raw or in the game's COMP wrapper. Empty `why` on
// success; otherwise what failed.
bool lb_mods_blueprint_file(const char* path, std::vector<uint8_t>* out,
                            std::string* why) {
    std::vector<uint8_t> b;
    FILE* f = path ? std::fopen(path, "rb") : nullptr;
    if (!f) { *why = "cannot open that file"; return false; }
    uint8_t magic[4] = {0, 0, 0, 0};
    const size_t got = std::fread(magic, 1, 4, f);
    std::fclose(f);
    if (got == 4 && (std::memcmp(magic, "PMOC", 4) == 0 ||
                     std::memcmp(magic, "BPNZ", 4) == 0)) {
        f = std::fopen(path, "rb");
        if (!f) { *why = "cannot open that file"; return false; }
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n <= 0 || n > (8 << 20)) { std::fclose(f); *why = "wrong size for a blueprint"; return false; }
        b.resize((size_t)n);
        const bool ok = std::fread(b.data(), 1, b.size(), f) == b.size();
        std::fclose(f);
        if (!ok) { *why = "read failed"; return false; }
    } else {
        RomFile rom;
        if (!rom.open(path) || !rom.read("BP/Entities.ebp", &b)) {
            *why = "not a ROM with BP/Entities.ebp, nor an Entities.ebp";
            return false;
        }
    }
    if (!comp_unwrap(&b) || b.size() < 4 || std::memcmp(b.data(), "BPNZ", 4) != 0) {
        *why = "that is not a LEGO Battles Entities.ebp";
        return false;
    }
    *out = std::move(b);
    return true;
}

// The player's own ROM's blueprint file, decompressed.
extern "C" int lb_rom_read(const char* path, unsigned char* out, int cap);
bool lb_mods_blueprint_own(std::vector<uint8_t>* out) {
    const int n = lb_rom_read("BP/Entities.ebp", nullptr, 0);
    if (n <= 0 || n > (8 << 20)) return false;
    std::vector<uint8_t> b((size_t)n);
    if (lb_rom_read("BP/Entities.ebp", b.data(), n) != n) return false;
    if (!comp_unwrap(&b) || b.size() < 4 || std::memcmp(b.data(), "BPNZ", 4) != 0)
        return false;
    *out = std::move(b);
    return true;
}

// Called with the player's cartridge before it reaches the emulator (main.cpp,
// apply_hooks.py docs/391). Adds the mods folder's maps; leaves the image
// untouched when there are none. Returns the number of maps added.
extern "C" int lb_mods_build(std::vector<uint8_t>* rom_p) {
    std::vector<uint8_t>& rom = *rom_p;
    g_maps.clear();
    if (rom.size() < 0x200) return 0;
    // LB_NOMODS=1: the cartridge as it is, whatever the folder holds (tests,
    // and the exactness gate, which is defined without mods).
    if (std::getenv("LB_NOMODS")) {
        g_status = "disabled (LB_NOMODS)";
        return 0;
    }
    const uint32_t fo = rd32(&rom[0x40]), fs_n = rd32(&rom[0x44]);
    const uint32_t ao = rd32(&rom[0x48]), as_n = rd32(&rom[0x4C]);
    if ((uint64_t)fo + fs_n > rom.size() || (uint64_t)ao + as_n > rom.size()) return 0;
    Nitro fs;
    if (!fs.parse(&rom[fo], fs_n, &rom[ao], as_n)) return 0;
    scan(rom, fs);
    if (g_maps.empty()) {
        std::fprintf(stderr, "[lb_mods] %s: no mod maps\n", g_status.c_str());
        std::fflush(stderr);
        return 0;
    }
    const int root = 0, maps = fs.child(0, "Maps"), bp = fs.child(0, "BP");
    const int loc = fs.child(0, "LOC");
    if (maps < 0 || bp < 0 || loc < 0) {
        g_maps.clear();
        return 0;
    }
    // The end of everything the image already uses.
    uint32_t end = std::max(fo + fs_n, ao + as_n);
    for (const auto& se : fs.fat) end = std::max(end, se.second);
    end = (end + 0x1FFu) & ~0x1FFu;
    std::vector<uint8_t> blob;   // the new data, placed at `end`
    auto place = [&](const std::vector<uint8_t>& d) {
        while (blob.size() & 3u) blob.push_back(0xFFu);
        const uint32_t at = end + (uint32_t)blob.size();
        blob.insert(blob.end(), d.begin(), d.end());
        return std::make_pair(at, at + (uint32_t)d.size());
    };
    // The three new directories, files in the order ids are counted.
    const size_t n = g_maps.size();
    auto add_dir = [&](int parent, const char* name) {
        Dir d;
        d.first = (uint16_t)fs.fat.size();
        d.parent = parent;
        fs.dirs.push_back(d);
        const int idx = (int)fs.dirs.size() - 1;
        fs.dirs[(size_t)parent].entries.push_back(Entry{true, name, idx});
        return idx;
    };
    // docs/439: one group of three directories per letter ('z' for maps 1..99,
    // then 'y'); a stock directory's files are the ones named for the stem.
    const int groups = ((int)n + kStemGroup - 1) / kStemGroup;
    for (int g = 0; g < groups; ++g) {
        const size_t lo = (size_t)g * kStemGroup, hi = std::min(n, lo + kStemGroup);
        const char letter[2] = {(char)('z' - g), 0};
        const std::string dname = std::string("DetailTiles_") + letter;
        const int zroot = add_dir(root, letter);
        for (size_t k = lo; k < hi; ++k)
            for (int j = 0; j < 4; ++j) {
                char nm[24];
                std::snprintf(nm, sizeof nm, "%02u%s", (unsigned)(k - lo + 1), kMiniExt[j]);
                fs.dirs[(size_t)zroot].entries.push_back(Entry{false, nm, -1});
                fs.fat.push_back(place(g_maps[k].mini[j]));
            }
        const int zmaps = add_dir(maps, letter);
        for (size_t k = lo; k < hi; ++k) {
            char nm[16];
            std::snprintf(nm, sizeof nm, "%02u.map", (unsigned)(k - lo + 1));
            fs.dirs[(size_t)zmaps].entries.push_back(Entry{false, nm, -1});
            fs.fat.push_back(place(g_maps[k].map));
        }
        const int zdetail = add_dir(bp, dname.c_str());
        for (size_t k = lo; k < hi; ++k) {
            char nm[16];
            std::snprintf(nm, sizeof nm, "%02u.tbp", (unsigned)(k - lo + 1));
            fs.dirs[(size_t)zdetail].entries.push_back(Entry{false, nm, -1});
            fs.fat.push_back(place(g_maps[k].detail));
        }
    }
    // Every language file gets the maps' names and descriptions appended; the
    // same ids in each (they all hold 3,064 in the retail ROM).
    std::vector<std::string> strings;
    for (const ModMap& m : g_maps) { strings.push_back(m.name); strings.push_back(m.desc); }
    uint32_t first_new = 0;
    bool first_set = false;
    {
        int id = fs.dirs[(size_t)loc].first;
        for (const Entry& e : fs.dirs[(size_t)loc].entries) {
            if (e.dir) continue;
            const int this_id = id++;
            if (e.name.size() < 4 ||
                !ci_eq(e.name.substr(e.name.size() - 4), ".lng"))
                continue;
            const auto se = fs.fat[(size_t)this_id];
            std::vector<uint8_t> f(rom.begin() + se.first, rom.begin() + se.second);
            if (f.size() < 0x10 || std::memcmp(f.data(), "LANG", 4) != 0) continue;
            uint32_t fn = 0;
            const std::vector<uint8_t> x = lng_extend(f, strings, &fn);
            if (first_set && fn != first_new) {
                std::fprintf(stderr, "[lb_mods] %s holds %u strings, not %u; "
                             "its map names are left out\n", e.name.c_str(), fn,
                             first_new);
                continue;
            }
            first_new = fn;
            first_set = true;
            fs.fat[(size_t)this_id] = place(x);
        }
    }
    if (!first_set) {
        g_maps.clear();
        return 0;
    }
    for (size_t k = 0; k < n; ++k) {
        g_maps[k].name_id = first_new + 2u * (uint32_t)k;
        g_maps[k].desc_id = first_new + 2u * (uint32_t)k + 1u;
    }
    // The tables, after the data.
    const std::vector<uint8_t> fnt = fs.serialize_fnt();
    std::vector<uint8_t> fat(fs.fat.size() * 8u);
    for (size_t i = 0; i < fs.fat.size(); ++i) {
        wr32(&fat[i * 8u], fs.fat[i].first);
        wr32(&fat[i * 8u + 4u], fs.fat[i].second);
    }
    const auto fnt_at = place(fnt);
    const auto fat_at = place(fat);
    const size_t need = (size_t)end + blob.size();
    if (need > rom.size()) rom.resize((need + 0x1FFFFu) & ~(size_t)0x1FFFFu, 0xFFu);
    std::memcpy(&rom[end], blob.data(), blob.size());
    wr32(&rom[0x40], fnt_at.first);
    wr32(&rom[0x44], (uint32_t)fnt.size());
    wr32(&rom[0x48], fat_at.first);
    wr32(&rom[0x4C], (uint32_t)fat.size());
    if (rd32(&rom[0x80]) < need) wr32(&rom[0x80], (uint32_t)need);
    wr16(&rom[0x15E], header_crc(rom.data()));
    std::fprintf(stderr, "[lb_mods] %s: %zu bytes added after 0x%08X; files "
                 "%zu -> %zu, directories +%d\n", g_status.c_str(), blob.size(),
                 end, (size_t)(as_n / 8u), fs.fat.size(), 3 * groups);
    for (size_t k = 0; k < n; ++k) {
        char slot[24] = "";   // docs/413: a loose folder has no slot
        if (g_maps[k].slot < 1000)
            std::snprintf(slot, sizeof slot, "slot mp%02d of ", g_maps[k].slot);
        char stem[8];
        stem_of((int)k, stem);
        std::fprintf(stderr, "[lb_mods]   %s  %-24s %d players, %s, from "
                     "%s%s\n", stem,
                     g_maps[k].name.c_str(), g_maps[k].players,
                     g_maps[k].world == 1 ? "Pirate" : g_maps[k].world == 2 ? "Mars"
                                                                           : "King",
                     slot, g_maps[k].source.c_str());
    }
    std::fflush(stderr);
    return (int)n;
}

extern "C" int lb_mods_count(void) { return (int)g_maps.size(); }
// docs/439: the most mod maps the game takes, the stem of map k ("z/01" ..
// "y/99"), and the map a stem names (-1 when it is not a mod map's).
extern "C" int lb_mods_limit(void) { return kMaxMods; }
extern "C" void lb_mods_stem(int k, char* out, int cap) {
    char s[8];
    stem_of(k < 0 ? 0 : k, s);
    std::snprintf(out, (size_t)cap, "%s", s);
}
extern "C" int lb_mods_stem_index(const char* stem) {
    if (!stem || stem[1] != '/') return -1;
    const int g = 'z' - stem[0];
    if (g < 0 || g >= kStemGroups || !std::isdigit((unsigned char)stem[2])) return -1;
    const int n = std::atoi(stem + 2);
    return n >= 1 && n <= kStemGroup ? g * kStemGroup + n - 1 : -1;
}
extern "C" int lb_mods_players(int k) {
    return (k >= 0 && k < (int)g_maps.size()) ? g_maps[(size_t)k].players : 0;
}
extern "C" int lb_mods_world(int k) {
    return (k >= 0 && k < (int)g_maps.size()) ? g_maps[(size_t)k].world : 0;
}
extern "C" unsigned lb_mods_name_id(int k) {
    return (k >= 0 && k < (int)g_maps.size()) ? g_maps[(size_t)k].name_id : 0u;
}
extern "C" unsigned lb_mods_desc_id(int k) {
    return (k >= 0 && k < (int)g_maps.size()) ? g_maps[(size_t)k].desc_id : 0u;
}
extern "C" const char* lb_mods_name(int k) {
    return (k >= 0 && k < (int)g_maps.size()) ? g_maps[(size_t)k].name.c_str() : "";
}
extern "C" const char* lb_mods_status(void) { return g_status.c_str(); }
// docs/420: the mods folder (LB_MODS_DIR, else beside the executable).
extern "C" const char* lb_mods_dir(void) {
    static std::string d;
    if (d.empty()) {
        const char* alt = std::getenv("LB_MODS_DIR");
        d = alt && *alt ? std::string(alt)
                        : (std::filesystem::path(exe_dir()) / "mods").string();
    }
    return d.c_str();
}
// docs/413: a map's content hash (map, detail tiles, minimaps), the identity
// a multiplayer lobby compares; and the map holding a hash, or -1.
extern "C" unsigned long long lb_mods_hash(int k) {
    return (k >= 0 && k < (int)g_maps.size()) ? g_maps[(size_t)k].hash : 0ull;
}
// docs/432: a Reset may rebuild the maps (lb_mods_rebuild_if_asked) while the
// bridge's thread reads them for a joiner (lb_mods_bundle).
static std::recursive_mutex g_maps_mu;
extern "C" int lb_mods_find_hash(unsigned long long hash) {
    std::lock_guard<std::recursive_mutex> lk(g_maps_mu);
    for (size_t k = 0; k < g_maps.size(); ++k)
        if (g_maps[k].hash == hash) return (int)k;
    return -1;
}
// docs/432: the bytes behind a map's hash, in the hash's order, for a host
// sending the map to a joiner (lb_maptx.cpp). Any thread.
bool lb_mods_bundle(unsigned long long hash, std::vector<uint8_t>* map,
                    std::vector<uint8_t>* detail, std::vector<uint8_t> mini[4]) {
    std::lock_guard<std::recursive_mutex> lk(g_maps_mu);
    const int k = lb_mods_find_hash(hash);
    if (k < 0) return false;
    const ModMap& m = g_maps[(size_t)k];
    *map = m.map;
    *detail = m.detail;
    for (int i = 0; i < 4; ++i) mini[i] = m.mini[i];
    return true;
}

// docs/431: the map browser's reading of a .map file (a plain or COMP-wrapped
// buffer). out[0] width, [1] height, [2] world (0 King, 1 Pirate, 2 Mars),
// [3] players, [4] mines (the MINE chunk's points, all four groups), [5..8]
// the ground-type layer's cell counts for types 0, 2, 3 and 5, [9] cells of any
// other type. Returns 0 when the file is not a map the loader would accept.
extern "C" int lb_mods_map_stats(const unsigned char* data, size_t n, int out[10]) {
    std::vector<uint8_t> m(data, data + n);
    int world = 0, players = 0;
    if (!map_facts(m, &world, &players) || !comp_unwrap(&m)) return 0;
    const size_t cells = (size_t)m[7] * m[8];
    out[0] = m[7];
    out[1] = m[8];
    out[2] = world;
    out[3] = players;
    for (int k = 4; k < 10; ++k) out[k] = 0;
    const uint8_t* a = m.data() + 3 + 0x28;          // the ground-type layer
    for (size_t i = 0; i < cells; ++i) {
        const uint8_t v = a[i];
        ++out[v == 0 ? 5 : v == 2 ? 6 : v == 3 ? 7 : v == 5 ? 8 : 9];
    }
    // MINE: 'L' count (x,y)*count, four groups, then 0x21 and "PAM".
    size_t q = m.size();
    while (q >= 4 && std::memcmp(m.data() + q - 4, "MINE", 4) != 0) --q;
    if (q >= 4) {
        size_t p = q;
        while (p + 2 <= m.size() && m[p] == 0x4C) {
            const unsigned c = m[p + 1];
            if (p + 2 + 2u * c > m.size()) break;
            out[4] += (int)c;
            p += 2 + 2u * c;
        }
    }
    return 1;
}

// docs/414: a map's REAL start count. The EVNT players that lb_mods_map_stats
// counts include players that only appear in other events (The Pond lists 4
// but places 3 bases, and a team with no base stalls the load). A start is an
// EVNT record of exactly six conditions (the base and its builders: the
// layouts build_map.py copies from mp05/mp18/mp20/mp24), its player the byte
// at +2 of a condition. Returns the distinct players of such records, or -1
// when the file is not a map.
extern "C" int lb_mods_map_starts(const unsigned char* data, size_t n) {
    std::vector<uint8_t> m(data, data + n);
    if (!comp_unwrap(&m) || m.size() < 0x40 || std::memcmp(m.data(), "MAP", 3) != 0 ||
        std::memcmp(m.data() + 3, "TERR", 4) != 0)
        return -1;
    const size_t cells = (size_t)m[7] * m[8];
    size_t q = 3 + 0x28 + 3 * cells;
    if (q + 2 > m.size()) return -1;
    q += 2 + rd16(m.data() + q);
    q += 2 * cells;
    if (q + 8 > m.size() || std::memcmp(m.data() + q, "RRET", 4) != 0 ||
        std::memcmp(m.data() + q + 4, "EVNT", 4) != 0)
        return -1;
    size_t p = q + 8;
    bool seen[256] = {false};
    while (p < m.size() && m[p] == 0x4C) {
        p += 2;
        if (p >= m.size()) return -1;
        p += 1 + 2u * m[p];
        if (p >= m.size()) return -1;
        const uint8_t area = m[p++];
        if (area) p += 4;
        static const int sizes[3] = {9, 8, 3};
        for (int k = 0; k < 3; ++k) {
            if (p >= m.size()) return -1;
            const unsigned c = m[p++];
            if (p + (size_t)c * sizes[k] > m.size()) return -1;
            if (k == 0 && c == 6)
                for (unsigned i = 0; i < c; ++i) seen[m[p + 9u * i + 2u]] = true;
            p += (size_t)c * sizes[k];
        }
    }
    int cnt = 0;
    for (bool s : seen) cnt += s ? 1 : 0;
    return cnt;
}

// docs/431: the game's COMP wrapper removed (a buffer without it is kept).
bool lb_mods_unwrap(std::vector<uint8_t>* b) { return comp_unwrap(b); }

// docs/431: the content hash (docs/413's identity of a mod map: map, detail
// tiles, the four minimap files) of the map folder `path`, computed as the
// boot scan does. Returns 1 and the hash, or 0 when the folder is not a map the
// loader accepts. A folder without detail tiles of its own takes the stock
// slot's, read from the attached ROM, so call it after the game has started.
// Reads files only; any thread.
static bool folder_hash(const char* path, unsigned long long* hash) {
    if (!path || !*path) return false;
    ModMap m;
    if (!loose_folder_map(std::filesystem::u8path(path),
                          [](const std::string& p, std::vector<uint8_t>* o) {
                              const int n = lb_rom_read(p.c_str(), nullptr, 0);
                              if (n <= 0) return false;
                              o->resize((size_t)n);
                              return lb_rom_read(p.c_str(), o->data(), n) == n;
                          },
                          &m))
        return false;
    *hash = m.hash;
    return true;
}
extern "C" int lb_mods_folder_hash(const char* path, unsigned long long* hash) {
    unsigned long long h = 0;
    if (!folder_hash(path, &h)) return 0;
    if (hash) *hash = h;
    return 1;
}

// docs/431: a map folder has just appeared in mods/maps. Returns 1 when its
// map is already in the game's Free Play / host lists (registered when the game
// started), 0 when it is not, or is not a map: the cartridge image, its file
// system and the mission registry are built at start-up (main.cpp, docs/391).
// docs/432: lb_mods_request_rebuild then puts it in with a Reset.
// Reads only; any thread.
extern "C" int lb_mods_register_folder(const char* path) {
    unsigned long long h = 0;
    return folder_hash(path, &h) && lb_mods_find_hash(h) >= 0 ? 1 : 0;
}

// ---- docs/432: a new map without a restart -----------------------------------------
// The emulator boots from main.cpp's `rom`, and its boot() -- which the menu's
// Reset runs -- loads the cartridge from it again. So the image as the ROM gave
// it is kept here, and a Reset that was asked to can rebuild `rom` with the
// mods folder as it is now: a map that arrived since is then in the game.
std::vector<uint8_t>* g_live_rom = nullptr;
std::vector<uint8_t> g_pristine_rom;
std::atomic<bool> g_rebuild_asked{false};

extern "C" void lb_rom_attach(const unsigned char* data, size_t n);   /* lb_teams.cpp */
extern "C" void lb_request_game_reset(void);                         /* lb_overlay.cpp */

void lb_mods_keep_rom(std::vector<uint8_t>* rom) {
    g_live_rom = rom;
    g_pristine_rom = *rom;
}

// Rebuild the cartridge with the mods folder and Reset the game, at the next
// frame. Returns 0 when it cannot (no image kept).
extern "C" int lb_mods_request_rebuild(void) {
    if (!g_live_rom || g_pristine_rom.empty()) return 0;
    g_rebuild_asked = true;
    lb_request_game_reset();
    return 1;
}

// The frontend's Reset point (apply_hooks docs/396/432), just before boot().
extern "C" void lb_mods_rebuild_if_asked(void) {
    if (!g_rebuild_asked.exchange(false) || !g_live_rom) return;
    std::lock_guard<std::recursive_mutex> lk(g_maps_mu);
    const size_t before = g_maps.size();
    *g_live_rom = g_pristine_rom;
    lb_mods_build(g_live_rom);
    lb_rom_attach(g_live_rom->data(), g_live_rom->size());
    std::fprintf(stderr, "[lb_mods] the cartridge rebuilt for the Reset: %zu mod map(s), "
                 "was %zu\n", g_maps.size(), before);
    std::fflush(stderr);
}
