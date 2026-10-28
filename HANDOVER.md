# AgentForge 项目交接文档

> **状态**: ✅ L1-L12 全阶段功能已实现并集成到 Web 主程序；编译、自检、Mock/真实 API 端到端验证通过
> **版本**: v2.0（2026-10-26）
> **主交付**: `bin/Release/agentforge_web.exe`（构建后生成）
> **验证记录**: [docs/VERIFICATION.md](docs/VERIFICATION.md) ｜ **矩阵**: [docs/L1-L12_MATRIX.md](docs/L1-L12_MATRIX.md)

---

## 1. 一句话结论

项目从"L1-L6 可用、L7-L12 只有骨架"升级为 **L1-L12 全部接入主流程**：
工具五步管线、六道闸门、Job Object 沙箱、权限弹窗/持久化、五级压缩、tree-sitter 代码地图、
JSONL 会话/检查点/三层记忆、Task 并行子代理、Trace/成本/上下文仪表盘都已实现并验证。

## 2. 立即上手

```bat
cd AgentForge
quick_build.bat      :: 自动找 CMake/VS（无需开发者命令行），首次编译 tree-sitter，1-3 分钟
run_server.bat       :: 启动并自动打开浏览器（exe 不存在时会先自动编译）
```

浏览器：<http://127.0.0.1:8080/>（端口被占用自动顺延）。
**第一次运行**会自动进入 `/setup.html` 向导：选服务商 → 填请求地址 → 填 API Key →
自动检测模型名 → 保存并进入工作台。之后在右上角 ⚙️ 设置里可随时换 Key / 请求地址 / 模型。

要求：CMake ≥ 3.15（或直接用 VS 自带的 CMake）+ VS 2022/2026（C++ 桌面工作负载）或 MinGW；
首次编译会编译 `thirdparty/tree-sitter`（约 25 MB）。

自检/测试：

```powershell
bin\Release\agentforge_v2.exe --selftest          # 15 项
powershell -ExecutionPolicy Bypass -File tools\smoke_test.ps1
python tools\mock_llm_server.py 18081             # 离线端到端
bin\Release\agentforge_web.exe --config config.test.json --port 18080
```

## 3. 本版新增/重构（相对 v1.0）

### 3.1 L4 工具管线

- `ToolRegistry::execute`：Schema 校验 → 权限 → 执行 → UTF-8 消毒 → 大结果落盘。
- Read 对 `key/token/secret/sk-*` 脱敏；Read 豁免落盘。
- `readOnlyView()` 供子代理使用。

### 3.2 L5/L7 上下文

- 新增 `include/context_manager.hpp`：L0 system/L1 项目知识/L2 代码地图/L3 检索/L5 历史/L6 用户输入。
- 五级压缩：L1 单条降级、L2 折叠旧工具结果、L3 摘要（LLM/抽取式）、L4 丢老消息；L0 由工具落盘。
- `sanitizeToolPairs` 消息合法性守卫。
- Anthropic 流式 `chatStreamAnthropic`。

### 3.3 L6 安全

- 六道闸门：步数/token/墙钟/连续错误/重复调用/取消。
- `/api/cancel` + `atomic<bool>`；前端停止按钮。
- Bash 沙箱升级为 **Job Object**（KILL_ON_JOB_CLOSE + 进程数/内存上限 + 超时 TerminateJobObject）。
- **工作区边界**：`tools.workspace_root`（空 = 对话 `workspace/`）；Write/Edit/Bash 受限制，Read/Glob/Grep 可读项目。
- 新增 `include/permission_gate.hpp`：`.af-permissions.json` 持久化、Bash 前缀规则、Deny 优先；
  Web 权限弹窗（允许一次/总是允许/拒绝/总是拒绝）。

### 3.4 L8 代码地图

- 引入 `thirdparty/tree-sitter`（0.25 + cpp grammar）和 `symbol_extract.cpp`（真解析 + 词法兜底）。
- `RepoIndex`：符号/签名、include 依赖图、PageRank、活动文件偏置、预算渲染、三信号检索。
- 地图注入 L2、检索注入 L3。

### 3.5 L10 会话/记忆

- `SessionStore`：每次对话一个目录 `conversations/<YYYYMMDD_HHMMSS_主题>/`
  （`README.md`、`meta.json`、`snapshot.json`、`events.jsonl`、`checkpoints/`、
  `trace.jsonl`、`artifacts/manifest.jsonl`、`workspace/`）。
- 文件检查点/恢复（Write/Edit 前快照）。
- 三层记忆：AGENTS.md/CLAUDE.md + MEMORY.md（自动抽取、去重、启动加载）。
- Web 会话侧栏、`/api/sessions`、`/api/memory`。

