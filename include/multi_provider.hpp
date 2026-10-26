// 多 API 提供商支持 - Anthropic Claude 和 OpenAI GPT
#pragma once
#include "model_provider.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <memory>

namespace agentforge {

// API 提供商配置
struct APIConfig {
    enum Provider {
        Anthropic,
        OpenAI,
        DeepSeek,
        Custom
    };

    Provider provider = Anthropic;
    std::string api_key;
    std::string api_endpoint;  // 自定义端点
    std::string model = "claude-3-5-sonnet-20241022";
    double temperature = 0.7;
    int max_tokens = 4096;
    bool use_custom_format = false;  // 使用自定义格式

    // 获取 API 端点
    std::string getEndpoint() const {
        // 如果设置了自定义端点，优先使用
        if (!api_endpoint.empty()) {
            return api_endpoint;
        }

        // 否则使用默认端点
        switch (provider) {
            case Anthropic:
                return "https://api.anthropic.com/v1/messages";
            case OpenAI:
                return "https://api.openai.com/v1/chat/completions";
            case DeepSeek:
                return "https://api.deepseek.com/v1/chat/completions";
            case Custom:
                return api_endpoint;
            default:
                return "";
        }
    }

    // 获取提供商类型（用于格式转换）
    Provider getProviderType() const {
        // 自定义端点时，根据 URL 推测类型
        if (provider == Custom && !api_endpoint.empty()) {
            if (api_endpoint.find("anthropic") != std::string::npos) {
                return Anthropic;
            } else if (api_endpoint.find("deepseek") != std::string::npos) {
                return DeepSeek;
            } else {
                // 默认使用 OpenAI 兼容格式（最通用）
                return OpenAI;
            }
        }
        return provider;
    }

    // 从 JSON 加载
    static APIConfig fromJSON(const nlohmann::json& j) {
        APIConfig config;

        if (j.contains("apiProvider")) {
            std::string provider = j["apiProvider"];
            if (provider == "openai") {
                config.provider = OpenAI;
            } else if (provider == "deepseek") {
                config.provider = DeepSeek;
            } else if (provider == "custom") {
                config.provider = Custom;
            } else {
                config.provider = Anthropic;
            }
        }

        if (j.contains("apiEndpoint")) config.api_endpoint = j["apiEndpoint"];
        if (j.contains("apiKey")) config.api_key = j["apiKey"];
        if (j.contains("model")) config.model = j["model"];
        if (j.contains("temperature")) config.temperature = j["temperature"];
        if (j.contains("maxTokens")) config.max_tokens = j["maxTokens"];
        if (j.contains("useCustomFormat")) config.use_custom_format = j["useCustomFormat"];

        return config;
    }
};

// 通用 HTTP Model Provider
class MultiAPIProvider : public IModelProvider {
public:
    MultiAPIProvider(IHttpTransport& transport, const APIConfig& config);

    ChatResponse chat(const ChatRequest& request) override;
    ChatResponse chatStream(const ChatRequest& request,
                            IModelProvider::StreamCallback on_chunk) override;
    std::string name() const override;

    // 更新配置
    void updateConfig(const APIConfig& config);
    APIConfig getConfig() const { return config_; }

private:
    // 构建 HTTP 请求
    HttpRequest buildHttpRequest(const ChatRequest& request, bool stream) const;

    // 构建请求体
    nlohmann::json buildRequestBody(const ChatRequest& request, bool stream) const;

    // 解析响应
    ChatResponse parseResponse(const nlohmann::json& response) const;

    // Anthropic 特定格式
    nlohmann::json buildAnthropicRequest(const ChatRequest& request, bool stream) const;
    ChatResponse parseAnthropicResponse(const nlohmann::json& response) const;

    // OpenAI 特定格式
    nlohmann::json buildOpenAIRequest(const ChatRequest& request, bool stream) const;
    ChatResponse parseOpenAIResponse(const nlohmann::json& response) const;

    // 流式聚合
    ChatResponse chatStreamOpenAI(const ChatRequest& request,
                                  IModelProvider::StreamCallback on_chunk);
    ChatResponse chatStreamAnthropic(const ChatRequest& request,
                                     IModelProvider::StreamCallback on_chunk);

    IHttpTransport& transport_;
    APIConfig config_;
};

} // namespace agentforge
