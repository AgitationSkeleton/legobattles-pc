// legobattles-decomp: fetching a host's mod map from the community archive
// (docs/420).
//
// A joiner without the host's mod map is told so and cannot ready (docs/413).
// With "Fetch missing maps" on (the default), it also looks the map up in the
// community map archive (github.com/Ryncat2000/Lego-Battles-Custom-Map-Archive)
// by the name the host's rules give it, which for a loose map is its folder's
// name:
//   * the archive's four zips are downloaded once, over HTTPS, into
//     mods/cache/ (and reused after that);
//   * every folder in them named like the map (letter case aside) is
//     extracted into mods/maps/_archive/<its path in the zip>/ -- the files of that
//     folder only, each under 8 MB, names checked to stay inside it;
//   * the player is told to restart: mod maps are registered at boot
//     (docs/391), and the lobby then matches the copy by its content hash.
// Nothing here runs unless a lobby asks for a map this machine lacks.

#include "lb_overlay.h"

#include <zlib.h>

#include "imgui.h"

#include <algorithm>
#include <map>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

bool lb_https_download(const std::string& host, const std::string& path,
                       std::string* data, size_t max, std::string* error);
extern "C" const char* lb_mods_dir(void);   /* lb_mods.cpp */
extern "C" int lb_setting_fetch_maps(void); /* lb_overlay.cpp */
// docs/431, the map browser's readers (lb_mods.cpp, lb_matchpic.cpp).
extern "C" int lb_mods_map_stats(const unsigned char* data, size_t n, int out[10]);
extern "C" int lb_mods_count(void);
extern "C" int lb_mods_register_folder(const char* path);
extern "C" int lb_mods_folder_hash(const char* path, unsigned long long* hash);
extern "C" int lb_matchpic_mini_rgba(const unsigned char* ncgr, size_t n, int world,
                                     unsigned int* out, int* size);

