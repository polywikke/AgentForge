#include "../include/model_provider.hpp"
#include <random>
#include <sstream>
#include <thread>
#include <chrono>

// ============================================================================
// 模拟模型提供者 (用于演示和测试)
// ============================================================================

class MockModelProvider : public IModelProvider {
public:
    MockModelProvider(const std::string& name = "mock-gpt")
        : name_(name), request_count_(0)
    {}

    ChatResponse chat(const ChatRequest& request) override {
        request_count_++;

        ChatResponse response;
        response.id = "mock-" + std::to_string(request_count_);

        // 模拟延迟
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 分析用户输入，决定如何响应
        const auto& last_msg = request.messages.back();

        // 如果有工具可用且是用户消息，模拟工具调用
        if (!request.tools.empty() && last_msg.role == Role::User) {
            // 模拟调用第一个可用工具
            response.message.role = Role::Assistant;
            response.message.content = "我将使用工具来完成这个任务。";

            // 根据任务类型选择工具
            std::string task = last_msg.content;
            for (const auto& tool : request.tools) {
                if (task.find("读取") != std::string::npos && tool.name == "Read") {
                    ToolCall call;
                    call.id = "call_" + std::to_string(request_count_);
                    call.name = "Read";
                    call.arguments = {{"path", "example.txt"}};
                    response.message.tool_calls.push_back(call);
                    break;
                }
                else if (task.find("写入") != std::string::npos && tool.name == "Write") {
                    ToolCall call;
                    call.id = "call_" + std::to_string(request_count_);
                    call.name = "Write";
                    call.arguments = {
                        {"path", "output.txt"},
                        {"content", "这是模拟的内容"}
                    };
                    response.message.tool_calls.push_back(call);
                    break;
                }
                else if (task.find("搜索") != std::string::npos && tool.name == "Grep") {
                    ToolCall call;
                    call.id = "call_" + std::to_string(request_count_);
                    call.name = "Grep";
                    call.arguments = {
                        {"pattern", "example"},
                        {"path", "."}
                    };
                    response.message.tool_calls.push_back(call);
                    break;
                }
            }

            // 如果没有匹配的工具或这是工具结果的后续响应
            if (response.message.tool_calls.empty()) {
                response.message.content = generateMockResponse(request);
                response.finish_reason = FinishReason::Stop;
            } else {
                response.finish_reason = FinishReason::ToolCalls;
            }
        } else {
            // 生成最终回复
            response.message.role = Role::Assistant;
            response.message.content = generateMockResponse(request);
            response.finish_reason = FinishReason::Stop;
        }

        // 模拟使用量
        response.usage.prompt_tokens = 100 + (int)request.messages.size() * 50;
        response.usage.completion_tokens = 50;
        response.usage.cache_read_tokens = 20;

        return response;
    }

    ChatResponse chatStream(const ChatRequest& request,
                           StreamCallback on_chunk) override {
        // 发送开始事件
        on_chunk(ChunkStart{"mock-stream-" + std::to_string(request_count_)});

        // 模拟流式输出
        std::string full_response = "这是一个流式响应的模拟。";
        for (char c : full_response) {
            on_chunk(ChunkDelta{std::string(1, c)});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // 发送完成事件
        Usage usage;
        usage.prompt_tokens = 100;
        usage.completion_tokens = 50;
        on_chunk(ChunkFinish{FinishReason::Stop, usage});

        // 返回聚合结果
        ChatResponse response;
        response.message.role = Role::Assistant;
        response.message.content = full_response;
        response.usage = usage;
        response.finish_reason = FinishReason::Stop;

        return response;
    }

    std::string name() const override {
        return name_;
    }

private:
    std::string name_;
    int request_count_;

    std::string generateMockResponse(const ChatRequest& request) {
        // 分析消息历史生成回复
        std::ostringstream oss;
        oss << "根据工具执行结果,我已经完成了您的请求。\n";

        // 查找工具结果
        int tool_results = 0;
        for (const auto& msg : request.messages) {
            if (msg.call_id.has_value()) {
                tool_results++;
            }
        }

        if (tool_results > 0) {
            oss << "执行了 " << tool_results << " 个工具调用。\n";
            oss << "任务已成功完成。";
        } else {
            oss << "我已经理解了您的需求。";
        }

        return oss.str();
    }
};

// ============================================================================
// 工厂函数
// ============================================================================

std::shared_ptr<IModelProvider> createMockProvider(const std::string& model_name) {
    return std::make_shared<MockModelProvider>(model_name);
}
