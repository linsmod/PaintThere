#!/usr/bin/env python3
"""主题导出器：预编译三件套 → CSS 子集（反编译）。

用法：
    python tools/export_theme_css.py <themeDir>            # 单个主题目录
    python tools/export_theme_css.py --all                  # assets/themes 下全部
    python tools/export_theme_css.py --all --check          # 导出→编译→逐字节比对
    选项：--out <dir>（默认 temp/css/<slug>）

只依赖协议（xalm.bin/strings.bin/resid2path.bin 冻结格式，见 docs/task-jp-xaml-theme.md），
不链接任何 C 代码——xaml 工具链与本工具仅协议一致。

CSS 子集（canonical，与编译器 jpx_html_theme 共同冻结）：
    :root {
      --Key: rgba(56,136,233,1.000);                        COLOR（颜色串原样）
      --Key: linear-gradient(dir(0,0,0,1), c1 o1, c2 o2);   LINEAR（dir 内嵌方向原串）
      --Key: radial-gradient(dir(cx,cy,r), ...);            RADIAL
      --Key: 30;                                            NUMBER（repr 精确往返）
      --Key: 'Tahoma';                                      STRING（单引号）
      --Key: path(x0,y0,x1,y1, 'M0,0 ...');                 PATH（bbox + 归一化 data）
      --Key: url(StartIconImage.png);                       IMAGE（相对 css 目录）
    }

--check 的逐字节一致依赖 strings.bin 池规范化（排序去重，与资源顺序无关）。
"""

import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "vm-desktop" / "build" / "test_jpx_xaml_theme.exe"
THEMES_ROOT = ROOT / "assets" / "themes"
CHECK_ROOT = ROOT / "temp" / "css_check"

BIN_FILES = ("xalm.bin", "strings.bin", "resid2path.bin")

# xalm.bin record 布局（冻结）
REC_SIZE = 100
OFF_TAG = 0
OFF_STOPCNT = 1
OFF_STOP_OFFSETS = 4    # f32 x8
OFF_STOP_COLORS = 36    # u32 x8
OFF_KEY = 68
OFF_VALUE = 72
OFF_BBOX = 76           # f32 x4
OFF_NUMBER = 92         # f64

KIND_COLOR, KIND_LINEAR, KIND_RADIAL = 1, 2, 3
KIND_NUMBER, KIND_STRING, KIND_PATH, KIND_IMAGE = 4, 5, 6, 7


def load_pool(theme_dir: Path) -> bytes:
    return (theme_dir / "strings.bin").read_bytes()


def pool_str(pool: bytes, off: int) -> str:
    end = pool.index(b"\x00", off)
    return pool[off:end].decode("utf-8")


def read_xalm(theme_dir: Path):
    data = (theme_dir / "xalm.bin").read_bytes()
    pool = load_pool(theme_dir)
    if data[:4] != b"XALM":
        raise ValueError("bad magic")
    version, res_count, strings_size = struct.unpack_from("<III", data, 4)
    if version != 1 or strings_size != len(pool):
        raise ValueError(f"bad xalm header (v{version})")
    recs = []
    for i in range(res_count):
        base = 16 + i * REC_SIZE
        tag = data[base + OFF_TAG]
        stopcnt = data[base + OFF_STOPCNT]
        stops = [
            (struct.unpack_from("<f", data, base + OFF_STOP_OFFSETS + s * 4)[0],
             pool_str(pool, struct.unpack_from("<I", data, base + OFF_STOP_COLORS + s * 4)[0]))
            for s in range(stopcnt)
        ]
        recs.append({
            "kind": tag,
            "key": pool_str(pool, struct.unpack_from("<I", data, base + OFF_KEY)[0]),
            "value": pool_str(pool, struct.unpack_from("<I", data, base + OFF_VALUE)[0]),
            "stops": stops,
            "bbox": struct.unpack_from("<4f", data, base + OFF_BBOX),
            "number": struct.unpack_from("<d", data, base + OFF_NUMBER)[0],
        })
    return recs


