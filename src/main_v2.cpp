// ============================================================================
// AgentForge v2 console —— L1-L12 功能演示入口
//
// 交互: /help /tools /cost /context /index /map /sessions /save /load /memory /quit
// 仪表盘: --cost <dir> / --context / --regress <file> / --selftest
// ============================================================================
#include "../include/types.hpp"
#include "../include/agent_loop.hpp"
#include "../include/tool_registry.hpp"
#include "../include/model_provider.hpp"
#include "../include/sandbox.hpp"
#include "../include/context_budget.hpp"
#include "../include/context_manager.hpp"
#include "../include/repo_index.hpp"
#include "../include/session_store.hpp"
#include "../include/subagent.hpp"
#include "../include/observability.hpp"
#include "../include/permission_gate.hpp"
#include "../include/trace.hpp"
#include "../include/win_utf8.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

std::shared_ptr<IUIPort> createConsoleUI();
std::shared_ptr<IModelProvider> createMockProvider(const std::string& model_name);
std::shared_ptr<ISandbox> createWindowsSandbox(SandboxConfig config);
void registerCoreTools(ToolRegistry& registry,
                       std::shared_ptr<ISandbox> sandbox,
                       std::shared_ptr<IUIPort> ui);

namespace fs = std::filesystem;

class AgentSystem {
public:
    AgentSystem()
        : sessions_("./sessions")
        , memory_(".") {
        ui_ = createConsoleUI();
        sandbox_ = createWindowsSandbox(SandboxConfig{});
        tools_ = std::make_shared<ToolRegistry>();
        registerCoreTools(*tools_, sandbox_, ui_);
        provider_ = createMockProvider("gpt-4");

        permission_ = std::make_shared<PersistentPermissionGate>(".af-permissions.json");

        context_ = std::make_shared<ContextManager>(BudgetConfig{});
        context_->setSystemPrompt(
            "你是 AgentForge 编码 Agent。需要文件内容时调用工具，不要猜测。");
        context_->setProjectKnowledge(memory_.loadProjectKnowledge() + memory_.loadMemory());
        context_->setMaxHistoryMessages(40);

        repo_ = std::make_shared<RepoIndex>();
        repo_->build(".");
        context_->setRepoMap(repo_->renderMap(8000));

        current_session_id_ = sessions_.createSession("console");
        ui_->output("[L1-L12] AgentForge v2 初始化完成");
        ui_->output("[L6] Sandbox: " + sandbox_->getStatus().substr(0, 80) + "...");
        ui_->output("[L8] Repo: " + std::to_string(repo_->getStats().total_files) +
                    " files, " + std::to_string(repo_->getStats().total_symbols) + " symbols");
        ui_->output("[L10] Session: " + current_session_id_);
    }

    void run() {
        ui_->output("\n输入任务或命令 (/help 查看帮助):");
        while (true) {
            std::string input = ui_->input("> ");
            if (input.empty()) continue;
            if (input[0] == '/') {
                if (!handleCommand(input)) break;
                continue;
            }
            executeTask(input);
        }
        saveCurrentSession();
        ui_->output("\n再见!");
    }

private:
    std::shared_ptr<IUIPort> ui_;
    std::shared_ptr<IModelProvider> provider_;
    std::shared_ptr<ToolRegistry> tools_;
    std::shared_ptr<ISandbox> sandbox_;
    std::shared_ptr<PersistentPermissionGate> permission_;
    std::shared_ptr<ContextManager> context_;
    std::shared_ptr<RepoIndex> repo_;
    SessionStore sessions_;
    MemoryStore memory_;
    std::string current_session_id_;
    std::vector<Message> messages_;

    bool handleCommand(const std::string& cmd) {
        if (cmd == "/quit" || cmd == "/q") return false;
        if (cmd == "/help" || cmd == "/h") {
            ui_->output("\n命令: /tools /cost /context /index /map /sessions /save /load <id> /memory /quit");
        } else if (cmd == "/tools") {
            for (const auto& def : tools_->getAllTools()) {
                ui_->output("  - " + def.name + ": " + def.description);
            }
            ui_->output("  - Task: 并行只读子代理");
        } else if (cmd == "/cost") {
            ui_->output("请使用: agentforge_v2 --cost conversations");
        } else if (cmd == "/context") {
            ui_->output("context tokens: " + std::to_string(context_->lastContextTokens()));
            ui_->output("repo map chars: " + std::to_string(context_->budget().l2_repo_map));
        } else if (cmd == "/index") {
            repo_->build(".");
            context_->setRepoMap(repo_->renderMap(8000));
            ui_->output("索引完成: " + std::to_string(repo_->getStats().total_files) + " files");
        } else if (cmd == "/map") {
            ui_->output(repo_->renderMap(6000));
        } else if (cmd == "/sessions") {
            for (const auto& meta : sessions_.listSessions()) {
                ui_->output("  " + meta.id + "  " + meta.title + "  (" +
                            std::to_string(meta.message_count) + " msgs)");
            }
        } else if (cmd == "/save") {
            saveCurrentSession();
            ui_->output("会话已保存");
        } else if (cmd.rfind("/load ", 0) == 0) {
            auto session = sessions_.loadSession(cmd.substr(6));
            if (!session) {
                ui_->error("会话不存在");
            } else {
                current_session_id_ = session->id;
                messages_ = session->messages;
                ui_->output("已加载 " + session->id + "，继续输入即可");
            }
        } else if (cmd == "/memory") {
            for (const auto& fact : memory_.facts()) ui_->output("  - " + fact);
        } else {
            ui_->error("未知命令");
        }
        return true;
    }

