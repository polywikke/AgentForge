#include "../include/tool_registry.hpp"
#include "../include/types.hpp"
#include "../include/win_utf8.hpp"
#include <cerrno>
#include <fstream>
#include <filesystem>
#include <memory>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

// ============================================================================
// 基础工具实现 (ADR-2: 工具是原语)
// ============================================================================

// Read 工具: 读取文件
ToolResult toolRead(const json& args) {
    try {
        std::string filepath = resolveUserPath(args.at("path").get<std::string>());
        fs::path path = utf8ToPath(filepath);

        // 检查文件是否存在
        if (!fs::exists(path)) {
            return ToolResult::error("", "文件不存在: " + filepath);
        }

        // 读取文件内容
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return ToolResult::error("", "无法打开文件: " + filepath);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        // 文本卫生处理 (C-4: 编码不可信)
        content = sanitizeUtf8(content);

        // 安全：对常见的密钥字段做脱敏，避免通过 Read 泄露 API Key
        try {
            static const std::regex secret_field(
                R"RE(("(?:api[_-]?key|apikey|key|token|secret|authorization)"\s*:\s*")[^"]*("))RE",
                std::regex::icase);
            content = std::regex_replace(content, secret_field, "$1***$2");
            static const std::regex sk_like(R"RE(sk-[A-Za-z0-9_\-]{12,})RE");
            content = std::regex_replace(content, sk_like, "sk-***");
        } catch (...) {
        }

        // 限制输出长度
        if (content.size() > 10000) {
            content = utf8Prefix(content, 10000) + "\n\n... (内容已截断)";
        }

        ToolResult result;
        result.content = "文件内容:\n" + content;
        return result;

    } catch (const json::exception& e) {
        return ToolResult::error("", std::string("参数解析错误: ") + e.what());
    } catch (const std::exception& e) {
        return ToolResult::error("", std::string("读取失败: ") + e.what());
    }
}

// Write 工具: 写入文件
ToolResult toolWrite(const json& args) {
    try {
        std::string filepath = resolveUserPath(args.at("path").get<std::string>());
        std::string content = args.at("content").get<std::string>();

        // 创建目录 (如果不存在)
        fs::path path = utf8ToPath(filepath);
        std::error_code ec;
        if (path.has_parent_path()) {
            fs::create_directories(path.parent_path(), ec);
        }

        // 写入文件
        std::ofstream file(path, std::ios::binary);
        if (!file) {
            std::string absolute = filepath;
            try {
                absolute = pathToUtf8(fs::absolute(path));
            } catch (...) {
            }
            std::string detail = "无法创建文件: " + absolute;
            detail += "\n可能原因: 目录不存在、路径非法，或当前进程没有写权限。";
#ifdef _WIN32
            detail += "\n[errno=" + std::to_string(errno) +
                      ", win32=" + std::to_string((unsigned long)GetLastError()) + "]";
#endif
            detail += "\n提示: 用户目录可用 %USERPROFILE% 表示，"
                      "例如 %USERPROFILE%\\Desktop\\sort.py；"
                      "请勿擅自改用相对路径，应向用户报告失败位置。";
            return ToolResult::error("", detail);
        }

        file << content;
        file.close();

        if (!file) {
            return ToolResult::error("", "写入过程中出错: " + filepath);
        }

        ToolResult result;
        result.content = "文件已写入: " + pathToUtf8(fs::absolute(path)) +
                        " (" + std::to_string(content.size()) + " 字节)";
        return result;

    } catch (const json::exception& e) {
        return ToolResult::error("", std::string("参数解析错误: ") + e.what());
    } catch (const std::exception& e) {
        return ToolResult::error("", std::string("写入失败: ") + e.what());
    }
}

