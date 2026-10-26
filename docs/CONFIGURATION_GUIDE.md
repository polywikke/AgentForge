# AgentForge 配置指南

> 本文档只描述**当前可执行程序真正读取的配置项**。
> 早期版本设计过更复杂的 `permissions` / `context` / `session` 等字段，运行时尚未使用，
> 写在 `config.json` 里会被忽略（不会报错）。
>
> 模板：[config.example.json](../config.example.json) ｜ 测试配置：[config.test.json](../config.test.json)

---

## 1. 配置文件位置与加载顺序

1. 命令行 `--config <path>` 指定；
2. 否则按顺序查找 `config.json`：当前目录 → exe 所在目录 → 上两级目录；
3. 找不到则使用内置默认值（API 未配置，Web UI 仍可打开）。

启动时日志会打印实际加载路径：

```
[Config] Loaded from ...\AgentForge\config.json
```

配置在**启动时**读入内存；通过网页 `/api/config` POST 修改会立即生效并写回文件。

---

## 2. 完整配置结构

```json
{
  "server": {
    "host": "127.0.0.1",
    "port": 8080,
    "web_root": "./web"
  },

  "api": {
    "provider": "custom",
    "endpoint": "https://your-proxy.com/v1/chat/completions",
    "key": "sk-...",
    "model": "your-model",
    "temperature": 0.7,
    "max_tokens": 4096
  },

  "agent": {
    "max_steps": 20,
    "time_limit_seconds": 300,
    "max_context_tokens": 100000,
    "max_total_tokens": 500000,
    "max_history_messages": 40,
    "max_repeat_calls": 3,
    "max_consecutive_errors": 3,
    "compaction": true,
    "llm_summary": false,
    "auto_memory": true,
    "max_subagent_depth": 2
  },

  "tools": {
    "sandbox": true,
    "work_dir": "",
    "dump_dir": ".af-dump",
    "persist_threshold": 20000,
    "permission_mode": "ask"
  },

  "retrieval": {
    "enabled": true,
    "top_k": 5
  },

  "systemPrompt": "你是 AgentForge ..."
}
```

所有字段都可省略（自动合并默认值）。

---

## 3. server

| 字段 | 默认 | 说明 |
|------|------|------|
| `host` | `127.0.0.1` | 监听地址；局域网访问可设 `0.0.0.0` |
| `port` | `8080` | 端口；也可用 `--port` 临时覆盖 |
| `web_root` | `./web` | 静态文件目录；相对路径会在 cwd / exe 目录 / 上两级中查找 |

---

## 4. api（核心）

| 字段 | 说明 |
|------|------|
| `provider` | `anthropic` / `openai` / `deepseek` / `custom` |
| `endpoint` | **完整**请求地址（含 `/v1/...` 路径） |
| `key` | API Key；`/api/config` 返回时脱敏为前 8 位 |
| `model` | 模型名 |
| `temperature` | 0~2；模型不支持非默认值时会自动去掉重试 |
| `max_tokens` | 单次回复上限 |

### 4.1 各提供商端点

| provider | endpoint | 示例模型 |
|----------|----------|----------|
| anthropic | `https://api.anthropic.com/v1/messages` | `claude-3-5-sonnet-20241022` |
| openai | `https://api.openai.com/v1/chat/completions` | `gpt-4o` |
| deepseek | `https://api.deepseek.com/v1/chat/completions` | `deepseek-chat` |
| custom | 由服务商提供 | 由服务商提供 |

### 4.2 provider 如何决定请求格式

- `anthropic` → Anthropic Messages API（`x-api-key` + `anthropic-version`，system 独立字段，工具用 `tool_use`/`tool_result`）
- `openai` / `deepseek` / `custom` → OpenAI Chat Completions（`Authorization: Bearer`，工具用 `tool_calls` + `role=tool`）
- `custom` 还会根据 URL 猜测：含 `anthropic` → Anthropic 格式，否则按 OpenAI 兼容处理。

