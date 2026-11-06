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
    // docs/466 (SG-0056): the invite. Empty join_secret: the lobby is not joinable
    // (or invites are off) and no party/secrets are sent.
    std::string party_id, join_secret;
    int party_size = 0, party_max = 0;
    bool operator==(const Activity& o) const {
        return details == o.details && state == o.state && large_key == o.large_key &&
               large_text == o.large_text && small_key == o.small_key &&
               small_text == o.small_text && party_id == o.party_id &&
               join_secret == o.join_secret && party_size == o.party_size &&
               party_max == o.party_max;
    }
};

std::mutex g_mu;
// docs/466: what Discord told the worker; the frame thread acts on it.
std::string g_join_in;                  // under g_mu: a join secret to act on, latest wins
std::atomic<bool> g_can_accept{false};  // the lobby is joinable and invites are on
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
        if (!a.join_secret.empty() && !a.party_id.empty() && a.party_max > 0) {
            j += ",\"party\":{\"id\":\"" + json_esc(a.party_id) + "\",\"size\":[" +
                 std::to_string(a.party_size) + "," + std::to_string(a.party_max) + "]}";
            j += ",\"secrets\":{\"join\":\"" + json_esc(a.join_secret) + "\"}";
            j += ",\"instance\":false";
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
    // docs/466: a frame is read only once ALL of it is waiting (the header is
    // peeked), so ReadFile never blocks on half a frame, and a header that
    // claims more than 64 KiB is a broken stream: the pipe is dropped and
    // reopened rather than guessed at.
    uint32_t hdr[2] = {0, 0};
    for (int waited = 0;; waited += 20) {
        DWORD avail = 0, peeked = 0;
        if (!PeekNamedPipe(h, hdr, 8, &peeked, &avail, nullptr)) { *broken = true; return false; }
        if (peeked >= 8) {
            if (hdr[1] > (1u << 16)) { *broken = true; return false; }
            if (avail >= 8u + hdr[1]) break;
        }
        if (waited >= wait_ms) return false;
        Sleep(20);
    }
    DWORD got = 0;
    if (!ReadFile(h, hdr, 8, &got, nullptr) || got != 8) { *broken = true; return false; }
    *op = hdr[0];
    json->assign(hdr[1], '\0');
    size_t at = 0;
    uint32_t left = hdr[1];
    while (left > 0) {
        if (!ReadFile(h, &(*json)[at], left, &got, nullptr) || got == 0) { *broken = true; return false; }
        at += got;
        left -= got;
    }
    return true;
}

