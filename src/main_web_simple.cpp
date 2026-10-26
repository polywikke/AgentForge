// ============================================================================
// AgentForge Web Server (v2.1) —— L1-L12 集成
// ============================================================================
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "httplib.h"

#include "win_utf8.hpp"
#include "types.hpp"
#include "tool_registry.hpp"
#include "sandbox.hpp"
#include "permission_gate.hpp"
#include "model_provider.hpp"
#include "multi_provider.hpp"
#include "simple_http_transport.hpp"
#include "context_manager.hpp"
#include "repo_index.hpp"
#include "session_store.hpp"
#include "subagent.hpp"
#include "trace.hpp"
#include "sse_ui_adapter.hpp"
#include "agent_loop.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

void registerCoreTools(ToolRegistry& registry,
                       std::shared_ptr<ISandbox> sandbox,
                       std::shared_ptr<IUIPort> ui);
std::shared_ptr<ISandbox> createWindowsSandbox(SandboxConfig config);

namespace {

std::mutex g_config_mutex;
json g_config;
fs::path g_config_path;
fs::path g_root;

std::shared_ptr<ToolRegistry> g_tools;
std::shared_ptr<ISandbox> g_sandbox;
std::shared_ptr<PersistentPermissionGate> g_permission;
std::shared_ptr<RepoIndex> g_repo;
std::shared_ptr<SessionStore> g_sessions;
std::shared_ptr<MemoryStore> g_memory;

std::mutex g_active_mutex;
std::map<std::string, std::shared_ptr<std::atomic<bool>>> g_active;
std::atomic<unsigned long long> g_seq{1};

std::mutex g_metrics_mutex;
json g_metrics = json::object();
json g_trace = json::object();

struct PendingPermission {
    std::mutex mutex;
    std::condition_variable cv;
    bool answered = false;
    bool allowed = false;
    std::string tool;
    json args = json::object();
};
std::mutex g_pending_mutex;
std::map<std::string, std::shared_ptr<PendingPermission>> g_pending;

const char* kPrompt =
    "你是 AgentForge, 一个运行在 Windows 上的编码 Agent。\n"
    "工具: Read / Write / Edit / Glob / Grep / Bash / Task。\n"
    "规则:\n"
    "1. 涉及文件/命令时必须调用工具, 不要凭猜测回答。\n"
    "2. 参数必须是合法 JSON; 修改文件前先 Read。\n"
    "3. Task 是只读子代理, 适合并行调研, 修改由主代理完成。\n"
    "4. 使用用户的语言, 简洁准确, 不要编造工具结果。\n"
    "Windows 路径: 桌面 = %USERPROFILE%\\Desktop; 工具会展开 %VAR% 和 ~。";

void mergeJson(json& base, const json& patch) {
    if (!patch.is_object() || !base.is_object()) { base = patch; return; }
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (base.contains(it.key()) && base[it.key()].is_object() &&
            it.value().is_object()) {
            mergeJson(base[it.key()], it.value());
        } else {
            base[it.key()] = it.value();
        }
    }
}

json defaultConfig() {
    return json{
        {"server", {{"host", "127.0.0.1"}, {"port", 8080}, {"web_root", "./web"}}},
        {"api", {{"provider", "custom"}, {"endpoint", ""}, {"key", ""},
                 {"model", "claude-sonnet-5-5"}, {"temperature", 0.7},
                 {"max_tokens", 4096}}},
        {"agent", {{"max_steps", 20}, {"time_limit_seconds", 300},
                   {"max_context_tokens", 100000}, {"max_total_tokens", 500000},
                   {"max_history_messages", 40},
                   {"max_repeat_calls", 3}, {"max_consecutive_errors", 3},
                   {"compaction", true}, {"llm_summary", false},
                   {"auto_memory", true}, {"max_subagent_depth", 2}}},
        {"tools", {{"sandbox", true}, {"work_dir", ""}, {"dump_dir", ".af-dump"},
                   {"persist_threshold", 20000}, {"permission_mode", "ask"},
                   {"workspace_root", ""}}},
        {"retrieval", {{"enabled", true}, {"top_k", 5}}},
        {"systemPrompt", kPrompt}};
}

fs::path exeDir() {
    wchar_t buf[MAX_PATH * 4] = {0};
    DWORD len = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return len ? fs::path(buf).parent_path() : fs::current_path();
}

fs::path findAsset(const std::string& rel) {
    for (const auto& p : {fs::path(rel), exeDir() / rel, exeDir() / ".." / rel,
                          exeDir() / ".." / ".." / rel}) {
        std::error_code ec;
        if (fs::exists(p, ec)) return fs::weakly_canonical(p);
    }
    return {};
}

void logLine(const std::string& s) { std::cout << s << std::endl; }

json maskedConfig() {
    json copy = g_config;
    if (copy.contains("api") && copy["api"].contains("key")) {
        std::string key = copy["api"].value("key", "");
        copy["api"]["hasKey"] = !key.empty();
        if (key.size() > 8) copy["api"]["key"] = key.substr(0, 8) + "...";
        else if (!key.empty()) copy["api"]["key"] = "***";
    }
    return copy;
}

bool apiConfigured() {
    const auto& api = g_config["api"];
    return !api.value("endpoint", "").empty() && !api.value("key", "").empty();
}

