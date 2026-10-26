#pragma once

#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <cstdint>
#include "nlohmann/json.hpp"

// ============================================================================
// AgentForge 类型契约层 (types.hpp)
// 定义全工程共享的数据结构、枚举和文本卫生函数
// 本文件不依赖任何项目内文件 (INV-12)
// ============================================================================

using json = nlohmann::json;

// ============================================================================
// 基础枚举类型 (INV-4: 使用 enum class 避免隐式转换)
// ============================================================================

enum class Role {
    System,
    User,
    Assistant
};

enum class FinishReason {
    Stop,          // 正常结束
    ToolCalls,     // 需要执行工具
    Length,        // 达到最大长度
    ContentFilter, // 内容过滤
    Error          // 错误
};

// INV-5: RiskLevel 单调递增
enum class RiskLevel {
    Read = 0,     // 只读操作
    Write = 1,    // 写入操作
    Exec = 2,     // 命令执行
    Network = 3   // 网络访问
};

enum class ToolChoice {
    Auto,         // 模型自主决定
    Required,     // 必须调用工具
    None          // 不调用工具
};

// 上下文层 (用于预算管理)
enum class Layer {
    L0_System = 0,      // system 提示
    L1_Project = 1,     // 项目知识 (CLAUDE.md)
    L2_RepoMap = 2,     // 仓库地图
    L3_Search = 3,      // 检索结果
    L4_ActiveFiles = 4, // 活动文件
    L5_History = 5,     // 对话历史
    L6_UserInput = 6    // 当前用户输入
};

enum class LoopOutcome {
    Success,           // 成功完成
    MaxSteps,          // 达到最大步数
    BudgetExceeded,    // 预算超出
    UserInterrupt,     // 用户中断
    Error,             // 错误
    Timeout            // 超时
};

// 降级策略 (用于内容超出预算时)
enum class Degrade {
    FullText,          // 完整文本
    SignaturesOnly,    // 仅函数签名
    SymbolsAndPaths,   // 符号名 + 路径
    PathsOnly,         // 仅路径
    Discard            // 丢弃
};

// ============================================================================
// 文本卫生函数 (INV-7: sanitizeUtf8 输出必为合法 UTF-8)
// ============================================================================

