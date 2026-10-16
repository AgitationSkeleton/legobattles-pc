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
//    directories, whose files take new file ids after the last one. Every
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
std::string g_status = "no mods folder";

std::string exe_dir() {
    char* base = SDL_GetBasePath();
    std::string d = base ? base : "";
    if (base) SDL_free(base);
    return d;
}

void scan(const std::vector<uint8_t>& rom, const Nitro& own) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(exe_dir()) / "mods";
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
    // A stable order whatever the folder holds: the slot, then the content.
    std::sort(g_maps.begin(), g_maps.end(), [](const ModMap& a, const ModMap& b) {
        return a.slot != b.slot ? a.slot < b.slot : a.hash < b.hash;
    });
    if (g_maps.size() > 30) g_maps.resize(30);   // one pass of the game's loop
    for (ModMap& m : g_maps)
        m.desc = "A map from " + m.source + ".";
    char buf[96];
    std::snprintf(buf, sizeof buf, "%zu mod map(s) from %d ROM(s)", g_maps.size(),
                  files);
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
    const int zroot = add_dir(root, "z");
    for (size_t k = 0; k < n; ++k)
        for (int j = 0; j < 4; ++j) {
            char nm[24];
            std::snprintf(nm, sizeof nm, "%02u%s", (unsigned)(k + 1), kMiniExt[j]);
            fs.dirs[(size_t)zroot].entries.push_back(Entry{false, nm, -1});
            fs.fat.push_back(place(g_maps[k].mini[j]));
        }
    const int zmaps = add_dir(maps, "z");
    for (size_t k = 0; k < n; ++k) {
        char nm[16];
        std::snprintf(nm, sizeof nm, "%02u.map", (unsigned)(k + 1));
        fs.dirs[(size_t)zmaps].entries.push_back(Entry{false, nm, -1});
        fs.fat.push_back(place(g_maps[k].map));
    }
    const int zdetail = add_dir(bp, "DetailTiles_z");
    for (size_t k = 0; k < n; ++k) {
        char nm[16];
        std::snprintf(nm, sizeof nm, "%02u.tbp", (unsigned)(k + 1));
        fs.dirs[(size_t)zdetail].entries.push_back(Entry{false, nm, -1});
        fs.fat.push_back(place(g_maps[k].detail));
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
                 "%zu -> %zu, directories +3\n", g_status.c_str(), blob.size(),
                 end, (size_t)(as_n / 8u), fs.fat.size());
    for (size_t k = 0; k < n; ++k)
        std::fprintf(stderr, "[lb_mods]   z/%02u  %-24s %d players, %s, from "
                     "slot mp%02d of %s\n", (unsigned)(k + 1),
                     g_maps[k].name.c_str(), g_maps[k].players,
                     g_maps[k].world == 1 ? "Pirate" : g_maps[k].world == 2 ? "Mars"
                                                                           : "King",
                     g_maps[k].slot, g_maps[k].source.c_str());
    std::fflush(stderr);
    return (int)n;
}

extern "C" int lb_mods_count(void) { return (int)g_maps.size(); }
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
