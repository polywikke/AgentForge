#pragma once

#include "types.hpp"
#include "tool_registry.hpp"

#include <functional>
#include <string>

// ============================================================================
// SSE UI Adapter - 将 Agent Loop 的 UI 事件转换为 SSE 事件
//
// 事件协议 (与 web/index.html 一致):
//   stage       {stage, message, percent}
//   chunk       {content}
//   tool_call   {id, name, arguments}
//   tool_result {success, content}
//   error       {message}
//   done        {usage, model, steps, tool_calls, duration_ms}
// ============================================================================
class SSEUIAdapter : public IUIPort {
public:
    using EventSender = std::function<void(const std::string&, const json&)>;
    using ConfirmToolHandler = std::function<bool(const std::string&, const json&)>;

    explicit SSEUIAdapter(EventSender sender, ConfirmToolHandler confirm_tool = nullptr)
        : sender_(std::move(sender)), confirm_tool_(std::move(confirm_tool)) {}

    void sendStart(const std::string& request_id) {
        sender_("start", {{"request_id", request_id}});
    }

    void output(const std::string& message) override {
        if (!message.empty()) {
            sender_("chunk", {{"content", message}});
        }
    }

    void streamDelta(const std::string& delta) override {
        if (!delta.empty()) {
            sender_("chunk", {{"content", delta}});
        }
    }

    void error(const std::string& message) override {
        sender_("error", {{"message", message}});
    }

    void progress(const std::string& message, int percent) override {
        sender_("stage", {
            {"stage", "progress"},
            {"message", message},
            {"percent", percent}
        });
    }

    void showToolCall(const ToolCall& call) override {
        sender_("tool_call", {
            {"id", call.id},
            {"name", call.name},
            {"arguments", call.arguments}
        });
    }

    void showToolResult(const ToolResult& result) override {
        std::string content = result.content;
        if (content.size() > 2000) {
            content = utf8Prefix(content, 2000) + "... (已截断)";
        }
        sender_("tool_result", {
            {"success", !result.is_error},
            {"blocked", result.blocked_by_sandbox},
            {"block_reason", result.block_reason},
            {"content", content}
        });
    }

    bool confirm(const std::string& prompt) override {
        if (!confirm_tool_) return true;
        return confirm_tool_("", json{{"prompt", prompt}});
    }

    bool confirmTool(const std::string& tool_name, const json& args) override {
        if (!confirm_tool_) return true;
        return confirm_tool_(tool_name, args);
    }

    std::string input(const std::string& /*prompt*/) override {
        return "";
    }

private:
    EventSender sender_;
    ConfirmToolHandler confirm_tool_;
};
