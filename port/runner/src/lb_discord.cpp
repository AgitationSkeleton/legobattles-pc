// docs/430, SG-0020: DISCORD RICH PRESENCE.
//
// Discord's desktop app listens on a local named pipe (\\.\pipe\discord-ipc-0
// .. 9). A client opens it and speaks frames of [u32 op][u32 length][JSON]:
//   op 0  handshake  {"v":1,"client_id":"<application id>"}
//   op 1  frame      {"cmd":"SET_ACTIVITY","args":{"pid":N,"activity":{...}},
//                     "nonce":"..."}
// The application's art assets are keyed by name (the keys in
// exports/discord-rpc/manifest.json: map_mpNN_<name>, hero_<name>).
//
// The frame thread (lb_discord_tick, once per overlay frame) works out what the
// player is doing from state the port already keeps -- the network state, the
// loaded map, the hero -- about once a second, and hands the result to a thread
// of its own, which owns the pipe: it connects when Discord is running,
// reconnects quietly when it is not or goes away, and sends an activity only when
// it changed. Nothing here touches the simulation. If the Discord app is not
// running, nothing happens at all.
//
// Test aids: every activity sent is logged ("[lb_discord] sent: ..."); with
// LB_DISCORD_LOG=1 the JSON is also logged when no Discord is there to take it.

#include "lb_overlay.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ctime>
#include <string>
#include <thread>

extern "C" {
#include "runtime_arm.h"   // bus_read_u32: the team pool, to tell a level from the menus
}
extern "C" int lb_net_local_hero(void);   // lb_patches.cpp
extern "C" int lb_mods_stem_index(const char* stem);   // lb_mods.cpp, docs/439

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

constexpr const char* kClientId = "1563424865980252240";

// ---- what the player is doing --------------------------------------------------
struct Activity {
    std::string details, state, large_key, large_text, small_key, small_text;
    long long start = 0;   // unix seconds; 0: none
    bool operator==(const Activity& o) const {
        return details == o.details && state == o.state && large_key == o.large_key &&
               large_text == o.large_text && small_key == o.small_key &&
               small_text == o.small_text;
    }
};

std::mutex g_mu;
Activity g_want;            // what the frame thread asked for
bool g_want_on = false;     // false: clear the status
unsigned g_want_gen = 0;
std::atomic<bool> g_thread_started{false};

// Art keys uploaded to the Discord application (exports/discord-rpc/manifest.json).
const char* const kMapKey[30] = {
    "map_mp01_the_pond", "map_mp02_entrenched", "map_mp03_avast", "map_mp04_river_crossing",
    "map_mp05_exile", "map_mp06_alpha_sector", "map_mp07_enclave", "map_mp08_the_black_seas",
    "map_mp09_omega_quadrant", "map_mp10_king_s_landing", "map_mp11_the_bayou",
    "map_mp12_dead_end", "map_mp13_the_groves", "map_mp14_le_poisson", "map_mp15_the_divide",
    "map_mp16_the_north_lands", "map_mp17_the_junction", "map_mp18_outbreak",
    "map_mp19_crossover", "map_mp20_tri_star", "map_mp21_dryside", "map_mp22_dark_waters",
    "map_mp23_lost_lagoon", "map_mp24_pulsar", "map_mp25_devil_s_horn",
    "map_mp26_kahuka_s_strait", "map_mp27_crossed_island", "map_mp28_proving_grounds",
    "map_mp29_the_dead_marsh", "map_mp30_ruthless"};

struct HeroKey { int bp; const char* key; };
const HeroKey kHeroKey[] = {
    {0, "hero_king"}, {1, "hero_queen"}, {20, "hero_wizard"}, {21, "hero_sorceress"},
    {40, "hero_captain_brickbeard"}, {41, "hero_lady_pirate"},
    {60, "hero_governor_broadside"}, {61, "hero_camilla"}, {80, "hero_biff"},
    {81, "hero_gemma"}, {100, "hero_alien_commander"}, {101, "hero_alien_queen"},
    {129, "hero_space_police_captain"}, {133, "hero_space_criminal_leader"},
    {145, "hero_king_kahuka"}, {149, "hero_ninja_master"}, {161, "hero_dwarf_king"},
    {165, "hero_troll_king"}, {175, "hero_forestman"}, {176, "hero_ghost"},
    {177, "hero_sheriff"}, {178, "hero_conquistador"}, {179, "hero_agent_chase"},
    {180, "hero_classic_space"}, {181, "hero_santa"},
    // docs/436: the neutral creatures, when one is the army's hero.
    {138, "hero_robot"}, {155, "hero_shark"}, {169, "hero_wolf"}, {153, "hero_monkey"}};

