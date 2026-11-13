// lb_pack.cpp - content packs (SG-0114, docs/490).
//
// Part of legobattles-decomp. Not an upstream ndsrecomp file; see
// port/runner/README.md.
//
// A mod is more than one kind of file: its custom units, the balance made for
// them, the rule toggles it is meant to be played with, and perhaps its maps.
// A content pack carries all of that in ONE file, chosen part by part when it
// is exported and installed part by part when it is imported.
//
// THE FORMAT (.lbpack), all little-endian, written and read only here:
//
//     "LBPK"  u16 version (1)  u16 entry count  u32 reserved (0)
//     then per entry:
//       u16 path length, the path (UTF-8, '/' separated, relative)
//       u32 data length, u32 CRC-32 (zlib's) of the data, the data
//
// and nothing after the last entry. The entries a pack may hold:
//
//     pack.json             its name and description (lb_overlay.cpp)
//     balance.json          the Balance values, exactly the file Balance >
//                           Presets' "Export my balance" writes
//     settings.json         a settings preset: chosen gameplay keys of
//                           settings.json, with "lb_settings_preset": 1
//     units/<folder>.lbub   a custom unit folder as its LBUB bundle (the form
//                           a lobby sends, lb_cunits.cpp), so the import goes
//                           through the same checks a received unit does
//     maps/<leaf>/<file>    a loose map folder's files (docs/413)
//
// Anything else is listed as ignored, never written.
//
// SAFETY. A pack comes from someone else. Every path is checked when the pack
// is read (lb_pack_path_ok: relative, no "." or "..", no drive or backslash,
// no character or device name Windows refuses), every entry's CRC must match,
// sizes are capped (lb_pack.h), and nothing is written under a path the pack
// names: a unit folder is lb_cunits_install_bundle's (its own name rules and
// bundle_path_ok), a map goes to mods/maps/Packs/<cleaned leaf>/<checked file
// name> with an extension the map loader reads. An existing folder is never
// overwritten: the same content is "already there", other content gets a new
// name.

#include "lb_pack.h"

#include "lb_cunits.h"   // lb_cunits_folder_bundle
#include "lb_xdelta.h"   // lb_cunits_install_bundle

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

#include <zlib.h>

extern "C" const char* lb_mods_dir(void);   // lb_mods.cpp

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxMapFile = 8u << 20;   // as the map loader reads them
constexpr size_t kMaxMapFiles = 64;
constexpr size_t kMaxMaps = 512;

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
std::string utf8_generic(const fs::path& p) {
    const auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool read_file(const fs::path& p, std::vector<uint8_t>* out, size_t cap) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    if (ec || n > cap) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->resize((size_t)n);
    if (n && !f.read((char*)out->data(), (std::streamsize)n)) return false;
    return true;
}

bool write_file(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!b.empty()) f.write((const char*)b.data(), (std::streamsize)b.size());
    f.close();
    return !f.fail();
}

void put16(std::vector<uint8_t>* o, uint32_t v) {
    o->push_back((uint8_t)v);
    o->push_back((uint8_t)(v >> 8));
}
void put32(std::vector<uint8_t>* o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o->push_back((uint8_t)(v >> (8 * i)));
}
uint32_t get16(const std::vector<uint8_t>& b, size_t at) { return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8); }
uint32_t get32(const std::vector<uint8_t>& b, size_t at) {
    return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8) | ((uint32_t)b[at + 2] << 16) | ((uint32_t)b[at + 3] << 24);
}

uint32_t crc_of(const std::vector<uint8_t>& d) {
    uLong c = crc32(0L, Z_NULL, 0);
    if (!d.empty()) c = crc32(c, d.data(), (uInt)d.size());
    return (uint32_t)c;
}

// CON, PRN, AUX, NUL, COM0-9, LPT0-9, with or without an extension.
bool device_name(const std::string& comp) {
    std::string b = lower(comp.substr(0, comp.find('.')));
    while (!b.empty() && b.back() == ' ') b.pop_back();
    if (b == "con" || b == "prn" || b == "aux" || b == "nul") return true;
    return b.size() == 4 && (b.compare(0, 3, "com") == 0 || b.compare(0, 3, "lpt") == 0) && b[3] >= '0' &&
           b[3] <= '9';
}

