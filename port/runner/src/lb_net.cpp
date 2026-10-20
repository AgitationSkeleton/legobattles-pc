// lb_net.cpp - the master-server client for the multiplayer tab.
//
// Part of legobattles-decomp. Not an upstream ndsrecomp file; see
// port/runner/README.md.
//
// SCOPE, stated plainly so the UI does not overclaim. This talks to the master
// server in services/master-server: it lists lobbies, announces one, and looks
// one up by code.
//
// It does NOT speak the game's own protocol and does not need to. docs/216
// showed two instances reaching a live two-player battle over the runner's
// local-wireless bridge, with the cartridge doing its own discovery, joining
// and lobby entirely by itself. The only thing that bridge could not do was
// reach another MACHINE, because it sent to 127.0.0.1 and nowhere else.
//
// So "joining" here is one address. A client looks the room code up, gets the
// host's address and UDP port, hands them to the bridge, and then uses the
// GAME's own Multiplayer > Join screen -- which now sees the remote host
// exactly as it saw a local one. Hosting is the mirror: advertise the port
// being listened on and learn the client's address from its first datagram.
// No game traffic passes through the master server, which is why its own
// README calls it a directory rather than a relay.
//
// AND NOW WITHOUT A BUTTON. lb_patches.cpp
// reports the cartridge's own network state every VBlank (lb_net_guest_update;
// the note there says where each state is set). lb_net_auto_tick turns that
// into the two things the game's own screens are expected to do:
//
//   * the game opens a Host lobby  -> the lobby is advertised, and withdrawn
//     when the host presses Start or leaves;
//   * the game's Join screen opens -> the lobby list is fetched every few
//     seconds and the newest listed host becomes the bridge's peer, so the
//     cartridge's own "Join Game" list shows it the way it shows a DS in the
//     same room. The bridge holds ONE peer, so one host at a time: a lobby
//     picked with a row's Join button (or Join by code) wins over the newest.
//
// Requests run on a worker thread. A frame must never block on a socket: the UI
// asks for a refresh, keeps drawing, and picks the result up when it lands.

#include "lb_overlay.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
// docs/405: UPnP, through miniupnpc (BSD-3-Clause), linked statically.
#define MINIUPNP_STATICLIB 1
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#define SOCKET int
#define INVALID_SOCKET (-1)
#endif

namespace {

std::mutex g_mutex;
std::vector<LbLobby> g_lobbies;
// Parallel to g_lobbies: each lobby's address resolved to IPv4 (network byte
// order) on the worker thread, INADDR_NONE when it cannot be. Resolving there
// rather than at Join keeps a DNS lookup off the frame.
std::vector<uint32_t> g_lobby_ip;
// docs/419: parallel too: the master server that listed each lobby, which a
// Join introduces itself through. With "Both" the list holds two servers'.
std::vector<std::pair<std::string, int>> g_lobby_srv;
// docs/419: the second server "Both" lists on and browses, or empty.
std::string g_extra_server;   // under g_mutex
int g_extra_port = 0;         // under g_mutex
std::string g_status = "Not queried yet.";
std::atomic<bool> g_busy{false};
// Bumped every time a list lands, so the automatic join can tell a new list
// from the one it already applied.
std::atomic<unsigned> g_list_gen{0};
// The peer a PERSON chose (Join by code, a row's Join, LB_MP_PEER). The
// automatic join leaves it alone, and puts it back when the game's Join screen
// closes.
uint32_t g_manual_ip = 0u;       // network byte order, under g_mutex
uint16_t g_manual_port = 0u;     // under g_mutex
// The room code of the lobby the bridge was last pointed at, so a joiner can
// tell the master server it is in that lobby (and leaving it). Under g_mutex.
std::string g_peer_code;
// docs/424: the server that listed that lobby, whose relay can reach it.
std::string g_peer_server;
int g_peer_sport = 0;

// Winsock is already started by the runner before boot() -- it prints
// "[wifi_net] Winsock initialized (WSAStartup 2.2)" -- so there is deliberately
// no WSAStartup here. Calling it again would be harmless but claiming ownership
// of somebody else's initialisation is not.

}  // namespace
// docs/419: HTTPS first, when the setting is on (lb_https.cpp, WinHTTP): port
// 80 becomes 443, any other port is tried as it is. A server without TLS
// leaves the plain request below to go ahead, and the log says so once.
extern "C" int lb_setting_master_https(void);   /* lb_overlay.cpp */
bool lb_https_request(const std::string& host, int port, const std::string& method,
                      const std::string& path, const std::string& body,
                      std::string* reply, std::string* error);
namespace {
std::string http_request(const std::string& host, int port,
                         const std::string& method, const std::string& path,
                         const std::string& body, std::string* error) {
    if (lb_setting_master_https()) {
        std::string reply, herr;
        if (lb_https_request(host, port == 80 ? 443 : port, method, path, body, &reply,
                             &herr)) {
            static std::mutex ok_mu;
            static std::string ok_said;
            std::lock_guard<std::mutex> lk(ok_mu);
            if (ok_said != host) {
                ok_said = host;
                std::fprintf(stderr, "[lb_net] HTTPS to %s\n", host.c_str());
            }
            return reply;
        }
        static std::mutex said_mu;
        static std::string said;
        std::lock_guard<std::mutex> lk(said_mu);
        if (said != host) {
            said = host;
            std::fprintf(stderr, "[lb_net] HTTPS to %s failed (%s); plain HTTP "
                         "instead\n", host.c_str(), herr.c_str());
        }
    }
    char port_text[16];
    std::snprintf(port_text, sizeof(port_text), "%d", port);

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* info = nullptr;
    if (getaddrinfo(host.c_str(), port_text, &hints, &info) != 0 || !info) {
        *error = "cannot resolve " + host;
        return {};
    }

    SOCKET sock = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(info);
        *error = "socket() failed";
        return {};
    }
    if (connect(sock, info->ai_addr, (int)info->ai_addrlen) != 0) {
        freeaddrinfo(info);
        closesocket(sock);
        *error = "cannot reach " + host + ":" + port_text;
        return {};
    }
    freeaddrinfo(info);

