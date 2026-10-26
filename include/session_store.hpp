#pragma once

#include "types.hpp"
#include "win_utf8.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ============================================================================
// 会话持久化 (L10)
//
// - 每个会话一个目录：sessions/<id>/
//     meta.json        元信息
//     events.jsonl     追加事件流（message / tool / compaction / checkpoint）
//     snapshot.json    最近一次完整快照（--resume 用）
//     checkpoints/     工具执行前的文件快照
// - 三层记忆：
//     会话历史（本文件） / 项目知识（AGENTS.md/CLAUDE.md） / 自动记忆（MEMORY.md）
// ============================================================================

struct Session {
    std::string id;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    std::string title;
    std::vector<Message> messages;
    Usage total_usage;
    std::map<std::string, std::string> metadata;
};

struct SessionMeta {
    std::string id;
    std::string title;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int message_count = 0;
    int total_tokens = 0;
};

struct CheckpointInfo {
    std::string id;
    std::string label;
    int64_t created_at = 0;
    std::vector<std::string> files;
};

class SessionStore {
public:
    explicit SessionStore(std::string storage_dir)
        : storage_dir_(std::move(storage_dir)) {
        std::error_code ec;
        std::filesystem::create_directories(utf8ToPath(storage_dir_), ec);
    }

    const std::string& storageDir() const { return storage_dir_; }

    // 每个会话的独立目录：conversations/<session_id>/
    std::string conversationDir(const std::string& id) const { return sessionDir(id); }
    std::string artifactsDir(const std::string& id) const {
        return sessionDir(id) + "/artifacts";
    }
    std::string tracePath(const std::string& id) const {
        return sessionDir(id) + "/trace.jsonl";
    }

    // 记录本次对话实际写入/修改过的文件路径（真实文件仍在原位置）
    bool recordArtifact(const std::string& session_id, const std::string& path,
                        const std::string& tool) {
        try {
            ensureSessionDir(session_id);
            std::ofstream out(utf8ToPath(artifactsDir(session_id) +
                                         "/manifest.jsonl"),
                              std::ios::app | std::ios::binary);
            if (!out) return false;
            out << json{{"path", path}, {"tool", tool},
                        {"at", getCurrentTimeMs()}}.dump()
                << "\n";
            return true;
        } catch (...) {
            return false;
        }
    }

    std::string createSession(const std::string& title = "") {
        Session session;
        session.id = generateSessionId(title);
        session.created_at = getCurrentTimeMs();
        session.updated_at = session.created_at;
        session.title = title;
        saveSession(session);
        return session.id;
    }

    // 完整快照 + 追加事件
    bool saveSession(const Session& session) {
        try {
            ensureSessionDir(session.id);

            json meta;
            meta["id"] = session.id;
            meta["title"] = session.title;
            meta["created_at"] = session.created_at;
            meta["updated_at"] = session.updated_at;
            meta["message_count"] = (int)session.messages.size();
            meta["total_tokens"] = session.total_usage.total();
            meta["metadata"] = session.metadata;
            writeJson(metaPath(session.id), meta);

            json snapshot;
            snapshot["id"] = session.id;
            snapshot["title"] = session.title;
            snapshot["created_at"] = session.created_at;
            snapshot["updated_at"] = session.updated_at;
            snapshot["total_usage"] = usageToJson(session.total_usage);
            snapshot["metadata"] = session.metadata;
            json messages = json::array();
            for (const auto& msg : session.messages) {
                messages.push_back(messageToJson(msg));
            }
            snapshot["messages"] = messages;
            writeJson(snapshotPath(session.id), snapshot);

            // 追加一条 snapshot 事件（可审计/可回放）
            json ev;
            ev["t"] = "snapshot";
            ev["ts"] = getCurrentTimeMs();
            ev["message_count"] = (int)session.messages.size();
            ev["total_tokens"] = session.total_usage.total();
            appendEventLine(session.id, ev);
            writeReadme(session);

            return true;
        } catch (...) {
            return false;
        }
    }

