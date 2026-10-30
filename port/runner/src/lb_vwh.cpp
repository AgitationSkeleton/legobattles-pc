// legobattles-decomp: the host-level WH (docs/422, docs/423).
//
// WHY. The game's multiplayer is NitroSDK's wh.c data sharing (context
// 0x02158AE0): once a step FUN_020F5888 hands WH_StepDS (0x020F43F4) this
// console's 0x8C-byte block and, on success, copies every station's block from
// WH_GetSharedDataAdr (0x020F43D4) for the net task (FUN_020F4B00), which runs
// one simulation turn per dataset. Over the emulated radio each dataset is a
// parent -> child -> parent exchange, so every step costs a network round
// trip: 60 ms of internet is 16 steps a second.
//
// WHAT. The wireless helper's functions are answered here instead, with no
// radio, and the data sharing is a lockstep pipeline of depth L over the
// bridge's UDP socket (the one NAT traversal opened):
//   * a console takes the game's block only on a step that consumes a
//     dataset, and gives it to slot cursor + L. A step that returns 0 leaves
//     the buffer with the game, which does not rebuild it while stalled
//     (FUN_020F59B8 returns at once while the stall flag is up), so every
//     block the game builds goes to exactly one slot;
//   * the HOST (L = 0) commits dataset r once every member sharing at r has
//     its block for r, consumes it, and sends it on;
//   * a CLIENT consumes dataset r once the host's commit of it is in. Its
//     first L slots are empty blocks (type 0, which the lobby handler
//     FUN_020F5168 skips), and so is any slot that raises L later.
// Every console consumes the same datasets in the same order, so every
// simulation runs the same turns; L steps of slack hide the round trip.
//
// DELIVERY. Each side resends what the other has not acknowledged: a new
// item twice, then again once a round trip has passed without its ack. Blocks
// are trimmed of trailing zeros, and a datagram stays under 1100 bytes so the
// internet never fragments it.
//
// THE JOIN. A host sends a beacon every 200 ms; a scanning console writes it
// into the game's own host table (0x0215BDC0, 20 x 0xC4: a WMBssDesc, +0xC0
// new, +0xC1 time to live) exactly as the scan callback FUN_020F616C does, and
// the Join list reads it from there. Child connect sends CONNECT; the host
// answers ACCEPT with an AID and the first dataset the joiner shares, and the
// joiner goes straight to DATASHARING.
//
// LB_VWH=0 restores the radio: every override is then a pure tail call.

#include "lb_overlay.h"

#include <SDL.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <vector>

extern "C" {
#include "runtime_arm.h"
}

extern "C" bool lb_vwh_override_return(void);      /* lb_patches.cpp */
extern "C" void nds_local_wireless_pump(void);      /* wifi_net.cpp, stanza -423 */
extern "C" int lb_setting_fast_link(void);          /* lb_overlay.cpp */

