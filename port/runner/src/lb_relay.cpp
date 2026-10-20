// legobattles-decomp: online play through the master server's relay (docs/424).
//
// WHY. The direct route is UDP between the two players, with STUN, hole
// punching (docs/400) and UPnP (docs/405) to open the routers. Some networks
// still let nothing in: a VPN, carrier-grade or symmetric NAT, a firewall that
// drops every unasked datagram. lsw2ds-pc met exactly that in a playtest and
// added a relay to its master server; this is the same relay for this port
// (services/master-server-cloudflare/src/relay.js).
//
// HOW. Both players dial OUT to the master server -- a WebSocket over the
// plain HTTP its lobby list already uses -- and the lobby's relay room joins
// them. What crosses it is the bridge's own datagrams, whole: the transport
// (wifi_net.cpp, stanza -424) hands every datagram it sends to lb_relay_send
// as well, and reads lb_relay_recv before its socket, so the host-level WH,
// chat and rules above it never know which way a datagram came.
//
//   host    while it lists a lobby on a server with a relay, keeps its room
//           open (reconnecting with backoff); sends to every relayed joiner.
//   joiner  opens the lobby's room only when the direct route has carried
//           nothing for 5 s (lb_net.cpp), and then stops using the direct
//           route, so nothing arrives twice.
//
// THE WIRE. A message is a batch: [u16 length, little-endian][datagram]...
// The host's carry [kind][peer] first (0 data, 255 = every joiner; 2 close);
// the room's to the host carry [0][peer] (data), [1][peer] (open), [2][peer]
// (gone). Text: "ready" to a paired joiner, "ping"/"pong".
//
// THREADS. One detached thread per connection reads the socket (select,
// 200 ms); sends are made by the caller under the connection's mutex. Nothing
// waits for a thread to end (see Session below). The WebSocket client is
// lsw2ds-pc's (port/net/lsw2_wsrelay.cpp), trimmed to what is used here.
//
// LB_TEST_FORCE_RELAY=1 (a test aid): a joiner goes to the relay at once, and
// neither side uses the direct route, so one machine can test it.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_t = SOCKET;
#  define LB_INVALID INVALID_SOCKET
#  define lb_closesock closesocket
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
#  define LB_INVALID (-1)
#  define lb_closesock ::close
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kKeepaliveMs = 20000;   // Cloudflare closes a WebSocket idle ~100 s
constexpr size_t kBatchBytes = 1200;  // flush a batch past this
constexpr size_t kInQueue = 1024;

int ms_since(Clock::time_point t) {
    return (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

void set_blocking(socket_t s, bool blocking) {
#ifdef _WIN32
    u_long on = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    const int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? (f & ~O_NONBLOCK) : (f | O_NONBLOCK));
#endif
}

bool send_all(socket_t s, const char* p, size_t n) {
    while (n > 0) {
        const int k = ::send(s, p, (int)std::min<size_t>(n, 1u << 20), 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

bool wait_readable(socket_t s, int ms) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return ::select((int)s + 1, &rd, nullptr, nullptr, &tv) > 0;
}

// Resolve and connect within `ms`, leaving the socket blocking.
socket_t connect_within(const std::string& host, int port, int ms) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char portbuf[16];
    std::snprintf(portbuf, sizeof portbuf, "%d", port);
    if (getaddrinfo(host.c_str(), portbuf, &hints, &res) != 0 || !res) return LB_INVALID;
    socket_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == LB_INVALID) {
        freeaddrinfo(res);
        return LB_INVALID;
    }
    set_blocking(s, false);
    bool ok = ::connect(s, res->ai_addr, (int)res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) {
        fd_set wr, ex;
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(s, &wr);
        FD_SET(s, &ex);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        if (::select((int)s + 1, nullptr, &wr, &ex, &tv) > 0 && !FD_ISSET(s, &ex)) {
            int err = 0;
            socklen_t len = sizeof err;
            ok = getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0;
        }
    }
    if (!ok) {
        lb_closesock(s);
        return LB_INVALID;
    }
    set_blocking(s, true);
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof on);
#ifdef _WIN32
    DWORD t = 5000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t);
#else
    timeval tv{5, 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
    return s;
}

std::string base64(const uint8_t* p, size_t n) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = ((uint32_t)p[i] << 16) | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0u) |
                           (i + 2 < n ? (uint32_t)p[i + 2] : 0u);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < n ? k[(v >> 6) & 63] : '=';
        out += i + 2 < n ? k[v & 63] : '=';
    }
    return out;
}

