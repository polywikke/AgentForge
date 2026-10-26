// ============================================================================
// Windows 沙箱实现 (L6)
//
// 提供:
//   - 命令执行 (WorkDir 限制 + 超时 + 危险命令拦截)
//   - 路径白名单校验
//   - 自检状态报告
// ============================================================================
#include "../include/sandbox.hpp"
#include "../include/win_utf8.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// 控制台程序 (cmd/dir 等) 的输出通常是 ANSI 代码页, 统一转成 UTF-8
std::string ansiToUtf8(const std::string& text) {
    if (text.empty() || isValidUtf8(text)) return sanitizeUtf8(text);

    int needed = MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring wide(needed, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), &wide[0], needed);
    return wideToUtf8(wide);
}

bool containsDangerousCommand(const std::string& command) {
    static const std::vector<std::string> kBlocked = {
        "format ",   "format.com", "diskpart",  "bcdedit",
        "shutdown",  "reboot",     "cipher /w", "takeown /f c:\\",
        "rd /s /q c:\\", "del /s /q c:\\", "rmdir /s /q c:\\"
    };

    std::string lower = command;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });

    for (const auto& danger : kBlocked) {
        if (lower.find(danger) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// ============================================================================
// WindowsSandbox
// ============================================================================
class WindowsSandbox : public ISandbox {
public:
    explicit WindowsSandbox(const SandboxConfig& config)
        : config_(config) {
        if (config_.work_dir.empty()) {
            config_.work_dir = pathToUtf8(fs::current_path());
        }
        std::error_code ec;
        fs::create_directories(utf8ToPath(config_.work_dir), ec);
    }

    SandboxResult execute(const std::string& command,
                          const std::vector<std::string>& args,
                          const SandboxConfig& cfg) override {
        SandboxResult result;

        const SandboxConfig& effective = cfg.work_dir.empty() ? config_ : cfg;

        if (command.empty()) {
            result.error = "命令为空";
            return result;
        }

        std::string original = command;
        if (!args.empty()) {
            for (const auto& arg : args) {
                original += " " + arg;
            }
        }

        if (containsDangerousCommand(original)) {
            result.blocked = true;
            result.block_reason = "命令命中危险操作黑名单: " + original;
            result.error = result.block_reason;
            result.exit_code = -1;
            return result;
        }

        std::string work_dir = effective.work_dir.empty()
                                   ? pathToUtf8(fs::current_path())
                                   : effective.work_dir;
        if (!isPathAllowed(work_dir, effective)) {
            result.blocked = true;
            result.block_reason = "工作目录不在允许范围内: " + work_dir;
            result.error = result.block_reason;
            result.exit_code = -1;
            return result;
        }

        // 通过 cmd.exe /S /C 执行, 支持重定向与管道
        std::wstring cmdline = L"cmd.exe /S /C \"" + utf8ToWide(original) + L"\"";

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE read_pipe = nullptr;
        HANDLE write_pipe = nullptr;
        if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
            result.error = "CreatePipe failed";
            result.exit_code = -1;
            return result;
        }
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write_pipe;
        si.hStdError = write_pipe;
        si.hStdInput = nullptr;

        PROCESS_INFORMATION pi{};

        // L6: Job Object —— 进程树生命周期 + 资源上限 + 超时强杀
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            info.BasicLimitInformation.ActiveProcessLimit = 16;
            info.ProcessMemoryLimit =
                (SIZE_T)(effective.max_memory_mb ? effective.max_memory_mb : 512) *
                1024 * 1024;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                    &info, sizeof(info));
        }

        std::wstring cwd = utf8ToWide(work_dir);
        BOOL ok = CreateProcessW(nullptr, cmdline.data(),
                                 nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                 cwd.c_str(), &si, &pi);
        CloseHandle(write_pipe);
        write_pipe = nullptr;

        if (!ok) {
            if (job) CloseHandle(job);
            CloseHandle(read_pipe);
            result.error = "CreateProcess failed (error " +
                           std::to_string(GetLastError()) + ")";
            result.exit_code = -1;
            return result;
        }

        if (job) {
            if (!AssignProcessToJobObject(job, pi.hProcess)) {
                // 分配失败不致命，退化为普通进程 + 超时强杀
            }
        }
        ResumeThread(pi.hThread);

        uint32_t timeout_ms = effective.timeout_seconds * 1000;
        if (timeout_ms == 0) timeout_ms = 30000;

        auto start = std::chrono::steady_clock::now();
        std::string output;
        bool timed_out = false;

        while (true) {
            DWORD available = 0;
            if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) &&
                available > 0) {
                char buffer[4096];
                DWORD to_read = available > sizeof(buffer) ? sizeof(buffer) : available;
                DWORD read_bytes = 0;
                if (ReadFile(read_pipe, buffer, to_read, &read_bytes, nullptr) && read_bytes > 0) {
                    output.append(buffer, read_bytes);
                    continue;
                }
            }

            DWORD wait = WaitForSingleObject(pi.hProcess, 10);
            if (wait == WAIT_OBJECT_0) {
                // 读取剩余数据
                DWORD read_bytes = 0;
                char buffer[4096];
                while (ReadFile(read_pipe, buffer, sizeof(buffer), &read_bytes, nullptr) &&
                       read_bytes > 0) {
                    output.append(buffer, read_bytes);
                }
                break;
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
            if ((uint64_t)elapsed > timeout_ms) {
                timed_out = true;
                if (job) {
                    TerminateJobObject(job, 1);
                } else {
                    TerminateProcess(pi.hProcess, 1);
                }
                WaitForSingleObject(pi.hProcess, 2000);
                break;
            }
        }

        DWORD exit_code = (DWORD)-1;
        GetExitCodeProcess(pi.hProcess, &exit_code);

        CloseHandle(read_pipe);
        if (job) CloseHandle(job);  // KILL_ON_JOB_CLOSE: 清理残留子进程
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        result.output = ansiToUtf8(output);
        result.exit_code = timed_out ? -2 : (int)exit_code;
        result.success = !timed_out && exit_code == 0;
        result.duration_ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();

        if (timed_out) {
            result.error = "命令执行超时 (" + std::to_string(timeout_ms) + " ms)";
        } else if (!result.success) {
            result.error = "命令退出码: " + std::to_string(exit_code);
        }

        return result;
    }

    bool isPathAllowed(const std::string& path, const SandboxConfig& cfg) override {
        const SandboxConfig& effective = cfg.work_dir.empty() ? config_ : cfg;

        try {
            fs::path target = fs::weakly_canonical(fs::absolute(utf8ToPath(path)));
            std::string target_str = target.string();

            // 显式黑名单优先
            for (const auto& blocked : effective.blocked_paths) {
                if (blocked.empty()) continue;
                fs::path b = fs::weakly_canonical(fs::absolute(utf8ToPath(blocked)));
                std::string b_str = b.string();
                if (target_str == b_str ||
                    target_str.rfind(b_str + "\\", 0) == 0 ||
                    target_str.rfind(b_str + "/", 0) == 0) {
                    return false;
                }
            }

            std::vector<fs::path> roots;
            if (!effective.work_dir.empty()) {
                roots.push_back(fs::weakly_canonical(fs::absolute(utf8ToPath(effective.work_dir))));
            }
            for (const auto& dir : effective.allowed_dirs) {
                if (!dir.empty()) {
                    roots.push_back(fs::weakly_canonical(fs::absolute(utf8ToPath(dir))));
                }
            }
            if (roots.empty()) return true;

            for (const auto& root : roots) {
                std::string root_str = root.string();
                if (target_str == root_str ||
                    target_str.rfind(root_str + "\\", 0) == 0 ||
                    target_str.rfind(root_str + "/", 0) == 0) {
                    return true;
                }
            }
            return false;
        } catch (...) {
            return false;
        }
    }

    std::string getStatus() const override {
        std::ostringstream oss;
        oss << "[Windows Sandbox]\n";
        oss << "  工作目录: " << config_.work_dir << "\n";
        oss << "  允许网络: " << (config_.allow_network ? "是" : "否") << "\n";
        oss << "  超时: " << config_.timeout_seconds << " 秒\n";
        oss << "  内存上限: " << config_.max_memory_mb << " MB\n";
        oss << "  额外允许目录: " << config_.allowed_dirs.size() << " 个\n";
        oss << "  禁止路径: " << config_.blocked_paths.size() << " 个\n";

        std::error_code ec;
        fs::path probe = utf8ToPath(config_.work_dir) / "__sandbox_probe__.tmp";
        {
            std::ofstream ofs(probe, std::ios::binary);
            if (!ofs) {
                oss << "  写权限: 否\n  状态: 不可用\n";
                return oss.str();
            }
            ofs << "probe";
        }
        fs::remove(probe, ec);
        oss << "  写权限: 是\n  状态: 就绪\n";
        return oss.str();
    }

private:
    SandboxConfig config_;
};

// ============================================================================
// 工厂函数
// ============================================================================
std::shared_ptr<ISandbox> createWindowsSandbox(SandboxConfig config) {
    if (config.work_dir.empty()) {
        config.work_dir = pathToUtf8(fs::current_path());
    }
    return std::make_shared<WindowsSandbox>(config);
}
