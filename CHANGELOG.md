# AgentForge 更新日志

记录项目的重要更新和版本变更。

---

## [v2.0] - 2026-10-26 —— L1-L12 全阶段实现

> 从"L1-L6 可用、L7-L12 骨架"升级为 L1-L12 全部接入 Web 主流程。
> 逐阶段矩阵见 [docs/L1-L12_MATRIX.md](docs/L1-L12_MATRIX.md)。

### L4 工具管线

- `ToolRegistry::execute` 五步：Schema 校验 → 权限 → 执行 → UTF-8 消毒 → 大结果落盘。
- Read 对 key/token/secret/sk-* 脱敏；按 Read 豁免 `.af-dump` 落盘。
- `readOnlyView()` 只读工具视图（子代理用）。

### L5/L7 上下文与压缩

- 新增 `ContextManager`：L0 system / L1 项目知识 / L2 代码地图 / L3 检索 / L5 历史 / L6 输入。
- 五级压缩：L1 单条降级、L2 折叠旧工具结果、L3 LLM/抽取式摘要、L4 丢老消息；L0 大结果落盘。
- `sanitizeToolPairs` 消息合法性守卫。
- Anthropic 流式 `chatStreamAnthropic`（失败回退非流式）。

### L6 安全

- 六道闸门：步数/token/墙钟/连续错误/重复调用/取消。
- `/api/cancel` + 前端停止按钮。
- Bash 沙箱升级 **Job Object**（KILL_ON_JOB_CLOSE、进程数/内存上限、超时 TerminateJobObject）。
- 新增 `PersistentPermissionGate`：`.af-permissions.json` 持久化、Bash 前缀规则、Deny 优先；
  Web 权限弹窗（允许一次/总是允许/拒绝/总是拒绝）。

### L8 代码地图

- 引入 `thirdparty/tree-sitter`（0.25 + cpp grammar）与 `symbol_extract.cpp`（真解析 + 词法兜底）。
- `RepoIndex`：符号/签名、include 依赖图、PageRank、活动文件偏置、预算渲染、三信号检索。
- 地图注入 L2、检索注入 L3。

### L10 会话/记忆

- `SessionStore`：`sessions/<id>/{meta,snapshot,events.jsonl,checkpoints}`。
- Write/Edit 前文件检查点；`/restore` 回滚（已实测）。
- 三层记忆：AGENTS.md/CLAUDE.md + MEMORY.md（自动抽取、去重、启动加载）。
- Web 会话侧栏 + `/api/sessions` + `/api/memory`。

### L11 子代理

- `Task(goals, max_concurrent)` 工具；并行 `SubagentRunner`、只读工具、NoopUI、maxDepth、独立 Usage 汇总。

### L12 可观测性

- `JsonlTracer`：span/event、环形缓冲、JSONL、线程安全。
- `/api/metrics`、`/api/trace`；Web 指标面板（成本/上下文/压缩/会话）。
- `agentforge_v2 --cost/--context/--regress/--selftest`（15 项自检）。

### L9 Web UI

- 权限弹窗、停止按钮、会话侧栏、指标面板；保留流式、工具卡片、设置。

### 验证

- 3 目标编译通过（含 tree-sitter）。
- selftest 15/15 PASS；smoke PASSED。
- Mock：对话/Read/Write/Bash/Task/压缩/取消/检查点/权限。
- 真实 API：非流式 + SSE + Read。
- 权限弹窗浏览器实测（CDP）。

---

## [v1.0.1] - 2026-10-26 —— 稳定性增强 + 项目精简

### 增强

- **历史裁剪**：新增 `agent.max_history_messages`（默认 40），发送给模型前只保留最近 N 条非 system
  消息，且从 `user` 消息开始，避免上下文无限增长；`/api/execute` 与 `/api/execute-stream` 同时生效。
- **重复调用闸门**：新增 `agent.max_repeat_calls`（默认 3），同一「工具 + 参数」重复超过上限立即中止，
  防止模型原地打转消耗 token。
