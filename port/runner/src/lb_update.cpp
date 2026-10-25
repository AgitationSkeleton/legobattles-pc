// legobattles-decomp: download and install a newer release in place (docs/435).
//
// Settings > General's update check finds the newest release on GitHub
// (lb_overlay.cpp). This downloads its game zip -- LegoBattles-PC-<v>.zip, one
// top folder of shipped files (tools/lb_dist.py) -- and copies those files
// over the install, the folder the running LegoBattles.exe is in:
//   * the player's own files are never written: settings.json, imgui.ini and
//     anything under saves/, mods/, rom/ or logs/ (the zip holds none of them
//     either; this is the second lock);
//   * a zip entry must sit inside the zip's single top folder, with no "..",
//     drive letter, or absolute path;
//   * a file Windows keeps open -- the running exe, its loaded DLLs -- cannot be
//     overwritten but can be renamed: every replaced file is first renamed to
//     <name>.old, and the .old files are deleted at the next start
//     (lb_update_cleanup);
//   * only an installed release updates itself: a development build (any exe
//     name but LegoBattles.exe, or no version.txt beside it) refuses.
// Then "Restart now" starts the new exe with this one's command line and quits.

#include "lb_overlay.h"

#include <SDL.h>
#include <zlib.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

bool lb_https_download(const std::string& host, const std::string& path,
                       std::string* data, size_t max, std::string* error);

