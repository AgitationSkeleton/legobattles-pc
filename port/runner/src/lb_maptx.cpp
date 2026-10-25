// legobattles-decomp: a host sends a joiner the mod map it lacks (docs/432).
//
// A joiner whose lobby names a mod map it does not have asks the host for it
// by the map's identity, the content hash the host's rules list (docs/413: map,
// detail tiles, the four minimaps). The host answers with exactly those bytes,
// in chunks of its own datagram type (LBMX, beside chat and the host-level WH;
// it rides every route they do, the relay included). The joiner accepts the
// map only when the bytes hash to the identity it asked for, and writes it as
// an ordinary loose map folder -- fixed file names, under mods/maps/_received/
// -- which the game then loads like any other (lb_mods.cpp).
//
// Wire (the payload of an LBMX datagram):
//   'Q' 1  hash u64  offset u32               a joiner: send from this offset
//   'D' 1  hash u64  total u32  offset u32  len u16  bytes   the host: a chunk
// The bundle the chunks carry: 'LBMB' 1, six u32 lengths (map, detail, mini x4),
// then the six blobs. A whole map is 15-35 KB; anything past 1 MB is refused.

#include "lb_overlay.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>

/* lb_mods.cpp: a map's own bytes, as lb_mods hashed them; and a Reset that
   rebuilds the cartridge with what the mods folder holds now. */
bool lb_mods_bundle(unsigned long long hash, std::vector<uint8_t>* map,
                    std::vector<uint8_t>* detail, std::vector<uint8_t> mini[4]);
extern "C" const char* lb_mods_dir(void);
extern "C" int lb_mods_request_rebuild(void);
extern "C" int lb_setting_fetch_maps(void);   /* lb_overlay.cpp */

