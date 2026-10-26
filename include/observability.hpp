#pragma once

#include "types.hpp"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <mutex>
#include <stack>
#include <limits>
#include <algorithm>

// ============================================================================
// 结构化日志 (L12)
// ============================================================================

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

struct LogEntry {
    int64_t timestamp;
    LogLevel level;
    std::string message;
    std::map<std::string, std::string> fields;
};

class StructuredLogger {
public:
    explicit StructuredLogger(const std::string& log_file)
        : log_file_(log_file)
    {
        // 确保日志目录存在
        namespace fs = std::filesystem;
        fs::path p(log_file);
        if (!fs::exists(p.parent_path())) {
            fs::create_directories(p.parent_path());
        }
    }

    void log(LogLevel level, const std::string& message,
            const std::map<std::string, std::string>& fields = {}) {
        LogEntry entry;
        entry.timestamp = getCurrentTimeMs();
        entry.level = level;
        entry.message = message;
        entry.fields = fields;

        writeEntry(entry);
    }

    void debug(const std::string& msg,
              const std::map<std::string, std::string>& fields = {}) {
        log(LogLevel::Debug, msg, fields);
    }

    void info(const std::string& msg,
             const std::map<std::string, std::string>& fields = {}) {
        log(LogLevel::Info, msg, fields);
    }

    void warning(const std::string& msg,
                const std::map<std::string, std::string>& fields = {}) {
        log(LogLevel::Warning, msg, fields);
    }

    void error(const std::string& msg,
              const std::map<std::string, std::string>& fields = {}) {
        log(LogLevel::Error, msg, fields);
    }

private:
    std::string log_file_;
    std::mutex mutex_;

    int64_t getCurrentTimeMs() const {
        auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
    }

    void writeEntry(const LogEntry& entry) {
        std::lock_guard<std::mutex> lock(mutex_);

        try {
            json j;
            j["timestamp"] = entry.timestamp;
            j["level"] = levelToString(entry.level);
            j["message"] = entry.message;
            j["fields"] = entry.fields;

            std::ofstream ofs(log_file_, std::ios::app);
            ofs << j.dump() << "\n";
            ofs.close();
        } catch (...) {
            // 忽略日志失败
        }
    }

    std::string levelToString(LogLevel level) const {
        switch (level) {
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info: return "INFO";
            case LogLevel::Warning: return "WARNING";
            case LogLevel::Error: return "ERROR";
        }
        return "UNKNOWN";
    }
};

// ============================================================================
// 追踪器 (Tracer)
// ============================================================================

struct Span {
    std::string name;
    int64_t start_time;
    int64_t end_time;
    std::map<std::string, std::string> tags;
    std::vector<std::shared_ptr<Span>> children;
};

class Tracer {
public:
    explicit Tracer(const std::string& trace_file)
        : trace_file_(trace_file)
        , root_span_(nullptr)
    {}

    // 开始根 Span
    void startRootSpan(const std::string& name) {
        root_span_ = std::make_shared<Span>();
        root_span_->name = name;
        root_span_->start_time = getCurrentTimeMs();
        current_span_ = root_span_;
    }

    // 开始子 Span
    void startSpan(const std::string& name) {
        if (!current_span_) {
            startRootSpan(name);
            return;
        }

        auto span = std::make_shared<Span>();
        span->name = name;
        span->start_time = getCurrentTimeMs();

        current_span_->children.push_back(span);
        span_stack_.push(current_span_);
        current_span_ = span;
    }

    // 结束当前 Span
    void endSpan(const std::map<std::string, std::string>& tags = {}) {
        if (!current_span_) return;

        current_span_->end_time = getCurrentTimeMs();
        current_span_->tags = tags;

        if (!span_stack_.empty()) {
            current_span_ = span_stack_.top();
            span_stack_.pop();
        }
    }

    // 添加标签
    void addTag(const std::string& key, const std::string& value) {
        if (current_span_) {
            current_span_->tags[key] = value;
        }
    }

    // 完成追踪并写入文件
    void finishTrace() {
        if (root_span_ && root_span_->end_time == 0) {
            root_span_->end_time = getCurrentTimeMs();
        }

        writeTrace();
    }

private:
    std::string trace_file_;
    std::shared_ptr<Span> root_span_;
    std::shared_ptr<Span> current_span_;
    std::stack<std::shared_ptr<Span>> span_stack_;

    int64_t getCurrentTimeMs() const {
        auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
    }

    void writeTrace() {
        if (!root_span_) return;

        try {
            json j = spanToJson(root_span_);

            std::ofstream ofs(trace_file_);
            ofs << j.dump(2);
            ofs.close();
        } catch (...) {
            // 忽略写入失败
        }
    }

    json spanToJson(const std::shared_ptr<Span>& span) const {
        json j;
        j["name"] = span->name;
        j["start_time"] = span->start_time;
        j["end_time"] = span->end_time;
        j["duration_ms"] = span->end_time - span->start_time;
        j["tags"] = span->tags;

        if (!span->children.empty()) {
            json children = json::array();
            for (const auto& child : span->children) {
                children.push_back(spanToJson(child));
            }
            j["children"] = children;
        }

        return j;
    }
};

