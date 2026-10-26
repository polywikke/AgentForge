#pragma once

// ============================================================================
// 上下文分层与压缩 (L5/L7)
//
// 八层结构的简化落地：
//   L0 system      系统提示
//   L1 project     项目知识（AGENTS.md / CLAUDE.md / MEMORY.md）
//   L2 repo map    代码地图
//   L3 retrieved   检索片段
//   L4 active      当前活动文件（用检索/工具结果近似）
//   L5 history     对话历史
//   L6 user input  当前用户输入
//
// 压缩级联（对齐课程 L7）：
//   L0 大结果落盘（由 ToolRegistry 完成）
//   L1 单条消息降级
//   L2 折叠旧工具结果
//   L3 摘要（LLM 或抽取式）
//   L4 丢弃老消息（保留首条任务 + 最近窗口）
// ============================================================================

#include "types.hpp"
#include "model_provider.hpp"
#include "win_utf8.hpp"

#include <algorithm>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

class ContextManager {
public:
    explicit ContextManager(const BudgetConfig& budget = BudgetConfig())
        : budget_(budget) {}

    void setBudget(const BudgetConfig& budget) { budget_ = budget; }
    BudgetConfig budget() const { return budget_; }

    void setSystemPrompt(const std::string& prompt) { system_prompt_ = prompt; }
    void setProjectKnowledge(const std::string& text) { project_knowledge_ = text; }
    void setRepoMap(const std::string& text) { repo_map_ = text; }
    void setRetrieved(const std::vector<RetrievedSnippet>& snippets) { retrieved_ = snippets; }
    void setUseLlmSummary(bool enabled) { use_llm_summary_ = enabled; }
    void setMaxHistoryMessages(int n) { max_history_messages_ = n; }

    int estimateTokens(const std::vector<Message>& messages) const {
        int total = 0;
        for (const auto& m : messages) {
            total += estimateText(m.content);
            for (const auto& call : m.tool_calls) {
                total += estimateText(call.name) + estimateText(call.arguments.dump());
            }
        }
        return total;
    }

    int estimateText(const std::string& text) const {
        // 粗略估算：中文约 1.5 字符/token，英文约 4 字符/token，取 3 作为折中
        return static_cast<int>(text.size() / 3) + 1;
    }

    // 是否需要在调用模型前压缩
    bool shouldCompact(const std::vector<Message>& history,
                       const Usage& accumulated) const {
        int system_tokens = estimateSystemTokens();
        int history_tokens = estimateTokens(history);
        int total = system_tokens + history_tokens;
        int limit = budget_.total_budget;
        if (accumulated.total() > limit * 0.90) return true;
        return total > static_cast<int>(limit * 0.85);
    }

    int estimateSystemTokens() const {
        return estimateText(system_prompt_) + estimateText(project_knowledge_) +
               estimateText(repo_map_) + estimateRetrieved();
    }

    // 构造本轮请求的消息（含分层 system，裁剪历史）
    std::vector<Message> build(const std::vector<Message>& history) {
        std::vector<Message> result;

        std::string system = sanitizeUtf8(buildSystemBlock());
        if (!system.empty()) {
            result.emplace_back(Role::System, system);
        }

        std::vector<Message> trimmed = trimHistory(history);
        for (auto& m : trimmed) { m.content = sanitizeUtf8(m.content); result.push_back(std::move(m)); }

        // 总预算兜底：从最老的非首条消息开始丢，直到进入预算
        int total = estimateTokens(result);
        while (total > budget_.total_budget && result.size() > 2) {
            size_t drop = 1;  // 保留系统消息
            if (result.size() > 1 && result[1].role == Role::System) drop = 1;
            if (drop < result.size() && result[drop].role == Role::User) {
                result.erase(result.begin() + (std::ptrdiff_t)drop);
            } else {
                // 找不到用户边界就停止，避免破坏 tool 配对
                break;
            }
            total = estimateTokens(result);
        }

        last_context_tokens_ = total;
        return result;
    }

    int lastContextTokens() const { return last_context_tokens_; }

