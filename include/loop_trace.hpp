#pragma once

#include "types.hpp"
#include <string>

// ============================================================================
// 循环追踪端口（L12）
// AgentLoop 只依赖这个接口，具体实现可以是 JSONL Tracer / NoopTracer。
// ============================================================================
class ILoopTracer {
public:
    virtual ~ILoopTracer() = default;
    virtual void spanStart(const std::string& name,
                           const json& tags = json::object()) = 0;
    virtual void spanEnd(const std::string& name,
                         const json& tags = json::object()) = 0;
    virtual void event(const std::string& name,
                       const json& tags = json::object()) = 0;
};