### 3.6 L11 子代理

- `Task(goals, max_concurrent)` 工具；`SubagentRunner` 并行、只读工具、NoopUI、maxDepth、独立 Usage 汇总。

### 3.7 L12 可观测性

- `JsonlTracer`（span/event、环形缓冲、JSONL、线程安全）。
- `/api/metrics`、`/api/trace`；Web 指标面板（成本/上下文/压缩/会话）。
- CLI：`--cost/--context/--regress/--selftest`。

### 3.8 L9 Web UI

- 权限弹窗、停止按钮、会话侧栏、指标面板；保留原有流式/工具卡片/设置。

## 4. 架构

```
web/index.html ──SSE──> main_web_simple.cpp
                          │
       ┌──────────────────┼───────────────────┐
       ▼                  ▼                   ▼
 ContextManager       AgentLoop           JsonlTracer
 (分层/压缩)          (六道闸门)          (/api/metrics)
       │                  │
       │         ┌────────┴────────┐
       ▼         ▼                 ▼
   RepoIndex  ToolRegistry     PersistentPermissionGate
 (tree-sitter)(校验/权限/落盘)   (.af-permissions.json)
                 │
                 ├── Read/Write/Edit/Glob/Grep
                 ├── Bash → WindowsSandbox(Job Object)
                 └── Task → SubagentRunner(并行只读)
```

关键契约：`types.hpp`、`model_provider.hpp`、`tool_registry.hpp`、`sandbox.hpp`、
`loop_trace.hpp`、`context_manager.hpp`、`permission_gate.hpp`。

## 5. 验证摘要

| 项目 | 结果 |
|------|------|
| 编译 3 目标（含 tree-sitter） | ✅ 0 error |
| `agentforge_v2 --selftest` | ✅ 15/15 |
| 冒烟（health/静态页/配置/权限/会话/记忆/指标/地图） | ✅ PASSED |
| Mock：对话/Read（脱敏）/Write/Bash/Task/压缩/取消/检查点/权限 | ✅ |
| 真实 API：非流式 + SSE + Read 工具 | ✅ |
| 权限弹窗浏览器实测 | ✅ `modalSeen=true`，允许后写入成功 |
| 检查点恢复 | ✅ 文件从损坏内容恢复 |
| 压缩 | ✅ 60,807 → 6,119 tokens（L3） |

## 6. 已知限制（非阻断）

1. **L9 用 Web 替代 FTXUI**：如果课程强制 TUI，需要另写 FTXUI 后端；当前 Web 功能更完整。
2. **tree-sitter 仅 C/C++**：其他语言走词法兜底；多文法可后续扩展。
3. **工作区是程序级边界**：Write/Edit/Bash 已受 `workspace_root` 限制，Read/Glob/Grep 仍可读项目；
   但这不是 OS 级文件系统沙箱，Bash 仍可能通过未识别的路径写法绕过，强隔离需 Windows Sandbox/容器。
4. **会话恢复是"继续历史"**，不是进程级自动重放；检查点只回滚文件。
5. **Anthropic 流式**未用真实 Anthropic Key 验收（无 Key），代码已实现并在失败时回退非流式。
6. **插件 ABI / git diff / 向量嵌入**未实现（超出本轮 L1-L12 主线，见矩阵文档）。
7. 构建中间目录 `build/`、测试产物、旧截图和重复的 `docs/QUICK_START.md` 已清理；
   重新构建请运行 `quick_build.bat`。

## 7. 常见问题

**Q: 首次编译很慢？** 需要编译 tree-sitter C 源码（约 25MB），1-3 分钟；之后增量。

**Q: 工具执行前弹权限窗？** `tools.permission_mode=ask` 是默认；可在弹窗选"总是允许"，或改成
`allow_all`（显式 deny 仍优先）。

**Q: 保存到桌面失败？** 用 `%USERPROFILE%\Desktop`；若报 Access denied 且复制到别处可写，
运行 `tools\fix_integrity.bat`（Low IL 问题）。

**Q: HTTPS 报 Schannel 无凭据？** 受限令牌/沙箱进程限制；用普通用户会话运行。

## 8. 文档

1. 本文档 — 交接
2. `README.md` — 安装/配置/API
3. `docs/L1-L12_MATRIX.md` — 逐阶段实现矩阵
4. `docs/VERIFICATION.md` — 验证记录
5. `docs/CONFIGURATION_GUIDE.md` — 配置项
6. `CHANGELOG.md` — v2.0 变更
7. `docs/images/` — UI 截图

**项目现在可以编译、演示和提交。**