- **错误事件去重**：AgentLoop 不再通过 UI 端口重复发错误，SSE 每轮只返回一个 `error`。
- **tool_call id 兜底**：模型/中转站未返回 id 时自动生成，保证 OpenAI 工具消息协议合法。
- **脱敏 Key 防护**：`POST /api/config` 识别 `sk-abc...` / `***` 等脱敏值并忽略，避免把真实 Key 写坏。
- **配置模板**：`config.example.json` / `config.test.json` 补充 `max_history_messages`、`max_repeat_calls`。
- **桌面/路径写入修复**：
  - 所有工具路径支持 `%USERPROFILE%`、`%TEMP%`、`~` 展开（新增 `win_utf8.hpp: resolveUserPath`）。
  - 默认 system prompt 明确"桌面 = `%USERPROFILE%\Desktop`"、写入失败不得擅自改路径。
  - `Write` 失败返回绝对路径 + `errno/win32` + 修复建议。
  - `config.json` 默认 `tools.sandbox=true`，Bash 可真实执行。
  - 新增 `tools/fix_integrity.bat`：修复外部沙箱把项目标记为 Low 完整性级别导致的"拒绝访问"。
- **验证**：真实模型把排序程序成功写入 `C:\Users\17102\Desktop\agentforge_sort.py`（4002 字节）。

### 精简（删除 42 个文件 + 3 个运行目录 + build 内容）

- 未参与构建的重复实现：`src/main_web.cpp`、`main_v2_backup/fixed/original.cpp`、
  `execute_stream_v2.cpp`、`test_compile.cpp`、`curl_transport.*`、`winhttp_transport.cpp`、
  `wininet_*`、`http_server.*`
- 旧文档：`PROJECT_STATUS.md`、`COMPLETION_REPORT.md`、`DELIVERY_REPORT.md`、`FINAL_REPORT.md`、
  `INTEGRATION_GUIDE.md`、`PROGRESS_REPORT.md`、`CURRENT_STATUS_SUMMARY.md`、`WEB_SERVER_*.md`、
  `项目实现总结*.md`，以及 `docs/` 下 10 篇过时技术文档
- 旧脚本：`build.ps1`、`rebuild.ps1`、`rebuild.sh`、`CMakeLists_old.txt`
- 前端无用文件：`web/app.js`、`web/api-test.html`、`test_api.html`
- 运行产物：`logs/`、`sessions/`、`tests/`，以及 `build/` 下全部中间文件

> 残留：`src/wininet_http_transport.cpp` 与空 `build/` 目录被高权限的陈旧
> `cl.exe` / `Tracker.exe` 占用，非管理员无法删除；重启后可删，不影响编译运行。

### 验证

- 100 条历史消息 → 实际发送 40 条（20 条 user），Mock 确认。
- 强制循环工具调用 → 第 4 次触发闸门，SSE 只返回一个 `error`。
- 删除旧文件后，用全新构建目录从零配置编译三目标 → 0 error / 0 warning。
- 冒烟测试 PASSED；真实 API（8080）对话返回正常。

---

## [v1.0] - 2026-10-26 —— 编译打通 & 端到端可用

> 从「代码写完但编译不过」到「三个可执行文件全部编译成功，Web/SSE/工具/真实 API 全链路验证通过」。
> 详细测试记录见 [docs/VERIFICATION.md](docs/VERIFICATION.md)。

### 修复（编译阻断）

- **统一 HTTP 契约**：删除 `http_transport.hpp` 与 `model_provider.hpp` 中重复且成员不一致的
  `HttpRequest/HttpResponse/IHttpTransport`，只保留 `model_provider.hpp` 一份。
- **重写 `SimpleHTTPTransport`**：真正实现 WinHTTP 同步请求 + SSE 流式逐行回调
  （不再依赖 `main_web_simple.cpp` 里的 `httpPost` 外部函数）。
- **统一沙箱接口**：`ISandbox` 只保留 `execute / isPathAllowed / getStatus`；
  `sandbox_win.cpp` 重写为真正的 Windows 实现（CreateProcess + 超时 + 危险命令黑名单 + 路径白名单）；
  删除 `ui_impl.cpp` 中重复的 `createWindowsSandbox` 符号。
