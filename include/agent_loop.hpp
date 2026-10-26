#pragma once

#include "types.hpp"
#include "model_provider.hpp"
#include "tool_registry.hpp"
#include "context_manager.hpp"
#include "loop_trace.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <vector>

// ============================================================================
// Agent 循环 - 核心执行结构 (L5/L6)
//
// 控制流: 上下文组装 → 调用模型 → 执行工具 → 回写结果 → 重复
// 六道闸门:
//   1 max_steps
//   2 max_tokens
//   3 max_wall_clock
//   4 max_consecutive_errors
//   5 max_repeat_calls
//   6 user_interrupt
// ============================================================================

struct LoopContext {
    std::vector<Message> messages;
    Usage accumulated_usage;
    int step_count = 0;
    int64_t start_time_ms = 0;
    std::atomic<bool> should_stop{false};

    LoopContext() = default;
};

// 循环统计
struct LoopStats {
    int total_steps = 0;
    int tool_calls = 0;
    int compactions = 0;
    int permission_denials = 0;
    int context_tokens = 0;
    bool cancelled = false;
    Usage total_usage;
    int64_t duration_ms = 0;
    LoopOutcome outcome = LoopOutcome::Success;
    std::string outcome_reason;
    std::vector<CompactionEvent> compaction_events;
};

// 六道闸门 (Guardrails)
struct Guardrails {
    bool checkMaxSteps(int current, int max) const { return current < max; }
    bool checkBudget(int used, int max) const { return used < max; }
    bool checkDuration(int64_t elapsed_ms, int max_ms) const {
        return elapsed_ms < max_ms;
    }
    bool checkConsecutiveErrors(int count, int max) const {
        return count < max;
    }
    bool checkRepeatCalls(const std::map<std::string, int>& call_counts,
                          int max_repeats) const {
        for (const auto& [key, count] : call_counts) {
            if (count >= max_repeats) return false;
        }
        return true;
    }
    bool checkUserInterrupt(bool should_stop) const { return !should_stop; }
};

// 追踪端口（避免 AgentLoop 依赖具体 Tracer 实现）定义于 loop_trace.hpp

class AgentLoop {
public:
    AgentLoop(std::shared_ptr<IModelProvider> provider,
              std::shared_ptr<ToolRegistry> tools,
              std::shared_ptr<IUIPort> ui)
        : provider_(std::move(provider))
        , tools_(std::move(tools))
        , ui_(std::move(ui))
    {}

    void setPermissionGate(std::shared_ptr<IPermissionGate> gate) {
        gate_ = std::move(gate);
    }
    void setContextManager(std::shared_ptr<ContextManager> context) {
        context_ = std::move(context);
    }
    void setTracer(std::shared_ptr<ILoopTracer> tracer) {
        tracer_ = std::move(tracer);
    }
    void setCancelFlag(std::shared_ptr<std::atomic<bool>> flag) {
        external_cancel_ = std::move(flag);
    }
    void setToolBeforeHook(std::function<void(const ToolCall&)> hook) {
        tool_before_hook_ = std::move(hook);
    }

    LoopStats run(const std::string& task,
                  const LoopLimits& limits = LoopLimits()) {
        LoopContext ctx;
        ctx.messages.push_back(Message(Role::User, task));
        return runWithContext(ctx, limits);
    }

    LoopStats run(const std::vector<Message>& messages,
                  const LoopLimits& limits = LoopLimits()) {
        LoopContext ctx;
        ctx.messages = messages;
        return runWithContext(ctx, limits);
    }

    // 运行并返回最终历史（Web 会话持久化需要）
    LoopStats run(const std::vector<Message>& messages,
                  const LoopLimits& limits,
                  std::vector<Message>& out_messages) {
        LoopContext ctx;
        ctx.messages = messages;
        LoopStats stats = runWithContext(ctx, limits);
        out_messages = ctx.messages;
        return stats;
    }

    // 用户中断（Web /api/cancel 调用）
    void requestStop() {
        stop_requested_.store(true);
    }

private:
    std::shared_ptr<IModelProvider> provider_;
    std::shared_ptr<ToolRegistry> tools_;
    std::shared_ptr<IUIPort> ui_;
    std::shared_ptr<IPermissionGate> gate_;
    std::shared_ptr<ContextManager> context_;
    std::shared_ptr<ILoopTracer> tracer_;
    std::shared_ptr<std::atomic<bool>> external_cancel_;
    std::atomic<bool> stop_requested_{false};
    std::function<void(const ToolCall&)> tool_before_hook_;