uint32_t next_rand(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

struct Message {
    uint8_t opcode = 0;   // 1 text, 2 binary, 8 close, 9 ping, 10 pong
    std::string data;
};

std::string ws_frame(uint8_t opcode, const uint8_t* data, size_t n, uint32_t mask) {
    std::string f;
    f.reserve(n + 14);
    f.push_back((char)(0x80u | (opcode & 0x0Fu)));
    if (n < 126) {
        f.push_back((char)(0x80u | n));
    } else if (n < 65536) {
        f.push_back((char)(0x80u | 126u));
        f.push_back((char)((n >> 8) & 0xFFu));
        f.push_back((char)(n & 0xFFu));
    } else {
        f.push_back((char)(0x80u | 127u));
        for (int i = 7; i >= 0; --i) f.push_back((char)(((uint64_t)n >> (8 * i)) & 0xFFu));
    }
    const uint8_t mk[4] = {(uint8_t)(mask >> 24), (uint8_t)(mask >> 16), (uint8_t)(mask >> 8),
                           (uint8_t)mask};
    f.append((const char*)mk, 4);
    for (size_t i = 0; i < n; ++i) f.push_back((char)(data[i] ^ mk[i & 3]));
    return f;
}

// Whole frames off the front of `buf`; continuations joined.
bool ws_parse(std::string& buf, std::string& partial, uint8_t& partial_op,
              std::vector<Message>* out) {
    size_t pos = 0;
    for (;;) {
        const size_t avail = buf.size() - pos;
        if (avail < 2) break;
        const uint8_t b0 = (uint8_t)buf[pos], b1 = (uint8_t)buf[pos + 1];
        const bool fin = (b0 & 0x80u) != 0;
        const uint8_t op = b0 & 0x0Fu;
        const bool masked = (b1 & 0x80u) != 0;
        uint64_t len = b1 & 0x7Fu;
        size_t h = 2;
        if (len == 126) {
            if (avail < 4) break;
            len = ((uint64_t)(uint8_t)buf[pos + 2] << 8) | (uint8_t)buf[pos + 3];
            h = 4;
        } else if (len == 127) {
            if (avail < 10) break;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | (uint8_t)buf[pos + 2 + (size_t)i];
            h = 10;
        }
        if (len > (1u << 24)) return false;
        uint8_t mk[4] = {0, 0, 0, 0};
        if (masked) {
            if (avail < h + 4) break;
            std::memcpy(mk, buf.data() + pos + h, 4);
            h += 4;
        }
        if (avail < h + len) break;
        std::string payload = buf.substr(pos + h, (size_t)len);
        if (masked)
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = (char)(payload[i] ^ mk[i & 3]);
        pos += h + (size_t)len;
        if (op >= 8) {
            out->push_back({op, std::move(payload)});
        } else if (op == 0) {
            partial += payload;
            if (fin) {
                out->push_back({partial_op, partial});
                partial.clear();
                partial_op = 0;
            }
        } else if (fin) {
            out->push_back({op, std::move(payload)});
        } else {
            partial_op = op;
            partial = std::move(payload);
        }
    }
    buf.erase(0, pos);
    return true;
}


// ---- the session -------------------------------------------------------------
//
// ONE OBJECT PER CONNECTION, owned by its thread and by g_cur while current.
// Stopping one only flags it and closes its socket; nothing ever waits for its
// thread, which may be inside a DNS lookup or a connect for seconds -- the
// game thread must not (lsw2ds-pc's join freeze, adcf0aa5). A session that is
// no longer current drops whatever it reads.
enum Role { kNone = 0, kHost = 1, kJoin = 2 };

struct Session {
    int role = kNone;
    std::string server, code, token;
    int port = 0;
    std::atomic<bool> stop{false};
    std::atomic<bool> ready{false};    // host: the room is open; joiner: paired
    std::atomic<bool> done{false};     // the thread has finished
    std::atomic<int> peers{0};         // host: relayed joiners
    std::atomic<long long> fd{-1};
    std::atomic<long long> last_send_ms{0};
    std::mutex send_mu;                // the socket's writes
    uint32_t rng = 0x9E3779B9u;        // under send_mu
    std::string buf, partial;          // the reader's (its thread only)
    uint8_t partial_op = 0;
};

std::mutex g_mu;                       // g_cur, the queues, the batch, the status
std::shared_ptr<Session> g_cur;
std::deque<std::string> g_in;          // datagrams received
std::string g_batch;                   // datagrams waiting to go
std::string g_status = "off";
unsigned g_rx = 0, g_tx = 0;           // datagrams, for the counts

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch()).count();
}

