#!/usr/bin/env python3
"""一键启动 C 版 VM 渲染器（canvas-cpp / ant 引擎），默认展示多进程桌面。

一条命令同时拉起桌面服务端和原生窗口渲染器：
    python tools/build_vm.py && python tools/start_vm_renderer.py

默认服务端为 `multi`（vm_wms + vm_shell + vm_app + vm_rdp，端口 9050）；
vm_renderer.exe 连到 vm_rdp 的 /events。单个 exe 也可以显式指定
（VM_RENDERER_APP=demo_wm）。

两个可执行文件由统一工程一次构建，产物都在 vm-desktop/build/；
本脚本和 start_vm_desktop.py 只检查产物并启动，不再各自构建。

进程级禁用代理，避免 SDK 的 HTTP 客户端把 127.0.0.1 请求发到系统代理。
Ctrl+C 会停掉服务端（多进程时按 `vmwm_ui_` 管道名收掉整组）和渲染器。

可选环境变量（调试用）：
    VM_RENDERER_APP      服务端，默认 multi（可设 demo_wm）
    VM_DESKTOP_PORT      监听端口，默认 9050
    VM_RENDERER_FRAMES   >0 时渲染指定帧数后自动退出，默认 0（一直运行）
    VM_RENDERER_STATE    设 1 打开 C 渲染器 [STATE] 探针，默认关闭
"""

import os
import signal
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

try:
    from .start_vm_desktop import stop_old, stop_old_multi
except ImportError:
    from start_vm_desktop import stop_old, stop_old_multi

ROOT = Path(__file__).resolve().parent.parent
VM_DIR = ROOT / "vm-desktop"
ASSETS = ROOT / "assets"
EXE = VM_DIR / "build" / "vm_renderer.exe"
PAGE = ASSETS / "vm_renderer.js"
DLL_SRC = Path(r"E:\canvas-cpp\recovery\out\recovered\build-ant\antjsb.dll")
SERVER_APP = "multi"
DEFAULT_PORT = 9050
MULTI_EXES = ("vm_wms", "vm_shell", "vm_app", "vm_rdp")


def clear_proxy():
    for name in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY",
                 "http_proxy", "https_proxy", "all_proxy"):
        os.environ.pop(name, None)
    os.environ["NO_PROXY"] = "*"
    os.environ["no_proxy"] = "*"


def ensure_built(app):
    if app == "multi":
        missing = [n for n in MULTI_EXES if not (EXE.parent / f"{n}.exe").is_file()]
        if EXE.is_file() and not missing and PAGE.is_file():
            return
        raise RuntimeError(
            f"缺少 {EXE} / {missing} / {PAGE}；先运行 python tools/build_vm.py"
        )
    server = EXE.parent / f"{app}.exe"
    if EXE.is_file() and server.is_file() and PAGE.is_file():
        return
    raise RuntimeError(
        f"缺少 {EXE} / {server} / {PAGE}；先运行 python tools/build_vm.py"
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
    app = os.environ.get("VM_RENDERER_APP", SERVER_APP)
    port = int(os.environ.get("VM_DESKTOP_PORT", str(DEFAULT_PORT)))
    frames = int(os.environ.get("VM_RENDERER_FRAMES", "0"))

    try:
        ensure_built(app)
        ensure_dll()
        # 服务端：复用现有启动脚本并显式选程序；它按进程名替换同端口旧实例。
        env = os.environ.copy()
        env["VM_DESKTOP_PORT"] = str(port)
        env["VM_DESKTOP_NO_BROWSER"] = "1"
        server = subprocess.Popen(
            [sys.executable, str(ROOT / "tools" / "start_vm_desktop.py"), app],
            cwd=ROOT, env=env,
        )
        time.sleep(0.6)
        wait_http(port)
    except Exception as exc:
        print(f"[renderer] 启动失败: {exc}", file=sys.stderr)
        return 1

    args = [str(EXE), str(PAGE), str(ASSETS), "800", "600", str(frames)]
    renderer_env = os.environ.copy()
    # C 宿主默认按 file:// + 9000 拼 SSE 地址；用页面 URL 把 location.origin
    # 指到实际服务端端口，vm_renderer.js 才会连到 demo_jpx 的 /events。
    renderer_env["CANVAS_PAGE_URL"] = f"http://127.0.0.1:{port}/"
    renderer = subprocess.Popen(args, cwd=ASSETS, env=renderer_env)
    print(f"[renderer] C 渲染器已启动 PID={renderer.pid}")
    print(f"[renderer] 数据源 {app} http://127.0.0.1:{port}/events")
    print("[renderer] Ctrl+C 退出")

    def stop_all():
        # 先收掉真正的桌面子进程：多进程按 `vmwm_ui_` 管道名，单进程按端口 + 进程名。
        # 否则只终止中间的 python 启动脚本，桌面进程会留下来继续出帧。
        if app == "multi":
            stop_old_multi()
        else:
            stop_old(port, app)
        for proc in (renderer, server):
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()

    try:
        return renderer.wait()
    except KeyboardInterrupt:
        stop_all()
        return 0
    finally:
        if renderer.poll() is None or server.poll() is None:
            stop_all()


if __name__ == "__main__":
    sys.exit(main())
