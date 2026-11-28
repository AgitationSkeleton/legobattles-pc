// legobattles-decomp: HTTPS for the master server (docs/419), through the
// system's own WinHTTP, so certificates are checked by Windows and no TLS
// library ships with the game. Its own translation unit because it needs
// <windows.h> (as lb_filedlg.cpp and lb_winloop.cpp do).
//
// lb_net.cpp's plain HTTP client asks this first when the setting is on; a
// failure here (no TLS on that server, a proxy that blocks it) leaves the
// plain request to go ahead, and says so once.

#include <cstdlib>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
}  // namespace

// The reply's body, or false with `error` set. Timeouts are short: a lobby
// list is small, and a hung server must not hold the panel.
bool lb_https_request(const std::string& host, int port, const std::string& method,
                      const std::string& path, const std::string& body,
                      std::string* reply, std::string* error) {
    reply->clear();
    HINTERNET session = WinHttpOpen(L"LEGO Battles PC", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { *error = "HTTPS unavailable"; return false; }
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
    HINTERNET conn = WinHttpConnect(session, widen(host).c_str(), (INTERNET_PORT)port, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, widen(method).c_str(),
                                              widen(path).c_str(), nullptr,
                                              WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE)
                         : nullptr;
    bool ok = false;
    if (req) {
        // Test aid (lb_relay.cpp): LB_TEST_WSS=host:port is a local wrangler
        // dev over HTTPS with a self-signed certificate.
        if (const char* t = std::getenv("LB_TEST_WSS")) {
            if (host + ":" + std::to_string(port) == t) {
                DWORD f = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
                WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &f, sizeof f);
            }
        }
        const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                              : L"Content-Type: application/json\r\n";
        const DWORD hlen = body.empty() ? 0 : (DWORD)-1L;
        if (WinHttpSendRequest(req, headers, hlen,
                               body.empty() ? WINHTTP_NO_REQUEST_DATA
                                            : (LPVOID)body.data(),
                               (DWORD)body.size(), (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, len = sizeof status;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                                WINHTTP_NO_HEADER_INDEX);
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                std::string chunk((size_t)avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0) break;
                reply->append(chunk.data(), got);
                if (reply->size() > (1u << 20)) break;   // a lobby list is never 1 MB
            }
            ok = status >= 200 && status < 500;   // a 4xx is the server's answer
            if (!ok) *error = "HTTPS status " + std::to_string((unsigned)status);
        } else {
            *error = "HTTPS request failed (" + std::to_string((unsigned)GetLastError()) + ")";
        }
    } else {
        *error = "cannot reach " + host + " over HTTPS";
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    if (!ok) reply->clear();
    return ok;
}

// docs/420: a whole file over HTTPS (redirects followed, as WinHTTP does by
// default), up to `max` bytes: the map archive's zips.
bool lb_https_download(const std::string& host, const std::string& path,
                       std::string* data, size_t max, std::string* error) {
    data->clear();
    HINTERNET session = WinHttpOpen(L"LEGO Battles PC", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { *error = "HTTPS unavailable"; return false; }
    WinHttpSetTimeouts(session, 8000, 8000, 15000, 30000);
    HINTERNET conn = WinHttpConnect(session, widen(host).c_str(),
                                    INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", widen(path).c_str(), nullptr,
                                              WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE)
                         : nullptr;
    bool ok = false;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                            WINHTTP_NO_HEADER_INDEX);
        ok = status == 200;
        while (ok) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk((size_t)avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0) break;
            data->append(chunk.data(), got);
            if (data->size() > max) { ok = false; *error = "file too large"; }
        }
        if (status != 200) *error = "HTTP status " + std::to_string((unsigned)status);
    } else {
        *error = "cannot download from " + host;
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    if (!ok) data->clear();
    return ok;
}

// ---- a WebSocket over TLS, for the relay (lb_relay.cpp) ----------------------
//
// The main server answers plain HTTP with a redirect to
// HTTPS (Cloudflare's "Always Use HTTPS", kept on as a security option), and
// the relay's own WebSocket client speaks plain HTTP only, so every relayed
// match was refused with a 301 (BR-0111). WinHTTP has a WebSocket client of its
// own over TLS: the upgrade, the framing, masking and the control frames are
// Windows'. Its functions are looked up at run time, so a Windows or Wine
// without them only loses this route (the caller falls back to plain).
//
// One reader thread per connection does the blocking receives into a queue;
// lb_wss_recv waits on that queue. Closing the WebSocket handle from another
// thread ends a blocked receive, and the reader then drops its reference.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include "lb_wss.h"