std::shared_ptr<Session> current() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cur;
}

bool is_current(const Session* s) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cur.get() == s;
}

void status(const Session* s, const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (s && g_cur.get() != s) return;
        if (g_status == text) return;
        g_status = text;
    }
    std::fprintf(stderr, "[lb_relay] %s\n", text.c_str());
    std::fflush(stderr);
}

bool send_frame(Session& s, uint8_t opcode, const uint8_t* data, size_t n) {
    std::lock_guard<std::mutex> lk(s.send_mu);
    const long long fd = s.fd.load();
    if (fd < 0) return false;
    const std::string f = ws_frame(opcode, data, n, next_rand(s.rng));
    if (!send_all((socket_t)fd, f.data(), f.size())) return false;
    s.last_send_ms = now_ms();
    return true;
}

void close_socket(Session& s) {
    std::lock_guard<std::mutex> lk(s.send_mu);
    const long long fd = s.fd.exchange(-1);
    if (fd >= 0) lb_closesock((socket_t)fd);
}

// The upgrade. True with the socket in s.fd; false with a reason, and with
// `final` set when retrying cannot help (no relay, no such lobby, refused).
bool ws_open(Session& s, const std::string& path, int timeout_ms,
             std::vector<Message>* early, std::string* why, bool* final) {
    *final = false;
    const auto t0 = Clock::now();
    socket_t sock = connect_within(s.server, s.port, timeout_ms);
    if (sock == LB_INVALID) {
        *why = "cannot reach " + s.server;
        return false;
    }
    uint8_t key[16];
    {
        std::lock_guard<std::mutex> lk(s.send_mu);
        s.rng ^= (uint32_t)Clock::now().time_since_epoch().count() | 1u;
        for (uint8_t& b : key) b = (uint8_t)next_rand(s.rng);
    }
    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + s.server + (s.port == 80 ? std::string() : ":" + std::to_string(s.port)) +
           "\r\n";
    req += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + base64(key, sizeof key) + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    if (!s.token.empty()) req += "X-Token: " + s.token + "\r\n";
    req += "\r\n";
    if (!send_all(sock, req.data(), req.size())) {
        lb_closesock(sock);
        *why = "the master server closed the connection";
        return false;
    }
    std::string head;
    size_t end = std::string::npos;
    while (end == std::string::npos) {
        const int left = timeout_ms - ms_since(t0);
        if (s.stop || left <= 0 || !wait_readable(sock, std::min(left, 200))) {
            if (!s.stop && left > 200) continue;
            lb_closesock(sock);
            *why = "the master server did not answer";
            return false;
        }
        char tmp[4096];
        const int k = ::recv(sock, tmp, (int)sizeof tmp, 0);
        if (k <= 0) {
            lb_closesock(sock);
            *why = "the master server closed the connection";
            return false;
        }
        head.append(tmp, (size_t)k);
        end = head.find("\r\n\r\n");
        if (head.size() > 65536) break;
    }
    if (end == std::string::npos || head.compare(0, 12, "HTTP/1.1 101") != 0) {
        lb_closesock(sock);
        const std::string line = head.substr(0, head.find("\r\n"));
        *final = true;
        if (line.find(" 401") != std::string::npos) {
            *why = "the relay did not accept this lobby's token";
        } else if (line.find(" 404") != std::string::npos) {
            const bool absent = head.find("not on the relay") != std::string::npos;
            *why = absent ? "the host is not on the relay" : "that lobby is no longer open";
            *final = !absent;
        } else if (line.find(" 409") != std::string::npos) {
            *why = "the relay room is full";
        } else if (line.find(" 426") != std::string::npos ||
                   line.find(" 503") != std::string::npos ||
                   line.find(" 400") != std::string::npos) {
            *why = "this master server has no relay";
        } else {
            *why = "the relay refused (" + line + ")";
            *final = false;
        }
        return false;
    }
    s.buf = head.substr(end + 4);
    s.partial.clear();
    s.partial_op = 0;
    s.fd = (long long)sock;
    if (s.stop) {   // stopped while connecting
        close_socket(s);
        return false;
    }
    if (!ws_parse(s.buf, s.partial, s.partial_op, early)) {
        close_socket(s);
        *why = "the relay sent a malformed frame";
        return false;
    }
    return true;
}

