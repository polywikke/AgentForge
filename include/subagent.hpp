#pragma once

#include "agent_loop.hpp"
#include "context_manager.hpp"
#include "loop_trace.hpp"
#include "tool_registry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// 子代理 (L11)
//
// 四个隔离边界：
//   1. 上下文隔离：全新 ContextManager + 独立 system prompt
//   2. 工具隔离：readOnlyView() 只保留 Read/Glob/Grep，并按名字排除 Task
//   3. UI 隔离：NoopUI（confirm 恒 false，不阻塞、不写主 UI）
//   4. 成本隔离：每个子代理返回自己的 Usage，由主线程汇总
// ============================================================================

struct SubagentSpec {
    std::string goal;
    std::string model;
    int max_steps = 10;
    int max_tokens = 60000;
};

struct SubagentResult {
    std::string goal;
    bool success = false;
    std::string summary;
    std::string error;
    Usage usage;
    int steps = 0;
    int tool_calls = 0;
    int64_t duration_ms = 0;
};

// 后台子代理使用：不渲染、不弹窗
class NoopUI : public IUIPort {
public:
    void output(const std::string&) override {}
    void error(const std::string&) override {}
    void progress(const std::string&, int) override {}
    bool confirm(const std::string&) override { return false; }
    std::string input(const std::string&) override { return ""; }
    void showToolCall(const ToolCall&) override {}
    void showToolResult(const ToolResult&) override {}
    void streamDelta(const std::string&) override {}
};

class SubagentRunner {
public:
    using EventCallback = std::function<void(const std::string&, const json&)>;

    SubagentRunner(std::shared_ptr<IModelProvider> provider,
                   ToolRegistry* tools,
                   std::shared_ptr<ILoopTracer> tracer = nullptr,
                   int max_depth = 2)
        : provider_(std::move(provider))
        , tools_(tools)
        , tracer_(std::move(tracer))
        , max_depth_(max_depth) {}

    void setEventCallback(EventCallback cb) { on_event_ = std::move(cb); }

    SubagentResult run(const SubagentSpec& spec, int depth = 0) {
        SubagentResult result;
        result.goal = spec.goal;
        auto start = nowMs();

        if (depth >= max_depth_) {
            result.error = "子代理递归深度超过上限 (" + std::to_string(max_depth_) + ")";
            result.duration_ms = nowMs() - start;
            return result;
        }
        if (!tools_ || !provider_) {
            result.error = "子代理缺少 provider/tools";
            result.duration_ms = nowMs() - start;
            return result;
        }

        try {
            // 工具隔离
            auto read_tools = tools_->readOnlyView("Task");
            if (!read_tools || read_tools->getAllTools().empty()) {
                result.error = "没有可用的只读工具";
                result.duration_ms = nowMs() - start;
                return result;
            }

            auto ui = std::make_shared<NoopUI>();
            AgentLoop loop(provider_, read_tools, ui);
            loop.setTracer(tracer_);

            // 上下文隔离
            BudgetConfig budget;
            budget.total_budget = spec.max_tokens;
            auto context = std::make_shared<ContextManager>(budget);
            context->setSystemPrompt(
                "你是一个只读子代理。你只能使用 Read/Glob/Grep 探索代码，"
                "不能修改文件、不能执行命令、不能再次派生子代理。\n"
                "你的最终回答必须是一份精简摘要：给出关键文件、关键结论、"
                "以及父代理下一步可以直接使用的结果，不要复述大段代码。");
            context->setMaxHistoryMessages(8);
            loop.setContextManager(context);

            LoopLimits limits;
            limits.max_steps = spec.max_steps;
            limits.max_tokens = spec.max_tokens;
            limits.max_duration_sec = 180;
            limits.max_repeat_calls = 3;
            limits.max_consecutive_errors = 3;
            limits.model = spec.model;

            if (on_event_) {
                on_event_("subagent_start", {{"goal", spec.goal}, {"depth", depth}});
            }

            std::vector<Message> messages;
            messages.emplace_back(Role::User, spec.goal);
            std::vector<Message> out_messages;
            LoopStats stats = loop.run(messages, limits, out_messages);

            result.steps = stats.total_steps;
            result.tool_calls = stats.tool_calls;
            result.usage = stats.total_usage;
            result.success = (stats.outcome == LoopOutcome::Success);

            // 取最后一条 assistant 文本作为摘要
            for (auto it = out_messages.rbegin(); it != out_messages.rend(); ++it) {
                if (it->role == Role::Assistant && it->tool_calls.empty() &&
                    !it->content.empty()) {
                    result.summary = sanitizeUtf8(it->content);
                    break;
                }
            }
            if (result.summary.empty()) {
                result.summary = stats.outcome_reason.empty()
                                     ? ("子任务结束: " + std::to_string((int)stats.outcome))
                                     : stats.outcome_reason;
            }

            if (on_event_) {
                on_event_("subagent_end",
                    {{"goal", spec.goal},
                     {"success", result.success},
                     {"tokens", result.usage.total()}});
            }
        } catch (const std::exception& e) {
            result.success = false;
            result.error = e.what();
            result.summary = "子任务异常: " + result.error;
        }

        result.duration_ms = nowMs() - start;
        return result;
    }