// 检查是否为合法 UTF-8
inline bool isValidUtf8(const std::string& s) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(s.data());
    size_t len = s.size();
    size_t i = 0;

    while (i < len) {
        unsigned char c = bytes[i];

        if (c <= 0x7F) {
            // ASCII 单字节
            i++;
        } else if ((c >> 5) == 0x06) {
            // 2 字节序列
            if (i + 1 >= len || (bytes[i + 1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if ((c >> 4) == 0x0E) {
            // 3 字节序列
            if (i + 2 >= len || (bytes[i + 1] & 0xC0) != 0x80 || (bytes[i + 2] & 0xC0) != 0x80)
                return false;
            i += 3;
        } else if ((c >> 3) == 0x1E) {
            // 4 字节序列
            if (i + 3 >= len || (bytes[i + 1] & 0xC0) != 0x80 ||
                (bytes[i + 2] & 0xC0) != 0x80 || (bytes[i + 3] & 0xC0) != 0x80)
                return false;
            i += 4;
        } else {
            return false;
        }
    }

    return true;
}

// 清理非法 UTF-8, 用 U+FFFD 替换非法字节
inline std::string sanitizeUtf8(const std::string& input) {
    if (isValidUtf8(input)) return input; // 快路径

    std::string output;
    output.reserve(input.size());
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(input.data());
    size_t len = input.size();
    size_t i = 0;

    while (i < len) {
        unsigned char c = bytes[i];

        if (c <= 0x7F) {
            output += c;
            i++;
        } else if ((c >> 5) == 0x06 && i + 1 < len && (bytes[i + 1] & 0xC0) == 0x80) {
            output += c;
            output += bytes[i + 1];
            i += 2;
        } else if ((c >> 4) == 0x0E && i + 2 < len &&
                   (bytes[i + 1] & 0xC0) == 0x80 && (bytes[i + 2] & 0xC0) == 0x80) {
            output += c;
            output += bytes[i + 1];
            output += bytes[i + 2];
            i += 3;
        } else if ((c >> 3) == 0x1E && i + 3 < len &&
                   (bytes[i + 1] & 0xC0) == 0x80 && (bytes[i + 2] & 0xC0) == 0x80 &&
                   (bytes[i + 3] & 0xC0) == 0x80) {
            output += c;
            output += bytes[i + 1];
            output += bytes[i + 2];
            output += bytes[i + 3];
            i += 4;
        } else {
            // 非法字节，替换为 U+FFFD (�)
            output += "\xEF\xBF\xBD";
            i++;
        }
    }

    return output;
}

// K-21: 按字节截断的边界安全函数
inline size_t utf8Snap(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    // 回退到最近的字符边界
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
        i--;
    }
    return i;
}

inline std::string utf8Prefix(const std::string& s, size_t n) {
    size_t safe_pos = utf8Snap(s, n);
    return s.substr(0, safe_pos);
}

inline std::string utf8Resize(std::string s, size_t n) {
    if (s.size() <= n) return s;
    return utf8Prefix(s, n);
}

// ============================================================================
// std::variant 辅助模板 (用于 std::visit)
// ============================================================================

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// ============================================================================
// 消息与工具相关类型
// ============================================================================

// 工具调用
struct ToolCall {
    std::string id;         // 调用 ID
    std::string name;       // 工具名称
    json arguments;         // 参数 (JSON)

    ToolCall() = default;
    ToolCall(std::string id_, std::string name_, json args_)
        : id(std::move(id_)), name(std::move(name_)), arguments(std::move(args_)) {}
};

// 消息 (INV-3: 非多态, 值语义)
struct Message {
    Role role;
    std::string content;
    std::vector<ToolCall> tool_calls;     // assistant 消息可能包含工具调用
    std::optional<std::string> call_id;   // tool 消息需要关联的 call_id

    Message() = default;
    Message(Role r, std::string c) : role(r), content(std::move(c)) {}

    // 创建工具结果消息
    static Message toolResult(const std::string& call_id, const std::string& result) {
        Message msg;
        msg.role = Role::Assistant;  // 或使用特殊的 Tool 角色
        msg.content = result;
        msg.call_id = call_id;
        return msg;
    }
};

// 使用量统计 (INV-10: total() 必须包含 cache 读写)
struct Usage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int cache_read_tokens = 0;    // 缓存命中的 token
    int cache_write_tokens = 0;   // 写入缓存的 token

    int total() const {
        return prompt_tokens + completion_tokens + cache_read_tokens + cache_write_tokens;
    }

    Usage& operator+=(const Usage& other) {
        prompt_tokens += other.prompt_tokens;
        completion_tokens += other.completion_tokens;
        cache_read_tokens += other.cache_read_tokens;
        cache_write_tokens += other.cache_write_tokens;
        return *this;
    }
};

// ============================================================================
// 工具定义与结果
// ============================================================================

// 工具定义
struct ToolDef {
    std::string name;
    std::string description;
    json parameters;        // JSON Schema 格式
    RiskLevel risk_level;

    ToolDef() : risk_level(RiskLevel::Read) {}
    ToolDef(std::string n, std::string desc, json params, RiskLevel risk)
        : name(std::move(n))
        , description(std::move(desc))
        , parameters(std::move(params))
        , risk_level(risk)
    {}
};

// 工具执行结果 (INV-9: callId 必须匹配某个 ToolCall.id)
struct ToolResult {
    std::string call_id;
    std::string content;
    bool is_error = false;
    bool blocked_by_sandbox = false;
    std::string block_reason;

    ToolResult() = default;
    ToolResult(std::string cid, std::string cnt)
        : call_id(std::move(cid)), content(std::move(cnt)) {}

    static ToolResult error(const std::string& call_id, const std::string& error_msg) {
        ToolResult r;
        r.call_id = call_id;
        r.content = error_msg;
        r.is_error = true;
        return r;
    }

    static ToolResult blocked(const std::string& call_id, const std::string& reason) {
        ToolResult r;
        r.call_id = call_id;
        r.content = reason;
        r.is_error = true;
        r.blocked_by_sandbox = true;
        r.block_reason = reason;
        return r;
    }
};

// ============================================================================
// 流式事件 (INV-1: ChatChunk 恰好 8 种事件)
// ============================================================================

struct ChunkStart {
    std::string request_id;
};

struct ChunkDelta {
    std::string content;
};

struct ChunkToolCallStart {
    std::string id;
    std::string name;
};

struct ChunkToolCallDelta {
    std::string id;
    std::string arguments_delta;
};

struct ChunkFinish {
    FinishReason reason;
    Usage usage;
};

struct ChunkError {
    std::string message;
    int code = 0;
};

struct ChunkPing {
    // SSE keepalive
};

struct ChunkUnknown {
    std::string raw;
};

// ChatChunk = 上述 8 种事件之一
using ChatChunk = std::variant<
    ChunkStart,
    ChunkDelta,
    ChunkToolCallStart,
    ChunkToolCallDelta,
    ChunkFinish,
    ChunkError,
    ChunkPing,
    ChunkUnknown
>;

// ============================================================================
// 请求与响应
// ============================================================================

struct ChatRequest {
    std::vector<Message> messages;
    std::vector<ToolDef> tools;          // INV-8: 顺序字节级稳定
    ToolChoice tool_choice = ToolChoice::Auto;
    double temperature = 0.7;
    int max_tokens = 4096;
    std::string model;

    ChatRequest() = default;
};

struct ChatResponse {
    std::string id;
    Message message;
    Usage usage;
    FinishReason finish_reason = FinishReason::Stop;
    bool is_error = false;
    std::string error_message;

    ChatResponse() = default;

    static ChatResponse error(const std::string& msg) {
        ChatResponse r;
        r.is_error = true;
        r.error_message = msg;
        return r;
    }
};

// ============================================================================
// 循环控制
// ============================================================================

struct LoopLimits {
    int max_steps = 30;              // 最大步数
    int max_tokens = 150000;         // 最大 token 预算
    int max_duration_sec = 600;      // 最大执行时间 (秒)
    int max_repeat_calls = 3;        // 最大重复调用次数
    int max_consecutive_errors = 3;  // 连续错误上限（L6 闸门 4）
    std::string model;               // 请求模型 (为空则由 provider 配置决定)
    std::string dump_dir = ".af-dump";      // 大结果落盘目录
    std::size_t persist_threshold = 20000;  // 超过该字符数落盘
    bool enable_compaction = true;          // 是否启用自动上下文压缩

    LoopLimits() = default;
};

// 检索片段（L8 代码地图 / L3 检索层）
struct RetrievedSnippet {
    std::string path;
    int line = 0;
    double score = 0.0;
    std::string text;
};

// Turn 表示一轮完整的任务周期 (INV-12: 必须在契约层定义)
struct Turn {
    int turn_id = 0;
    std::vector<Message> messages;
    Usage accumulated_usage;
    LoopOutcome outcome = LoopOutcome::Success;
    std::string outcome_reason;

    Turn() = default;
    explicit Turn(int id) : turn_id(id) {}
};

// ============================================================================
// 预算配置
// ============================================================================

struct BudgetConfig {
    int total_budget = 128000;       // 总预算

    // 各层预算分配 (INV-6: 各层预算之和 + 历史 = 可用量)
    int l0_system = 10000;
    int l1_project = 8000;
    int l2_repo_map = 15000;
    int l3_search = 20000;
    int l4_active_files = 25000;
    int l5_history = 40000;          // 历史消息
    int l6_user_input = 10000;       // 当前输入

    BudgetConfig() = default;

    int layerBudget(Layer layer) const {
        switch (layer) {
            case Layer::L0_System: return l0_system;
            case Layer::L1_Project: return l1_project;
            case Layer::L2_RepoMap: return l2_repo_map;
            case Layer::L3_Search: return l3_search;
            case Layer::L4_ActiveFiles: return l4_active_files;
            case Layer::L5_History: return l5_history;
            case Layer::L6_UserInput: return l6_user_input;
        }
        return 0;
    }

    int totalAllocated() const {
        return l0_system + l1_project + l2_repo_map + l3_search +
               l4_active_files + l5_history + l6_user_input;
    }
};

// ============================================================================
// 压缩事件 (L7 新增)
// ============================================================================

struct CompactionEvent {
    std::string level;               // "mild" / "aggressive"
    int tokens_before = 0;
    int tokens_after = 0;
    std::string reason;

    CompactionEvent() = default;
};