std::string json_esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char e[8]; std::snprintf(e, sizeof e, "\\u%04x", c); o += e; }
        else o += (char)c;
    }
    return o;
}

// The activity as the SET_ACTIVITY frame's JSON.
std::string activity_json(bool on, const Activity& a, unsigned nonce) {
    char head[96];
    std::snprintf(head, sizeof head, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu",
                  (unsigned long)GetCurrentProcessId());
    std::string j = head;
    if (on) {
        j += ",\"activity\":{\"details\":\"" + json_esc(a.details) + "\"";
        if (!a.state.empty()) j += ",\"state\":\"" + json_esc(a.state) + "\"";
        if (a.start > 0) j += ",\"timestamps\":{\"start\":" + std::to_string(a.start) + "}";
        if (!a.large_key.empty() || !a.small_key.empty()) {
            j += ",\"assets\":{";
            bool first = true;
            if (!a.large_key.empty()) {
                j += "\"large_image\":\"" + json_esc(a.large_key) + "\"";
                if (!a.large_text.empty()) j += ",\"large_text\":\"" + json_esc(a.large_text) + "\"";
                first = false;
            }
            if (!a.small_key.empty()) {
                if (!first) j += ",";
                j += "\"small_image\":\"" + json_esc(a.small_key) + "\"";
                if (!a.small_text.empty()) j += ",\"small_text\":\"" + json_esc(a.small_text) + "\"";
            }
            j += "}";
        }
        j += "}";
    }
    j += "},\"nonce\":\"lb" + std::to_string(nonce) + "\"}";
    return j;
}

// ---- the pipe --------------------------------------------------------------------
bool pipe_write(HANDLE h, uint32_t op, const std::string& json) {
    std::string frame(8, '\0');
    const uint32_t len = (uint32_t)json.size();
    std::memcpy(&frame[0], &op, 4);
    std::memcpy(&frame[4], &len, 4);
    frame += json;
    DWORD put = 0;
    return WriteFile(h, frame.data(), (DWORD)frame.size(), &put, nullptr) &&
           put == frame.size();
}

// Reads one frame if one is waiting (waits up to `wait_ms`); false: none, or the
// pipe is broken (`broken` is set).
bool pipe_read(HANDLE h, uint32_t* op, std::string* json, int wait_ms, bool* broken) {
    for (int waited = 0;; waited += 20) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) { *broken = true; return false; }
        if (avail >= 8) break;
        if (waited >= wait_ms) return false;
        Sleep(20);
    }
    uint32_t hdr[2];
    DWORD got = 0;
    if (!ReadFile(h, hdr, 8, &got, nullptr) || got != 8) { *broken = true; return false; }
    *op = hdr[0];
    json->assign(hdr[1] > (1u << 16) ? 0 : hdr[1], '\0');
    size_t at = 0;
    uint32_t left = hdr[1];
    char buf[512];
    while (left > 0) {
        const DWORD want = left < sizeof buf ? left : (DWORD)sizeof buf;
        if (!ReadFile(h, buf, want, &got, nullptr) || got == 0) { *broken = true; return false; }
        if (at + got <= json->size()) { std::memcpy(&(*json)[at], buf, got); at += got; }
        left -= got;
    }
    return true;
}