> 中转站绝大多数是 OpenAI 兼容；若返回 404/400，先确认 endpoint 是否包含完整路径。

### 4.3 直接 curl 测试

```bash
curl -s -X POST http://127.0.0.1:8080/api/execute \
  -H "Content-Type: application/json" \
  -d "{\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}]}"
```

---

## 5. agent（Agent Loop 闸门）

| 字段 | 默认 | 说明 |
|------|------|------|
| `max_steps` | 20 | 单轮最多「模型调用 + 工具执行」步数；0 = 不限 |
| `time_limit_seconds` | 300 | 单轮总时长上限；0 = 不限 |
| `max_context_tokens` | 100000 | 上下文窗口预算（分层组装/压缩）；0 = 不限（关闭预算触发压缩） |
| `max_total_tokens` | 500000 | 单轮累计 token 预算；0 = 不限 |
| `max_history_messages` | 40 | 发送给模型的历史消息上限；0 = 不限 |
| `max_repeat_calls` | 3 | 同一工具 + 同一参数的最大重复次数；0 = 不限 |
| `max_consecutive_errors` | 3 | 连续模型错误上限；0 = 不限 |
| `compaction` | true | 是否在调用模型前自动压缩上下文 |
| `llm_summary` | false | 压缩时是否用模型写摘要（true 会产生额外 token；false 用抽取式） |
| `auto_memory` | true | 任务成功后是否写入 MEMORY.md |
| `max_subagent_depth` | 2 | Task 子代理最大递归深度 |

`api.max_tokens=0` 表示不发送 `max_tokens` 字段，交给模型默认/最大输出。

达到步数上限时仍会返回已完成的内容，`done.outcome` 为 `max_steps`。

---

## 6. tools（工具与沙箱）

| 字段 | 默认 | 说明 |
|------|------|------|
| `sandbox` | true | `true`：Bash 通过 Job Object 沙箱执行；`false`：Bash 返回演示文本 |
| `work_dir` | 空（项目根目录） | 沙箱允许的工作目录；命令在该目录下执行 |
| `dump_dir` | `.af-dump` | 大工具结果落盘目录 |
| `persist_threshold` | 20000 | 超过该字符数的工具结果落盘（Read 豁免） |
| `permission_mode` | `ask` | `ask` 弹窗 / `allow_all` / `deny_all`；显式 deny 规则始终优先 |

沙箱行为（`sandbox_win.cpp`）：

