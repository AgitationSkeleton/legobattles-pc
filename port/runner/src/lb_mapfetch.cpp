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
extern "C" int lb_mods_limit(void);   // docs/439
extern "C" int lb_mods_register_folder(const char* path);
extern "C" int lb_mods_folder_hash(const char* path, unsigned long long* hash);
extern "C" int lb_matchpic_mini_rgba(const unsigned char* ncgr, size_t n, int world,
                                     unsigned int* out, int* size);

namespace {
namespace fs = std::filesystem;

// docs/431: where maps come from. Each source is a GitHub repo whose maps sit in
// zips at the repo's top. To add a source, add one entry: its zips are cached
// in mods/cache/<cache>/ (the first source keeps mods/cache/ itself).
// docs/440: which zips a repo holds is asked of GitHub (list_zips), so a pack
// added to either repo shows without a new build; `zips` is only the fallback
// when GitHub and the cached index are both out of reach. Every zip is read: a
// folder that is not a Free Play map (the campaign fixes) is left out by the
// browser's own rules.
struct Source {
    const char* name;      // shown in the Maps page
    const char* repo;      // owner/name on its forge
    const char* host;      // raw file host
    const char* base;      // path up to the zips, with both slashes
    const char* cache;     // subfolder of mods/cache ("" for none)
    const char* zips[8];   // the fallback list
    int nzips;
    const char* api_host;  // the forge's API (GitHub's, Codeberg's: the same tree JSON)
    const char* tree;      // the top-level tree of the default branch
    bool lbz;              // one map per .lbz file (LiruJ's map tool) instead of zips of folders
};
const Source kSources[] = {
    // lnee's (codeberg.org/lnee/lbzmaps): each map an .lbz, the package
    // LegoBattlesMapTool writes -- a zip of map.map, mapimages/@mini*.NCGR/.NCBR,
    // detailtiles/detailtiles_@.tbp (optional) and manifest.toml, `@` standing
    // for the map's name. Listed first, by the owner's wish.
    {"lnee's maps (Codeberg)", "lnee/lbzmaps", "codeberg.org", "/lnee/lbzmaps/raw/branch/master/",
     "lnee", {}, 0, "codeberg.org", "/api/v1/repos/lnee/lbzmaps/git/trees/master", true},
    {"Community map archive (Ryncat2000)", "Ryncat2000/Lego-Battles-Custom-Map-Archive",
     "raw.githubusercontent.com", "/Ryncat2000/Lego-Battles-Custom-Map-Archive/main/", "",
     {"64 sized.zip", "96 sized.zip", "Better Free Play maps pack.zip",
      "DIFFICULTY PATCH-first wave.zip"},
     4, "api.github.com", "/repos/Ryncat2000/Lego-Battles-Custom-Map-Archive/git/trees/main", false},
    // The port's own (github.com/AgitationSkeleton/legobattles-custom-maps):
    // the cut Mars maps rebuilt from their minimaps, the prototypes' Free
    // Play maps that differ from the final game's, and Ninjago's Free Play maps
    // converted to the retail format.
    {"LEGO Battles PC custom maps", "AgitationSkeleton/legobattles-custom-maps",
     "raw.githubusercontent.com", "/AgitationSkeleton/legobattles-custom-maps/main/", "custom",
     {"Mars beta maps.zip", "Prototype Free Play maps.zip", "Ninjago Free Play maps.zip"},
     3, "api.github.com", "/repos/AgitationSkeleton/legobattles-custom-maps/git/trees/main", false},
};
const int kNumSources = (int)(sizeof kSources / sizeof kSources[0]);

// A zip of a source, and its git blob id (empty when unknown).
struct Listed {
    std::string name, sha;
};

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
fs::path cache_dir(const Source& src) {
    return fs::u8path(lb_mods_dir()) / "cache" / fs::u8path(src.cache);
}
std::string read_text(const fs::path& p) {
    std::string s;
    if (FILE* f = std::fopen(p.string().c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && s.size() < (1u << 20)) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}
void write_text(const fs::path& p, const std::string& s) {
    if (FILE* f = std::fopen(p.string().c_str(), "wb")) {
        std::fwrite(s.data(), 1, s.size(), f);
        std::fclose(f);
    }
}

// docs/440: the zips at the top of the source's repo, as GitHub's tree API lists
// them now (`online`), else as last listed (mods/cache/<cache>/index.txt), else
// the built-in list. Each comes with its blob id, so a zip changed in the repo
// is downloaded again (load_zip).
std::vector<Listed> list_zips(const Source& src, bool online) {
    std::vector<Listed> out;
    std::error_code ec;
    const fs::path dir = cache_dir(src);
    fs::create_directories(dir, ec);
    const fs::path index = dir / "index.txt";
    if (online) {
        std::string body, err;
        const std::string suffix = src.lbz ? ".lbz" : ".zip";
        if (lb_https_download(src.api_host, src.tree, &body, 4u << 20, &err)) {
            const std::string key = "\"path\":\"";
            for (size_t p = body.find(key); p != std::string::npos; p = body.find(key, p + 1)) {
                const size_t a = p + key.size(), b = body.find('"', a);
                if (b == std::string::npos) break;
                const std::string name = body.substr(a, b - a);
                const size_t end = body.find('}', b);
                const std::string obj = body.substr(b, end == std::string::npos ? std::string::npos
                                                                                : end - b);
                if (name.find('/') != std::string::npos || name.size() < 5 ||
                    lower(name.substr(name.size() - 4)) != suffix ||
                    obj.find("\"type\":\"blob\"") == std::string::npos)
                    continue;
                std::string sha;
                const size_t s = obj.find("\"sha\":\"");
                if (s != std::string::npos) sha = obj.substr(s + 7, 40);
                out.push_back({name, sha});
            }
            if (!out.empty()) {
                std::string t;
                for (const Listed& l : out) t += l.sha + "\t" + l.name + "\n";
                write_text(index, t);
            }
        } else {
            std::fprintf(stderr, "[lb_mapfetch] %s: the repo's list could not be read (%s)\n",
                         src.repo, err.c_str());
        }
    }
    if (out.empty()) {
        const std::string t = read_text(index);
        size_t a = 0;
        while (a < t.size()) {
            size_t e = t.find('\n', a);
            if (e == std::string::npos) e = t.size();
            const std::string line = t.substr(a, e - a);
            const size_t tab = line.find('\t');
            if (tab != std::string::npos && tab + 1 < line.size())
                out.push_back({line.substr(tab + 1), line.substr(0, tab)});
            a = e + 1;
        }
    }
    if (out.empty())
        for (int z = 0; z < src.nzips; ++z) out.push_back({src.zips[z], ""});
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
        // A file at the zip's root (an .lbz's map.map and manifest.toml) has
        // the folder "".
        const size_t slash = e.name.rfind('/');
        e.folder = slash == std::string::npos ? std::string() : e.name.substr(0, slash);
        e.file = slash == std::string::npos ? e.name : e.name.substr(slash + 1);
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

// ---- .lbz: one map per file (LegoBattlesMapTool's package) ----
//
// A zip of map.map, mapimages/@mini.NCGR/.NCBR and @miniNT.NCGR/.NCBR,
// optionally detailtiles/detailtiles_@.tbp, and manifest.toml (type, name,
// creator, args = ["mpNN"]: the stock slot it was made from). Every file is
// already in the game's COMP container, as the loader wants. Installed, it
// becomes an ordinary map folder: <folder>/<stem>.map, <stem>mini.NCGR ...,
// DetailTiles_<stem>.tbp.

bool ends_ci(const std::string& s, const std::string& suf);   // below

// A manifest's `key = "value"` (or the first string of `key = [ "..." ]`).
std::string toml_str(const std::string& t, const char* key) {
    const std::string k = std::string(key);
    for (size_t p = 0; (p = t.find(k, p)) != std::string::npos; p += k.size()) {
        if (p && t[p - 1] != '\n') continue;
        const size_t q0 = t.find('"', p + k.size());
        const size_t eol = t.find('\n', p);
        if (q0 == std::string::npos || (eol != std::string::npos && q0 > eol)) return {};
        const size_t q1 = t.find('"', q0 + 1);
        if (q1 == std::string::npos) return {};
        return t.substr(q0 + 1, q1 - q0 - 1);
    }
    return {};
}
// A name safe as a folder and file stem.
std::string lbz_stem(const std::string& file) {
    std::string s = file.size() > 4 ? file.substr(0, file.size() - 4) : file;
    std::string out;
    for (char c : s)
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') out += c;
    return out.empty() ? std::string("map") : out;
}
// The folder an .lbz installs as: its stem, or "NN-stem" when it brings no
// detail tiles of its own and names a stock slot, so the loader takes that
// slot's (docs/419's folder rule).
std::string lbz_folder(const std::string& zip, const std::string& file) {
    std::vector<ZipEnt> ents;
    bool tiles = false;
    std::string manifest;
    if (zip_list(zip, &ents))
        for (const ZipEnt& e : ents) {
            // (some packages carry an empty, 0-byte one: no tiles)
            if (lower(e.name).rfind("detailtiles/", 0) == 0 && ends_ci(e.name, ".tbp") && e.usize > 0)
                tiles = true;
            if (lower(e.name) == "manifest.toml")
                zip_entry(zip, e.local, e.method, e.csize, e.usize, &manifest, 64u << 10);
        }
    const std::string stem = lbz_stem(file);
    const std::string slot = toml_str(manifest, "args");
    int n = 0;
    if (lower(slot).rfind("mp", 0) == 0) n = std::atoi(slot.c_str() + 2);
    if (!tiles && n >= 1 && n <= 30) {
        char pre[8];
        std::snprintf(pre, sizeof pre, "%02d-", n);
        return pre + stem;
    }
    return stem;
}
// Unpacks the .lbz `file` into dest/<folder>/. The map files take the folder's
// stem without a slot prefix.
int extract_lbz(const std::string& zip, const std::string& folder, const fs::path& dest) {
    std::vector<ZipEnt> ents;
    if (!zip_list(zip, &ents)) return 0;
    const size_t dash = folder.size() > 3 && folder[2] == '-' && std::isdigit((unsigned char)folder[0])
                            ? 3 : 0;
    const std::string stem = folder.substr(dash);
    std::error_code ec;
    const fs::path dir = dest / fs::u8path(folder);
    fs::create_directories(dir, ec);
    int written = 0;
    for (const ZipEnt& e : ents) {
        const std::string n = lower(e.name);
        std::string out;
        if (n == "map.map") out = stem + ".map";
        else if (n.rfind("mapimages/@", 0) == 0) out = stem + e.name.substr(11);   // "mini.NCGR"...
        else if (n.rfind("detailtiles/detailtiles_@", 0) == 0 && ends_ci(n, ".tbp"))
            out = "DetailTiles_" + stem + ".tbp";
        else if (n == "manifest.toml") out = "manifest.toml";
        if (out.empty() || out.find('/') != std::string::npos || out.find("..") != std::string::npos ||
            e.usize == 0)   // an empty file is no file (the loader would refuse it)
            continue;
        std::string bytes;
        if (!zip_entry(zip, e.local, e.method, e.csize, e.usize, &bytes, 8u << 20)) continue;
        if (FILE* f = std::fopen((dir / fs::u8path(out)).string().c_str(), "wb")) {
            if (std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size()) ++written;
            std::fclose(f);
        }
    }
    return written;
}
// One map of a source extracted under `dest`: a folder of a zip, or an .lbz.
int extract_map(const Source& src, const std::string& zip, const std::string& folder,
                const fs::path& dest) {
    return src.lbz ? extract_lbz(zip, folder, dest) : extract_folder(zip, folder, dest);
}

// One of the archive's zips: the cached copy in mods/cache, else (when
// `download`) fetched over HTTPS and cached. docs/440: a cached copy whose blob
// id (<zip>.sha beside it) is not `sha` is out of date: when downloading, it is
// fetched again (and kept if that fails).
bool load_zip(const Source& src, const Listed& lz, bool download, std::string* zip,
              std::string* error) {
    const char* z = lz.name.c_str();
    const fs::path cache = cache_dir(src);
    std::error_code ec;
    fs::create_directories(cache, ec);
    const fs::path local = cache / fs::u8path(z);
    const fs::path sha_file = cache / fs::u8path(lz.name + ".sha");
    zip->clear();
    const bool stale = download && !lz.sha.empty() && fs::is_regular_file(local, ec) &&
                       read_text(sha_file) != lz.sha;
    if (stale) {
        std::string fresh, err;
        if (lb_https_download(src.host, std::string(src.base) + url_path(z), &fresh, 64u << 20,
                              &err)) {
            write_text(local, fresh);
            write_text(sha_file, lz.sha);
            std::fprintf(stderr, "[lb_mapfetch] %s changed in the repo: downloaded again "
                         "(%zu bytes)\n", z, fresh.size());
            *zip = std::move(fresh);
            return true;
        }
        std::fprintf(stderr, "[lb_mapfetch] %s changed in the repo but could not be "
                     "downloaded (%s); the cached copy is used\n", z, err.c_str());
    }
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
    write_text(local, *zip);
    if (!lz.sha.empty()) write_text(sha_file, lz.sha);
    std::fprintf(stderr, "[lb_mapfetch] downloaded %s (%zu bytes)\n", z, zip->size());
    return true;
}

void fetch(std::string name) {
    const std::string want = lower(name);
    const fs::path mods = fs::u8path(lb_mods_dir());
    int files = 0;
    std::string last_error;
    for (const Source& src : kSources)
        for (const Listed& lz : list_zips(src, true)) {
            std::string zip;
            if (!load_zip(src, lz, true, &zip, &last_error)) continue;
            if (src.lbz) {   // an .lbz is one map, installed under its folder name
                const std::string folder = lbz_folder(zip, lz.name);
                if (lower(folder) == want || lower(lbz_stem(lz.name)) == want)
                    files += extract_lbz(zip, folder, mods / "maps" / "_archive");
                continue;
            }
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
// ---- docs/431: the Maps page (the Maps header) ----------------------------------
//
// Lists the archive's Free Play maps one by one: a thumbnail (the map's own
// minimap pair, drawn with the map select's palette), what its .map file says
// (size, world, players, mines, ground types) and its notes. A folder is
// listed when it holds exactly one .map (an empty one does not count) and that
// map's four minimap files, or none when it stands in for a stock slot (mpNN,
// or a folder named "29-..."), whose minimap it then takes: the rules
// lb_mods.cpp loads by. The campaign fixes' folders (several maps each) are
// left out by them. Nothing is downloaded until the player presses the button
// (or the zips are already cached, which only reads them).

extern "C" int lb_rom_read(const char* path, unsigned char* out, int cap);   /* lb_teams.cpp */

struct ArcMap {
    std::string name, folder, pack, notes;
    int src = 0, zip = 0;       // the source, and its zip's place in the listing
    Listed lz;                  // that zip
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
    // Anywhere under mods/ (lb_mods.cpp reads map folders there), but not the
    // downloaded zips in mods/cache/.
    const fs::path loose = fs::u8path(lb_mods_dir());
    if (!fs::is_directory(loose, ec)) return;
    for (auto it = fs::recursive_directory_iterator(loose, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it.depth() == 0 && it->is_directory(ec) &&
            lower(it->path().filename().string()) == "cache") {
            it.disable_recursion_pending();
            continue;
        }
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
    for (int si = 0; si < kNumSources; ++si) {
      const Source& src = kSources[si];
      const std::vector<Listed> zl = list_zips(src, download);
      for (int z = 0; z < (int)zl.size(); ++z) {
        {
            std::lock_guard<std::mutex> lk(g_arc_mu);
            g_arc_msg = std::string(download ? "Loading " : "Reading ") + zl[z].name + "...";
        }
        std::string zip, err;
        std::vector<ZipEnt> ents;
        if (!load_zip(src, zl[z], download, &zip, &err) || !zip_list(zip, &ents)) {
            warn += (err.empty() ? zl[z].name + " could not be read" : err) + ". ";
            continue;
        }
        if (src.lbz) {   // one map per .lbz: its fixed member names
            const ZipEnt *map_e = nullptr, *mini = nullptr, *man = nullptr;
            for (const ZipEnt& e : ents) {
                const std::string n = lower(e.name);
                if (n == "map.map") map_e = &e;
                else if (n == "mapimages/@mini.ncgr") mini = &e;
                else if (n == "manifest.toml") man = &e;
            }
            std::string mapb, ncgr, manifest;
            const char* why = nullptr;
            if (!map_e || !mini) why = "no map.map or mapimages/@mini.NCGR";
            else if (!zip_entry(zip, map_e->local, map_e->method, map_e->csize, map_e->usize, &mapb, 8u << 20) ||
                     !zip_entry(zip, mini->local, mini->method, mini->csize, mini->usize, &ncgr, 1u << 20))
                why = "its files could not be read";
            if (man && !why)
                zip_entry(zip, man->local, man->method, man->csize, man->usize, &manifest, 64u << 10);
            ArcMap m;
            if (!why && !lb_mods_map_stats((const unsigned char*)mapb.data(), mapb.size(), m.st))
                why = "map.map is not a map the game reads";
            if (why) {
                std::fprintf(stderr, "[lb_mapbrowser] %s left out: %s (%zu entries)\n",
                             zl[z].name.c_str(), why, ents.size());
                continue;
            }
            m.src = si;
            m.zip = z;
            m.lz = zl[z];
            m.folder = lbz_folder(zip, zl[z].name);
            m.name = lbz_stem(zl[z].name);
            m.pack = src.name;
            m.hash = fnv_bytes(mapb);
            m.ncgr.assign(ncgr.begin(), ncgr.end());
            const std::string by = toml_str(manifest, "creator");
            const std::string title = toml_str(manifest, "name");
            const std::string slot = toml_str(manifest, "args");
            m.notes = zl[z].name;
            if (!title.empty() && lower(title) != lower(m.name)) m.notes += ": \"" + title + "\"";
            if (!by.empty()) m.notes += ", by " + by;
            if (!slot.empty()) m.notes += " (made from slot " + slot + ")";
            m.notes += ".";
            maps.push_back(std::move(m));
            continue;
        }
        std::map<std::string, std::vector<size_t>> by;
        for (size_t i = 0; i < ents.size(); ++i) by[ents[i].folder].push_back(i);
        for (const auto& kv : by) {
            const ZipEnt* map_e = nullptr;
            int map_n = 0;
            for (size_t i : kv.second)
                if (ends_ci(ents[i].file, ".map") && ents[i].usize > 0) {
                    map_e = &ents[i];
                    ++map_n;
                }
            if (map_n != 1) continue;
            static const char* const kMini[4] = {"mini.NCGR", "mini.NCBR", "miniNT.NCGR",
                                                 "miniNT.NCBR"};
            const std::string stem = map_e->file.substr(0, map_e->file.size() - 4);
            const size_t up = kv.first.rfind('/');
            const std::string leaf = up == std::string::npos ? kv.first : kv.first.substr(up + 1);
            const ZipEnt* mini[4] = {nullptr, nullptr, nullptr, nullptr};
            for (int k = 0; k < 4; ++k)
                for (size_t i : kv.second)
                    if (lower(ents[i].file) == lower(stem + kMini[k])) mini[k] = &ents[i];
            std::string mapb, ncgr;
            if (!zip_entry(zip, map_e->local, map_e->method, map_e->csize, map_e->usize,
                           &mapb, 8u << 20))
                continue;
            if (mini[0] && mini[1] && mini[2] && mini[3]) {
                if (!zip_entry(zip, mini[0]->local, mini[0]->method, mini[0]->csize,
                               mini[0]->usize, &ncgr, 1u << 20))
                    continue;
            } else {
                // No minimap of its own: a slot map's is the stock slot's
                // (lb_mods.cpp loose_folder_map). The slot is mpNN, else the
                // folder's leading number.
                int slot = 0;
                if (lower(stem).rfind("mp", 0) == 0) slot = std::atoi(stem.c_str() + 2);
                if ((slot < 1 || slot > 30) && !leaf.empty() &&
                    std::isdigit((unsigned char)leaf[0])) {
                    size_t d = 0;
                    while (d < leaf.size() && std::isdigit((unsigned char)leaf[d])) ++d;
                    if (d < leaf.size() && (leaf[d] == '-' || leaf[d] == ' '))
                        slot = std::atoi(leaf.c_str());
                }
                if (slot < 1 || slot > 30) continue;
                char nm[32];
                std::snprintf(nm, sizeof nm, "mp%02dmini.NCGR", slot);
                const int n = lb_rom_read(nm, nullptr, 0);
                if (n <= 0 || n > (1 << 20)) continue;
                ncgr.assign((size_t)n, '\0');
                if (lb_rom_read(nm, (unsigned char*)&ncgr[0], n) != n) continue;
            }
            ArcMap m;
            if (!lb_mods_map_stats((const unsigned char*)mapb.data(), mapb.size(), m.st))
                continue;
            m.src = si;
            m.zip = z;
            m.lz = zl[z];
            m.folder = kv.first;
            m.name = leaf;
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
    for (const Source& src : kSources) {
        // A source never listed yet (one added in a newer build: nothing cached,
        // no built-in list) is not cached, or the page would never fetch it.
        const std::vector<Listed> zl = list_zips(src, false);
        if (zl.empty()) return false;
        for (const Listed& lz : zl)
            if (!fs::is_regular_file(cache_dir(src) / fs::u8path(lz.name), ec))
                return false;
    }
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
    if (!load_zip(kSources[m.src], m.lz, false, &zip, &err)) {
        g_dl_msg = "The archive is not cached: " + err;
        return;
    }
    const int n = extract_map(kSources[m.src], zip, m.folder, fs::u8path(lb_mods_dir()) / "maps" / "_archive");
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

// SG-0030: DOWNLOAD ALL, for a pack or every pack. A worker extracts the maps
// that are not installed yet, each zip read once (a pack is up to 16 MB), from
// a copy of the list; the page marks them installed when it is done.
struct DlJob { int src; Listed lz; std::string folder; };
std::atomic<bool> g_dlm_busy{false}, g_dlm_done{false};
std::string g_dlm_msg;   // under g_arc_mu

void arc_download_many(const std::vector<size_t>& idx) {
    std::vector<DlJob> jobs;
    for (size_t i : idx)
        if (i < g_arc.size() && !g_arc[i].installed)
            jobs.push_back({g_arc[i].src, g_arc[i].lz, g_arc[i].folder});
    if (jobs.empty() || g_dlm_busy.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lk(g_arc_mu);
        g_dlm_msg = "Downloading " + std::to_string(jobs.size()) + " map(s)...";
    }
    std::thread([jobs]() {
        int maps = 0, failed = 0;
        std::string zip, zip_of, err;
        try {
            for (const DlJob& j : jobs) {
                const std::string key = std::to_string(j.src) + "/" + j.lz.name;
                if (key != zip_of) {
                    zip.clear();
                    zip_of = key;
                    if (!load_zip(kSources[j.src], j.lz, false, &zip, &err)) zip.clear();
                }
                if (zip.empty() ||
                    extract_map(kSources[j.src], zip, j.folder, fs::u8path(lb_mods_dir()) / "maps" / "_archive") <= 0)
                    ++failed;
                else
                    ++maps;
            }
        } catch (const std::exception& e) {
            err = e.what();
            ++failed;
        }
        std::fprintf(stderr, "[lb_mapbrowser] download all: %d map(s) extracted, %d failed\n",
                     maps, failed);
        std::fflush(stderr);
        {
            std::lock_guard<std::mutex> lk(g_arc_mu);
            g_dlm_msg = std::to_string(maps) + " map(s) downloaded into mods/maps/_archive" +
                        (failed ? " (" + std::to_string(failed) + " could not be written)" : "") +
                        ". Press Load new maps now to see them in the Free Play map list.";
        }
        g_dlm_done = true;
        g_dlm_busy = false;
    }).detach();
}

void arc_draw(SDL_Renderer* ren) {
    if (g_dlm_done.exchange(false)) {   // SG-0030: a batch finished
        arc_scan_installed();
        arc_mark_installed();
        g_dl_new = true;
        std::lock_guard<std::mutex> lk(g_arc_mu);
        g_dl_msg = g_dlm_msg;
    }
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
        ImGui::TextWrapped("Nothing is downloaded until you press the button. The map "
                           "archives (lnee's, the community's and the port's own) are a few "
                           "files of about 50 MB in all, fetched over HTTPS from GitHub and "
                           "Codeberg into mods/cache and read from there afterwards. A pack that "
                           "changes in its archive is fetched again the next time you "
                           "press it.");
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
    // THE CATALOGUE: every map's thumbnail with its name
    // beneath, in a grid grouped by pack; a click shows that map's details
    // beside it.
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##arcsearch", "Search by name", search, sizeof search);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    const char* kSizes[] = {"All sizes", "64x64 (small)", "96x96 (large)"};
    ImGui::Combo("##arcsize", &size_filter, kSizes, 3);
    {   // SG-0030: every map of every pack not installed yet.
        std::vector<size_t> missing;
        for (size_t i = 0; i < g_arc.size(); ++i)
            if (!g_arc[i].installed) missing.push_back(i);
        ImGui::SameLine();
        const bool busy = g_dlm_busy.load();
        ImGui::BeginDisabled(missing.empty() || busy);
        char label[64];
        std::snprintf(label, sizeof label, "Download all (%zu)", missing.size());
        // Test aid: LB_MAPS_DOWNLOAD_ALL=1 presses it once.
        static bool aid_all = std::getenv("LB_MAPS_DOWNLOAD_ALL") != nullptr;
        if (ImGui::Button(busy ? "Downloading..." : label) || (aid_all && !missing.empty())) {
            aid_all = false;
            arc_download_many(missing);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Copies every map not yet installed, from every pack, into\n"
                              "mods/maps/_archive. The game loads at most %d mod maps.",
                              lb_mods_limit());
        ImGui::SameLine();
        if (ImGui::Button("Check the archives for new maps")) arc_start(true);
        ImGui::SetItemTooltip("Asks each archive what it holds now and downloads the packs\n"
                              "that are new or changed. Otherwise the page reads what is\n"
                              "already in mods/cache.");
        if (!g_dl_msg.empty() && sel < 0) ImGui::TextWrapped("%s", g_dl_msg.c_str());
        if (g_dl_new && sel < 0 && ImGui::Button("Load new maps now##top")) {
            if (lb_mods_request_rebuild()) {
                g_dl_new = false;
                g_dl_msg = "Loading the new maps: the game resets to the title screen.";
            } else {
                g_dl_msg = "Restart the game to see the new maps.";
            }
        }
        if (lb_mods_count() + (int)missing.size() > lb_mods_limit() && !missing.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "With every map installed there would be more than the %d "
                               "the game loads; the rest are left out.", lb_mods_limit());
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float detail_w = sel >= 0 ? std::max(300.0f, avail.x * 0.34f) : 0.0f;
    ImGui::BeginChild("lb_arc_grid", ImVec2(sel >= 0 ? avail.x - detail_w - 8.0f : 0.0f, avail.y),
                      ImGuiChildFlags_Borders);
    const std::string q = lower(search);
    const float tile = 112.0f, pad = 12.0f;
    // A tile is the image button with its frame (and the name, cut to it): the
    // next one goes on this row only while it fits in what is left of it.
    const float cell = tile + 2.0f * ImGui::GetStyle().FramePadding.x;
    const float row_end = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    std::string last_pack;
    int shown = 0, col = 0;
    for (int i = 0; i < (int)g_arc.size(); ++i) {
        ArcMap& m = g_arc[(size_t)i];
        if (!q.empty() && lower(m.name).find(q) == std::string::npos) continue;
        if (size_filter && m.st[0] != (size_filter == 1 ? 64 : 96)) continue;
        if (m.pack != last_pack) {
            last_pack = m.pack;
            if (col) ImGui::NewLine();
            col = 0;
            ImGui::Spacing();
            ImGui::TextDisabled("%s", m.pack.c_str());
            {   // SG-0030: the pack's maps not installed yet.
                std::vector<size_t> missing;
                for (size_t k = 0; k < g_arc.size(); ++k)
                    if (g_arc[k].pack == m.pack && !g_arc[k].installed) missing.push_back(k);
                if (!missing.empty()) {
                    ImGui::SameLine();
                    char label[64];
                    std::snprintf(label, sizeof label, "Download pack (%zu)##%d", missing.size(), i);
                    ImGui::BeginDisabled(g_dlm_busy.load());
                    if (ImGui::SmallButton(label)) arc_download_many(missing);
                    ImGui::EndDisabled();
                }
            }
            ImGui::Separator();
        }
        if (col && ImGui::GetItemRectMax().x + pad + cell <= row_end) ImGui::SameLine(0.0f, pad);
        else col = 0;
        ImGui::BeginGroup();
        ImGui::PushID(i);
        arc_texture(m, ren);
        const ImVec4 tint = m.installed ? ImVec4(0.75f, 1.0f, 0.75f, 1.0f)
                                        : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        const ImVec4 bg = sel == i ? ImVec4(0.95f, 0.75f, 0.15f, 1.0f) : ImVec4(0, 0, 0, 0);
        bool hit;
        if (m.tex && m.tsize > 0) {
            const float u = (float)m.tsize / 128.0f;
            hit = ImGui::ImageButton("##t", (ImTextureID)(intptr_t)m.tex, ImVec2(tile, tile),
                                     ImVec2(0, 0), ImVec2(u, u), bg, tint);
        } else {
            hit = ImGui::Button("?", ImVec2(tile + 8.0f, tile + 8.0f));
        }
        if (hit) sel = (sel == i) ? -1 : i;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%dx%d, %d players%s", lb_swear_shown(m.name.c_str()), m.st[0],
                              m.st[1], m.st[3], m.installed ? "\nInstalled" : "");
        // The name beneath, cut to the tile's width.
        std::string nm = lb_swear_shown(m.name.c_str());
        while (nm.size() > 3 && ImGui::CalcTextSize(nm.c_str()).x > tile + 8.0f)
            nm = nm.substr(0, nm.size() - 4) + "..";
        const float nx = (tile + 8.0f - ImGui::CalcTextSize(nm.c_str()).x) * 0.5f;
        if (nx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + nx);
        if (m.installed) ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", nm.c_str());
        else ImGui::TextUnformatted(nm.c_str());
        ImGui::PopID();
        ImGui::EndGroup();
        ++col;
        ++shown;
    }
    if (!shown) ImGui::TextDisabled("No map matches.");
    ImGui::EndChild();
    if (sel < 0 || sel >= (int)g_arc.size()) {
        sel = -1;
        return;
    }
    ImGui::SameLine();
    ImGui::BeginChild("lb_arc_detail", ImVec2(0, avail.y), ImGuiChildFlags_Borders);
    ArcMap& m = g_arc[(size_t)sel];
    arc_texture(m, ren);
    ImGui::SetWindowFontScale(1.2f);
    ImGui::TextUnformatted(lb_swear_shown(m.name.c_str()));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("%s  /  %s", m.pack.c_str(), lb_swear_shown(m.folder.c_str()));
    ImGui::Spacing();
    if (m.tex && m.tsize > 0) {
        const float u = (float)m.tsize / 128.0f;
        ImGui::Image((ImTextureID)(intptr_t)m.tex, ImVec2(192.0f, 192.0f), ImVec2(0, 0),
                     ImVec2(u, u));
    } else {
        ImGui::TextDisabled("No preview.");
    }
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
    if (lb_mods_count() >= lb_mods_limit())
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "The game loads at most %d mod maps and %d are already in use.",
                           lb_mods_limit(), lb_mods_count());
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

// docs/431: the Maps page's body (the Maps header in lb_overlay.cpp).
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
    // Each zip the listing read, once.
    std::vector<std::pair<int, Listed>> zips;
    for (const ArcMap& m : g_arc)
        if (zips.empty() || zips.back().first != m.src || zips.back().second.name != m.lz.name)
            zips.push_back({m.src, m.lz});
    for (size_t zi = 0; zi < zips.size() && !found; ++zi) {
            const int si = zips[zi].first;
            const Listed& lz = zips[zi].second;
            std::string zip, err;
            if (!load_zip(kSources[si], lz, true, &zip, &err)) continue;
            for (const ArcMap& m : g_arc) {
                if (m.src != si || m.lz.name != lz.name) continue;
                fs::remove_all(probe, ec);
                if (extract_map(kSources[si], zip, m.folder, probe) <= 0) continue;
                unsigned long long h = 0;
                if (!lb_mods_folder_hash((probe / fs::u8path(m.folder)).string().c_str(), &h) ||
                    h != hash)
                    continue;
                const fs::path dest = mods / "maps" / "_archive";
                if (extract_map(kSources[si], zip, m.folder, dest) <= 0) continue;
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