    bool cancelled() const {
        if (stop_requested_.load()) return true;
        if (external_cancel_ && external_cancel_->load()) return true;
        return false;
    }

    LoopStats runWithContext(LoopContext& ctx, const LoopLimits& limits) {
        LoopStats stats;
        Guardrails guards;

        ctx.start_time_ms = getCurrentTimeMs();
        std::map<std::string, int> call_counts;
        int consecutive_errors = 0;
        bool aborted = false;

        if (tracer_) {
            tracer_->spanStart("agent.run", {{"model", limits.model}});
        }

        while (true) {
            // ---- 闸门 1: 步数 ----
            if (!guards.checkMaxSteps(ctx.step_count, limits.max_steps)) {
                stats.outcome = LoopOutcome::MaxSteps;
                stats.outcome_reason = "达到最大步数限制";
                break;
            }
            // ---- 闸门 2: token 预算 ----
            if (!guards.checkBudget(ctx.accumulated_usage.total(), limits.max_tokens)) {
                stats.outcome = LoopOutcome::BudgetExceeded;
                stats.outcome_reason = "超出 token 预算";
                break;
            }
            // ---- 闸门 3: 墙钟 ----
            int64_t elapsed = getCurrentTimeMs() - ctx.start_time_ms;
            if (!guards.checkDuration(elapsed, limits.max_duration_sec * 1000)) {
                stats.outcome = LoopOutcome::Timeout;
                stats.outcome_reason = "超出时间限制";
                break;
            }
            // ---- 闸门 6: 用户中断 ----
            if (cancelled() || !guards.checkUserInterrupt(ctx.should_stop.load())) {
                stats.outcome = LoopOutcome::UserInterrupt;
                stats.outcome_reason = "用户中断";
                stats.cancelled = true;
                break;
            }
            // ---- 闸门 4: 连续错误 ----
            if (!guards.checkConsecutiveErrors(consecutive_errors,
                                               limits.max_consecutive_errors)) {
                stats.outcome = LoopOutcome::Error;
                stats.outcome_reason = "连续错误次数超过上限 (" +
                                       std::to_string(limits.max_consecutive_errors) + ")";
                break;
            }

            // ---- 上下文压缩 + 组装 ----
            if (context_ && limits.enable_compaction &&
                context_->shouldCompact(ctx.messages, ctx.accumulated_usage)) {
                CompactionEvent event;
                Usage summary_usage;
                if (tracer_) tracer_->spanStart("compaction");
                std::vector<Message> compacted =
                    context_->compact(ctx.messages, *provider_, event, summary_usage);
                ctx.messages.swap(compacted);
                ctx.accumulated_usage += summary_usage;
                stats.compactions++;
                stats.compaction_events.push_back(event);
                if (tracer_) {
                    tracer_->spanEnd("compaction",
                        {{"level", event.level},
                         {"before", std::to_string(event.tokens_before)},
                         {"after", std::to_string(event.tokens_after)}});
                    tracer_->event("compaction", {{"level", event.level}});
                }
                ui_->progress("上下文已压缩 (" + event.level + ")", -1);
            }

            ChatRequest request;
            request.messages = context_ ? context_->build(ctx.messages) : ctx.messages;
            request.tools = tools_->getAllTools();
            request.model = limits.model;
            if (context_) {
                stats.context_tokens = context_->lastContextTokens();
            } else {
                int estimated = 0;
                for (const auto& m : request.messages) {
                    estimated += static_cast<int>(m.content.size() / 3) + 1;
                }
                stats.context_tokens = estimated;
            }

            ui_->progress("正在思考...", -1);

            if (tracer_) {
                tracer_->spanStart("chat",
                    {{"messages", std::to_string(request.messages.size())},
                     {"tools", std::to_string(request.tools.size())}});
            }

            std::string streamed_text;
            ChatResponse response = provider_->chatStream(
                request,
                [&](const ChatChunk& chunk) {
                    if (cancelled()) return;  // 流式回调中尽快停止渲染
                    std::visit(overloaded{
                        [&](const ChunkDelta& d) {
                            if (d.content.empty()) return;
                            streamed_text += d.content;
                            ui_->streamDelta(d.content);
                        },
                        [&](const ChunkError& e) {
                            ui_->error(e.message);
                        },
                        [&](const auto&) {}
                    }, chunk);
                });

            if (tracer_) {
                tracer_->spanEnd("chat",
                    {{"error", response.is_error ? "true" : "false"},
                     {"prompt_tokens", std::to_string(response.usage.prompt_tokens)},
                     {"completion_tokens", std::to_string(response.usage.completion_tokens)}});
            }

            if (cancelled()) {
                stats.outcome = LoopOutcome::UserInterrupt;
                stats.outcome_reason = "用户中断";
                stats.cancelled = true;
                break;
            }

            if (response.is_error) {
                ++consecutive_errors;
                if (consecutive_errors >= limits.max_consecutive_errors) {
                    stats.outcome = LoopOutcome::Error;
                    stats.outcome_reason = response.error_message;
                    break;
                }
                // 回喂错误信息，让模型/循环有机会调整
                ctx.messages.emplace_back(Role::System,
                    "[模型请求失败] " + response.error_message +
                    "。请重试或换一种方式。");
                ui_->error("模型请求失败: " + response.error_message);
                continue;
            }

            consecutive_errors = 0;
            ctx.accumulated_usage += response.usage;
            ctx.step_count++;
            stats.total_steps = ctx.step_count;

            if (response.message.tool_calls.empty()) {
                if (response.message.content.empty() && !streamed_text.empty()) {
                    response.message.content = streamed_text;
                }
                if (streamed_text.empty()) {
                    ui_->output(response.message.content);
                }
                // 把最终 assistant 消息写入历史（会话持久化/子代理摘要依赖它）
                ctx.messages.push_back(response.message);
                stats.outcome = LoopOutcome::Success;
                break;
            }

            ctx.messages.push_back(response.message);

            int auto_id = 0;
            for (auto& call : ctx.messages.back().tool_calls) {
                if (call.id.empty()) {
                    call.id = "call_" + std::to_string(ctx.step_count) + "_" +
                              std::to_string(auto_id++);
                }
            }

            for (const auto& call : ctx.messages.back().tool_calls) {
                if (cancelled()) {
                    aborted = true;
                    stats.outcome = LoopOutcome::UserInterrupt;
                    stats.outcome_reason = "用户中断";
                    stats.cancelled = true;
                    break;
                }

                // ---- 闸门 5: 重复调用 ----
                std::string signature = call.name + "|" + call.arguments.dump();
                int count = ++call_counts[signature];
                if (count > limits.max_repeat_calls) {
                    stats.outcome = LoopOutcome::Error;
                    stats.outcome_reason = "重复工具调用超过上限 (" +
                                           std::to_string(limits.max_repeat_calls) +
                                           "): " + call.name;
                    aborted = true;
                    break;
                }

                ui_->showToolCall(call);
                stats.tool_calls++;

                if (tracer_) {
                    tracer_->spanStart("tool." + call.name,
                        {{"args", utf8Prefix(call.arguments.dump(), 300)}});
                }

                ToolInvocationContext tctx;
                tctx.gate = gate_.get();
                tctx.ui = ui_.get();
                tctx.dump_dir = limits.dump_dir;
                tctx.persist_threshold = limits.persist_threshold;
                tctx.before_execute = tool_before_hook_;
                ToolResult result = tools_->execute(call, &tctx);

                if (result.content.find("被权限策略拒绝") != std::string::npos ||
                    result.content.find("用户拒绝了这次操作") != std::string::npos) {
                    stats.permission_denials++;
                }

                if (tracer_) {
                    tracer_->spanEnd("tool." + call.name,
                        {{"error", result.is_error ? "true" : "false"},
                         {"bytes", std::to_string(result.content.size())}});
                }

                ui_->showToolResult(result);

                std::string content = result.content.empty() ? "(空结果)" : result.content;
                ctx.messages.push_back(Message::toolResult(call.id, content));
            }

            if (aborted) break;
        }

        stats.total_usage = ctx.accumulated_usage;
        stats.duration_ms = getCurrentTimeMs() - ctx.start_time_ms;

        if (tracer_) {
            tracer_->spanEnd("agent.run",
                {{"outcome", std::to_string(static_cast<int>(stats.outcome))},
                 {"steps", std::to_string(stats.total_steps)},
                 {"tokens", std::to_string(stats.total_usage.total())}});
        }

        return stats;
    }

    int64_t getCurrentTimeMs() const {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
};