namespace {
namespace fs = std::filesystem;

// docs/431: where maps come from. Each source is a GitHub repo whose maps sit in
// zips at the repo's top (the browser reads the first `nbrowse` of them; any
// others, such as campaign fixes, are only searched by name for a lobby's
// missing map). To add a source, add one entry: its zips are cached in
// mods/cache/<cache>/ (the first source keeps mods/cache/ itself).
struct Source {
    const char* name;      // shown in the Maps page
    const char* host;      // raw file host
    const char* base;      // path up to the zips, with both slashes
    const char* cache;     // subfolder of mods/cache ("" for none)
    const char* zips[8];
    int nzips;
    int nbrowse;
};
const Source kSources[] = {
    {"Community map archive (Ryncat2000)", "raw.githubusercontent.com",
     "/Ryncat2000/Lego-Battles-Custom-Map-Archive/main/", "",
     {"64 sized.zip", "96 sized.zip", "Better Free Play maps pack.zip",
      "DIFFICULTY PATCH-first wave.zip"},
     4, 3},
    // The port's own (github.com/AgitationSkeleton/legobattles-custom-maps):
    // the cut Mars maps rebuilt from their minimaps, the prototypes' Free
    // Play maps that differ from the final game's; Ninjago's later.
    {"LEGO Battles PC custom maps", "raw.githubusercontent.com",
     "/AgitationSkeleton/legobattles-custom-maps/main/", "custom",
     {"Mars beta maps.zip", "Prototype Free Play maps.zip"},
     2, 2},
};
const int kNumSources = (int)(sizeof kSources / sizeof kSources[0]);

std::mutex g_mu;
std::set<std::string> g_asked;      // names asked for this run (lowercase)
std::atomic<bool> g_busy{false};

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string url_path(const char* name) {
    std::string out;
    for (const char* p = name; *p; ++p) {
        if (*p == ' ') out += "%20";
        else out += *p;
    }
    return out;
}
uint32_t le16(const std::string& d, size_t at) {
    return (uint32_t)(uint8_t)d[at] | ((uint32_t)(uint8_t)d[at + 1] << 8);
}
uint32_t le32(const std::string& d, size_t at) {
    return le16(d, at) | (le16(d, at + 2) << 16);
}

// One file of a zip: stored (0) or deflated (8), at most `max` bytes.
bool zip_entry(const std::string& zip, size_t local, uint32_t method, uint32_t csize,
               uint32_t usize, std::string* out, size_t max) {
    if (local + 30 > zip.size() || le32(zip, local) != 0x04034B50u) return false;
    const size_t data = local + 30 + le16(zip, local + 26) + le16(zip, local + 28);
    if (data + csize > zip.size() || usize > max) return false;
    if (method == 0) {
        out->assign(zip, data, csize);
        return csize == usize;
    }
    if (method != 8) return false;
    out->assign(usize, '\0');
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
    z.next_in = (Bytef*)(zip.data() + data);
    z.avail_in = csize;
    z.next_out = (Bytef*)&(*out)[0];
    z.avail_out = usize;
    const int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return r == Z_STREAM_END && z.total_out == usize;
}

// One file of a zip's central directory (docs/431: shared by the fetch and the
// map browser). `folder` and `file` are the name split at its last '/'.
struct ZipEnt {
    std::string name, folder, file;
    uint32_t method = 0, csize = 0, usize = 0, local = 0;
};

// Every file (not folder) entry of `zip`.
bool zip_list(const std::string& zip, std::vector<ZipEnt>* out) {
    // The end-of-central-directory record, searched from the end.
    if (zip.size() < 22) return false;
    size_t eocd = std::string::npos;
    for (size_t i = zip.size() - 22; i + 1 > 0 && zip.size() - i < 66000; --i) {
        if (le32(zip, i) == 0x06054B50u) { eocd = i; break; }
        if (i == 0) break;
    }
    if (eocd == std::string::npos) return false;
    const uint32_t count = le16(zip, eocd + 10);
    size_t at = le32(zip, eocd + 16);
    for (uint32_t k = 0; k < count && at + 46 <= zip.size(); ++k) {
        if (le32(zip, at) != 0x02014B50u) break;
        ZipEnt e;
        e.method = le16(zip, at + 10);
        e.csize = le32(zip, at + 20);
        e.usize = le32(zip, at + 24);
        const uint32_t nlen = le16(zip, at + 28), xlen = le16(zip, at + 30);
        const uint32_t clen = le16(zip, at + 32);
        e.local = le32(zip, at + 42);
        e.name = zip.substr(at + 46, nlen);
        at += 46 + nlen + xlen + clen;
        for (char& c : e.name) if (c == '\\') c = '/';
        if (e.name.empty() || e.name.back() == '/') continue;   // a folder entry
        const size_t slash = e.name.rfind('/');
        if (slash == std::string::npos) continue;
        e.folder = e.name.substr(0, slash);
        e.file = e.name.substr(slash + 1);
        out->push_back(std::move(e));
    }
    return true;
}

// Inside the folder only: no "..", no drive or absolute paths.
bool safe_entry(const ZipEnt& e) {
    return !e.file.empty() && e.name.find("..") == std::string::npos &&
           e.name.find(':') == std::string::npos && e.name[0] != '/';
}

bool write_entry(const std::string& zip, const ZipEnt& e, const fs::path& dest) {
    std::string bytes;
    if (!zip_entry(zip, e.local, e.method, e.csize, e.usize, &bytes, 8u << 20))
        return false;
    std::error_code ec;
    const fs::path dir = dest / fs::u8path(e.folder);
    fs::create_directories(dir, ec);
    FILE* f = std::fopen((dir / fs::u8path(e.file)).string().c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

// Every folder of `zip` named `want` (lowercase), extracted under `dest`.
int extract_named(const std::string& zip, const std::string& want, const fs::path& dest) {
    std::vector<ZipEnt> ents;
    if (!zip_list(zip, &ents)) return 0;
    int written = 0;
    for (const ZipEnt& e : ents) {
        const size_t up = e.folder.rfind('/');
        const std::string leaf = up == std::string::npos ? e.folder : e.folder.substr(up + 1);
        if (lower(leaf) != want || !safe_entry(e)) continue;
        if (write_entry(zip, e, dest)) ++written;
    }
    return written;
}

// The folder `folder` (its path in the zip, exactly) extracted under `dest`.
int extract_folder(const std::string& zip, const std::string& folder, const fs::path& dest) {
    std::vector<ZipEnt> ents;
    if (!zip_list(zip, &ents)) return 0;
    int written = 0;
    for (const ZipEnt& e : ents)
        if (e.folder == folder && safe_entry(e) && write_entry(zip, e, dest)) ++written;
    return written;
}

// One of the archive's zips: the cached copy in mods/cache, else (when
// `download`) fetched over HTTPS and cached.
bool load_zip(const Source& src, const char* z, bool download, std::string* zip,
              std::string* error) {
    const fs::path mods = fs::u8path(lb_mods_dir());
    const fs::path cache = mods / "cache" / fs::u8path(src.cache);
    std::error_code ec;
    fs::create_directories(cache, ec);
    const fs::path local = cache / fs::u8path(z);
    zip->clear();
    if (FILE* f = std::fopen(local.string().c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 0 && n < (64L << 20)) {
            zip->resize((size_t)n);
            if (std::fread(&(*zip)[0], 1, zip->size(), f) != zip->size()) zip->clear();
        }
        std::fclose(f);
    }
    if (!zip->empty()) return true;
    if (!download) {
        *error = std::string(z) + " is not in mods/cache";
        return false;
    }
    std::string err;
    if (!lb_https_download(src.host, std::string(src.base) + url_path(z), zip, 64u << 20, &err)) {
        *error = err;
        std::fprintf(stderr, "[lb_mapfetch] %s: %s\n", z, err.c_str());
        return false;
    }
    if (FILE* f = std::fopen(local.string().c_str(), "wb")) {
        std::fwrite(zip->data(), 1, zip->size(), f);
        std::fclose(f);
    }
    std::fprintf(stderr, "[lb_mapfetch] downloaded %s (%zu bytes)\n", z, zip->size());
    return true;
}

void fetch(std::string name) {
    const std::string want = lower(name);
    const fs::path mods = fs::u8path(lb_mods_dir());
    int files = 0;
    std::string last_error;
    for (const Source& src : kSources)
        for (int zi = 0; zi < src.nzips; ++zi) {
            std::string zip;
            if (!load_zip(src, src.zips[zi], true, &zip, &last_error)) continue;
            // The zips hold their own top folder ("64 sized/..."), kept as it is.
            files += extract_named(zip, want, mods / "maps" / "_archive");
        }
    char text[256];
    if (files > 0) {
        std::snprintf(text, sizeof text,
                      "\"%s\" was downloaded from the map archive into "
                      "mods/maps/_archive. Restart the game to use it, then join "
                      "again.", name.c_str());
    } else {
        std::snprintf(text, sizeof text,
                      "\"%s\" is not in the map archive%s%s. Ask the host for "
                      "its folder, or pick another map.", name.c_str(),
                      last_error.empty() ? "" : " (or it could not be reached: ",
                      last_error.empty() ? "" : (last_error + ")").c_str());
    }
    std::fprintf(stderr, "[lb_mapfetch] \"%s\": %d file(s) extracted\n", name.c_str(),
                 files);
    std::fflush(stderr);
    lb_overlay_notice(text);
    g_busy = false;
}
// ---- docs/431: the Maps page (Tools > Maps) ----------------------------------
//
// Lists the archive's Free Play maps one by one: a thumbnail (the map's own
// minimap pair, drawn with the map select's palette), what its .map file says
// (size, world, players, mines, ground types) and its notes. The first three
// zips are the Free Play ones; the fourth holds campaign difficulty fixes (more
// than one .map per folder, no minimap) and is left out. A folder is listed
// when it holds exactly one .map and that map's four minimap files, the same
// rule lb_mods.cpp loads by. Nothing is downloaded until the player presses the
// button (or the zips are already cached, which only reads them).

struct ArcMap {
    std::string name, folder, pack, notes;
    int src = 0, zip = 0;       // the source, and its zip
    int st[10] = {0};            // lb_mods_map_stats
    std::vector<uint8_t> ncgr;   // <stem>mini.NCGR as shipped
    uint64_t hash = 0;           // of the .map file
    bool installed = false;
    SDL_Texture* tex = nullptr;
    int tsize = 0;
    bool tex_done = false;
};
std::vector<ArcMap> g_arc;
std::atomic<int> g_arc_state{0};   // 0 not loaded, 1 loading, 2 ready
std::mutex g_arc_mu;
std::string g_arc_msg;             // progress, or what went wrong
std::string g_dl_msg;              // the last download's result
bool g_dl_new = false;             // a downloaded map is not in the game yet (docs/432)
std::set<std::string> g_inst_names;   // lowercase folder names holding a .map
std::set<uint64_t> g_inst_hashes;     // fnv of every installed .map

uint64_t fnv_bytes(const std::string& b) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : b) { h ^= c; h *= 1099511628211ull; }
    return h;
}
bool ends_ci(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && lower(s.substr(s.size() - suf.size())) == suf;
}
bool is_size_txt(const std::string& f) {   // "64x64.txt"
    if (!ends_ci(f, ".txt") || f.size() < 6) return false;
    for (size_t i = 0; i + 4 < f.size(); ++i)
        if (!std::isdigit((unsigned char)f[i]) && f[i] != 'x' && f[i] != 'X') return false;
    return true;
}

// What mods/maps already holds: its folders' names and its .map files' hashes.
void arc_scan_installed() {
    g_inst_names.clear();
    g_inst_hashes.clear();
    std::error_code ec;
    const fs::path loose = fs::u8path(lb_mods_dir()) / "maps";
    if (!fs::is_directory(loose, ec)) return;
    for (auto it = fs::recursive_directory_iterator(loose, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || lower(it->path().extension().string()) != ".map")
            continue;
        g_inst_names.insert(lower(it->path().parent_path().filename().string()));
        std::string b;
        if (FILE* f = std::fopen(it->path().string().c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n > 0 && n < (8L << 20)) {
                b.resize((size_t)n);
                if (std::fread(&b[0], 1, b.size(), f) != b.size()) b.clear();
            }
            std::fclose(f);
        }
        if (!b.empty()) g_inst_hashes.insert(fnv_bytes(b));
    }
}
void arc_mark_installed() {
    for (ArcMap& m : g_arc)
        m.installed = g_inst_hashes.count(m.hash) || g_inst_names.count(lower(m.name));
}

void arc_load(bool download) {
    std::vector<ArcMap> maps;
    std::string warn;
    for (int si = 0; si < kNumSources; ++si)
      for (int z = 0; z < kSources[si].nbrowse; ++z) {
        const Source& src = kSources[si];
        {
            std::lock_guard<std::mutex> lk(g_arc_mu);
            g_arc_msg = std::string(download ? "Loading " : "Reading ") + src.zips[z] + "...";
        }
        std::string zip, err;
        std::vector<ZipEnt> ents;
        if (!load_zip(src, src.zips[z], download, &zip, &err) || !zip_list(zip, &ents)) {
            warn += (err.empty() ? std::string(src.zips[z]) + " could not be read" : err) + ". ";
            continue;
        }
        std::map<std::string, std::vector<size_t>> by;
        for (size_t i = 0; i < ents.size(); ++i) by[ents[i].folder].push_back(i);
        for (const auto& kv : by) {
            const ZipEnt* map_e = nullptr;
            int map_n = 0;
            for (size_t i : kv.second)
                if (ends_ci(ents[i].file, ".map")) { map_e = &ents[i]; ++map_n; }
            if (map_n != 1) continue;
            static const char* const kMini[4] = {"mini.NCGR", "mini.NCBR", "miniNT.NCGR",
                                                 "miniNT.NCBR"};
            const std::string stem = map_e->file.substr(0, map_e->file.size() - 4);
            const ZipEnt* mini[4] = {nullptr, nullptr, nullptr, nullptr};
            for (int k = 0; k < 4; ++k)
                for (size_t i : kv.second)
                    if (lower(ents[i].file) == lower(stem + kMini[k])) mini[k] = &ents[i];
            if (!mini[0] || !mini[1] || !mini[2] || !mini[3]) continue;
            std::string mapb, ncgr;
            if (!zip_entry(zip, map_e->local, map_e->method, map_e->csize, map_e->usize,
                           &mapb, 8u << 20) ||
                !zip_entry(zip, mini[0]->local, mini[0]->method, mini[0]->csize,
                           mini[0]->usize, &ncgr, 1u << 20))
                continue;
            ArcMap m;
            if (!lb_mods_map_stats((const unsigned char*)mapb.data(), mapb.size(), m.st))
                continue;
            m.src = si;
            m.zip = z;
            m.folder = kv.first;
            const size_t up = kv.first.rfind('/');
            m.name = up == std::string::npos ? kv.first : kv.first.substr(up + 1);
            m.pack = (kNumSources > 1 ? std::string(src.name) + " / " : std::string()) +
                     kv.first.substr(0, kv.first.find('/'));
            m.hash = fnv_bytes(mapb);
            m.ncgr.assign(ncgr.begin(), ncgr.end());
            for (size_t i : kv.second) {
                const ZipEnt& e = ents[i];
                if (!ends_ci(e.file, ".txt") || is_size_txt(e.file)) continue;
                std::string t;
                if (!zip_entry(zip, e.local, e.method, e.csize, e.usize, &t, 64u << 10))
                    continue;
                t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
                if (t.size() > 3000) t.resize(3000);
                if (!m.notes.empty()) m.notes += "\n\n";
                m.notes += t;
            }
            maps.push_back(std::move(m));
        }
    }
    std::sort(maps.begin(), maps.end(), [](const ArcMap& a, const ArcMap& b) {
        if (a.src != b.src) return a.src < b.src;
        return a.zip != b.zip ? a.zip < b.zip : lower(a.name) < lower(b.name);
    });
    arc_scan_installed();
    {
        std::lock_guard<std::mutex> lk(g_arc_mu);
        g_arc_msg = warn;
        g_arc = std::move(maps);
    }
    arc_mark_installed();
    std::fprintf(stderr, "[lb_mapbrowser] %zu map(s) listed%s%s\n", g_arc.size(),
                 warn.empty() ? "" : "; ", warn.c_str());
    std::fflush(stderr);
    g_arc_state = g_arc.empty() ? 0 : 2;
}

void arc_start(bool download) {
    if (g_arc_state.exchange(1) == 1) return;
    std::thread([download]() {
        try {
            arc_load(download);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(g_arc_mu);
            g_arc_msg = std::string("Could not read the archive: ") + e.what();
            g_arc_state = 0;
        }
    }).detach();
}

bool arc_all_cached() {
    std::error_code ec;
    for (const Source& src : kSources)
        for (int z = 0; z < src.nbrowse; ++z)
            if (!fs::is_regular_file(fs::u8path(lb_mods_dir()) / "cache" /
                                         fs::u8path(src.cache) / fs::u8path(src.zips[z]), ec))
                return false;
    return true;
}

// The picture is made on first use: the palette comes from the game's memory.
void arc_texture(ArcMap& m, SDL_Renderer* ren) {
    if (m.tex_done || !ren) return;
    m.tex_done = true;
    static std::vector<unsigned int> px(128 * 128);
    int size = 0;
    if (!lb_matchpic_mini_rgba(m.ncgr.data(), m.ncgr.size(), m.st[2], px.data(), &size))
        return;
    m.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                              128, 128);
    if (!m.tex) return;
    SDL_UpdateTexture(m.tex, nullptr, px.data(), 128 * 4);
    SDL_SetTextureScaleMode(m.tex, SDL_ScaleModeNearest);
    m.tsize = size;
}

