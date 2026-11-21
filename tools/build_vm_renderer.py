#!/usr/bin/env python3
"""编译 C 版 VM 渲染器，并同步 canvas-cpp 的静态库与运行时 DLL。

一条命令：
    python tools/build_vm_renderer.py

流程：
  1. 检查 canvas-cpp 的 build-ant 产物（libcanvas.a / antjsb.dll）
  2. 同步 libcanvas.a → vm-desktop/renderer/build/canvas-cpp/
  3. 同步 antjsb.dll → renderer/build/ 与 assets/
  4. cmake 配置（首次）+ 构建，重新链接 vm_renderer.exe

可选环境变量：
    CANVAS_BUILD_DIR   canvas-cpp 构建目录，默认 E:/canvas-cpp/recovery/out/recovered/build-ant
    CANVAS_SOURCE_DIR  canvas-cpp 源码目录，默认 E:/canvas-cpp/recovery/out/recovered
    VM_BUILD_DIR       cmake 构建目录，默认 <repo>/vm-desktop/renderer/build
    VM_FORCE_CONFIGURE 设为 1 时强制重新 cmake 配置
"""

import os
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RENDERER = ROOT / "vm-desktop" / "renderer"
ASSETS = ROOT / "assets"
BUILD = Path(os.environ.get("VM_BUILD_DIR", str(RENDERER / "build")))
CANVAS_BUILD = Path(os.environ.get(
    "CANVAS_BUILD_DIR", r"E:\canvas-cpp\recovery\out\recovered\build-ant"))
CANVAS_SRC = Path(os.environ.get(
    "CANVAS_SOURCE_DIR", r"E:\canvas-cpp\recovery\out\recovered"))
CMAKE = Path(r"C:\msys64\mingw64\bin\cmake.exe")
GCC = Path(r"C:\msys64\mingw64\bin\gcc.exe")
GXX = Path(r"C:\msys64\mingw64\bin\g++.exe")
ANT_ROOT = Path(r"H:\ant-v16")

SRC_A = CANVAS_BUILD / "libcanvas.a"
SRC_DLL = CANVAS_BUILD / "antjsb.dll"
DST_A = BUILD / "canvas-cpp" / "libcanvas.a"
DST_DLLS = (BUILD / "antjsb.dll", ASSETS / "antjsb.dll")


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
    if not (RENDERER / "CMakeLists.txt").is_file():
        return f"找不到 {RENDERER / 'CMakeLists.txt'}"
    return None


def sync_file(src, dst):
    if dst.is_file() and dst.stat().st_mtime >= src.stat().st_mtime:
        print(f"[build] 已最新: {dst.name}")
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    print(f"[build] 同步: {src.name} -> {dst}")


def newest_source_time():
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

    newest_src = newest_source_time()
    if newest_src > SRC_A.stat().st_mtime:
        print(f"[build] 警告: canvas-cpp 源码比 libcanvas.a 新 "
              f"({datetime.fromtimestamp(newest_src):%H:%M:%S} > "
              f"{datetime.fromtimestamp(SRC_A.stat().st_mtime):%H:%M:%S})，"
              f"请先在 canvas-cpp 重新构建 build-ant")

    sync_file(SRC_A, DST_A)
    for dst in DST_DLLS:
        sync_file(SRC_DLL, dst)

    need_configure = (
        os.environ.get("VM_FORCE_CONFIGURE") == "1"
        or not (BUILD / "CMakeCache.txt").is_file()
    )
    if need_configure:
        print("[build] cmake 配置 ...")
        cmd = [
            str(CMAKE), "-S", str(RENDERER), "-B", str(BUILD),
            "-G", "MinGW Makefiles",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_C_COMPILER={GCC}",
            f"-DCMAKE_CXX_COMPILER={GXX}",
            f"-DCANVAS_SOURCE={CANVAS_SRC}",
            f"-DANT_ROOT={ANT_ROOT}",
        ]
        r = subprocess.run(cmd, text=True)
        if r.returncode != 0:
            return fail(f"cmake 配置失败（{r.returncode}）")

    print("[build] cmake 构建 ...")
    r = subprocess.run([str(CMAKE), "--build", str(BUILD), "-j4"], text=True)
    if r.returncode != 0:
        return fail(f"cmake 构建失败（{r.returncode}）")

    exe = BUILD / "vm_renderer.exe"
    if not exe.is_file():
        return fail(f"未生成 {exe}")
    print(f"[build] 完成: {exe}  "
          f"({exe.stat().st_size} bytes, "
          f"{datetime.fromtimestamp(exe.stat().st_mtime):%H:%M:%S})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
