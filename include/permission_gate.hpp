#pragma once

#include "tool_registry.hpp"
#include "win_utf8.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// ============================================================================
// PersistentPermissionGate (L6 + 大作业题目十)
//
// - 规则来自：内置默认规则 + `.af-permissions.json`
// - Deny 永远优先于 Allow（排序时 deny 在前）
// - 支持 "总是允许 / 总是拒绝" 持久化
// - Bash 规则支持命令前缀匹配：{"tool":"Bash","arg_prefix":"rm"}
// ============================================================================

class PersistentPermissionGate : public IPermissionGate {
public:
    explicit PersistentPermissionGate(std::string file_path = ".af-permissions.json",
                                      bool default_allow_read = true)
        : file_path_(std::move(file_path)) {
        // 内置默认：读操作允许，写/执行询问
        addDefaults();
        (void)default_allow_read;

        load();
        sortDenyFirst();
    }

    Decision evaluate(const ToolDef& tool, const json& args) override {
        std::lock_guard<std::mutex> lock(mutex_);
        deny_reason_.clear();

        // 1) deny 优先
        for (const auto& rule : rules_) {
            if (rule.decision != Decision::Deny) continue;
            if (matches(rule, tool.name, args)) {
                deny_reason_ = rule.reason.empty() ? "deny rule" : rule.reason;
                return Decision::Deny;
            }
        }

        // 2) 全局模式（allow_all / deny_all）优先于普通规则
        if (default_decision_ != Decision::Ask) return default_decision_;

        // 3) 其余规则
        for (const auto& rule : rules_) {
            if (rule.decision == Decision::Deny) continue;
            if (matches(rule, tool.name, args)) return rule.decision;
        }

        return Decision::Ask;
    }

    std::string getDenyReason() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return deny_reason_;
    }

    // 记住用户选择：tool + arg_prefix 唯一确定一条规则
    void remember(const std::string& tool, const std::string& arg_prefix,
                  Decision decision, const std::string& reason = "") {        std::string stored_reason = reason.empty()
                                        ? "[saved] 用户记住的选择"
                                        : "[saved] " + reason;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool replaced = false;
            for (auto& rule : rules_) {
                if (rule.tool_pattern == tool && rule.arg_prefix == arg_prefix) {
                    rule.decision = decision;
                    rule.reason = stored_reason;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                rules_.push_back(PermissionRule{tool, decision, stored_reason, arg_prefix});
            }
            sortDenyFirstLocked();
        }
        save();
    }

    void resetPersistent() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rules_.clear();
            addDefaults();
            sortDenyFirstLocked();
        }
        save();
    }

    std::vector<PermissionRule> rules() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rules_;
    }

    void setDefaultDecision(Decision decision) {
        std::lock_guard<std::mutex> lock(mutex_);
        default_decision_ = decision;
    }

    // 运行时规则（不落盘）：用于 permission_mode=allow_all/deny_all
    void addRuntimeRule(const std::string& tool, const std::string& arg_prefix,
                        Decision decision, const std::string& reason) {
        std::lock_guard<std::mutex> lock(mutex_);
        rules_.push_back(PermissionRule{tool, decision, reason, arg_prefix});
        sortDenyFirstLocked();
    }

    std::string filePath() const { return file_path_; }