// Edit 工具: 精确替换
ToolResult toolEdit(const json& args) {
    try {
        std::string filepath = resolveUserPath(args.at("path").get<std::string>());
        std::string old_text = args.at("old_string").get<std::string>();
        std::string new_text = args.at("new_string").get<std::string>();
        fs::path path = utf8ToPath(filepath);

        // 读取文件
        if (!fs::exists(path)) {
            return ToolResult::error("", "文件不存在: " + filepath);
        }

        std::ifstream file(path, std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();
        file.close();

        // 查找并替换
        size_t pos = content.find(old_text);
        if (pos == std::string::npos) {
            return ToolResult::error("", "未找到要替换的文本");
        }

        // 检查唯一性
        size_t pos2 = content.find(old_text, pos + 1);
        if (pos2 != std::string::npos) {
            return ToolResult::error("", "要替换的文本不唯一,无法确定替换位置");
        }

        // 执行替换
        content.replace(pos, old_text.size(), new_text);

        // 写回文件
        std::ofstream outfile(path, std::ios::binary);
        outfile << content;
        outfile.close();

        ToolResult result;
        result.content = "文件已修改: " + filepath;
        return result;

    } catch (const json::exception& e) {
        return ToolResult::error("", std::string("参数解析错误: ") + e.what());
    } catch (const std::exception& e) {
        return ToolResult::error("", std::string("编辑失败: ") + e.what());
    }
}

// Glob 工具: 文件搜索
ToolResult toolGlob(const json& args) {
    try {
        std::string pattern = args.at("pattern").get<std::string>();
        std::string base_path = resolveUserPath(args.value("path", "."));

        std::vector<std::string> matches;

        // 简单实现: 递归搜索文件
        fs::path base = utf8ToPath(base_path);
        for (const auto& entry : fs::recursive_directory_iterator(base)) {
            if (entry.is_regular_file()) {
                std::string path = pathToUtf8(entry.path());
                // 简单的模式匹配 (应该使用更完善的 glob 匹配)
                if (path.find(pattern) != std::string::npos) {
                    matches.push_back(path);
                }
            }
        }

        std::ostringstream oss;
        oss << "找到 " << matches.size() << " 个匹配文件:\n";
        for (size_t i = 0; i < matches.size() && i < 50; ++i) {
            oss << "  " << matches[i] << "\n";
        }
        if (matches.size() > 50) {
            oss << "  ... (还有 " << (matches.size() - 50) << " 个)";
        }

        ToolResult result;
        result.content = oss.str();
        return result;

    } catch (const json::exception& e) {
        return ToolResult::error("", std::string("参数解析错误: ") + e.what());
    } catch (const std::exception& e) {
        return ToolResult::error("", std::string("搜索失败: ") + e.what());
    }
}

// Grep 工具: 内容搜索
ToolResult toolGrep(const json& args) {
    try {
        std::string pattern = args.at("pattern").get<std::string>();
        std::string path = resolveUserPath(args.value("path", "."));

        std::vector<std::string> matches;

        // 简单实现: 搜索包含关键词的行
        fs::path base = utf8ToPath(path);
        for (const auto& entry : fs::recursive_directory_iterator(base)) {
            if (entry.is_regular_file() && entry.path().extension() != ".exe") {
                std::ifstream file(entry.path());
                std::string line;
                int line_no = 1;

                while (std::getline(file, line)) {
                    if (line.find(pattern) != std::string::npos) {
                        matches.push_back(pathToUtf8(entry.path()) + ":" +
                                        std::to_string(line_no) + ": " + line);
                        if (matches.size() >= 100) break; // 限制结果数量
                    }
                    line_no++;
                }
            }
        }

        std::ostringstream oss;
        oss << "找到 " << matches.size() << " 个匹配:\n";
        for (const auto& match : matches) {
            oss << "  " << match << "\n";
        }

        ToolResult result;
        result.content = oss.str();
        return result;

    } catch (const json::exception& e) {
        return ToolResult::error("", std::string("参数解析错误: ") + e.what());
    } catch (const std::exception& e) {
        return ToolResult::error("", std::string("搜索失败: ") + e.what());
    }
}

// Bash 工具: 命令执行 (简化版)
ToolResult toolBash(const json& args) {
    try {
        std::string command = args.at("command").get<std::string>();

        // 警告: 这里应该使用 ISandbox 执行
        // 当前仅作演示
        ToolResult result;
        result.content = "[演示模式] 将执行命令: " + command + "\n";
        result.content += "(实际实现应通过沙箱执行)";
        return result;

    } catch (const json::exception& e) {
        return ToolResult::error("", std::string("参数解析错误: ") + e.what());
    }
}

// ============================================================================
// 注册所有工具
// ============================================================================

// V2 版本：接受 sandbox 和 ui 参数
void registerCoreTools(ToolRegistry& registry,
                      std::shared_ptr<ISandbox> sandbox,
                      std::shared_ptr<IUIPort> ui) {
    // Read 工具
    {
        ToolDef def;
        def.name = "Read";
        def.description = "读取文件内容";
        def.risk_level = RiskLevel::Read;
        def.parameters = {
            {"type", "object"},
            {"properties", {
                {"path", {
                    {"type", "string"},
                    {"description", "文件路径"}
                }}
            }},
            {"required", json::array({"path"})}
        };
        registry.registerTool(def, toolRead);
    }

    // Write 工具
    {
        ToolDef def;
        def.name = "Write";
        def.description = "写入文件";
        def.risk_level = RiskLevel::Write;
        def.parameters = {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "文件路径"}}},
                {"content", {{"type", "string"}, {"description", "文件内容"}}}
            }},
            {"required", json::array({"path", "content"})}
        };
        registry.registerTool(def, toolWrite);
    }

    // Edit 工具
    {
        ToolDef def;
        def.name = "Edit";
        def.description = "精确替换文件中的文本";
        def.risk_level = RiskLevel::Write;
        def.parameters = {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "文件路径"}}},
                {"old_string", {{"type", "string"}, {"description", "要替换的文本"}}},
                {"new_string", {{"type", "string"}, {"description", "新文本"}}}
            }},
            {"required", json::array({"path", "old_string", "new_string"})}
        };
        registry.registerTool(def, toolEdit);
    }

    // Glob 工具
    {
        ToolDef def;
        def.name = "Glob";
        def.description = "按模式搜索文件";
        def.risk_level = RiskLevel::Read;
        def.parameters = {
            {"type", "object"},
            {"properties", {
                {"pattern", {{"type", "string"}, {"description", "搜索模式"}}},
                {"path", {{"type", "string"}, {"description", "搜索路径"}}}
            }},
            {"required", json::array({"pattern"})}
        };
        registry.registerTool(def, toolGlob);
    }

    // Grep 工具
    {
        ToolDef def;
        def.name = "Grep";
        def.description = "在文件中搜索内容";
        def.risk_level = RiskLevel::Read;
        def.parameters = {
            {"type", "object"},
            {"properties", {
                {"pattern", {{"type", "string"}, {"description", "搜索模式"}}},
                {"path", {{"type", "string"}, {"description", "搜索路径"}}}
            }},
            {"required", json::array({"pattern"})}
        };
        registry.registerTool(def, toolGrep);
    }

    // Bash 工具
    {
        ToolDef def;
        def.name = "Bash";
        def.description = "执行 shell 命令 (受沙箱约束)";
        def.risk_level = RiskLevel::Exec;
        def.parameters = {
            {"type", "object"},
            {"properties", {
                {"command", {{"type", "string"}, {"description", "要执行的命令"}}}
            }},
            {"required", json::array({"command"})}
        };

        auto sandbox_ptr = sandbox;
        registry.registerTool(def, [sandbox_ptr](const json& args) -> ToolResult {
            try {
                std::string command = args.at("command").get<std::string>();

                if (!sandbox_ptr) {
                    ToolResult result;
                    result.content = "[演示模式] 将执行命令: " + command +
                                     "\n(未配置沙箱, 实际未执行)";
                    return result;
                }

                SandboxConfig cfg;  // 使用沙箱自身的默认配置
                SandboxResult r = sandbox_ptr->execute(command, {}, cfg);

                ToolResult result;
                std::string output = r.output;
                if (output.empty() && !r.error.empty()) output = r.error;
                if (output.size() > 8000) {
                    output = utf8Prefix(output, 8000) + "\n... (输出已截断)";
                }
                result.content = "[exit=" + std::to_string(r.exit_code) + "]\n" + output;
                result.is_error = !r.success;
                result.blocked_by_sandbox = r.blocked;
                result.block_reason = r.block_reason;
                return result;
            } catch (const json::exception& e) {
                return ToolResult::error("", std::string("参数解析错误: ") + e.what());
            } catch (const std::exception& e) {
                return ToolResult::error("", std::string("执行失败: ") + e.what());
            }
        });
    }
}

// V1 版本：向后兼容，不需要 sandbox 和 ui 参数
void registerCoreTools(ToolRegistry& registry) {
    registerCoreTools(registry,
                      std::shared_ptr<ISandbox>(),
                      std::shared_ptr<IUIPort>());
}