namespace {
namespace fs = std::filesystem;

constexpr uint32_t kChunk = 1024;
constexpr uint32_t kMaxBundle = 1u << 20;
constexpr uint32_t kBurst = 48;   // chunks sent per request (48 KB: most maps)

uint64_t fnv64(const uint8_t* p, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
void put32(std::vector<uint8_t>* v, uint32_t x) {
    for (int b = 0; b < 4; ++b) v->push_back((uint8_t)(x >> (8 * b)));
}
uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
void wr64(uint8_t* p, uint64_t x) {
    for (int b = 0; b < 8; ++b) p[b] = (uint8_t)(x >> (8 * b));
}
void wr32(uint8_t* p, uint32_t x) {
    for (int b = 0; b < 4; ++b) p[b] = (uint8_t)(x >> (8 * b));
}

std::mutex g_mu;
std::deque<std::vector<uint8_t>> g_out;        // datagrams to send

// The host: bundles by hash, built once.
std::map<uint64_t, std::vector<uint8_t>> g_bundles;

// The joiner: the one map it is receiving.
struct Want {
    uint64_t hash = 0;
    std::string name;
    uint32_t total = 0;
    std::vector<uint8_t> data;
    std::vector<uint8_t> have;                 // one flag per chunk
    uint64_t next_ask = 0, started = 0;
    bool asking = false;                       // the archive had none: ask the host
    bool done = false;
};
Want g_want;
std::map<uint64_t, int> g_finished;            // hash -> 1 written, -1 refused

bool build_bundle(uint64_t hash, std::vector<uint8_t>* out) {
    std::vector<uint8_t> map, detail, mini[4];
    if (!lb_mods_bundle(hash, &map, &detail, mini)) return false;
    out->clear();
    out->insert(out->end(), {'L', 'B', 'M', 'B', 1});
    put32(out, (uint32_t)map.size());
    put32(out, (uint32_t)detail.size());
    for (auto& m : mini) put32(out, (uint32_t)m.size());
    out->insert(out->end(), map.begin(), map.end());
    out->insert(out->end(), detail.begin(), detail.end());
    for (auto& m : mini) out->insert(out->end(), m.begin(), m.end());
    return out->size() <= kMaxBundle;
}

// The bundle's six blobs, checked against the identity asked for.
bool open_bundle(const std::vector<uint8_t>& b, uint64_t hash,
                 std::vector<std::vector<uint8_t>>* parts) {
    if (b.size() < 5 + 24 || std::memcmp(b.data(), "LBMB\x01", 5) != 0) return false;
    size_t at = 5 + 24;
    uint64_t h = 1469598103934665603ull;
    parts->clear();
    for (int i = 0; i < 6; ++i) {
        const uint32_t n = rd32(b.data() + 5 + 4 * i);
        if (n > kMaxBundle || at + n > b.size()) return false;
        parts->emplace_back(b.begin() + (long)at, b.begin() + (long)(at + n));
        h = fnv64(b.data() + at, n, h);
        at += n;
    }
    return at == b.size() && h == hash && !(*parts)[0].empty();
}

// A folder name from the host's map name: letters, digits and a few marks.
std::string safe_name(const std::string& n) {
    std::string o;
    for (char c : n) {
        const unsigned char u = (unsigned char)c;
        if (std::isalnum(u) || c == ' ' || c == '_' || c == '-' || c == '\'' || c == '(' ||
            c == ')' || c == '.')
            o += c;
    }
    while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
    while (!o.empty() && (o[0] == '.' || o[0] == ' ')) o.erase(0, 1);
    if (o.size() > 31) o.resize(31);
    return o.empty() ? std::string("received map") : o;
}

bool write_file(const fs::path& p, const std::vector<uint8_t>& d) {
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(d.data(), 1, d.size(), f) == d.size();
    std::fclose(f);
    return ok;
}

// The received map as a loose folder: mods/maps/_received/<name>/map.map,
// mapmini.NCGR ... DetailTiles_map.tbp (lb_mods.cpp's loose-folder names).
int write_folder(const std::string& name, const std::vector<std::vector<uint8_t>>& parts,
                 std::string* where) {
    std::error_code ec;
    const fs::path base = fs::u8path(lb_mods_dir()) / "maps" / "_received";
    fs::path dir = base / fs::u8path(safe_name(name));
    for (int n = 2; fs::exists(dir, ec) && n < 20; ++n)
        dir = base / fs::u8path(safe_name(name) + " (" + std::to_string(n) + ")");
    fs::create_directories(dir, ec);
    if (ec) return 0;
    static const char* const kMini[4] = {"mini.NCGR", "mini.NCBR", "miniNT.NCGR",
                                         "miniNT.NCBR"};
    bool ok = write_file(dir / "map.map", parts[0]);
    if (!parts[1].empty()) ok = ok && write_file(dir / "DetailTiles_map.tbp", parts[1]);
    for (int k = 0; k < 4; ++k) ok = ok && write_file(dir / (std::string("map") + kMini[k]), parts[2 + k]);
    if (!ok) {
        fs::remove_all(dir, ec);
        return 0;
    }
    *where = fs::path(dir).lexically_relative(fs::u8path(lb_mods_dir())).generic_string();
    return 1;
}

void queue_request(uint64_t hash, uint32_t offset) {
    std::vector<uint8_t> d(14);
    d[0] = 'Q';
    d[1] = 1;
    wr64(&d[2], hash);
    wr32(&d[10], offset);
    g_out.push_back(std::move(d));
}

// The host: up to kBurst chunks of `hash` from `offset`.
void queue_chunks(uint64_t hash, uint32_t offset) {
    auto it = g_bundles.find(hash);
    if (it == g_bundles.end()) {
        std::vector<uint8_t> b;
        if (!build_bundle(hash, &b)) return;   // not ours: someone else's lobby
        it = g_bundles.emplace(hash, std::move(b)).first;
        std::fprintf(stderr, "[lb_maptx] a joiner asked for map %016llX: sending %zu bytes\n",
                     (unsigned long long)hash, it->second.size());
        std::fflush(stderr);
    }
    const std::vector<uint8_t>& b = it->second;
    offset -= offset % kChunk;
    for (uint32_t k = 0; k < kBurst && offset < b.size(); ++k, offset += kChunk) {
        const uint32_t len = std::min<uint32_t>(kChunk, (uint32_t)b.size() - offset);
        std::vector<uint8_t> d(20 + len);
        d[0] = 'D';
        d[1] = 1;
        wr64(&d[2], hash);
        wr32(&d[10], (uint32_t)b.size());
        wr32(&d[14], offset);
        d[18] = (uint8_t)(len & 0xFF);
        d[19] = (uint8_t)(len >> 8);
        std::memcpy(&d[20], b.data() + offset, len);
        g_out.push_back(std::move(d));
    }
}

// Make the game use a map folder that just arrived: 1 when it already is in
// the game, 2 when the game resets to load it (mod maps are built into the
// cartridge, docs/391; a Reset rebuilds it, lb_mods_request_rebuild), 0 when
// neither (the player must restart).
int register_received(const std::string& path) {
    if (lb_mods_register_folder(path.c_str())) return 1;
    return lb_mods_request_rebuild() ? 2 : 0;
}

std::string loaded_text(const std::string& name, const char* from, int how) {
    const std::string head = "The map \"" + name + "\" " + from + ". ";
    if (how == 1) return head + "You can ready up.";
    if (how == 2) return head + "The game resets to load it: join the lobby again.";
    return head + "Restart the game to use it, then join again.";
}

void finish_locked() {
    Want& w = g_want;
    std::vector<std::vector<uint8_t>> parts;
    std::string where;
    const bool good = open_bundle(w.data, w.hash, &parts);
    const int wrote = good ? write_folder(w.name, parts, &where) : 0;
    g_finished[w.hash] = wrote ? 1 : -1;
    w.done = true;
    std::fprintf(stderr, "[lb_maptx] map %016llX \"%s\": %zu bytes received, %s%s\n",
                 (unsigned long long)w.hash, w.name.c_str(), w.data.size(),
                 !good ? "REFUSED (not the map the host's rules name)"
                       : wrote ? "written to " : "could not be written",
                 where.c_str());
    std::fflush(stderr);
    std::string text;
    if (wrote) {
        const std::string full =
            (fs::u8path(lb_mods_dir()) / fs::u8path(where)).string();
        text = loaded_text(w.name, "came from the host", register_received(full));
    } else {
        text = "The host's map \"" + w.name + "\" could not be received.";
    }
    lb_overlay_notice(text.c_str());
}

// ARCHIVE FIRST (the owner's rule): the community archive's copy with this
// exact identity (lb_mapfetch_install_by_hash, which hashes every candidate),
// on a worker thread; the host is asked only when the archive has none.
void archive_then_host(uint64_t hash, std::string name) {
    char path[600] = "", err[200] = "";
    int got = 0;
    try {
        got = lb_mapfetch_install_by_hash(hash, path, (int)sizeof path, err, (int)sizeof err);
    } catch (const std::exception& e) {
        std::snprintf(err, sizeof err, "%s", e.what());
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_want.hash != hash || g_want.done) return;   // left the lobby meanwhile
    if (got) {
        g_finished[hash] = 1;
        g_want.done = true;
        std::fprintf(stderr, "[lb_maptx] map %016llX \"%s\": the archive has it (%s)\n",
                     (unsigned long long)hash, name.c_str(), path);
        std::fflush(stderr);
        lb_overlay_notice(loaded_text(name, "came from the map archive",
                                      register_received(path)).c_str());
        return;
    }
    std::fprintf(stderr, "[lb_maptx] map %016llX \"%s\": not in the archive%s%s; asking "
                 "the host\n", (unsigned long long)hash, name.c_str(), err[0] ? ": " : "", err);
    std::fflush(stderr);
    g_want.asking = true;
    g_want.started = SDL_GetTicks64();
}

}  // namespace

// A lobby wants this map, which this machine lacks: ask the host for it. Once
// per map per run; a later call for the same map is ignored.
extern "C" void lb_maptx_want(unsigned long long hash, const char* name) {
    if (!hash || !lb_setting_fetch_maps()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_finished.count(hash) || (g_want.hash == hash && !g_want.done)) return;
    g_want = Want();
    g_want.hash = hash;
    g_want.name = name ? name : "";
    // LB_MAPTX_NO_ARCHIVE=1: straight to the host (a test aid).
    const bool skip = std::getenv("LB_MAPTX_NO_ARCHIVE") != nullptr;
    std::fprintf(stderr, "[lb_maptx] looking for map %016llX \"%s\": %s\n",
                 (unsigned long long)hash, g_want.name.c_str(),
                 skip ? "asking the host (archive skipped)" : "the archive first");
    std::fflush(stderr);
    if (skip) {
        g_want.asking = true;
        g_want.started = SDL_GetTicks64();
        return;
    }
    std::thread([hash, n = g_want.name]() { archive_then_host(hash, n); }).detach();
}

// The lobby was left: stop asking.
extern "C" void lb_maptx_forget(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_want.done) g_want = Want();
}

