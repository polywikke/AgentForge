#include "multi_provider.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace agentforge {

namespace {

std::string trimCopy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

ChatResponse makeError(const std::string& message) {
    ChatResponse resp;
    resp.is_error = true;
    resp.error_message = message;
    resp.finish_reason = FinishReason::Error;
    resp.message.role = Role::Assistant;
    resp.message.content = "API Error: " + message;
    return resp;
}

void applyUsageFromJson(Usage& usage, const json& j) {
    if (!j.contains("usage") || !j["usage"].is_object()) return;
    const auto& u = j["usage"];
    if (u.contains("prompt_tokens")) usage.prompt_tokens = u.value("prompt_tokens", 0);
    if (u.contains("input_tokens")) usage.prompt_tokens = u.value("input_tokens", 0);
    if (u.contains("completion_tokens")) usage.completion_tokens = u.value("completion_tokens", 0);
    if (u.contains("output_tokens")) usage.completion_tokens = u.value("output_tokens", 0);
    if (u.contains("prompt_cache_hit_tokens"))
        usage.cache_read_tokens = u.value("prompt_cache_hit_tokens", 0);

    // 部分中转站只返回 total_tokens, 用总量反推 completion
    if (usage.completion_tokens == 0 && u.contains("total_tokens")) {
        int total = u.value("total_tokens", 0);
        int rest = total - usage.prompt_tokens - usage.cache_read_tokens;
        if (rest > 0) usage.completion_tokens = rest;
    }
}

} // namespace

MultiAPIProvider::MultiAPIProvider(IHttpTransport& transport, const APIConfig& config)
    : transport_(transport), config_(config) {}

void MultiAPIProvider::updateConfig(const APIConfig& config) {
    config_ = config;
}

std::string MultiAPIProvider::name() const {
    switch (config_.provider) {
        case APIConfig::Anthropic: return "Anthropic";
        case APIConfig::OpenAI: return "OpenAI";
        case APIConfig::DeepSeek: return "DeepSeek";
        case APIConfig::Custom: return "Custom";
        default: return "Unknown";
    }
}

HttpRequest MultiAPIProvider::buildHttpRequest(const ChatRequest& request, bool stream) const {
    HttpRequest http_req;
    http_req.url = config_.getEndpoint();
    http_req.method = "POST";
    http_req.timeout_ms = 180000;

    if (config_.getProviderType() == APIConfig::Anthropic) {
        http_req.headers["x-api-key"] = config_.api_key;
        http_req.headers["anthropic-version"] = "2023-06-01";
    } else {
        http_req.headers["Authorization"] = "Bearer " + config_.api_key;
    }
    http_req.headers["Content-Type"] = "application/json";
    if (stream) http_req.headers["Accept"] = "text/event-stream";

    http_req.body = buildRequestBody(request, stream).dump();
    return http_req;
}

ChatResponse MultiAPIProvider::chat(const ChatRequest& request) {
    HttpRequest http_req = buildHttpRequest(request, false);
    HttpResponse http_resp = transport_.request(http_req);

    // 某些模型 (如 claude-sonnet-5-5) 只接受默认 temperature, 返回 400。
    // 这里自动去掉 temperature 重试一次, 提升兼容性。
    if (!http_resp.is_error && http_resp.status_code == 400 &&
        http_resp.body.find("temperature") != std::string::npos) {
        try {
            json body = json::parse(http_req.body);
            body.erase("temperature");
            http_req.body = body.dump();
            std::cerr << "[Provider] Retrying without temperature\n";
            http_resp = transport_.request(http_req);
        } catch (...) {
        }
    }

    if (http_resp.is_error) {
        return makeError(http_resp.error_message);
    }
    if (http_resp.status_code >= 400) {
        std::string detail = http_resp.body;
        try {
            json j = json::parse(http_resp.body);
            if (j.contains("error")) {
                detail = j["error"].is_string() ? j["error"].get<std::string>()
                                                : j["error"].dump();
            }
        } catch (...) {
        }
        return makeError("HTTP " + std::to_string(http_resp.status_code) + ": " + detail);
    }

    try {
        return parseResponse(json::parse(http_resp.body));
    } catch (const std::exception& e) {
        return makeError(std::string("Parse Error: ") + e.what());
    }
}