    // 五级压缩。返回压缩后的历史；summary_usage 记录 LLM 摘要开销。
    std::vector<Message> compact(const std::vector<Message>& history,
                                 IModelProvider& provider,
                                 CompactionEvent& event,
                                 Usage& summary_usage) {
        event.level = "none";
        event.tokens_before = estimateTokens(history);
        event.tokens_after = event.tokens_before;
        event.reason.clear();

        std::vector<Message> work = history;
        int target = static_cast<int>(budget_.total_budget * 0.70);

        // L1: 单条消息降级
        if (estimateTokens(work) > target) {
            bool changed = false;
            for (auto& msg : work) {
                const std::size_t cap = 8000;
                if (msg.content.size() > cap) {
                    msg.content = utf8Prefix(msg.content, cap) +
                                  "\n...[单条消息过长，已降级截断]";
                    changed = true;
                }
            }
            if (changed) event.level = "L1";
        }

        // L2: 折叠旧工具结果（保留最近 3 条）
        if (estimateTokens(work) > target) {
            int tool_seen = 0;
            bool changed = false;
            for (auto it = work.rbegin(); it != work.rend(); ++it) {
                if (!it->call_id.has_value()) continue;
                ++tool_seen;
                if (tool_seen <= 3) continue;
                if (it->content.size() > 300) {
                    it->content = utf8Prefix(it->content, 300) +
                                  "\n...[旧工具结果已折叠]";
                    changed = true;
                }
            }
            if (changed) event.level = "L2";
        }

        // L3: 摘要老历史（保留最近 8 条）
        if (estimateTokens(work) > target && work.size() > 12) {
            size_t keep_tail = 8;
            size_t head_end = 1;
            // 找到第一个可摘要区间
            size_t cut = work.size() > keep_tail ? work.size() - keep_tail : 0;
            while (cut > head_end && work[cut].role != Role::User) --cut;
            if (cut > head_end) {
                std::vector<Message> middle(work.begin() + (std::ptrdiff_t)head_end,
                                            work.begin() + (std::ptrdiff_t)cut);
                std::string summary;
                if (use_llm_summary_) {
                    summary = summarizeWithLlm(provider, middle, summary_usage);
                }
                if (summary.empty()) summary = extractiveSummary(middle);

                std::vector<Message> compacted;
                for (size_t i = 0; i < head_end && i < work.size(); ++i) {
                    compacted.push_back(work[i]);
                }
                compacted.emplace_back(Role::System, "[历史摘要] " + summary);
                for (size_t i = cut; i < work.size(); ++i) {
                    compacted.push_back(work[i]);
                }
                work.swap(compacted);
                event.level = "L3";
            }
        }

        // L4: 丢弃老消息，保留首条任务 + 最近窗口
        if (estimateTokens(work) > target && work.size() > 6) {
            const size_t keep_tail = 6;
            size_t cut = work.size() - keep_tail;
            while (cut > 1 && work[cut].role != Role::User) --cut;
            if (cut > 1) {
                std::vector<Message> compacted;
                compacted.push_back(work.front());
                if (work.size() > 1 && work[1].role == Role::System) {
                    compacted.push_back(work[1]);
                }
                compacted.emplace_back(Role::System,
                    "[历史压缩] 已丢弃较早的 " + std::to_string(cut) + " 条消息。");
                for (size_t i = cut; i < work.size(); ++i) {
                    compacted.push_back(work[i]);
                }
                work.swap(compacted);
                event.level = "L4";
            }
        }

        event.tokens_after = estimateTokens(work);
        if (event.level == "none") {
            event.level = "L4";
            event.reason = "预算已满足，无需压缩";
        } else {
            event.reason = "预算压力触发五级压缩";
        }
        return work;
    }

    // 消息合法性守卫：删除孤立 tool 消息（call_id 找不到对应的 assistant.tool_calls）
    static std::vector<Message> sanitizeToolPairs(const std::vector<Message>& messages) {
        std::vector<Message> result;
        std::vector<std::string> pending;
        for (const auto& msg : messages) {
            if (!msg.tool_calls.empty()) {
                pending.clear();
                for (const auto& call : msg.tool_calls) pending.push_back(call.id);
                result.push_back(msg);
                continue;
            }
            if (msg.call_id.has_value()) {
                auto it = std::find(pending.begin(), pending.end(), *msg.call_id);
                if (it == pending.end()) continue;  // 孤儿 tool 消息，丢弃
                pending.erase(it);
                result.push_back(msg);
                continue;
            }
            // 普通消息：如果前面还有未配对的 tool 调用，说明配对窗口被打断
            pending.clear();
            result.push_back(msg);
        }
        return result;
    }

private:
    BudgetConfig budget_;
    std::string system_prompt_;
    std::string project_knowledge_;
    std::string repo_map_;
    std::vector<RetrievedSnippet> retrieved_;
    bool use_llm_summary_ = false;
    int max_history_messages_ = 40;
    int last_context_tokens_ = 0;

