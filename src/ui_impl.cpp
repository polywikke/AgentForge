#include "../include/tool_registry.hpp"
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================================
// 控制台 UI 实现
// ============================================================================

class ConsoleUI : public IUIPort {
public:
    ConsoleUI() {
        // Windows 下设置控制台为 UTF-8 编码
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        // 启用 ANSI 转义序列支持
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD dwMode = 0;
        GetConsoleMode(hOut, &dwMode);
        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, dwMode);
#endif
    }
    void output(const std::string& message) override {
        std::cout << "[输出] " << message << std::endl;
    }

    void error(const std::string& message) override {
        std::cerr << "[错误] " << message << std::endl;
    }

    void progress(const std::string& message, int percent) override {
        std::cout << "[进度] " << message;
        if (percent >= 0) {
            std::cout << " (" << percent << "%)";
        }
        std::cout << std::endl;
    }

    bool confirm(const std::string& prompt) override {
        std::cout << "[确认] " << prompt << " (y/n): ";
        std::string input;
        std::getline(std::cin, input);
        return input == "y" || input == "Y" || input == "yes";
    }

    std::string input(const std::string& prompt) override {
        std::cout << "[输入] " << prompt << ": ";
        std::string result;
        std::getline(std::cin, result);
        return result;
    }

    void showToolCall(const ToolCall& call) override {
        std::cout << "\n[工具调用] " << call.name << std::endl;
        std::cout << "  参数: " << call.arguments.dump(2) << std::endl;
    }

    void showToolResult(const ToolResult& result) override {
        if (result.is_error) {
            std::cout << "[工具错误] " << result.content << std::endl;
        } else {
            std::cout << "[工具结果] " << result.content << std::endl;
        }
    }

    void streamDelta(const std::string& delta) override {
        // 流式输出: 原样打印, 不追加前缀/换行
        std::cout << delta << std::flush;
    }
};

// ============================================================================
// 简单权限门实现
// ============================================================================

class SimplePermissionGate : public IPermissionGate {
public:
    SimplePermissionGate() {
        // 默认规则: 读取允许, 其他需要确认
        rules_.push_back({"Read", Decision::Allow, "读取操作默认允许"});
        rules_.push_back({"Write", Decision::Ask, "写入操作需要确认"});
        rules_.push_back({"Bash", Decision::Ask, "命令执行需要确认"});
    }

    Decision evaluate(const ToolDef& tool, const json& args) override {
        // deny 规则优先 (ADR-4)
        for (const auto& rule : rules_) {
            if (rule.decision == Decision::Deny && matchPattern(tool.name, rule.tool_pattern)) {
                deny_reason_ = rule.reason;
                return Decision::Deny;
            }
        }

        // 其他规则
        for (const auto& rule : rules_) {
            if (matchPattern(tool.name, rule.tool_pattern)) {
                return rule.decision;
            }
        }

        return Decision::Ask; // 默认需要确认
    }

    std::string getDenyReason() const override {
        return deny_reason_;
    }

    void addRule(const PermissionRule& rule) {
        rules_.push_back(rule);
    }

private:
    std::vector<PermissionRule> rules_;
    std::string deny_reason_;

    bool matchPattern(const std::string& name, const std::string& pattern) const {
        // 简单匹配: 完全相等或通配符
        if (pattern == "*") return true;
        return name == pattern;
    }
};

// ============================================================================
// 工厂函数
// ============================================================================

std::shared_ptr<IUIPort> createConsoleUI() {
    return std::make_shared<ConsoleUI>();
}

// 注意: 真实的 Windows 沙箱实现位于 src/sandbox_win.cpp
// (createWindowsSandbox), 这里不再提供空实现, 避免符号重复与行为不一致。

std::shared_ptr<IPermissionGate> createSimplePermissionGate() {
    return std::make_shared<SimplePermissionGate>();
}
