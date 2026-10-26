#pragma once

#include "types.hpp"
#include "sandbox.hpp"
#include "win_utf8.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>

// ============================================================================
// 工具注册表 + 工具执行管线 (L4/L6)
//
// 五步管线（对齐课程 L4）：
//   0. 查找工具       6. 未知工具返回可用列表
//   1. Schema 校验    7. 参数类型/必填项
//   2. 权限判定       IPermissionGate: Allow / Ask / Deny
//   3. 执行           ToolFunction
//   4. 文本卫生       sanitizeUtf8
//   5. 大结果落盘     超过阈值写 .af-dump/，上下文只留预览 + 路径
// ============================================================================

// 工具执行函数签名
using ToolFunction = std::function<ToolResult(const json& args)>;

enum class Decision {
    Allow,    // 允许执行
    Ask,      // 需要请求用户授权
    Deny      // 拒绝执行
};

struct PermissionRule {
    std::string tool_pattern;        // 工具名称模式 (支持 * 通配)
    std::string arg_prefix;          // 可选: JSON 参数前缀匹配 (用于 Bash 命令)
    Decision decision = Decision::Ask;
    std::string reason;

    PermissionRule() = default;
    PermissionRule(std::string pattern, Decision dec, std::string r = "",
                   std::string arg_pref = "")
        : tool_pattern(std::move(pattern))
        , arg_prefix(std::move(arg_pref))
        , decision(dec)
        , reason(std::move(r))
    {}
};

class IPermissionGate {
public:
    virtual ~IPermissionGate() = default;

    // 评估一次工具调用的权限。deny 规则永远优先于 allow。
    virtual Decision evaluate(const ToolDef& tool, const json& args) = 0;

    // 获取拒绝原因
    virtual std::string getDenyReason() const = 0;

protected:
    IPermissionGate() = default;
    IPermissionGate(const IPermissionGate&) = delete;
    IPermissionGate& operator=(const IPermissionGate&) = delete;
};

class IUIPort {
public:
    virtual ~IUIPort() = default;

    virtual void output(const std::string& message) = 0;
    virtual void error(const std::string& message) = 0;
    virtual void progress(const std::string& message, int percent = -1) = 0;

    // 请求用户确认（权限门 Ask 时调用）
    virtual bool confirm(const std::string& prompt) = 0;
    virtual std::string input(const std::string& prompt) = 0;

    virtual void showToolCall(const ToolCall& call) = 0;
    virtual void showToolResult(const ToolResult& result) = 0;

    // 结构化权限确认（Web 端可弹窗）；默认退化为文本确认
    virtual bool confirmTool(const std::string& tool_name, const json& args) {
        return confirm("允许执行工具 " + tool_name + "？\n参数: " +
                       utf8Prefix(args.dump(), 500));
    }

    // 流式文本增量 (支持流式输出的实现可重写; 默认忽略)
    virtual void streamDelta(const std::string& delta) { (void)delta; }
};

// 工具执行上下文：把权限门、UI、落盘策略显式传给 registry
struct ToolInvocationContext {
    IPermissionGate* gate = nullptr;
    IUIPort* ui = nullptr;
    std::string dump_dir = ".af-dump";
    std::size_t persist_threshold = 20000;  // 超过字符数落盘
    bool persist_enabled = true;
    bool read_only = false;
    std::function<void(const ToolCall&)> before_execute;  // 检查点等
};

class ToolRegistry {
public:
    ToolRegistry() = default;

    void registerTool(const ToolDef& def, ToolFunction func) {
        tools_[def.name] = def;
        functions_[def.name] = std::move(func);
    }

    // 写/改/命令的工作区边界（空 = 不启用）
    void setWorkspaceRoot(const std::string& root) { workspace_root_ = root; }
    const std::string& workspaceRoot() const { return workspace_root_; }

    // 执行工具（带五步管线）
    ToolResult execute(const ToolCall& call,
                       const ToolInvocationContext* ctx = nullptr);

