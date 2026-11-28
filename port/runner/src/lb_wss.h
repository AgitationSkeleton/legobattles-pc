// legobattles-decomp: a WebSocket over TLS through WinHTTP (lb_https.cpp), for
// the relay (lb_relay.cpp). See lb_https.cpp for why.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct LbWss;

struct LbWssMessage {
    int opcode = 0;   // 1 text, 2 binary (as the relay's own frames)
    std::string data;
};

// Whether this system's WinHTTP has the WebSocket functions.
bool lb_wss_available();
// The upgrade to wss://host:port/path. Null on failure, with `status` the
// HTTP status when the server answered (and `body` its text), else 0.
std::shared_ptr<LbWss> lb_wss_open(const std::string& host, int port, const std::string& path,
                                   const std::string& headers, int timeout_ms, int* status,
                                   std::string* body, std::string* error,
                                   bool insecure = false);   // test aid: accept any certificate
bool lb_wss_send(LbWss* w, int opcode, const void* data, size_t n);
// Up to timeout_ms for messages; the count taken, or -1 once the link closed.
int lb_wss_recv(LbWss* w, int timeout_ms, std::vector<LbWssMessage>* out);
// Ends the link at once (never waits on the network).
void lb_wss_close(LbWss* w);