// Read what is waiting; false once the connection has closed.
bool ws_receive(Session& s, std::vector<Message>* out) {
    const long long fd = s.fd.load();
    if (fd < 0) return false;
    char tmp[16384];
    const int k = ::recv((socket_t)fd, tmp, (int)sizeof tmp, 0);
    if (k <= 0) return false;
    s.buf.append(tmp, (size_t)k);
    std::vector<Message> got;
    if (!ws_parse(s.buf, s.partial, s.partial_op, &got)) return false;
    bool open = true;
    for (Message& m : got) {
        if (m.opcode == 9) {
            send_frame(s, 10, (const uint8_t*)m.data.data(), m.data.size());
            continue;
        }
        if (m.opcode == 10) continue;
        if (m.opcode == 8) open = false;
        out->push_back(std::move(m));
    }
    return open;
}

// A batch's datagrams into the receive queue -- the current session's only.
void take_batch(const Session& s, const uint8_t* p, size_t n) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_cur.get() != &s) return;
    size_t at = 0;
    while (at + 2 <= n) {
        const size_t len = (size_t)p[at] | ((size_t)p[at + 1] << 8);
        at += 2;
        if (at + len > n) break;
        if (g_in.size() >= kInQueue) g_in.pop_front();
        g_in.emplace_back((const char*)p + at, len);
        at += len;
        ++g_rx;
    }
}

std::string relayed_text(int n) {
    return n ? "hosting through the relay too: " + std::to_string(n) + " player(s) relayed"
             : "hosting; the relay is open for players who cannot connect directly";
}

void handle(Session& s, const Message& m) {
    if (m.opcode == 1) {
        if (s.role == kJoin && m.data == "ready") {
            s.ready = true;
            status(&s, "joined through the relay (lobby " + s.code + ")");
        }
        return;
    }
    if (m.opcode != 2) return;
    const uint8_t* p = (const uint8_t*)m.data.data();
    const size_t n = m.data.size();
    if (s.role == kJoin) {
        take_batch(s, p, n);
        return;
    }
    if (n < 2) return;
    if (p[0] == 0) {
        take_batch(s, p + 2, n - 2);
    } else if (p[0] == 1) {
        status(&s, relayed_text(++s.peers));
    } else if (p[0] == 2) {
        if (s.peers > 0) --s.peers;
        status(&s, relayed_text(s.peers));
    }
}

// Read until closed or stopped, keeping the connection alive meanwhile.
void pump(Session& s) {
    while (!s.stop) {
        const long long fd = s.fd.load();
        if (fd < 0) return;
        if (now_ms() - s.last_send_ms.load() > kKeepaliveMs)
            send_frame(s, 1, (const uint8_t*)"ping", 4);
        if (!wait_readable((socket_t)fd, 200)) continue;
        std::vector<Message> got;
        const bool open = ws_receive(s, &got);
        for (const Message& m : got) handle(s, m);
        if (!open) return;
    }
}

void nap(Session& s, int ms) {
    for (int w = 0; w < ms && !s.stop; w += 100)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void run_host(std::shared_ptr<Session> sp) {
    Session& s = *sp;
    int backoff = 1000;
    while (!s.stop) {
        std::vector<Message> early;
        std::string why;
        bool final = false;
        if (!ws_open(s, "/relay/" + s.code + "?role=host", 5000, &early, &why, &final)) {
            if (s.stop) break;
            status(&s, "relay: " + why + (final ? "" : "; retrying"));
            if (final) break;
            nap(s, backoff);
            backoff = std::min(backoff * 2, 30000);
            continue;
        }
        backoff = 1000;
        s.peers = 0;
        s.last_send_ms = now_ms();
        s.ready = true;
        status(&s, relayed_text(0));
        for (const Message& m : early) handle(s, m);
        pump(s);
        s.ready = false;
        s.peers = 0;
        close_socket(s);
        if (!s.stop) status(&s, "relay: the connection closed; reopening");
    }
    close_socket(s);
    s.done = true;
}

void run_join(std::shared_ptr<Session> sp) {
    Session& s = *sp;
    std::vector<Message> early;
    std::string why;
    bool final = false;
    status(&s, "trying the relay for lobby " + s.code);
    // The host may open its room a moment after listing: a few tries.
    for (int attempt = 0; attempt < 4 && !s.stop; ++attempt) {
        early.clear();
        if (ws_open(s, "/relay/" + s.code + "?role=join", 5000, &early, &why, &final)) break;
        if (s.stop) break;
        status(&s, "relay: " + why);
        if (final) break;
        nap(s, 1500);
    }
    if (s.fd.load() >= 0) {
        s.last_send_ms = now_ms();
        for (const Message& m : early) handle(s, m);
        pump(s);
        const bool was = s.ready.exchange(false);
        if (!s.stop)
            status(&s, was ? "relay: the host's room closed" : "relay: could not pair with the host");
    }
    close_socket(s);
    s.done = true;   // gone or never paired: the direct route is this player's again
}

bool forced() {
    static const int f = [] {
        const char* v = std::getenv("LB_TEST_FORCE_RELAY");
        return v && *v && *v != '0' ? 1 : 0;
    }();
    return f != 0;
}

// Flag the current session to stop and let it go; never waits.
void drop_current() {
    std::shared_ptr<Session> old;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        old.swap(g_cur);
        g_in.clear();
        g_batch.clear();
    }
    if (!old) return;
    old->stop = true;
    old->ready = false;
    close_socket(*old);
}