HANDLE open_discord() {
    for (int i = 0; i < 10; ++i) {
        char name[48];
        std::snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return INVALID_HANDLE_VALUE;
}

void worker() {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    unsigned sent_gen = 0;
    bool sent_on = false;
    Activity sent;
    unsigned nonce = 0;
    bool said_down = false;
    const bool log_all = std::getenv("LB_DISCORD_LOG") != nullptr;
    for (;;) {
        Activity want;
        bool want_on;
        unsigned gen;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            want = g_want;
            want_on = g_want_on;
            gen = g_want_gen;
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            if (log_all && gen != sent_gen && want_on) {
                std::fprintf(stderr, "[lb_discord] (no Discord) would send: %s\n",
                             activity_json(true, want, nonce + 1).c_str());
                std::fflush(stderr);
                sent_gen = gen;
            }
            // Only look for Discord while there is something to show.
            if (want_on) {
                pipe = open_discord();
                if (pipe != INVALID_HANDLE_VALUE) {
                    bool broken = false;
                    uint32_t op = 0;
                    std::string reply;
                    const std::string hs = std::string("{\"v\":1,\"client_id\":\"") + kClientId + "\"}";
                    if (pipe_write(pipe, 0, hs) && pipe_read(pipe, &op, &reply, 3000, &broken) &&
                        reply.find("READY") != std::string::npos) {
                        std::fprintf(stderr, "[lb_discord] connected to Discord\n");
                        std::fflush(stderr);
                        said_down = false;
                        sent_gen = 0;
                        sent_on = false;
                    } else {
                        CloseHandle(pipe);
                        pipe = INVALID_HANDLE_VALUE;
                    }
                }
            }
            if (pipe == INVALID_HANDLE_VALUE) {
                if (!said_down && want_on) {
                    said_down = true;
                    std::fprintf(stderr, "[lb_discord] Discord is not running; will keep "
                                 "looking quietly\n");
                    std::fflush(stderr);
                }
                for (int i = 0; i < 50; ++i) { Sleep(100); }   // 5 s between looks
                continue;
            }
        }
        // Connected: send a change; drain what Discord says; notice a loss.
        bool broken = false;
        uint32_t op = 0;
        std::string reply;
        while (pipe_read(pipe, &op, &reply, 0, &broken)) {
            if (op == 2) { broken = true; break; }   // CLOSE
            if (op == 3) pipe_write(pipe, 4, reply); // PING -> PONG
        }
        if (!broken && (gen != sent_gen || want_on != sent_on)) {
            const std::string j = activity_json(want_on, want, ++nonce);
            if (pipe_write(pipe, 1, j)) {
                std::fprintf(stderr, "[lb_discord] sent: %s\n", j.c_str());
                std::fflush(stderr);
                sent_gen = gen;
                sent_on = want_on;
                sent = want;
            } else broken = true;
        }
        if (broken) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
            sent_gen = 0;
            std::fprintf(stderr, "[lb_discord] the Discord pipe closed\n");
            std::fflush(stderr);
            Sleep(2000);
            continue;
        }
        Sleep(250);
    }
}

// ---- the frame thread's side ---------------------------------------------------------
bool in_level() {
    // The team pool (the match's teams) exists while a level is loaded.
    const uint32_t pool = bus_read_u32(0x0215540Cu);
    if (pool < 0x02000000u || pool >= 0x02400000u) return false;
    const uint32_t cnt = bus_read_u32(pool + 0xCu);
    return cnt >= 1u && cnt <= 8u;
}

int map_number(const char* stem) {   // "mp07" -> 7, else 0
    if (stem[0] == 'm' && stem[1] == 'p' && stem[2] >= '0' && stem[2] <= '9' &&
        stem[3] >= '0' && stem[3] <= '9' && !stem[4])
        return (stem[2] - '0') * 10 + (stem[3] - '0');
    return 0;
}

}  // namespace

extern "C" void lb_discord_tick(void) {
    static unsigned frame = 0;
    static Activity last;
    static bool last_on = false;
    static long long since = 0;
    if (++frame % 30u != 0u) return;   // about twice a second
    const bool on = lb_setting_discord_status() != 0 &&
                    !(std::getenv("LB_DISCORD") && std::getenv("LB_DISCORD")[0] == '0');
    Activity a;
    if (on) {
        const int net = lb_net_guest_state();
        const char* stem = lb_level_map_stem();
        const char* map = lb_level_map_name();
        const bool level = in_level() && stem[0];
        const int mp = map_number(stem);
        const bool free_play = mp > 0 || lb_mods_stem_index(stem) >= 0;   // docs/439
        const bool hosting = net == 2 || net == 3 || net == 4 || net == 10;
        const bool joined = net == 5;
        const bool match = net == 11 || net == 12;
        if (match && level) {
            a.details = std::string("Online match: ") + (map[0] ? map : "a map");
        } else if (hosting || (net == 11)) {
            a.details = "Hosting a lobby";
        } else if (joined || net == 12) {
            a.details = "In a lobby";
        } else if (level && free_play) {
            a.details = std::string("Free Play: ") + (map[0] ? map : stem);
        } else if (level) {
            a.details = std::string("Singleplayer: ") + stem;
        } else {
            a.details = "In the menus";
        }
        if (level && mp > 0 && mp <= 30) {
            a.large_key = kMapKey[mp - 1];
            a.large_text = map;
        }
        if (level && lb_setting_discord_hero()) {
            const int bp = lb_net_local_hero();
            for (const HeroKey& h : kHeroKey)
                if (h.bp == bp) {
                    a.small_key = h.key;
                    const char* nm = lb_teams_unit_name_stock(bp);
                    if (nm && std::strcmp(nm, "?") != 0) a.small_text = nm;
                    break;
                }
        }
    }
    if (on != last_on || !(a == last)) {
        since = (long long)std::time(nullptr);
        last = a;
        last_on = on;
        a.start = since;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_want = a;
            g_want_on = on;
            ++g_want_gen;
        }
    }
    if (on && !g_thread_started.exchange(true)) std::thread(worker).detach();
}

#else
extern "C" void lb_discord_tick(void) {}
#endif
