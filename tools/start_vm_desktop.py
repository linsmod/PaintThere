#!/usr/bin/env python3
"""零参数启动 vm-desktop 的桌面 demo。

日常用法：
    python tools/start_vm_desktop.py             # 默认 demo_jpx（JPX 应用外壳）
    python tools/start_vm_desktop.py demo_wm     # WM 双窗口 demo
    python tools/start_vm_desktop.py demo_xaml --port 9061 --theme "<theme.xaml>" --font <font.ttf>

第一个位置参数选择程序：内置 demo_wm / demo_jpx / demo_xaml 有默认端口映射；
其他值按 build/<名字>.exe 的构建产物运行（默认端口 9000，建议显式 --port）。
`--port N` 指定端口（优先于 VM_DESKTOP_PORT）；`--no-browser` 不自动开浏览器；
其余参数原样透传给 exe（如 demo_xaml 的 --theme / --font）。

脚本自行定位仓库根、共享资源和 exe，显式传入 webRoot 绝对路径，
因此从任何目录调用都不依赖工作目录或 exe 的默认参数。
构建由 tools/build_vm.py 统一完成；本脚本只检查产物并按端口停掉旧实例，
源码改动后先运行 python tools/build_vm.py。

脚本在前台保持运行（exe 由 Popen 拉起），按 Ctrl+C 会同时停止它；
需要终端继续干活就另开一个窗口，不要再靠关终端来结束 demo。

可选环境变量（仅调试用，日常无需设置）：
    VM_DESKTOP_PORT        监听端口，默认 demo_wm=9000 / demo_jpx=9050 / demo_xaml=9060
    VM_DESKTOP_NO_BROWSER  设为 1 时不自动打开浏览器
    VM_DESKTOP_APP         默认启动的程序，默认 demo_jpx（demo_wm / demo_jpx / demo_xaml），命令行参数优先
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
BUILD = VM_DIR / "build"
ASSETS = ROOT / "assets"
FONT_SOURCE = Path(r"H:\github_repos\mystbfont_c\fonts\NotoSans-Regular.ttf")

APP_NAME = "demo_jpx"
EXE = BUILD / "demo_jpx.exe"

# 内置 demo 的默认端口；不在表里的 exe 一律默认 9000。
DEFAULT_PORTS = {"demo_wm": 9000, "demo_jpx": 9050, "demo_xaml": 9060}

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


def parse_args(argv):
    """解析控制参数，返回 (app, port_override, no_browser, exe_args)。

    app 是第一个不以 - 开头的参数；--port/--no-browser 是脚本自己的控制参数；
    其余参数原样透传给 exe。exe 的 --theme/--font 是路径参数：exe 以 vm-desktop
    为工作目录启动，相对路径会落错位置，这里按调用者的 CWD 转成绝对路径。"""
    app = None
    port_override = None
    no_browser = False
    exe_args = []
    path_flags = ("--theme", "--font", "--theme-css")
    expect_path = False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--port":
            if i + 1 >= len(argv):
                raise ValueError("--port 需要端口号")
            port_override = int(argv[i + 1])
            i += 2
            expect_path = False
        elif a == "--no-browser":
            no_browser = True
            i += 1
            expect_path = False
        elif a.startswith("-"):
            if app is None:
                raise ValueError(f"未知参数: {a}（可用 --port N / --no-browser）")
            # 程序名之后的未知旗标属于 exe 自己（如 --theme / --font），透传。
            exe_args.append(a)
            expect_path = a in path_flags
            i += 1
        elif app is None:
            app = a
            i += 1
        else:
            if expect_path and not os.path.isabs(a) and not a.startswith("/"):
                a = os.path.abspath(a)
            exe_args.append(a)
            expect_path = False
            i += 1
    return app, port_override, no_browser, exe_args


def main():
    global APP_NAME, EXE
    try:
        app, port_override, no_browser, exe_args = parse_args(sys.argv[1:])
    except ValueError as exc:
        print(f"[start] {exc}", file=sys.stderr)
        return 1
    if app is None:
        app = os.environ.get("VM_DESKTOP_APP", "demo_jpx")
    APP_NAME = app
    EXE = BUILD / f"{app}.exe"
    # demo 默认端口分开，可以同时开着对比；未知 exe 默认 9000。
    default_port = DEFAULT_PORTS.get(app, 9000)
    if port_override is not None:
        port = port_override
    else:
        port = int(os.environ.get("VM_DESKTOP_PORT", str(default_port)))

    try:
        ensure_assets()
        check_built()
        stop_old(port, APP_NAME)
    except Exception as exc:
        print(f"[start] 失败: {exc}", file=sys.stderr)
        return 1

    proc = subprocess.Popen(
        [str(EXE), str(port), str(ASSETS)] + exe_args, cwd=VM_DIR
    )
    time.sleep(0.3)
    if proc.poll() is not None:
        print(f"[start] 失败: {APP_NAME}.exe 退出码 {proc.returncode}，端口 {port} 可能被其他进程占用", file=sys.stderr)
        return 1
    try:
        wait_ready(port)
        url = f"http://127.0.0.1:{port}/"
        print(f"[start] {APP_NAME}.exe 已启动 PID={proc.pid}")
        print(f"[start] {url}")
        print("[start] 保持运行，按 Ctrl+C 停止")
        if os.environ.get("VM_DESKTOP_NO_BROWSER") != "1" and not no_browser:
            webbrowser.open(url)
        proc.wait()
    except KeyboardInterrupt:
        print(f"\n[start] Ctrl+C，停止 {APP_NAME}.exe")
    except Exception as exc:
        print(f"[start] 失败: {exc}", file=sys.stderr)
        return 1
    finally:
        # Ctrl+C 之外，脚本被终止或 wait 抛错时也要收掉子进程，
        # 否则 demo 会变成没人管的后台进程。
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    return 0


if __name__ == "__main__":
    sys.exit(main())