- **修正 `ChatResponse` 使用**：错误响应改用 `ChatResponse::is_error/error_message/message`。
- **重写 `main_web_simple.cpp`**：修正命名空间（`ToolRegistry` 为全局）、`APIConfig` 字段、
  `SSEUIAdapter` 基类、`AgentLoop` 构造与 `LoopStats` 使用。
- **CMake 目标整理**：`agentforge` / `agentforge_v2` / `agentforge_web` 各自源文件不再重复，
  避免 `main.cpp` 直接 `#include *.cpp` 导致的重复符号。
- **SSEUIAdapter**：修正 `ToolResult.status`（不存在）为 `!result.is_error`。

### 新增 / 增强

- **真实流式**：`MultiAPIProvider::chatStream` 支持 OpenAI 兼容 SSE（内容增量、tool_calls 增量聚合、
  usage 解析），失败时自动回退非流式；`AgentLoop` 通过 `IUIPort::streamDelta` 实时推送。
- **Function Calling 规范序列化**：OpenAI（assistant.tool_calls + role=tool）与
  Anthropic（tool_use / tool_result 内容块）双向转换，工具闭环可用。
- **多轮对话**：前端发送完整历史，服务端补充 system prompt，工具结果按 `call_id` 关联。
- **temperature 兼容**：模型返回 “does not support non-default temperature” 时自动去掉该字段重试。
- **UTF-8 中文路径**：新增 `include/win_utf8.hpp`，所有路径/命令/文件 IO 统一 UTF-8 ↔ 宽字符转换；
  修复 httplib 静态目录挂载、中文文件名写入、沙箱工作目录乱码。
- **SSE 正常结束**：调用 `sink.done()` 写入 chunked 结束标记，修复浏览器 “network error” /
  curl exit 18 (partial file)。
- **Web UI**：工具卡片插入到回答之前（时间顺序）；消息与工具参数做 HTML 转义；
  `addToolCallCard` 支持 `beforeEl`。
- **脚本**：重写 `build_and_test.ps1`（vswhere 探测 VS、增量编译、产物校验、`-Run` 参数）、
  `quick_build.bat`（不再每次删 build）、新增 `run_server.bat`、
  `tools/smoke_test.ps1`、`tools/mock_llm_server.py`、`tools/check_web_ui.py`。

### 验证结论

- 编译：3 个目标 0 error（MSVC 14.44 / VS 2022 Build Tools）。
- 冒烟：health / 静态页 / tools / config 脱敏全部通过。
- 离线：Mock LLM 覆盖基础对话、历史、流式、Read/Write(中文)/Bash/Grep 全部通过。
- 真实：中转站 API 非流式 + SSE + 工具调用 + 历史问答全部通过；浏览器 UI 实测截图见 docs/images。

---

## [v0.9] - 2026-10-26

### 🎉 架构完成阶段

**核心成就**:
- ✅ 完成 L1-L12 所有模块的代码实现（~8000 行）
- ✅ 13 模块 3 层架构全部就位
- ✅ Web 界面和 HTTP 服务器完成
- ✅ 支持多 API 提供商（Anthropic/OpenAI/DeepSeek/Custom）

### 新增功能

**核心代码** (`include/`, `src/`):
- 完整的类型系统 (`types.hpp`)
- Agent Loop 核心循环 (`agent_loop.hpp`)
- 6 个核心工具 (`core_tools.cpp`)
- 多 API 提供商接口 (`multi_provider.hpp`)
- 安全沙箱机制 (`sandbox.hpp`)
- 会话持久化 (`session_store.hpp`)
- 可观测性系统 (`observability.hpp`)
- 仓库理解系统 (`repo_understanding.hpp`)
- 上下文管理 (`context.hpp`)
- 子代理系统 (`sub_agent.hpp`)

**Web 前端** (`web/`):
- 现代化暗色主题界面
- 实时 SSE 流式显示
- 消息正确分类（用户/状态/AI）
- 自动滚动和打字机效果
- Settings 配置页面

**HTTP 服务器** (`src/http_server.cpp`):
- 基于 cpp-httplib 的 RESTful API
- `/api/execute` 端点支持 SSE
- 静态文件服务
- CORS 支持

### 已知问题