namespace {

// ---- the game's side -------------------------------------------------------
constexpr uint32_t kInitialize = 0x020F40D8u, kStartScan = 0x020F3324u,
                   kEndScan = 0x020F3690u, kMeasure = 0x020F3DACu,
                   kParentConnect = 0x020F4254u, kChildConnect = 0x020F4340u,
                   kShared = 0x020F43D4u, kStep = 0x020F43F4u,
                   kReset = 0x020F4448u, kFinalize = 0x020F4490u,
                   kEnd = 0x020F4598u, kConnectedAids = 0x02118628u,
                   kLinkLevel = 0x021188C4u, kSetGameInfo = 0x0211A084u,
                   kSetEntry = 0x0211A1B4u;

constexpr uint32_t kCtx = 0x02158AE0u;          // WH context
constexpr uint32_t kCtxAid = kCtx + 0x00u;      // u16
constexpr uint32_t kCtxBest = kCtx + 0x02u;     // u16 best-channel bitmap
constexpr uint32_t kCtxInit = kCtx + 0x04u;     // u16 initialized
constexpr uint32_t kCtxBitmap = kCtx + 0x0Au;   // u16 connected bitmap
constexpr uint32_t kCtxChannel = kCtx + 0x0Eu;  // u16 chosen channel
constexpr uint32_t kCtxState = kCtx + 0x10u;    // u32 sysState
constexpr uint32_t kCtxMode = kCtx + 0x28u;     // u32 connectMode
constexpr uint32_t kCtxError = kCtx + 0x40u;    // u32 last error
constexpr uint32_t kCtxScanCb = kCtx + 0x48u;   // scan callback
constexpr uint32_t kTargetBss = 0x02158C20u;    // WH's copy of the chosen host
constexpr uint32_t kDataSet = 0x02158EA0u + 4u; // where shared blocks are served
constexpr uint32_t kHostTable = 0x0215BDC0u;    // the game's join list
constexpr int kHostEntries = 20, kHostEntry = 0xC4;
constexpr uint32_t kGgid = 0x00800320u;
enum { kStop = 0, kIdle = 1, kScanning = 2, kBusy = 3, kDataSharing = 5,
       kMeasured = 7, kConnectFail = 8, kError = 9 };

constexpr int kStations = 3;
constexpr int kBlock = 0x8C;

// ---- the session -----------------------------------------------------------
constexpr uint32_t kMagic = 0x3156424Cu;   // "LBV1"
enum Kind : uint8_t { kBeacon = 1, kConnect = 2, kAccept = 3, kRefuse = 4,
                      kBlocks = 5, kDatasets = 6, kBye = 7 };
constexpr int kRing = 256;                 // host slots kept, a power of two
constexpr int kMaxL = 30;
constexpr int kJoinLead = 40;              // datasets between ACCEPT and sharing
constexpr int kStallKick = 900;            // host steps waiting on one member
// BR-0031/32: 20 s, not 10. A relayed link that drops takes up to 6 s to be
// noticed (lb_relay.cpp kDeadMs) and a few more to reopen; the relay holds
// the room 15 s. Resends cover what was lost meanwhile.
constexpr unsigned long long kSilentMs = 20000ull;
constexpr size_t kDatagramBudget = 1100;

struct Block {
    uint8_t len = 0;
    uint8_t b[kBlock] = {};
};
Block trim(const uint8_t* raw) {
    Block k;
    int n = kBlock;
    while (n > 0 && raw[n - 1] == 0) --n;
    k.len = (uint8_t)n;
    std::memcpy(k.b, raw, (size_t)n);
    return k;
}

struct Dataset {
    uint32_t index = 0;
    uint8_t present = 0, members = 0;
    Block block[kStations];
};

// An item one side keeps resending until the other acknowledges it.
template <class T> struct Outgoing {
    uint32_t index;
    T item;
    unsigned long long sent_at = 0;
    int sends = 0;
};

struct Member {
    uint32_t origin = 0;
    uint32_t first = 0;          // first dataset it shares
    uint32_t acked = 0;          // its cursor: the next dataset it needs
    uint32_t held = 0;           // the host holds all its blocks below this
    uint32_t echo = 0;           // its latest timestamp, echoed back
    unsigned long long heard = 0;
    unsigned waited_on = 0;      // host steps that waited for its block
    int rtt_ms = 100;
};

struct Seen {
    uint32_t id;
    uint8_t info[16];
    uint8_t open, members;
    uint16_t tgid;
    unsigned long long at;
    bool dirty;              // heard since it was last written
};

struct Session {
    bool on = false, host = false;
    uint32_t origin = 0;            // this console's random id
    uint32_t host_id = 0;
    int aid = 0;
    int L = 0;
    uint32_t cursor = 0;            // next dataset to consume
    uint32_t next_send = 0;         // next slot this console gives a block
    uint8_t present_last = 0;       // present bits of the dataset last served
    uint8_t members_last = 1;
    // HOST
    Block blk[kRing][kStations];
    uint32_t have[kRing][kStations];        // slot + 1 when present
    uint8_t members = 1;
    Member m[kStations];
    std::deque<Outgoing<Dataset>> committed;
    int waiting = 0;
    bool entry_open = true;
    uint8_t info[16] = {};
    uint16_t tgid = 0;
    unsigned long long last_beacon = 0, last_step = 0, last_datasets = 0;
    int step_us = 16667;
    // CLIENT
    std::deque<Outgoing<Block>> own;        // blocks the host has not acked
    std::deque<Dataset> inbox;
    uint32_t host_has = 0;          // the host holds our blocks below this
    bool connecting = false;
    unsigned long long connect_at = 0, heard = 0, last_sent = 0;
    int rtt_ms = -1;
    unsigned waited_seen = 0, window_steps = 0, window_base = 0;
    bool window_short = false;      // the previous window waited too
    int L0 = 0;
    // Scanning.
    bool scanning = false;
    std::vector<Seen> seen;
    // Statistics.
    unsigned waits = 0, steps = 0;
    unsigned long long report_at = 0;
    unsigned report_steps = 0, report_waits = 0, report_cursor = 0;
};

std::mutex g_mu;
Session* g_s = new Session();     // large; never freed
uint32_t g_origin = 0;
std::deque<std::vector<uint8_t>> g_out;     // datagrams for the bridge
struct Delayed { unsigned long long at; std::vector<uint8_t> d; };
std::deque<Delayed> g_in_delayed;           // LB_VWH_LATENCY_MS test aid
// Traffic counts, for the reports: received by kind, sent, dropped unsent.
unsigned g_rx[8] = {}, g_rx_bad = 0, g_tx = 0, g_tx_dropped = 0;
void traffic(char* out, size_t cap) {
    std::snprintf(out, cap, "rx beacon %u connect %u accept %u blocks %u datasets %u "
                  "bye %u bad %u, tx %u (dropped %u), delayed %u", g_rx[kBeacon],
                  g_rx[kConnect], g_rx[kAccept], g_rx[kBlocks], g_rx[kDatasets], g_rx[kBye],
                  g_rx_bad, g_tx, g_tx_dropped, (unsigned)g_in_delayed.size());
}

unsigned long long now_ms() { return SDL_GetTicks64(); }

int env_int(const char* name, int dflt) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : dflt;
}

void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[lb_vwh] ");
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    va_end(ap);
}

uint32_t origin_id() {
    if (!g_origin) {
        const uint64_t t = SDL_GetPerformanceCounter();
        g_origin = (uint32_t)(t ^ (t >> 29) ^ 0xA5C31E7Bu ^ (uint32_t)SDL_GetTicks());
        if (!g_origin) g_origin = 1;
    }
    return g_origin;
}

void fresh_session() {
    g_s->~Session();
    new (g_s) Session();
    std::memset(g_s->have, 0, sizeof g_s->have);
    g_s->origin = origin_id();
}

void set_state(uint32_t st) { bus_write_u32(kCtxState, st); }
uint32_t state() { return bus_read_u32(kCtxState); }

