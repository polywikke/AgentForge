# AgentForge L1-L12 实现矩阵

> 对应课程 L1-L12 阶段；状态以当前代码与验证记录为准。
> 验证记录见 [VERIFICATION.md](VERIFICATION.md)，交接见 [../HANDOVER.md](../HANDOVER.md)。

---

## 总览

| 阶段 | 课程主题 | 状态 | 主要落点 |
|------|----------|------|----------|
| L1 | 需求与总体设计 | ✅ | 13 模块分层、W1-W4、端口/适配器、契约不变式 |
| L2 | 模块设计与核心契约 | ✅ | `types.hpp`、`model_provider.hpp`、`tool_registry.hpp`、`sandbox.hpp`、`loop_trace.hpp` |
| L3 | v0 能对话 | ✅ | 双协议 Provider、SSE、WinHTTP 传输、重试/超时 |
| L4 | v1 能读 | ✅ | 工具五步管线、Read/Glob/Grep、Schema 校验、权限、落盘 |
| L5 | v2/v3 能改能自主循环 | ✅ | AgentLoop、ContextManager 分层、Write/Edit/Bash、消息守卫 |
| L6 | v4 能刹车 | ✅ | 六道闸门、取消、Job Object 沙箱、权限弹窗/持久化 |
| L7 | v5 有记忆 | ✅ | 五级压缩、预算、LLM/抽取摘要、Anthropic 流式 |
| L8 | v6 有地图 | ✅ | tree-sitter + 词法兜底、依赖图、PageRank、地图注入、检索 |
| L9 | v7 有面目 | ✅ | Web UI：流式、工具卡片、Auto/Ask/Plan 模式、权限弹窗、停止、会话、指标 |
| L10 | v8 能存档 | ✅ | JSONL 会话、检查点/恢复、三层记忆 |
| L11 | v9 会分身 | ✅ | Task 并行只读子代理、深度限制、成本汇总 |
| L12 | v10 有仪表盘 | ✅ | JsonlTracer、`/api/metrics`、`--cost/--context/--regress` |

> **与课程参考实现的差异**
> - L9 用 Web UI 替代 FTXUI TUI（功能等价，且权限弹窗/停止/指标更易演示）。
> - L8 符号抽取为 **tree-sitter 真解析 + 词法兜底**；未做跨语言多文法（仅 C/C++）。
> - L7 的 LLM 摘要默认关闭（`agent.llm_summary=false`），默认走零成本抽取式摘要；打开后走模型摘要。
> - L10 的会话恢复是“新 turn 继续历史”，不是进程级 `--resume` 自动重放；检查点支持文件回滚。

---

## L1 需求与总体设计

- 13 模块：核心框架/安全/可观测/配置/持久化、模型适配/仓库理解/上下文/增量状态/Agent 循环、工具/UI/测试。
- W1 上下文闭环：ContextManager 分层 + 压缩 + 预算。
- W2 Agent 循环：AgentLoop 六道闸门。
- W3 工具协议：ToolDef JSON Schema + tool_calls/tool_result。
- W4 流式：SSE 事件 + 前端增量渲染。
- 关键文件：`docs/L1-L12_MATRIX.md`、`HANDOVER.md`、`include/types.hpp`。

## L2 核心契约

| 端口 | 文件 | 说明 |
|------|------|------|
| `IModelProvider` | `model_provider.hpp` | 流式/非流式、Usage |
| `IHttpTransport` | `model_provider.hpp` | 同步 + 流式逐行回调 |
| `ToolRegistry` | `tool_registry.hpp` | 注册/执行/只读视图/Schema |
| `ISandbox` | `sandbox.hpp` | 沙箱执行/路径校验/状态 |
| `IPermissionGate` | `tool_registry.hpp` | allow/ask/deny |
| `IUIPort` | `tool_registry.hpp` | 事件、确认、权限弹窗 |
| `ILoopTracer` | `loop_trace.hpp` | span/event |
| `IContext` | `context_manager.hpp` | 分层/压缩/预算 |

## L3 能对话

- `MultiAPIProvider`：OpenAI 兼容 + Anthropic Messages API。
- `SimpleHTTPTransport`：WinHTTP，同步 + SSE 逐行。
- SSE UI：`chunk` 增量、`done` 统计。
- 校验：真实中转 API 非流式/流式均跑通；Anthropic 流式解析已实现（无 Anthropic Key 时回退非流式）。

## L4 能读（工具管线）

`ToolRegistry::execute` 五步：

1. Schema 校验（required + type）
2. 权限门（Allow/Ask/Deny；Ask → `IUIPort::confirmTool`）
3. 执行 ToolFunction
4. `sanitizeUtf8` 文本卫生
5. 大结果落盘 `.af-dump/`（Read 豁免，超阈值截断）

另含：`Read` 对 API Key/token 字段脱敏；Bash 退出码进结果；Edit 唯一性校验。

## L5 能改能自主循环

- `AgentLoop`：模型→工具→回写→再推理；最终 assistant 消息入历史。
- `ContextManager`：
  - L0 system / L1 项目知识 / L2 代码地图 / L3 检索 / L5 历史 / L6 用户输入。
  - `trimHistory` + `sanitizeToolPairs`（孤儿 tool 消息、配对窗口、半回填）。
- 工具：Read/Write/Edit/Glob/Grep/Bash。

## L6 能刹车

