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

脚本会自己找 CMake（PATH → Visual Studio 自带的 CMake → 常见安装位置）和编译器
（Visual Studio 2022/2026 → VS 2019 → Ninja/MinGW），**不需要** Developer Command Prompt。
`build\` 若是别的电脑留下的旧缓存，会被自动识别并重建。

PowerShell 版本（会打印本机 Visual Studio 检测结果）：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_and_test.ps1
```

需要：CMake ≥ 3.15 + Visual Studio 2022/2026（含 "使用 C++ 的桌面开发"，Build Tools 亦可），
或 MinGW-w64 g++ + Ninja。

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

- 若还没编译，会自动先调用 `quick_build.bat`；
- 服务器就绪后**自动打开浏览器**（默认 <http://127.0.0.1:8080/>，端口被占用会自动顺延）；
- 端口可用 `<run_server.bat --port 9000>` 指定，`--no-open` 表示不自动开浏览器。

## 3. 第一次配置（自动进入向导）

`config.json` 里不带任何 Key，所以第一次启动浏览器会直接停在 `/setup.html`：

1. 选服务商（DeepSeek / OpenAI / Claude / 自定义中转站）；
2. 填请求地址（只填 base_url 也行，保存时自动补 `/v1/chat/completions`）；
3. 填 API Key；
4. **模型名自动检测**：填完 Key 后自动请求 `/models` 并给出下拉列表，自动选中第一个；
   若该地址没有 `/models`，会给出常见模型候选，也可以手动输入；
5. 点「保存并进入 AgentForge」，配置写进本机 `config.json`。

之后随时可以在 ⚙️ **设置** 里换 Key / 请求地址 / 模型（点「🔍 检测模型」自动出列表），
或者打开 `/setup.html` 重新走一遍向导，**不需要重启服务器**。

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