    std::optional<Session> loadSession(const std::string& session_id) {
        try {
            auto path = snapshotPath(session_id);
            if (!std::filesystem::exists(utf8ToPath(path))) {
                // 没有快照则从 events.jsonl 回放
                return replaySession(session_id);
            }
            std::ifstream in(utf8ToPath(path), std::ios::binary);
            json j;
            in >> j;

            Session session;
            session.id = j.value("id", session_id);
            session.title = j.value("title", "");
            session.created_at = j.value("created_at", (int64_t)0);
            session.updated_at = j.value("updated_at", (int64_t)0);
            if (j.contains("total_usage")) session.total_usage = jsonToUsage(j["total_usage"]);
            if (j.contains("metadata") && j["metadata"].is_object()) {
                session.metadata = j["metadata"].get<std::map<std::string, std::string>>();
            }
            if (j.contains("messages") && j["messages"].is_array()) {
                for (const auto& item : j["messages"]) {
                    session.messages.push_back(jsonToMessage(item));
                }
            }
            return session;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::vector<SessionMeta> listSessions() const {
        std::vector<SessionMeta> result;
        namespace fs = std::filesystem;
        try {
            for (const auto& entry : fs::directory_iterator(utf8ToPath(storage_dir_))) {
                if (!entry.is_directory()) continue;
                std::string id = pathToUtf8(entry.path().filename());
                auto meta = loadMeta(id);
                if (meta) result.push_back(*meta);
            }
        } catch (...) {
        }
        std::sort(result.begin(), result.end(),
                  [](const SessionMeta& a, const SessionMeta& b) {
                      return a.updated_at > b.updated_at;
                  });
        return result;
    }

    bool deleteSession(const std::string& session_id) {
        try {
            return std::filesystem::remove_all(utf8ToPath(sessionDir(session_id))) > 0;
        } catch (...) {
            return false;
        }
    }

    bool appendMessage(const std::string& session_id, const Message& message) {
        auto session_opt = loadSession(session_id);
        if (!session_opt) return false;
        Session session = *session_opt;
        session.messages.push_back(message);
        session.updated_at = getCurrentTimeMs();
        if (!saveSession(session)) return false;
        return true;
    }

    bool appendEvent(const std::string& session_id, const json& event) {
        try {
            json ev = event;
            if (!ev.contains("ts")) ev["ts"] = getCurrentTimeMs();
            appendEventLine(session_id, ev);
            return true;
        } catch (...) {
            return false;
        }
    }

    // ---- 检查点（工具执行前文件快照） ----
    std::string createCheckpoint(const std::string& session_id,
                                 const std::vector<std::string>& files,
                                 const std::string& label = "") {
        try {
            std::string cp_id = std::to_string(getCurrentTimeMs());
            ensureSessionDir(session_id);
            auto cp_dir = checkpointsDir(session_id) + "/" + cp_id;
            std::filesystem::create_directories(utf8ToPath(cp_dir));

            json manifest;
            manifest["id"] = cp_id;
            manifest["label"] = label;
            manifest["created_at"] = getCurrentTimeMs();
            manifest["files"] = json::array();

            int i = 0;
            for (const auto& file : files) {
                if (file.empty()) continue;
                std::string abs = file;
                std::filesystem::path p = utf8ToPath(file);
                if (!p.is_absolute()) p = std::filesystem::absolute(p);
                if (!std::filesystem::exists(p)) continue;

                std::string stored = "f" + std::to_string(i++) + "_" +
                                     pathToUtf8(p.filename());
                std::filesystem::copy_file(
                    p, utf8ToPath(cp_dir + "/" + stored),
                    std::filesystem::copy_options::overwrite_existing);

                manifest["files"].push_back({
                    {"original", pathToUtf8(p)},
                    {"stored", stored}
                });
            }

            if (manifest["files"].empty()) {
                std::filesystem::remove_all(utf8ToPath(cp_dir));
                return "";
            }

            writeJson(cp_dir + "/manifest.json", manifest);

            json ev;
            ev["t"] = "checkpoint";
            ev["ts"] = getCurrentTimeMs();
            ev["checkpoint_id"] = cp_id;
            ev["files"] = manifest["files"];
            appendEventLine(session_id, ev);
            return cp_id;
        } catch (...) {
            return "";
        }
    }

    std::vector<CheckpointInfo> listCheckpoints(const std::string& session_id) const {
        std::vector<CheckpointInfo> result;
        namespace fs = std::filesystem;
        try {
            auto dir = utf8ToPath(checkpointsDir(session_id));
            if (!fs::exists(dir)) return result;
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (!entry.is_directory()) continue;
                auto man = entry.path() / "manifest.json";
                if (!fs::exists(man)) continue;
                std::ifstream in(man);
                json j;
                in >> j;
                CheckpointInfo info;
                info.id = j.value("id", "");
                info.label = j.value("label", "");
                info.created_at = j.value("created_at", (int64_t)0);
                if (j.contains("files") && j["files"].is_array()) {
                    for (const auto& f : j["files"]) {
                        info.files.push_back(f.value("original", ""));
                    }
                }
                result.push_back(std::move(info));
            }
        } catch (...) {
        }
        std::sort(result.begin(), result.end(),
                  [](const CheckpointInfo& a, const CheckpointInfo& b) {
                      return a.created_at > b.created_at;
                  });
        return result;
    }

    bool restoreCheckpoint(const std::string& session_id,
                           const std::string& checkpoint_id,
                           std::string& error) {
        namespace fs = std::filesystem;
        try {
            auto dir = utf8ToPath(checkpointsDir(session_id) + "/" + checkpoint_id);
            auto man = dir / "manifest.json";
            if (!fs::exists(man)) {
                error = "检查点不存在";
                return false;
            }
            std::ifstream in(man);
            json j;
            in >> j;
            for (const auto& f : j["files"]) {
                fs::path original = utf8ToPath(f.value("original", ""));
                fs::path stored = dir / utf8ToPath(f.value("stored", ""));
                if (!fs::exists(stored)) {
                    error = "备份文件缺失: " + f.value("stored", "");
                    return false;
                }
                fs::create_directories(original.parent_path());
                fs::copy_file(stored, original, fs::copy_options::overwrite_existing);
            }
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

private:
    std::string storage_dir_;

    std::string sessionDir(const std::string& id) const {
        return storage_dir_ + "/" + id;
    }
    std::string metaPath(const std::string& id) const {
        return sessionDir(id) + "/meta.json";
    }
    std::string snapshotPath(const std::string& id) const {
        return sessionDir(id) + "/snapshot.json";
    }
    std::string eventsPath(const std::string& id) const {
        return sessionDir(id) + "/events.jsonl";
    }
    std::string checkpointsDir(const std::string& id) const {
        return sessionDir(id) + "/checkpoints";
    }

    void ensureSessionDir(const std::string& id) {
        std::error_code ec;
        std::filesystem::create_directories(utf8ToPath(sessionDir(id)), ec);
        std::filesystem::create_directories(utf8ToPath(artifactsDir(id)), ec);
        std::string art_readme = artifactsDir(id) + "/README.md";
        if (!std::filesystem::exists(utf8ToPath(art_readme), ec)) {
            std::ofstream out(utf8ToPath(art_readme), std::ios::binary);
            if (out) {
                out << "# 本次对话的产物索引\n\n"
                    << "`manifest.jsonl` 记录本次对话 Write/Edit 过的文件路径与工具。\n"
                    << "实际文件仍保留在原项目路径，不在此处重复复制，避免副本过期。\n";
            }
        }
    }

    void writeReadme(const Session& session) {
        try {
            std::ofstream out(utf8ToPath(sessionDir(session.id) + "/README.md"),
                              std::ios::binary);
            if (!out) return;
            out << "# 对话 " << session.id << "\n\n";
            out << "- 标题: " << (session.title.empty() ? "(未命名)" : session.title) << "\n";
            out << "- 创建时间(ms): " << session.created_at << "\n";
            out << "- 更新时间(ms): " << session.updated_at << "\n";
            out << "- 消息数: " << session.messages.size() << "\n";
            out << "- Token: " << session.total_usage.total() << "\n\n";
            out << "## 文件\n\n";
            out << "- `meta.json` 元信息\n";
            out << "- `events.jsonl` 追加事件流\n";
            out << "- `snapshot.json` 完整消息快照\n";
            out << "- `checkpoints/` Write/Edit 前文件备份\n";
            out << "- `trace.jsonl` 本次对话 Trace（L12）\n";
            out << "- `artifacts/manifest.jsonl` 本次对话改动的文件路径\n";
        } catch (...) {
        }
    }

    std::optional<SessionMeta> loadMeta(const std::string& id) const {
        try {
            std::ifstream in(utf8ToPath(metaPath(id)), std::ios::binary);
            if (!in) return std::nullopt;
            json j;
            in >> j;
            SessionMeta meta;
            meta.id = j.value("id", id);
            meta.title = j.value("title", "");
            meta.created_at = j.value("created_at", (int64_t)0);
            meta.updated_at = j.value("updated_at", (int64_t)0);
            meta.message_count = j.value("message_count", 0);
            meta.total_tokens = j.value("total_tokens", 0);
            return meta;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<Session> replaySession(const std::string& id) {
        try {
            std::ifstream in(utf8ToPath(eventsPath(id)), std::ios::binary);
            if (!in) return std::nullopt;
            Session session;
            session.id = id;
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                try {
                    json ev = json::parse(line);
                    if (ev.value("t", "") == "message" && ev.contains("message")) {
                        session.messages.push_back(jsonToMessage(ev["message"]));
                    }
                } catch (...) {
                }
            }
            return session;
        } catch (...) {
            return std::nullopt;
        }
    }

    void appendEventLine(const std::string& id, const json& event) {
        ensureSessionDir(id);
        std::ofstream out(utf8ToPath(eventsPath(id)), std::ios::app | std::ios::binary);
        if (out) out << event.dump() << "\n";
    }

    void writeJson(const std::string& path, const json& j) {
        std::filesystem::path p = utf8ToPath(path);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        if (!out) throw std::runtime_error("write failed");
        out << j.dump(2);
    }

    static std::string sanitizeTopic(std::string topic) {
        topic = sanitizeUtf8(topic);
        std::string out;
        bool last_sep = false;
        for (unsigned char c : topic) {
            if (c < 0x20 || c == 0x7f) continue;
            const bool bad = c == '<' || c == '>' || c == ':' || c == '"' ||
                             c == '/' || c == '\\' || c == '|' || c == '?' ||
                             c == '*' || c == ' ';
            if (bad) {
                if (!last_sep) { out.push_back('_'); last_sep = true; }
            } else {
                out.push_back((char)c);
                last_sep = false;
            }
        }
        while (!out.empty() && (out.back() == '_' || out.back() == '.')) out.pop_back();
        while (!out.empty() && (out.front() == '_' || out.front() == '.')) out.erase(out.begin());
        if (out.empty()) out = "chat";
        return utf8Prefix(out, 32);
    }

    std::string generateSessionId(const std::string& title) {
        std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char stamp[32] = {0};
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);

        const std::string base = std::string(stamp) + "_" + sanitizeTopic(title);
        std::string id = base;
        int n = 2;
        while (std::filesystem::exists(utf8ToPath(sessionDir(id)))) {
            id = base + "_" + std::to_string(n++);
        }
        return id;
    }

    static int64_t getCurrentTimeMs() {
        auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()).count();
    }

    static json messageToJson(const Message& msg) {
        json j;
        if (msg.call_id.has_value()) j["role"] = "tool";
        else if (msg.role == Role::User) j["role"] = "user";
        else if (msg.role == Role::System) j["role"] = "system";
        else j["role"] = "assistant";
        j["content"] = sanitizeUtf8(msg.content);
        if (!msg.tool_calls.empty()) {
            json calls = json::array();
            for (const auto& call : msg.tool_calls) {
                calls.push_back({
                    {"id", call.id},
                    {"name", call.name},
                    {"arguments", call.arguments}
                });
            }
            j["tool_calls"] = calls;
        }
        if (msg.call_id) j["call_id"] = *msg.call_id;
        return j;
    }

    static Message jsonToMessage(const json& j) {
        Message msg;
        if (j.contains("role") && j["role"].is_number_integer()) {
            msg.role = static_cast<Role>(j["role"].get<int>());
        } else {
            std::string role = j.value("role", "user");
            if (role == "system") msg.role = Role::System;
            else if (role == "assistant" || role == "tool") msg.role = Role::Assistant;
            else msg.role = Role::User;
        }
        msg.content = sanitizeUtf8(j.value("content", ""));
        if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
            for (const auto& item : j["tool_calls"]) {
                ToolCall call;
                call.id = item.value("id", "");
                call.name = item.value("name", "");
                call.arguments = item.value("arguments", json::object());
                msg.tool_calls.push_back(std::move(call));
            }
        }
        if (j.contains("call_id") && j["call_id"].is_string()) {
            msg.call_id = j["call_id"].get<std::string>();
        }
        return msg;
    }

    static json usageToJson(const Usage& usage) {
        return json{
            {"prompt_tokens", usage.prompt_tokens},
            {"completion_tokens", usage.completion_tokens},
            {"cache_read_tokens", usage.cache_read_tokens},
            {"cache_write_tokens", usage.cache_write_tokens}
        };
    }

    static Usage jsonToUsage(const json& j) {
        Usage usage;
        usage.prompt_tokens = j.value("prompt_tokens", 0);
        usage.completion_tokens = j.value("completion_tokens", 0);
        usage.cache_read_tokens = j.value("cache_read_tokens", 0);
        usage.cache_write_tokens = j.value("cache_write_tokens", 0);
        return usage;
    }
};

// ============================================================================
// 三层记忆：项目知识 + 自动记忆
// ============================================================================
class MemoryStore {
public:
    explicit MemoryStore(std::string root = ".") : root_(std::move(root)) {}

