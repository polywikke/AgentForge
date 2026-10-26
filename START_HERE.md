# 🚀 AgentForge 快速启动

> **项目状态**: ✅ L1-L12 全阶段功能已实现并集成（2026-10-26）
> 首次阅读顺序：本页 → [README.md](README.md) → [docs/L1-L12_MATRIX.md](docs/L1-L12_MATRIX.md)
> 功能自检：`bin\Release\agentforge_v2.exe --selftest`（15 项）

---

## 1. 编译（首次 1-3 分钟，含 tree-sitter）

```bat
cd AgentForge
quick_build.bat
```

PowerShell 版本（会打印本机 Visual Studio 检测结果）：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_and_test.ps1
```

需要：CMake ≥ 3.15 + Visual Studio 2022/2026（含 "使用 C++ 的桌面开发"，Build Tools 亦可）。
**不需要** Developer Command Prompt。

产物：

```
bin\Release\agentforge_web.exe    ← 主程序
bin\Release\agentforge.exe        ← V1 控制台 Demo
bin\Release\agentforge_v2.exe     ← L1-L12 控制台版
```

## 2. 启动

```bat
run_server.bat
```

或：

```bat
bin\Release\agentforge_web.exe --config config.json
```

浏览器自动打开 <http://127.0.0.1:8080/>。

## 3. 配置 API

任选其一：

- 页面右上角 **设置** → 填 Endpoint / API Key / Model → 保存；
- 打开 <http://127.0.0.1:8080/setup.html> 使用配置向导；
- 直接编辑 `config.json`（参考 `config.example.json`）后重启。

配置示例（OpenAI 兼容中转站）：

```json
"api": {
  "provider": "custom",
  "endpoint": "https://your-proxy.com/v1/chat/completions",
  "key": "sk-...",
  "model": "your-model"
}
```

## 4. 开始对话

在输入框输入例如：

- `读取 README.md 并总结这个项目`
- `在 src 目录搜索 "AgentLoop" 出现在哪些文件`
- `新建 hello.txt，内容为 Hello AgentForge`

工具调用会以卡片形式显示参数与执行结果。

---

## 5. 验证 / 排错

| 目的 | 命令 |
|------|------|
| 冒烟测试（不调用外部 API） | `powershell -ExecutionPolicy Bypass -File tools\smoke_test.ps1` |
| 前端静态检查 | `python tools\check_web_ui.py` |
| 离线端到端（Mock LLM） | `python tools\mock_llm_server.py 18081` + `bin\Release\agentforge_web.exe --config config.test.json --port 18080` |

常见问题：

- **HTTP 404 / 页面打不开**：确认从 `AgentForge` 目录启动，`web/` 存在。
- **API 未配置**：打开 `/setup.html` 填写；保存后自动写入 `config.json`。
- **HTTPS 失败（Schannel 无凭据）**：用普通用户会话运行，勿在受限令牌/服务账户中运行。
- 更多见 [HANDOVER.md 第 7 节](HANDOVER.md#7-常见问题)。

---

**详细文档**：[README.md](README.md) ｜ [docs/VERIFICATION.md](docs/VERIFICATION.md) ｜ [docs/CONFIGURATION_GUIDE.md](docs/CONFIGURATION_GUIDE.md)