    std::string buildSystemBlock() const {
        std::string system = system_prompt_;
        appendSection(system, "项目知识", project_knowledge_,
                      budget_.l1_project * 3);
        appendSection(system, "代码地图", repo_map_,
                      budget_.l2_repo_map * 3);
        if (!retrieved_.empty()) {
            std::ostringstream oss;
            for (const auto& s : retrieved_) {
                oss << "- " << s.path << ":" << s.line << " (score " << s.score
                    << ")\n" << utf8Prefix(s.text, 400) << "\n";
            }
            appendSection(system, "相关代码", oss.str(),
                          budget_.l3_search * 3);
        }
        return system;
    }

    static void appendSection(std::string& target, const std::string& title,
                              const std::string& body, int max_chars) {
        if (body.empty()) return;
        std::string trimmed = body.size() > (size_t)max_chars
                                  ? utf8Prefix(body, max_chars) + "\n...[已截断]"
                                  : body;
        target += "\n\n## " + title + "\n" + trimmed;
    }

    int estimateRetrieved() const {
        int total = 0;
        for (const auto& s : retrieved_) total += estimateText(s.text);
        return total;
    }

    std::vector<Message> trimHistory(const std::vector<Message>& history) const {
        std::vector<Message> cleaned = sanitizeToolPairs(history);
        if (max_history_messages_ <= 0) return cleaned;

        std::vector<size_t> indices;
        for (size_t i = 0; i < cleaned.size(); ++i) {
            if (cleaned[i].role != Role::System) indices.push_back(i);
        }
        if ((int)indices.size() <= max_history_messages_) return cleaned;

        size_t keep_from = indices[indices.size() - (size_t)max_history_messages_];
        while (keep_from < cleaned.size() && cleaned[keep_from].role != Role::User) {
            ++keep_from;
        }
        if (keep_from >= cleaned.size()) return cleaned;
        return std::vector<Message>(cleaned.begin() + (std::ptrdiff_t)keep_from,
                                    cleaned.end());
    }

    std::string extractiveSummary(const std::vector<Message>& messages) const {
        std::ostringstream oss;
        int user_count = 0;
        for (const auto& msg : messages) {
            if (msg.role == Role::User) {
                ++user_count;
                oss << "- 需求: " << utf8Prefix(msg.content, 200) << "\n";
            } else if (msg.call_id.has_value()) {
                oss << "- 工具结果: " << utf8Prefix(msg.content, 120) << "\n";
            }
            if (user_count > 8) break;
        }
        if (oss.str().empty()) return "（较早历史已压缩）";
        return oss.str();
    }

    std::string summarizeWithLlm(IModelProvider& provider,
                                 const std::vector<Message>& messages,
                                 Usage& summary_usage) {
        std::string text;
        for (const auto& msg : messages) {
            if (msg.role == Role::System) continue;
            text += (msg.role == Role::User ? "用户: " : "助手: ");
            text += utf8Prefix(msg.content, 1500);
            text += "\n";
        }
        if (text.empty()) return "";

        ChatRequest request;
        request.messages.emplace_back(Role::System,
            "请把以下编码任务对话压缩成简洁摘要，保留：原始需求、改过哪些文件、"
            "用过哪些工具、当前结论。不要编造。");
        request.messages.emplace_back(Role::User, text);
        request.max_tokens = 800;
        request.temperature = 0.2;

        try {
            ChatResponse response = provider.chat(request);
            if (!response.is_error && !response.message.content.empty()) {
                summary_usage += response.usage;
                return response.message.content;
            }
        } catch (...) {
        }
        return "";
    }
};