namespace {
using PFN_Upgrade = HINTERNET(WINAPI*)(HINTERNET, DWORD_PTR);
using PFN_Send = DWORD(WINAPI*)(HINTERNET, WINHTTP_WEB_SOCKET_BUFFER_TYPE, PVOID, DWORD);
using PFN_Receive = DWORD(WINAPI*)(HINTERNET, PVOID, DWORD, DWORD*, WINHTTP_WEB_SOCKET_BUFFER_TYPE*);
using PFN_Close = DWORD(WINAPI*)(HINTERNET, USHORT, PVOID, DWORD);
struct WsApi {
    PFN_Upgrade upgrade = nullptr;
    PFN_Send send = nullptr;
    PFN_Receive receive = nullptr;
    PFN_Close close = nullptr;
    bool ok() const { return upgrade && send && receive && close; }
};
const WsApi& ws_api() {
    static const WsApi api = [] {
        WsApi a;
        if (HMODULE m = GetModuleHandleW(L"winhttp.dll")) {
            a.upgrade = (PFN_Upgrade)(void*)GetProcAddress(m, "WinHttpWebSocketCompleteUpgrade");
            a.send = (PFN_Send)(void*)GetProcAddress(m, "WinHttpWebSocketSend");
            a.receive = (PFN_Receive)(void*)GetProcAddress(m, "WinHttpWebSocketReceive");
            a.close = (PFN_Close)(void*)GetProcAddress(m, "WinHttpWebSocketClose");
        }
        return a;
    }();
    return api;
}
}  // namespace

struct LbWss {
    HINTERNET session = nullptr, conn = nullptr;
    std::atomic<HINTERNET> ws{nullptr};  // taken (and closed) once by lb_wss_close
    std::mutex mu;                       // the queue, `closed`, the handles' release
    std::condition_variable cv;
    std::deque<LbWssMessage> in;
    bool closed = false;                 // the reader has stopped (or close was asked)
    std::mutex send_mu;
    ~LbWss() {
        if (HINTERNET h = ws.exchange(nullptr)) WinHttpCloseHandle(h);
        if (conn) WinHttpCloseHandle(conn);
        if (session) WinHttpCloseHandle(session);
    }
};