- 通过 `cmd.exe /S /C` 执行，捕获 stdout+stderr，输出转 UTF-8；
- **Job Object**：KILL_ON_JOB_CLOSE、进程数上限 16、内存上限、超时 `TerminateJobObject`；
- 黑名单：`format`、`diskpart`、`bcdedit`、`shutdown`、`reboot`、`rd /s /q c:\` 等；
- 路径校验：工作目录 + `allowed_dirs` 内允许，`blocked_paths` 优先拒绝。

权限规则保存在 `.af-permissions.json`，格式：

```json
{"version":1,"rules":[
  {"tool":"Write","arg_prefix":"","decision":"allow","reason":"[saved] web"},
  {"tool":"Bash","arg_prefix":"rm","decision":"deny","reason":"[saved] dangerous"}
]}
```

- Deny 规则永远先于 Allow；
- `arg_prefix` 对 Bash 匹配命令前缀；
- 可在 Web 弹窗选“总是允许/总是拒绝”，也可手工编辑该文件后重启。

其它 5 个文件工具（Read/Write/Edit/Glob/Grep）直接操作文件系统，当前**不经过沙箱**。

### 6.1 路径写法

所有工具的 `path` 参数都支持：

- `%USERPROFILE%\Desktop\sort.py`（环境变量展开）
- `~\Desktop\sort.py`（`~` = 用户目录）
- 绝对路径 / 相对路径（相对于服务进程工作目录）

> "保存到桌面"请使用 `%USERPROFILE%\Desktop`，不要用 `C:\Users\Public\Desktop`
> （公共桌面需要管理员权限）。

如果写桌面/文档报 `Access denied (errno=13, win32=5)`，而同一程序复制到别处可以写，
通常是项目文件被外部沙箱标记为 Low 完整性级别，运行 `tools\fix_integrity.bat` 修复。

---

## 7. systemPrompt

自定义系统提示（可选）。不填时使用内置提示：说明 6 个工具、使用规则，
以及 Windows 路径约定（桌面 = `%USERPROFILE%\Desktop`、失败不要擅自改路径）。

---

## 8. 网页端修改配置

`POST /api/config` 支持三种写法（都会合并到当前配置并写盘）：

**1) 只改 api 子字段**

```json
{ "api": { "endpoint": "https://...", "key": "sk-...", "model": "gpt-4o" } }
```

**2) 扁平字段**

```json
{ "endpoint": "https://...", "apiKey": "sk-...", "model": "gpt-4o" }
```

**3) 嵌套 + 其它模块**

```json
{ "api": { "temperature": 0.3 }, "agent": { "max_steps": 10 } }
```

注意：

- `api.key` 传空字符串表示「保持原值」，不会被清空；
- `/api/config` GET 返回的 key 是脱敏值，**不要**把脱敏值再 POST 回去（本程序会识别并保留原值）。

### 8.1 权限 / 会话 / 指标 API

| 方法 | 路径 | 用途 |
|------|------|------|
| POST | `/api/models` | 探测 endpoint 的模型列表（不落盘，供设置页下拉选择） |
| GET/POST | `/api/permissions` | 查看规则；`{"action":"reset"}` 重置；或 `{tool,arg_prefix,decision}` 手工加规则 |
| POST | `/api/permission` | 权限弹窗回传：`{id,decision: allow_once/always_allow/deny_once/always_deny}` |
| GET/POST | `/api/sessions` | 会话列表 / 新建 |
| GET | `/api/sessions/{id}` | 会话消息（role=user/assistant/tool） |
| GET | `/api/sessions/{id}/checkpoints` | 文件检查点列表 |
| POST | `/api/sessions/{id}/restore` | `{checkpoint_id}` 回滚文件 |
| GET/POST | `/api/memory` | 长期记忆 facts / 追加 |
| POST | `/api/index` | 重建 tree-sitter 代码地图 |
| GET | `/api/repo-map` | 地图文本 |
| GET | `/api/metrics` | 成本/上下文/压缩/会话/仓库指标 |
| GET | `/api/trace` | 最近一次 Trace（span/event） |
| POST | `/api/cancel` | `{request_id}` 取消运行中的 Agent |

---

## 9. 常见配置问题

| 现象 | 原因 / 处理 |
|------|-------------|
| 启动提示 `API key/endpoint not configured` | `api.key` 或 `api.endpoint` 为空 |
| HTTP 400 `... does not support non-default temperature` | 已自动重试；仍失败请检查模型 |
| HTTP 404 | endpoint 少了 `/v1/chat/completions` 等路径 |
| HTTP 401 | Key 错误 / 与 endpoint 不匹配 |
| 工具返回「演示模式」 | `tools.sandbox=false`，Bash 不真正执行 |
| 中文路径写入失败 | 请使用包含本轮修复的最新 exe |
| `No credentials are available...` | Windows Schannel 在受限令牌下不可用；换普通用户会话 |

---

## 10. 安全建议

1. `config.json` 含明文 Key，已在 `.gitignore` 中；共享项目前请清空。
2. 不要把 `host` 设为 `0.0.0.0` 后暴露到公网（没有鉴权）。
3. `Bash` 是高危工具，沙箱只做基本防护；不要让它执行不可信输入拼接出来的命令。
4. 定期轮换 API Key。