// One part of a path.
bool component_ok(const std::string& c) {
    if (c.empty() || c == "." || c == ".." || c.size() > 96) return false;
    if (c.back() == '.' || c.back() == ' ' || c.front() == ' ') return false;
    for (unsigned char ch : c) {
        if (ch < 0x20 || ch == 0x7F) return false;
        if (std::strchr("<>:\"\\|?*/", (int)ch)) return false;
    }
    return !device_name(c);
}

std::vector<std::string> split_path(const std::string& p) {
    std::vector<std::string> v;
    size_t start = 0;
    while (true) {
        const size_t slash = p.find('/', start);
        v.push_back(p.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return v;
}

bool ends_with(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// The files a map folder may carry: what the map loader and the level editor
// read (the map, its minimaps and detail tiles, level.txt) and a picture.
bool map_ext_ok(const std::string& name) {
    const std::string n = lower(name);
    for (const char* e : {".map", ".ncgr", ".ncbr", ".nclr", ".tbp", ".txt", ".png"})
        if (ends_with(n, e)) return true;
    return false;
}

// A map folder name that is safe on any file system (as lb_mods.cpp's
// safe_map_folder).
std::string safe_leaf(const std::string& n) {
    std::string o;
    for (char ch : n) {
        const unsigned char c = (unsigned char)ch;
        if (c < 0x80 && (std::isalnum(c) || ch == ' ' || ch == '-' || ch == '_' || ch == '\'' || ch == '(' ||
                         ch == ')' || ch == '!' || ch == '&' || ch == ',' || ch == '.'))
            o += ch;
        else if (c >= 0x80 || ch == '/' || ch == ':')
            o += '_';
    }
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    while (!o.empty() && (o.front() == ' ' || o.front() == '.')) o.erase(o.begin());
    if (o.size() > 48) o.resize(48);
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    if (o.empty()) o = "Pack map";
    if (device_name(o)) o = "Map " + o;
    return o;
}

fs::path units_dir() { return fs::u8path(lb_mods_dir()) / "units"; }
fs::path maps_dir() { return fs::u8path(lb_mods_dir()) / "maps"; }

// unit.json's "name" (the menus' name for the unit).
std::string json_name(const std::string& s) {
    size_t p = s.find("\"name\"");
    if (p == std::string::npos) return "";
    p = s.find(':', p + 6);
    if (p == std::string::npos) return "";
    p = s.find('"', p);
    if (p == std::string::npos) return "";
    std::string o;
    for (++p; p < s.size() && s[p] != '"' && o.size() < 64; ++p) {
        if (s[p] == '\\' && p + 1 < s.size()) ++p;
        if ((unsigned char)s[p] >= 0x20) o += s[p];
    }
    return o;
}

// unit.json out of an LBUB bundle ("" when absent or malformed).
std::string bundle_unit_json(const std::vector<uint8_t>& b) {
    if (b.size() < 7 || std::memcmp(b.data(), "LBUB\x01", 5) != 0) return "";
    const size_t count = get16(b, 5);
    size_t at = 7;
    for (size_t i = 0; i < count; ++i) {
        if (at + 2 > b.size()) return "";
        const size_t pl = get16(b, at);
        at += 2;
        if (at + pl + 4 > b.size()) return "";
        const std::string path((const char*)b.data() + at, pl);
        at += pl;
        const size_t dl = get32(b, at);
        at += 4;
        if (dl > b.size() - at) return "";
        if (path == "unit.json") return std::string((const char*)b.data() + at, dl);
        at += dl;
    }
    return "";
}

std::vector<std::string> enabled_list() {
    std::vector<std::string> v;
    std::vector<uint8_t> b;
    if (!read_file(units_dir() / "enabled.txt", &b, 64u << 10)) return v;
    std::string line;
    for (size_t i = 0; i <= b.size(); ++i) {
        if (i == b.size() || b[i] == '\n' || b[i] == '\r') {
            while (!line.empty() && line.back() == ' ') line.pop_back();
            if (!line.empty()) v.push_back(line);
            line.clear();
        } else {
            line += (char)b[i];
        }
    }
    return v;
}

// A folder holding exactly one non-empty .map (lb_mods.cpp's test; the
// minimaps it also needs are the loader's to check).
bool is_map_folder(const fs::path& dir) {
    std::error_code ec;
    int maps = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (lower(utf8(e.path().extension())) == ".map" && fs::file_size(e.path(), ec) > 0 && !ec) ++maps;
    }
    return maps == 1;
}

int map_count(const LbPackMap& m) {
    int n = 0;
    for (const auto& f : m.files)
        if (ends_with(lower(f.first), ".map") && !f.second.empty()) ++n;
    return n;
}

// The folder already holds exactly these files.
bool same_contents(const fs::path& dir, const LbPackMap& m) {
    std::error_code ec;
    size_t files = 0;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) ++files;
    if (files != m.files.size()) return false;
    for (const auto& f : m.files) {
        std::vector<uint8_t> have;
        if (!read_file(dir / fs::u8path(f.first), &have, kMaxMapFile) || have != f.second) return false;
    }
    return true;
}

}  // namespace