    void executeTask(const std::string& task) {
        messages_.emplace_back(Role::User, task);

        auto context = std::make_shared<ContextManager>(context_->budget());
        context->setSystemPrompt("你是 AgentForge 编码 Agent。需要文件内容时调用工具，不要猜测。");
        context->setProjectKnowledge(memory_.loadProjectKnowledge() + memory_.loadMemory());
        context->setRepoMap(repo_->renderMap(8000));
        context->setMaxHistoryMessages(40);

        auto loop = std::make_shared<AgentLoop>(provider_, tools_, ui_);
        loop->setContextManager(context);
        loop->setPermissionGate(permission_);
        auto task_tracer = std::make_shared<JsonlTracer>();
        loop->setTracer(task_tracer);

        LoopLimits limits;
        limits.max_steps = 20;
        limits.max_tokens = 100000;
        limits.max_duration_sec = 300;
        limits.max_repeat_calls = 3;
        limits.max_consecutive_errors = 3;
        limits.dump_dir = ".af-dump";

        std::vector<Message> out;
        auto start = std::chrono::steady_clock::now();
        LoopStats stats = loop->run(messages_, limits, out);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start).count();

        messages_ = out;
        ui_->output("\n步骤 " + std::to_string(stats.total_steps) +
                    "，工具 " + std::to_string(stats.tool_calls) +
                    "，tokens " + std::to_string(stats.total_usage.total()) +
                    "，压缩 " + std::to_string(stats.compactions) +
                    "，用时 " + std::to_string(ms) + " ms");
        memory_.autoRemember(task, "");
        saveCurrentSession();
    }

    void saveCurrentSession() {
        Session session;
        session.id = current_session_id_;
        session.created_at = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        session.updated_at = session.created_at;
        session.title = messages_.empty() ? "console" : utf8Prefix(messages_.front().content, 40);
        session.messages = messages_;
        sessions_.saveSession(session);
    }
};

// ---------------------------------------------------------------------------
// 仪表盘
// ---------------------------------------------------------------------------
namespace {

long long tagLong(const json& tags, const std::string& key) {
    if (!tags.is_object() || !tags.contains(key)) return 0;
    const auto& v = tags[key];
    try {
        if (v.is_number_integer()) return v.get<long long>();
        if (v.is_number()) return (long long)v.get<double>();
        if (v.is_string()) return std::stoll(v.get<std::string>());
    } catch (...) {}
    return 0;
}

int runCostDashboard(const std::string& dir) {
    namespace fs2 = std::filesystem;
    if (!fs2::exists(utf8ToPath(dir))) {
        std::cerr << "trace 目录不存在: " << dir << "\n";
        return 1;
    }
    int requests = 0, tools = 0, bad = 0;
    long long prompt = 0, completion = 0, wall = 0;
    for (const auto& entry : fs2::recursive_directory_iterator(utf8ToPath(dir))) {
        if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") continue;
        std::ifstream in(entry.path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            try {
                json j = json::parse(line);
                if (j.value("type", "") != "span") continue;
                wall += j.value("duration_ms", 0LL);
                std::string name = j.value("name", "");
                if (name == "chat") ++requests;
                if (name.rfind("tool.", 0) == 0) ++tools;
                prompt += tagLong(j["tags"], "prompt_tokens");
                completion += tagLong(j["tags"], "completion_tokens");
            } catch (...) {
                ++bad;
            }
        }
    }
    std::cout << "requests=" << requests << " tools=" << tools
              << " prompt=" << prompt << " completion=" << completion
              << " wall_ms=" << wall << " bad=" << bad << "\n";
    return 0;
}

int runContextDashboard() {
    RepoIndex repo;
    repo.build(".");
    BudgetConfig budget;
    ContextManager context(budget);
    context.setSystemPrompt("你是 AgentForge 编码 Agent。");
    context.setRepoMap(repo.renderMap((std::size_t)budget.l2_repo_map * 3));
    context.build({});
    auto stats = repo.getStats();
    std::cout << "repo_files=" << stats.total_files
              << " repo_symbols=" << stats.total_symbols
              << " system_tokens=" << context.estimateSystemTokens()
              << " assembled_tokens=" << context.lastContextTokens()
              << " budget=" << budget.total_budget << "\n";
    return 0;
}

int runRegressDashboard(const std::string& file) {
    std::ifstream in(utf8ToPath(file));
    if (!in) {
        std::cerr << "regress 文件不存在\n";
        return 1;
    }
    int total = 0, pass = 0;
    double cost = 0;
    long long steps = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json j;
        try {
            j = json::parse(line);
        } catch (...) {
            return 2;
        }
        if (!j.contains("pass") || !j.contains("cost") || !j.contains("steps")) {
            std::cerr << "缺少 pass/cost/steps 字段\n";
            return 2;
        }
        ++total;
        if (j["pass"].get<bool>()) ++pass;
        cost += j["cost"].get<double>();
        steps += j["steps"].get<long long>();
    }
    std::cout << "tasks=" << total << " pass_rate="
              << (total ? 100.0 * pass / total : 0.0) << "% cost=" << cost
              << " steps=" << steps << "\n";
    return 0;
}