// ============================================================================
// 成本分析器
// ============================================================================

struct CostBreakdown {
    double total_cost = 0.0;
    double input_cost = 0.0;
    double output_cost = 0.0;
    double cache_cost = 0.0;
    int total_requests = 0;
};

class CostAnalyzer {
public:
    // 价格表 (USD per 1M tokens)
    struct Pricing {
        double input_per_1m = 3.0;    // GPT-4 input
        double output_per_1m = 15.0;  // GPT-4 output
        double cache_per_1m = 0.15;   // Cache 价格
    };

    CostAnalyzer()
        : pricing_()
    {}

    explicit CostAnalyzer(Pricing pricing)
        : pricing_(pricing)
    {}

    // 添加使用记录
    void addUsage(const Usage& usage) {
        total_usage_.prompt_tokens += usage.prompt_tokens;
        total_usage_.completion_tokens += usage.completion_tokens;
        total_usage_.cache_read_tokens += usage.cache_read_tokens;
        total_usage_.cache_write_tokens += usage.cache_write_tokens;
        request_count_++;
    }

    // 计算成本
    CostBreakdown calculateCost() const {
        CostBreakdown breakdown;

        breakdown.input_cost = (total_usage_.prompt_tokens / 1000000.0) *
                              pricing_.input_per_1m;
        breakdown.output_cost = (total_usage_.completion_tokens / 1000000.0) *
                               pricing_.output_per_1m;
        breakdown.cache_cost = (total_usage_.cache_read_tokens / 1000000.0) *
                              pricing_.cache_per_1m;

        breakdown.total_cost = breakdown.input_cost +
                              breakdown.output_cost +
                              breakdown.cache_cost;
        breakdown.total_requests = request_count_;

        return breakdown;
    }

    // 生成报告
    std::string generateReport() const {
        auto breakdown = calculateCost();

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4);

        oss << "═══════════════ 成本分析 ═══════════════\n";
        oss << "总请求数: " << breakdown.total_requests << "\n\n";

        oss << "Token 使用:\n";
        oss << "  输入:   " << total_usage_.prompt_tokens << " tokens\n";
        oss << "  输出:   " << total_usage_.completion_tokens << " tokens\n";
        oss << "  缓存:   " << total_usage_.cache_read_tokens << " tokens\n";
        oss << "  总计:   " << total_usage_.total() << " tokens\n\n";

        oss << "成本明细:\n";
        oss << "  输入成本:   $" << breakdown.input_cost << "\n";
        oss << "  输出成本:   $" << breakdown.output_cost << "\n";
        oss << "  缓存成本:   $" << breakdown.cache_cost << "\n";
        oss << "  总成本:     $" << breakdown.total_cost << "\n";

        oss << "═══════════════════════════════════════\n";

        return oss.str();
    }

    Usage getTotalUsage() const { return total_usage_; }

private:
    Pricing pricing_;
    Usage total_usage_;
    int request_count_ = 0;
};

// ============================================================================
// 性能度量器
// ============================================================================

struct PerformanceMetrics {
    int64_t total_duration_ms = 0;
    int64_t avg_request_ms = 0;
    int64_t min_request_ms = 0;
    int64_t max_request_ms = 0;
    int total_requests = 0;
};

class PerformanceMonitor {
public:
    void recordRequest(int64_t duration_ms) {
        request_durations_.push_back(duration_ms);
    }

    PerformanceMetrics getMetrics() const {
        PerformanceMetrics metrics;

        if (request_durations_.empty()) {
            return metrics;
        }

        metrics.total_requests = static_cast<int>(request_durations_.size());

        int64_t total = 0;
        int64_t min_val = std::numeric_limits<int64_t>::max();
        int64_t max_val = 0;

        for (int64_t duration : request_durations_) {
            total += duration;
            min_val = std::min(min_val, duration);
            max_val = std::max(max_val, duration);
        }

        metrics.total_duration_ms = total;
        metrics.avg_request_ms = total / metrics.total_requests;
        metrics.min_request_ms = min_val;
        metrics.max_request_ms = max_val;

        return metrics;
    }

    std::string generateReport() const {
        auto metrics = getMetrics();

        std::ostringstream oss;
        oss << "═══════════════ 性能指标 ═══════════════\n";
        oss << "总请求数:   " << metrics.total_requests << "\n";
        oss << "总耗时:     " << metrics.total_duration_ms << " ms\n";
        oss << "平均耗时:   " << metrics.avg_request_ms << " ms\n";
        oss << "最短耗时:   " << metrics.min_request_ms << " ms\n";
        oss << "最长耗时:   " << metrics.max_request_ms << " ms\n";
        oss << "═══════════════════════════════════════\n";

        return oss.str();
    }

private:
    std::vector<int64_t> request_durations_;
};