void arc_download(ArcMap& m) {
    std::string zip, err;
    if (!load_zip(kSources[m.src], kSources[m.src].zips[m.zip], false, &zip, &err)) {
        g_dl_msg = "The archive is not cached: " + err;
        return;
    }
    const int n = extract_folder(zip, m.folder, fs::u8path(lb_mods_dir()) / "maps" / "_archive");
    std::fprintf(stderr, "[lb_mapbrowser] \"%s\": %d file(s) extracted\n", m.name.c_str(), n);
    std::fflush(stderr);
    if (n <= 0) {
        g_dl_msg = "Could not write the map's files into mods/maps.";
        return;
    }
    arc_scan_installed();
    arc_mark_installed();
    g_dl_msg = "Downloaded into mods/maps/_archive. Press Load new maps now (the game "
               "resets to the title screen) to see it in the Free Play map list.";
    g_dl_new = true;
}

void arc_draw(SDL_Renderer* ren) {
    static char search[64] = "";
    static int size_filter = 0;   // 0 all, 1 = 64x64, 2 = 96x96
    static int sel = -1;
    static bool auto_tried = false;
    if (g_arc_state == 0 && !auto_tried) {
        auto_tried = true;
        if (arc_all_cached()) arc_start(false);   // only reads mods/cache
    }
    const int state = g_arc_state;
    // Test aids: LB_MAPS_SELECT=<index> picks a map, LB_MAPS_DOWNLOAD=1 then
    // presses its Download once.
    static bool aid_download = false;
    if (state == 2 && sel < 0) {
        if (const char* e = std::getenv("LB_MAPS_SELECT")) {
            sel = std::atoi(e);
            aid_download = std::getenv("LB_MAPS_DOWNLOAD") != nullptr;
        }
    }
    if (state == 2 && aid_download && sel >= 0 && sel < (int)g_arc.size()) {
        aid_download = false;
        if (!g_arc[(size_t)sel].installed) arc_download(g_arc[(size_t)sel]);
    }
    // LB_MAPS_REGTEST=<folder>: logs lb_mods_register_folder / folder_hash once.
    if (state == 2) {
        static bool tested = false;
        const char* t = std::getenv("LB_MAPS_REGTEST");
        if (t && !tested) {
            tested = true;
            unsigned long long h = 0;
            const int ok = lb_mods_folder_hash(t, &h);
            std::fprintf(stderr, "[lb_mapbrowser] regtest: hash ok=%d %016llx register=%d\n",
                         ok, h, lb_mods_register_folder(t));
            std::fflush(stderr);
        }
    }
    if (state != 2) {
        std::string msg;
        {
            std::lock_guard<std::mutex> lk(g_arc_mu);
            msg = g_arc_msg;
        }
        if (state == 1) {
            ImGui::TextUnformatted(msg.empty() ? "Loading the archive..." : msg.c_str());
            return;
        }
        ImGui::TextWrapped("Nothing is downloaded until you press the button. The two "
                           "map archives (the community's and the port's own) are a few "
                           "zips of about 30 MB in all, fetched once over HTTPS from "
                           "GitHub into mods/cache and read from there afterwards.");
        // Test aid: LB_MAPS_LOAD=1 presses Load the archive once.
        static bool aid_load = std::getenv("LB_MAPS_LOAD") != nullptr;
        if (ImGui::Button("Load the archive") || aid_load) {
            aid_load = false;
            auto_tried = true;
            arc_start(true);
        }
        if (!msg.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", msg.c_str());
        return;
    }

    {
        std::string warn;
        {
            std::lock_guard<std::mutex> lk(g_arc_mu);
            warn = g_arc_msg;
        }
        if (!warn.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", warn.c_str());
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("lb_arc_list", ImVec2(avail.x * 0.38f, avail.y), ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##arcsearch", "Search by name", search, sizeof search);
    ImGui::SetNextItemWidth(-1.0f);
    const char* kSizes[] = {"All sizes", "64x64 (small)", "96x96 (large)"};
    ImGui::Combo("##arcsize", &size_filter, kSizes, 3);
    ImGui::Separator();
    const std::string q = lower(search);
    std::string last_pack;
    int shown = 0;
    for (int i = 0; i < (int)g_arc.size(); ++i) {
        const ArcMap& m = g_arc[(size_t)i];
        if (!q.empty() && lower(m.name).find(q) == std::string::npos) continue;
        if (size_filter && m.st[0] != (size_filter == 1 ? 64 : 96)) continue;
        if (m.pack != last_pack) {
            last_pack = m.pack;
            ImGui::TextDisabled("%s", m.pack.c_str());
        }
        char label[256];
        std::snprintf(label, sizeof label, "%s  (%dx%d)%s###arc%d", lb_swear_shown(m.name.c_str()),
                      m.st[0], m.st[1], m.installed ? "  - installed" : "", i);
        if (ImGui::Selectable(label, sel == i)) sel = i;
        ++shown;
    }
    if (!shown) ImGui::TextDisabled("No map matches.");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("lb_arc_detail", ImVec2(0, avail.y), ImGuiChildFlags_Borders);
    if (sel < 0 || sel >= (int)g_arc.size()) {
        ImGui::TextDisabled("Pick a map in the list.");
        ImGui::EndChild();
        return;
    }
    ArcMap& m = g_arc[(size_t)sel];
    arc_texture(m, ren);
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextUnformatted(lb_swear_shown(m.name.c_str()));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("%s  /  %s", m.pack.c_str(), lb_swear_shown(m.folder.c_str()));
    ImGui::Spacing();
    if (m.tex && m.tsize > 0) {
        const float u = (float)m.tsize / 128.0f;
        ImGui::Image((ImTextureID)(intptr_t)m.tex, ImVec2(256.0f, 256.0f), ImVec2(0, 0),
                     ImVec2(u, u));
    } else {
        ImGui::TextDisabled("No preview.");
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    static const char* const kWorld[3] = {"King (castle)", "Pirate", "Mars"};
    ImGui::Text("Size: %d x %d", m.st[0], m.st[1]);
    ImGui::Text("Setting: %s", kWorld[m.st[2] >= 0 && m.st[2] <= 2 ? m.st[2] : 0]);
    ImGui::Text("Players: %d", m.st[3]);
    ImGui::Text("Mines: %d", m.st[4]);
    const float cells = (float)std::max(1, m.st[0] * m.st[1]);
    static const char* const kTerrain[5] = {"Land", "Shore", "Water", "Shallows", "Other"};
    ImGui::Spacing();
    ImGui::TextDisabled("Ground (share of the map)");
    for (int k = 0; k < 5; ++k) {
        const int c = m.st[5 + k];
        if (k == 4 && c == 0) continue;
        char t[32];
        std::snprintf(t, sizeof t, "%.0f%%", 100.0f * (float)c / cells);
        ImGui::Text("%-9s", kTerrain[k]);
        ImGui::SameLine(80.0f);
        ImGui::ProgressBar((float)c / cells, ImVec2(150.0f, 0.0f), t);
    }
    ImGui::EndGroup();
    ImGui::SetItemTooltip("Counted from the ground-type layer of the map file.");
    ImGui::Spacing();
    if (m.installed) {
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Installed");
        ImGui::SameLine();
        ImGui::TextDisabled("(it is in your mods/maps folder)");
    } else if (ImGui::Button("Download")) {
        arc_download(m);
    }
    ImGui::SetItemTooltip("Copies this map's folder into mods/maps/_archive. The game\n"
                          "reads mods/maps when it starts.");
    if (!g_dl_msg.empty()) ImGui::TextWrapped("%s", g_dl_msg.c_str());
    // docs/432: a Reset rebuilds the cartridge with the mods folder as it is now.
    if (g_dl_new && ImGui::Button("Load new maps now")) {
        if (lb_mods_request_rebuild()) {
            g_dl_new = false;
            g_dl_msg = "Loading the new maps: the game resets to the title screen.";
        } else {
            g_dl_msg = "Restart the game to see the new maps.";
        }
    }
    if (lb_mods_count() >= 30)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "The game loads at most 30 mod maps and %d are already in use.",
                           lb_mods_count());
    if (!m.notes.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Notes from the archive");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(lb_swear_shown(m.notes.c_str()));
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}
}  // namespace

// docs/431: the Maps page's body (Tools > Maps in lb_overlay.cpp).
void lb_mapbrowser_draw(SDL_Renderer* renderer) { arc_draw(renderer); }

// A lobby wants `name`, which this machine lacks. Once per name per run.
extern "C" void lb_mapfetch_request(const char* name) {
    if (!name || !*name || !lb_setting_fetch_maps()) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_asked.insert(lower(name)).second) return;
    }
    if (g_busy.exchange(true)) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_asked.erase(lower(name));   // ask again when this one is done
        return;
    }
    std::fprintf(stderr, "[lb_mapfetch] looking for \"%s\" in the map archive\n", name);
    std::fflush(stderr);
    std::thread([n = std::string(name)]() {
        // A zip's names may not be UTF-8, and a path built from one throws.
        try {
            fetch(n);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[lb_mapfetch] \"%s\": failed (%s)\n", n.c_str(), e.what());
            std::fflush(stderr);
            g_busy = false;
        }
    }).detach();
}