bool lb_pack_path_ok(const std::string& p) {
    if (p.empty() || p.size() > kLbPackMaxPath || p.front() == '/' || p.back() == '/') return false;
    for (const std::string& c : split_path(p))
        if (!component_ok(c)) return false;
    return true;
}

bool lb_pack_write(const std::string& file, const std::vector<LbPackEntry>& entries, std::string* why) {
    if (entries.empty()) {
        *why = "nothing was chosen to go in the pack";
        return false;
    }
    if (entries.size() > kLbPackMaxEntries) {
        *why = "too many files (at most " + std::to_string(kLbPackMaxEntries) + ")";
        return false;
    }
    std::set<std::string> seen;
    std::vector<uint8_t> o = {'L', 'B', 'P', 'K'};
    put16(&o, 1);
    put16(&o, (uint32_t)entries.size());
    put32(&o, 0);
    for (const LbPackEntry& e : entries) {
        if (!lb_pack_path_ok(e.path)) {
            *why = "a file name the pack cannot hold: " + e.path;
            return false;
        }
        if (!seen.insert(lower(e.path)).second) {
            *why = "two files named " + e.path;
            return false;
        }
        if (e.data.size() > kLbPackMaxEntry) {
            *why = e.path + " is over 16 MB";
            return false;
        }
        put16(&o, (uint32_t)e.path.size());
        o.insert(o.end(), e.path.begin(), e.path.end());
        put32(&o, (uint32_t)e.data.size());
        put32(&o, crc_of(e.data));
        o.insert(o.end(), e.data.begin(), e.data.end());
        if (o.size() > kLbPackMaxTotal) {
            *why = "the pack would be over 128 MB";
            return false;
        }
    }
    // A temporary, then a rename: a failed write leaves no half pack.
    const fs::path dest = fs::u8path(file);
    fs::path tmp = dest;
    tmp += ".tmp";
    if (!write_file(tmp, o)) {
        *why = "could not write " + file;
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(dest, ec);
        fs::rename(tmp, dest, ec);
    }
    if (ec) {
        fs::remove(tmp, ec);
        *why = "could not write " + file;
        return false;
    }
    return true;
}

bool lb_pack_read(const std::string& file, std::vector<LbPackEntry>* out, std::string* why) {
    out->clear();
    std::vector<uint8_t> b;
    if (!read_file(fs::u8path(file), &b, kLbPackMaxTotal)) {
        *why = "could not read it (or it is over 128 MB)";
        return false;
    }
    if (b.size() < 12 || std::memcmp(b.data(), "LBPK", 4) != 0) {
        *why = "not a content pack (.lbpack)";
        return false;
    }
    const uint32_t version = get16(b, 4);
    if (version != 1) {
        *why = "made by a newer version of the port (pack format " + std::to_string(version) + ")";
        return false;
    }
    const size_t count = get16(b, 6);
    if (count > kLbPackMaxEntries) {
        *why = "too many files in it";
        return false;
    }
    std::set<std::string> seen;
    size_t at = 12;
    for (size_t i = 0; i < count; ++i) {
        if (at + 2 > b.size()) {
            *why = "damaged (cut short)";
            return false;
        }
        const size_t pl = get16(b, at);
        at += 2;
        if (pl == 0 || pl > kLbPackMaxPath || at + pl + 8 > b.size()) {
            *why = "damaged (a file name)";
            return false;
        }
        LbPackEntry e;
        e.path.assign((const char*)b.data() + at, pl);
        at += pl;
        const size_t dl = get32(b, at);
        const uint32_t crc = get32(b, at + 4);
        at += 8;
        if (!lb_pack_path_ok(e.path)) {
            *why = "it names an unsafe path: " + e.path;
            return false;
        }
        if (!seen.insert(lower(e.path)).second) {
            *why = "it holds two files named " + e.path;
            return false;
        }
        if (dl > kLbPackMaxEntry || dl > b.size() - at) {
            *why = "damaged (" + e.path + " is cut short or too big)";
            return false;
        }
        e.data.assign(b.begin() + (long)at, b.begin() + (long)(at + dl));
        at += dl;
        if (crc_of(e.data) != crc) {
            *why = "damaged (" + e.path + " fails its checksum)";
            return false;
        }
        out->push_back(std::move(e));
    }
    if (at != b.size()) {
        *why = "damaged (bytes after the last file)";
        out->clear();
        return false;
    }
    return true;
}