// ---- wire helpers ----------------------------------------------------------
void put16(std::vector<uint8_t>& d, uint32_t v) {
    d.push_back((uint8_t)v);
    d.push_back((uint8_t)(v >> 8));
}
void put32(std::vector<uint8_t>& d, uint32_t v) {
    for (int i = 0; i < 4; ++i) d.push_back((uint8_t)(v >> (8 * i)));
}
uint32_t get16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
void put_block(std::vector<uint8_t>& d, const Block& k) {
    d.push_back(k.len);
    d.insert(d.end(), k.b, k.b + k.len);
}
bool get_block(const uint8_t* p, int n, int& at, Block& k) {
    if (at >= n) return false;
    const int len = p[at++];
    if (len > kBlock || at + len > n) return false;
    k = Block{};
    k.len = (uint8_t)len;
    std::memcpy(k.b, p + at, (size_t)len);
    at += len;
    return true;
}
std::vector<uint8_t> head(Kind k) {
    std::vector<uint8_t> d;
    put32(d, kMagic);
    d.push_back((uint8_t)k);
    put32(d, g_s->origin);
    return d;
}
void post(std::vector<uint8_t> d) {
    if (g_out.size() > 64) {
        g_out.pop_front();
        ++g_tx_dropped;
    }
    g_out.push_back(std::move(d));
}
// A new item goes three times, 20 ms apart, so a short burst of loss does not
// cost a round trip; after that, once per round trip until acknowledged.
template <class T> bool due(const Outgoing<T>& o, unsigned long long t, int rtt) {
    if (o.sends == 0) return true;
    if (o.sends < 3) return t - o.sent_at >= 20ull;
    return t - o.sent_at > (unsigned long long)std::max(40, rtt + rtt / 4 + 15);
}

// ---- HOST ------------------------------------------------------------------
int ring(uint32_t slot) { return (int)(slot & (kRing - 1)); }
bool member(int a) { return (g_s->members >> a) & 1u; }
bool sharing(int a, uint32_t r) { return member(a) && (a == 0 || r >= g_s->m[a].first); }

void host_store(int a, uint32_t slot, const Block& k) {
    g_s->blk[ring(slot)][a] = k;
    g_s->have[ring(slot)][a] = slot + 1u;
}
bool host_has(int a, uint32_t slot) { return g_s->have[ring(slot)][a] == slot + 1u; }

bool host_ready(uint32_t r) {
    for (int a = 1; a < kStations; ++a)
        if (sharing(a, r) && !host_has(a, r)) return false;
    return true;
}

Dataset host_commit(uint32_t r, const Block& mine) {
    host_store(0, r, mine);
    Dataset d;
    d.index = r;
    d.members = g_s->members;
    for (int a = 0; a < kStations; ++a)
        if (sharing(a, r)) {
            d.present |= (uint8_t)(1u << a);
            d.block[a] = g_s->blk[ring(r)][a];
        }
    if (g_s->members != 1u) g_s->committed.push_back({r, d});
    return d;
}

uint32_t host_oldest_needed() {
    uint32_t oldest = 0xFFFFFFFFu;
    for (int a = 1; a < kStations; ++a)
        if (member(a)) oldest = std::min(oldest, g_s->m[a].acked);
    return oldest;
}

void host_send_datasets() {
    if (g_s->members == 1u) {
        g_s->committed.clear();
        return;
    }
    const unsigned long long t = now_ms();
    const uint32_t oldest = host_oldest_needed();
    while (!g_s->committed.empty() && g_s->committed.front().index < oldest)
        g_s->committed.pop_front();
    int rtt = 40;
    for (int a = 1; a < kStations; ++a)
        if (member(a)) rtt = std::max(rtt, g_s->m[a].rtt_ms);
    auto start = [&]() {
        std::vector<uint8_t> d = head(kDatasets);
        for (int a = 0; a < kStations; ++a) {
            put32(d, g_s->m[a].held);               // our ack of its blocks
            put16(d, std::min(g_s->m[a].waited_on, 0xFFFFu));
            put32(d, g_s->m[a].echo);
        }
        put32(d, g_s->cursor);
        put32(d, (uint32_t)(g_s->step_us));
        d.push_back(0);                              // count
        return d;
    };
    std::vector<uint8_t> d = start();
    size_t count_at = d.size() - 1;
    int n = 0, datagrams = 0;
    for (auto& o : g_s->committed) {
        if (!due(o, t, rtt)) continue;
        size_t size = 6;
        for (int a = 0; a < kStations; ++a)
            if (o.item.present & (1u << a)) size += 1u + o.item.block[a].len;
        if (d.size() + size > kDatagramBudget || n == 255) {
            d[count_at] = (uint8_t)n;
            post(std::move(d));
            g_s->last_datasets = t;
            if (++datagrams >= 4) return;
            d = start();
            count_at = d.size() - 1;
            n = 0;
        }
        put32(d, o.item.index);
        d.push_back(o.item.present);
        d.push_back(o.item.members);
        for (int a = 0; a < kStations; ++a)
            if (o.item.present & (1u << a)) put_block(d, o.item.block[a]);
        o.sent_at = t;
        ++o.sends;
        ++n;
    }
    // Nothing due: a keep-alive (the acks ride on it) at most every 50 ms,
    // not one every tick (docs/424: each is a relay message too).
    if (n == 0 && datagrams == 0 && t - g_s->last_datasets < 50ull) return;
    d[count_at] = (uint8_t)n;
    post(std::move(d));
    g_s->last_datasets = t;
}

void host_beacon() {
    const unsigned long long t = now_ms();
    if (t - g_s->last_beacon < 200ull) return;
    g_s->last_beacon = t;
    std::vector<uint8_t> d = head(kBeacon);
    d.push_back(g_s->entry_open ? 1 : 0);
    d.push_back(g_s->members);
    d.insert(d.end(), g_s->info, g_s->info + 16);
    put16(d, g_s->tgid);
    post(std::move(d));
}

void host_drop(int a, const char* why) {
    if (!member(a)) return;
    g_s->members &= (uint8_t)~(1u << a);
    g_s->m[a] = Member{};
    log_line("host: AID %d %s", a, why);
}