// 允许用户只填 base_url：自动补 /v1/chat/completions 或 /v1/messages
std::string normalizeChatEndpoint(std::string endpoint, const std::string& provider) {
    while (!endpoint.empty() &&
           (endpoint.back() == ' ' || endpoint.back() == '\t' ||
            endpoint.back() == '\r' || endpoint.back() == '\n')) {
        endpoint.pop_back();
    }
    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
    if (endpoint.empty()) return endpoint;

    // 已经是完整端点
    if (endpoint.find("/chat/completions") != std::string::npos) return endpoint;
    if (endpoint.size() >= 8 &&
        endpoint.compare(endpoint.size() - 8, 8, "/messages") == 0) {
        return endpoint;
    }

    const bool anthropic = provider == "anthropic" ||
                           endpoint.find("anthropic") != std::string::npos;
    const std::string suffix = anthropic ? "/messages" : "/chat/completions";
    if (endpoint.size() >= 3 && endpoint.compare(endpoint.size() - 3, 3, "/v1") == 0) {
        return endpoint + suffix;
    }
    if (endpoint.find("/v1/") != std::string::npos) {
        return endpoint + suffix;
    }
    return endpoint + "/v1" + suffix;
}

agentforge::APIConfig makeApiConfig() {
    agentforge::APIConfig cfg;
    const auto& api = g_config["api"];
    cfg.api_key = api.value("key", "");
    std::string endpoint = api.value("endpoint", "");
    std::string provider = api.value("provider", "custom");

    // base_url 也能用：根据域名/路径推断 provider，再补全协议路径
    if (provider == "custom" || provider.empty()) {
        if (endpoint.find("anthropic") != std::string::npos) provider = "anthropic";
        else if (endpoint.find("deepseek") != std::string::npos) provider = "deepseek";
        else if (endpoint.find("openai") != std::string::npos) provider = "openai";
    }

    cfg.api_endpoint = normalizeChatEndpoint(endpoint, provider);
    cfg.model = api.value("model", "claude-sonnet-5-5");
    cfg.temperature = api.value("temperature", 0.7);
    cfg.max_tokens = api.value("max_tokens", 4096);

    if (provider == "anthropic") cfg.provider = agentforge::APIConfig::Anthropic;
    else if (provider == "openai") cfg.provider = agentforge::APIConfig::OpenAI;
    else if (provider == "deepseek") cfg.provider = agentforge::APIConfig::DeepSeek;
    else cfg.provider = agentforge::APIConfig::Custom;

    if (cfg.api_endpoint.empty()) {
        if (cfg.provider == agentforge::APIConfig::Anthropic)
            cfg.api_endpoint = "https://api.anthropic.com/v1/messages";
        else if (cfg.provider == agentforge::APIConfig::OpenAI)
            cfg.api_endpoint = "https://api.openai.com/v1/chat/completions";
        else if (cfg.provider == agentforge::APIConfig::DeepSeek)
            cfg.api_endpoint = "https://api.deepseek.com/v1/chat/completions";
    }
    return cfg;
}

std::string systemPrompt() {
    const auto& sp = g_config.value("systemPrompt", std::string(kPrompt));
    return sp.empty() ? kPrompt : sp;
}

// 从 chat/completions 地址推导 models 地址
std::string modelsUrlFromEndpoint(const std::string& endpoint) {
    std::string ep = endpoint;
    while (!ep.empty() && ep.back() == '/') ep.pop_back();
    auto replace_suffix = [&](const std::string& suffix, const std::string& repl) -> bool {
        if (ep.size() >= suffix.size() &&
            ep.compare(ep.size() - suffix.size(), suffix.size(), suffix) == 0) {
            ep.replace(ep.size() - suffix.size(), suffix.size(), repl);
            return true;
        }
        return false;
    };
    if (replace_suffix("/chat/completions", "/models")) return ep;
    if (replace_suffix("/v1/messages", "/v1/models")) return ep;
    auto pos = ep.find("/v1/");
    if (pos != std::string::npos) return ep.substr(0, pos) + "/v1/models";
    if (ep.size() >= 3 && ep.compare(ep.size() - 3, 3, "/v1") == 0) return ep + "/models";
    return ep + "/v1/models";
}

std::vector<Message> parseMessages(const json& body) {
    std::vector<Message> out;
    if (body.contains("messages") && body["messages"].is_array()) {
        for (const auto& item : body["messages"]) {
            if (!item.is_object() || !item.contains("role") || !item.contains("content")) continue;
            std::string role = item.value("role", "user");
            std::string content = item["content"].is_string()
                                      ? item["content"].get<std::string>()
                                      : item["content"].dump();
            if (role == "system") out.emplace_back(Role::System, content);
            else if (role == "assistant") out.emplace_back(Role::Assistant, content);
            else if (role == "tool") {
                Message m(Role::Assistant, content);
                m.call_id = item.value("tool_call_id", "");
                out.push_back(m);
            } else out.emplace_back(Role::User, content);
        }
    } else if (body.contains("description") && body["description"].is_string()) {
        out.emplace_back(Role::User, body["description"].get<std::string>());
    } else if (body.contains("task") && body["task"].is_string()) {
        out.emplace_back(Role::User, body["task"].get<std::string>());
    }
    return out;
}

std::vector<Message> stripSystem(std::vector<Message> messages) {
    messages.erase(std::remove_if(messages.begin(), messages.end(),
                                  [](const Message& m) { return m.role == Role::System; }),
                   messages.end());
    return messages;
}

std::string lastUser(const std::vector<Message>& msgs) {
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        if (it->role == Role::User) return it->content;
    return "";
}

std::string lastAssistant(const std::vector<Message>& msgs) {
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        if (it->role == Role::Assistant && it->tool_calls.empty() && !it->content.empty())
            return it->content;
    return "";
}