    std::vector<ToolDef> getAllTools() const {
        std::vector<ToolDef> result;
        for (const auto& [name, def] : tools_) result.push_back(def);
        return result;
    }

    std::optional<ToolDef> getTool(const std::string& name) const {
        auto it = tools_.find(name);
        if (it != tools_.end()) return it->second;
        return std::nullopt;
    }

    std::vector<std::string> listTools() const {
        std::vector<std::string> result;
        for (const auto& [name, def] : tools_) result.push_back(name);
        return result;
    }

    // 只读视图：只保留 RiskLevel::Read 的工具，并按名字排除 Task (防递归)
    std::shared_ptr<ToolRegistry> readOnlyView(const std::string& exclude = "Task") const {
        auto view = std::make_shared<ToolRegistry>();
        for (const auto& [name, def] : tools_) {
            if (def.risk_level != RiskLevel::Read) continue;
            if (!exclude.empty() && name == exclude) continue;
            auto fit = functions_.find(name);
            if (fit == functions_.end()) continue;
            view->tools_[name] = def;
            view->functions_[name] = fit->second;
        }
        return view;
    }

    // 按名字移除（用于 Task 子代理视图）
    void removeTool(const std::string& name) {
        tools_.erase(name);
        functions_.erase(name);
    }

    // 基础 JSON Schema 校验（required + type），错误信息面向模型
    static std::string validateArgs(const ToolDef& def, const json& args);

private:
    std::map<std::string, ToolDef> tools_;
    std::map<std::string, ToolFunction> functions_;
    std::string workspace_root_;

    static bool isPathInside(const std::filesystem::path& root, const std::filesystem::path& p);
    std::string enforceWorkspace(ToolCall& call);

    static bool matchToolPattern(const std::string& name, const std::string& pattern);
    static std::string jsonTypeName(const json& value);
    static std::string persistLargeResult(const std::string& content,
                                          const std::string& tool_name,
                                          const ToolInvocationContext& ctx);
};

// ============================================================================
// 实现 (header-only)
// ============================================================================

inline bool ToolRegistry::matchToolPattern(const std::string& name,
                                           const std::string& pattern) {
    if (pattern.empty() || pattern == "*") return true;
    auto star = pattern.find('*');
    if (star == std::string::npos) return name == pattern;
    std::string prefix = pattern.substr(0, star);
    std::string suffix = pattern.substr(star + 1);
    return name.size() >= prefix.size() + suffix.size() &&
           name.compare(0, prefix.size(), prefix) == 0 &&
           (suffix.empty() ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0);
}

inline std::string ToolRegistry::jsonTypeName(const json& value) {
    if (value.is_null()) return "null";
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer()) return "integer";
    if (value.is_number()) return "number";
    if (value.is_string()) return "string";
    if (value.is_array()) return "array";
    if (value.is_object()) return "object";
    return "unknown";
}

inline std::string ToolRegistry::validateArgs(const ToolDef& def, const json& args) {
    if (!args.is_object()) {
        return "参数必须是 JSON 对象，当前是 " + jsonTypeName(args);
    }
    if (!def.parameters.is_object()) return {};

    const json* properties = nullptr;
    if (def.parameters.contains("properties") &&
        def.parameters["properties"].is_object()) {
        properties = &def.parameters["properties"];
    }

    if (def.parameters.contains("required") &&
        def.parameters["required"].is_array()) {
        for (const auto& required : def.parameters["required"]) {
            if (!required.is_string()) continue;
            std::string key = required.get<std::string>();
            if (!args.contains(key)) {
                return "缺少必需字段 `" + key + "`。请修正参数后重试。";
            }
        }
    }

    if (!properties) return {};

    for (auto it = properties->begin(); it != properties->end(); ++it) {
        const std::string& key = it.key();
        if (!args.contains(key)) continue;

        const json& schema = it.value();
        if (!schema.is_object() || !schema.contains("type")) continue;

        std::string expected = schema["type"].is_string()
                                   ? schema["type"].get<std::string>()
                                   : "";
        const json& value = args[key];

        bool ok = true;
        if (expected == "string") ok = value.is_string();
        else if (expected == "integer")
            ok = value.is_number_integer() || value.is_number_unsigned();
        else if (expected == "number")
            ok = value.is_number();
        else if (expected == "boolean") ok = value.is_boolean();
        else if (expected == "array") ok = value.is_array();
        else if (expected == "object") ok = value.is_object();

        if (!ok) {
            return "字段 `" + key + "` 类型错误：期望 " + expected +
                   "，实际是 " + jsonTypeName(value) + "。";
        }
    }

    return {};
}

