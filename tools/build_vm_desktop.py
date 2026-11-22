#!/usr/bin/env python3
"""编译 vm-desktop 桌面服务端 demo_wm.exe（含 remote tiles 协议）。

一条命令：
    python tools/build_vm_desktop.py

构建产物：vm-desktop/demo_wm.exe
源文件与头文件都比产物旧时跳过构建；用 VM_FORCE_BUILD=1 强制重建。

注意：Windows 下无法覆盖正在运行的 exe。先停掉 demo_wm.exe 再构建，
或用 tools/start_vm_desktop.py / tools/start_vm_renderer.py（会按端口停旧实例后自动构建）。

可选环境变量（仅调试用，日常无需设置）：
    VM_GCC            gcc 路径，默认 C:/msys64/mingw64/bin/gcc.exe
    CLAY_DIR          clay.h 所在目录，默认 H:/github_repos/clay
    VM_DESKTOP_EXE    输出路径，默认 <repo>/vm-desktop/demo_wm.exe
    VM_FORCE_BUILD    设为 1 时强制重建
"""

import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VM_DIR = ROOT / "vm-desktop"

GCC = Path(os.environ.get("VM_GCC", r"C:\msys64\mingw64\bin\gcc.exe"))
CLAY_DIR = Path(os.environ.get("CLAY_DIR", r"H:\github_repos\clay"))
EXE = Path(os.environ.get("VM_DESKTOP_EXE", str(VM_DIR / "demo_wm.exe")))

SOURCES = (
    "demo_wm.c",
    "clay_frame_adapter.c",
    "vm_display.c",
    "vm_serialize_json.c",
    "vm_tiles.c",
    "vm_wm.c",
    "vm_server.c",
)
HEADERS = (
    "clay_frame_adapter.h",
    "vm_display.h",
    "vm_serialize_json.h",
    "vm_tiles.h",
    "vm_wm.h",
    "vm_server.h",
)


def fail(msg):
    print(f"[build-desktop] 失败: {msg}", file=sys.stderr)
    return 1


def check_inputs():
    if not GCC.is_file():
        return f"找不到 gcc: {GCC}"
    if not (CLAY_DIR / "clay.h").is_file():
        return f"找不到 {CLAY_DIR / 'clay.h'}；用 CLAY_DIR 指定 clay 目录"
    for name in SOURCES + HEADERS:
        if not (VM_DIR / name).is_file():
            return f"找不到源文件: {VM_DIR / name}"
    return None


def need_build():
    if os.environ.get("VM_FORCE_BUILD") == "1" or not EXE.is_file():
        return True
    exe_time = EXE.stat().st_mtime
    for name in SOURCES + HEADERS:
        if (VM_DIR / name).stat().st_mtime > exe_time:
            return True
    return False


def main():
    err = check_inputs()
    if err:
        return fail(err)

    if not need_build():
        print(f"[build-desktop] 已最新: {EXE}")
        return 0

    cmd = [
        str(GCC), "-std=c11",
        "-I" + str(CLAY_DIR), "-I" + str(VM_DIR),
        *[str(VM_DIR / name) for name in SOURCES],
        "-lws2_32",
        "-o", str(EXE),
    ]
    print(f"[build-desktop] 构建 {EXE.name} ...")
    r = subprocess.run(cmd, cwd=VM_DIR, text=True)
    if r.returncode != 0:
        return fail(f"gcc 退出码 {r.returncode}；若提示 Permission denied，先停掉正在运行的 demo_wm.exe")

    if not EXE.is_file():
        return fail(f"未生成 {EXE}")
    print(f"[build-desktop] 完成: {EXE}  "
          f"({EXE.stat().st_size} bytes, "
          f"{datetime.fromtimestamp(EXE.stat().st_mtime):%H:%M:%S})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
