#!/usr/bin/env python3
"""零参数启动 vm-desktop 的 WM demo。

日常用法：
    python tools/start_vm_desktop.py

脚本自行定位仓库根、共享资源和 exe，显式传入 webRoot 绝对路径，
因此从任何目录调用都不依赖工作目录或 exe 的默认参数。

可选环境变量（仅调试用，日常无需设置）：
    VM_DESKTOP_PORT        监听端口，默认 9000
    VM_DESKTOP_NO_BROWSER  设为 1 时不自动打开浏览器
"""

import os
import shutil
import subprocess
import sys
import time
import urllib.request
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VM_DIR = ROOT / "vm-desktop"
ASSETS = ROOT / "assets"
EXE = VM_DIR / "demo_wm.exe"
FONT_SOURCE = Path(r"H:\github_repos\mystbfont_c\fonts\NotoSans-Regular.ttf")

REQUIRED_ASSETS = (
    "app.js",
    "systemColors.js",
    "font_wasm.js",
    "stb_font_wasm.mjs",
    "stb_font_wasm.wasm",
    "font.ttf",
)


def ensure_assets():
    missing = [name for name in REQUIRED_ASSETS if not (ASSETS / name).is_file()]
    if "font.ttf" in missing and FONT_SOURCE.is_file():
        shutil.copy2(FONT_SOURCE, ASSETS / "font.ttf")
        missing.remove("font.ttf")
        print("[start] 已从 mystbfont_c 补齐 assets/font.ttf")
    if missing:
        raise RuntimeError("assets 缺失文件: " + ", ".join(missing))


def stop_old(port):
    ps = (
        "$ids = @(Get-NetTCPConnection -LocalPort %d -State Listen "
        "-ErrorAction SilentlyContinue | Select-Object -ExpandProperty OwningProcess); "
        "foreach ($id in $ids) { "
        "$p = Get-Process -Id $id -ErrorAction SilentlyContinue; "
        "if ($p -and $p.ProcessName -eq 'demo_wm') "
        "{ Stop-Process -Id $id; Write-Output $id } "
        "}"
    ) % port
    result = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
        capture_output=True,
        text=True,
        check=False,
    )
    killed = result.stdout.split()
    for pid in killed:
        print(f"[start] 停止旧的 demo_wm.exe (PID {pid})")
    if killed:
        time.sleep(0.6)


def wait_ready(port, timeout=8.0):
    url = f"http://127.0.0.1:{port}/"
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=1) as response:
                if response.status == 200:
                    return
        except Exception:
            pass
        time.sleep(0.15)
    raise RuntimeError(f"服务未在 {timeout:g}s 内就绪: {url}")


def main():
    port = int(os.environ.get("VM_DESKTOP_PORT", "9000"))
    try:
        ensure_assets()
        if not EXE.is_file():
            raise RuntimeError(f"找不到 {EXE}，请先按工作流编译 demo_wm.exe")
        stop_old(port)
    except Exception as exc:
        print(f"[start] 失败: {exc}", file=sys.stderr)
        return 1

    proc = subprocess.Popen([str(EXE), str(port), str(ASSETS)], cwd=VM_DIR)
    time.sleep(0.3)
    if proc.poll() is not None:
        print(f"[start] 失败: demo_wm.exe 退出码 {proc.returncode}，端口 {port} 可能被其他进程占用", file=sys.stderr)
        return 1
    try:
        wait_ready(port)
    except Exception as exc:
        print(f"[start] 失败: {exc}", file=sys.stderr)
        proc.terminate()
        return 1

    url = f"http://127.0.0.1:{port}/"
    print(f"[start] demo_wm.exe 已启动 PID={proc.pid}")
    print(f"[start] {url}")
    if os.environ.get("VM_DESKTOP_NO_BROWSER") != "1":
        webbrowser.open(url)
    return 0


if __name__ == "__main__":
    sys.exit(main())