json usageJson(const Usage& u) {
    return json{{"prompt_tokens", u.prompt_tokens},
                {"completion_tokens", u.completion_tokens},
                {"total_tokens", u.total()}};
}

double costUsd(const Usage& u) {
    return (u.prompt_tokens / 1000000.0) * 3.0 +
           (u.completion_tokens / 1000000.0) * 15.0 +
           (u.cache_read_tokens / 1000000.0) * 0.15;
}

std::string nowId() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

BudgetConfig makeBudget() {
    BudgetConfig b;
    int ctx = g_config["agent"].value("max_context_tokens", 100000);
    if (ctx <= 0) {
        // 0 = 不限：给一个足够大的预算，同时限制地图/检索，避免启动时拉满内存
        b.total_budget = 1000000000;
        b.l2_repo_map = 16000;
        b.l3_search = 12000;
        b.l5_history = 1000000000;
        return b;
    }
    b.total_budget = ctx;
    b.l2_repo_map = std::max(4000, b.total_budget / 12);
    b.l3_search = std::max(4000, b.total_budget / 12);
    b.l5_history = std::max(8000, b.total_budget / 2);
    return b;
}

std::string workspaceForSession(const std::string& session_id) {
    std::string configured;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        configured = g_config["tools"].value("workspace_root", "");
    }
    fs::path root;
    if (configured.empty()) {
        root = utf8ToPath(g_sessions->conversationDir(session_id)) / "workspace";
    } else {
        root = utf8ToPath(resolveUserPath(configured));
        if (!root.is_absolute()) root = g_root / root;
    }
    std::error_code ec;
    fs::create_directories(root, ec);
    fs::path canonical = fs::weakly_canonical(root, ec);
    if (ec) canonical = fs::absolute(root, ec).lexically_normal();
    return pathToUtf8(canonical);
}

std::string permissionPrefix(const std::string& tool, const json& args) {
    if (tool != "Bash" || !args.is_object()) return "";
    std::string command = args.value("command", "");
    std::string token;
    for (char c : command) {
        if (std::isspace((unsigned char)c)) break;
        token += c;
    }
    return token;
}

bool askPermission(httplib::DataSink& sink, const std::string& tool, const json& args,
                   std::shared_ptr<std::atomic<bool>> cancel) {
    if (!sink.is_writable() || (cancel && cancel->load())) return false;
    std::string id = nowId() + "-" + std::to_string(g_seq.fetch_add(1));
    auto pending = std::make_shared<PendingPermission>();
    pending->tool = tool;
    pending->args = args;
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending[id] = pending;
    }
    std::string msg = "event: permission_request\ndata: " +
                      json{{"id", id}, {"tool", tool}, {"arguments", args}}.dump() + "\n\n";
    if (!sink.write(msg.c_str(), msg.size())) {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending.erase(id);
        return false;
    }
    bool allowed = false;
    {
        std::unique_lock<std::mutex> lock(pending->mutex);
        if (pending->cv.wait_for(lock, std::chrono::seconds(65),
                                 [&] { return pending->answered; })) {
            allowed = pending->allowed;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending.erase(id);
    }
    return allowed;
}