    std::string request = method + " " + path + " HTTP/1.1\r\nHost: " + host +
                          "\r\nConnection: close\r\n";
    if (!body.empty()) {
        request += "Content-Type: application/json\r\nContent-Length: " +
                   std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n" + body;

    if (send(sock, request.data(), (int)request.size(), 0) < 0) {
        closesocket(sock);
        *error = "send failed";
        return {};
    }

    std::string reply;
    char buffer[2048];
    for (;;) {
        const int got = recv(sock, buffer, (int)sizeof(buffer), 0);
        if (got <= 0) break;
        reply.append(buffer, (size_t)got);
        if (reply.size() > (1u << 20)) break;   // a lobby list is never 1 MB
    }
    closesocket(sock);

    const size_t split = reply.find("\r\n\r\n");
    if (split == std::string::npos) {
        *error = "malformed reply";
        return {};
    }
    // A CHUNKED REPLY, decoded. The Cloudflare server (services/master-server-
    // cloudflare) sends Content-Length today, but a proxy is free to re-frame
    // a reply as chunks, and the JSON readers below would then see the chunk
    // sizes as text.
    std::string head = reply.substr(0, split);
    for (char& c : head) c = (char)std::tolower((unsigned char)c);
    std::string payload = reply.substr(split + 4);
    if (head.find("transfer-encoding: chunked") != std::string::npos) {
        std::string out;
        size_t at = 0;
        for (;;) {
            const size_t eol = payload.find("\r\n", at);
            if (eol == std::string::npos) break;
            const unsigned long n = std::strtoul(payload.c_str() + at, nullptr, 16);
            if (n == 0 || eol + 2 + n > payload.size()) break;
            out.append(payload, eol + 2, n);
            at = eol + 2 + n + 2;
        }
        payload.swap(out);
    }
    return payload;
}

// A DELIBERATELY MINIMAL extractor, not a JSON parser. It reads exactly the
// fields services/master-server/README.md documents, from replies this port's
// own server produces. It would not survive a hostile or restructured
// response, and a real client should carry a parser -- but shipping one here to
// read six known keys would be more code than the feature.
std::string field(const std::string& src, size_t from, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    size_t at = src.find(needle, from);
    if (at == std::string::npos) return {};
    at += needle.size();
    while (at < src.size() && (src[at] == ' ')) ++at;
    if (at >= src.size()) return {};
    if (src[at] == '"') {
        const size_t end = src.find('"', ++at);
        return end == std::string::npos ? std::string() : src.substr(at, end - at);
    }
    size_t end = at;
    while (end < src.size() && src[end] != ',' && src[end] != '}') ++end;
    return src.substr(at, end - at);
}

// The index of the brace or bracket that closes the one at `open`, skipping
// strings, or npos.
size_t close_of(const std::string& s, size_t open) {
    int depth = 0;
    bool in_string = false;
    for (size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (in_string) {
            if (c == '\\') ++i;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

// One lobby object with everything NESTED in it blanked out, so `field` can
// only find the lobby's own keys. The server's lobby carries "options" (an
// object) and "players_list" (objects with their own "name" and "address"):
// searched unbounded, as this used to be, every players_list entry became a
// phantom lobby row -- measured against the real server, a lobby announced
// with two players listed produced three rows, two of them at port 0.
std::string top_level(const std::string& obj) {
    std::string out = obj;
    int depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        const bool nested = depth > 1;
        if (in_string) {
            if (c == '\\' && i + 1 < out.size()) {
                if (nested) out[i] = out[i + 1] = ' ';
                ++i;
                continue;
            }
            if (c == '"') in_string = false;
            if (nested) out[i] = ' ';
            continue;
        }
        if (c == '"') {
            in_string = true;
            if (nested) out[i] = ' ';
            continue;
        }
        if (c == '{' || c == '[') {
            ++depth;
            if (depth > 1) out[i] = ' ';
            continue;
        }
        if (c == '}' || c == ']') {
            if (depth > 1) out[i] = ' ';
            --depth;
            continue;
        }
        if (nested) out[i] = ' ';
    }
    return out;
}

// A JSON string literal. The announce body used to splice names in raw, so a
// name with a quote or a backslash -- which the Player name field accepts --
// produced a body the server refused outright ("400 bad or oversized body").
std::string json_str(const char* text) {
    std::string out = "\"";
    for (const char* p = text ? text : ""; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c < 0x20) {
            char esc[8];
            std::snprintf(esc, sizeof esc, "\\u%04x", (unsigned)c);
            out += esc;
        } else {
            out += (char)c;   // UTF-8 passes through; the server reads UTF-8
        }
    }
    return out + "\"";
}

// IPv4 for a dotted quad or a name, network byte order, or INADDR_NONE.
uint32_t resolve_ipv4(const char* host) {
    if (!host || !*host) return INADDR_NONE;
    unsigned long ip = inet_addr(host);
    if (ip != INADDR_NONE) return (uint32_t)ip;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, nullptr, &hints, &res) == 0 && res) {
        ip = ((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
        return (uint32_t)ip;
    }
    return INADDR_NONE;
}

bool is_loopback(uint32_t ip_be) { return (ntohl(ip_be) >> 24) == 127u; }

// Where to send to reach a lobby's host. The master server records the
// address it SAW the host announce from, so this is normally a dotted quad; a
// name means the host published one by hand, which the server allows.
//
// LOOPBACK MEANS "WHERE THE MASTER SERVER IS". The record is
// `"address": body.get("address") or self.peer` (master_server.py), so a host
// on the master server's own machine -- e.g. the server run from the
// build folder -- announces over loopback and is listed as 127.0.0.1. To a
// client on ANOTHER machine that is itself. That host is reachable at the
// master server's own address, which the client already knows. A client on
// the master server's machine keeps 127.0.0.1, which is right for it.
//
// docs/400: A PRIVATE ADDRESS MEANS THE SAME. A server run on a home network
// sees a host on that network arrive through the router, and lists it as the
// router's address (192.168.1.1 on 2026-10-18), which nobody outside can
// use. When the server itself is public, that host is behind the server's
// own address. A server on a private address is a LAN server, and its
// private listings are right for its LAN.
bool is_private_ip(uint32_t ip_be) {
    const uint32_t a = ntohl(ip_be);
    return (a >> 24) == 10u || (a >> 20) == 0xAC1u || (a >> 16) == 0xC0A8u ||
           (a >> 22) == 0x191u || (a >> 16) == 0xA9FEu;
}
std::string local_ipv4();   // docs/400, below
uint32_t lobby_ip(const char* address, const std::string& server) {
    const uint32_t ip = resolve_ipv4(address);
    // docs/400: a host on THIS machine, listed by its LAN pair (a second copy
    // behind the same router), is reached over loopback, where the bridge
    // knows it for a sibling. By the LAN address every frame would arrive
    // twice: once from the sibling sweep and once from the peer.
    if (ip != INADDR_NONE && is_private_ip(ip)) {
        const std::string me = local_ipv4();
        if (!me.empty() && resolve_ipv4(me.c_str()) == ip) return htonl(INADDR_LOOPBACK);
    }
    if (ip == INADDR_NONE || !(is_loopback(ip) || is_private_ip(ip))) return ip;
    const uint32_t via = resolve_ipv4(server.c_str());
    if (via != INADDR_NONE && !is_loopback(via) && !is_private_ip(via)) return via;
    return ip;
}

// "a.b.c.d" for a network-order address, for status lines that must say
// where the bridge was really pointed, not what the listing said.
std::string ip_text(uint32_t ip_be) {
    char text[20];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u",
                  (unsigned)(ip_be & 0xFFu), (unsigned)((ip_be >> 8) & 0xFFu),
                  (unsigned)((ip_be >> 16) & 0xFFu),
                  (unsigned)((ip_be >> 24) & 0xFFu));
    return text;
}

// The status line after pointing the bridge somewhere.
std::string peer_set_text(uint32_t ip_be, int port, const char* listed) {
    const std::string real = ip_text(ip_be);
    std::string out = "Peer set to " + real + ":" + std::to_string(port);
    if (listed && real != listed)
        out += std::string(" (listed as ") + listed + ")";
    return out + " -- now use the game's Multiplayer > Join screen";
}

void parse_lobbies(const std::string& json, const std::string& server, int sport = 0,
                   bool append = false) {
    std::vector<LbLobby> out;
    std::vector<uint32_t> ips;
    size_t at = json.find("\"lobbies\"");
    if (at == std::string::npos) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = "reply had no lobby list";
        return;
    }
    at = json.find('[', at);
    const size_t list_end =
        at == std::string::npos ? std::string::npos : close_of(json, at);
    if (list_end == std::string::npos) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = "reply had no lobby list";
        return;
    }
    for (size_t i = at + 1; i < list_end; ++i) {
        if (json[i] != '{') continue;
        const size_t end = close_of(json, i);
        if (end == std::string::npos || end > list_end) break;
        const std::string obj = top_level(json.substr(i, end - i + 1));
        i = end;

        LbLobby lobby{};
        std::snprintf(lobby.code, sizeof(lobby.code), "%s",
                      field(obj, 0, "code").c_str());
        std::snprintf(lobby.name, sizeof(lobby.name), "%s",
                      field(obj, 0, "name").c_str());
        std::snprintf(lobby.host_name, sizeof(lobby.host_name), "%s",
                      field(obj, 0, "host_name").c_str());
        std::snprintf(lobby.map_name, sizeof(lobby.map_name), "%s",
                      field(obj, 0, "map_name").c_str());
        lobby.players = std::atoi(field(obj, 0, "players").c_str());
        lobby.max_players = std::atoi(field(obj, 0, "max_players").c_str());
        // Where the host actually is. Without these a listing is something to
        // look at rather than something to join.
        std::snprintf(lobby.address, sizeof(lobby.address), "%s",
                      field(obj, 0, "address").c_str());
        lobby.port = std::atoi(field(obj, 0, "port").c_str());
        out.push_back(lobby);
        ips.push_back(lobby_ip(lobby.address, server));
        if (out.size() >= 64) break;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (append) {   // docs/419: the second server's, after the first's
        for (size_t k = 0; k < out.size(); ++k) {
            bool dup = false;   // one lobby listed on both is shown once
            for (size_t j = 0; j < g_lobbies.size(); ++j)
                if (j < g_lobby_ip.size() && g_lobby_ip[j] == ips[k] &&
                    g_lobbies[j].port == out[k].port)
                    dup = true;
            if (dup) continue;
            g_lobbies.push_back(out[k]);
            g_lobby_ip.push_back(ips[k]);
            g_lobby_srv.push_back({server, sport});
        }
    } else {
        g_lobbies.swap(out);
        g_lobby_ip.swap(ips);
        g_lobby_srv.assign(g_lobbies.size(), {server, sport});
    }
    g_status = g_lobbies.empty() ? "No lobbies advertised."
                                 : std::to_string(g_lobbies.size()) + " lobby(ies).";
    g_list_gen.fetch_add(1u);
}

}  // namespace

/* The runner's local-wireless bridge (runner/src/wifi_net.cpp, extended by
   port/runner/apply_hooks.py section 5d). Declared here rather than
   included because wifi_net.h drags melonDS headers in. */
extern "C" void nds_local_wireless_set_peer(uint32_t ip_be, uint16_t port);
extern "C" void nds_local_wireless_set_listen(int on);
extern "C" int  nds_local_wireless_peer(uint32_t* ip_be, uint16_t* port,
                                        int* learned);
extern "C" uint16_t nds_local_wireless_port(void);
/* docs/400: NAT traversal (apply_hooks.py 0zzzzzzzzzzzzz-400). */
extern "C" void nds_local_wireless_stun(uint32_t ip_be, uint16_t port);
extern "C" int  nds_local_wireless_public(uint32_t* ip_be, uint16_t* port);
extern "C" void nds_local_wireless_punch(uint32_t ip_be, uint16_t port, int ms);
extern "C" void nds_local_wireless_stats(unsigned* sent, unsigned* recvd);

