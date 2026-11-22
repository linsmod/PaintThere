#!/usr/bin/env python3
"""一键启动 C 版 VM 渲染器（canvas-cpp / ant 引擎）。

一条命令同时拉起 SSE 服务端和原生窗口渲染器：
    python tools/build_vm.py && python tools/start_vm_renderer.py

两个可执行文件由统一工程一次构建，产物都在 vm-desktop/build/；
本脚本和 start_vm_desktop.py 只检查产物并启动，不再各自构建。

进程级禁用代理，避免 SDK 的 HTTP 客户端把 127.0.0.1 请求发到系统代理。
Ctrl+C 会同时停掉服务端和渲染器。

可选环境变量（调试用）：
    VM_DESKTOP_PORT      监听端口，默认 9000
    VM_RENDERER_FRAMES   >0 时渲染指定帧数后自动退出，默认 0（一直运行）
"""

import os
import signal
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VM_DIR = ROOT / "vm-desktop"
ASSETS = ROOT / "assets"
EXE = VM_DIR / "build" / "vm_renderer.exe"
PAGE = ASSETS / "vm_renderer.js"
DLL_SRC = Path(r"E:\canvas-cpp\recovery\out\recovered\build-ant\antjsb.dll")


def clear_proxy():
    for name in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
                 "http_proxy", "https_proxy", "all_proxy"):
        os.environ.pop(name, None)
    os.environ["NO_PROXY"] = "*"
    os.environ["no_proxy"] = "*"


def ensure_built():
    desktop = EXE.parent / "demo_wm.exe"
    if EXE.is_file() and desktop.is_file() and PAGE.is_file():
        return
    raise RuntimeError(
        f"缺少 {EXE} / {desktop} / {PAGE}；先运行 python tools/build_vm.py"
    )


def ensure_dll():
    if not DLL_SRC.is_file():
        return
    for target in (EXE.parent / "antjsb.dll", ASSETS / "antjsb.dll"):
        try:
            if not target.is_file() or target.stat().st_mtime < DLL_SRC.stat().st_mtime:
                target.write_bytes(DLL_SRC.read_bytes())
        except OSError:
            pass


def wait_http(port, timeout=8.0):
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
    raise RuntimeError(f"服务端未在 {timeout:g}s 内就绪: {url}")


def main():
    clear_proxy()
    port = int(os.environ.get("VM_DESKTOP_PORT", "9000"))
    frames = int(os.environ.get("VM_RENDERER_FRAMES", "0"))

    try:
        ensure_built()
        ensure_dll()
        # 服务端：复用现有零参数启动脚本；它按进程名替换同端口旧实例。
        env = os.environ.copy()
        env["VM_DESKTOP_PORT"] = str(port)
        env["VM_DESKTOP_NO_BROWSER"] = "1"
        server = subprocess.Popen(
            [sys.executable, str(ROOT / "tools" / "start_vm_desktop.py")],
            cwd=ROOT, env=env,
        )
        time.sleep(0.6)
        wait_http(port)
    except Exception as exc:
        print(f"[renderer] 启动失败: {exc}", file=sys.stderr)
        return 1

    args = [str(EXE), str(PAGE), str(ASSETS), "800", "600", str(frames)]
    renderer = subprocess.Popen(args, cwd=ASSETS, env=os.environ.copy())
    print(f"[renderer] C 渲染器已启动 PID={renderer.pid}")
    print(f"[renderer] 数据源 http://127.0.0.1:{port}/events")
    print("[renderer] Ctrl+C 退出")

    def stop_all():
        for proc in (renderer, server):
            if proc.poll() is None:
                proc.terminate()
        for proc in (renderer, server):
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()

    try:
        return renderer.wait()
    except KeyboardInterrupt:
        stop_all()
        return 0
    finally:
        if renderer.poll() is not None and server.poll() is None:
            stop_all()


if __name__ == "__main__":
    sys.exit(main())