ChatResponse MultiAPIProvider::chatStream(const ChatRequest& request,
                                          IModelProvider::StreamCallback on_chunk) {
    if (config_.getProviderType() == APIConfig::Anthropic) {
        return chatStreamAnthropic(request, std::move(on_chunk));
    }
    return chatStreamOpenAI(request, std::move(on_chunk));
}

ChatResponse MultiAPIProvider::chatStreamOpenAI(const ChatRequest& request,
                                                IModelProvider::StreamCallback on_chunk) {
    ChatResponse acc;
    acc.message.role = Role::Assistant;

    std::map<int, ToolCall> calls;
    std::map<int, std::string> arg_buffers;
    bool got_content = false;
    bool stream_failed = false;
    std::string stream_error;
    FinishReason finish = FinishReason::Stop;

    auto emit = [&on_chunk](const ChatChunk& chunk) {
        if (on_chunk) on_chunk(chunk);
    };

    auto on_line = [&](const std::string& line) {
        std::string s = trimCopy(line);
        if (s.rfind("data:", 0) != 0) return;

        std::string payload = trimCopy(s.substr(5));
        if (payload.empty() || payload == "[DONE]") return;

        json j;
        try {
            j = json::parse(payload);
        } catch (...) {
            return;
        }

        if (j.contains("error")) {
            stream_failed = true;
            stream_error = j["error"].is_string() ? j["error"].get<std::string>()
                                                  : j["error"].dump();
            return;
        }

        applyUsageFromJson(acc.usage, j);

        if (!j.contains("choices") || j["choices"].empty()) return;
        const auto& choice = j["choices"][0];

        if (choice.contains("delta") && choice["delta"].is_object()) {
            const auto& delta = choice["delta"];

            if (delta.contains("content") && !delta["content"].is_null()) {
                std::string content = delta["content"].get<std::string>();
                if (!content.empty()) {
                    got_content = true;
                    acc.message.content += content;
                    emit(ChunkDelta{content});
                }
            }

            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                for (const auto& tc : delta["tool_calls"]) {
                    int index = tc.value("index", 0);
                    ToolCall& call = calls[index];

                    if (tc.contains("id") && !tc["id"].is_null()) {
                        call.id = tc["id"].get<std::string>();
                    }

                    if (tc.contains("function") && tc["function"].is_object()) {
                        const auto& fn = tc["function"];

                        if (fn.contains("name") && !fn["name"].is_null()) {
                            std::string name = fn["name"].get<std::string>();
                            if (call.name.empty() && !name.empty()) {
                                call.name = name;
                                emit(ChunkToolCallStart{call.id, call.name});
                            } else {
                                call.name += name;
                            }
                        }

                        if (fn.contains("arguments") && !fn["arguments"].is_null()) {
                            std::string args = fn["arguments"].get<std::string>();
                            arg_buffers[index] += args;
                            emit(ChunkToolCallDelta{call.id, args});
                        }
                    }
                }
            }
        }

        if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
            std::string reason = choice["finish_reason"].get<std::string>();
            if (reason == "tool_calls") finish = FinishReason::ToolCalls;
            else if (reason == "length") finish = FinishReason::Length;
            else if (reason == "stop") finish = FinishReason::Stop;
        }
    };

    HttpResponse http_resp = transport_.requestStream(buildHttpRequest(request, true), on_line);

    bool stream_usable = !stream_failed && !http_resp.is_error &&
                         http_resp.status_code < 400 &&
                         (got_content || !calls.empty());

    if (!stream_usable) {
        // 流式失败 (中转站不支持 stream / 解析异常) → 回退非流式, 保证可用性
        if (!stream_error.empty()) {
            std::cerr << "[Provider] Stream error, fallback: " << stream_error << "\n";
        } else if (http_resp.status_code >= 400) {
            std::cerr << "[Provider] Stream HTTP " << http_resp.status_code
                      << ", fallback to non-stream\n";
        }
        return chat(request);
    }

    for (auto& [index, call] : calls) {
        const std::string& raw_args = arg_buffers[index];
        if (raw_args.empty()) {
            call.arguments = json::object();
        } else {
            try {
                call.arguments = json::parse(raw_args);
            } catch (...) {
                call.arguments = json::object();
            }
        }
        if (!call.name.empty()) {
            acc.message.tool_calls.push_back(call);
        }
    }

    acc.finish_reason = !acc.message.tool_calls.empty() ? FinishReason::ToolCalls : finish;
    emit(ChunkFinish{acc.finish_reason, acc.usage});

    return acc;
}