inline std::string ToolRegistry::persistLargeResult(
    const std::string& content, const std::string& tool_name,
    const ToolInvocationContext& ctx) {
    namespace fs = std::filesystem;

    std::string preview = utf8Prefix(content, 2000);

    if (!ctx.persist_enabled || content.size() <= ctx.persist_threshold) {
        return content;
    }

    // Read 豁免落盘：避免 "读文件 → 落盘 → 再读落盘文件" 的循环依赖
    if (tool_name == "Read") {
        return utf8Prefix(content, ctx.persist_threshold) + "\n...(内容已截断)";
    }

    try {
        static std::atomic<unsigned long long> counter{0};
        auto now = std::chrono::system_clock::now().time_since_epoch();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        auto seq = counter.fetch_add(1);

        fs::path dir = utf8ToPath(ctx.dump_dir);
        fs::create_directories(dir);
        std::string filename = std::to_string(ms) + "_" + std::to_string(seq) + "_" +
                               tool_name + ".txt";
        fs::path file = dir / utf8ToPath(filename);

        std::ofstream out(file, std::ios::binary);
        if (!out) throw std::runtime_error("open failed");
        out.write(content.data(), (std::streamsize)content.size());
        out.close();

        std::ostringstream oss;
        oss << preview;
        oss << "\n\n...[结果过长，完整内容已保存到: " << ctx.dump_dir << "/" << filename
            << " (" << content.size() << " 字节)；如需查看请用 Read 工具读取该路径]";
        return oss.str();
    } catch (...) {
        return preview + "\n\n...[结果过长且落盘失败，以上为截断预览]";
    }
}

inline bool ToolRegistry::isPathInside(const std::filesystem::path& root, const std::filesystem::path& p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto canon = [&](const fs::path& x) {
        fs::path y = fs::weakly_canonical(x, ec);
        if (ec) {
            ec.clear();
            y = fs::absolute(x, ec).lexically_normal();
        }
        std::string s = y.generic_string();
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        return s;
    };
    const std::string rs = canon(root);
    const std::string ps = canon(p);
    if (ps.size() < rs.size()) return false;
    if (ps.compare(0, rs.size(), rs) != 0) return false;
    return ps.size() == rs.size() || ps[rs.size()] == '/';
}

