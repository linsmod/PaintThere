"""从系统字体集合（TTC）提取单个字体为 TTF，供 demo --font 使用。

为什么需要：Windows 的中文宋体只有 simsun.ttc（TTC 集合，18 MB），而 guest 与
前端共用的 stb_truetype 路径按「单个 sfnt」读取；TTC 支持需要重编 wasm 工具链。
提取成独立 TTF 后，现有 wasm 与 vm_font 都能直接加载，且可子集化控制体积。

用法：
  python tools/extract_system_font.py simsun              # 原样提取
  python tools/extract_system_font.py simsun --subset     # 裁剪到 ASCII + GB2312
  python tools/extract_system_font.py C:/path/x.ttc --index 1

输出：temp/fonts/<name>.ttf（temp/ 不受 git 管理；系统字体有版权，不进仓库）
"""

import argparse
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "temp", "fonts")
FONTS_DIR = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")


def gb2312_chars() -> set[str]:
    """GB2312 全部符号与汉字：够覆盖界面文案，又比整个宋体小得多。"""
    out: set[str] = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                out.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source", help="系统字体名（simsun / simsun.ttc）或绝对路径")
    ap.add_argument("--index", type=int, default=0, help="TTC 内的字体序号，默认 0")
    ap.add_argument("--subset", action="store_true", help="裁剪到 ASCII + GB2312")
    ap.add_argument("--out", default=None, help="输出路径，默认 temp/fonts/<name>.ttf")
    args = ap.parse_args()

    src = args.source
    if not os.path.isabs(src):
        if not os.path.splitext(src)[1]:
            src += ".ttc"
        src = os.path.join(FONTS_DIR, src)
    if not os.path.exists(src):
        print("missing font:", src)
        return 1

    os.makedirs(OUT_DIR, exist_ok=True)
    dst = args.out or os.path.join(OUT_DIR, os.path.splitext(os.path.basename(src))[0] + ".ttf")

    font = TTFont(src, fontNumber=args.index)
    if args.subset:
        text = "".join(chr(c) for c in range(0x20, 0x7F)) + "".join(sorted(gb2312_chars()))
        opts = subset.Options()
        opts.name_IDs = ["*"]
        opts.name_legacy = True
        opts.notdef_outline = True
        opts.recalc_bounds = True
        sub = subset.Subsetter(options=opts)
        sub.populate(text=text)
        sub.subset(font)

    font.save(dst)
    print("wrote %s (%.1f MB)" % (dst, os.path.getsize(dst) / 1048576))
    return 0


if __name__ == "__main__":
    sys.exit(main())
