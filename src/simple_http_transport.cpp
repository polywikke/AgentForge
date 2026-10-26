// ============================================================================
// SimpleHTTPTransport - WinHTTP 实现 (同步 + SSE 流式)
// ============================================================================
#include "simple_http_transport.hpp"

#include <windows.h>
#include <winhttp.h>

#include <iostream>
#include <stdexcept>
#include <string>

#pragma comment(lib, "winhttp.lib")

namespace {

std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], needed);
    return out;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                     nullptr, 0, nullptr, nullptr);
    std::string out(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &out[0], needed, nullptr, nullptr);
    return out;
}

struct WinHttpHandles {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;

    ~WinHttpHandles() {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
};

} // namespace

HttpResponse SimpleHTTPTransport::request(const HttpRequest& req) {
    return perform(req, nullptr);
}

HttpResponse SimpleHTTPTransport::requestStream(const HttpRequest& req,
                                                IHttpTransport::StreamCallback on_line) {
    return perform(req, &on_line);
}

HttpResponse SimpleHTTPTransport::perform(const HttpRequest& req,
                                          IHttpTransport::StreamCallback* on_line) {
    HttpResponse response;

    std::string url = req.url.empty() ? base_url_ : req.url;
    if (url.empty()) {
        return HttpResponse::error("Empty request URL");
    }

    std::wstring wurl = toWide(url);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);

    wchar_t scheme[16] = {0};
    wchar_t host[256] = {0};
    wchar_t path[4096] = {0};
    wchar_t extra[2048] = {0};

    uc.lpszScheme = scheme;
    uc.dwSchemeLength = _countof(scheme);
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = _countof(extra);

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        return HttpResponse::error("Failed to parse URL: " + url);
    }

    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    INTERNET_PORT port = uc.nPort;
    if (port == 0) port = https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    const std::string method = req.method.empty() ? "POST" : req.method;

    WinHttpHandles h;
    h.session = WinHttpOpen(L"AgentForge/2.0",
                            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session) {
        return HttpResponse::error("WinHttpOpen failed");
    }

    DWORD timeout = req.timeout_ms > 0 ? (DWORD)req.timeout_ms : 60000;
    WinHttpSetTimeouts(h.session, timeout, timeout, timeout, timeout);

    h.connect = WinHttpConnect(h.session, host, port, 0);
    if (!h.connect) {
        return HttpResponse::error("WinHttpConnect failed");
    }

    std::wstring wpath = std::wstring(path) + extra;
    if (wpath.empty()) wpath = L"/";

    std::wstring wmethod = toWide(method);
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    h.request = WinHttpOpenRequest(h.connect, wmethod.c_str(), wpath.c_str(),
                                   nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!h.request) {
        return HttpResponse::error("WinHttpOpenRequest failed");
    }

    // 中转站/自签名证书容错 (仅 HTTPS)
    if (https) {
        DWORD securityFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                              SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                              SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        WinHttpSetOption(h.request, WINHTTP_OPTION_SECURITY_FLAGS,
                         &securityFlags, sizeof(securityFlags));
    }

    // 请求头
    bool has_content_type = false;
    std::wstring allHeaders;
    for (const auto& [key, value] : req.headers) {
        if (_stricmp(key.c_str(), "Content-Type") == 0) has_content_type = true;
        allHeaders += toWide(key) + L": " + toWide(value) + L"\r\n";
    }
    if (!has_content_type) {
        allHeaders = L"Content-Type: application/json\r\n" + allHeaders;
    }
    if (on_line) {
        allHeaders += L"Accept: text/event-stream\r\n";
    }

    if (!WinHttpAddRequestHeaders(h.request, allHeaders.c_str(),
                                  (DWORD)allHeaders.length(),
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        return HttpResponse::error("WinHttpAddRequestHeaders failed");
    }

    LPVOID body_ptr = req.body.empty() ? nullptr : (LPVOID)req.body.data();
    DWORD body_len = (DWORD)req.body.size();

    if (!WinHttpSendRequest(h.request,
                            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            body_ptr, body_len, body_len, 0)) {
        return HttpResponse::error("WinHttpSendRequest failed (error " +
                                   std::to_string(GetLastError()) + ")");
    }

    if (!WinHttpReceiveResponse(h.request, nullptr)) {
        return HttpResponse::error("WinHttpReceiveResponse failed (error " +
                                   std::to_string(GetLastError()) + ")");
    }

    DWORD status = 0;
    DWORD status_len = sizeof(status);
    WinHttpQueryHeaders(h.request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len,
                        WINHTTP_NO_HEADER_INDEX);
    response.status_code = (int)status;

    std::string pending;
    auto flush_lines = [&](bool flush_all) {
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (on_line && *on_line) (*on_line)(line);
        }
        if (flush_all && !pending.empty()) {
            std::string line = pending;
            pending.clear();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (on_line && *on_line) (*on_line)(line);
        }
    };

    char buffer[8192];
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(h.request, &available)) break;
        if (available == 0) break;

        DWORD to_read = available > sizeof(buffer) ? sizeof(buffer) : available;
        DWORD downloaded = 0;
        if (!WinHttpReadData(h.request, buffer, to_read, &downloaded)) break;
        if (downloaded == 0) break;

        response.body.append(buffer, downloaded);

        if (on_line) {
            pending.append(buffer, downloaded);
            flush_lines(false);
        }
    }
    if (on_line) flush_lines(true);

    return response;
}