inline std::string ToolRegistry::enforceWorkspace(ToolCall& call) {
    namespace fs = std::filesystem;
    if (workspace_root_.empty()) return {};
    const fs::path root = utf8ToPath(workspace_root_);
    std::error_code ec;
    fs::create_directories(root, ec);

    if (call.name == "Write" || call.name == "Edit" || call.name == "Read") {
        if (!call.arguments.is_object() || !call.arguments.contains("path") ||
            !call.arguments["path"].is_string()) {
            return {};
        }
        std::string raw = resolveUserPath(call.arguments["path"].get<std::string>());
        fs::path p = utf8ToPath(raw);
        const bool relative = !p.is_absolute();
        if (relative) p = root / p;

        if (call.name == "Read" && relative) {
            // 读：优先读工作区里的同名文件；没有则回退项目目录
            if (!fs::exists(p, ec)) return {};
        }

        fs::path abs = fs::weakly_canonical(p, ec);
        if (ec) {
            ec.clear();
            abs = fs::absolute(p, ec).lexically_normal();
        }
        if (!isPathInside(root, abs)) {
            return "路径不在工作区: " + pathToUtf8(abs) +
                   "（工作区: " + workspace_root_ + "）";
        }
        call.arguments["path"] = pathToUtf8(abs);
        return {};
    }

    if (call.name == "Bash" && call.arguments.is_object() &&
        call.arguments.contains("command") &&
        call.arguments["command"].is_string()) {
        const std::string command = call.arguments["command"].get<std::string>();
        if (command.find("..") != std::string::npos) {
            return "Bash 命令包含上级目录 '..'，已拒绝（工作区: " +
                   workspace_root_ + "）";
        }
        static const std::regex abs_path(R"(([A-Za-z]:[\\/][^\s"']+))");
        for (std::sregex_iterator it(command.begin(), command.end(), abs_path), end;
             it != end; ++it) {
            fs::path p = utf8ToPath((*it)[1].str());
            if (!isPathInside(root, p)) {
                return "Bash 命令包含工作区外路径: " + (*it)[1].str() +
                       "（工作区: " + workspace_root_ + "）";
            }
        }
    }
    return {};
}

inline ToolResult ToolRegistry::execute(const ToolCall& call,
                                        const ToolInvocationContext* ctx) {
    // 0. 查找
    auto fit = functions_.find(call.name);
    if (fit == functions_.end()) {
        std::string names;
        for (const auto& [name, def] : tools_) {
            if (!names.empty()) names += ", ";
            names += name;
        }
        return ToolResult::error(call.id, "未知工具 `" + call.name +
                                              "`。可用工具: " + names);
    }

    auto dit = tools_.find(call.name);
    ToolDef def = (dit != tools_.end()) ? dit->second : ToolDef{call.name, "", {}, RiskLevel::Read};

    // 工作区边界：Write/Edit 的相对路径落到 workspace，绝对路径越界直接拒绝
    ToolCall effective = call;
    std::string workspace_error = enforceWorkspace(effective);
    if (!workspace_error.empty()) {
        return ToolResult::error(call.id, workspace_error);
    }

    // 1. Schema 校验
    std::string validation = validateArgs(def, effective.arguments);
    if (!validation.empty()) {
        return ToolResult::error(call.id, "参数不合法: " + validation);
    }

    // 2. 权限判定
    if (ctx && ctx->gate) {
        Decision decision = ctx->gate->evaluate(def, effective.arguments);
        if (decision == Decision::Deny) {
            std::string reason = ctx->gate->getDenyReason();
            return ToolResult::error(call.id, "被权限策略拒绝: " +
                                                  (reason.empty() ? "deny rule" : reason));
        }
        if (decision == Decision::Ask) {
            bool allowed = (ctx->ui != nullptr)
                               ? ctx->ui->confirmTool(def.name, effective.arguments)
                               : false;
            if (!allowed) {
                return ToolResult::error(call.id,
                    "用户拒绝了这次操作。请换一种方式，或说明为什么必须要它。");
            }
        }
    }

    // 2.5 执行前钩子（例如文件检查点）
    if (ctx && ctx->before_execute) {
        try {
            ctx->before_execute(effective);
        } catch (...) {
        }
    }

    // 3. 执行
    ToolResult result;
    try {
        result = fit->second(effective.arguments);
    } catch (const json::exception& e) {
        return ToolResult::error(call.id, std::string("参数解析错误: ") + e.what());
    } catch (const std::exception& e) {
        return ToolResult::error(call.id, std::string("工具执行失败: ") + e.what());
    }

    if (result.call_id.empty()) result.call_id = call.id;

    // 4. 文本卫生
    result.content = sanitizeUtf8(result.content);

    // 5. 大结果落盘
    if (ctx) {
        result.content = persistLargeResult(result.content, call.name, *ctx);
    } else if (result.content.size() > 30000) {
        result.content = utf8Prefix(result.content, 30000) + "\n...(内容已截断)";
    }

    return result;
}