void host_receive(Kind k, uint32_t from, const uint8_t* p, int n) {
    int aid = -1;
    for (int a = 1; a < kStations; ++a)
        if (member(a) && g_s->m[a].origin == from) aid = a;
    if (k == kConnect && n >= 8) {
        if (get32(p) != g_s->origin) return;
        const uint32_t t0 = get32(p + 4);
        if (aid < 0 && g_s->entry_open)
            for (int a = 1; a < kStations && aid < 0; ++a)
                if (!member(a)) aid = a;
        if (aid < 0) {
            std::vector<uint8_t> d = head(kRefuse);
            put32(d, from);
            post(std::move(d));
            return;
        }
        if (!member(aid)) {
            g_s->members |= (uint8_t)(1u << aid);
            Member& m = g_s->m[aid];
            m = Member{};
            m.origin = from;
            m.first = g_s->cursor + (uint32_t)kJoinLead;
            m.acked = m.first;
            m.held = m.first;
            m.heard = now_ms();
            log_line("host: AID %d joins (session %08X), sharing from dataset %u",
                     aid, from, m.first);
        }
        std::vector<uint8_t> d = head(kAccept);
        put32(d, from);
        d.push_back((uint8_t)aid);
        put32(d, g_s->m[aid].first);
        put32(d, t0);
        put32(d, (uint32_t)g_s->step_us);
        post(std::move(d));
        return;
    }
    if (aid < 0) return;
    Member& m = g_s->m[aid];
    m.heard = now_ms();
    if (k == kBlocks && n >= 11) {
        const uint32_t ack = get32(p);
        m.echo = get32(p + 4);
        m.rtt_ms = (int)get16(p + 8);
        const int count = p[10];
        if (ack > m.acked) m.acked = ack;
        int at = 11;
        for (int i = 0; i < count; ++i) {
            if (at + 4 > n) return;
            const uint32_t slot = get32(p + at);
            at += 4;
            Block b;
            if (!get_block(p, n, at, b)) return;
            if (slot >= m.first && slot >= g_s->cursor &&
                slot < g_s->cursor + (uint32_t)kRing - 8u && !host_has(aid, slot))
                host_store(aid, slot, b);
        }
        // A running count, not a scan of the ring, which wraps.
        while (host_has(aid, m.held)) ++m.held;
    } else if (k == kBye) {
        host_drop(aid, "left");
    }
}

// ---- CLIENT ----------------------------------------------------------------
void client_send_blocks(bool force) {
    const unsigned long long t = now_ms();
    while (!g_s->own.empty() && g_s->own.front().index < g_s->host_has) g_s->own.pop_front();
    const int rtt = g_s->rtt_ms < 0 ? 100 : g_s->rtt_ms;
    bool any = false;
    for (const auto& o : g_s->own) any |= due(o, t, rtt);
    if (!any && !force && t - g_s->last_sent < 50ull) return;
    auto start = [&]() {
        std::vector<uint8_t> d = head(kBlocks);
        put32(d, g_s->cursor);
        put32(d, (uint32_t)t);
        put16(d, (uint32_t)std::clamp(rtt, 0, 0xFFFF));
        d.push_back(0);
        return d;
    };
    std::vector<uint8_t> d = start();
    size_t count_at = d.size() - 1;
    int n = 0, datagrams = 0;
    for (auto& o : g_s->own) {
        if (!due(o, t, rtt)) continue;
        if (d.size() + 5u + o.item.len > kDatagramBudget) {
            d[count_at] = (uint8_t)n;
            post(std::move(d));
            if (++datagrams >= 4) {
                g_s->last_sent = t;
                return;   // the rest next time; d is gone
            }
            d = start();
            count_at = d.size() - 1;
            n = 0;
        }
        put32(d, o.index);
        put_block(d, o.item);
        o.sent_at = t;
        ++o.sends;
        ++n;
    }
    d[count_at] = (uint8_t)n;
    post(std::move(d));
    g_s->last_sent = t;
}

void client_give(const Block& b) {
    g_s->own.push_back({g_s->next_send++, b});
}

void client_receive(Kind k, uint32_t from, const uint8_t* p, int n) {
    if (from != g_s->host_id) return;
    g_s->heard = now_ms();
    if (k == kAccept && n >= 17 && get32(p) == g_s->origin && g_s->connecting) {
        g_s->connecting = false;
        g_s->aid = p[4];
        g_s->cursor = get32(p + 5);
        g_s->rtt_ms = (int)(uint32_t)((uint32_t)now_ms() - get32(p + 9));
        const int step_us = std::clamp((int)get32(p + 13), 8000, 100000);
        const int forced = env_int("LB_VWH_DEPTH", 0);
        const int need = (g_s->rtt_ms + 30) * 1000 / step_us + 2;
        g_s->L = forced > 0 ? std::min(forced, kMaxL) : std::clamp(need, 2, kMaxL);
        g_s->L0 = g_s->L;
        g_s->next_send = g_s->cursor;
        g_s->host_has = g_s->cursor;
        for (int i = 0; i < g_s->L; ++i) client_give(Block{});
        bus_write_u16(kCtxAid, (uint16_t)g_s->aid);
        bus_write_u16(kCtxBitmap, 1u);
        set_state(kDataSharing);
        log_line("client: AID %d, sharing from dataset %u; round trip %d ms, step "
                 "%d us -> depth %d", g_s->aid, g_s->cursor, g_s->rtt_ms, step_us, g_s->L);
        client_send_blocks(true);
        return;
    }
    if (k == kRefuse && n >= 4 && get32(p) == g_s->origin && g_s->connecting) {
        g_s->connecting = false;
        g_s->on = false;
        set_state(kConnectFail);
        log_line("client: the host refused (full or closed)");
        return;
    }
    if (k == kDatasets && !g_s->connecting) {
        constexpr int kHead = kStations * 10 + 9;
        if (n < kHead) return;
        const uint8_t* me = p + 10 * g_s->aid;
        const uint32_t has = get32(me);
        if (has > g_s->host_has) g_s->host_has = has;
        const unsigned waited = get16(me + 4);
        const uint32_t echo = get32(me + 6);
        if (echo) {
            const int sample = (int)(uint32_t)((uint32_t)now_ms() - echo);
            if (sample >= 0 && sample < 5000)
                g_s->rtt_ms = g_s->rtt_ms < 0 ? sample : (g_s->rtt_ms * 7 + sample) / 8;
        }
        // The host waited on our blocks: the pipeline is too shallow.
        if (waited > g_s->waited_seen) {
            g_s->window_base += waited - g_s->waited_seen;
            g_s->waited_seen = waited;
        }
        int at = kHead;
        const int count = p[at - 1];
        for (int i = 0; i < count; ++i) {
            if (at + 6 > n) return;
            Dataset ds;
            ds.index = get32(p + at);
            ds.present = p[at + 4];
            ds.members = p[at + 5];
            at += 6;
            for (int a = 0; a < kStations; ++a)
                if ((ds.present & (1u << a)) && !get_block(p, n, at, ds.block[a])) return;
            if (ds.index < g_s->cursor || ds.index > g_s->cursor + 512u) continue;
            bool dup = false;
            for (const Dataset& q : g_s->inbox) dup |= q.index == ds.index;
            if (!dup) g_s->inbox.push_back(ds);
        }
        return;
    }
    if (k == kBye) {
        log_line("client: the host left");
        g_s->on = false;
        set_state(kError);   // the game's own link-lost path (net task, state 9)
    }
}

