#!/usr/bin/env python3
"""一次构建两个 C 项目：桌面服务端 demo_wm.exe 与 C 渲染器 vm_renderer.exe。

一条命令：
    python tools/build_vm.py

CMake 工程在 vm-desktop/，产物统一输出到 vm-desktop/build/：
    vm-desktop/build/demo_wm.exe
    vm-desktop/build/vm_renderer.exe

流程：
  1. 检查工具链、canvas-cpp 产物与 Clay 头文件
  2. 同步 libcanvas.a / antjsb.dll 到 build 目录与 assets
  3. cmake 配置 + 构建两个目标

可选环境变量（仅调试用，日常无需设置）：
    CANVAS_BUILD_DIR   canvas-cpp 构建目录，默认 E:/canvas-cpp/recovery/out/recovered/build-ant
    CANVAS_SOURCE_DIR  canvas-cpp 源码目录，默认 E:/canvas-cpp/recovery/out/recovered
    CLAY_DIR           clay.h 所在目录，默认 H:/github_repos/clay
    VM_BUILD_DIR       构建目录，默认 <repo>/vm-desktop/build
"""

import os
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VM_DIR = ROOT / "vm-desktop"
ASSETS = ROOT / "assets"
BUILD = Path(os.environ.get("VM_BUILD_DIR", str(VM_DIR / "build")))

CANVAS_BUILD = Path(os.environ.get(
    "CANVAS_BUILD_DIR", r"E:\canvas-cpp\recovery\out\recovered\build-ant"))
CANVAS_SRC = Path(os.environ.get(
    "CANVAS_SOURCE_DIR", r"E:\canvas-cpp\recovery\out\recovered"))
CLAY_DIR = Path(os.environ.get("CLAY_DIR", r"H:\github_repos\clay"))

CMAKE = Path(r"C:\msys64\mingw64\bin\cmake.exe")
GCC = Path(r"C:\msys64\mingw64\bin\gcc.exe")
GXX = Path(r"C:\msys64\mingw64\bin\g++.exe")
ANT_ROOT = Path(r"H:\ant-v16")

SRC_A = CANVAS_BUILD / "libcanvas.a"
SRC_DLL = CANVAS_BUILD / "antjsb.dll"
DST_A = BUILD / "canvas-cpp" / "libcanvas.a"
DST_DLLS = (BUILD / "antjsb.dll", ASSETS / "antjsb.dll")
EXES = (BUILD / "demo_wm.exe", BUILD / "vm_renderer.exe")


def fail(msg):
    print(f"[build] 失败: {msg}", file=sys.stderr)
    return 1


def check_inputs():
    for tool in (CMAKE, GCC, GXX):
        if not tool.is_file():
            return f"找不到工具链: {tool}"
    for src in (SRC_A, SRC_DLL):
        if not src.is_file():
            return f"找不到 canvas-cpp 产物: {src}（先构建 canvas-cpp 的 build-ant）"
    if not (VM_DIR / "CMakeLists.txt").is_file():
        return f"找不到 {VM_DIR / 'CMakeLists.txt'}"
    if not (CLAY_DIR / "clay.h").is_file():
        return f"找不到 {CLAY_DIR / 'clay.h'}；用 CLAY_DIR 指定 clay 目录"
    return None


def sync_file(src, dst):
    if dst.is_file() and dst.stat().st_mtime >= src.stat().st_mtime:
        print(f"[build] 已最新: {dst.name}")
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    print(f"[build] 同步: {src.name} -> {dst}")


def newest_canvas_source_time():
    newest = 0.0
    src_dir = CANVAS_SRC / "src"
    if not src_dir.is_dir():
        return newest
    for p in src_dir.rglob("*"):
        if p.is_file() and p.suffix.lower() in (".c", ".h", ".cpp", ".hpp"):
            newest = max(newest, p.stat().st_mtime)
    return newest


def main():
    err = check_inputs()
    if err:
        return fail(err)

    newest_src = newest_canvas_source_time()
    if newest_src > SRC_A.stat().st_mtime:
        print(f"[build] 警告: canvas-cpp 源码比 libcanvas.a 新 "
              f"({datetime.fromtimestamp(newest_src):%H:%M:%S} > "
              f"{datetime.fromtimestamp(SRC_A.stat().st_mtime):%H:%M:%S})，"
              f"请先在 canvas-cpp 重新构建 build-ant")

    sync_file(SRC_A, DST_A)
    for dst in DST_DLLS:
        sync_file(SRC_DLL, dst)

    print("[build] cmake 配置 ...")
    configure = [
        str(CMAKE), "-S", str(VM_DIR), "-B", str(BUILD),
        "-G", "MinGW Makefiles",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        f"-DCMAKE_C_COMPILER={GCC}",
        f"-DCMAKE_CXX_COMPILER={GXX}",
        # 本机 mingw64/x86_64-w64-mingw32/bin/as.exe 启动即 0xC0000139
        # （DLL 入口点找不到），gcc 默认按相对路径挑中它；-B 强制用 bin 下可用的那份。
        "-DCMAKE_C_FLAGS=-BC:/msys64/mingw64/bin/",
        "-DCMAKE_CXX_FLAGS=-BC:/msys64/mingw64/bin/",
        f"-DCANVAS_SOURCE={CANVAS_SRC}",
        f"-DANT_ROOT={ANT_ROOT}",
        f"-DCLAY_DIR={CLAY_DIR}",
        "-DCANVAS_JS_ENGINE=ant",
    ]
    r = subprocess.run(configure, text=True)
    if r.returncode != 0:
        return fail(f"cmake 配置失败（{r.returncode}）")

    print("[build] cmake 构建 demo_wm + vm_renderer ...")
    r = subprocess.run([str(CMAKE), "--build", str(BUILD), "-j4"], text=True)
    if r.returncode != 0:
        return fail(f"cmake 构建失败（{r.returncode}）")

    for exe in EXES:
        if not exe.is_file():
            return fail(f"未生成 {exe}")
        print(f"[build] 完成: {exe}  "
              f"({exe.stat().st_size} bytes, "
              f"{datetime.fromtimestamp(exe.stat().st_mtime):%H:%M:%S})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