// ---- docs/466 (SG-0056): invites ------------------------------------------------------
// The join secret names a lobby on a master server and nothing else:
//   "lb1|<master host>|<master port>|<room code>"
// (Discord caps a secret at 128 bytes.) It is what the lobby already shows on the
// public list, so it grants nothing; the host token never leaves lb_net.cpp.
bool token_ok(const std::string& s, size_t max_len) {
    if (s.empty() || s.size() > max_len) return false;
    for (unsigned char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_'))
            return false;
    return true;
}

bool make_secret(const char* host, int port, const char* code, std::string* out) {
    if (!host || !code || port <= 0 || port > 65535) return false;
    if (!token_ok(host, 100) || !token_ok(code, 16)) return false;
    *out = std::string("lb1|") + host + "|" + std::to_string(port) + "|" + code;
    return out->size() <= 120;
}

// Strict inverse of make_secret: anything else (a stranger's string) is refused.
bool parse_secret(const std::string& s, std::string* host, int* port, std::string* code) {
    if (s.size() < 8 || s.size() > 128 || s.compare(0, 4, "lb1|") != 0) return false;
    const size_t a = s.find('|', 4);
    if (a == std::string::npos) return false;
    const size_t b = s.find('|', a + 1);
    if (b == std::string::npos) return false;
    *host = s.substr(4, a - 4);
    const std::string p = s.substr(a + 1, b - a - 1);
    *code = s.substr(b + 1);
    if (!token_ok(*host, 100) || !token_ok(*code, 16)) return false;
    if (p.empty() || p.size() > 5) return false;
    for (char c : p) if (c < '0' || c > '9') return false;
    *port = std::atoi(p.c_str());
    return *port > 0 && *port <= 65535;
}

// The string value of the first "key":"..." in a frame; false when absent or when it
// holds an escape (no value Discord sends us here does).
bool json_value(const std::string& j, const char* key, std::string* out) {
    const std::string pat = std::string("\"") + key + "\":\"";
    const size_t at = j.find(pat);
    if (at == std::string::npos) return false;
    const size_t from = at + pat.size();
    const size_t end = j.find('"', from);
    if (end == std::string::npos) return false;
    *out = j.substr(from, end - from);
    return out->find('\\') == std::string::npos;
}

// Lets Discord start the game from an invite when it is not running: the
// discord-<application id> URL protocol, which Discord opens. HKCU only. Loaded
// dynamically so the runner needs no extra import library.
void register_launch_protocol() {
    HMODULE adv = LoadLibraryA("advapi32.dll");
    if (!adv) return;
    typedef LONG (WINAPI *CreateFn)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
                                    LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
    typedef LONG (WINAPI *SetFn)(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
    typedef LONG (WINAPI *CloseFn)(HKEY);
    CreateFn create = (CreateFn)(void*)GetProcAddress(adv, "RegCreateKeyExA");
    SetFn set = (SetFn)(void*)GetProcAddress(adv, "RegSetValueExA");
    CloseFn close = (CloseFn)(void*)GetProcAddress(adv, "RegCloseKey");
    char exe[MAX_PATH] = {0};
    const DWORD exe_len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (create && set && close && exe_len > 0 && exe_len < MAX_PATH - 1) {
        const std::string root = std::string("Software\\Classes\\discord-") + kClientId;
        const std::string cmd = std::string("\"") + exe + "\"";
        const std::string desc = std::string("URL:Run game ") + kClientId + " protocol";
        struct Val { std::string key, name, data; };
        const Val vals[] = {{root, "", desc},
                            {root, "URL Protocol", ""},
                            {root + "\\DefaultIcon", "", std::string("\"") + exe + "\",0"},
                            {root + "\\shell\\open\\command", "", cmd}};
        bool ok = true;
        for (const Val& v : vals) {
            HKEY k = nullptr;
            if (create(HKEY_CURRENT_USER, v.key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k,
                       nullptr) != ERROR_SUCCESS) { ok = false; continue; }
            if (set(k, v.name.empty() ? nullptr : v.name.c_str(), 0, REG_SZ,
                    (const BYTE*)v.data.c_str(), (DWORD)v.data.size() + 1) != ERROR_SUCCESS)
                ok = false;
            close(k);
        }
        std::fprintf(stderr, "[lb_discord] launch protocol %s -> %s\n",
                     ok ? "registered" : "FAILED to register", cmd.c_str());
        std::fflush(stderr);
    }
    FreeLibrary(adv);
}

// One frame from Discord. Events arrive as {"cmd":"DISPATCH","evt":"...","data":{...}}.
void handle_frame(HANDLE pipe, const std::string& j, unsigned* nonce) {
    if (j.find("\"evt\":\"ACTIVITY_JOIN\"") != std::string::npos) {
        std::string secret;
        if (json_value(j, "secret", &secret) && !secret.empty()) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_join_in = secret;   // the frame thread parses and validates it
        }
    } else if (j.find("\"evt\":\"ACTIVITY_JOIN_REQUEST\"") != std::string::npos) {
        // Someone clicked "Ask to join" on our status. Let them in only while the
        // lobby is joinable and invites are on; otherwise turn the request away.
        std::string uid;
        if (json_value(j, "id", &uid) && token_ok(uid, 24)) {
            const bool yes = g_can_accept.load();
            const std::string f = std::string("{\"cmd\":\"") +
                (yes ? "SEND_ACTIVITY_JOIN_INVITE" : "CLOSE_ACTIVITY_JOIN_REQUEST") +
                "\",\"args\":{\"user_id\":\"" + uid + "\"},\"nonce\":\"lb" +
                std::to_string(++*nonce) + "\"}";
            pipe_write(pipe, 1, f);
            std::fprintf(stderr, "[lb_discord] join request from %s: %s\n", uid.c_str(),
                         yes ? "invited" : "declined");
            std::fflush(stderr);
        }
    } else if (j.find("\"evt\":\"ERROR\"") != std::string::npos) {
        std::fprintf(stderr, "[lb_discord] Discord refused a command: %.200s\n", j.c_str());
        std::fflush(stderr);
    }
}

bool subscribe(HANDLE pipe, const char* evt, unsigned* nonce) {
    return pipe_write(pipe, 1, std::string("{\"cmd\":\"SUBSCRIBE\",\"evt\":\"") + evt +
                      "\",\"nonce\":\"lb" + std::to_string(++*nonce) + "\"}");
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
    bool sub_req = false;   // subscribed to ACTIVITY_JOIN_REQUEST on this connection
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
                        // docs/466: hear invites. ACTIVITY_JOIN is someone joining
                        // through our status's friend; the JOIN_REQUEST (a click on
                        // "Ask to join") only matters while we offer invites.
                        sub_req = false;
                        subscribe(pipe, "ACTIVITY_JOIN", &nonce);
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
            if (op == 1) handle_frame(pipe, reply, &nonce);
        }
        if (!broken && !sub_req && lb_setting_discord_invites() != 0) {
            sub_req = subscribe(pipe, "ACTIVITY_JOIN_REQUEST", &nonce);
            if (!sub_req) broken = true;
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
    bool can_accept = false;
    if (on) {
        // docs/466: an invite someone used in Discord. Act on it from here, the frame
        // thread; the lookup itself runs on lb_net's own thread. Only from idle: a
        // player already in a lobby or a match is never pulled out of it.
        std::string secret;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            secret = g_join_in;
        }
        if (!secret.empty()) {
            std::string host, code;
            int port = 0;
            if (!parse_secret(secret, &host, &port, &code)) {
                std::fprintf(stderr, "[lb_discord] ignoring a malformed join secret\n");
                std::lock_guard<std::mutex> lk(g_mu);
                if (g_join_in == secret) g_join_in.clear();
            } else if (lb_net_guest_state() > 0) {
                lb_overlay_notice("Discord invite ignored: you are already in a lobby.");
                std::lock_guard<std::mutex> lk(g_mu);
                if (g_join_in == secret) g_join_in.clear();
            } else if (!lb_net_busy()) {
                std::fprintf(stderr, "[lb_discord] joining %s via %s:%d from a Discord invite\n",
                             code.c_str(), host.c_str(), port);
                lb_net_join_by_code(host.c_str(), port, code.c_str());
                lb_overlay_notice("Discord invite: looking up the lobby. Then open "
                                  "Multiplayer > Join in the game.");
                std::lock_guard<std::mutex> lk(g_mu);
                if (g_join_in == secret) g_join_in.clear();
            }   // busy: the same secret is tried again at the next tick
        }

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
            // docs/466: the invite, only while the lobby is open with a free seat
            // (lb_net_host_invite is 0 once a match starts or the lobby is full).
            if (net == 2 && lb_setting_discord_invites()) {
                char srv[128] = {0}, code[24] = {0};
                int port = 0, n = 0, m = 0;
                std::string secret;
                if (lb_net_host_invite(srv, sizeof srv, &port, code, sizeof code, &n, &m) &&
                    make_secret(srv, port, code, &secret)) {
                    a.party_id = std::string("lb-") + code;
                    a.party_size = n;
                    a.party_max = m;
                    a.join_secret = secret;
                    can_accept = true;
                }
            }
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
    g_can_accept = can_accept;
    // docs/466: Discord needs the launch protocol to start the game from an invite.
    static bool registered = false;
    if (on && !registered && lb_setting_discord_invites()) {
        registered = true;
        register_launch_protocol();
    }
    if (on != last_on || !(a == last)) {
        // The elapsed-time clock restarts when what the player is doing changes, not
        // when only the party's size does.
        if (on != last_on || a.details != last.details) since = (long long)std::time(nullptr);
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