void sendJson(httplib::Response& res, const json& j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

json messageToApiJson(const Message& m) {
    json item{{"role", m.call_id.has_value() ? "tool" :
                       m.role == Role::User ? "user" :
                       m.role == Role::Assistant ? "assistant" : "system"},
              {"content", m.content}};
    if (!m.tool_calls.empty()) {
        item["tool_calls"] = json::array();
        for (const auto& c : m.tool_calls)
            item["tool_calls"].push_back({{"id", c.id}, {"name", c.name},
                                           {"arguments", c.arguments}});
    }
    if (m.call_id) item["tool_call_id"] = *m.call_id;
    return item;
}

} // namespace

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::cout << "\n    AgentForge Web Server v2.1 (L1-L12)\n\n";

    std::string config_arg, web_override;
    int port_override = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) config_arg = argv[++i];
        else if ((arg == "--port" || arg == "-p") && i + 1 < argc) port_override = std::atoi(argv[++i]);
        else if (arg == "--web" && i + 1 < argc) web_override = argv[++i];
    }

    if (!config_arg.empty()) g_config_path = fs::path(config_arg);
    else {
        fs::path found = findAsset("config.json");
        g_config_path = found.empty() ? fs::path("config.json") : found;
    }

    g_config = defaultConfig();
    if (fs::exists(g_config_path)) {
        try {
            std::ifstream in(g_config_path);
            json loaded;
            in >> loaded;
            mergeJson(g_config, loaded);
            logLine("[Config] Loaded from " + pathToUtf8(g_config_path));
        } catch (const std::exception& e) {
            logLine(std::string("[Config] parse failed: ") + e.what());
        }
    }

    fs::path root_asset = findAsset("CMakeLists.txt");
    g_root = root_asset.empty() ? fs::current_path() : root_asset.parent_path();

    std::string host = g_config["server"].value("host", "127.0.0.1");
    int port = port_override > 0 ? port_override : g_config["server"].value("port", 8080);

    std::string web_root = web_override;
    if (web_root.empty()) web_root = g_config["server"].value("web_root", "./web");
    fs::path web_path = findAsset(web_root);
    web_root = web_path.empty() ? web_root : pathToUtf8(web_path);

    if (!apiConfigured()) logLine("[Warning] API not configured (/setup.html)");
    else logLine("[API] " + g_config["api"].value("endpoint", "") + "  model=" +
                 g_config["api"].value("model", ""));

    SandboxConfig sc;
    sc.work_dir = g_config["tools"].value("work_dir", std::string(""));
    if (sc.work_dir.empty()) sc.work_dir = pathToUtf8(g_root);
    sc.timeout_seconds = 60;
    if (g_config["tools"].value("sandbox", true)) {
        g_sandbox = createWindowsSandbox(sc);
        logLine("[Init] Sandbox: " + sc.work_dir);
    }

    g_tools = std::make_shared<ToolRegistry>();
    registerCoreTools(*g_tools, g_sandbox, std::shared_ptr<IUIPort>());
    logLine("[Init] Core tools: " + std::to_string(g_tools->listTools().size()));

    g_permission = std::make_shared<PersistentPermissionGate>(
        pathToUtf8(g_root / ".af-permissions.json"));
    std::string mode = g_config["tools"].value("permission_mode", "ask");
    if (mode == "allow_all") g_permission->setDefaultDecision(Decision::Allow);
    else if (mode == "deny_all") g_permission->setDefaultDecision(Decision::Deny);

    // 每次对话一个目录：<project>/conversations/<session_id>/
    // 兼容旧版本：把 sessions/ 下的会话迁移到 conversations/
    fs::path conversations = g_root / "conversations";
    fs::path legacy_sessions = g_root / "sessions";
    std::error_code conv_ec;
    fs::create_directories(conversations, conv_ec);
    if (fs::exists(legacy_sessions, conv_ec)) {
        for (const auto& entry : fs::directory_iterator(legacy_sessions, conv_ec)) {
            if (!entry.is_directory()) continue;
            fs::path dst = conversations / entry.path().filename();
            if (!fs::exists(dst, conv_ec)) {
                fs::copy(entry.path(), dst,
                         fs::copy_options::recursive, conv_ec);
            }
        }
        logLine("[Init] Migrated legacy sessions -> conversations/");
    }
    g_sessions = std::make_shared<SessionStore>(pathToUtf8(conversations));
    g_memory = std::make_shared<MemoryStore>(pathToUtf8(g_root));

    g_repo = std::make_shared<RepoIndex>();
    logLine("[Init] Indexing repo (tree-sitter)...");
    g_repo->build(pathToUtf8(g_root));
    auto rs = g_repo->getStats();
    logLine("[Init] Repo: " + std::to_string(rs.total_files) + " files, " +
            std::to_string(rs.total_symbols) + " symbols");

    httplib::Server svr;
    svr.set_read_timeout(300, 0);
    svr.set_write_timeout(300, 0);
    svr.set_idle_interval(1, 0);
    svr.set_payload_max_length(32 * 1024 * 1024);
    if (!svr.set_mount_point("/", web_root.c_str()))
        logLine("[Warning] web root not found: " + web_root);

    // ------------------------- 基础 API -------------------------
    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        auto r = g_repo->getStats();
        sendJson(res, {{"status", "ok"}, {"version", "2.1"},
                       {"api_configured", apiConfigured()},
                       {"tools", g_tools->listTools()},
                       {"features", json::array({"stream", "tools", "permissions",
                                                 "sessions", "checkpoints", "repo_map",
                                                 "retrieval", "compaction", "tasks",
                                                 "tracing", "metrics"})},
                       {"repo_files", r.total_files}, {"repo_symbols", r.total_symbols},
                       {"sessions", (int)g_sessions->listSessions().size()}});
    });

    svr.Get("/api/config", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        sendJson(res, maskedConfig());
    });

    svr.Post("/api/config", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json patch = json::parse(req.body);
            if (patch.contains("endpoint") || patch.contains("apiKey") ||
                patch.contains("model") || patch.contains("apiProvider")) {
                json api = json::object();
                if (patch.contains("apiProvider")) api["provider"] = patch["apiProvider"];
                if (patch.contains("endpoint")) api["endpoint"] = patch["endpoint"];
                if (patch.contains("apiEndpoint")) api["endpoint"] = patch["apiEndpoint"];
                if (patch.contains("apiKey")) api["key"] = patch["apiKey"];
                if (patch.contains("key")) api["key"] = patch["key"];
                if (patch.contains("model")) api["model"] = patch["model"];
                if (patch.contains("temperature")) api["temperature"] = patch["temperature"];
                if (patch.contains("maxTokens")) api["max_tokens"] = patch["maxTokens"];
                patch = json{{"api", api}};
            }
            if (patch.contains("api") && patch["api"].is_object() &&
                patch["api"].contains("key") && patch["api"]["key"].is_string()) {
                std::string key = patch["api"]["key"].get<std::string>();
                if (key.empty() || key.find('*') != std::string::npos ||
                    (key.size() > 3 && key.compare(key.size() - 3, 3, "...") == 0)) {
                    patch["api"].erase("key");
                }
            }
            {
                std::lock_guard<std::mutex> lock(g_config_mutex);
                if (patch.contains("api") && patch["api"].is_object() &&
                    patch["api"].contains("endpoint")) {
                    std::string ep = patch["api"].value("endpoint", "");
                    if (ep.find("/v1/messages") != std::string::npos ||
                        ep.find("anthropic") != std::string::npos) {
                        patch["api"]["provider"] = "anthropic";
                    } else if (ep.find("/chat/completions") != std::string::npos &&
                               g_config["api"].value("provider", "") == "anthropic") {
                        patch["api"]["provider"] = "custom";
                    }
                }
                mergeJson(g_config, patch);
                std::ofstream out(g_config_path);
                if (!out) throw std::runtime_error("write config failed");
                out << g_config.dump(2);
            }
            sendJson(res, {{"status", "ok"}});
        } catch (const std::exception& e) {
            sendJson(res, {{"status", "error"}, {"message", e.what()}}, 400);
        }
    });

    svr.Get("/api/tools", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& def : g_tools->getAllTools())
            arr.push_back({{"name", def.name}, {"description", def.description},
                           {"risk", (int)def.risk_level}, {"parameters", def.parameters}});
        auto task = makeTaskToolDef();
        arr.push_back({{"name", task.name}, {"description", task.description},
                       {"risk", (int)task.risk_level}, {"parameters", task.parameters}});
        sendJson(res, arr);
    });

    // 从 OpenAI 兼容 / Anthropic 的 /models 拉取模型列表（不落盘）
    svr.Post("/api/models", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = req.body.empty() ? json::object() : json::parse(req.body);
            std::string endpoint = body.value("endpoint", "");
            std::string key = body.value("key", "");
            json cfg;
            { std::lock_guard<std::mutex> lock(g_config_mutex); cfg = g_config; }
            if (endpoint.empty()) endpoint = cfg["api"].value("endpoint", "");
            if (key.empty() || key.find('*') != std::string::npos ||
                (key.size() > 3 && key.compare(key.size() - 3, 3, "...") == 0)) {
                key = cfg["api"].value("key", "");
            }
            if (endpoint.empty() || key.empty()) {
                sendJson(res, {{"status", "error"},
                               {"message", "endpoint 或 key 为空（可先保存设置）"}}, 400);
                return;
            }

            const std::string url = modelsUrlFromEndpoint(endpoint);
            const bool anthropic = endpoint.find("/v1/messages") != std::string::npos ||
                                   endpoint.find("anthropic") != std::string::npos;
            HttpRequest hr;
            hr.url = url;
            hr.method = "GET";
            hr.timeout_ms = 20000;
            hr.headers["Accept"] = "application/json";
            hr.headers["Content-Type"] = "application/json";
            if (anthropic) {
                hr.headers["x-api-key"] = key;
                hr.headers["anthropic-version"] = "2023-06-01";
            } else {
                hr.headers["Authorization"] = "Bearer " + key;
            }

            SimpleHTTPTransport transport;
            HttpResponse resp = transport.request(hr);
            if (resp.is_error) {
                sendJson(res, {{"status", "error"}, {"message", resp.error_message},
                               {"url", url}}, 502);
                return;
            }
            if (resp.status_code < 200 || resp.status_code >= 300) {
                sendJson(res, {{"status", "error"},
                               {"message", "HTTP " + std::to_string(resp.status_code) +
                                           ": " + utf8Prefix(resp.body, 300)},
                               {"url", url}}, 502);
                return;
            }

            json parsed = json::parse(resp.body);
            std::set<std::string> unique;
            if (parsed.contains("data") && parsed["data"].is_array()) {
                for (const auto& item : parsed["data"]) {
                    if (item.is_string()) unique.insert(item.get<std::string>());
                    else if (item.is_object() && item.contains("id") &&
                             item["id"].is_string()) {
                        unique.insert(item["id"].get<std::string>());
                    }
                }
            } else if (parsed.contains("models") && parsed["models"].is_array()) {
                for (const auto& item : parsed["models"]) {
                    if (item.is_string()) unique.insert(item.get<std::string>());
                    else if (item.is_object() && item.contains("id") &&
                             item["id"].is_string()) {
                        unique.insert(item["id"].get<std::string>());
                    }
                }
            }
            json models = json::array();
            for (const auto& m : unique) {
                models.push_back(m);
                if (models.size() >= 200) break;
            }
            sendJson(res, {{"status", "ok"}, {"url", url}, {"count", (int)models.size()},
                           {"models", models}});
        } catch (const std::exception& e) {
            sendJson(res, {{"status", "error"},
                           {"message", std::string("解析失败: ") + e.what()}}, 400);
        }
    });

    // ------------------------- 权限 -------------------------
    svr.Get("/api/permissions", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& rule : g_permission->rules())
            arr.push_back({{"tool", rule.tool_pattern}, {"arg_prefix", rule.arg_prefix},
                           {"decision", rule.decision == Decision::Allow ? "allow" :
                                        rule.decision == Decision::Deny ? "deny" : "ask"},
                           {"reason", rule.reason}});
        sendJson(res, {{"file", g_permission->filePath()}, {"rules", arr}});
    });

    svr.Post("/api/permissions", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            if (body.value("action", "") == "reset") {
                g_permission->resetPersistent();
                sendJson(res, {{"status", "ok"}});
                return;
            }
            std::string decision = body.value("decision", "ask");
            Decision d = decision == "allow" ? Decision::Allow :
                         decision == "deny" ? Decision::Deny : Decision::Ask;
            g_permission->remember(body.value("tool", "*"), body.value("arg_prefix", ""),
                                   d, body.value("reason", "manual"));
            sendJson(res, {{"status", "ok"}});
        } catch (const std::exception& e) {
            sendJson(res, {{"status", "error"}, {"message", e.what()}}, 400);
        }
    });

    svr.Post("/api/permission", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string id = body.value("id", "");
            std::shared_ptr<PendingPermission> pending;
            {
                std::lock_guard<std::mutex> lock(g_pending_mutex);
                auto it = g_pending.find(id);
                if (it == g_pending.end()) {
                    sendJson(res, {{"status", "error"}, {"message", "expired"}}, 404);
                    return;
                }
                pending = it->second;
            }
            std::string decision = body.value("decision", "deny_once");
            bool allow = decision == "allow_once" || decision == "always_allow";
            std::string prefix = permissionPrefix(pending->tool, pending->args);
            if (decision == "always_allow")
                g_permission->remember(pending->tool, prefix, Decision::Allow, "web");
            else if (decision == "always_deny")
                g_permission->remember(pending->tool, prefix, Decision::Deny, "web");
            {
                std::lock_guard<std::mutex> lock(pending->mutex);
                pending->answered = true;
                pending->allowed = allow;
            }
            pending->cv.notify_all();
            sendJson(res, {{"status", "ok"}, {"allowed", allow}});
        } catch (const std::exception& e) {
            sendJson(res, {{"status", "error"}, {"message", e.what()}}, 400);
        }
    });

    svr.Post("/api/cancel", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            std::string id = body.value("request_id", "");
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(g_active_mutex);
                auto it = g_active.find(id);
                if (it != g_active.end()) { it->second->store(true); found = true; }
            }
            sendJson(res, {{"status", found ? "ok" : "not_found"}});
        } catch (...) {
            sendJson(res, {{"status", "error"}}, 400);
        }
    });

    // ------------------------- 会话 -------------------------
    svr.Get("/api/sessions", [](const httplib::Request&, httplib::Response& res) {
        json arr = json::array();
        for (const auto& m : g_sessions->listSessions())
            arr.push_back({{"id", m.id}, {"title", m.title},
                           {"created_at", (long long)m.created_at},
                           {"updated_at", (long long)m.updated_at},
                           {"message_count", m.message_count},
                           {"tokens", m.total_tokens}});
        sendJson(res, arr);
    });

    svr.Post("/api/sessions", [](const httplib::Request& req, httplib::Response& res) {
        json body = req.body.empty() ? json::object() : json::parse(req.body);
        sendJson(res, {{"status", "ok"}, {"id", g_sessions->createSession(body.value("title", ""))}});
    });

    svr.Get(R"(/api/sessions/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string sid = httplib::detail::decode_url(req.matches[1], false);
        auto session = g_sessions->loadSession(sid);
        if (!session) { sendJson(res, {{"status", "error"}}, 404); return; }
        json msgs = json::array();
        for (const auto& m : session->messages) msgs.push_back(messageToApiJson(m));
        sendJson(res, {{"id", session->id}, {"title", session->title},
                       {"tokens", session->total_usage.total()},
                       {"metrics", g_sessions->loadMetrics(sid)},
                       {"messages", msgs}});
    });

    svr.Get(R"(/api/sessions/([^/]+)/checkpoints)", [](const httplib::Request& req,
                                                       httplib::Response& res) {
        json arr = json::array();
        std::string sid = httplib::detail::decode_url(req.matches[1], false);
        for (const auto& cp : g_sessions->listCheckpoints(sid))
            arr.push_back({{"id", cp.id}, {"label", cp.label},
                           {"created_at", (long long)cp.created_at}, {"files", cp.files}});
        sendJson(res, arr);
    });

    svr.Post(R"(/api/sessions/([^/]+)/restore)", [](const httplib::Request& req,
                                                     httplib::Response& res) {
        json body = json::parse(req.body);
        std::string error;
        std::string sid = httplib::detail::decode_url(req.matches[1], false);
        bool ok = g_sessions->restoreCheckpoint(sid,
                                                body.value("checkpoint_id", ""), error);
        sendJson(res, {{"status", ok ? "ok" : "error"}, {"message", error}},
                 ok ? 200 : 400);
    });

    // ------------------------- 记忆 / 索引 / 指标 -------------------------
    svr.Get("/api/memory", [](const httplib::Request&, httplib::Response& res) {
        sendJson(res, {{"facts", g_memory->facts()}});
    });
    svr.Post("/api/memory", [](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body);
        bool ok = g_memory->appendFact(body.value("fact", ""), "user");
        sendJson(res, {{"status", ok ? "ok" : "duplicate_or_empty"}});
    });

    svr.Post("/api/index", [](const httplib::Request&, httplib::Response& res) {
        g_repo->build(pathToUtf8(g_root));
        auto s = g_repo->getStats();
        sendJson(res, {{"status", "ok"}, {"files", s.total_files}, {"symbols", s.total_symbols}});
    });
    svr.Get("/api/repo-map", [](const httplib::Request&, httplib::Response& res) {
        sendJson(res, {{"map", g_repo->renderMap(12000)}});
    });

    svr.Get("/api/metrics", [](const httplib::Request& req, httplib::Response& res) {
        json m;
        if (req.has_param("session_id")) {
            m = g_sessions->loadMetrics(req.get_param_value("session_id"));
        } else {
            std::lock_guard<std::mutex> lock(g_metrics_mutex);
            m = g_metrics;
        }
        auto r = g_repo->getStats();
        m["sessions"] = (int)g_sessions->listSessions().size();
        m["memory_facts"] = (int)g_memory->facts().size();
        m["repo"] = {{"files", r.total_files}, {"symbols", r.total_symbols}};
        m["permission_rules"] = (int)g_permission->rules().size();
        sendJson(res, m);
    });
    svr.Get("/api/trace", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_metrics_mutex);
        sendJson(res, g_trace);
    });

    // ------------------------- SSE 执行 -------------------------
    svr.Post("/api/execute-stream", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_header("Access-Control-Allow-Origin", "*");
        auto body = std::make_shared<std::string>(req.body);

        res.set_chunked_content_provider(
            "text/event-stream",
            [body](size_t offset, httplib::DataSink& sink) -> bool {
                if (offset > 0) return false;
                std::string request_id = nowId() + "-" + std::to_string(g_seq.fetch_add(1));
                auto cancel = std::make_shared<std::atomic<bool>>(false);
                {
                    std::lock_guard<std::mutex> lock(g_active_mutex);
                    g_active[request_id] = cancel;
                }
                bool alive = true;
                auto send_event = [&](const std::string& event, const json& data) -> bool {
                    if (!alive) return false;
                    std::string msg = "event: " + event + "\ndata: " + data.dump() + "\n\n";
                    if (!sink.write(msg.c_str(), msg.size())) {
                        alive = false;
                        cancel->store(true);
                        return false;
                    }
                    return true;
                };

                std::string session_id;
                try {
                    json request_json = json::parse(*body);
                    session_id = request_json.value("session_id", "");
                    std::string mode = request_json.value("mode", "ask");
                    if (mode != "auto" && mode != "plan" && mode != "ask") mode = "ask";
                    std::vector<Message> messages = stripSystem(parseMessages(request_json));
                    if (messages.empty()) {
                        send_event("error", {{"message", "没有收到有效消息"}});
                        sink.done();
                        return true;
                    }
                    // 新对话：目录名 = 日期_时间_首条消息主题
                    if (session_id.empty()) {
                        session_id = g_sessions->createSession(
                            utf8Prefix(lastUser(messages), 40));
                    }
                    json cfg;
                    { std::lock_guard<std::mutex> lock(g_config_mutex); cfg = g_config; }
                    if (cfg["api"].value("key", "").empty() ||
                        cfg["api"].value("endpoint", "").empty()) {
                        send_event("error", {{"message", "API 未配置"}});
                        sink.done();
                        return true;
                    }

                    agentforge::APIConfig api_cfg = makeApiConfig();
                    SimpleHTTPTransport transport(api_cfg.api_endpoint, api_cfg.api_key);
                    auto provider = std::make_shared<agentforge::MultiAPIProvider>(transport, api_cfg);
                    auto tracer = std::make_shared<JsonlTracer>();

                    std::string workspace = workspaceForSession(session_id);

                    std::shared_ptr<ToolRegistry> tools;
                    if (mode == "plan") {
                        // Plan 模式：只读调研，绝不修改/执行
                        tools = g_tools->readOnlyView("Task");
                        tools->setWorkspaceRoot(workspace);
                    } else {
                        tools = std::make_shared<ToolRegistry>();
                        SandboxConfig wsc;
                        wsc.work_dir = workspace;
                        wsc.timeout_seconds = 60;
                        auto request_sandbox = createWindowsSandbox(wsc);
                        registerCoreTools(*tools, request_sandbox,
                                          std::shared_ptr<IUIPort>());
                        registerTaskTool(*tools, provider, tracer,
                                         cfg["agent"].value("max_subagent_depth", 2),
                                         nullptr, api_cfg.model);
                        tools->setWorkspaceRoot(workspace);
                    }

                    // Auto：默认允许，但仍受显式 deny 规则约束；Ask 沿用全局弹窗门
                    std::shared_ptr<IPermissionGate> request_gate = g_permission;
                    if (mode == "auto" &&
                        cfg["tools"].value("permission_mode", "ask") != "deny_all") {
                        auto auto_gate = std::make_shared<PersistentPermissionGate>(
                            g_permission->filePath());
                        auto_gate->setDefaultDecision(Decision::Allow);
                        request_gate = auto_gate;
                    }

                    auto budget = makeBudget();
                    auto context = std::make_shared<ContextManager>(budget);
                    std::string mode_prompt = systemPrompt();
                    if (mode == "plan") {
                        mode_prompt += "\n当前是 PLAN 模式：只允许读文件和搜索，"
                                       "请先调研并给出计划，不要修改文件或执行命令。";
                    } else if (mode == "auto") {
                        mode_prompt += "\n当前是 AUTO 模式：可以自主调用工具完成任务，"
                                       "但不要执行危险/破坏性命令。";
                    } else {
                        mode_prompt += "\n当前是 ASK 模式：每次修改/执行前会请求用户确认。";
                    }
                    mode_prompt += "\n当前工作区: " + workspace +
                                   "\nWrite/Edit 只能写该目录；相对路径相对于工作区。"
                                   "\nRead/Glob/Grep 仍可读取项目源码。";
                    context->setSystemPrompt(mode_prompt);
                    context->setProjectKnowledge(g_memory->loadProjectKnowledge() +
                                                 g_memory->loadMemory());
                    context->setRepoMap(g_repo->renderMap((std::size_t)budget.l2_repo_map * 3));
                    context->setMaxHistoryMessages(
                        cfg["agent"].value("max_history_messages", 40));
                    context->setUseLlmSummary(cfg["agent"].value("llm_summary", false));
                    if (cfg["retrieval"].value("enabled", true)) {
                        std::string q = lastUser(messages);
                        if (!q.empty())
                            context->setRetrieved(g_repo->search(
                                q, cfg["retrieval"].value("top_k", 5)));
                    }

                    auto ui = std::make_shared<SSEUIAdapter>(
                        send_event,
                        [&](const std::string& tool, const json& args) {
                            return askPermission(sink, tool, args, cancel);
                        });
                    ui->sendStart(request_id);
                    send_event("stage", {{"stage", "thinking"}, {"message", "正在思考..."}});

                    auto checkpoint = [session_id](const ToolCall& call) {
                        if (call.name != "Write" && call.name != "Edit") return;
                        std::string path = call.arguments.value("path", "");
                        if (!path.empty())
                            g_sessions->createCheckpoint(session_id, {resolveUserPath(path)},
                                                         "before " + call.name);
                        if (!path.empty())
                            g_sessions->recordArtifact(session_id, resolveUserPath(path),
                                                       call.name);
                    };

                    AgentLoop loop(provider, tools, ui);
                    loop.setPermissionGate(request_gate);
                    loop.setContextManager(context);
                    loop.setTracer(tracer);
                    loop.setCancelFlag(cancel);
                    loop.setToolBeforeHook(checkpoint);

                    LoopLimits limits;
                    limits.max_steps = cfg["agent"].value("max_steps", 20);
                    limits.max_tokens = cfg["agent"].value(
                        "max_total_tokens",
                        cfg["agent"].value("max_context_tokens", 100000));
                    limits.max_duration_sec = cfg["agent"].value("time_limit_seconds", 300);
                    limits.max_repeat_calls = cfg["agent"].value("max_repeat_calls", 3);
                    limits.max_consecutive_errors =
                        cfg["agent"].value("max_consecutive_errors", 3);
                    limits.enable_compaction = cfg["agent"].value("compaction", true);
                    limits.model = api_cfg.model;
                    limits.dump_dir = cfg["tools"].value("dump_dir", ".af-dump");
                    limits.persist_threshold =
                        (std::size_t)cfg["tools"].value("persist_threshold", 20000);

                    std::vector<Message> out;
                    LoopStats stats = loop.run(messages, limits, out);

                    Session session;
                    auto old = g_sessions->loadSession(session_id);
                    if (old) session = *old;
                    else {
                        session.id = session_id;
                        session.created_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        session.title = utf8Prefix(lastUser(messages), 60);
                    }
                    session.messages = out;
                    session.updated_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    session.total_usage += stats.total_usage;
                    g_sessions->saveSession(session);

                    if (cfg["agent"].value("auto_memory", true) &&
                        stats.outcome == LoopOutcome::Success) {
                        g_memory->autoRemember(lastUser(messages), lastAssistant(out));
                    }

                    std::string trace_file = g_sessions->tracePath(session_id);
                    tracer->saveJsonl(trace_file);

                    json m = tracer->summary();
                    m["usage"] = usageJson(stats.total_usage);
                    m["cost_usd"] = costUsd(stats.total_usage);
                    m["steps"] = stats.total_steps;
                    m["tool_calls"] = stats.tool_calls;
                    m["compactions"] = stats.compactions;
                    m["permission_denials"] = stats.permission_denials;
                    m["context_tokens"] = stats.context_tokens;
                    m["duration_ms"] = (long long)stats.duration_ms;
                    m["session_id"] = session_id;
                    m["trace_file"] = trace_file;
                    g_sessions->saveMetrics(session_id, m);
                    {
                        std::lock_guard<std::mutex> lock(g_metrics_mutex);
                        g_metrics = m;
                        g_trace = tracer->toJson();
                    }

                    if (stats.outcome == LoopOutcome::Success ||
                        stats.outcome == LoopOutcome::MaxSteps) {
                        send_event("done", {{"usage", usageJson(stats.total_usage)},
                                            {"model", api_cfg.model},
                                            {"steps", stats.total_steps},
                                            {"tool_calls", stats.tool_calls},
                                            {"compactions", stats.compactions},
                                            {"permission_denials", stats.permission_denials},
                                            {"context_tokens", stats.context_tokens},
                                            {"duration_ms", (long long)stats.duration_ms},
                                            {"cost_usd", costUsd(stats.total_usage)},
                                            {"session_id", session_id},
                                            {"trace_file", trace_file},
                                            {"mode", mode},
                                            {"outcome", stats.outcome == LoopOutcome::Success ?
                                                            "success" : "max_steps"}});
                    } else {
                        send_event("error", {{"message", stats.outcome_reason},
                                             {"session_id", session_id}});
                    }
                } catch (const std::exception& e) {
                    send_event("error", {{"message", std::string("服务器异常: ") + e.what()}});
                }
                {
                    std::lock_guard<std::mutex> lock(g_active_mutex);
                    g_active.erase(request_id);
                }
                sink.done();
                return true;
            });
    });

    // ------------------------- 非流式 -------------------------
    svr.Post("/api/execute", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            auto messages = stripSystem(parseMessages(body));
            if (messages.empty()) { sendJson(res, {{"status", "error"}}, 400); return; }
            agentforge::APIConfig api_cfg = makeApiConfig();
            if (api_cfg.api_key.empty()) { sendJson(res, {{"status", "error"}}, 400); return; }

            SimpleHTTPTransport transport(api_cfg.api_endpoint, api_cfg.api_key);
            auto provider = std::make_shared<agentforge::MultiAPIProvider>(transport, api_cfg);
            auto context = std::make_shared<ContextManager>(makeBudget());
            context->setSystemPrompt(systemPrompt());
            context->setProjectKnowledge(g_memory->loadProjectKnowledge() +
                                         g_memory->loadMemory());
            context->setRepoMap(g_repo->renderMap(12000));

            ChatRequest request;
            request.messages = context->build(messages);
            request.model = api_cfg.model;
            ChatResponse response = provider->chat(request);
            if (response.is_error) {
                sendJson(res, {{"status", "error"}, {"message", response.error_message}}, 502);
                return;
            }
            sendJson(res, {{"status", "success"}, {"result", response.message.content},
                           {"model", api_cfg.model}, {"usage", usageJson(response.usage)}});
        } catch (const std::exception& e) {
            sendJson(res, {{"status", "error"}, {"message", e.what()}}, 500);
        }
    });

    logLine("[Server] http://" + host + ":" + std::to_string(port));
    if (!svr.listen(host.c_str(), port)) {
        std::cerr << "[Error] listen failed\n";
        return 1;
    }
    return 0;
}