std::vector<LbPackUnitFolder> lb_pack_list_units() {
    std::vector<LbPackUnitFolder> v;
    const std::vector<std::string> on = enabled_list();
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(units_dir(), ec)) {
        if (!de.is_directory(ec)) continue;
        const std::string n = utf8(de.path().filename());
        if (n == "export" || n == "_received") continue;
        std::vector<uint8_t> j;
        if (!read_file(de.path() / "unit.json", &j, 256u << 10)) continue;
        LbPackUnitFolder u;
        u.folder = n;
        u.name = json_name(std::string(j.begin(), j.end()));
        u.enabled = std::find(on.begin(), on.end(), n) != on.end();
        v.push_back(u);
    }
    std::sort(v.begin(), v.end(), [](const LbPackUnitFolder& a, const LbPackUnitFolder& b) {
        return lower(a.folder) < lower(b.folder);
    });
    return v;
}

std::vector<LbPackMapFolder> lb_pack_list_maps() {
    std::vector<LbPackMapFolder> v;
    const fs::path root = maps_dir();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return v;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_directory(ec)) continue;
        const std::string leaf = utf8(it->path().filename());
        // Maps a lobby's host sent: theirs to share, not this machine's.
        if (it.depth() == 0 && lower(leaf) == "_received") {
            it.disable_recursion_pending();
            continue;
        }
        if (it.depth() > 6) {
            it.disable_recursion_pending();
            continue;
        }
        if (!is_map_folder(it->path())) continue;
        LbPackMapFolder m;
        m.rel = utf8_generic(it->path().lexically_relative(root));
        m.leaf = safe_leaf(leaf);
        v.push_back(m);
    }
    std::sort(v.begin(), v.end(),
              [](const LbPackMapFolder& a, const LbPackMapFolder& b) { return lower(a.rel) < lower(b.rel); });
    return v;
}

bool lb_pack_add_unit(const std::string& folder, std::vector<LbPackEntry>* out, std::string* why) {
    LbPackEntry e;
    e.path = "units/" + folder + ".lbub";
    if (folder.empty() || folder.find('/') != std::string::npos || !lb_pack_path_ok(e.path)) {
        *why = "the folder name " + folder + " cannot go in a pack";
        return false;
    }
    if (!lb_cunits_folder_bundle(folder, &e.data)) {
        *why = "its files are over 1 MB (the limit a lobby has too)";
        return false;
    }
    out->push_back(std::move(e));
    return true;
}

bool lb_pack_add_map(const LbPackMapFolder& m, const std::string& leaf, std::vector<LbPackEntry>* out,
                     std::string* why) {
    if (!component_ok(leaf)) {
        *why = "the folder name " + leaf + " cannot go in a pack";
        return false;
    }
    const fs::path dir = maps_dir() / fs::u8path(m.rel);
    std::error_code ec;
    std::vector<LbPackEntry> files;
    int maps = 0;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (!de.is_regular_file(ec)) continue;
        const std::string n = utf8(de.path().filename());
        if (!map_ext_ok(n) || !component_ok(n)) continue;   // the loader reads none of the rest
        LbPackEntry e;
        e.path = "maps/" + leaf + "/" + n;
        if (!lb_pack_path_ok(e.path)) continue;
        if (!read_file(de.path(), &e.data, kMaxMapFile)) {
            *why = n + " could not be read, or is over 8 MB";
            return false;
        }
        if (ends_with(lower(n), ".map") && !e.data.empty()) ++maps;
        files.push_back(std::move(e));
        if (files.size() > kMaxMapFiles) {
            *why = "more than " + std::to_string(kMaxMapFiles) + " files";
            return false;
        }
    }
    if (maps != 1) {
        *why = "not a map folder (one .map file)";
        return false;
    }
    for (LbPackEntry& e : files) out->push_back(std::move(e));
    return true;
}