// ── docs/400: REACHING A HOST BEHIND A ROUTER ─────────────────────────────
//
// The directory alone could not do it. A joiner sent to the listed host, and
// the host's router dropped the datagram: the router had never seen the host
// send anywhere, so nothing told it where the datagram should go. Every
// hosting session of 2026-10-18 logged `frames sent=1800 recv=0`.
//
// So both sides now:
//   * learn their PUBLIC UDP mapping -- the bridge sends a STUN binding
//     request from its own socket, and the answer says what address and port
//     the router gave that socket;
//   * advertise it: the host in /announce, the joiner in /punch;
//   * and send to each other. A joiner already says hello to its peer every
//     250 ms; a host, told about a joiner by /poll, says hello back for 15 s.
// The first datagram out of each router opens it for the other's, and the
// bridge's existing learning takes over.
//
// Two routers that hand every destination a different port (symmetric NAT,
// some carrier-grade NAT) defeat this; they need a relay (docs/399, 0b).
namespace {
// The STUN servers, resolved off the frame. The bridge asks again by itself
// whenever its socket is reopened, so one resolution serves the session; the
// second server is tried only if the first never answered.
std::atomic<bool> g_stun_asked{false};
void request_stun() {
    // docs/424: LB_NO_STUN=1, a test aid -- a host advertises the address the
    // server sees, so two instances on one machine reach each other directly.
    static const bool off = std::getenv("LB_NO_STUN") != nullptr;
    if (off) return;
    if (g_stun_asked.exchange(true)) return;
    std::thread([]() {
        static const struct { const char* host; uint16_t port; } kStun[] = {
            {"stun.l.google.com", 19302}, {"stun.cloudflare.com", 3478}};
        for (const auto& s : kStun) {
            const uint32_t ip = resolve_ipv4(s.host);
            if (ip == INADDR_NONE) continue;
            nds_local_wireless_stun(ip, s.port);
            for (int i = 0; i < 40; ++i) {
                if (nds_local_wireless_public(nullptr, nullptr)) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::fprintf(stderr, "[lb_net] no STUN answer from %s\n", s.host);
        }
        g_stun_asked = false;   // let a later host or join try again
    }).detach();
}

bool wait_public(int ms, uint32_t* ip, uint16_t* port) {
    for (int waited = 0;; waited += 100) {
        if (nds_local_wireless_public(ip, port)) return true;
        if (waited >= ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// This machine's address on its own network: the source address a datagram
// to the internet would leave from. Nothing is sent.
std::string local_ipv4() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return {};
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = inet_addr("8.8.8.8");
    std::string out;
    if (connect(s, (sockaddr*)&to, sizeof to) == 0) {
        sockaddr_in me{};
        socklen_t len = sizeof me;
        if (getsockname(s, (sockaddr*)&me, &len) == 0)
            out = ip_text(me.sin_addr.s_addr);
    }
    closesocket(s);
    return out;
}

// The server the lobby list came from, for the joins that start from it.
std::string g_list_server;   // under g_mutex
int g_list_port = 0;         // under g_mutex

// "I want to join CODE; here is my public mapping." Sent by a joiner each
// time it points the bridge at a lobby; the host collects it with /poll.
void post_punch(const std::string& server, int port, const std::string& code) {
    if (server.empty() || code.empty()) return;
    request_stun();
    const unsigned short local = nds_local_wireless_port();
    std::thread([server, port, code, local]() {
        uint32_t pip = 0u;
        uint16_t pport = 0u;
        if (!wait_public(3000, &pip, &pport)) {
            std::fprintf(stderr, "[lb_net] join %s: no public address (STUN "
                         "did not answer); the host cannot be introduced\n",
                         code.c_str());
            return;
        }
        const std::string body =
            "{\"code\":" + json_str(code.c_str()) +
            ",\"address\":" + json_str(ip_text(pip).c_str()) +
            ",\"port\":" + std::to_string(pport) +
            ",\"lan_address\":" + json_str(local_ipv4().c_str()) +
            ",\"lan_port\":" + std::to_string(local) + "}";
        std::string error;
        const std::string reply =
            http_request(server, port, "POST", "/punch", body, &error);
        std::fprintf(stderr, "[lb_net] join %s: introduced as %s:%u (%s)\n",
                     code.c_str(), ip_text(pip).c_str(), (unsigned)pport,
                     reply.find("\"ok\":true") != std::string::npos
                         ? "the host will answer"
                         : (error.empty() ? "the server refused" : error.c_str()));
    }).detach();
}
}  // namespace

// ── docs/405: UPnP -- ASKING THE ROUTER TO FORWARD THE PORT ────────────────
//
// Hole punching (above) opens a router by sending through it, and fails when
// the router gives every destination a different port. A router that speaks
// UPnP can simply be asked: "forward UDP <port> to this machine". When the
// game hosts, and the setting is on (it is by default), the host asks before
// it announces, and advertises the router's public address with that port.
// The mapping is removed when the lobby closes and when the program exits;
// its lease (an hour) covers a program that is killed.
//
// miniupnpc (BSD-3-Clause, miniupnp.free.fr) does the protocol: SSDP
// discovery, the IGD's description, and the SOAP calls.
extern "C" int lb_setting_upnp(void);   /* lb_overlay.cpp */
namespace {
std::mutex g_upnp_mutex;
std::string g_upnp_text;        // under g_upnp_mutex
uint16_t g_upnp_port = 0;       // the port forwarded now, or 0
std::string g_upnp_wan;         // the router's public address
#ifdef _WIN32
UPNPUrls g_upnp_urls{};
IGDdatas g_upnp_data{};
#endif

void upnp_status(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_upnp_mutex);
    g_upnp_text = s;
}

void upnp_unmap() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_upnp_mutex);
    if (!g_upnp_port) return;
    char p[8];
    std::snprintf(p, sizeof p, "%u", (unsigned)g_upnp_port);
    const int e = UPNP_DeletePortMapping(g_upnp_urls.controlURL,
                                         g_upnp_data.first.servicetype, p,
                                         "UDP", nullptr);
    FreeUPNPUrls(&g_upnp_urls);
    std::fprintf(stderr, "[lb_upnp] UDP %s mapping removed (%s)\n", p,
                 e == UPNPCOMMAND_SUCCESS ? "ok" : strupnperror(e));
    g_upnp_port = 0;
    g_upnp_text = "The router no longer forwards the port.";
#endif
}

// On a worker thread: true when the router forwards `port` to this machine,
// with its public address in *wan.
bool upnp_map(uint16_t port, std::string* wan) {
#ifdef _WIN32
    if (!lb_setting_upnp()) {
        upnp_status("UPnP is off.");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_upnp_mutex);
        if (g_upnp_port == port && !g_upnp_wan.empty()) {
            *wan = g_upnp_wan;
            return true;
        }
    }
    upnp_status("Asking the router to forward the port (UPnP)...");
    int err = 0;
    UPNPDev* devs = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY,
                                 0, 2, &err);
    if (!devs) {
        upnp_status("No router answered UPnP (it may be switched off in the "
                    "router's settings).");
        std::fprintf(stderr, "[lb_upnp] no UPnP device answered (error %d)\n", err);
        return false;
    }
    UPNPUrls urls{};
    IGDdatas data{};
    char lan[64] = "", wanaddr[64] = "";
    const int r = UPNP_GetValidIGD(devs, &urls, &data, lan, (int)sizeof lan,
                                   wanaddr, (int)sizeof wanaddr);
    freeUPNPDevlist(devs);
    if (r != UPNP_CONNECTED_IGD) {
        if (r != UPNP_NO_IGD) FreeUPNPUrls(&urls);
        upnp_status(r == UPNP_PRIVATEIP_IGD
                        ? "The router is itself behind another router (its "
                          "internet address is private), so UPnP cannot open it."
                        : "No connected UPnP router was found.");
        std::fprintf(stderr, "[lb_upnp] no usable gateway (result %d)\n", r);
        return false;
    }
    char p[8];
    std::snprintf(p, sizeof p, "%u", (unsigned)port);
    int e = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, p, p,
                                lan, "LEGO Battles", "UDP", nullptr, "3600");
    if (e == 725)   // OnlyPermanentLeasesSupported
        e = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, p, p,
                                lan, "LEGO Battles", "UDP", nullptr, "0");
    if (e != UPNPCOMMAND_SUCCESS) {
        FreeUPNPUrls(&urls);
        upnp_status(std::string("The router refused to forward the port (") +
                    strupnperror(e) + ").");
        std::fprintf(stderr, "[lb_upnp] UDP %s refused: %s (%d)\n", p,
                     strupnperror(e), e);
        return false;
    }
    char ext[40] = "";
    UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, ext);
    {
        std::lock_guard<std::mutex> lock(g_upnp_mutex);
        if (g_upnp_port) FreeUPNPUrls(&g_upnp_urls);
        g_upnp_urls = urls;
        g_upnp_data = data;
        g_upnp_port = port;
        g_upnp_wan = ext;
        g_upnp_text = std::string("The router forwards UDP ") + p + " to this "
                      "machine (UPnP)" + (ext[0] ? std::string(", at ") + ext : "") + ".";
    }
    static bool registered = false;
    if (!registered) {
        registered = true;
        std::atexit(upnp_unmap);
    }
    std::fprintf(stderr, "[lb_upnp] the router forwards UDP %s to %s (public %s)\n",
                 p, lan, ext[0] ? ext : "unknown");
    *wan = ext;
    return true;
#else
    (void)port;
    (void)wan;
    return false;
#endif
}
}  // namespace

extern "C" const char* lb_net_upnp_text(void) {
    static std::string copy;
    std::lock_guard<std::mutex> lock(g_upnp_mutex);
    copy = g_upnp_text;
    return copy.c_str();
}

extern "C" int lb_net_public_text(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    uint32_t ip = 0u;
    uint16_t port = 0u;
    if (!nds_local_wireless_public(&ip, &port)) {
        out[0] = 0;
        return 0;
    }
    std::snprintf(out, (size_t)cap, "%s:%u", ip_text(ip).c_str(), (unsigned)port);
    return 1;
}

