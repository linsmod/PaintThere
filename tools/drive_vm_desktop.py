"""用 HTTP /input 合成指针事件驱动 VM 桌面（vm_rdp 呈现端），
同时保持一条 SSE 连接累积帧流，供 inspect_vm_frame.py 回放。

为什么不用浏览器点击：这里要的是「确定的逻辑坐标 + 精确时序」，
浏览器点击受 CSS 缩放与 DPR 影响，且拿不到逐帧快照。

用法：
  python tools/drive_vm_desktop.py <port> <action> [<action> ...]
动作：
  move X Y      鼠标移动
  down X Y      移动 + 左键按下（中间留一帧，保证 Clay 有 hover 帧）
  up X Y        左键抬起
  key K         按键
  snap NAME     把「目前收到的全部 SSE 数据」写到 $VM_DRIVE_OUT/NAME.txt
                （默认 temp/，连接保持，可多次抓取）
  wait MS       等待
  alive         打印服务是否可达

注意：POST /input 的 body 必须与请求头在同一次 send 里发出，
服务端按单次 recv 取 body（见 rdp/rdp_server.c 的 POST 分支）。
"""
import os
import re
import socket
import sys
import threading
import time

PORT = int(sys.argv[1])
CID = "diag"
OUT_DIR = os.environ.get("VM_DRIVE_OUT", "temp")   # snap 的落盘目录
buf = bytearray()
lock = threading.Lock()
status = {"connected": False, "closed": False}
frame_times = []   # (monotonic, bytes) at each SSE data: frame boundary


def fetch_boot():
    """GET /boot：/events 与 /ws 认这个门票，服务进程重启即失效。"""
    s = socket.socket()
    s.settimeout(3)
    data = b""
    try:
        s.connect(("127.0.0.1", PORT))
        s.sendall(b"GET /boot HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    finally:
        s.close()
    _, _, body = data.partition(b"\r\n\r\n")
    return body.decode("ascii", "replace").strip()


def recorder():
    s = socket.socket()
    s.settimeout(0.3)
    try:
        s.connect(("127.0.0.1", PORT))
    except OSError as e:
        print("sse connect failed: %s" % e)
        return
    status["connected"] = True
    boot = fetch_boot()
    req = ("GET /events?client=%s&boot=%s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
           "Accept: text/event-stream\r\n\r\n" % (CID, boot))
    s.sendall(req.encode("ascii"))
    while not status["closed"]:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            break
        with lock:
            buf.extend(chunk)
            text = chunk.decode("utf-8", "replace")
            n = text.count("\n\n")
            if n:
                now = time.monotonic()
                for _ in range(n):
                    frame_times.append((now, len(chunk)))
    s.close()


def post(body):
    """裸 socket 一次性发出请求：服务端按单次 recv 取 body。"""
    data = body.encode("utf-8")
    req = ("POST /input HTTP/1.1\r\nHost: 127.0.0.1\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: %d\r\nConnection: close\r\n\r\n" % len(data))
    s = socket.socket()
    s.settimeout(3)
    try:
        s.connect(("127.0.0.1", PORT))
        s.sendall(req.encode("ascii") + data)
        resp = s.recv(4096)
        m = re.match(rb"HTTP/1\.1 (\d+)", resp)
        return m.group(1).decode() if m else "no-status"
    except Exception as e:  # noqa: BLE001
        return "ERR %s" % e
    finally:
        s.close()


def alive():
    s = socket.socket()
    s.settimeout(0.5)
    try:
        s.connect(("127.0.0.1", PORT))
        return True
    except OSError:
        return False
    finally:
        s.close()


def mouse(kind, x, y, button=0):
    body = ('{"type":"input","kind":"%s","x":%d,"y":%d,"button":%d,'
            '"clientId":"%s","color":"#ff0000","frameId":1}'
            % (kind, x, y, button, CID))
    print("  [%8.3f] %-9s (%d,%d) button=%d -> %s"
          % (time.monotonic(), kind, x, y, button, post(body)))
    sys.stdout.flush()


def snap(name):
    with lock:
        data = bytes(buf)
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, "%s.txt" % name)
    with open(path, "wb") as f:
        f.write(data)
    text = data.decode("utf-8", "replace")
    marks = ["fullframe", "setTile", "w32512_body", "开始"]
    print("  snap %s: %d bytes, %s" % (
        name, len(data), ", ".join("%s=%d" % (m, text.count(m)) for m in marks)))
    sys.stdout.flush()


def main():
    t = threading.Thread(target=recorder, daemon=True)
    t.start()
    for _ in range(50):
        if status["connected"]:
            break
        time.sleep(0.05)

    args = sys.argv[2:]
    i = 0
    while i < len(args):
        a = args[i]
        if a == "move":
            mouse("mousemove", int(args[i + 1]), int(args[i + 2]))
            i += 3
        elif a == "down":
            x, y = int(args[i + 1]), int(args[i + 2])
            mouse("mousemove", x, y)
            time.sleep(0.12)
            mouse("mousedown", x, y)
            i += 3
        elif a == "up":
            mouse("mouseup", int(args[i + 1]), int(args[i + 2]))
            i += 3
        elif a == "rdown":
            x, y = int(args[i + 1]), int(args[i + 2])
            mouse("mousemove", x, y, 2)
            time.sleep(0.08)
            mouse("mousedown", x, y, 2)
            i += 3
        elif a == "rup":
            mouse("mouseup", int(args[i + 1]), int(args[i + 2]), 2)
            i += 3
        elif a == "times":
            with lock:
                recent = frame_times[-20:]
            for t, n in recent:
                print("  frame @%8.3f chunk=%d" % (t, n))
            sys.stdout.flush()
            i += 1
        elif a == "key":
            body = ('{"type":"input","kind":"keydown","key":"%s","code":"%s",'
                    '"clientId":"%s","frameId":1}' % (args[i + 1], args[i + 1], CID))
            print("  key %-6s -> %s" % (args[i + 1], post(body)))
            i += 2
        elif a == "snap":
            snap(args[i + 1])
            i += 2
        elif a == "wait":
            time.sleep(int(args[i + 1]) / 1000.0)
            i += 2
        elif a == "alive":
            print("  alive=%s" % alive())
            i += 1
        else:
            print("unknown action %r" % a)
            return 2

    time.sleep(0.3)
    status["closed"] = True
    time.sleep(0.1)
    print("  alive=%s" % alive())
    return 0


if __name__ == "__main__":
    sys.exit(main())