int runSelfTest() {
    int ok = 0, fail = 0;
    auto check = [&](bool cond, const std::string& name) {
        std::cout << (cond ? "[PASS] " : "[FAIL] ") << name << "\n";
        cond ? ++ok : ++fail;
    };

    std::string bad = "abc";
    bad.push_back((char)0xB6);
    check(sanitizeUtf8(bad) != bad, "utf8 sanitize");

    ToolRegistry registry;
    ToolDef def{"Read", "", json{{"type", "object"}, {"required", json::array({"path"})}},
               RiskLevel::Read};
    registry.registerTool(def, [](const json&) { return ToolResult{"", "ok"}; });
    ToolCall missing;
    missing.name = "Read";
    missing.arguments = json::object();
    check(registry.execute(missing).is_error, "schema validation");

    auto tmp = std::filesystem::temp_directory_path() / ("af_selftest_" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()));
    std::filesystem::create_directories(tmp);

    PersistentPermissionGate gate((tmp / "perm.json").string());
    ToolDef wdef{"Write", "", json::object(), RiskLevel::Write};
    gate.remember("Write", "", Decision::Deny, "selftest");
    check(gate.evaluate(wdef, json::object()) == Decision::Deny, "permission deny");
    gate.remember("Write", "", Decision::Allow, "selftest");
    check(gate.evaluate(wdef, json::object()) == Decision::Allow, "permission allow");
    gate.remember("Bash", "rm", Decision::Deny, "selftest");
    gate.remember("Bash", "", Decision::Allow, "selftest");
    ToolDef bdef{"Bash", "", json::object(), RiskLevel::Exec};
    check(gate.evaluate(bdef, json{{"command", "rm x"}}) == Decision::Deny,
          "deny wins over allow");
    check(gate.evaluate(bdef, json{{"command", "echo hi"}}) == Decision::Allow,
          "wildcard allow");

    BudgetConfig budget;
    budget.total_budget = 8000;
    budget.l5_history = 2000;
    ContextManager context(budget);
    context.setSystemPrompt("system");
    std::vector<Message> history;
    for (int i = 0; i < 60; ++i) {
        history.emplace_back(Role::User, std::string(1000, 'x'));
        history.emplace_back(Role::Assistant, "ok");
    }
    int before = context.estimateTokens(history);
    CompactionEvent event;
    Usage usage;
    auto mock = createMockProvider("mock");
    auto compacted = context.compact(history, *mock, event, usage);
    check(context.estimateTokens(compacted) < before, "context compaction");

    RepoIndex repo;
    check(repo.build("."), "repo build");
    check(repo.getStats().total_symbols > 0, "tree-sitter symbols");
    check(!repo.search("AgentLoop").empty(), "retrieval");

    SessionStore store((tmp / "sessions").string());
    auto id = store.createSession("selftest");
    Session session;
    session.id = id;
    session.messages.emplace_back(Role::User, "hi");
    check(store.saveSession(session), "session save");
    check(store.loadSession(id).has_value(), "session load");

    MemoryStore memory((tmp / "mem").string());
    std::filesystem::create_directories(tmp / "mem");
    check(memory.appendFact("fact", "selftest"), "memory append");
    check(!memory.appendFact("fact", "selftest"), "memory dedupe");

    JsonlTracer tracer;
    tracer.spanStart("chat");
    tracer.spanEnd("chat");
    tracer.event("tool.Read");
    auto tj = tracer.toJson();
    check(tj["span_count"].get<int>() == 1 && tj["event_count"].get<int>() == 1, "tracer");

    std::filesystem::remove_all(tmp);
    std::cout << "selftest: " << ok << " passed, " << fail << " failed\n";
    return fail == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--cost") return runCostDashboard(argc > 2 ? argv[2] : "conversations");
        if (arg == "--context") return runContextDashboard();
        if (arg == "--selftest") return runSelfTest();
        if (arg == "--regress") {
            if (argc < 3) return 1;
            return runRegressDashboard(argv[2]);
        }
    }

    try {
        AgentSystem system;
        system.run();
    } catch (const std::exception& e) {
        std::cerr << "致命错误: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