extern "C" void lb_net_link_stats(unsigned* sent, unsigned* recvd) {
    nds_local_wireless_stats(sent, recvd);
}

namespace {
void remember_manual_peer(uint32_t ip_be, uint16_t port) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_manual_ip = ip_be;
    g_manual_port = port;
}

// A Join by code that resolved, waiting for the frame thread.
// The lookup runs on a worker, and the bridge's set_peer reopens its socket
// when the peer turns it from loopback to INADDR_ANY -- on the calling thread,
// while the emulation thread may be inside sendto/recvfrom on that socket. So
// the worker only records the answer; lb_net_auto_tick, which runs on the
// emulation's own thread, hands it to the bridge.
bool g_join_pending = false;       // under g_mutex
uint32_t g_join_ip = 0u;           // under g_mutex
uint16_t g_join_port = 0u;         // under g_mutex
}  // namespace

extern "C" void lb_net_refresh_lobbies(const char* host, int port) {
    if (g_busy.exchange(true)) return;   // one request in flight is enough
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = "Querying...";
    }
    std::string h = host ? host : "";
    std::string xh;
    int xp = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        xh = g_extra_server;
        xp = g_extra_port;
    }
    std::thread([h, port, xh, xp]() {
        std::string error;
        const std::string body = http_request(h, port, "GET", "/list", "", &error);
        if (body.empty()) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_lobbies.clear();
            g_lobby_ip.clear();
            g_lobby_srv.clear();
            g_status = error.empty() ? "empty reply" : error;
        } else {
            parse_lobbies(body, h, port);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_list_server = h;     // docs/400: a row's Join introduces itself here
            g_list_port = port;
        }
        // docs/419: "Both" -- the second server's lobbies after the first's.
        if (!xh.empty() && !(xh == h && xp == port)) {
            std::string xerr;
            const std::string xbody = http_request(xh, xp, "GET", "/list", "", &xerr);
            if (!xbody.empty()) parse_lobbies(xbody, xh, xp, true);
        }
        g_busy = false;
    }).detach();
}

extern "C" int lb_net_lobby_count(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return (int)g_lobbies.size();
}

extern "C" int lb_net_lobby_at(int index, LbLobby* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!out || index < 0 || index >= (int)g_lobbies.size()) return 0;
    *out = g_lobbies[(size_t)index];
    return 1;
}

extern "C" const char* lb_net_status(void) {
    static std::string copy;
    std::lock_guard<std::mutex> lock(g_mutex);
    copy = g_status;
    return copy.c_str();
}

extern "C" int lb_net_busy(void) { return g_busy.load() ? 1 : 0; }

// ── Hosting ────────────────────────────────────────────────────────────────
//
// Announcing puts a lobby on the master server so other players can SEE it. It
// does not make the game joinable: nothing yet speaks the wire protocol in
// docs/87-89. The UI says so rather than letting a working button imply a
// working game.
//
// The server expires a lobby after 90 seconds (services/master-server), so a
// heartbeat has to keep it alive. That is driven from the frame loop rather
// than a timer thread: the overlay is already called every frame and one more
// clock check there is cheaper than another thread to reason about.

namespace {
std::mutex g_host_mutex;
std::string g_host_code;
std::string g_host_token;
std::string g_host_server;
int g_host_port = 0;
std::string g_host_status = "Not hosting.";
std::atomic<bool> g_hosting{false};
std::atomic<long long> g_last_beat{0};
std::atomic<int> g_host_players{1};
// docs/394: what the lobby is doing, for the master server ("open" listed,
// "starting" unlisted, "playing" the match), and its map once one loaded.
std::string g_host_state = "open";   // under g_host_mutex
std::string g_host_map;              // under g_host_mutex
// docs/419: the same lobby's listing on the second server ("Both"), or empty.
std::string g_host2_server, g_host2_code, g_host2_token;   // under g_host_mutex
int g_host2_port = 0;                                      // under g_host_mutex
}  // namespace

extern "C" void lb_net_set_extra_server(const char* host, int port) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_extra_server = host ? host : "";
    g_extra_port = port;
}

extern "C" void lb_net_announce(const char* server, int port, const char* name,
                                const char* host_name, const char* map_name,
                                int game_port, int players, int max_players) {
    // NO PLACEHOLDER PORT. The tab used to advertise 28700 whenever local
    // wireless was off, and a 2026-09-24 test did exactly that
    // (build/logs/master.log: "hosted by <name> at 127.0.0.1:28700"): a
    // lobby that lists, looks joinable, and points at a port nothing binds.
    if (game_port <= 0 || game_port > 65535) {
        std::lock_guard<std::mutex> lock(g_host_mutex);
        g_host_status = "Local wireless is off: there is no port to advertise.";
        return;
    }
    if (g_busy.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(g_host_mutex);
        g_host_status = "Announcing...";
    }
    // Field names are exactly the ones services/master-server/README.md pins.
    // Unknown keys are ignored silently by the server, so "host" instead of
    // "host_name" would advertise a lobby with a blank host and no error.
    std::string body = "{";
    body += "\"name\":" + json_str(name ? name : "LEGO Battles") + ",";
    body += "\"host_name\":" +
            json_str(host_name && *host_name ? host_name : "Player") + ",";
    body += "\"map_name\":" + json_str(map_name ? map_name : "") + ",";
    body += "\"players\":" + std::to_string(players) + ",";
    body += "\"max_players\":" + std::to_string(max_players) + ",";
    // The host's simulation settings, by the panel's labels, and its build:
    // what the master server's Discord log shows for the lobby (docs/394).
    body += "\"version\":" + json_str(lb_port_version()) + ",";
    body += "\"options\":" + std::string(lb_net_lobby_options_json());
    body += "}";

    g_host_players = players > 0 ? players : 1;
    // ADVERTISING IS ALSO "START LISTENING". A lobby whose address nobody can
    // send to is a directory entry and nothing else, and the socket binds
    // loopback until it is told otherwise -- deliberately, so that merely
    // enabling local wireless does not open a port to the network. Saying
    // "advertise" is the moment the player asks for that.
    nds_local_wireless_set_listen(1);
    request_stun();   // docs/400
    std::string s = server ? server : "";
    std::string xh;   // docs/419: and the second server, for Both
    int xp = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        xh = g_extra_server;
        xp = g_extra_port;
    }
    std::thread([s, port, body, game_port, xh, xp]() {
        // docs/400: THE ADDRESS AND PORT THE ROUTER GAVE THIS SOCKET, from
        // STUN, rather than the address the server saw the HTTP request come
        // from and the port bound here. They differ whenever the router
        // renumbers ports, and on a home-run server the seen address is the
        // router's private one. Without an answer the old fields stand.
        // lan_address/lan_port reach players behind the same router, whom
        // the server tells apart by their public address.
        std::string b = body;
        b.pop_back();   // the closing brace
        // docs/405: a router that forwards the port by UPnP is reachable by
        // anyone at its public address and that port, whatever its NAT does
        // to other traffic -- so that pair is advertised first.
        std::string wan;
        const bool mapped = upnp_map((uint16_t)game_port, &wan);
        const uint32_t wan_ip = mapped ? resolve_ipv4(wan.c_str()) : INADDR_NONE;
        uint32_t pip = 0u;
        uint16_t pport = 0u;
        if (mapped && wan_ip != INADDR_NONE && wan_ip != 0u && !is_private_ip(wan_ip)) {
            b += ",\"address\":" + json_str(wan.c_str()) +
                 ",\"port\":" + std::to_string(game_port);
            std::fprintf(stderr, "[lb_net] hosting at %s:%d (UPnP)\n", wan.c_str(),
                         game_port);
        } else if (wait_public(2500, &pip, &pport)) {
            b += ",\"address\":" + json_str(ip_text(pip).c_str()) +
                 ",\"port\":" + std::to_string(pport);
            std::fprintf(stderr, "[lb_net] hosting at public %s:%u (STUN)\n",
                         ip_text(pip).c_str(), (unsigned)pport);
        } else {
            b += ",\"port\":" + std::to_string(game_port);
            std::fprintf(stderr, "[lb_net] no STUN answer: hosting without a "
                         "public address; only a forwarded UDP %d can be "
                         "reached\n", game_port);
        }
        b += ",\"lan_address\":" + json_str(local_ipv4().c_str()) +
             ",\"lan_port\":" + std::to_string(game_port) + "}";
        std::string error;
        const std::string reply =
            http_request(s, port, "POST", "/announce", b, &error);
        // docs/419: Both -- the same lobby on the second server, its own code
        // and token, beaten, polled and removed alongside the first.
        std::string c2, t2;
        if (!xh.empty() && !(xh == s && xp == port) && !reply.empty()) {
            std::string e2;
            const std::string r2 = http_request(xh, xp, "POST", "/announce", b, &e2);
            c2 = field(r2, 0, "code");
            t2 = field(r2, 0, "token");
            if (c2.empty())
                std::fprintf(stderr, "[lb_net] not listed on %s too: %s\n", xh.c_str(),
                             e2.empty() ? field(r2, 0, "error").c_str() : e2.c_str());
            else
                std::fprintf(stderr, "[lb_net] also listed on %s as room %s\n",
                             xh.c_str(), c2.c_str());
        }
        std::lock_guard<std::mutex> lock(g_host_mutex);
        g_host2_server = c2.empty() ? std::string() : xh;
        g_host2_port = xp;
        g_host2_code = c2;
        g_host2_token = t2;
        if (reply.empty()) {
            g_host_status = error.empty() ? "no reply" : error;
        } else {
            const std::string code = field(reply, 0, "code");
            const std::string token = field(reply, 0, "token");
            if (code.empty()) {
                g_host_status = "refused: " + field(reply, 0, "error");
            } else {
                g_host_code = code;
                g_host_token = token;
                g_host_server = s;
                g_host_port = port;
                g_hosting = true;
                g_host_state = "open";
                g_host_map.clear();
                g_host_status = "Advertised as " + code;
                g_last_beat = 0;   /* beat promptly, do not wait 30 s */
                std::fprintf(stderr, "[lb_net] hosting as room %s\n", code.c_str());
            }
        }
        g_busy = false;
    }).detach();
}

