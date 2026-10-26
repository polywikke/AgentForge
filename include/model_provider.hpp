#pragma once

#include "types.hpp"
#include <functional>
#include <memory>
#include <map>

// ============================================================================
// IModelProvider - 模型服务抽象端口 (INV-2: 端口必须是抽象类)
// 隔离不受本系统控制的模型服务
// ============================================================================

class IModelProvider {
public:
    virtual ~IModelProvider() = default;

    // 非流式请求
    virtual ChatResponse chat(const ChatRequest& request) = 0;

    // 流式请求, 通过回调逐块返回
    // 返回值: 最终的 ChatResponse (聚合结果)
    using StreamCallback = std::function<void(const ChatChunk&)>;
    virtual ChatResponse chatStream(const ChatRequest& request,
                                   StreamCallback on_chunk) = 0;

    // 获取提供者名称
    virtual std::string name() const = 0;

    // 估算 token 数量 (用于预算管理)
    virtual int estimateTokens(const std::string& text) const {
        // 简单估算: 1 token ≈ 4 字符 (英文) 或 1.5 字符 (中文)
        // 实际应该调用 tokenizer, 这里提供默认实现
        return static_cast<int>(text.size() / 3);
    }

protected:
    // 防止直接实例化 (INV-2)
    IModelProvider() = default;
    IModelProvider(const IModelProvider&) = delete;
    IModelProvider& operator=(const IModelProvider&) = delete;
};

// ============================================================================
// HTTP 传输层抽象端口
// ============================================================================

struct HttpRequest {
    std::string url;
    std::string method = "POST";
    std::map<std::string, std::string> headers;
    std::string body;
    int timeout_ms = 30000;
};

struct HttpResponse {
    int status_code = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    bool is_error = false;
    std::string error_message;

    static HttpResponse error(const std::string& msg) {
        HttpResponse r;
        r.is_error = true;
        r.error_message = msg;
        return r;
    }
};

class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;

    // 同步请求
    virtual HttpResponse request(const HttpRequest& req) = 0;

    // 流式请求 (SSE)
    using StreamCallback = std::function<void(const std::string& line)>;
    virtual HttpResponse requestStream(const HttpRequest& req,
                                      StreamCallback on_line) = 0;

protected:
    IHttpTransport() = default;
    IHttpTransport(const IHttpTransport&) = delete;
    IHttpTransport& operator=(const IHttpTransport&) = delete;
};

// ============================================================================
// 请求追踪 (用于可观测性)
// ============================================================================

struct RequestTrace {
    std::string request_id;
    std::string model;
    int64_t start_time_ms = 0;
    int64_t end_time_ms = 0;
    Usage usage;
    bool is_error = false;
    std::string error_message;

    int durationMs() const {
        return static_cast<int>(end_time_ms - start_time_ms);
    }
};
