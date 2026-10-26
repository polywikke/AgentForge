#pragma once

#include "types.hpp"
#include <string>
#include <vector>

// ============================================================================
// 沙箱接口 - 操作系统级强制访问控制
// 注意: 本工程契约层 (types/tool_registry/sandbox) 统一使用全局命名空间
// ============================================================================

// 沙箱配置
struct SandboxConfig {
    std::string work_dir;                    // 工作目录 (允许访问的根目录)
    bool allow_network = false;              // 是否允许网络访问
    uint64_t max_memory_mb = 512;            // 最大内存限制 (MB)
    uint32_t max_cpu_percent = 80;           // 最大CPU使用率 (%)
    uint32_t timeout_seconds = 30;           // 超时时间 (秒)
    std::vector<std::string> allowed_dirs;   // 额外允许的目录
    std::vector<std::string> blocked_paths;  // 明确禁止的路径
};

// 沙箱执行结果
struct SandboxResult {
    bool success = false;              // 是否成功执行
    std::string output;                // 标准输出
    std::string error;                 // 标准错误
    int exit_code = 0;                 // 退出码
    bool blocked = false;              // 是否被沙箱阻止
    std::string block_reason;          // 阻止原因
    uint64_t memory_used_mb = 0;       // 实际内存使用 (MB)
    uint32_t cpu_percent = 0;          // 实际CPU使用率 (%)
    uint32_t duration_ms = 0;          // 执行时长 (ms)
};

// ISandbox - 沙箱抽象接口 (INV-2: 端口必须是抽象类)
class ISandbox {
public:
    virtual ~ISandbox() = default;

    // 在沙箱中执行命令
    virtual SandboxResult execute(
        const std::string& command,
        const std::vector<std::string>& args,
        const SandboxConfig& config
    ) = 0;

    // 检查路径是否在允许范围内
    virtual bool isPathAllowed(
        const std::string& path,
        const SandboxConfig& config
    ) = 0;

    // 获取沙箱状态信息
    virtual std::string getStatus() const = 0;
};
