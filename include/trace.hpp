#pragma once

#include "loop_trace.hpp"
#include "types.hpp"
#include "win_utf8.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// JSONL Tracer (L12)
// span: 有始有终；event: 某一刻发生的事；环形缓冲丢弃可见。
// ============================================================================

struct TraceSpan {
    int id = 0;
    int parent_id = -1;
    std::string name;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    json tags = json::object();
    int64_t durationMs() const { return end_ms >= start_ms ? end_ms - start_ms : 0; }
};

struct TraceEvent {
    int64_t ts_ms = 0;
    std::string name;
    json tags = json::object();
};

class JsonlTracer : public ILoopTracer {
public:
    explicit JsonlTracer(std::size_t capacity = 4096) : capacity_(capacity) {}

    void spanStart(const std::string& name, const json& tags = json::object()) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto tid = std::this_thread::get_id();
        int parent = -1;
        auto sit = stacks_.find(tid);
        if (sit != stacks_.end() && !sit->second.empty()) parent = sit->second.back();

        if (spans_.size() >= capacity_) {
            ++dropped_;
            stacks_[tid].push_back(-1);
            return;
        }
        TraceSpan span;
        span.id = next_id_++;
        span.parent_id = parent;
        span.name = name;
        span.start_ms = nowMs();
        span.tags = tags;
        spans_.push_back(std::move(span));
        stacks_[tid].push_back(spans_.back().id);
    }

    void spanEnd(const std::string& name, const json& tags = json::object()) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto tid = std::this_thread::get_id();
        auto sit = stacks_.find(tid);
        if (sit == stacks_.end() || sit->second.empty()) return;
        int id = sit->second.back();
        sit->second.pop_back();

        for (auto it = spans_.rbegin(); it != spans_.rend(); ++it) {
            if (it->id != id) continue;
            if (!name.empty() && it->name != name) continue;
            it->end_ms = nowMs();
            if (tags.is_object()) {
                for (auto tit = tags.begin(); tit != tags.end(); ++tit) {
                    it->tags[tit.key()] = tit.value();
                }
            }
            break;
        }
    }

    void event(const std::string& name, const json& tags = json::object()) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.size() >= capacity_) {
            ++dropped_;
            return;
        }
        TraceEvent ev;
        ev.ts_ms = nowMs();
        ev.name = name;
        ev.tags = tags;
        events_.push_back(std::move(ev));
    }

    json toJson() const {
        std::lock_guard<std::mutex> lock(mutex_);
        json result;
        result["dropped"] = (long long)dropped_;
        result["span_count"] = (int)spans_.size();
        result["event_count"] = (int)events_.size();
        result["spans"] = json::array();
        result["events"] = json::array();
        for (const auto& span : spans_) {
            result["spans"].push_back({{"id", span.id},
                                       {"parent", span.parent_id},
                                       {"name", span.name},
                                       {"start_ms", (long long)span.start_ms},
                                       {"end_ms", (long long)span.end_ms},
                                       {"duration_ms", (long long)span.durationMs()},
                                       {"tags", span.tags}});
        }
        for (const auto& ev : events_) {
            result["events"].push_back({{"ts_ms", (long long)ev.ts_ms},
                                        {"name", ev.name},
                                        {"tags", ev.tags}});
        }
        return result;
    }

    bool saveJsonl(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            std::filesystem::path p = utf8ToPath(path);
            if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
            std::ofstream out(p, std::ios::app | std::ios::binary);
            if (!out) return false;
            for (const auto& span : spans_) {
                out << json{{"type", "span"},
                            {"id", span.id},
                            {"parent", span.parent_id},
                            {"name", span.name},
                            {"start_ms", (long long)span.start_ms},
                            {"end_ms", (long long)span.end_ms},
                            {"duration_ms", (long long)span.durationMs()},
                            {"tags", span.tags}}.dump()
                    << "\n";
            }
            for (const auto& ev : events_) {
                out << json{{"type", "event"},
                            {"ts_ms", (long long)ev.ts_ms},
                            {"name", ev.name},
                            {"tags", ev.tags}}.dump()
                    << "\n";
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    std::size_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    json summary() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::string, int> counts;
        int64_t total_span_ms = 0;
        for (const auto& span : spans_) {
            counts[span.name]++;
            total_span_ms += span.durationMs();
        }
        json j;
        j["spans"] = counts;
        j["total_span_ms"] = (long long)total_span_ms;
        j["dropped"] = (long long)dropped_;
        return j;
    }

private:
    std::size_t capacity_ = 4096;
    mutable std::mutex mutex_;
    std::vector<TraceSpan> spans_;
    std::vector<TraceEvent> events_;
    std::map<std::thread::id, std::vector<int>> stacks_;
    std::size_t dropped_ = 0;
    int next_id_ = 1;

    static int64_t nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
};