// docs/431: find the map whose content hash (docs/413: map, detail tiles,
// minimaps; lb_mods_folder_hash) is `hash` among the sources' Free Play maps and
// extract it into mods/maps/_archive/<its path in the zip>/. Returns 1 and the
// folder's path in `path_out`, or 0 (not found, or a source could not be read;
// `err_out` says which). BLOCKS: it may download the zips (once) and extracts
// every candidate folder to a scratch folder to hash it, so call it from a
// worker thread, never the frame. The ROM must be attached (lb_mods_folder_hash
// reads stock detail tiles from it). The game still has to be restarted to use
// the map (lb_mods_register_folder).
extern "C" int lb_mapfetch_install_by_hash(unsigned long long hash, char* path_out, int cap,
                                           char* err_out, int errcap) {
    if (err_out && errcap > 0) err_out[0] = '\0';
    if (g_arc_state != 2) {
        arc_load(true);   // the state it leaves is 2 when anything was listed
        if (g_arc_state != 2) {
            if (err_out && errcap > 0) {
                std::lock_guard<std::mutex> lk(g_arc_mu);
                std::snprintf(err_out, (size_t)errcap, "%s", g_arc_msg.c_str());
            }
            return 0;
        }
    }
    const fs::path mods = fs::u8path(lb_mods_dir());
    const fs::path probe = mods / "cache" / "_probe";
    std::error_code ec;
    int found = 0;
    for (int si = 0; si < kNumSources && !found; ++si)
        for (int z = 0; z < kSources[si].nbrowse && !found; ++z) {
            std::string zip, err;
            if (!load_zip(kSources[si], kSources[si].zips[z], true, &zip, &err)) continue;
            for (const ArcMap& m : g_arc) {
                if (m.src != si || m.zip != z) continue;
                fs::remove_all(probe, ec);
                if (extract_folder(zip, m.folder, probe) <= 0) continue;
                unsigned long long h = 0;
                if (!lb_mods_folder_hash((probe / fs::u8path(m.folder)).string().c_str(), &h) ||
                    h != hash)
                    continue;
                const fs::path dest = mods / "maps" / "_archive";
                if (extract_folder(zip, m.folder, dest) <= 0) continue;
                if (path_out && cap > 0)
                    std::snprintf(path_out, (size_t)cap, "%s",
                                  (dest / fs::u8path(m.folder)).string().c_str());
                found = 1;
                break;
            }
        }
    fs::remove_all(probe, ec);
    return found;
}
