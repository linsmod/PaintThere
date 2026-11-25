#!/usr/bin/env python3
"""验证 vm_rdp 按 clientId 建会话的行为（纯标准库，无第三方依赖）。

覆盖四件事：
  1. 同一 clientId 再开 SSE：旧连接收到 takenOver 后断开，新连接保持；
  2. 同一 clientId 的 SSE 与 WS 属于同一会话，互不驱逐；
  3. 同一 clientId 再开 WS：旧连接收到 close 4001；
  4. 会话表满（默认 8）：新 clientId 直接 503，已在表里的 clientId 重连仍 200。

用法：
  python tools/build_vm.py
  python tools/probe_rdp_session.py --port 19099        # 需先起 vm_rdp（不必连上 vm_wms）

退出码：0 全部通过；1 有失败。
"""

import argparse
import base64
import os
import socket
import sys
import time
import urllib.parse

TAKEOVER_CODE = 4001


class Sse:
    """一条 /events 连接：只读，够验证握手状态与事件内容。"""

    def __init__(self, host, port, client=None, timeout=3.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        path = "/events"
        if client:
            path += "?client=" + urllib.parse.quote(client, safe="")
        req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nAccept: text/event-stream\r\n\r\n"
               % (path, host, port))
        self.sock.sendall(req.encode())
        self.buf = b""
        self.status = self._read_headers()

    def _read_headers(self):
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            self.buf += chunk
        head, _, rest = self.buf.partition(b"\r\n\r\n")
        self.buf = rest
        line = head.split(b"\r\n", 1)[0].decode("latin-1")
        parts = line.split()
        return int(parts[1]) if len(parts) > 1 else 0

    def _fill(self, timeout):
        end = time.time() + timeout
        while True:
            remaining = end - time.time()
            if remaining <= 0:
                return False
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                return False
            if not chunk:
                return False
            self.buf += chunk

    def read_until(self, marker, timeout=2.0):
        end = time.time() + timeout
        while marker not in self.buf:
            remaining = end - time.time()
            if remaining <= 0:
                break
            # 对端可能在同一轮里先发事件再关闭：EOF 之后仍要检查已到的字节。
            if not self._fill(remaining):
                break
        return marker in self.buf

    def closed(self, timeout=2.0):
        """对端是否已关闭连接（期间收到的数据留在 buf 里）。"""
        end = time.time() + timeout
        while True:
            remaining = end - time.time()
            if remaining <= 0:
                return False
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                return False
            if not chunk:
                return True
            self.buf += chunk

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Ws:
    """最小 WebSocket 客户端：握手 + 读取服务端帧（服务端不加掩码）。"""

    def __init__(self, host, port, client=None, timeout=3.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        path = "/ws"
        if client:
            path += "?client=" + urllib.parse.quote(client, safe="")
        req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n" % (path, host, port, key))
        self.sock.sendall(req.encode())
        self.buf = b""
        self.status = self._read_headers()

    def _read_headers(self):
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            self.buf += chunk
        head, _, rest = self.buf.partition(b"\r\n\r\n")
        self.buf = rest
        line = head.split(b"\r\n", 1)[0].decode("latin-1")
        parts = line.split()
        return int(parts[1]) if len(parts) > 1 else 0

    def read_frame(self, timeout=2.0):
        """返回 (opcode, payload)，超时或对端关闭返回 (None, b"")。"""
        end = time.time() + timeout
        while True:
            if len(self.buf) >= 2:
                opcode = self.buf[0] & 0x0F
                plen = self.buf[1] & 0x7F
                hdr = 2
                if plen == 126:
                    if len(self.buf) < 4:
                        pass
                    else:
                        plen = int.from_bytes(self.buf[2:4], "big")
                        hdr = 4
                if len(self.buf) >= hdr + plen:
                    payload = self.buf[hdr:hdr + plen]
                    self.buf = self.buf[hdr + plen:]
                    return opcode, payload
            remaining = end - time.time()
            if remaining <= 0:
                return None, b""
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                return None, b""
            if not chunk:
                return None, b""
            self.buf += chunk

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


FAILS = []


def check(name, ok, detail=""):
    print("%s %s%s" % ("ok  " if ok else "FAIL", name, (" - " + detail) if detail else ""))
    if not ok:
        FAILS.append(name)


def main():
    ap = argparse.ArgumentParser(description="probe vm_rdp client sessions")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--sessions", type=int, default=8, help="服务端会话上限（默认 8）")
    args = ap.parse_args()
    host, port = args.host, args.port
    tag = "p%d" % os.getpid()

    # 1. SSE 接管
    a = Sse(host, port, tag + "-a")
    check("sse open -> 200", a.status == 200, "status=%d" % a.status)
    b = Sse(host, port, tag + "-a")
    check("sse same client -> 200", b.status == 200, "status=%d" % b.status)
    check("old sse got takenOver", a.read_until(b'"takenOver"', 3.0))
    check("old sse closed", a.closed(3.0))
    check("new sse kept open", not b.closed(0.4))
    a.close()

    # 2. 同一会话的 SSE 与 WS 共存
    s = Sse(host, port, tag + "-b")
    w = Ws(host, port, tag + "-b")
    check("sse+ws same client -> 101", w.status == 101, "status=%d" % w.status)
    check("sse not evicted by ws", not s.closed(0.5))
    s.close()
    w.close()
    b.close()
    time.sleep(0.3)   # 让服务端回收会话槽

    # 3. WS 接管
    w1 = Ws(host, port, tag + "-c")
    w2 = Ws(host, port, tag + "-c")
    check("ws same client -> 101", w1.status == 101 and w2.status == 101,
          "status=%d/%d" % (w1.status, w2.status))
    code = None
    for _ in range(4):
        opcode, payload = w1.read_frame(2.0)
        if opcode is None:
            break
        if opcode == 0x8:
            code = int.from_bytes(payload[:2], "big") if len(payload) >= 2 else 0
            break
    check("old ws got close %d" % TAKEOVER_CODE, code == TAKEOVER_CODE,
          "code=%s" % code)
    w1.close()
    w2.close()
    time.sleep(0.3)

    # 4. 会话表满
    held = []
    for i in range(args.sessions):
        c = Sse(host, port, "%s-full-%d" % (tag, i))
        held.append(c)
    check("fill %d sessions" % args.sessions,
          all(c.status == 200 for c in held),
          "status=%s" % ",".join(str(c.status) for c in held))
    extra = Sse(host, port, tag + "-overflow")
    check("overflow new client -> 503", extra.status == 503,
          "status=%d" % extra.status)
    again = Sse(host, port, "%s-full-0" % tag)
    check("known client reconnect -> 200", again.status == 200,
          "status=%d" % again.status)
    extra.close()
    again.close()
    for c in held:
        c.close()
    time.sleep(0.4)

    # 5. 槽位回收
    freed = Sse(host, port, tag + "-after")
    check("slot reused after close -> 200", freed.status == 200,
          "status=%d" % freed.status)
    freed.close()

    if FAILS:
        print("failed: %d (%s)" % (len(FAILS), ", ".join(FAILS)), file=sys.stderr)
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