def read_r2p(theme_dir: Path):
    pool = load_pool(theme_dir)
    data = (theme_dir / "resid2path.bin").read_bytes()
    if data[:4] != b"R2PP":
        raise ValueError("bad magic")
    count, strings_size = struct.unpack_from("<II", data, 4)
    if strings_size != len(pool):
        raise ValueError("strings size mismatch")
    out = []
    for i in range(count):
        koff, poff = struct.unpack_from("<II", data, 12 + i * 8)
        out.append((pool_str(pool, koff), pool_str(pool, poff)))
    return out


def value_css(rec: dict) -> str:
    kind = rec["kind"]
    if kind == KIND_COLOR:
        return rec["value"]
    if kind in (KIND_LINEAR, KIND_RADIAL):
        head = "linear-gradient" if kind == KIND_LINEAR else "radial-gradient"
        stops = ", ".join(f"{c} {repr(o)}" for o, c in rec["stops"])
        return f"{head}(dir({rec['value']}), {stops})"
    if kind == KIND_NUMBER:
        return repr(rec["number"])
    if kind == KIND_STRING:
        if "'" in rec["value"]:
            raise ValueError(f"STRING contains quote: {rec['key']}")
        return f"'{rec['value']}'"
    if kind == KIND_PATH:
        b = ", ".join(repr(v) for v in rec["bbox"])
        if "'" in rec["value"]:
            raise ValueError(f"PATH contains quote: {rec['key']}")
        return f"path({b}, '{rec['value']}')"
    raise ValueError(f"unknown kind {kind}")


def export(theme_dir: Path, out_dir: Path) -> Path:
    recs = read_xalm(theme_dir)
    images = read_r2p(theme_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    lines = [":root {"]
    for rec in recs:                      # xalm record 顺序
        lines.append(f"  --{rec['key']}: {value_css(rec)};")
    for key, rel in images:               # resid2path 顺序
        lines.append(f"  --{key}: url({rel});")
    lines.append("}")
    css = out_dir / "theme.css"
    css.write_text("\n".join(lines) + "\n", encoding="utf-8")

    for _, rel in images:                 # 图片复制，导出目录自包含
        src = theme_dir / rel
        dst = out_dir / rel
        if src.is_file():
            shutil.copyfile(src, dst)
        elif not dst.is_file():
            raise FileNotFoundError(f"missing image: {src}")
    return css


def check_one(theme_dir: Path) -> bool:
    """导出 → 编译回 → 三件套逐字节比对。"""
    slug = theme_dir.name
    css_dir = CHECK_ROOT / slug
    css = export(theme_dir, css_dir)
    out = CHECK_ROOT / f"{slug}-compiled"
    r = subprocess.run([str(EXE), "--compile-css", str(css), str(out)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  {slug}: compile failed: {(r.stdout + r.stderr).strip()}")
        return False
    ok = True
    for name in BIN_FILES:
        a = (theme_dir / name).read_bytes()
        b = (out / name).read_bytes()
        if a != b:
            ok = False
            print(f"  {slug}: {name} differs ({len(a)} vs {len(b)} bytes)")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("target", nargs="?", help="主题目录（含三件套）")
    ap.add_argument("--all", action="store_true", help="assets/themes 下全部主题")
    ap.add_argument("--check", action="store_true", help="导出→编译→逐字节比对")
    ap.add_argument("--out", help="导出目录（默认 temp/css/<slug>）")
    args = ap.parse_args()

    if args.all:
        themes = sorted(p for p in THEMES_ROOT.iterdir() if p.is_dir())
    elif args.target:
        themes = [Path(args.target)]
    else:
        ap.print_help()
        return 1

    if args.check:
        if not EXE.is_file():
            print(f"找不到 {EXE}，先运行 python tools/build_vm.py", file=sys.stderr)
            return 1
        fail = 0
        for t in themes:
            ok = check_one(t)
            print(f"{t.name:24s} {'ok (byte-identical)' if ok else 'FAIL'}")
            fail += 0 if ok else 1
        print(f"[css] {len(themes) - fail}/{len(themes)} round-trip byte-identical")
        return 1 if fail else 0

    for t in themes:
        slug = t.name
        out = Path(args.out) if args.out else ROOT / "temp" / "css" / slug
        css = export(t, out)
        print(f"{t.name:24s} -> {css} ({css.stat().st_size} B)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