namespace {
std::atomic<long long> g_last_poll{0};
std::atomic<bool> g_polling{false};

// docs/400: while the lobby waits for its first player, ask the server every
// 2 s who wants in, and say hello to each for 15 s. A host that has learned a
// player stops: the bridge holds one peer.
void host_poll(long long now_ms) {
    if (now_ms - g_last_poll.load() < 2000 || g_polling.load()) return;
    uint32_t pip = 0u;
    uint16_t pport = 0u;
    int learned = 0;
    nds_local_wireless_peer(&pip, &pport, &learned);
    if (learned) return;
    std::string server, code, token, state;
    int port;
    std::vector<std::pair<std::string, int>> where;   // docs/419: and Both's
    std::vector<std::string> bodies;
    {
        std::lock_guard<std::mutex> lock(g_host_mutex);
        server = g_host_server; code = g_host_code;
        token = g_host_token;   port = g_host_port;
        state = g_host_state;
        if (!g_host2_code.empty()) {
            where.push_back({g_host2_server, g_host2_port});
            bodies.push_back("{\"code\":\"" + g_host2_code + "\",\"token\":\"" +
                             g_host2_token + "\"}");
        }
    }
    if (code.empty() || state != "open") return;
    g_last_poll = now_ms;
    g_polling = true;
    where.insert(where.begin(), {server, port});
    bodies.insert(bodies.begin(),
                  "{\"code\":\"" + code + "\",\"token\":\"" + token + "\"}");
    std::thread([where, bodies]() {
      for (size_t w = 0; w < where.size(); ++w) {
        const std::string& server = where[w].first;
        const int port = where[w].second;
        const std::string& body = bodies[w];
        std::string error;
        const std::string reply =
            http_request(server, port, "POST", "/poll", body, &error);
        size_t at = reply.find("\"punch\"");
        at = at == std::string::npos ? at : reply.find('[', at);
        const size_t end = at == std::string::npos ? at : close_of(reply, at);
        for (size_t i = at; end != std::string::npos && i < end; ++i) {
            if (reply[i] != '{') continue;
            const size_t close = close_of(reply, i);
            if (close == std::string::npos || close > end) break;
            const std::string obj = reply.substr(i, close - i + 1);
            i = close;
            const uint32_t ip = resolve_ipv4(field(obj, 0, "address").c_str());
            const int p = std::atoi(field(obj, 0, "port").c_str());
            if (ip == INADDR_NONE || p <= 0 || p > 65535) continue;
            nds_local_wireless_punch(ip, (uint16_t)p, 15000);
            static std::string said;
            const std::string who = ip_text(ip) + ":" + std::to_string(p);
            if (said != who) {
                said = who;
                std::fprintf(stderr, "[lb_net] a player is joining from %s; "
                             "answering it\n", who.c_str());
            }
        }
      }
        g_polling = false;
    }).detach();
}
}  // namespace

extern "C" void lb_net_host_tick(long long now_ms) {
    if (!g_hosting.load()) return;
    host_poll(now_ms);   // docs/400
    // 30 s, comfortably inside the server's 90 s TTL.
    if (now_ms - g_last_beat.load() < 30000) return;
    g_last_beat = now_ms;

    std::string server, code, token, state, map;
    int port;
    {
        std::lock_guard<std::mutex> lock(g_host_mutex);
        server = g_host_server; code = g_host_code;
        token = g_host_token;   port = g_host_port;
        state = g_host_state;   map = g_host_map;
    }
    if (code.empty()) return;
    // "players" MUST be sent. The server's _heartbeat reads it with a default
    // of 0, so a heartbeat that omits it silently resets the advertised player
    // count to zero every time it fires -- the lobby would still be listed but
    // would claim nobody is in it.
    // docs/394: and the lobby's state and map. The Python server ignores both.
    std::string body =
        "{\"code\":\"" + code + "\",\"token\":\"" + token + "\",\"players\":" +
        std::to_string(g_host_players.load()) + ",\"state\":" +
        json_str(state.c_str());
    if (!map.empty()) body += ",\"map_name\":" + json_str(map.c_str());
    body += "}";
    std::thread([server, port, body]() {
        std::string error;
        http_request(server, port, "POST", "/heartbeat", body, &error);
    }).detach();
    // docs/419: Both -- the second server's listing, with its own code and token.
    std::string s2, c2, t2;
    int p2 = 0;
    {
        std::lock_guard<std::mutex> lock(g_host_mutex);
        s2 = g_host2_server; c2 = g_host2_code; t2 = g_host2_token; p2 = g_host2_port;
    }
    if (!c2.empty()) {
        std::string b2 = body;
        const std::string from = "\"code\":\"" + code + "\",\"token\":\"" + token + "\"";
        const std::string to = "\"code\":\"" + c2 + "\",\"token\":\"" + t2 + "\"";
        const size_t at = b2.find(from);
        if (at != std::string::npos) b2.replace(at, from.size(), to);
        std::thread([s2, p2, b2]() {
            std::string error;
            http_request(s2, p2, "POST", "/heartbeat", b2, &error);
        }).detach();
    }
}

namespace {
// Set when a PERSON stops advertising while the game's lobby is still open, so
// the automatic path does not put the lobby straight back.
std::atomic<bool> g_user_stopped{false};

// `reason` (docs/394): "disband", the lobby closed without a match, or "end",
// a match ended. The Python server ignores it.
void stop_hosting_impl(const char* reason = "disband") {
    if (!g_hosting.exchange(false)) return;
    std::string server, code, token;
    int port;
    {
        std::lock_guard<std::mutex> lock(g_host_mutex);
        server = g_host_server; code = g_host_code;
        token = g_host_token;   port = g_host_port;
        g_host_status = "Not hosting.";
        g_host_code.clear();
        // docs/419: Both -- take the second listing down as well.
        if (!g_host2_code.empty()) {
            const std::string b2 = "{\"code\":\"" + g_host2_code + "\",\"token\":\"" +
                                   g_host2_token + "\",\"reason\":" + json_str(reason) + "}";
            const std::string s2 = g_host2_server;
            const int p2 = g_host2_port;
            std::thread([s2, p2, b2]() {
                std::string error;
                http_request(s2, p2, "POST", "/remove", b2, &error);
            }).detach();
            g_host2_code.clear();
            g_host2_server.clear();
        }
    }
    if (code.empty()) return;
    const std::string body =
        "{\"code\":\"" + code + "\",\"token\":\"" + token + "\",\"reason\":" +
        json_str(reason) + "}";
    std::thread([server, port, body]() {
        std::string error;
        http_request(server, port, "POST", "/remove", body, &error);
        upnp_unmap();   // docs/405: the lobby is gone; so is the open port
    }).detach();
}
}  // namespace

extern "C" void lb_net_stop_hosting(void) {
    g_user_stopped = true;
    stop_hosting_impl();
}

extern "C" int lb_net_is_hosting(void) { return g_hosting.load() ? 1 : 0; }

extern "C" const char* lb_net_host_status(void) {
    static std::string copy;
    std::lock_guard<std::mutex> lock(g_host_mutex);
    copy = g_host_status;
    return copy.c_str();
}

extern "C" const char* lb_net_host_code(void) {
    static std::string copy;
    std::lock_guard<std::mutex> lock(g_host_mutex);
    copy = g_host_code;
    return copy.c_str();
}

