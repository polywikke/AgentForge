# AgentForge 新手快速上手

> **5 分钟跑起来** ｜ 当前状态：L1-L12 全阶段功能已实现（2026-10-26）
> 完整矩阵见 [L1-L12_MATRIX.md](L1-L12_MATRIX.md)；自检 `agentforge_v2.exe --selftest`

---

## 0. 前置条件

- Windows 10/11 x64
- CMake ≥ 3.15
- Visual Studio 2022/2026（含 "使用 C++ 的桌面开发" 工作负载）
- 一个可用的 LLM API（OpenAI / Anthropic / DeepSeek / 中转站均可）

> `httplib.h` 与 `nlohmann/json.hpp` 已随仓库提供，无需下载。

---

## 1. 编译

```bat
cd AgentForge
quick_build.bat
```

成功标志：`bin\Release\agentforge_web.exe`（约 656 KB）。

---

## 2. 配置 API

### 方式 A：网页配置（推荐）

1. 启动服务（下一步）；
2. 浏览器打开 <http://127.0.0.1:8080/setup.html>；
3. 选择提供商 → 填 Endpoint / API Key / Model → 保存。

### 方式 B：直接编辑 config.json

```bat
copy config.example.json config.json
notepad config.json
```

```json
{
  "api": {
    "provider": "custom",
    "endpoint": "https://your-proxy.com/v1/chat/completions",
    "key": "sk-xxxx",
    "model": "your-model"
  }
}
```

常见提供商端点：

| 提供商 | endpoint | 示例模型 |
|--------|----------|----------|
| Anthropic | `https://api.anthropic.com/v1/messages` | `claude-3-5-sonnet-20241022` |
| OpenAI | `https://api.openai.com/v1/chat/completions` | `gpt-4o` |
| DeepSeek | `https://api.deepseek.com/v1/chat/completions` | `deepseek-chat` |
| 中转站 | 由服务商提供（OpenAI 兼容居多） | 由服务商提供 |

---

## 3. 启动

```bat
run_server.bat
```

浏览器自动打开，或手动访问 <http://127.0.0.1:8080/>。

其他参数：

```bat
bin\Release\agentforge_web.exe --config config.json --port 8090
```

---

## 4. 试一试

| 输入 | 预期行为 |
|------|----------|
| `你好，介绍一下你自己` | 直接回答（不调工具） |
| `读取 README.md 的前 20 行并总结` | 出现 🔧 Read 卡片 + 结果，再给出总结 |
| `在 src 目录搜索 "AgentLoop"` | 出现 Grep 卡片 |
| `新建 note.txt，内容为 Hello` | 出现 Write 卡片（有文件副作用） |
| `执行命令 echo hi` | Bash 卡片；`tools.sandbox=true` 时在沙箱中执行 |

第二轮追问（如"刚才的文件里第一行是什么"）会带上历史，模型能记住上下文。

---

## 5. 测试与排错

```powershell
# 只测服务与静态页面
powershell -ExecutionPolicy Bypass -File tools\smoke_test.ps1

# 不依赖外网的完整 Agent 流程（另开两个终端）
python tools\mock_llm_server.py 18081
bin\Release\agentforge_web.exe --config config.test.json --port 18080
```

| 现象 | 处理 |
|------|------|
| 页面 404 | 从 `AgentForge` 目录启动；确认 `web/index.html` 存在 |
| 提示 API 未配置 | 打开 `/setup.html` 配置；或检查 `config.json` 的 `api.key` |
| 模型返回 400 `temperature` | 新版会自动去掉 temperature 重试；仍失败请检查模型名 |
| HTTPS 报安全包无凭据 | 在普通用户会话运行（受限令牌下 Schannel 不可用） |
| 前端 `network error` | 使用最新编译产物（已修复 SSE 正常结束） |

更多：[HANDOVER.md](../HANDOVER.md) ｜ [VERIFICATION.md](VERIFICATION.md)