// ---- scanning: beacons into the game's join list --------------------------
void scan_receive(uint32_t from, const uint8_t* p, int n) {
    if (n < 2 + 16 + 2) return;
    Seen s{};
    s.id = from;
    s.open = p[0];
    s.members = p[1];
    std::memcpy(s.info, p + 2, 16);
    s.tgid = (uint16_t)get16(p + 18);
    s.at = now_ms();
    s.dirty = true;
    for (auto& q : g_s->seen)
        if (q.id == from) { q = s; return; }
    if (g_s->seen.size() < 8) g_s->seen.push_back(s);
}

// The BSSID a host is listed under: 02 4C + its id, so ChildConnect finds it.
void bssid_of(uint32_t id, uint8_t out[6]) {
    out[0] = 0x02;
    out[1] = 0x4C;
    for (int i = 0; i < 4; ++i) out[2 + i] = (uint8_t)(id >> (8 * i));
}

// FUN_020F616C's work: a BSSID already listed has its entry refreshed and its
// time to live reset; a new one takes the first free entry with "new" set.
void write_host_entry(const Seen& s) {
    uint8_t bss[0xC0] = {};
    uint8_t id[6];
    bssid_of(s.id, id);
    bss[0] = 0x60;                                   // length, in u16 words
    std::memcpy(bss + 4, id, 6);                     // bssid
    bss[0x32] = 200;                                 // beacon period
    bss[0x36] = 1;                                   // channel
    bss[0x3C] = 0x20;                                // gameInfoLength
    bss[0x40] = 1;                                   // magicNumber
    for (int i = 0; i < 4; ++i) bss[0x44 + i] = (uint8_t)(kGgid >> (8 * i));
    bss[0x48] = (uint8_t)s.tgid;
    bss[0x49] = (uint8_t)(s.tgid >> 8);
    bss[0x4A] = 0x10;                                // userGameInfoLength
    bss[0x4B] = s.open ? 1 : 0;                      // attribute: entry open
    bss[0x4C] = (uint8_t)(kStations * kBlock + 4);
    bss[0x4D] = (uint8_t)((kStations * kBlock + 4) >> 8);
    bss[0x4E] = kBlock;
    std::memcpy(bss + 0x50, s.info, 16);             // the host's user info
    int slot = -1;
    for (int e = 0; e < kHostEntries && slot < 0; ++e) {
        const uint32_t base = kHostTable + (uint32_t)(e * kHostEntry);
        if (!bus_read_u8(base + 0xC1u)) continue;
        bool same = true;
        for (int i = 0; i < 6; ++i) same &= bus_read_u8(base + 4u + (uint32_t)i) == id[i];
        if (same) slot = e;
    }
    bool fresh = false;
    for (int e = 0; e < kHostEntries && slot < 0; ++e)
        if (!bus_read_u8(kHostTable + (uint32_t)(e * kHostEntry) + 0xC1u)) {
            slot = e;
            fresh = true;
        }
    if (slot < 0) return;
    const uint32_t base = kHostTable + (uint32_t)(slot * kHostEntry);
    for (int i = 0; i < 0xC0; ++i) bus_write_u8(base + (uint32_t)i, bss[i]);
    if (fresh) bus_write_u8(base + 0xC0u, 1u);
    bus_write_u8(base + 0xC1u, 0xB4u);
}

// ---- receiving -------------------------------------------------------------
void dispatch(const uint8_t* d, int n) {
    if (n < 9 || get32(d) != kMagic || d[4] >= 8) {
        ++g_rx_bad;
        return;
    }
    const Kind k = (Kind)d[4];
    ++g_rx[k];
    const uint32_t from = get32(d + 5);
    if (from == g_s->origin) return;
    const uint8_t* p = d + 9;
    const int len = n - 9;
    if (k == kBeacon) {
        if (g_s->scanning) scan_receive(from, p, len);
        return;
    }
    if (!g_s->on) return;
    if (g_s->host) host_receive(k, from, p, len);
    else client_receive(k, from, p, len);
}

bool silent_since(unsigned long long t, unsigned long long heard) {
    return t > heard && t - heard > kSilentMs;
}