private:
    std::string file_path_;
    std::vector<PermissionRule> rules_;
    std::string deny_reason_;
    Decision default_decision_ = Decision::Ask;
    mutable std::mutex mutex_;

    void addDefaults() {
        addRuleInternal({"Read", Decision::Allow, "读取操作默认允许"});
        addRuleInternal({"Glob", Decision::Allow, "文件搜索默认允许"});
        addRuleInternal({"Grep", Decision::Allow, "内容搜索默认允许"});
        addRuleInternal({"Write", Decision::Ask, "写入操作需要确认"});
        addRuleInternal({"Edit", Decision::Ask, "修改操作需要确认"});
        addRuleInternal({"Bash", Decision::Ask, "命令执行需要确认"});
        addRuleInternal({"Task", Decision::Allow, "只读子代理默认允许"});
    }

    void addRuleInternal(PermissionRule rule) {
        rules_.push_back(std::move(rule));
    }

    void sortDenyFirst() {
        std::lock_guard<std::mutex> lock(mutex_);
        sortDenyFirstLocked();
    }

    void sortDenyFirstLocked() {
        std::stable_sort(rules_.begin(), rules_.end(),
                         [](const PermissionRule& a, const PermissionRule& b) {
                             return a.decision == Decision::Deny &&
                                    b.decision != Decision::Deny;
                         });
    }

    static std::string decisionToString(Decision d) {
        switch (d) {
            case Decision::Allow: return "allow";
            case Decision::Ask: return "ask";
            case Decision::Deny: return "deny";
        }
        return "ask";
    }

    static Decision decisionFromString(const std::string& s) {
        if (s == "allow") return Decision::Allow;
        if (s == "deny") return Decision::Deny;
        return Decision::Ask;
    }

    static bool matches(const PermissionRule& rule, const std::string& tool,
                        const json& args) {
        if (rule.tool_pattern.empty() || rule.tool_pattern == "*") {
            // wildcard: 继续检查 arg_prefix
        } else {
            std::string pattern = rule.tool_pattern;
            auto star = pattern.find('*');
            if (star == std::string::npos) {
                if (tool != pattern) return false;
            } else {
                std::string prefix = pattern.substr(0, star);
                std::string suffix = pattern.substr(star + 1);
                if (tool.size() < prefix.size() + suffix.size()) return false;
                if (tool.compare(0, prefix.size(), prefix) != 0) return false;
                if (!suffix.empty() &&
                    tool.compare(tool.size() - suffix.size(), suffix.size(), suffix) != 0) {
                    return false;
                }
            }
        }

        if (rule.arg_prefix.empty()) return true;

        std::string haystack;
        if (args.is_object() && args.contains("command") && args["command"].is_string()) {
            haystack = args["command"].get<std::string>();
        } else {
            haystack = args.dump();
        }

        // 命令前缀/包含匹配（例如 "rm" 匹配 "rm -rf x"）
        if (haystack.rfind(rule.arg_prefix, 0) == 0) return true;
        return haystack.find(rule.arg_prefix) != std::string::npos;
    }

    void load() {
        namespace fs = std::filesystem;
        try {
            fs::path path = utf8ToPath(file_path_);
            if (!fs::exists(path)) return;

            std::ifstream in(path, std::ios::binary);
            if (!in) return;
            json j;
            in >> j;
            if (!j.contains("rules") || !j["rules"].is_array()) return;

            for (const auto& item : j["rules"]) {
                if (!item.is_object()) continue;
                PermissionRule rule;
                rule.tool_pattern = item.value("tool", "*");
                rule.arg_prefix = item.value("arg_prefix", "");
                rule.decision = decisionFromString(item.value("decision", "ask"));
                rule.reason = item.value("reason", "");
                if (rule.reason.empty()) rule.reason = "[saved] 用户记住的选择";

                // 持久化规则覆盖同 tool + arg_prefix 的内置规则
                bool replaced = false;
                for (auto& existing : rules_) {
                    if (existing.tool_pattern == rule.tool_pattern &&
                        existing.arg_prefix == rule.arg_prefix) {
                        existing = rule;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) rules_.push_back(std::move(rule));
            }
        } catch (...) {
            // 文件损坏时忽略，使用内置规则
        }
    }

    void save() const {
        namespace fs = std::filesystem;
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            json j;
            j["version"] = 1;
            j["rules"] = json::array();
            for (const auto& rule : rules_) {
                // 只持久化用户显式保存的规则
                if (rule.reason.find("[saved]") != 0) continue;
                j["rules"].push_back({
                    {"tool", rule.tool_pattern},
                    {"arg_prefix", rule.arg_prefix},
                    {"decision", decisionToString(rule.decision)},
                    {"reason", rule.reason}
                });
            }

            fs::path path = utf8ToPath(file_path_);
            if (path.has_parent_path()) fs::create_directories(path.parent_path());
            fs::path tmp = path;
            tmp += ".tmp";

            std::ofstream out(tmp, std::ios::binary);
            if (!out) return;
            out << j.dump(2);
            out.close();
            std::error_code ec;
            fs::rename(tmp, path, ec);
            if (ec) {
                fs::remove(path, ec);
                fs::rename(tmp, path, ec);
            }
        } catch (...) {
        }
    }
};