| 闸门 | 实现 |
|------|------|
| 1 步数 | `LoopLimits::max_steps` |
| 2 token | `accumulated_usage.total()` |
| 3 墙钟 | `max_duration_sec` |
| 4 连续错误 | `max_consecutive_errors` |
| 5 重复调用 | 工具名+参数签名计数 |
| 6 用户中断 | `/api/cancel` → `atomic<bool>` |

- 沙箱：`CreateProcess(SUSPENDED)` + `Job Object`（KILL_ON_JOB_CLOSE/ActiveProcessLimit/ProcessMemoryLimit）+ 超时 `TerminateJobObject`；危险命令黑名单、工作目录校验。
- 权限：`.af-permissions.json`；内建 Read/Glob/Grep allow、Write/Edit/Bash ask；Deny 优先；Web 弹窗可“允许一次/总是允许/拒绝/总是拒绝”。
- 注意：文件工具（Read/Write/Edit/Grep/Glob）不走沙箱的路径白名单（当前只对 Bash 生效）；这是已知取舍。

## L7 有记忆（压缩）

`ContextManager::compact` 级联：

| 级别 | 手段 |
|------|------|
| L0 | 工具大结果落盘（ToolRegistry） |
| L1 | 单条消息降级（>8000 字符截断） |
| L2 | 折叠旧工具结果（保留最近 3 条） |
| L3 | 老历史摘要（LLM 或抽取式） |
| L4 | 丢弃老消息（保留首条+最近窗口） |

- 预算：`BudgetConfig` 各层配额；`shouldCompact` 在 85% 触发。
- Anthropic 流式：`chatStreamAnthropic` 支持 text_delta / input_json_delta / tool_use / message_delta。

## L8 有地图

- `thirdparty/tree-sitter`（0.25 + cpp grammar）编译为静态库；`symbol_extract` 真解析 + 词法兜底。
- `RepoIndex`：
  - 符号：class/struct/enum/function/method + 签名；
  - 依赖：`#include "..."` / `<...>` → basename/相对路径解析；
  - PageRank：d=0.85，20 轮，活动文件偏置 ×8；
  - `renderMap(max_chars)` 按 rank 输出、按字符预算截断；
  - `search(query)` 符号/路径/内容三信号，供 L3 检索层。
- 注入：ContextManager 把地图放进 L2 层，检索结果放进 L3 层。

## L9 有面目（Web）

- `web/index.html`：暗色主题、流式渲染、工具调用卡片、Token/成本/上下文/压缩指标、会话侧栏、权限弹窗、停止按钮。
- **运行模式**（右上角切换）：
  - `Auto`：自主执行，Write/Edit/Bash 默认允许（显式 deny 仍优先）；
  - `Ask`：修改/执行前弹权限窗；
  - `Plan`：只读工具（Read/Glob/Grep），不写文件、不执行命令。
- **历史会话**：侧栏列出会话，点击加载消息并设置当前会话，继续发送沿用同一 `session_id`。
- SSE 事件：`start/stage/chunk/tool_call/tool_result/permission_request/done/error`。
- 说明：课程 L9 用 FTXUI；本项目用浏览器 UI，异步性由 HTTP 工作线程 + SSE 提供。

## L10 能存档

- 会话目录：`conversations/<id>/`（`README.md`、`meta.json`、`snapshot.json`、`events.jsonl`、`checkpoints/`、`trace.jsonl`、`artifacts/manifest.jsonl`）。
- API：`/api/sessions`、`/api/sessions/{id}`、`/checkpoints`、`/restore`。
- 检查点：Write/Edit 前快照目标文件；`restore` 回滚（已实测）。
- 三层记忆：
  - 会话历史（JSONL）
  - 项目知识 `AGENTS.md` / `CLAUDE.md`
  - 自动记忆 `MEMORY.md`（任务结束抽取、去重、启动加载）

## L11 会分身

- `Task(goals, max_concurrent)` 工具。
- `SubagentRunner`：
  - 独立 ContextManager + system prompt；
  - `readOnlyView()` 仅 Read/Glob/Grep，排除 Task（防递归）；
  - NoopUI（不弹窗）；
  - maxDepth（默认 2）；
  - 每子代理独立 Usage，汇总返回。
- 已实测 2 个子代理并行，返回合并摘要。

## L12 有仪表盘

- `JsonlTracer`：span/event、每线程 span 栈、环形缓冲（4096，dropped 可见）、JSONL 落盘。
- Web：`/api/metrics`（usage/cost/context/compactions/session/repo/tools）、`/api/trace`。
- CLI：
  - `agentforge_v2 --cost conversations`
  - `agentforge_v2 --context`
  - `agentforge_v2 --regress <file.jsonl>`（强制 pass/cost/steps 三字段）
  - `agentforge_v2 --selftest`（15 项）

---

## 未纳入本轮的功能（超出 L1-L12 阶段或依赖外部服务）

- 插件 DLL/ABI 管理器、C ABI 插件热加载（模块①，L1/L2 设计层）。
- 向量嵌入检索（L8 的 `L3_Retrieved` 当前用关键词/符号检索，未接 embedding/向量库）。
- git diff / 工作区增量状态（模块⑤）。
- 完整 TUI 后端（FTXUI）；当前只有 Web + 控制台文本。
- 多语言 tree-sitter 文法（仅 C/C++）。

这些不影响 L1-L12 的课程验收主线；如需可在后续扩展。