// Every frame (vblank) and every step: deliver, scan, beacon, keep alive.
void tick_locked() {
    {
        const unsigned long long now = now_ms();
        while (!g_in_delayed.empty() && g_in_delayed.front().at <= now) {
            dispatch(g_in_delayed.front().d.data(), (int)g_in_delayed.front().d.size());
            g_in_delayed.pop_front();
        }
    }
    // After the deliveries, which stamp "heard" with the clock: a time taken
    // before them is older than those stamps, and t - heard would wrap.
    const unsigned long long t = now_ms();
    // One write per beacon heard, as the scan callback runs once per beacon:
    // the net task raises an update for an entry whose time to live is full.
    if (g_s->scanning)
        for (auto& s : g_s->seen)
            if (s.dirty) {
                s.dirty = false;
                write_host_entry(s);
            }
    if (!g_s->on) return;
    if (g_s->host) {
        host_beacon();
        for (int a = 1; a < kStations; ++a)
            if (member(a) && silent_since(t, g_s->m[a].heard)) {
                char tr[200];
                traffic(tr, sizeof tr);
                log_line("host: traffic at silence: %s", tr);
                host_drop(a, "went silent");
            }
        host_send_datasets();
        return;
    }
    if (g_s->connecting) {
        if (t - g_s->connect_at > 250ull) {
            g_s->connect_at = t;
            std::vector<uint8_t> d = head(kConnect);
            put32(d, g_s->host_id);
            put32(d, (uint32_t)t);
            post(std::move(d));
        }
        if (silent_since(t, g_s->heard)) {
            g_s->connecting = false;
            g_s->on = false;
            set_state(kConnectFail);
            log_line("client: no answer from session %08X", g_s->host_id);
        }
        return;
    }
    if (silent_since(t, g_s->heard)) {
        char tr[200];
        traffic(tr, sizeof tr);
        log_line("client: the host went silent (%s)", tr);
        g_s->on = false;
        set_state(kError);
        return;
    }
    client_send_blocks(false);
}

// Chosen when WH is initialized and kept until it ends, so a session never
// changes route: Multiplayer > "Fast online link", or LB_VWH=0/1.
int g_on = -1;
bool vwh_choose() {
    const char* v = std::getenv("LB_VWH");
    const int on = v && *v ? (std::atoi(v) != 0) : (lb_setting_fast_link() != 0);
    if (on != g_on)
        log_line("%s", on ? "on: host code answers the wireless helper (docs/423)"
                          : "off: the emulated radio carries the game");
    g_on = on;
    return on != 0;
}
bool vwh_on() { return g_on < 0 ? vwh_choose() : g_on != 0; }

void tail(uint32_t addr) {
    void (*original)(void) = lb_dispatch_original(0, addr);
    if (original) original();
}

void vwh_leave(const char* why) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_s->on) {
            std::vector<uint8_t> d = head(kBye);
            post(d);
            post(std::move(d));
            log_line("%s: left the session (%u steps, %u waited)", why, g_s->steps,
                     g_s->waits);
        }
        g_s->on = false;
        g_s->connecting = false;
        g_s->scanning = false;
        g_s->present_last = 0;
    }
    nds_local_wireless_pump();
}

}  // namespace

// ---- the bridge's side -----------------------------------------------------
extern "C" int lb_vwh_outgoing(unsigned char* buf, int cap) {
    std::lock_guard<std::mutex> lk(g_mu);
    while (!g_out.empty()) {
        std::vector<uint8_t> d = std::move(g_out.front());
        g_out.pop_front();
        if ((int)d.size() > cap) {
            ++g_tx_dropped;
            continue;
        }
        ++g_tx;
        std::memcpy(buf, d.data(), d.size());
        return (int)d.size();
    }
    return 0;
}

extern "C" void lb_vwh_incoming(const unsigned char* buf, int len, unsigned int sender) {
    (void)sender;
    std::lock_guard<std::mutex> lk(g_mu);
    static const int lat = env_int("LB_VWH_LATENCY_MS", 0);   // test aid: one way
    static const int loss = env_int("LB_VWH_LOSS_PCT", 0);    // test aid
    if (loss > 0) {
        static uint32_t x = 0x9E3779B9u;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        if ((int)(x % 100u) < loss) return;
    }
    if (lat > 0) {
        g_in_delayed.push_back({now_ms() + (unsigned long long)lat,
                                std::vector<uint8_t>(buf, buf + len)});
        return;
    }
    dispatch(buf, len);
}

// Once a frame, from the vblank (lb_patches.cpp).
extern "C" void lb_vwh_tick(void) {
    if (!vwh_on()) return;
    bool live;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        live = g_s->on || g_s->scanning || !g_in_delayed.empty() || !g_out.empty();
    }
    if (!live) return;
    nds_local_wireless_pump();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        tick_locked();
    }
    nds_local_wireless_pump();
}

extern "C" int lb_vwh_status(char* out, int cap) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_s->on) return std::snprintf(out, (size_t)cap, "not sharing");
    return std::snprintf(out, (size_t)cap, "%s AID %d, members %X, dataset %u, depth %d, "
                         "round trip %d ms, waited %u of %u steps",
                         g_s->host ? "host" : "client", g_s->aid, g_s->members, g_s->cursor,
                         g_s->L, g_s->rtt_ms, g_s->waits, g_s->steps);
}

// ---- the overrides (ARM, all of them) -------------------------------------
// Each answers the WH function in place of its cartridge code and returns to
// the caller, with R0 the result. Nothing here calls WM.