ChatResponse MultiAPIProvider::chatStreamAnthropic(const ChatRequest& request,
                                                   IModelProvider::StreamCallback on_chunk) {
    HttpRequest http_req = buildHttpRequest(request, true);
    ChatResponse acc;
    acc.message.role = Role::Assistant;

    std::map<int, ToolCall> tool_blocks;
    std::map<int, std::string> arg_buffers;
    bool got_content = false;
    bool got_any = false;
    bool stream_failed = false;
    std::string stream_error;
    FinishReason finish = FinishReason::Stop;

    auto emit = [&on_chunk](const ChatChunk& chunk) {
        if (on_chunk) on_chunk(chunk);
    };

    auto on_line = [&](const std::string& line) {
        std::string s = trimCopy(line);
        if (s.rfind("data:", 0) != 0) return;
        std::string payload = trimCopy(s.substr(5));
        if (payload.empty() || payload == "[DONE]") return;

        json j;
        try {
            j = json::parse(payload);
        } catch (...) {
            return;
        }

        std::string type = j.value("type", "");
        if (type == "error") {
            stream_failed = true;
            stream_error = j.contains("error") ? j["error"].dump() : payload;
            return;
        }

        if (type == "message_start" && j.contains("message")) {
            got_any = true;
            const auto& message = j["message"];
            if (message.contains("usage")) {
                acc.usage.prompt_tokens = message["usage"].value("input_tokens", 0);
                acc.usage.cache_read_tokens =
                    message["usage"].value("cache_read_input_tokens", 0);
            }
            return;
        }

        if (type == "content_block_start" && j.contains("content_block")) {
            got_any = true;
            int index = j.value("index", 0);
            const auto& block = j["content_block"];
            std::string block_type = block.value("type", "");
            if (block_type == "tool_use") {
                ToolCall call;
                call.id = block.value("id", "");
                call.name = block.value("name", "");
                tool_blocks[index] = call;
                emit(ChunkToolCallStart{call.id, call.name});
            }
            return;
        }

        if (type == "content_block_delta" && j.contains("delta")) {
            got_any = true;
            int index = j.value("index", 0);
            const auto& delta = j["delta"];
            std::string delta_type = delta.value("type", "");
            if (delta_type == "text_delta") {
                std::string text = delta.value("text", "");
                if (!text.empty()) {
                    got_content = true;
                    acc.message.content += text;
                    emit(ChunkDelta{text});
                }
            } else if (delta_type == "input_json_delta") {
                std::string piece = delta.value("partial_json", "");
                arg_buffers[index] += piece;
                auto it = tool_blocks.find(index);
                emit(ChunkToolCallDelta{it != tool_blocks.end() ? it->second.id : "", piece});
            }
            return;
        }

        if (type == "content_block_stop") {
            int index = j.value("index", 0);
            auto it = tool_blocks.find(index);
            if (it != tool_blocks.end()) {
                const std::string& raw = arg_buffers[index];
                if (raw.empty()) {
                    it->second.arguments = json::object();
                } else {
                    try {
                        it->second.arguments = json::parse(raw);
                    } catch (...) {
                        it->second.arguments = json::object();
                    }
                }
                if (!it->second.name.empty()) {
                    acc.message.tool_calls.push_back(it->second);
                }
                tool_blocks.erase(it);
            }
            return;
        }

        if (type == "message_delta") {
            if (j.contains("delta")) {
                const auto& delta = j["delta"];
                std::string reason = delta.value("stop_reason", "");
                if (reason == "tool_use") finish = FinishReason::ToolCalls;
                else if (reason == "max_tokens") finish = FinishReason::Length;
                else if (reason == "end_turn") finish = FinishReason::Stop;
            }
            if (j.contains("usage")) {
                acc.usage.completion_tokens = j["usage"].value("output_tokens", 0);
            }
            return;
        }

        if (type == "message_stop") {
            got_any = true;
            return;
        }
    };

    HttpResponse http_resp = transport_.requestStream(http_req, on_line);
    bool usable = !stream_failed && !http_resp.is_error &&
                  http_resp.status_code < 400 && got_any &&
                  (got_content || !acc.message.tool_calls.empty());

    if (!usable) {
        // 流式不可用（中转站不支持 Anthropic stream 等）→ 回退非流式
        return chat(request);
    }

    acc.finish_reason = acc.message.tool_calls.empty() ? finish : FinishReason::ToolCalls;
    emit(ChunkFinish{acc.finish_reason, acc.usage});
    return acc;
}

