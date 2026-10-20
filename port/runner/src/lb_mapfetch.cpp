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

namespace {
namespace fs = std::filesystem;

const char* const kHost = "raw.githubusercontent.com";
const char* const kBase = "/Ryncat2000/Lego-Battles-Custom-Map-Archive/main/";
const char* const kZips[] = {"64 sized.zip", "96 sized.zip",
                             "Better Free Play maps pack.zip",
                             "DIFFICULTY PATCH-first wave.zip"};

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

// Every folder of `zip` named `want` (lowercase), extracted under `dest`.
int extract_named(const std::string& zip, const std::string& want, const fs::path& dest) {
    // The end-of-central-directory record, searched from the end.
    if (zip.size() < 22) return 0;
    size_t eocd = std::string::npos;
    for (size_t i = zip.size() - 22; i + 1 > 0 && zip.size() - i < 66000; --i) {
        if (le32(zip, i) == 0x06054B50u) { eocd = i; break; }
        if (i == 0) break;
    }
    if (eocd == std::string::npos) return 0;
    const uint32_t count = le16(zip, eocd + 10);
    size_t at = le32(zip, eocd + 16);
    int written = 0;
    for (uint32_t k = 0; k < count && at + 46 <= zip.size(); ++k) {
        if (le32(zip, at) != 0x02014B50u) break;
        const uint32_t method = le16(zip, at + 10);
        const uint32_t csize = le32(zip, at + 20), usize = le32(zip, at + 24);
        const uint32_t nlen = le16(zip, at + 28), xlen = le16(zip, at + 30);
        const uint32_t clen = le16(zip, at + 32);
        const uint32_t local = le32(zip, at + 42);
        std::string name = zip.substr(at + 46, nlen);
        at += 46 + nlen + xlen + clen;
        for (char& c : name) if (c == '\\') c = '/';
        if (name.empty() || name.back() == '/') continue;   // a folder entry
        const size_t slash = name.rfind('/');
        if (slash == std::string::npos) continue;
        const std::string folder = name.substr(0, slash);
        const size_t up = folder.rfind('/');
        const std::string leaf = up == std::string::npos ? folder : folder.substr(up + 1);
        if (lower(leaf) != want) continue;
        // Inside the folder only: no "..", no drive or absolute paths.
        const std::string file = name.substr(slash + 1);
        if (file.empty() || file.find("..") != std::string::npos ||
            file.find(':') != std::string::npos || name.find("..") != std::string::npos)
            continue;
        std::string bytes;
        if (!zip_entry(zip, local, method, csize, usize, &bytes, 8u << 20)) continue;
        std::error_code ec;
        const fs::path dir = dest / fs::u8path(folder);
        fs::create_directories(dir, ec);
        FILE* f = std::fopen((dir / fs::u8path(file)).string().c_str(), "wb");
        if (!f) continue;
        const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        std::fclose(f);
        if (ok) ++written;
    }
    return written;
}

void fetch(std::string name) {
    const std::string want = lower(name);
    const fs::path mods = fs::u8path(lb_mods_dir());
    const fs::path cache = mods / "cache";
    std::error_code ec;
    fs::create_directories(cache, ec);
    int files = 0;
    std::string last_error;
    for (const char* z : kZips) {
        const fs::path local = cache / fs::u8path(z);
        std::string zip;
        if (FILE* f = std::fopen(local.string().c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n > 0 && n < (64L << 20)) {
                zip.resize((size_t)n);
                if (std::fread(&zip[0], 1, zip.size(), f) != zip.size()) zip.clear();
            }
            std::fclose(f);
        }
        if (zip.empty()) {
            std::string err;
            if (!lb_https_download(kHost, std::string(kBase) + url_path(z), &zip,
                                   64u << 20, &err)) {
                last_error = err;
                std::fprintf(stderr, "[lb_mapfetch] %s: %s\n", z, err.c_str());
                continue;
            }
            if (FILE* f = std::fopen(local.string().c_str(), "wb")) {
                std::fwrite(zip.data(), 1, zip.size(), f);
                std::fclose(f);
            }
            std::fprintf(stderr, "[lb_mapfetch] downloaded %s (%zu bytes)\n", z,
                         zip.size());
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
}  // namespace

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
