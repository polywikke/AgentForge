#!/usr/bin/env python3
"""静态检查 web/index.html 与 web/setup.html 的 DOM/JS 引用是否自洽。

无需浏览器:
  - JS 里 getElementById('x') 的 id 必须存在
  - inline onclick/oninput 调用的函数必须有定义
  - 前端调用的 API 路径必须由服务端提供
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERVER_ENDPOINTS = {"/api/health", "/api/config", "/api/tools",
                    "/api/models", "/api/execute", "/api/execute-stream",
                    "/api/cancel", "/api/metrics", "/api/permission",
                    "/api/permissions", "/api/sessions", "/api/sessions/",
                    "/api/memory", "/api/index", "/api/repo-map", "/api/trace"}


def check_file(path: Path):
    html = path.read_text(encoding="utf-8")
    errors = []

    ids = set(re.findall(r'id="([^"]+)"', html))
    for used in sorted(set(re.findall(r"getElementById\(['\"]([^'\"]+)['\"]\)", html))):
        if used not in ids:
            errors.append(f"missing element id: {used}")

    functions = set(re.findall(r"function\s+([A-Za-z_$][\w$]*)\s*\(", html))
    for handler_attr in re.findall(r'on(?:click|input|change)="([^"]+)"', html):
        for called in re.findall(r"([A-Za-z_$][\w$]*)\s*\(", handler_attr):
            if called in {"if", "return", "alert", "confirm"}:
                continue
            if called not in functions:
                errors.append(f"handler calls undefined function: {called}")

    used_endpoints = set(re.findall(r"fetch\(['\"](/api/[\w\-\/]+)['\"]", html))
    for endpoint in sorted(used_endpoints):
        if endpoint not in SERVER_ENDPOINTS:
            errors.append(f"unknown API endpoint: {endpoint}")

    return html, errors, used_endpoints


def main():
    ok = True
    for name in ("index.html", "setup.html"):
        path = ROOT / "web" / name
        html, errors, endpoints = check_file(path)
        status = "OK" if not errors else "FAIL"
        print(f"[{status}] {name}  ({len(html)} bytes, endpoints: {sorted(endpoints)})")
        for err in errors:
            print("       -", err)
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
