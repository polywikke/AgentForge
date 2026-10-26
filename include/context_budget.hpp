#pragma once

#include "types.hpp"
#include "model_provider.hpp"
#include <vector>
#include <string>

// ============================================================================
// 上下文预算管理 (L7)
// ============================================================================

class ContextBudget {
public:
    explicit ContextBudget(const BudgetConfig& config)
        : config_(config)
        , current_usage_{}
    {}

    // 检查是否超出预算
    bool isOverBudget() const {
        return getTotalUsed() > config_.total_budget;
    }

    // 获取剩余预算
    int getRemainingBudget() const {
        return config_.total_budget - getTotalUsed();
    }

    // 获取某一层的预算
    int getLayerBudget(Layer layer) const {
        return config_.layerBudget(layer);
    }

    // 更新使用量
    void updateUsage(Layer layer, int tokens) {
        current_usage_[static_cast<int>(layer)] = tokens;
    }

    // 获取当前使用量
    int getLayerUsage(Layer layer) const {
        return current_usage_[static_cast<int>(layer)];
    }

    // 获取总使用量
    int getTotalUsed() const {
        int total = 0;
        for (int usage : current_usage_) {
            total += usage;
        }
        return total;
    }

    // 检查是否需要压缩
    bool shouldCompress() const {
        const double THRESHOLD = 0.85;  // 85% 触发压缩
        return getTotalUsed() > config_.total_budget * THRESHOLD;
    }

    // 获取压缩目标
    int getCompactionTarget() const {
        const double TARGET = 0.70;  // 压缩到70%
        return static_cast<int>(config_.total_budget * TARGET);
    }

    BudgetConfig config() const { return config_; }

private:
    BudgetConfig config_;
    std::array<int, 7> current_usage_;  // 7层的使用量
};

// ============================================================================
// 消息压缩器
// ============================================================================

// 压缩结果
struct CompactionResult {
    std::vector<Message> messages;
    CompactionEvent event;
};

class MessageCompactor {
public:
    explicit MessageCompactor(std::shared_ptr<IModelProvider> provider)
        : provider_(std::move(provider))
    {}

    // 压缩消息列表
    CompactionResult compact(const std::vector<Message>& messages,
                            int target_size,
                            const std::string& level = "mild") {

        CompactionResult result;
        result.event.level = level;
        result.event.tokens_before = estimateTokens(messages);

        if (level == "mild") {
            result.messages = compactMild(messages, target_size);
        } else {
            result.messages = compactAggressive(messages, target_size);
        }

        result.event.tokens_after = estimateTokens(result.messages);
        result.event.reason = "预算压力触发压缩";

        return result;
    }

    // 生成摘要
    std::string summarize(const std::vector<Message>& messages) {
        // 构造摘要请求
        ChatRequest request;

        Message system_msg(Role::System,
            "请将以下对话总结为简洁的要点，保留关键信息和决策。");
        request.messages.push_back(system_msg);

        // 拼接要摘要的内容
        std::string content;
        for (const auto& msg : messages) {
            content += roleToString(msg.role) + ": " + msg.content + "\n\n";
        }

        Message user_msg(Role::User, content);
        request.messages.push_back(user_msg);
        request.model = "gpt-3.5-turbo";  // 使用快速模型

        try {
            ChatResponse response = provider_->chat(request);
            return response.message.content;
        } catch (...) {
            // INV-15: 摘要失败必须降级，不得抛异常
            return "[摘要生成失败，保留原始内容]";
        }
    }

private:
    std::shared_ptr<IModelProvider> provider_;

    // 温和压缩：仅压缩历史消息
    std::vector<Message> compactMild(const std::vector<Message>& messages,
                                     int target_size) {
        if (messages.empty()) return messages;

        std::vector<Message> result;

        // 保留前几条消息 (任务目标)
        size_t keep_head = std::min<size_t>(2, messages.size());
        for (size_t i = 0; i < keep_head; ++i) {
            result.push_back(messages[i]);
        }

        // 保留最近的消息
        int current_size = estimateTokens(result);
        size_t start_idx = messages.size();

        for (size_t i = messages.size(); i-- > keep_head; ) {
            int msg_size = estimateTokens({messages[i]});
            if (current_size + msg_size <= target_size) {
                start_idx = i;
                current_size += msg_size;
            } else {
                break;
            }
        }

        // 如果中间有gap，插入摘要
        if (start_idx > keep_head) {
            std::vector<Message> to_summarize(
                messages.begin() + keep_head,
                messages.begin() + start_idx
            );

            std::string summary = summarize(to_summarize);
            Message summary_msg(Role::System,
                "[历史摘要] " + summary);
            result.push_back(summary_msg);
        }

        // 添加保留的尾部消息
        for (size_t i = start_idx; i < messages.size(); ++i) {
            result.push_back(messages[i]);
        }

        return result;
    }

    // 激进压缩：压缩所有可压缩内容
    std::vector<Message> compactAggressive(const std::vector<Message>& messages,
                                          int target_size) {
        std::vector<Message> result;

        // 保留第一条消息 (任务目标)
        if (!messages.empty()) {
            result.push_back(messages[0]);
        }

        // 其余全部摘要
        if (messages.size() > 1) {
            std::vector<Message> to_summarize(
                messages.begin() + 1,
                messages.end()
            );

            std::string summary = summarize(to_summarize);
            Message summary_msg(Role::System,
                "[激进压缩摘要] " + summary);
            result.push_back(summary_msg);
        }

        return result;
    }

    int estimateTokens(const std::vector<Message>& messages) const {
        int total = 0;
        for (const auto& msg : messages) {
            // 简单估算: 1 token ≈ 3 字符
            total += static_cast<int>(msg.content.size() / 3);
        }
        return total;
    }

    std::string roleToString(Role role) const {
        switch (role) {
            case Role::System: return "System";
            case Role::User: return "User";
            case Role::Assistant: return "Assistant";
        }
        return "Unknown";
    }
};