// The bridge's drain: the next datagram to send, or 0.
extern "C" int lb_maptx_outgoing(unsigned char* buf, int cap) {
    std::lock_guard<std::mutex> lk(g_mu);
    Want& w = g_want;
    const uint64_t now = SDL_GetTicks64();
    if (w.hash && w.asking && !w.done && now >= w.next_ask) {
        if (now - w.started > 120000u) {   // two minutes: the host has not answered
            std::fprintf(stderr, "[lb_maptx] map %016llX: no answer from the host\n",
                         (unsigned long long)w.hash);
            std::fflush(stderr);
            g_finished[w.hash] = -1;
            w.done = true;
        } else {
            uint32_t first = 0;   // the first chunk still missing
            while (first < w.have.size() && w.have[first]) ++first;
            queue_request(w.hash, first * kChunk);
            w.next_ask = now + 700u;
        }
    }
    if (g_out.empty()) return 0;
    const std::vector<uint8_t> d = std::move(g_out.front());
    g_out.pop_front();
    if ((int)d.size() > cap) return 0;
    std::memcpy(buf, d.data(), d.size());
    return (int)d.size();
}

extern "C" void lb_maptx_incoming(const unsigned char* p, int len, unsigned int sender) {
    (void)sender;
    if (len < 14 || p[1] != 1) return;
    const uint64_t hash = rd64(p + 2);
    std::lock_guard<std::mutex> lk(g_mu);
    if (p[0] == 'Q') {
        const int s = lb_net_guest_state();
        if (s == 2 || s == 10)               // only a host with its lobby open answers
            queue_chunks(hash, rd32(p + 10));
        return;
    }
    if (p[0] != 'D' || len < 20) return;
    Want& w = g_want;
    if (hash != w.hash || !w.asking || w.done) return;   // a chunk for someone else
    const uint32_t total = rd32(p + 10), offset = rd32(p + 14);
    const uint32_t n = (uint32_t)p[18] | ((uint32_t)p[19] << 8);
    if (total == 0 || total > kMaxBundle || n > kChunk || (int)(20 + n) != len ||
        offset % kChunk != 0 || offset + n > total)
        return;
    if (w.total == 0) {
        w.total = total;
        w.data.assign(total, 0);
        w.have.assign((total + kChunk - 1) / kChunk, 0);
    }
    if (total != w.total) return;
    const uint32_t k = offset / kChunk;
    if (w.have[k]) return;
    if (n != std::min<uint32_t>(kChunk, total - offset)) return;
    std::memcpy(&w.data[offset], p + 20, n);
    w.have[k] = 1;
    if (std::all_of(w.have.begin(), w.have.end(), [](uint8_t f) { return f != 0; }))
        finish_locked();
}
