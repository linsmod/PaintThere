"""对呈现端（vm_rdp / demo_jpx）的 /ws 通道做一次握手并发一条输入事件。

为什么需要它：前端 vm_renderer.js 的上行主路径是 WebSocket，HTTP `POST /input`
只是回退。命令行验证只用 curl 的话，覆盖不到 WS 握手、掩码帧与 ping/pong，
所以单独做一个最小客户端（Python 标准库，无第三方依赖）。

用法：
  python tools/probe_ws_input.py --port 19080
  python tools/probe_ws_input.py --port 19080 --kind keydown --key Tab
  python tools/probe_ws_input.py --port 9000 --kind mousedown --x 220 --y 160 --quiet

退出码：0 握手与发送成功；1 失败（连接失败、握手非 101、发送异常）。
"""

import argparse
import base64
import json
import os
import socket
import sys
import time

POINTER_KINDS = {"mousemove", "mousedown", "mouseup", "wheel", "click"}
KEY_KINDS = {"keydown", "keyup"}


def recv_until(sock, marker, limit=4096):
    buf = b""
    while marker not in buf and len(buf) < limit:
        chunk = sock.recv(1024)
        if not chunk:
            break
        buf += chunk
    return buf


def send_text(sock, text):
    payload = text.encode("utf-8")
    mask = os.urandom(4)
    n = len(payload)
    hdr = bytearray([0x81])          # FIN + text
    if n < 126:
        hdr.append(0x80 | n)
    elif n < 65536:
        hdr.append(0x80 | 126)
        hdr += n.to_bytes(2, "big")
    else:
        hdr.append(0x80 | 127)
        hdr += n.to_bytes(8, "big")
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    sock.sendall(bytes(hdr) + mask + masked)


def main():
    ap = argparse.ArgumentParser(description="send one input event over /ws")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--kind", default="mousedown", choices=sorted(POINTER_KINDS | KEY_KINDS))
    ap.add_argument("--x", type=int, default=200)
    ap.add_argument("--y", type=int, default=150)
    ap.add_argument("--button", type=int, default=0)
    ap.add_argument("--key", default="")
    ap.add_argument("--code", default="")
    ap.add_argument("--client-id", default="wsprobe")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    payload = {"type": "input", "kind": args.kind, "clientId": args.client_id, "frameId": 1}
    if args.kind in POINTER_KINDS:
        payload.update({"x": args.x, "y": args.y, "button": args.button})
    else:
        payload.update({"key": args.key, "code": args.code})

    try:
        sock = socket.create_connection((args.host, args.port), timeout=5)
    except OSError as exc:
        print("ws fail: connect %s:%d: %s" % (args.host, args.port, exc), file=sys.stderr)
        return 1

    try:
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            "GET /ws HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
            % (args.host, args.port, key)
        )
        sock.sendall(req.encode())
        resp = recv_until(sock, b"\r\n\r\n")
        first = resp.split(b"\r\n", 1)[0].decode("latin-1") if resp else ""
        if "101" not in first or b"Sec-WebSocket-Accept" not in resp:
            print("ws fail: handshake: %s" % (first or "<no response>"), file=sys.stderr)
            return 1

        send_text(sock, json.dumps(payload))
        time.sleep(0.1)
    finally:
        sock.close()

    if not args.quiet:
        detail = ("x=%d y=%d" % (args.x, args.y)) if args.kind in POINTER_KINDS \
            else ("key=%s" % args.key)
        print("ws ok: 101 switching protocols, sent kind=%s %s" % (args.kind, detail))
    return 0


if __name__ == "__main__":
    sys.exit(main())
