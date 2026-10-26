#pragma once
#include "http_transport.hpp"
#include <string>
#include <map>

// ============================================================================
// SimpleHTTPTransport - 基于 WinHTTP 的 HTTP 传输实现
//
// 支持:
//   - 同步 POST/GET 请求
//   - SSE 流式请求 (逐行回调, 供 chatStream 使用)
//   - 自签名/中转站证书容错
// ============================================================================

class SimpleHTTPTransport : public IHttpTransport {
public:
    SimpleHTTPTransport(const std::string& base_url = "", const std::string& api_key = "")
        : base_url_(base_url), api_key_(api_key) {}

    void setBaseURL(const std::string& url) { base_url_ = url; }
    void setAPIKey(const std::string& key) { api_key_ = key; }

    const std::string& baseURL() const { return base_url_; }

    // IHttpTransport
    HttpResponse request(const HttpRequest& req) override;
    HttpResponse requestStream(const HttpRequest& req,
                               IHttpTransport::StreamCallback on_line) override;

private:
    std::string base_url_;
    std::string api_key_;

    // 内部实现: streaming 为 true 时逐行回调
    HttpResponse perform(const HttpRequest& req, IHttpTransport::StreamCallback* on_line);
};