namespace {
void wss_reader(std::shared_ptr<LbWss> w) {
    const WsApi& api = ws_api();
    const HINTERNET h = w->ws.load();
    std::string msg;
    std::vector<char> buf(65536);
    for (;;) {
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD err = api.receive(h, buf.data(), (DWORD)buf.size(), &got, &type);
        if (err != ERROR_SUCCESS || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        msg.append(buf.data(), got);
        if (msg.size() > (1u << 24)) break;
        const bool text = type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                          type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;
        if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
            continue;   // more of the same message to come
        {
            std::lock_guard<std::mutex> lk(w->mu);
            if (w->in.size() < 4096) w->in.push_back({text ? 1 : 2, std::move(msg)});
        }
        msg.clear();
        w->cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(w->mu);
        w->closed = true;
    }
    w->cv.notify_all();
}
}  // namespace

bool lb_wss_available() { return ws_api().ok(); }

std::shared_ptr<LbWss> lb_wss_open(const std::string& host, int port, const std::string& path,
                                   const std::string& headers, int timeout_ms, int* status,
                                   std::string* body, std::string* error, bool insecure) {
    *status = 0;
    body->clear();
    const WsApi& api = ws_api();
    if (!api.ok()) { *error = "no WebSocket support in WinHTTP"; return nullptr; }
    auto w = std::make_shared<LbWss>();
    w->session = WinHttpOpen(L"LEGO Battles PC", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!w->session) { *error = "HTTPS unavailable"; return nullptr; }
    // Connect/send/receive limits for the upgrade only; the WebSocket's own
    // receives then block until a message (the reader's), and the relay's
    // pings and dead-link timer decide when a link is gone.
    WinHttpSetTimeouts(w->session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    w->conn = WinHttpConnect(w->session, widen(host).c_str(), (INTERNET_PORT)port, 0);
    if (!w->conn) { *error = "cannot reach " + host + " over HTTPS"; return nullptr; }
    HINTERNET req = WinHttpOpenRequest(w->conn, L"GET", widen(path).c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    if (!req) { *error = "cannot reach " + host + " over HTTPS"; return nullptr; }
    if (insecure) {   // LB_TEST_WSS only: a local wrangler dev's self-signed certificate
        DWORD f = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &f, sizeof f);
    }
    // WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET (114): the request becomes the upgrade.
    if (!WinHttpSetOption(req, 114, nullptr, 0)) {
        WinHttpCloseHandle(req);
        *error = "no WebSocket support in WinHTTP";
        return nullptr;
    }
    const std::wstring wh = widen(headers);
    if (!WinHttpSendRequest(req, wh.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wh.c_str(),
                            wh.empty() ? 0 : (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        *error = "HTTPS request failed (" + std::to_string((unsigned)GetLastError()) + ")";
        WinHttpCloseHandle(req);
        return nullptr;
    }
    DWORD st = 0, len = sizeof st;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &st, &len, WINHTTP_NO_HEADER_INDEX);
    *status = (int)st;
    if (st != 101) {
        for (;;) {   // the refusal's text ("not on the relay", ...)
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk((size_t)avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0) break;
            body->append(chunk.data(), got);
            if (body->size() > 4096) break;
        }
        WinHttpCloseHandle(req);
        *error = "HTTP status " + std::to_string((unsigned)st);
        return nullptr;
    }
    w->ws = api.upgrade(req, 0);
    WinHttpCloseHandle(req);
    if (!w->ws) {
        *error = "the WebSocket upgrade failed (" + std::to_string((unsigned)GetLastError()) + ")";
        return nullptr;
    }
    std::thread(wss_reader, w).detach();
    return w;
}

bool lb_wss_send(LbWss* w, int opcode, const void* data, size_t n) {
    if (!w) return false;
    {
        std::lock_guard<std::mutex> lk(w->mu);
        if (w->closed) return false;
    }
    std::lock_guard<std::mutex> lk(w->send_mu);
    const auto type = opcode == 1 ? WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
                                  : WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
    const HINTERNET h = w->ws.load();
    return h && ws_api().send(h, type, (PVOID)data, (DWORD)n) == ERROR_SUCCESS;
}

int lb_wss_recv(LbWss* w, int timeout_ms, std::vector<LbWssMessage>* out) {
    if (!w) return -1;
    std::unique_lock<std::mutex> lk(w->mu);
    w->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                   [w] { return !w->in.empty() || w->closed; });
    const int n = (int)w->in.size();
    while (!w->in.empty()) {
        out->push_back(std::move(w->in.front()));
        w->in.pop_front();
    }
    if (n == 0 && w->closed) return -1;
    return n;
}

void lb_wss_close(LbWss* w) {
    if (!w) return;
    {
        std::lock_guard<std::mutex> lk(w->mu);
        w->closed = true;
    }
    // The handle, closed at once: that cancels a receive blocked in the
    // reader. No close handshake -- WinHttpWebSocketClose waits for the
    // server's answer, and this runs on the game thread (drop_current); the
    // relay treats a dropped socket the same way.
    std::lock_guard<std::mutex> lk(w->send_mu);
    if (HINTERNET h = w->ws.exchange(nullptr)) WinHttpCloseHandle(h);
    w->cv.notify_all();
}

#else
bool lb_wss_available() { return false; }
std::shared_ptr<LbWss> lb_wss_open(const std::string&, int, const std::string&,
                                   const std::string&, int, int* status, std::string* body,
                                   std::string* error, bool) {
    *status = 0;
    body->clear();
    *error = "HTTPS not built on this platform";
    return nullptr;
}
bool lb_wss_send(LbWss*, int, const void*, size_t) { return false; }
int lb_wss_recv(LbWss*, int, std::vector<LbWssMessage>*) { return -1; }
void lb_wss_close(LbWss*) {}
bool lb_https_download(const std::string&, const std::string&, std::string* data,
                       size_t, std::string* error) {
    data->clear();
    *error = "HTTPS not built on this platform";
    return false;
}
// Elsewhere (docs/417): not yet; the plain request goes ahead.
bool lb_https_request(const std::string&, int, const std::string&, const std::string&,
                      const std::string&, std::string* reply, std::string* error) {
    reply->clear();
    *error = "HTTPS not built on this platform";
    return false;
}
#endif
