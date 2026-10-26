#include "../include/agent_loop.hpp"
#include "../include/tool_registry.hpp"
#include "../include/model_provider.hpp"
#include "../include/sandbox.hpp"
#include "../include/win_utf8.hpp"
#include <filesystem>
#include <iostream>
#include <memory>

// 外部实现 (ui_impl.cpp / mock_provider.cpp / core_tools.cpp / sandbox_win.cpp)
std::shared_ptr<IUIPort> createConsoleUI();
std::shared_ptr<IModelProvider> createMockProvider(const std::string& model_name);
std::shared_ptr<ISandbox> createWindowsSandbox(SandboxConfig config);
void registerCoreTools(ToolRegistry& registry,
                       std::shared_ptr<ISandbox> sandbox,
                       std::shared_ptr<IUIPort> ui);

// ============================================================================
// AgentForge 主程序
// ============================================================================

void printBanner() {
    std::cout << R"(
╔═══════════════════════════════════════════════════════════╗
║                                                           ║
║                    AgentForge v1.0                        ║
║        面向代码仓库的编码 Agent Harness 系统                 ║
║                                                           ║
╚═══════════════════════════════════════════════════════════╝
)" << std::endl;
}

void printHelp() {
    std::cout << "\n可用命令:\n";
    std::cout << "  /help     - 显示此帮助信息\n";
    std::cout << "  /tools    - 列出所有可用工具\n";
    std::cout << "  /limits   - 显示循环限制配置\n";
    std::cout << "  /quit     - 退出程序\n";
    std::cout << "  <任务>    - 输入任务描述开始执行\n";
    std::cout << std::endl;
}

void printTools(const ToolRegistry& registry) {
    auto tools = registry.getAllTools();
    std::cout << "\n可用工具 (" << tools.size() << " 个):\n";
    for (const auto& tool : tools) {
        std::cout << "  • " << tool.name << " - " << tool.description;
        std::cout << " [风险: ";
        switch (tool.risk_level) {
            case RiskLevel::Read: std::cout << "读取"; break;
            case RiskLevel::Write: std::cout << "写入"; break;
            case RiskLevel::Exec: std::cout << "执行"; break;
            case RiskLevel::Network: std::cout << "网络"; break;
        }
        std::cout << "]\n";
    }
    std::cout << std::endl;
}

void printLimits(const LoopLimits& limits) {
    std::cout << "\n循环限制配置:\n";
    std::cout << "  最大步数: " << limits.max_steps << "\n";
    std::cout << "  最大 token: " << limits.max_tokens << "\n";
    std::cout << "  最大时长: " << limits.max_duration_sec << " 秒\n";
    std::cout << "  最大重复调用: " << limits.max_repeat_calls << "\n";
    std::cout << std::endl;
}

void printStats(const LoopStats& stats) {
    std::cout << "\n═══════════════════════════════════════\n";
    std::cout << "执行统计:\n";
    std::cout << "  总步数: " << stats.total_steps << "\n";
    std::cout << "  工具调用: " << stats.tool_calls << "\n";
    std::cout << "  Token 使用:\n";
    std::cout << "    - 提示: " << stats.total_usage.prompt_tokens << "\n";
    std::cout << "    - 完成: " << stats.total_usage.completion_tokens << "\n";
    std::cout << "    - 缓存读: " << stats.total_usage.cache_read_tokens << "\n";
    std::cout << "    - 总计: " << stats.total_usage.total() << "\n";
    std::cout << "  执行时长: " << stats.duration_ms << " ms\n";
    std::cout << "  结果: ";

    switch (stats.outcome) {
        case LoopOutcome::Success:
            std::cout << "成功完成\n";
            break;
        case LoopOutcome::MaxSteps:
            std::cout << "达到最大步数\n";
            break;
        case LoopOutcome::BudgetExceeded:
            std::cout << "超出预算\n";
            break;
        case LoopOutcome::UserInterrupt:
            std::cout << "用户中断\n";
            break;
        case LoopOutcome::Error:
            std::cout << "错误: " << stats.outcome_reason << "\n";
            break;
    }

    std::cout << "═══════════════════════════════════════\n";
    std::cout << std::endl;
}

int main(int argc, char* argv[]) {
    try {
        printBanner();
        std::cout << "AgentForge 初始化中...\n" << std::endl;

        // 创建 UI
        auto ui = createConsoleUI();

        // 创建沙箱
        SandboxConfig sandbox_cfg;
        sandbox_cfg.work_dir = pathToUtf8(std::filesystem::current_path());
        auto sandbox = createWindowsSandbox(sandbox_cfg);

        // 创建工具注册表
        auto tools = std::make_shared<ToolRegistry>();
        registerCoreTools(*tools, sandbox, ui);
        ui->output("已注册 " + std::to_string(tools->getAllTools().size()) + " 个核心工具");

        // 创建模型提供者 (演示模式使用 Mock)
        auto provider = createMockProvider("mock-gpt-4");
        ui->output("使用模拟模型提供者: " + provider->name());

        // 创建 Agent 循环
        AgentLoop loop(provider, tools, ui);

        // 循环限制
        LoopLimits limits;
        limits.max_steps = 10;
        limits.max_tokens = 50000;
        limits.max_duration_sec = 300;

        ui->output("系统就绪! 输入 /help 查看帮助信息\n");

        // 主循环
        bool running = true;
        while (running) {
            std::cout << "\n> ";
            std::string input;
            std::getline(std::cin, input);

            if (input.empty()) continue;

            // 处理命令
            if (input[0] == '/') {
                if (input == "/help") {
                    printHelp();
                }
                else if (input == "/tools") {
                    printTools(*tools);
                }
                else if (input == "/limits") {
                    printLimits(limits);
                }
                else if (input == "/quit" || input == "/exit") {
                    ui->output("再见!");
                    running = false;
                }
                else {
                    ui->error("未知命令: " + input);
                    printHelp();
                }
            }
            else {
                // 执行任务
                std::cout << "\n执行任务: " << input << "\n";
                std::cout << "─────────────────────────────────────\n";

                try {
                    LoopStats stats = loop.run(input, limits);
                    printStats(stats);
                }
                catch (const std::exception& e) {
                    ui->error(std::string("执行失败: ") + e.what());
                }
            }
        }

        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "致命错误: " << e.what() << std::endl;
        return 1;
    }
}