// ── Joining ────────────────────────────────────────────────────────────────
//
// WHAT JOINING ACTUALLY IS, now that docs/216 has shown the game's own lobby
// working between two instances.
//
// The cartridge finds and joins a peer entirely by itself, over DS wireless.
// The port carries those frames between processes (LocalMpTransport), and the
// only thing missing for two MACHINES was somewhere to send them. So joining is
// not a protocol this port implements -- it is one address:
//
//     GET /lobby/<code>  ->  {"address": "...", "port": N}
//     nds_local_wireless_set_peer(that)
//     ...and then the player uses the game's OWN Multiplayer -> Join screen,
//        which now sees the remote host exactly as it saw a local one.
//
// The master server's own README calls itself "a directory, not a relay", and
// this is the whole of what consuming a directory entry means here. No game
// traffic goes near it.
//
// A HOST does the mirror of this: it advertises the UDP port it is listening
// on, turns on listening (which is what makes the socket bind INADDR_ANY rather
// than loopback), and learns the client's address from the first datagram that
// arrives. Neither side needs the other's address in advance except the one the
// directory hands over.
extern "C" void lb_net_join_by_code(const char* server, int port,
                                    const char* code) {
    if (!code || !*code) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = "enter a room code first";
        return;
    }
    if (g_busy.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = std::string("Looking up ") + code + "...";
    }
    std::string s = server ? server : "";
    std::string c = code;
    std::thread([s, port, c]() {
        std::string error;
        const std::string reply =
            http_request(s, port, "GET", "/lobby/" + c, "", &error);
        std::string msg;
        if (reply.empty()) {
            msg = error.empty() ? "no reply" : error;
        } else {
            const std::string addr = field(reply, 0, "address");
            const std::string prt = field(reply, 0, "port");
            if (addr.empty()) {
                msg = "no such room: " + c;
            } else {
                const int p = std::atoi(prt.c_str());
                // lobby_ip also swaps a loopback address for the master
                // server's own when the master server is elsewhere.
                const uint32_t ip = lobby_ip(addr.c_str(), s);
                if (ip == INADDR_NONE || p <= 0 || p > 65535) {
                    msg = "bad address for " + c + ": " + addr + ":" + prt;
                } else {
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_join_pending = true;
                        g_join_ip = ip;
                        g_join_port = (uint16_t)p;
                        g_peer_code = c;
                        g_peer_server = s;      // docs/424
                        g_peer_sport = port;
                    }
                    remember_manual_peer(ip, (uint16_t)p);
                    msg = peer_set_text(ip, p, addr.c_str());
                    std::fprintf(stderr,
                                 "[lb_net] join %s -> peer %s:%d (listed %s)\n",
                                 c.c_str(), ip_text(ip).c_str(), p,
                                 addr.c_str());
                    post_punch(s, port, c);   // docs/400
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status = msg;
        }
        g_busy = false;
    }).detach();
}

// Point the bridge at a lobby already in the list. No round trip: browsing
// already fetched and resolved the address, so a per-row Join button costs one
// function call and no waiting, where "Join by code" has to ask the directory
// again.
extern "C" int lb_net_join_index(int index) {
    LbLobby lobby{};
    uint32_t ip = INADDR_NONE;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (index < 0 || index >= (int)g_lobbies.size()) return 0;
        lobby = g_lobbies[(size_t)index];
        if ((size_t)index < g_lobby_ip.size()) ip = g_lobby_ip[(size_t)index];
    }
    if (!lobby.address[0] || lobby.port <= 0 || lobby.port > 65535) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = "that lobby published no address";
        return 0;
    }
    if (ip == INADDR_NONE) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = std::string("cannot resolve ") + lobby.address;
        return 0;
    }
    nds_local_wireless_set_peer(ip, (uint16_t)lobby.port);
    remember_manual_peer(ip, (uint16_t)lobby.port);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = peer_set_text(ip, lobby.port, lobby.address);
        g_peer_code = lobby.code;
    }
    std::fprintf(stderr, "[lb_net] join lobby %d -> peer %s:%d (listed %s)\n",
                 index, ip_text(ip).c_str(), lobby.port, lobby.address);
    std::string ls;
    int lp = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ls = g_list_server;
        lp = g_list_port;
        // docs/419: through the server that listed this lobby.
        if ((size_t)index < g_lobby_srv.size() && !g_lobby_srv[(size_t)index].first.empty()) {
            ls = g_lobby_srv[(size_t)index].first;
            lp = g_lobby_srv[(size_t)index].second;
        }
        g_peer_server = ls;   // docs/424
        g_peer_sport = lp;
    }
    post_punch(ls, lp, lobby.code);   // docs/400
    return 1;
}

extern "C" unsigned short lb_net_wireless_port(void) {
    return nds_local_wireless_port();
}

extern "C" const char* lb_net_peer_text(void) {
    static char text[64];
    uint32_t ip = 0;
    uint16_t port = 0;
    int learned = 0;
    if (!nds_local_wireless_peer(&ip, &port, &learned) || !ip) {
        text[0] = 0;
        return text;
    }
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u:%u%s",
                  (unsigned)(ip & 0xFFu), (unsigned)((ip >> 8) & 0xFFu),
                  (unsigned)((ip >> 16) & 0xFFu), (unsigned)((ip >> 24) & 0xFFu),
                  (unsigned)port, learned ? " (learned)" : "");
    return text;
}

// ── What the cartridge's own network layer is doing ───────────────────────
//
// Reported every VBlank by lb_patches.cpp (report_net_state), which carries the
// decomp reading: the state word the cartridge's network task keeps, and the
// loaded profile's name as the Multiplayer menu hands it to the lobby.

namespace {
std::atomic<int> g_guest_state{0};
std::mutex g_guest_mutex;
std::string g_profile;   // under g_guest_mutex
}  // namespace

extern "C" void lb_net_guest_update(int net_state, const char* profile_name) {
    g_guest_state.store(net_state);
    // At most 10 bytes: the cartridge keeps 11 with the NUL (FUN_0204FE4C
    // copies whole characters into a 10-byte budget). A control byte means the
    // buffer is not a name -- mid-load, or never loaded -- and reads as none.
    char clean[16];
    int n = 0;
    if (profile_name) {
        for (int i = 0; i < 10 && profile_name[i]; ++i) {
            const unsigned char c = (unsigned char)profile_name[i];
            if (c < 0x20 || c == 0x7F) {
                n = 0;
                break;
            }
            clean[n++] = (char)c;
        }
    }
    clean[n] = 0;
    std::lock_guard<std::mutex> lock(g_guest_mutex);
    if (g_profile != clean) g_profile = clean;
}

extern "C" int lb_net_guest_state(void) { return g_guest_state.load(); }

extern "C" const char* lb_net_guest_state_text(void) {
    static char text[48];
    const int s = g_guest_state.load();
    const char* what = nullptr;
    switch (s) {
    case 0:  what = "idle"; break;
    case -1: what = "idle (after an error)"; break;
    case 10: what = "hosting: opening the lobby"; break;
    case 2:  what = "hosting: lobby open"; break;
    case 3:
    case 4:  what = "hosting: starting the match"; break;
    case 11: what = "hosting: in a match"; break;
    case 9:  what = "joining: starting the search"; break;
    case 6:  what = "joining: Join screen, searching"; break;
    case 7:  what = "joining: connecting"; break;
    case 5:  what = "joining: in the host's lobby"; break;
    case 12: what = "joining: in a match"; break;
    case 8:  what = "leaving"; break;
    default: break;
    }
    if (what) std::snprintf(text, sizeof(text), "%s", what);
    else      std::snprintf(text, sizeof(text), "state %d", s);
    return text;
}

extern "C" int lb_net_profile_name(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    std::lock_guard<std::mutex> lock(g_guest_mutex);
    std::snprintf(out, (size_t)cap, "%s", g_profile.c_str());
    return (int)std::strlen(out);
}

// ── Automatic hosting and joining ─────────────────────────────────────────
//
// Everything here runs on the thread that calls lb_net_auto_tick -- the
// overlay's frame, which is also the emulation's thread -- except what the
// worker threads above already do.

namespace {
int       g_auto_prev = 0;
long long g_auto_announce_at = 0;
long long g_auto_refresh_at = 0;
unsigned  g_auto_gen_applied = 0;
bool      g_auto_peers = false;
bool      g_auto_hosted = false;
bool      g_auto_warned_off = false;
// docs/394: the match's map and the joined lobby.
unsigned  g_auto_map_gen = 0;      // lb_level_map_gen() when Start was pressed
long long g_auto_play_at = 0;      // report "playing" without a map from here
std::string g_auto_joined_code;    // the lobby a joiner reported itself in

// "I am in / I left this lobby", fire and forget: a lost report costs a line
// in a log, and a frame must never wait on it.
void post_presence(const char* server, int port, const char* path,
                   const std::string& code, const char* name) {
    const std::string body = "{\"code\":" + json_str(code.c_str()) +
                             ",\"name\":" +
                             json_str(name && *name ? name : "Player") + "}";
    std::string s = server ? server : "";
    std::string p = path;
    std::thread([s, port, p, body]() {
        std::string error;
        http_request(s, port, "POST", p, body, &error);
    }).detach();
}

bool have_manual_peer() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_manual_ip != 0u && g_manual_port != 0u;
}

// Back to what a person chose: the peer they picked, or none.
void restore_manual_peer() {
    uint32_t ip;
    uint16_t port;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ip = g_manual_ip;
        port = g_manual_port;
    }
    nds_local_wireless_set_peer(ip, port);   // (0, 0) clears it
}

// A host learns a client only into an EMPTY peer slot, and a client learned in
// an earlier session keeps it. Hosting again must start from what a person
// chose, or the new lobby could never answer anyone.
void forget_learned_peer() {
    uint32_t ip = 0u;
    uint16_t port = 0u;
    int learned = 0;
    if (nds_local_wireless_peer(&ip, &port, &learned) && learned)
        restore_manual_peer();
}