❌ **核心闭环未打通**:
1. `HTTPModelProvider::chat()` 方法仅模拟回复，未真正调用 LLM API
2. Function Calling 协议未实现
3. 工具调用流程未完整打通
4. 对话历史管理缺失

### 技术债务

```cpp
// src/multi_provider.cpp
std::string HTTPModelProvider::chat(...) {
    // TODO: 实现真实的 HTTP 请求
    return "模拟回复"; // ❌ 当前只是假的
}
```

### 文档更新

- 新增 `PROJECT_STATUS.md` - 项目全面分析和开发计划
- 新增 `docs/CONFIGURATION_GUIDE.md` - 详细配置指南
- 新增 `docs/QUICK_START.md` - 新手快速上手
- 新增 `config.example.json` - 配置文件模板
- 更新 `README.md` - 添加项目状态链接

---

## [未来版本规划]

### v1.0 - 核心闭环打通 (计划 2-3 天)

**目标**: 让 AgentForge 真正能调用 LLM 并执行工具

**关键任务**:
- [ ] P0.1: 实现 HTTPModelProvider::chat() - 真实 API 调用
- [ ] P0.2: 实现 Function Calling 流程
- [ ] P0.3: 实现对话历史管理
- [ ] 测试基础任务：Read/Write/Bash

**验收标准**:
- ✅ 用户："读取 README.md"
- ✅ AI 调用 Read 工具，真正读取文件
- ✅ AI 返回文件内容
- ✅ 用户："文件有多少行？"
- ✅ AI 记住上一轮内容，直接回答

---

### v1.1 - UI 增强 (计划 1-2 天)

**目标**: 提升用户体验，可视化工具调用

**任务**:
- [ ] 工具调用卡片显示
- [ ] 代码语法高亮
- [ ] Token 统计显示
- [ ] 执行时长显示
- [ ] 错误提示优化
- [ ] 一键复制代码块

---

### v1.2 - 功能完善 (计划 2-3 天)

**目标**: 完整的会话管理和仓库理解

**任务**:
- [ ] 会话保存/加载
- [ ] 多会话管理
- [ ] 导出对话
- [ ] 仓库索引 UI
- [ ] 设置面板优化
- [ ] 快捷键支持

---

### v2.0 - 生产就绪 (计划 1 周)

**目标**: 可在生产环境使用的完整系统

**任务**:
- [ ] 完整的单元测试
- [ ] 集成测试
- [ ] 性能优化
- [ ] 安全加固
- [ ] Docker 部署
- [ ] CI/CD 流程
- [ ] 用户文档完善

---

## 版本历史

### 开发阶段 (2026-06 ~ 2026-10)

**L1-L2** (2026-06):
- 需求分析和架构设计
- 13 模块定义
- 核心类型契约

**L3** (2026-07):
- 实现 Agent Loop 基础框架
- 实现 MockModelProvider

**L4** (2026-07):
- 实现 Read 工具
- 完成工具注册系统

**L5** (2026-08):
- 实现 Write/Edit 工具
- 实现自主循环逻辑

**L6** (2026-08):
- 实现安全沙箱
- 路径白名单
- 命令黑名单

**L7** (2026-09):
- 实现上下文管理
- Token 预算和压缩

**L8** (2026-09):
- 实现仓库理解
- 符号索引
- 依赖图分析

**L9** (2026-09):
- 实现 Web UI
- SSE 流式响应

**L10** (2026-10):
- 实现会话持久化
- 历史记录管理

**L11** (2026-10):
- 实现子代理系统
- 并发管理

**L12** (2026-10):
- 实现可观测性
- 日志、追踪、监控

**v0.9** (2026-10-26):
- 架构完成
- 代码整合
- 文档完善

**v1.0** (2026-10-26):
- 编译打通（3 个目标）
- 真实 LLM 调用 / Function Calling / 对话历史闭环
- SSE 流式 + 工具卡片 + 中文路径
- 端到端验证完成
- **当前版本** ← 你在这里

---

## 贡献者

感谢所有为 AgentForge 做出贡献的人！

---

## 许可证

本项目为教学项目，遵循 MIT 许可证。

---

**文档版本**: v1.0  
**最后更新**: 2026-10-26
