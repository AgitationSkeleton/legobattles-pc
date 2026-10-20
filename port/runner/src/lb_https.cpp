// legobattles-decomp: HTTPS for the master server (docs/419), through the
// system's own WinHTTP, so certificates are checked by Windows and no TLS
// library ships with the game. Its own translation unit because it needs
// <windows.h> (as lb_filedlg.cpp and lb_winloop.cpp do).
//
// lb_net.cpp's plain HTTP client asks this first when the setting is on; a
// failure here (no TLS on that server, a proxy that blocks it) leaves the
// plain request to go ahead, and says so once.

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

#else
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