bool lb_pack_split(const std::vector<LbPackEntry>& entries, LbPackContents* out, std::string* why) {
    *out = LbPackContents();
    for (const LbPackEntry& e : entries) {
        const std::string text(e.data.begin(), e.data.end());
        if (e.path == "pack.json" && e.data.size() <= (64u << 10)) {
            out->has_manifest = true;
            out->manifest = text;
            continue;
        }
        if (e.path == "balance.json" && e.data.size() <= (1u << 20)) {
            out->has_balance = true;
            out->balance = text;
            continue;
        }
        if (e.path == "settings.json" && e.data.size() <= (1u << 20)) {
            out->has_settings = true;
            out->settings = text;
            continue;
        }
        const std::vector<std::string> c = split_path(e.path);
        if (c.size() == 2 && c[0] == "units" && ends_with(c[1], ".lbub") && c[1].size() > 5 &&
            e.data.size() >= 7 && std::memcmp(e.data.data(), "LBUB\x01", 5) == 0) {
            out->units.push_back({c[1].substr(0, c[1].size() - 5), e.data});
            out->unit_names.push_back(json_name(bundle_unit_json(e.data)));
            continue;
        }
        if (c.size() == 3 && c[0] == "maps" && map_ext_ok(c[2]) && e.data.size() <= kMaxMapFile) {
            auto it = std::find_if(out->maps.begin(), out->maps.end(),
                                   [&](const LbPackMap& m) { return m.leaf == c[1]; });
            if (it == out->maps.end()) {
                if (out->maps.size() >= kMaxMaps) {
                    out->ignored.push_back(e.path);
                    continue;
                }
                out->maps.push_back(LbPackMap());
                out->maps.back().leaf = c[1];
                it = out->maps.end() - 1;
            }
            if (it->files.size() >= kMaxMapFiles) {
                out->ignored.push_back(e.path);
                continue;
            }
            it->files.push_back({c[2], e.data});
            continue;
        }
        out->ignored.push_back(e.path);
    }
    // A map folder without exactly one map is not installed.
    for (size_t i = 0; i < out->maps.size();) {
        if (map_count(out->maps[i]) != 1) {
            out->ignored.push_back("maps/" + out->maps[i].leaf + "/ (not one .map)");
            out->maps.erase(out->maps.begin() + (long)i);
        } else {
            ++i;
        }
    }
    if (out->units.empty() && out->maps.empty() && !out->has_balance && !out->has_settings) {
        *why = "it holds no units, maps, balance or settings this port reads";
        return false;
    }
    return true;
}

int lb_pack_install_unit(const std::string& want, const std::vector<uint8_t>& bundle, std::string* folder,
                         std::string* why) {
    // The lobby's own path: the folder name cleaned, a clash renamed, the
    // bundle's every file checked, the folder added to enabled.txt. The same
    // unit already in mods/units is enabled instead of copied again.
    return lb_cunits_install_bundle(bundle, want, false, folder, why);
}

int lb_pack_install_map(const LbPackMap& m, std::string* folder, std::string* why) {
    folder->clear();
    if (map_count(m) != 1) {
        *why = "not a map folder (one .map file)";
        return 0;
    }
    for (const auto& f : m.files)
        if (!component_ok(f.first) || !map_ext_ok(f.first)) {
            *why = "a file name that is not allowed: " + f.first;
            return 0;
        }
    const std::string leaf = safe_leaf(m.leaf);
    const fs::path base = maps_dir() / "Packs";
    std::error_code ec;
    for (int n = 1; n <= 50; ++n) {
        const std::string dn = n == 1 ? leaf : leaf + " (" + std::to_string(n) + ")";
        const fs::path dir = base / fs::u8path(dn);
        *folder = "mods/maps/Packs/" + dn;
        if (fs::exists(dir, ec)) {
            if (same_contents(dir, m)) return 2;
            continue;   // someone else's folder by that name: never overwritten
        }
        fs::create_directories(dir, ec);
        if (ec) {
            *why = "could not make " + *folder;
            return 0;
        }
        for (const auto& f : m.files)
            if (!write_file(dir / fs::u8path(f.first), f.second)) {
                fs::remove_all(dir, ec);
                *why = "could not write to " + *folder;
                return 0;
            }
        return 1;
    }
    *why = "too many folders named " + leaf;
    return 0;
}
