#!/usr/bin/env python3
"""启动 vm-desktop 桌面。

日常用法：
    python tools/start_vm_desktop.py              # 多进程桌面：vm_wms + vm_shell + vm_app + vm_rdp
    python tools/start_vm_desktop.py demo_wm      # 单进程 WM demo（9000）
    python tools/start_vm_desktop.py vm_app ...   # 单独跑 build/<name>.exe

多进程模式（默认）：按顺序拉起 vm_wms / vm_shell / vm_app / vm_rdp，共用一条本次运行
独有的命名管道（`vmwm_ui_<pid>`）；Ctrl+C 或脚本退出时按逆序收掉全部子进程。
四个进程的日志分别写到 temp/desktop-<name>.err.txt。

`--port N` 指定端口（优先于 VM_DESKTOP_PORT）；`--no-browser` 不自动开浏览器；
多进程模式只认 `--font <ttf>`，其余参数忽略；单进程模式原样透传给 exe。

脚本自行定位仓库根、共享资源与 exe；构建由 tools/build_vm.py 统一完成，
源码改动后先运行 python tools/build_vm.py。

可选环境变量（仅调试用）：
    VM_DESKTOP_PORT        监听端口，默认 多进程=9050 / demo_wm=9000
    VM_DESKTOP_NO_BROWSER  设为 1 时不自动打开浏览器
    VM_DESKTOP_APP         默认启动项，默认 multi
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

APP_NAME = "multi"
EXE = BUILD / "multi.exe"

# 内置启动项的默认端口；不在表里的 exe 一律默认 9000。
DEFAULT_PORTS = {"multi": 9050, "demo_wm": 9000}

# 多进程桌面的组成与启动顺序（rdp 最后，它连上就请求 fullframe）。
MULTI_EXES = ("vm_wms", "vm_shell", "vm_app", "vm_rdp")

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


def check_built_multi():
    missing = [f"{name}.exe" for name in MULTI_EXES if not (BUILD / f"{name}.exe").is_file()]
    if missing:
        raise RuntimeError("找不到 " + ", ".join(missing) + "，先运行 python tools/build_vm.py")


def stop_old_multi():
    """只停本启动器拉起的那一组：命令行里带 `vmwm_ui_` 显式管道名。"""
    ps = (
        "$p = Get-CimInstance Win32_Process -Filter "
        "\"Name='vm_wms.exe' or Name='vm_shell.exe' or Name='vm_app.exe' or Name='vm_rdp.exe'\" | "
        "Where-Object { $_.CommandLine -like '*vmwm_ui_*' }; "
        "foreach ($x in $p) { Stop-Process -Id $x.ProcessId -Force -ErrorAction SilentlyContinue; "
        "Write-Output $x.ProcessId }"
    )
    result = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
        capture_output=True,
        text=True,
        check=False,
    )
    for pid in result.stdout.split():
        print(f"[start] 停止旧的多进程实例 (PID {pid})")
    if result.stdout.split():
        time.sleep(0.6)


def run_multi(port, no_browser, exe_args):
    pipe = rf"\\.\pipe\vmwm_ui_{os.getpid()}"
    font = str(ASSETS / "font.ttf")
    for i, a in enumerate(exe_args):
        if a == "--font" and i + 1 < len(exe_args):
            font = exe_args[i + 1]
    if exe_args and "--font" not in exe_args:
        print("[start] 多进程模式只认 --font，其余参数忽略")

    log_dir = ROOT / "temp"
    log_dir.mkdir(exist_ok=True)
    procs = []

    def spawn(name, args):
        log_path = log_dir / f"desktop-{name}.err.txt"
        log = open(log_path, "wb")
        p = subprocess.Popen(
            [str(BUILD / f"{name}.exe")] + args,
            cwd=VM_DIR,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        procs.append((name, p, log))
        print(f"[start] {name}.exe PID={p.pid} 日志 {log_path}")
        return p

    try:
        spawn("vm_wms", ["--pipe", pipe, "--screen", "800x600"])
        time.sleep(0.7)
        spawn("vm_shell", ["--pipe", pipe, "--font", font])
        time.sleep(0.4)
        spawn("vm_app", ["--pipe", pipe, "--title", "Hello",
                         "--x", "60", "--y", "60", "--font", font])
        spawn("vm_rdp", ["--port", str(port), "--pipe", pipe,
                         "--web-root", str(ASSETS), "--font", font])

        wait_ready(port)
        url = f"http://127.0.0.1:{port}/"
        print(f"[start] 多进程桌面已就绪：{url}")
        print("[start] 保持运行，按 Ctrl+C 停止（wms/shell/app/rdp 一起退出）")
        if os.environ.get("VM_DESKTOP_NO_BROWSER") != "1" and not no_browser:
            webbrowser.open(url)

        while True:
            for name, p, _log in procs:
                if p.poll() is None:
                    continue
                # wms / rdp 是链路根：它们退出后桌面不再可用，收掉其余进程。
                if name in ("vm_wms", "vm_rdp"):
                    print(f"[start] {name}.exe 已退出（code={p.returncode}）", file=sys.stderr)
                    return 1
                print(f"[start] {name}.exe 已退出（code={p.returncode}），桌面继续", file=sys.stderr)
            time.sleep(0.4)
    except KeyboardInterrupt:
        print("\n[start] Ctrl+C，停止多进程桌面")
        return 0
    except Exception as exc:
        print(f"[start] 失败: {exc}", file=sys.stderr)
        return 1
    finally:
        for name, p, log in reversed(procs):
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
            log.close()


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
        app = os.environ.get("VM_DESKTOP_APP", "multi")
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
        if app == "multi":
            check_built_multi()
            stop_old_multi()
        else:
            check_built()
            stop_old(port, APP_NAME)
    except Exception as exc:
        print(f"[start] 失败: {exc}", file=sys.stderr)
        return 1

    if app == "multi":
        return run_multi(port, no_browser, exe_args)

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