json MultiAPIProvider::buildRequestBody(const ChatRequest& request, bool stream) const {
    if (config_.getProviderType() == APIConfig::Anthropic) {
        return buildAnthropicRequest(request, stream);
    }
    return buildOpenAIRequest(request, stream);
}

json MultiAPIProvider::buildAnthropicRequest(const ChatRequest& request, bool stream) const {
    json req;
    req["model"] = config_.model;
    if (config_.max_tokens > 0) req["max_tokens"] = config_.max_tokens;
    req["temperature"] = config_.temperature;
    if (stream) req["stream"] = true;

    json messages = json::array();
    std::string system_prompt;

    for (const auto& msg : request.messages) {
        if (msg.role == Role::System) {
            system_prompt += (system_prompt.empty() ? "" : "\n") + msg.content;
            continue;
        }

        // 工具结果消息
        if (msg.call_id.has_value()) {
            json block;
            block["type"] = "tool_result";
            block["tool_use_id"] = *msg.call_id;
            block["content"] = sanitizeUtf8(msg.content);
            json m;
            m["role"] = "user";
            m["content"] = json::array({block});
            messages.push_back(m);
            continue;
        }

        json m;
        m["role"] = (msg.role == Role::User) ? "user" : "assistant";

        if (!msg.tool_calls.empty()) {
            json blocks = json::array();
            if (!msg.content.empty()) {
                blocks.push_back({{"type", "text"}, {"text", msg.content}});
            }
            for (const auto& call : msg.tool_calls) {
                blocks.push_back({
                    {"type", "tool_use"},
                    {"id", call.id},
                    {"name", call.name},
                    {"input", call.arguments}
                });
            }
            m["content"] = blocks;
        } else {
            m["content"] = sanitizeUtf8(msg.content);
        }
        messages.push_back(m);
    }

    if (!system_prompt.empty()) {
        req["system"] = sanitizeUtf8(system_prompt);
    }
    req["messages"] = messages;

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& tool : request.tools) {
            json t;
            t["name"] = tool.name;
            t["description"] = sanitizeUtf8(tool.description);
            t["input_schema"] = tool.parameters;
            tools.push_back(t);
        }
        req["tools"] = tools;
    }

    return req;
}

json MultiAPIProvider::buildOpenAIRequest(const ChatRequest& request, bool stream) const {
    json req;
    req["model"] = config_.model;
    if (config_.max_tokens > 0) req["max_tokens"] = config_.max_tokens;
    req["temperature"] = config_.temperature;
    if (stream) {
        req["stream"] = true;
        req["stream_options"] = {{"include_usage", true}};
    }

    json messages = json::array();
    for (const auto& msg : request.messages) {
        // 工具结果消息
        if (msg.call_id.has_value()) {
            messages.push_back({
                {"role", "tool"},
                {"tool_call_id", *msg.call_id},
                {"content", sanitizeUtf8(msg.content)}
            });
            continue;
        }

        json message;
        if (msg.role == Role::System) {
            message["role"] = "system";
        } else if (msg.role == Role::User) {
            message["role"] = "user";
        } else {
            message["role"] = "assistant";
        }

        if (!msg.tool_calls.empty()) {
            json tool_calls = json::array();
            for (const auto& call : msg.tool_calls) {
                tool_calls.push_back({
                    {"id", call.id},
                    {"type", "function"},
                    {"function", {
                        {"name", call.name},
                        {"arguments", call.arguments.dump()}
                    }}
                });
            }
            message["tool_calls"] = tool_calls;
            if (!msg.content.empty()) message["content"] = sanitizeUtf8(msg.content);
        } else {
            message["content"] = sanitizeUtf8(msg.content);
        }

        messages.push_back(message);
    }

    req["messages"] = messages;

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& tool : request.tools) {
            json t;
            t["type"] = "function";
            json func;
            func["name"] = tool.name;
            func["description"] = sanitizeUtf8(tool.description);
            func["parameters"] = tool.parameters;
            t["function"] = func;
            tools.push_back(t);
        }
        req["tools"] = tools;
    }

    return req;
}

