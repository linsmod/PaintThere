#!/usr/bin/env python3
"""零参数启动 vm-desktop 的桌面 demo。

日常用法：
    python tools/start_vm_desktop.py             # demo_wm，原样
    python tools/start_vm_desktop.py demo_jpx    # JPX 应用外壳：开始菜单 / ProcMan / Files

脚本自行定位仓库根、共享资源和 exe，显式传入 webRoot 绝对路径，
因此从任何目录调用都不依赖工作目录或 exe 的默认参数。
构建由 tools/build_vm.py 统一完成；本脚本只检查产物并按端口停掉旧实例，
源码改动后先运行 python tools/build_vm.py。

可选环境变量（仅调试用，日常无需设置）：
    VM_DESKTOP_PORT        监听端口，默认 9000
    VM_DESKTOP_NO_BROWSER  设为 1 时不自动打开浏览器
    VM_DESKTOP_APP         默认启动的程序（demo_wm / demo_jpx），命令行参数优先
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
FONT_SOURCE = Path(r"H:\github_repos\mystbfont_c\fonts\NotoSans-Regular.ttf")

APP_NAME = "demo_wm"
EXE = VM_DIR / "build" / "demo_wm.exe"

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


def stop_old(port, app_name):
    ps = (
        "$ids = @(Get-NetTCPConnection -LocalPort %d -State Listen "
        "-ErrorAction SilentlyContinue | Select-Object -ExpandProperty OwningProcess); "
        "foreach ($id in $ids) { "
        "$p = Get-Process -Id $id -ErrorAction SilentlyContinue; "
        "if ($p -and $p.ProcessName -eq '%s') "
        "{ Stop-Process -Id $id; Write-Output $id } "
        "}"
    ) % (port, app_name)
    result = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
        capture_output=True,
        text=True,
        check=False,
    )
    killed = result.stdout.split()
    for pid in killed:
        print(f"[start] 停止旧的 {app_name}.exe (PID {pid})")
    if killed:
        time.sleep(0.6)


def check_built():
    if not EXE.is_file():
        raise RuntimeError(f"找不到 {EXE}，先运行 python tools/build_vm.py")


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
    global APP_NAME, EXE
    port = int(os.environ.get("VM_DESKTOP_PORT", "9000"))
    arg = None
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        arg = sys.argv[1]
    if arg is None:
        arg = os.environ.get("VM_DESKTOP_APP", "demo_wm")
    if arg not in ("demo_wm", "demo_jpx"):
        print(f"[start] 未知程序: {arg}（可用 demo_wm / demo_jpx）", file=sys.stderr)
        return 1
    APP_NAME = arg
    EXE = VM_DIR / "build" / f"{arg}.exe"

    try:
        ensure_assets()
        check_built()
        stop_old(port, APP_NAME)
    except Exception as exc:
        print(f"[start] 失败: {exc}", file=sys.stderr)
        return 1

    proc = subprocess.Popen([str(EXE), str(port), str(ASSETS)], cwd=VM_DIR)
    time.sleep(0.3)
    if proc.poll() is not None:
        print(f"[start] 失败: {APP_NAME}.exe 退出码 {proc.returncode}，端口 {port} 可能被其他进程占用", file=sys.stderr)
        return 1
    try:
        wait_ready(port)
    except Exception as exc:
        print(f"[start] 失败: {exc}", file=sys.stderr)
        proc.terminate()
        return 1

    url = f"http://127.0.0.1:{port}/"
    print(f"[start] {APP_NAME}.exe 已启动 PID={proc.pid}")
    print(f"[start] {url}")
    if os.environ.get("VM_DESKTOP_NO_BROWSER") != "1":
        webbrowser.open(url)
    return 0


if __name__ == "__main__":
    sys.exit(main())