void start(int role, const char* server, int port, const char* code, const char* token) {
    drop_current();
    auto s = std::make_shared<Session>();
    s->role = role;
    s->server = server;
    s->port = port;
    s->code = code;
    s->token = token ? token : "";
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_cur = s;
    }
    std::thread(role == kHost ? run_host : run_join, s).detach();
}

// The current session's role, or none once its thread has given up.
int live_role(const std::shared_ptr<Session>& s) {
    return s && !s->done.load() ? s->role : kNone;
}

}  // namespace

// ---- lb_net.cpp's side -------------------------------------------------------
extern "C" void lb_relay_stop(void) {
    if (!current()) return;
    drop_current();
    status(nullptr, "off");
}

extern "C" void lb_relay_host_start(const char* server, int port, const char* code,
                                    const char* token) {
    if (!server || !*server || !code || !*code || !token || !*token) return;
    const auto s = current();
    if (live_role(s) == kHost && s->code == code && s->server == server) return;
    start(kHost, server, port, code, token);
}

extern "C" void lb_relay_join_start(const char* server, int port, const char* code) {
    if (!server || !*server || !code || !*code) return;
    const auto s = current();
    if (live_role(s) == kJoin && s->code == code && s->server == server) return;
    start(kJoin, server, port, code, nullptr);
}

extern "C" int lb_relay_role(void) { return live_role(current()); }
extern "C" int lb_relay_forced(void) { return forced() ? 1 : 0; }

extern "C" int lb_relay_status(char* out, int cap) {
    std::lock_guard<std::mutex> lk(g_mu);
    return std::snprintf(out, (size_t)cap, "%s", g_status.c_str());
}

// ---- the transport's side (wifi_net.cpp, stanza -424) ------------------------

// Whether datagrams go to the relay: a host with relayed joiners, or a
// paired joiner.
extern "C" int lb_relay_active(void) {
    const auto s = current();
    if (!s || !s->ready.load()) return 0;
    return s->role == kJoin || s->peers.load() > 0 ? 1 : 0;
}

// Whether the transport should leave its direct route alone: a joiner on the
// relay (or trying it), so nothing arrives twice, and either side under
// LB_TEST_FORCE_RELAY.
extern "C" int lb_relay_direct_off(void) {
    if (forced()) return 1;
    return live_role(current()) == kJoin ? 1 : 0;
}

namespace {
void send_batch(std::string batch) {
    const auto s = current();
    if (!s || !lb_relay_active()) return;
    std::string msg;
    if (s->role == kHost) msg.assign("\x00\xFF", 2);
    msg += batch;
    send_frame(*s, 2, (const uint8_t*)msg.data(), msg.size());
}
}  // namespace

extern "C" void lb_relay_send(const unsigned char* d, int n) {
    if (n <= 0 || n > 0xFFFF || !lb_relay_active()) return;
    std::string ready;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_batch.push_back((char)(n & 0xFF));
        g_batch.push_back((char)(n >> 8));
        g_batch.append((const char*)d, (size_t)n);
        ++g_tx;
        if (g_batch.size() < kBatchBytes) return;
        ready.swap(g_batch);
    }
    send_batch(std::move(ready));
}

// Send what has gathered: once per drain of the transport.
extern "C" void lb_relay_flush(void) {
    std::string ready;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_batch.empty()) return;
        ready.swap(g_batch);
    }
    send_batch(std::move(ready));
}

extern "C" int lb_relay_recv(unsigned char* buf, int cap) {
    std::lock_guard<std::mutex> lk(g_mu);
    while (!g_in.empty()) {
        std::string d = std::move(g_in.front());
        g_in.pop_front();
        if ((int)d.size() > cap) continue;
        std::memcpy(buf, d.data(), d.size());
        return (int)d.size();
    }
    return 0;
}

extern "C" void lb_relay_counts(unsigned* rx, unsigned* tx) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (rx) *rx = g_rx;
    if (tx) *tx = g_tx;
}