    // L1 项目知识：AGENTS.md / CLAUDE.md
    std::string loadProjectKnowledge() const {
        std::string result;
        for (const char* name : {"AGENTS.md", "CLAUDE.md"}) {
            std::string text = readFile(root_ + "/" + name);
            if (!text.empty()) {
                result += "\n## " + std::string(name) + "\n" + text + "\n";
            }
        }
        return result;
    }

    // 自动记忆：MEMORY.md
    std::string loadMemory() const {
        std::string text = readFile(root_ + "/MEMORY.md");
        if (text.empty()) return "";
        return "\n## 长期记忆 (MEMORY.md)\n" + text + "\n";
    }

    std::vector<std::string> facts() const {
        std::string text = readFile(root_ + "/MEMORY.md");
        std::vector<std::string> result;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("- ", 0) == 0) {
                std::string fact = line.substr(2);
                auto source_pos = fact.find("  _(source:");
                if (source_pos != std::string::npos) fact.resize(source_pos);
                result.push_back(fact);
            }
        }
        return result;
    }

    bool appendFact(const std::string& fact, const std::string& source = "") {
        if (fact.empty()) return false;
        std::string normalized = normalize(fact);
        if (normalized.empty()) return false;

        for (const auto& existing : facts()) {
            if (normalize(existing) == normalized) return false;  // 去重
        }

        std::string path = root_ + "/MEMORY.md";
        bool exists = std::filesystem::exists(utf8ToPath(path));
        std::ofstream out(utf8ToPath(path), std::ios::app | std::ios::binary);
        if (!out) return false;
        if (!exists) out << "# AgentForge Memory\n\n";
        out << "- " << fact;
        if (!source.empty()) out << "  _(source: " << source << ")_";
        out << "\n";
        return true;
    }

    // 任务结束后的抽取式自动记忆（不调用模型，零额外成本）
    bool autoRemember(const std::string& task, const std::string& result) {
        std::string fact = "任务: " + utf8Prefix(task, 120);
        if (!result.empty()) fact += " → " + utf8Prefix(result, 160);
        return appendFact(fact, "auto");
    }

private:
    std::string root_;

    static std::string readFile(const std::string& path) {
        std::ifstream in(utf8ToPath(path), std::ios::binary);
        if (!in) return "";
        std::stringstream ss;
        ss << in.rdbuf();
        return sanitizeUtf8(ss.str());
    }

    static std::string normalize(const std::string& s) {
        std::string out;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c >= 0x80) {
                out += (char)std::tolower(c);
            }
        }
        return out;
    }
};