extern "C" void vwh_initialize_override(void) {          // WH_Initialize
    if (bus_read_u16(kCtxInit) == 0u) vwh_choose();
    if (!vwh_on()) return tail(kInitialize);
    if (bus_read_u16(kCtxInit) == 0u) {
        bus_write_u16(kCtxAid, 0u);
        bus_write_u16(kCtxBitmap, 1u);
        bus_write_u32(kCtxError, 0u);
        bus_write_u16(kCtxInit, 1u);
        set_state(kIdle);
        std::lock_guard<std::mutex> lk(g_mu);
        fresh_session();
        log_line("WH_Initialize (session %08X)", g_s->origin);
    }
    lb_vwh_override_return();
}

extern "C" void vwh_measure_override(void) {             // WH_StartMeasureChannel
    if (!vwh_on()) return tail(kMeasure);
    bus_write_u16(kCtxChannel, 0u);
    bus_write_u16(kCtxBest, 0x2082u);         // channels 1, 7 and 13
    set_state(kMeasured);
    g_cpu.R[0] = 1u;
    lb_vwh_override_return();
}

extern "C" void vwh_parent_connect_override(void) {      // WH_ParentConnect(mode, tgid, channel)
    if (!vwh_on()) return tail(kParentConnect);
    bus_write_u32(kCtxMode, g_cpu.R[0]);
    bus_write_u16(kCtxAid, 0u);
    bus_write_u16(kCtxBitmap, 1u);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        uint8_t info[16];
        std::memcpy(info, g_s->info, 16);
        fresh_session();
        std::memcpy(g_s->info, info, 16);
        g_s->on = true;
        g_s->host = true;
        g_s->host_id = g_s->origin;
        g_s->tgid = (uint16_t)g_cpu.R[1];
        log_line("host: data sharing as session %08X", g_s->origin);
    }
    set_state(kDataSharing);
    g_cpu.R[0] = 1u;
    lb_vwh_override_return();
}

extern "C" void vwh_start_scan_override(void) {          // WH_StartScan(callback, mac, channel)
    if (!vwh_on()) return tail(kStartScan);
    bus_write_u32(kCtxScanCb, g_cpu.R[0]);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_s->scanning = true;
        g_s->seen.clear();
    }
    set_state(kScanning);
    g_cpu.R[0] = 1u;
    lb_vwh_override_return();
}

extern "C" void vwh_end_scan_override(void) {            // WH_EndScan
    if (!vwh_on()) return tail(kEndScan);
    uint32_t r = 0u;
    if (state() == kScanning) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_s->scanning = false;
        set_state(kIdle);
        r = 1u;
    }
    g_cpu.R[0] = r;
    lb_vwh_override_return();
}

extern "C" void vwh_child_connect_override(void) {       // WH_ChildConnect(mode, bss)
    if (!vwh_on()) return tail(kChildConnect);
    const uint32_t bss = g_cpu.R[1];
    bus_write_u32(kCtxMode, g_cpu.R[0]);
    uint8_t id[6];
    for (int i = 0; i < 6; ++i) id[i] = bus_read_u8(bss + 4u + (uint32_t)i);
    for (int i = 0; i < 0xC0; ++i)
        bus_write_u8(kTargetBss + (uint32_t)i, bus_read_u8(bss + (uint32_t)i));
    const uint32_t host = (uint32_t)id[2] | ((uint32_t)id[3] << 8) |
                          ((uint32_t)id[4] << 16) | ((uint32_t)id[5] << 24);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        fresh_session();
        g_s->on = true;
        g_s->host_id = host;
        g_s->connecting = true;
        g_s->heard = now_ms();
        log_line("client: connecting to session %08X", host);
    }
    set_state(kBusy);
    g_cpu.R[0] = 1u;
    lb_vwh_override_return();
}

extern "C" void vwh_step_override(void) {                // WH_StepDS(send)
    if (!vwh_on()) return tail(kStep);
    const uint32_t send = g_cpu.R[0];
    nds_local_wireless_pump();
    uint32_t ok = 0u;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        tick_locked();
        if (g_s->on && !g_s->connecting && state() == kDataSharing) {
            ++g_s->steps;
            const unsigned long long t = now_ms();
            // Every 10 s: datasets consumed a second, and the steps that waited.
            if (!g_s->report_at) {
                g_s->report_at = t;
                g_s->report_cursor = g_s->cursor;
            } else if (t - g_s->report_at >= 10000ull) {
                const double secs = (double)(t - g_s->report_at) / 1000.0;
                char tr[200];
                traffic(tr, sizeof tr);
                log_line("%s: %.1f datasets/s, %u of %u steps waited, depth %d, round "
                         "trip %d ms, members %X; %s", g_s->host ? "host" : "client",
                         (double)(g_s->cursor - g_s->report_cursor) / secs,
                         g_s->waits - g_s->report_waits, g_s->steps - g_s->report_steps,
                         g_s->L, g_s->rtt_ms, g_s->members, tr);
                g_s->report_at = t;
                g_s->report_steps = g_s->steps;
                g_s->report_waits = g_s->waits;
                g_s->report_cursor = g_s->cursor;
            }
            if (g_s->last_step && t - g_s->last_step < 200ull) {
                const int us = (int)(t - g_s->last_step) * 1000;
                g_s->step_us = (g_s->step_us * 15 + us) / 16;
            }
            g_s->last_step = t;
            uint8_t raw[kBlock];
            for (int i = 0; i < kBlock; ++i) raw[i] = bus_read_u8(send + (uint32_t)i);
            Dataset ds;
            bool have = false;
            if (g_s->host) {
                if (host_ready(g_s->cursor)) {
                    ds = host_commit(g_s->cursor, trim(raw));
                    have = true;
                    g_s->waiting = 0;
                } else {
                    for (int a = 1; a < kStations; ++a)
                        if (sharing(a, g_s->cursor) && !host_has(a, g_s->cursor))
                            ++g_s->m[a].waited_on;
                    if (++g_s->waiting > kStallKick) {
                        for (int a = 1; a < kStations; ++a)
                            if (sharing(a, g_s->cursor) && !host_has(a, g_s->cursor))
                                host_drop(a, "timed out");
                        g_s->waiting = 0;
                    }
                }
            } else {
                for (auto it = g_s->inbox.begin(); it != g_s->inbox.end(); ++it)
                    if (it->index == g_s->cursor) {
                        ds = *it;
                        g_s->inbox.erase(it);
                        have = true;
                        break;
                    }
                if (have) {
                    // A shallow pipeline shows as the host waiting on us: one
                    // more slot of slack, as an empty block. Only after two
                    // windows of 120 steps in a row that waited -- a lost
                    // datagram makes one, and depth would not cure it -- and
                    // never more than 4 past the starting depth.
                    if (++g_s->window_steps >= 120u) {
                        const bool short_now = g_s->window_base > 6u;
                        if (short_now && g_s->window_short &&
                            g_s->L < std::min(kMaxL, g_s->L0 + 4)) {
                            client_give(Block{});
                            ++g_s->L;
                            log_line("client: depth %d (the host waited %u steps)", g_s->L,
                                     g_s->window_base);
                        }
                        g_s->window_short = short_now;
                        g_s->window_steps = 0;
                        g_s->window_base = 0;
                    }
                    client_give(trim(raw));
                }
            }
            if (have) {
                for (int a = 0; a < kStations; ++a) {
                    const Block& b = ds.block[a];
                    for (int i = 0; i < kBlock; ++i)
                        bus_write_u8(kDataSet + (uint32_t)(a * kBlock + i),
                                     i < b.len ? b.b[i] : 0u);
                }
                g_s->present_last = ds.present;
                g_s->members_last = ds.members;
                if (g_s->host) bus_write_u16(kCtxBitmap, g_s->members);
                ++g_s->cursor;
                ok = 1u;
                if (g_s->host) host_send_datasets();
                else client_send_blocks(false);
            } else {
                ++g_s->waits;
                bus_write_u32(kCtxError, 5u);   // NO_DATASET, as WH sets it
            }
        }
    }
    nds_local_wireless_pump();
    g_cpu.R[0] = ok;
    lb_vwh_override_return();
}

