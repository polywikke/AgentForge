#!/usr/bin/env python3
# ============================================================================
# Mock OpenAI-compatible LLM server (用于 AgentForge 端到端测试, 无需外网)
#
# 支持:
#   POST /v1/chat/completions   (stream=true / false)
#   GET  /v1/models             (返回 mock 模型列表, 用于验证“自动检测模型”)
#   GET  /health
#
# 行为:
#   - 用户消息包含 "读取"/"read" 且尚无工具结果 -> 返回 Read 工具调用
#   - 已有工具结果 -> 基于工具输出生成最终回答
#   - 其他 -> 普通对话, 并回显历史轮数 (用于验证对话历史)
# ============================================================================
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18081


def build_reply(body):
    messages = body.get("messages", [])
    tools = body.get("tools") or []
    tool_msgs = [m for m in messages if m.get("role") == "tool"]
    user_msgs = [m for m in messages if m.get("role") == "user"]
    last_user = user_msgs[-1]["content"] if user_msgs else ""
    tool_names = {t.get("function", {}).get("name") for t in tools}

    # 写桌面测试: 验证 Write 工具的 %USERPROFILE% 展开与跨目录写权限
    if "写桌面测试" in last_user and "Write" in tool_names and not tool_msgs:
        return {
            "kind": "tool_call",
            "name": "Write",
            "arguments": json.dumps(
                {"path": "%USERPROFILE%\\Desktop\\af_server_desktop.txt",
                 "content": "written by AgentForge server"},
                ensure_ascii=False),
            "content": "写入桌面测试文件。",
        }

    # 权限拒绝测试: Bash(rm)
    if "删除测试" in last_user and "Bash" in tool_names and not tool_msgs:
        return {
            "kind": "tool_call",
            "name": "Bash",
            "arguments": json.dumps({"command": "rm test.txt"}, ensure_ascii=False),
            "content": "我来删除测试文件。",
        }

    # 慢速响应，用于测试取消 (L6)
    if "慢速测试" in last_user:
        return {"kind": "text", "content": "慢速响应开始。" + ("这是用于测试取消的长文本。" * 120)}

    # 并行子代理测试: 触发 Task 工具
    if "并行调研" in last_user and "Task" in tool_names and not tool_msgs:
        return {
            "kind": "tool_call",
            "name": "Task",
            "arguments": json.dumps({
                "goals": [
                    "总结 README.md 的项目定位（只读调研）",
                    "总结 config.json 的配置结构（只读调研）"
                ],
                "max_concurrent": 2
            }, ensure_ascii=False),
            "content": "我派两个只读子代理并行调研。",
        }

    wants_read = ("读取" in last_user or "read" in last_user.lower()) and "Read" in tool_names
    if wants_read and not tool_msgs:
        return {
            "kind": "tool_call",
            "name": "Read",
            "arguments": json.dumps({"path": "config.json"}, ensure_ascii=False),
            "content": "我先读取 config.json。",
        }

    # 反复调用同一工具，用于验证 AgentLoop 的 "重复调用闸门"
    if "循环测试" in last_user and "Read" in tool_names:
        return {
            "kind": "tool_call",
            "name": "Read",
            "arguments": json.dumps({"path": "config.json"}, ensure_ascii=False),
            "content": "再次读取 config.json。",
        }

    if "写入" in last_user and "Write" in tool_names and not tool_msgs:
        return {
            "kind": "tool_call",
            "name": "Write",
            "arguments": json.dumps(
                {"path": "test_output/中文测试.txt", "content": "你好，AgentForge 写入测试"},
                ensure_ascii=False),
            "content": "我来写入测试文件。",
        }

    if ("执行" in last_user or "bash" in last_user.lower()) and "Bash" in tool_names and not tool_msgs:
        return {
            "kind": "tool_call",
            "name": "Bash",
            "arguments": json.dumps({"command": "echo AgentForge sandbox test"},
                                    ensure_ascii=False),
            "content": "我来执行命令。",
        }

    if "搜索" in last_user and "Grep" in tool_names and not tool_msgs:
        return {
            "kind": "tool_call",
            "name": "Grep",
            "arguments": json.dumps({"pattern": "AgentForge", "path": "src"},
                                    ensure_ascii=False),
            "content": "我来搜索代码。",
        }

    if tool_msgs:
        raw = tool_msgs[-1].get("content", "")
        preview = raw[:400]
        return {
            "kind": "text",
            "content": "工具执行完成，结果预览：\n" + preview,
        }

    return {
        "kind": "text",
        "content": "你好，我是 Mock LLM。本次请求共包含 {} 条用户消息，历史上下文{}。".format(
            len(user_msgs), "已保留" if len(user_msgs) > 1 else "为首轮"),
    }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[mock] " + (fmt % args) + "\n")

    def do_GET(self):
        if self.path == "/health":
            payload = json.dumps({"status": "ok"}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        elif self.path in ("/v1/models", "/models"):
            # 让“自动检测模型”可以在离线环境里跑通
            payload = json.dumps({
                "object": "list",
                "data": [
                    {"id": "mock-model", "object": "model"},
                    {"id": "mock-model-mini", "object": "model"},
                ],
            }, ensure_ascii=False).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_error(404)
            return

        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length) or b"{}")
        reply = build_reply(body)
        model = body.get("model", "mock-model")

        if body.get("stream"):
            self.stream_reply(reply, model)
        else:
            self.json_reply(reply, model)

    def json_reply(self, reply, model):
        message = {"role": "assistant", "content": reply.get("content", "")}
        finish = "stop"
        if reply["kind"] == "tool_call":
            message["tool_calls"] = [{
                "id": "call_mock_1",
                "type": "function",
                "function": {"name": reply["name"], "arguments": reply["arguments"]},
            }]
            finish = "tool_calls"

        payload = json.dumps({
            "id": "chatcmpl-mock",
            "object": "chat.completion",
            "model": model,
            "choices": [{"index": 0, "message": message, "finish_reason": finish}],
            "usage": {"prompt_tokens": 120, "completion_tokens": 40, "total_tokens": 160},
        }, ensure_ascii=False).encode()

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def stream_reply(self, reply, model):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        def send_chunk(obj):
            data = ("data: " + json.dumps(obj, ensure_ascii=False) + "\n\n").encode()
            self.wfile.write(("%x\r\n" % len(data)).encode() + data + b"\r\n")
            self.wfile.flush()

        def base(delta, finish=None):
            return {
                "id": "chatcmpl-mock",
                "object": "chat.completion.chunk",
                "model": model,
                "choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
            }

        send_chunk(base({"role": "assistant", "content": ""}))

        if reply["kind"] == "tool_call":
            send_chunk(base({
                "tool_calls": [{
                    "index": 0,
                    "id": "call_mock_1",
                    "type": "function",
                    "function": {"name": reply["name"], "arguments": ""},
                }]
            }))
            time.sleep(0.05)
            send_chunk(base({
                "tool_calls": [{
                    "index": 0,
                    "function": {"arguments": reply["arguments"]},
                }]
            }))
            time.sleep(0.05)
            send_chunk(base({}, "tool_calls"))
        else:
            text = reply.get("content", "")
            for i in range(0, len(text), 8):
                send_chunk(base({"content": text[i:i + 8]}))
                time.sleep(0.03)
            send_chunk(base({}, "stop"))

        send_chunk({
            "id": "chatcmpl-mock",
            "object": "chat.completion.chunk",
            "model": model,
            "choices": [],
            "usage": {"prompt_tokens": 120, "completion_tokens": 40, "total_tokens": 160},
        })
        done = b"data: [DONE]\n\n"
        self.wfile.write(("%x\r\n" % len(done)).encode() + done + b"\r\n")
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()


if __name__ == "__main__":
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print("[mock] listening on http://127.0.0.1:%d" % PORT, flush=True)
    server.serve_forever()