// The bridge holds one peer, so the Join screen reaches one host: the newest
// listed lobby that is not this machine's own, unless a person already chose.
void point_at_listed_host() {
    if (have_manual_peer()) return;
    const unsigned short own = nds_local_wireless_port();
    std::string own_code;
    {
        std::lock_guard<std::mutex> lock(g_host_mutex);
        own_code = g_host_code;
    }
    uint32_t pub_ip = 0u;
    uint16_t pub_port = 0u;
    const bool have_pub = nds_local_wireless_public(&pub_ip, &pub_port) != 0;
    uint32_t ip = 0u;
    uint16_t port = 0u;
    char code[8] = "";
    std::pair<std::string, int> via;   // docs/419: the server that listed it
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (size_t i = g_lobbies.size(); i-- > 0;) {
            if (i >= g_lobby_ip.size()) continue;
            const LbLobby& l = g_lobbies[i];
            const uint32_t at = g_lobby_ip[i];
            if (at == INADDR_NONE || at == 0u) continue;
            if (l.port <= 0 || l.port > 65535) continue;
            if (!own_code.empty() && own_code == l.code) continue;
            // This machine's own lobby, still listed for up to 90 s after it
            // closed: talking to ourselves is never a join.
            if (is_loopback(at) && own != 0 && l.port == (int)own) continue;
            // docs/400: the same, by this machine's public mapping -- what a
            // host now advertises, and what the 2026-10-18 Join screen kept
            // choosing after a restart.
            if (have_pub && at == pub_ip && l.port == (int)pub_port) continue;
            ip = at;
            port = (uint16_t)l.port;
            std::snprintf(code, sizeof code, "%s", l.code);
            if (i < g_lobby_srv.size()) via = g_lobby_srv[i];
            break;
        }
    }
    if (!ip) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_peer_code = code;
        g_peer_server = via.first.empty() ? g_list_server : via.first;   // docs/424
        g_peer_sport = via.first.empty() ? g_list_port : via.second;
    }
    uint32_t cur_ip = 0u;
    uint16_t cur_port = 0u;
    int learned = 0;
    nds_local_wireless_peer(&cur_ip, &cur_port, &learned);
    if (cur_ip == ip && cur_port == port) return;
    nds_local_wireless_set_peer(ip, port);
    std::fprintf(stderr,
                 "[lb_net] Join screen: bridge pointed at lobby %s "
                 "(%u.%u.%u.%u:%u)\n", code,
                 (unsigned)(ip & 0xFFu), (unsigned)((ip >> 8) & 0xFFu),
                 (unsigned)((ip >> 16) & 0xFFu), (unsigned)((ip >> 24) & 0xFFu),
                 (unsigned)port);
    std::string ls;
    int lp = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ls = g_list_server;
        lp = g_list_port;
    }
    if (!via.first.empty()) {   // docs/419
        ls = via.first;
        lp = via.second;
    }
    post_punch(ls, lp, code);   // docs/400
}
// ---- docs/424: the master server's relay --------------------------------
//
// A HOST opens its lobby's relay room once it is listed, and keeps it while
// it hosts, so a player who cannot reach it directly can still join. A JOINER
// uses the relay only when the direct route has carried nothing for 5 s since
// the bridge was pointed at the lobby (LB_TEST_FORCE_RELAY: at once).
//
// The room is on the server that listed the lobby. With Both, a lobby from
// the second server is looked up in the first server's list (same address and
// port) for its code there, since the first is this port's own server.
extern "C" void lb_relay_host_start(const char* server, int port, const char* code,
                                    const char* token);
extern "C" void lb_relay_join_start(const char* server, int port, const char* code);
extern "C" void lb_relay_stop(void);
extern "C" int lb_relay_role(void);
extern "C" int lb_relay_forced(void);

uint32_t    g_rw_ip = 0u;          // the peer being watched
uint16_t    g_rw_port = 0u;
long long   g_rw_at = 0;           // when it was set
unsigned    g_rw_rx = 0u;          // direct datagrams received by then
std::string g_rw_tried;            // the lobby the relay was last tried for
long long   g_rw_idle_at = 0;      // a joiner's relay with nothing to carry since
std::string g_rh_code;             // the lobby the host's room was opened for

void relay_target(const std::string& primary, int primary_port, std::string* srv,
                  int* sport, std::string* code) {
    std::lock_guard<std::mutex> lock(g_mutex);
    *code = g_peer_code;
    *srv = g_peer_server;
    *sport = g_peer_sport;
    if (srv->empty() || *srv == primary) return;
    // Listed by the other server: the same lobby in this server's list.
    const char* addr = nullptr;
    int lport = 0;
    for (size_t i = 0; i < g_lobbies.size(); ++i)
        if (*code == g_lobbies[i].code && i < g_lobby_srv.size() &&
            g_lobby_srv[i].first == *srv) {
            addr = g_lobbies[i].address;
            lport = g_lobbies[i].port;
        }
    if (!addr) return;
    for (size_t i = 0; i < g_lobbies.size(); ++i) {
        const bool ours = i >= g_lobby_srv.size() || g_lobby_srv[i].first.empty() ||
                          g_lobby_srv[i].first == primary;
        if (ours && g_lobbies[i].port == lport && std::strcmp(g_lobbies[i].address, addr) == 0) {
            *code = g_lobbies[i].code;
            *srv = primary;
            *sport = primary_port;
            return;
        }
    }
}

void relay_tick(int s, const char* server, int port, long long now_ms) {
    // The host's room, for as long as it hosts.
    if (g_hosting.load()) {
        std::string srv, code, token;
        int sport = 0;
        {
            std::lock_guard<std::mutex> lock(g_host_mutex);
            srv = g_host_server;
            sport = g_host_port;
            code = g_host_code;
            token = g_host_token;
        }
        if (!code.empty() && code != g_rh_code) {
            g_rh_code = code;
            lb_relay_host_start(srv.c_str(), sport, code.c_str(), token.c_str());
        }
        return;
    }
    if (!g_rh_code.empty()) {
        g_rh_code.clear();
        if (lb_relay_role() == 1) lb_relay_stop();
    }

    // The joiner's fallback.
    uint32_t ip = 0u;
    uint16_t pport = 0u;
    int learned = 0;
    nds_local_wireless_peer(&ip, &pport, &learned);
    unsigned sent = 0u, recvd = 0u;
    nds_local_wireless_stats(&sent, &recvd);
    if (ip != g_rw_ip || pport != g_rw_port) {
        g_rw_ip = ip;
        g_rw_port = pport;
        g_rw_at = now_ms;
        g_rw_rx = recvd;
        g_rw_tried.clear();
        if (lb_relay_role() == 2) lb_relay_stop();   // a different lobby now
    }
    // 9/6 the Join screen, 7 connecting, 5 in the host's lobby, 12 its match.
    const bool joining = s == 9 || s == 6 || s == 7 || s == 5 || s == 12;
    if (lb_relay_role() == 2) {
        if (joining) g_rw_idle_at = 0;
        else if (!g_rw_idle_at) g_rw_idle_at = now_ms;
        else if (now_ms - g_rw_idle_at > 3000) {
            lb_relay_stop();
            g_rw_idle_at = 0;
        }
        return;
    }
    if (!ip || !joining) return;
    const bool quiet = recvd == g_rw_rx;
    if (!lb_relay_forced() && !(quiet && now_ms - g_rw_at > 5000)) return;
    std::string srv, code;
    int sport = 0;
    relay_target(server ? server : "", port, &srv, &sport, &code);
    if (code.empty() || srv.empty() || code == g_rw_tried) return;
    g_rw_tried = code;
    std::fprintf(stderr, "[lb_net] %s; trying the relay for lobby %s on %s\n",
                 lb_relay_forced() ? "LB_TEST_FORCE_RELAY" :
                 "no direct traffic from the host in 5 s",
                 code.c_str(), srv.c_str());
    lb_relay_join_start(srv.c_str(), sport, code.c_str());
}
}  // namespace

