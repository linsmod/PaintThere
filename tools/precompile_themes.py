#!/usr/bin/env python3
"""把 RetroBar 风格 .xaml 主题预编译进 assets/themes/<slug>/。

用法：
    python tools/precompile_themes.py <themesDir>     # 目录下全部 .xaml
    python tools/precompile_themes.py <theme.xaml>    # 单个主题
    python tools/precompile_themes.py <themesDir> --out <dir>   # 自定义输出根

预编译本体在 C 侧（test_jpx_xaml_theme.exe --precompile <theme.xaml> <outDir>），
合并语义、几何归一化与二进制格式只有一份实现；本脚本只做编排：
slug 命名、逐主题调用、产物核对与汇总。

产物（demo_xaml --preset <slug> 启动时按目录读取）：
    assets/themes/<slug>/xalm.bin        资源 record（tagId 预定义元素标记）
    assets/themes/<slug>/strings.bin     字符串池（key/颜色/方向/path/路径）
    assets/themes/<slug>/resid2path.bin  图片资源 id -> 相对路径
    assets/themes/<slug>/<资源id>.png    图片本体（按 id 重命名）
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "vm-desktop" / "build" / "test_jpx_xaml_theme.exe"
OUT_ROOT = ROOT / "assets" / "themes"

REQUIRED_FILES = ("xalm.bin", "strings.bin", "resid2path.bin")


def slug(name: str) -> str:
    """主题文件名 -> 目录名：去 Windows 前缀、非字母数字转连字符。
    'Windows XP Blue' -> 'xp-blue'，'Windows 95-98' -> '95-98'。"""
    s = name.lower()
    if s.startswith("windows "):
        s = s[len("windows "):]
    out = "".join(ch if ch.isalnum() else "-" for ch in s)
    while "--" in out:
        out = out.replace("--", "-")
    return out.strip("-")


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--out"]
    out_override = None
    if "--out" in sys.argv[1:]:
        out_override = sys.argv[sys.argv.index("--out") + 1]
    if not args:
        print(__doc__)
        return 1
    if not EXE.is_file():
        print(f"[precompile] 找不到 {EXE}，先运行 python tools/build_vm.py", file=sys.stderr)
        return 1

    src = Path(args[0])
    themes = sorted(src.glob("*.xaml")) if src.is_dir() else [src]
    if not themes:
        print(f"[precompile] 没有找到 .xaml: {src}", file=sys.stderr)
        return 1
    out_root = Path(out_override) if out_override else OUT_ROOT

    ok, fail = 0, 0
    for t in themes:
        out = out_root / slug(t.stem)
        r = subprocess.run([str(EXE), "--precompile", str(t), str(out)],
                           capture_output=True, text=True)
        tail = (r.stdout or r.stderr).strip().splitlines()
        tail = tail[-1] if tail else ""
        if r.returncode != 0:
            fail += 1
            print(f"{t.name:42s} -> {out.name:22s} FAIL  {tail}")
            continue
        missing = [f for f in REQUIRED_FILES if not (out / f).is_file()]
        if missing:
            fail += 1
            print(f"{t.name:42s} -> {out.name:22s} FAIL  缺产物 {missing}")
            continue
        ok += 1
        print(f"{t.name:42s} -> {out.name:22s} ok    {tail}")

    print(f"[precompile] {ok} ok, {fail} failed -> {out_root}")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
