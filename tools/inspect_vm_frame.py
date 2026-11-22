"""回放一段 SSE 流（由 drive_vm_desktop.py 的 snap 抓取），按前端语义
维护 48 块瓦片的当前内容，再把 tile 局部坐标还原成画布绝对坐标。

用途：前端画对了没有、某段文字落在哪个坐标、一次交互改了哪些瓦片。
SSE 里 fullframe 带全量、增量帧只有 data:（无 event: 头），两者都要吃。

用法：python tools/inspect_vm_frame.py <抓包文件> [--text 关键词] [--tiles] [--hist]
  --text K   只列包含 K 的文本
  --tiles    打印每块瓦片的命令条数
  --hist     打印每次 setTile 的块号序列（看增量帧动了哪些块）
"""
import json
import sys

PATH = sys.argv[1]
COLS, ROWS, TW, TH = 8, 6, 100, 100
only = None
if "--text" in sys.argv:
    only = sys.argv[sys.argv.index("--text") + 1]

raw = open(PATH, "rb").read().decode("utf-8", "replace")

events = []
event_name = None
data_lines = []
for line in raw.splitlines():
    if line.startswith("event:"):
        event_name = line[6:].strip()
    elif line.startswith("data:"):
        data_lines.append(line[5:].strip())
    elif line == "":
        # 增量帧没有 event: 头，只有 data:（见 vm_server.c 的 vm_push_frames）
        if data_lines:
            payload = "\n".join(data_lines)
            try:
                events.append((event_name or "message", json.loads(payload)))
            except json.JSONDecodeError:
                print("bad json in event %s" % (event_name or "message"))
        event_name = None
        data_lines = []

tiles = {}
hist = []
for ev, p in events:
    if not isinstance(p, dict):
        continue
    ops = p.get("ops")
    if not isinstance(ops, list):
        continue
    if ev == "fullframe":
        tiles = {}
    applied = []
    for o in ops:
        op = o.get("op")
        if op == "setTile":
            tiles[o.get("index", 0)] = o.get("cmds", [])
            applied.append(o.get("index", 0))
        elif op == "config":
            globals()["TW"] = o.get("tileW", TW)
            globals()["TH"] = o.get("tileH", TH)
            globals()["COLS"] = o.get("cols", COLS)
    if applied:
        hist.append((ev, p.get("frameId"), sorted(applied)))

print("events=%d  setTile 批次=%d  当前瓦片=%d" % (len(events), len(hist), len(tiles)))
if "--hist" in sys.argv:
    for ev, fid, idxs in hist:
        print("  %-9s frame=%-4s tiles=%s" % (ev, fid, idxs))

if "--tiles" in sys.argv:
    for idx in sorted(tiles):
        print("  tile%-2d cmds=%d" % (idx, len(tiles[idx])))

for idx in sorted(tiles):
    ox, oy = (idx % COLS) * TW, (idx // COLS) * TH
    for c in tiles[idx]:
        cmd = c.get("cmd")
        args = c.get("args") or []
        if cmd == "drawText":
            text = args[4] if len(args) > 4 else "?"
            if only and only not in text:
                continue
            print("  tile%-2d abs(%4d,%4d) text=%r" % (idx, ox + args[0], oy + args[1], text))
        elif cmd in ("clipPush", "clipPop") and "--ids" in sys.argv:
            print("  tile%-2d abs(%4d,%4d) %s id=%s" % (idx, ox + args[0], oy + args[1],
                                                        cmd, c.get("id")))