namespace {
namespace fs = std::filesystem;

std::mutex g_mu;
std::atomic<int> g_state{0};   // 0 idle, 1 downloading, 2 installing, 3 installed, 4 failed
std::string g_msg;
std::atomic<bool> g_busy{false};

void set(int state, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_state = state;
    g_msg = msg;
    std::fprintf(stderr, "[lb_update] %s\n", msg.c_str());
    std::fflush(stderr);
}

fs::path install_dir() {
    char* base = SDL_GetBasePath();
    const fs::path d = base ? fs::u8path(base) : fs::path(".");
    if (base) SDL_free(base);
    return d;
}

uint32_t le16(const std::string& d, size_t at) {
    return (uint32_t)(uint8_t)d[at] | ((uint32_t)(uint8_t)d[at + 1] << 8);
}
uint32_t le32(const std::string& d, size_t at) { return le16(d, at) | (le16(d, at + 2) << 16); }

bool inflate_entry(const std::string& zip, size_t local, uint32_t method, uint32_t csize,
                   uint32_t usize, std::string* out) {
    if (local + 30 > zip.size() || le32(zip, local) != 0x04034B50u) return false;
    const size_t data = local + 30 + le16(zip, local + 26) + le16(zip, local + 28);
    if (data + csize > zip.size() || usize > (400u << 20)) return false;
    if (method == 0) {
        out->assign(zip, data, csize);
        return csize == usize;
    }
    if (method != 8) return false;
    out->assign(usize, '\0');
    if (usize == 0) return true;
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

struct Entry { std::string rel; uint32_t method, csize, usize, local; };

// The zip's files below its one top folder, or false if it is not a release.
bool list_release(const std::string& zip, std::vector<Entry>* out, std::string* why) {
    size_t eocd = std::string::npos;
    for (size_t i = zip.size() >= 22 ? zip.size() - 22 : 0; zip.size() >= 22; --i) {
        if (le32(zip, i) == 0x06054B50u) { eocd = i; break; }
        if (i == 0 || zip.size() - i > 66000) break;
    }
    if (eocd == std::string::npos) { *why = "not a zip"; return false; }
    const uint32_t count = le16(zip, eocd + 10);
    size_t at = le32(zip, eocd + 16);
    std::string top;
    bool have_exe = false, have_version = false;
    for (uint32_t k = 0; k < count && at + 46 <= zip.size(); ++k) {
        if (le32(zip, at) != 0x02014B50u) { *why = "a damaged zip"; return false; }
        Entry e;
        e.method = le16(zip, at + 10);
        e.csize = le32(zip, at + 20);
        e.usize = le32(zip, at + 24);
        const uint32_t nlen = le16(zip, at + 28), xlen = le16(zip, at + 30),
                       clen = le16(zip, at + 32);
        e.local = le32(zip, at + 42);
        std::string name = zip.substr(at + 46, nlen);
        at += 46 + nlen + xlen + clen;
        for (char& c : name) if (c == '\\') c = '/';
        const size_t slash = name.find('/');
        if (slash == std::string::npos || slash == 0) { *why = "a file outside the release folder"; return false; }
        const std::string t = name.substr(0, slash);
        if (top.empty()) top = t;
        if (t != top || t.compare(0, 15, "LegoBattles-PC-") != 0) {
            *why = "not a LegoBattles-PC release";
            return false;
        }
        e.rel = name.substr(slash + 1);
        if (e.rel.empty() || e.rel.back() == '/') continue;   // a folder
        if (e.rel.find("..") != std::string::npos || e.rel.find(':') != std::string::npos ||
            e.rel[0] == '/') {
            *why = "an unsafe path in the zip";
            return false;
        }
        if (e.rel == "LegoBattles.exe") have_exe = true;
        if (e.rel == "version.txt") have_version = true;
        out->push_back(e);
    }
    if (!have_exe || !have_version) { *why = "the zip has no LegoBattles.exe"; return false; }
    return true;
}

// The player's files: never written by an update.
bool players_own(const std::string& rel) {
    std::string r = rel;
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    for (const char* d : {"saves/", "mods/", "rom/", "logs/"})
        if (r.compare(0, std::strlen(d), d) == 0) return true;
    return r == "settings.json" || r == "imgui.ini";
}

bool write_file(const fs::path& p, const std::string& data) {
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

void run(std::string url, std::string version) {
    set(1, "Downloading " + version + "...");
    // https://github.com/<owner>/<repo>/releases/download/<tag>/<file>.zip
    const std::string pre = "https://";
    std::string host, path;
    if (url.compare(0, pre.size(), pre) == 0) {
        const size_t s = url.find('/', pre.size());
        if (s != std::string::npos) {
            host = url.substr(pre.size(), s - pre.size());
            path = url.substr(s);
        }
    }
    if (host != "github.com" || path.find("/AgitationSkeleton/legobattles-pc/releases/download/") != 0) {
        set(4, "That download is not one of this game's releases.");
        g_busy = false;
        return;
    }
    std::string zip, err;
    if (!lb_https_download(host, path, &zip, 400u << 20, &err)) {
        set(4, "The download failed: " + err);
        g_busy = false;
        return;
    }
    set(2, "Installing " + version + "...");
    std::vector<Entry> files;
    std::string why;
    if (!list_release(zip, &files, &why)) {
        set(4, "The download is " + why + "; nothing was changed.");
        g_busy = false;
        return;
    }
    const fs::path dir = install_dir();
    // Unpack everything first: a broken file stops the update before anything
    // in the install is touched.
    std::vector<std::pair<std::string, std::string>> data;
    for (const Entry& e : files) {
        if (players_own(e.rel)) continue;
        std::string bytes;
        if (!inflate_entry(zip, e.local, e.method, e.csize, e.usize, &bytes)) {
            set(4, "The download is damaged (" + e.rel + "); nothing was changed.");
            g_busy = false;
            return;
        }
        data.push_back({e.rel, std::move(bytes)});
    }
    zip.clear();
    zip.shrink_to_fit();
    std::error_code ec;
    int written = 0;
    for (const auto& f : data) {
        const fs::path target = dir / fs::u8path(f.first);
        fs::create_directories(target.parent_path(), ec);
        if (fs::exists(target, ec)) {
            const fs::path old = target.string() + ".old";
            fs::remove(old, ec);
            fs::rename(target, old, ec);
            if (ec) {
                set(4, "Could not replace " + f.first + " (" + ec.message() +
                           "). The update stopped part way: download it from the release "
                           "page instead.");
                g_busy = false;
                return;
            }
        }
        if (!write_file(target, f.second)) {
            set(4, "Could not write " + f.first + ". The update stopped part way: download "
                   "it from the release page instead.");
            g_busy = false;
            return;
        }
        ++written;
    }
    set(3, version + " is installed (" + std::to_string(written) +
               " files). Restart the game to use it.");
    g_busy = false;
}

}  // namespace

// 1 when this is an installed release that can update itself.
extern "C" int lb_update_supported(void) {
#ifdef _WIN32
    char exe[MAX_PATH] = "";
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return 0;
    const fs::path p(exe);
    std::error_code ec;
    return p.filename() == "LegoBattles.exe" &&
                   fs::exists(p.parent_path() / "version.txt", ec)
               ? 1
               : 0;
#else
    return 0;
#endif
}

// Start downloading and installing `url` (a release's game zip). 0 when one
// is already running or this build cannot update itself.
extern "C" int lb_update_start(const char* url, const char* version) {
    if (!url || !*url || !lb_update_supported()) return 0;
    if (g_busy.exchange(true)) return 0;
    g_state = 1;
    std::thread([u = std::string(url), v = std::string(version ? version : "")]() {
        try {
            run(u, v);
        } catch (const std::exception& e) {
            set(4, std::string("The update failed: ") + e.what());
            g_busy = false;
        }
    }).detach();
    return 1;
}

// 0 idle, 1 downloading, 2 installing, 3 installed (restart to use), 4 failed.
extern "C" int lb_update_state(char* msg, int cap) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (msg && cap > 0) std::snprintf(msg, (size_t)cap, "%s", g_msg.c_str());
    return g_state;
}

// Start the installed exe with this one's command line, then quit this one.
extern "C" void lb_update_restart(void) {
#ifdef _WIN32
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::string cmd = GetCommandLineA();
    const std::string dir = install_dir().string();
    if (CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(),
                       &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        SDL_Event q{};
        q.type = SDL_QUIT;
        SDL_PushEvent(&q);
    } else {
        set(4, "Could not start the new version: start the game again by hand.");
    }
#endif
}

// At start: remove the .old files a previous update left (they were in use then).
extern "C" void lb_update_cleanup(void) {
    if (!lb_update_supported()) return;
    std::error_code ec;
    int n = 0;
    const fs::path dir = install_dir();
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        const std::string rel = fs::relative(p, dir, ec).generic_string();
        if (players_own(rel)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && p.extension() == ".old") {
            std::error_code e2;
            if (fs::remove(p, e2)) ++n;
        }
    }
    if (n) {
        std::fprintf(stderr, "[lb_update] removed %d file(s) left by the last update\n", n);
        std::fflush(stderr);
    }
}