    std::vector<SubagentResult> runParallel(const std::vector<SubagentSpec>& specs,
                                            int max_concurrent = 4,
                                            int depth = 0) {
        std::vector<SubagentResult> results;
        if (specs.empty()) return results;

        max_concurrent = std::max(1, max_concurrent);
        std::atomic<std::size_t> next{0};
        std::mutex result_mutex;

        results.resize(specs.size());
        std::vector<std::thread> workers;
        int worker_count = std::min<int>(max_concurrent, (int)specs.size());

        for (int w = 0; w < worker_count; ++w) {
            workers.emplace_back([&]() {
                while (true) {
                    std::size_t i = next.fetch_add(1);
                    if (i >= specs.size()) break;
                    SubagentResult r = run(specs[i], depth + 1);
                    std::lock_guard<std::mutex> lock(result_mutex);
                    results[i] = std::move(r);
                }
            });
        }
        for (auto& t : workers) t.join();
        return results;
    }

private:
    std::shared_ptr<IModelProvider> provider_;
    ToolRegistry* tools_ = nullptr;
    std::shared_ptr<ILoopTracer> tracer_;
    int max_depth_ = 2;
    EventCallback on_event_;

    static int64_t nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
};

// ============================================================================
// Task 工具：一次派发 N 个只读子代理并合并结果
// ============================================================================

inline ToolDef makeTaskToolDef() {
    ToolDef def;
    def.name = "Task";
    def.description =
        "派发一个或多个只读子代理并行探索代码。每个子代理有独立上下文和只读工具"
        "（Read/Glob/Grep），只返回精简摘要，适合大范围调研，避免污染主上下文。";
    def.risk_level = RiskLevel::Read;
    def.parameters = {
        {"type", "object"},
        {"properties", {
            {"goals", {
                {"type", "array"},
                {"description", "子任务列表，每项是一句明确的调研目标"},
                {"items", {{"type", "string"}}}
            }},
            {"max_concurrent", {
                {"type", "integer"},
                {"description", "最大并发数，默认 4"}
            }}
        }},
        {"required", json::array({"goals"})}
    };
    return def;
}

inline ToolFunction makeTaskToolHandler(
    std::shared_ptr<IModelProvider> provider,
    ToolRegistry* tools,
    std::shared_ptr<ILoopTracer> tracer,
    int max_depth = 2,
    SubagentRunner::EventCallback on_event = nullptr,
    const std::string& model = "") {
    return [provider, tools, tracer, max_depth, on_event, model](const json& args) -> ToolResult {
        if (!args.contains("goals") || !args["goals"].is_array()) {
            return ToolResult::error("", "缺少 goals 数组");
        }

        std::vector<SubagentSpec> specs;
        for (const auto& goal : args["goals"]) {
            if (!goal.is_string()) continue;
            std::string text = goal.get<std::string>();
            if (text.empty()) continue;
            SubagentSpec spec;
            spec.goal = text;
            spec.model = model;
            specs.push_back(std::move(spec));
        }
        if (specs.empty()) {
            return ToolResult::error("", "goals 为空");
        }

        int concurrency = args.value("max_concurrent", 4);
        if (concurrency <= 0) concurrency = 4;

        auto runner = std::make_shared<SubagentRunner>(provider, tools, tracer, max_depth);
        if (on_event) runner->setEventCallback(on_event);
        std::vector<SubagentResult> results = runner->runParallel(specs, concurrency, 0);

        std::ostringstream oss;
        Usage total;
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            oss << "## 子代理 " << (i + 1) << ": " << r.goal << "\n";
            if (!r.success && !r.error.empty()) oss << "状态: 失败 (" << r.error << ")\n";
            oss << r.summary << "\n";
            oss << "（步数 " << r.steps << "，工具 " << r.tool_calls
                << "，tokens " << r.usage.total() << "）\n\n";
            total += r.usage;
        }
        oss << "—— 子代理汇总：tokens " << total.total()
            << "（prompt " << total.prompt_tokens
            << " / completion " << total.completion_tokens << "）";

        return ToolResult{"", sanitizeUtf8(oss.str())};
    };
}

inline void registerTaskTool(ToolRegistry& registry,
                             std::shared_ptr<IModelProvider> provider,
                             std::shared_ptr<ILoopTracer> tracer = nullptr,
                             int max_depth = 2,
                             SubagentRunner::EventCallback on_event = nullptr,
                             const std::string& model = "") {
    registry.registerTool(makeTaskToolDef(),
        makeTaskToolHandler(std::move(provider), &registry, std::move(tracer),
                            max_depth, std::move(on_event), model));
}