ChatResponse MultiAPIProvider::parseResponse(const nlohmann::json& response) const {
    if (response.contains("error")) {
        std::string msg = response["error"].is_string()
                              ? response["error"].get<std::string>()
                              : response["error"].dump();
        return makeError(msg);
    }
    if (config_.getProviderType() == APIConfig::Anthropic) {
        return parseAnthropicResponse(response);
    }
    return parseOpenAIResponse(response);
}

ChatResponse MultiAPIProvider::parseAnthropicResponse(const json& response) const {
    ChatResponse chat_resp;

    applyUsageFromJson(chat_resp.usage, response);

    std::string stop_reason = response.value("stop_reason", "end_turn");
    if (stop_reason == "max_tokens") chat_resp.finish_reason = FinishReason::Length;
    else if (stop_reason == "tool_use") chat_resp.finish_reason = FinishReason::ToolCalls;
    else chat_resp.finish_reason = FinishReason::Stop;

    Message msg;
    msg.role = Role::Assistant;

    if (response.contains("content") && response["content"].is_array()) {
        for (const auto& block : response["content"]) {
            std::string type = block.value("type", "");
            if (type == "text") {
                msg.content += block.value("text", "");
            } else if (type == "tool_use") {
                ToolCall call;
                call.id = block.value("id", "");
                call.name = block.value("name", "");
                call.arguments = block.value("input", json::object());
                msg.tool_calls.push_back(call);
            }
        }
    }

    if (!msg.tool_calls.empty()) {
        chat_resp.finish_reason = FinishReason::ToolCalls;
    }
    chat_resp.message = msg;
    return chat_resp;
}

ChatResponse MultiAPIProvider::parseOpenAIResponse(const json& response) const {
    ChatResponse chat_resp;

    applyUsageFromJson(chat_resp.usage, response);

    if (!response.contains("choices") || response["choices"].empty()) {
        return makeError("响应中没有 choices 字段: " + response.dump());
    }

    const auto& choice = response["choices"][0];
    const auto& message = choice.contains("message") ? choice["message"] : json::object();

    Message msg;
    msg.role = Role::Assistant;

    if (message.contains("content") && !message["content"].is_null()) {
        msg.content = message["content"].get<std::string>();
    }

    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
        for (const auto& tc : message["tool_calls"]) {
            ToolCall call;
            call.id = tc.value("id", "");

            if (tc.contains("function") && tc["function"].is_object()) {
                const auto& fn = tc["function"];
                call.name = fn.value("name", "");
                std::string args = fn.value("arguments", "");
                if (args.empty()) {
                    call.arguments = json::object();
                } else {
                    try {
                        call.arguments = json::parse(args);
                    } catch (...) {
                        call.arguments = json::object();
                    }
                }
            }
            msg.tool_calls.push_back(call);
        }
    }

    chat_resp.message = msg;

    std::string finish = choice.value("finish_reason", "stop");
    if (finish == "length") chat_resp.finish_reason = FinishReason::Length;
    else if (finish == "tool_calls") chat_resp.finish_reason = FinishReason::ToolCalls;
    else if (finish == "content_filter") chat_resp.finish_reason = FinishReason::ContentFilter;
    else chat_resp.finish_reason = FinishReason::Stop;

    if (!msg.tool_calls.empty()) chat_resp.finish_reason = FinishReason::ToolCalls;

    return chat_resp;
}

} // namespace agentforge