extern "C" void vwh_shared_override(void) {              // WH_GetSharedDataAdr(aid)
    if (!vwh_on()) return tail(kShared);
    const uint32_t aid = g_cpu.R[0];
    uint32_t r = 0u;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (aid < (uint32_t)kStations && (g_s->present_last & (1u << aid)))
            r = kDataSet + aid * (uint32_t)kBlock;
    }
    g_cpu.R[0] = r;
    lb_vwh_override_return();
}

extern "C" void vwh_reset_override(void) {               // WH_Reset
    if (!vwh_on()) return tail(kReset);
    vwh_leave("WH_Reset");
    bus_write_u16(kCtxBitmap, 1u);
    set_state(kIdle);
    lb_vwh_override_return();
}

extern "C" void vwh_finalize_override(void) {            // WH_Finalize
    if (!vwh_on()) return tail(kFinalize);
    vwh_leave("WH_Finalize");
    bus_write_u16(kCtxBitmap, 1u);
    set_state(kIdle);
    lb_vwh_override_return();
}

extern "C" void vwh_end_override(void) {                 // WH_End
    if (!vwh_on()) return tail(kEnd);
    vwh_leave("WH_End");
    bus_write_u16(kCtxInit, 0u);
    set_state(kStop);
    g_cpu.R[0] = 1u;
    lb_vwh_override_return();
}

// WM_GetConnectedAIDs: the AIDs of this console's partners (FUN_020F6684
// counts them).
extern "C" void wm_connected_override(void) {
    if (!vwh_on()) return tail(kConnectedAids);
    uint32_t r = 0u;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        // The partners only, never this console: the host's children, a
        // child's parent. The lobby's Continue test (FUN_020B8EB4,
        // FUN_020BA65C) counts these bits against the clients in records
        // 1-2, so a host bit 0 here made the count one too many.
        if (g_s->on && !g_s->connecting) r = g_s->host ? (g_s->members & ~1u) & 0xFFu : 1u;
    }
    g_cpu.R[0] = r;
    lb_vwh_override_return();
}

extern "C" void wm_link_override(void) {                 // WM_GetLinkLevel
    if (!vwh_on()) return tail(kLinkLevel);
    uint32_t r = 0u;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_s->on && !g_s->connecting) {
            const int rtt = g_s->host ? 0 : g_s->rtt_ms;
            r = rtt < 120 ? 3u : rtt < 250 ? 2u : 1u;
        }
    }
    g_cpu.R[0] = r;
    lb_vwh_override_return();
}

extern "C" void wm_game_info_override(void) {            // WM_SetGameInfo(cb, info, size, ...)
    if (!vwh_on()) return tail(kSetGameInfo);
    const uint32_t info = g_cpu.R[1], size = g_cpu.R[2] & 0xFFFFu;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (uint32_t i = 0; i < 16u; ++i)
            g_s->info[i] = i < size ? bus_read_u8(info + i) : 0u;
    }
    g_cpu.R[0] = 2u;   // WM_ERRCODE_OPERATING, which the caller expects
    lb_vwh_override_return();
}

// WM_SetEntry. Its one caller is the Start (net state 3, FUN_020F57C4), which
// closes the lobby; a 0 there makes it run the callback FUN_020F6158 itself.
extern "C" void wm_set_entry_override(void) {
    if (!vwh_on()) return tail(kSetEntry);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_s->entry_open = false;
        log_line("entry closed (the match starts)");
    }
    g_cpu.R[0] = 0u;   // done at once: the caller runs its callback itself
    lb_vwh_override_return();
}