extern "C" void lb_net_auto_tick(const char* server, int port,
                                 const char* player_name, int auto_host,
                                 int auto_join, long long now_ms) {
    const int s = g_guest_state.load();
    const int prev = g_auto_prev;
    g_auto_prev = s;

    // A Join by code the worker resolved: to the bridge, on this thread.
    {
        bool pending;
        uint32_t ip;
        uint16_t p;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            pending = g_join_pending;
            ip = g_join_ip;
            p = g_join_port;
            g_join_pending = false;
        }
        if (pending) nds_local_wireless_set_peer(ip, p);
    }

    // ---- The game hosts, so the lobby is advertised -----------------------
    //
    // State 2 is the host's lobby, open and accepting (FUN_020F4B00 moves 10
    // to 2 once the parent is up). It is advertised with the port this
    // instance is actually bound to, under the player's name.
    if (s != 2) g_user_stopped = false;
    if (auto_host && s == 2 && !g_hosting.load() && !g_user_stopped.load() &&
        !g_busy.load() && now_ms >= g_auto_announce_at) {
        const unsigned short wp = nds_local_wireless_port();
        if (wp == 0) {
            if (!g_auto_warned_off) {
                g_auto_warned_off = true;
                {
                    std::lock_guard<std::mutex> lock(g_host_mutex);
                    g_host_status = "The game is hosting, but local wireless "
                                    "is off: nobody can reach this lobby.";
                }
                std::fprintf(stderr,
                             "[lb_net] the game opened a lobby with local "
                             "wireless off; not advertising it\n");
            }
        } else {
            // A refused or unreachable announce is retried, not hammered.
            g_auto_announce_at = now_ms + 10000;
            g_auto_hosted = true;
            forget_learned_peer();
            std::fprintf(stderr,
                         "[lb_net] the game opened a lobby; advertising it on "
                         "UDP %u\n", (unsigned)wp);
            lb_net_announce(server, port, "LEGO Battles", player_name, "",
                            (int)wp, 1, 3);
        }
    }
    if (s != 2) g_auto_warned_off = false;
    // THE LOBBY'S LIFE AFTER IT IS ADVERTISED (docs/394). It used to be
    // withdrawn the moment the game left state 2, which a directory needs --
    // a LEGO Battles match cannot be joined once it has begun -- but which
    // told the master server nothing about why. Now:
    //   3, 4   Start was pressed: "starting", unlisted, still heartbeating
    //   11     the match is on: "playing", sent once the match's map has
    //          loaded (or after 20 s without one), with that map's name
    //   other  the lobby closed: removed, as "end" after a match and
    //          "disband" before one
    if (g_auto_hosted && s != 2 && s != 10) {
        if (g_hosting.load()) {
            std::string state;
            {
                std::lock_guard<std::mutex> lock(g_host_mutex);
                state = g_host_state;
            }
            if (s == 3 || s == 4 || s == 11) {
                if (state == "open") {
                    // New strangers are no longer wanted. A client already
                    // learned stays the peer, so the match keeps its traffic.
                    nds_local_wireless_set_listen(0);
                    std::lock_guard<std::mutex> lock(g_host_mutex);
                    g_host_state = "starting";
                    g_host_map.clear();
                    g_auto_map_gen = lb_level_map_gen();
                    g_auto_play_at = 0;
                    g_last_beat = 0;   // tell the server at once
                    std::fprintf(stderr, "[lb_net] the game started its match "
                                 "(state %d); lobby unlisted\n", s);
                }
                if (s == 11 && state != "playing") {
                    if (!g_auto_play_at) g_auto_play_at = now_ms + 20000;
                    const bool fresh = lb_level_map_gen() != g_auto_map_gen;
                    if (fresh || now_ms >= g_auto_play_at) {
                        std::lock_guard<std::mutex> lock(g_host_mutex);
                        g_host_state = "playing";
                        g_host_map = fresh ? lb_level_map_name() : "";
                        g_last_beat = 0;
                        std::fprintf(stderr, "[lb_net] match under way on "
                                     "\"%s\"\n", g_host_map.c_str());
                    }
                }
            } else {
                const bool played = state == "playing";
                stop_hosting_impl(played ? "end" : "disband");
                nds_local_wireless_set_listen(0);
                g_auto_hosted = false;
                std::fprintf(stderr,
                             "[lb_net] the game closed its lobby (state %d); "
                             "advertisement withdrawn (%s)\n", s,
                             played ? "match over" : "disbanded");
            }
        } else if (!g_busy.load()) {
            g_auto_hosted = false;   // the announce never landed
        }
    }

    // ---- docs/394: a joiner says which lobby it is in, by name ------------
    //
    // 5 is in the host's lobby, 12 in its match (7 is still connecting). The
    // lobby is the one the bridge was pointed at.
    const bool joined_now = s == 5 || s == 12;
    if (joined_now && g_auto_joined_code.empty()) {
        std::string code;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            code = g_peer_code;
        }
        if (!code.empty()) {
            g_auto_joined_code = code;
            post_presence(server, port, "/join", code, player_name);
            std::fprintf(stderr, "[lb_net] in lobby %s; told the master "
                         "server\n", code.c_str());
        }
    } else if (!joined_now && s != 7 && !g_auto_joined_code.empty()) {
        post_presence(server, port, "/leave", g_auto_joined_code, player_name);
        std::fprintf(stderr, "[lb_net] left lobby %s\n",
                     g_auto_joined_code.c_str());
        g_auto_joined_code.clear();
    }

    // ---- The game's Join screen reaches the master server's lobbies
    //
    // 9 is the Join screen asking for a scan (User::JoinWifiGame::virtual_12
    // -> FUN_020F60A4), 6 the scan running with the Join Game list live.
    const bool scanning = s == 9 || s == 6;
    if (auto_join && scanning) {
        if (!g_auto_peers || !(prev == 9 || prev == 6)) {
            g_auto_peers = true;
            g_auto_refresh_at = 0;
            point_at_listed_host();   // the list already held, at once
            g_auto_gen_applied = g_list_gen.load();
        }
        if (!g_busy.load() && now_ms >= g_auto_refresh_at) {
            g_auto_refresh_at = now_ms + 5000;
            lb_net_refresh_lobbies(server, port);
        }
        const unsigned gen = g_list_gen.load();
        if (gen != g_auto_gen_applied) {
            g_auto_gen_applied = gen;
            point_at_listed_host();
        }
    } else if (g_auto_peers && !(s == 7 || s == 5 || s == 12)) {
        // Left the Join screen without joining, or the session is over. 7, 5
        // and 12 are connecting, in the host's lobby and in the match: the
        // peer stays for those.
        restore_manual_peer();
        g_auto_peers = false;
    }

    relay_tick(s, server, port, now_ms);   // docs/424
}

// LB_MP_PEER=host:port and LB_MP_LISTEN=1 do from the command line what the
// Multiplayer tab's buttons do, so two machines can be tested without a human
// clicking anything. Same reasoning as LB_NET_TEST above.
extern "C" void lb_net_apply_env_peer(void) {
    static bool done = false;
    if (done) return;
    done = true;
    if (std::getenv("LB_MP_LISTEN")) {
        nds_local_wireless_set_listen(1);
        std::fprintf(stderr, "[lb_net] LB_MP_LISTEN: listening on UDP %u\n",
                     (unsigned)nds_local_wireless_port());
    }
    const char* spec = std::getenv("LB_MP_PEER");
    if (!spec) return;
    const char* colon = std::strrchr(spec, ':');
    if (!colon) {
        std::fprintf(stderr, "[lb_net] LB_MP_PEER wants host:port\n");
        return;
    }
    char host[128] = {0};
    const size_t n = (size_t)(colon - spec);
    std::snprintf(host, sizeof(host), "%.*s",
                  (int)(n < sizeof(host) - 1 ? n : sizeof(host) - 1), spec);
    const int port = std::atoi(colon + 1);
    const uint32_t ip = resolve_ipv4(host);
    if (ip == INADDR_NONE || port <= 0 || port > 65535) {
        std::fprintf(stderr, "[lb_net] LB_MP_PEER: bad %s\n", spec);
        return;
    }
    nds_local_wireless_set_peer(ip, (uint16_t)port);
    remember_manual_peer(ip, (uint16_t)port);
    std::fprintf(stderr, "[lb_net] LB_MP_PEER: peer %s:%d\n", host, port);
}

// LB_NET_TEST=host:port queries once at startup and prints what came back, so
// the client can be verified without a human clicking the button. Test aid
// only, like LB_AUTOPLAY, and off unless the variable is set.
extern "C" void lb_net_selftest_if_asked(void) {
    static bool done = false;
    if (done) return;
    done = true;
    const char* spec = std::getenv("LB_NET_TEST");
    if (!spec) return;

    char host[128] = {0};
    int port = 28555;
    const char* colon = std::strrchr(spec, ':');
    if (colon) {
        const size_t n = (size_t)(colon - spec);
        std::snprintf(host, sizeof(host), "%.*s", (int)(n < sizeof(host) - 1 ? n : sizeof(host) - 1), spec);
        port = std::atoi(colon + 1);
    } else {
        std::snprintf(host, sizeof(host), "%s", spec);
    }
    // LB_NET_HOST=1 also advertises a lobby first, so the round trip
    // announce -> list can be checked in one run.
    if (std::getenv("LB_NET_HOST")) {
        std::fprintf(stderr, "[lb_net] self-test: advertising on %s:%d\n",
                     host, port);
        // The port this instance is ACTUALLY bound to. Advertising a constant
        // publishes a lobby nobody can reach, so with local wireless off this
        // now refuses and says why, like the tab does.
        const unsigned short wp = nds_local_wireless_port();
        lb_net_announce(host, port, "Self-test Lobby", "SelfTest", "Castle",
                        (int)wp, 2, 3);
        for (int i = 0; i < 50 && lb_net_busy(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        std::fprintf(stderr, "[lb_net] self-test: host status: %s\n",
                     lb_net_host_status());
    }

    std::fprintf(stderr, "[lb_net] self-test: querying %s:%d\n", host, port);
    lb_net_refresh_lobbies(host, port);

    // Give the worker a moment; this is startup, not a frame, so a short wait
    // costs nothing and makes the result appear in the log next to the request.
    for (int i = 0; i < 50 && lb_net_busy(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(40));

    const int n = lb_net_lobby_count();
    std::fprintf(stderr, "[lb_net] self-test: %s (%d lobby(ies))\n",
                 lb_net_status(), n);
    for (int i = 0; i < n; ++i) {
        LbLobby lobby{};
        if (!lb_net_lobby_at(i, &lobby)) continue;
        std::fprintf(stderr,
                     "[lb_net]   %s  %s  host=%s  map=%s  %d/%d  at %s:%d\n",
                     lobby.code, lobby.name, lobby.host_name, lobby.map_name,
                     lobby.players, lobby.max_players,
                     lobby.address, lobby.port);
    }
    // LB_MP_JOIN=1 also points the bridge at the first lobby listed, which is
    // the whole client flow -- browse, pick, join -- with no human in it.
    if (n > 0 && std::getenv("LB_MP_JOIN")) {
        const int ok = lb_net_join_index(0);
        std::fprintf(stderr, "[lb_net] self-test: join first lobby -> %s\n",
                     ok ? lb_net_status() : "FAILED");
    }
}
