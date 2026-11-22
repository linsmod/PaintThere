#include "vm_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define VM_CLOSE closesocket
#define VM_INVALID INVALID_SOCKET
typedef SOCKET vm_socket;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define VM_CLOSE close
#define VM_INVALID (-1)
typedef int vm_socket;
#endif

#define VM_MAX_CLIENTS 16
#define VM_RECV_BUFFER 8192
/* 单帧 JSON 缓冲。窗口内容多时一帧可到几百 KB（tile 切分后每块都带命令），
   1MB 会让整帧被静默丢弃，表现为画面整体不更新。 */
#define VM_FRAME_BUFFER (4 * 1024 * 1024)

typedef struct {
    vm_socket fd;
    int isSse;
    int isWs;
    unsigned int lastSentFrameId;
    char recvBuf[VM_RECV_BUFFER];
    size_t recvLen;
} VmClient;

struct VmServer {
    unsigned short port;
    char webRoot[512];
    vm_socket listenFd;
    VmClient clients[VM_MAX_CLIENTS];
    VmFrameProvider frameProvider;
    void *frameUserData;
    VmInputHandler inputHandler;
    void *inputUserData;
    VmResourceProvider resourceProvider;
    void *resourceUserData;
    VmFullFrameProvider fullFrameProvider;
    void *fullFrameUserData;
    VmTickHandler tickHandler;
    void *tickUserData;
    int tickIntervalMs;
    unsigned long long lastTickMs;
    /* 资源热更新：调用方拷入 JSON，主循环消费后广播 resources-update。 */
    char *pendingResources;
    int stop;
    unsigned int currentFrameId;
    char currentFrame[VM_FRAME_BUFFER];
    int hasFrame;
    /* SSE 事件缓冲：堆上按需增长，避免每轮循环在栈上放 1MB。 */
    char *sseEvent;
    size_t sseEventCap;
};

static void vm_poll_frames(VmServer *s);
static void vm_push_frames(VmServer *s);

static unsigned long long vm_now_ms(void) {
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
#endif
}

static void vm_server_tick(VmServer *s) {
    if (!s->tickHandler || s->tickIntervalMs <= 0) return;
    unsigned long long now = vm_now_ms();
    if (s->lastTickMs != 0 && now - s->lastTickMs < (unsigned long long)s->tickIntervalMs) return;
    s->lastTickMs = now;
    s->tickHandler(s->tickUserData);
}

/* Minimal HTTP index: loads the real app.js primitives from webRoot. */
/* HTML 只加载 JS；所有 DOM/样式由 vm_renderer.js 统一创建，与 C 宿主可比。 */
static const char VM_INDEX_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>VM Frame</title>"
    "<script type='module' src='/vm_renderer.js'></script></head><body></body></html>";

/* 非阻塞：SSE 写不能卡住单线程主循环（慢客户端会在阻塞 send 上挂死整个服务）。 */
static int vm_set_nonblocking(vm_socket fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

/* 返回 1 表示整段写完；非阻塞 socket 上写不动（EWOULDBLOCK）按失败处理。 */
static int vm_send_all(vm_socket fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send(fd, data + sent, (int)(len - sent), 0);
#else
        ssize_t n = send(fd, data + sent, len - sent, 0);
#endif
        if (n <= 0) return 0;
        sent += (size_t)n;
    }
    return 1;
}

static int vm_send_text(vm_socket fd, const char *text) {
    return vm_send_all(fd, text, strlen(text));
}

/* ---------------- WebSocket 上行输入通道 ----------------
   下行帧仍走 SSE；上行 UI 事件走 WS，避免每个事件一个 HTTP 连接和头部开销。 */

#ifdef _WIN32
#define vm_strnicmp _strnicmp
#else
#define vm_strnicmp strncasecmp
#endif

typedef struct {
    unsigned int h[5];
    unsigned long long total;
    unsigned char buf[64];
    size_t bufLen;
} VmSha1;

static unsigned int vm_rol32(unsigned int v, int bits) {
    return (v << bits) | (v >> (32 - bits));
}

static void vm_sha1_init(VmSha1 *c) {
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->total = 0;
    c->bufLen = 0;
}

static void vm_sha1_block(VmSha1 *c, const unsigned char *p) {
    unsigned int w[80];
    unsigned int a, b, d, e, f;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned int)p[i * 4] << 24) | ((unsigned int)p[i * 4 + 1] << 16) |
               ((unsigned int)p[i * 4 + 2] << 8) | (unsigned int)p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = vm_rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = c->h[0]; b = c->h[1]; d = c->h[2]; e = c->h[3]; f = c->h[4];
    for (i = 0; i < 80; i++) {
        unsigned int t, k, tmp;
        if (i < 20) { t = (b & d) | ((~b) & e); k = 0x5A827999u; }
        else if (i < 40) { t = b ^ d ^ e; k = 0x6ED9EBA1u; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e); k = 0x8F1BBCDCu; }
        else { t = b ^ d ^ e; k = 0xCA62C1D6u; }
        tmp = vm_rol32(a, 5) + t + f + k + w[i];
        f = e; e = d; d = vm_rol32(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

static void vm_sha1_update(VmSha1 *c, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    c->total += len;
    while (len > 0) {
        size_t take = 64 - c->bufLen;
        if (take > len) take = len;
        memcpy(c->buf + c->bufLen, p, take);
        c->bufLen += take;
        p += take;
        len -= take;
        if (c->bufLen == 64) {
            vm_sha1_block(c, c->buf);
            c->bufLen = 0;
        }
    }
}

static void vm_sha1_final(VmSha1 *c, unsigned char out[20]) {
    unsigned long long bits = c->total * 8;
    unsigned char pad = 0x80;
    unsigned char zero = 0;
    unsigned char lenBytes[8];
    int i;
    vm_sha1_update(c, &pad, 1);
    while (c->bufLen != 56) vm_sha1_update(c, &zero, 1);
    for (i = 0; i < 8; i++) lenBytes[i] = (unsigned char)(bits >> (56 - i * 8));
    vm_sha1_update(c, lenBytes, 8);
    for (i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->h[i];
    }
}

static size_t vm_base64(const unsigned char *in, size_t len, char *out, size_t cap) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    while (i + 2 < len && o + 5 < cap) {
        unsigned int v = ((unsigned int)in[i] << 16) | ((unsigned int)in[i + 1] << 8) | in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];
        out[o++] = tbl[v & 63];
        i += 3;
    }
    if (i < len && o + 5 < cap) {
        unsigned int v = (unsigned int)in[i] << 16;
        if (i + 1 < len) v |= (unsigned int)in[i + 1] << 8;
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

/* 大小写不敏感地查 HTTP 头，返回值起点。 */
static const char *vm_find_header(const char *req, const char *name) {
    size_t nlen = strlen(name);
    const char *p = req;
    while (*p) {
        if ((p == req || p[-1] == '\n') && vm_strnicmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ') v++;
            return v;
        }
        p++;
    }
    return NULL;
}

/* 计算 Sec-WebSocket-Accept。 */
static int vm_ws_accept(const char *key, char *out, size_t cap) {
    static const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char src[192];
    unsigned char digest[20];
    VmSha1 sha;
    size_t klen = 0, glen = strlen(guid);
    while (key[klen] && key[klen] != '\r' && key[klen] != '\n' &&
           klen + glen + 1 < sizeof(src)) klen++;
    memcpy(src, key, klen);
    memcpy(src + klen, guid, glen);
    vm_sha1_init(&sha);
    vm_sha1_update(&sha, src, klen + glen);
    vm_sha1_final(&sha, digest);
    return vm_base64(digest, sizeof(digest), out, cap) > 0;
}

static int vm_ws_send_frame(vm_socket fd, int opcode, const unsigned char *payload, size_t len) {
    unsigned char frame[8 + 128];
    size_t n = 0;
    if (len > 125) len = 125;
    frame[n++] = (unsigned char)(0x80 | (opcode & 0x0F));
    frame[n++] = (unsigned char)len;
    if (len) {
        memcpy(frame + n, payload, len);
        n += len;
    }
    return vm_send_all(fd, (const char *)frame, n);
}

static void vm_ws_close(VmClient *c) {
    if (c->fd != VM_INVALID) {
        VM_CLOSE(c->fd);
        c->fd = VM_INVALID;
    }
    c->isWs = 0;
    c->recvLen = 0;
}

/* 解析客户端帧（浏览器帧必须 mask），文本帧交给 inputHandler。 */
static void vm_ws_consume(VmServer *s, VmClient *c) {
    size_t off = 0;
    while (c->fd != VM_INVALID && c->recvLen - off >= 2) {
        const unsigned char *b = (const unsigned char *)c->recvBuf + off;
        int fin = (b[0] & 0x80) != 0;
        int opcode = b[0] & 0x0F;
        int masked = (b[1] & 0x80) != 0;
        unsigned long long plen = (unsigned long long)(b[1] & 0x7F);
        size_t hdr = 2;
        unsigned char *payload;
        if (plen == 126) {
            if (c->recvLen - off < 4) break;
            plen = ((unsigned long long)b[2] << 8) | (unsigned long long)b[3];
            hdr = 4;
        } else if (plen == 127) {
            vm_ws_close(c); /* 超长帧不是输入事件，直接断开 */
            return;
        }
        if (masked) hdr += 4;
        if (c->recvLen - off < hdr + (size_t)plen) break;
        payload = (unsigned char *)c->recvBuf + off + hdr;
        if (masked) {
            const unsigned char *m = (const unsigned char *)c->recvBuf + off + hdr - 4;
            size_t i;
            for (i = 0; i < (size_t)plen; i++) payload[i] = (unsigned char)(payload[i] ^ m[i & 3]);
        }
        if (opcode == 0x8) {
            vm_ws_close(c);
            return;
        }
        if (opcode == 0x9) {
            vm_ws_send_frame(c->fd, 0x0A, payload, (size_t)plen);
        } else if (opcode == 0x1 && fin && plen > 0 && s->inputHandler) {
            /* payload 不是 C 字符串：它后面可能是下一个帧或缓冲残留。不补 '\0' 时，
               下层的 strstr 会读到别的帧，把 x/y 解析成默认值 -1，指针就跳到固定位置。 */
            char message[VM_RECV_BUFFER + 1];
            if (plen <= VM_RECV_BUFFER) {
                memcpy(message, payload, (size_t)plen);
                message[plen] = '\0';
                s->inputHandler(message, (size_t)plen, s->inputUserData);
            }
            /* 每条输入立即出一帧：Clay 的 PRESSED_THIS_FRAME 只在某一帧布局里成立。
               down/up 若在同一批数据里处理完，按钮点击会被吞掉。 */
            vm_poll_frames(s);
            vm_push_frames(s);
        }
        off += hdr + (size_t)plen;
    }
    if (off > 0) {
        memmove(c->recvBuf, c->recvBuf + off, c->recvLen - off);
        c->recvLen -= off;
    }
}

static void vm_send_response(vm_socket fd, int code, const char *reason,
                             const char *contentType, const char *body, size_t len) {
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n",
                     code, reason, contentType, len);
    if (n <= 0) return;
    vm_send_all(fd, header, (size_t)n);
    if (len) vm_send_all(fd, body, len);
}

static int vm_safe_join(char *out, size_t cap, const char *root, const char *name) {
    if (strcmp(name, "app.js") != 0 && strcmp(name, "systemColors.js") != 0 &&
        strcmp(name, "index.html") != 0 && strcmp(name, "vm_renderer.js") != 0 &&
        strcmp(name, "font_wasm.js") != 0 &&
        strcmp(name, "stb_font_wasm.mjs") != 0 && strcmp(name, "stb_font_wasm.wasm") != 0 &&
        strcmp(name, "font.ttf") != 0) return 0;
    int n = snprintf(out, cap, "%s/%s", root, name);
    return n > 0 && (size_t)n < cap;
}

static void vm_serve_static(VmServer *s, vm_socket fd, const char *path) {
    const char *name = NULL;
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        vm_send_response(fd, 200, "OK", "text/html; charset=utf-8",
                         VM_INDEX_HTML, sizeof(VM_INDEX_HTML) - 1);
        return;
    }
    if (strcmp(path, "/vm_renderer.js") == 0) name = "vm_renderer.js";
    else if (strcmp(path, "/app.js") == 0) name = "app.js";
    else if (strcmp(path, "/systemColors.js") == 0) name = "systemColors.js";
    else if (strcmp(path, "/font_wasm.js") == 0) name = "font_wasm.js";
    else if (strcmp(path, "/stb_font_wasm.mjs") == 0) name = "stb_font_wasm.mjs";
    else if (strcmp(path, "/stb_font_wasm.wasm") == 0) name = "stb_font_wasm.wasm";
    else if (strcmp(path, "/font.ttf") == 0) name = "font.ttf";
    if (!name) {
        vm_send_response(fd, 404, "Not Found", "text/plain", "not found", 9);
        return;
    }
    char full[640];
    if (!vm_safe_join(full, sizeof(full), s->webRoot, name)) {
        vm_send_response(fd, 403, "Forbidden", "text/plain", "forbidden", 9);
        return;
    }
    FILE *f = fopen(full, "rb");
    if (!f) {
        vm_send_response(fd, 404, "Not Found", "text/plain", "not found", 9);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* 中文字体（如 SimHei 9.7MB）需要更大的静态资源上限；
       这是本地演示服务，只提供白名单内的几个静态文件，提高上限无外部风险。 */
    if (size < 0 || size > 16 * 1024 * 1024) {
        fclose(f);
        vm_send_response(fd, 413, "Payload Too Large", "text/plain", "too large", 9);
        return;
    }
    char *body = (char *)malloc((size_t)size ? (size_t)size : 1);
    if (!body) {
        fclose(f);
        vm_send_response(fd, 500, "Internal Error", "text/plain", "oom", 3);
        return;
    }
    size_t readLen = fread(body, 1, (size_t)size, f);
    fclose(f);
    const char *ct = "text/plain; charset=utf-8";
    if (strstr(name, ".js") || strstr(name, ".mjs")) ct = "text/javascript; charset=utf-8";
    else if (strstr(name, ".wasm")) ct = "application/wasm";
    vm_send_response(fd, 200, "OK", ct, body, readLen);
    free(body);
}

static void vm_handle_request(VmServer *s, VmClient *client) {
    char *req = client->recvBuf;
    char *headerEnd = strstr(req, "\r\n\r\n");
    if (!headerEnd) return;

    char method[16] = {0}, path[256] = {0};
    if (sscanf(req, "%15s %255s", method, path) != 2) {
        vm_send_response(client->fd, 400, "Bad Request", "text/plain", "bad request", 11);
        VM_CLOSE(client->fd);
        client->fd = VM_INVALID;
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/events") == 0) {
        if (!vm_send_text(client->fd,
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/event-stream\r\n"
                          "Cache-Control: no-cache\r\n"
                          "Connection: keep-alive\r\n\r\n")) {
            VM_CLOSE(client->fd);
            client->fd = VM_INVALID;
            return;
        }
        client->isSse = 1;
        client->lastSentFrameId = 0xFFFFFFFFu; /* force initial frame (frameId may be 0) */
        client->recvLen = 0;
        /* 初始事件（resources + fullframe）仍在阻塞模式下发完，保证新客户端拿到完整基线。 */
        if (s->resourceProvider) {
            const char *res = s->resourceProvider(s->resourceUserData);
            if (res && *res) {
                size_t need = strlen(res) + 32;
                char *event = (char *)malloc(need);
                if (event) {
                    int n = snprintf(event, need, "event: resources\ndata: %s\n\n", res);
                    if (n > 0) vm_send_all(client->fd, event, (size_t)n);
                    free(event);
                }
            }
        }
        if (s->fullFrameProvider) {
            const char *full = s->fullFrameProvider(s->fullFrameUserData);
            if (full && *full) {
                size_t need = strlen(full) + 32;
                char *event = (char *)malloc(need);
                if (event) {
                    int n = snprintf(event, need, "event: fullframe\ndata: %s\n\n", full);
                    if (n > 0) vm_send_all(client->fd, event, (size_t)n);
                    free(event);
                }
            }
        }
        /* 基线发完后转非阻塞：后续帧推送永不阻塞主循环。 */
        if (vm_set_nonblocking(client->fd) != 0) {
            VM_CLOSE(client->fd);
            client->fd = VM_INVALID;
            return;
        }
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/ws") == 0) {
        const char *key = vm_find_header(req, "Sec-WebSocket-Key");
        char accept[64];
        char resp[512];
        int n;
        if (!key || !vm_ws_accept(key, accept, sizeof(accept))) {
            vm_send_response(client->fd, 400, "Bad Request", "text/plain", "bad ws", 6);
            VM_CLOSE(client->fd);
            client->fd = VM_INVALID;
            return;
        }
        n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
        if (n <= 0 || !vm_send_all(client->fd, resp, (size_t)n)) {
            VM_CLOSE(client->fd);
            client->fd = VM_INVALID;
            return;
        }
        client->isWs = 1;
        /* 握手请求之后可能已经跟着帧数据：保留并按帧解析。 */
        {
            size_t consumed = (size_t)(headerEnd + 4 - req);
            if (consumed < client->recvLen) {
                memmove(client->recvBuf, headerEnd + 4, client->recvLen - consumed);
                client->recvLen -= consumed;
            } else {
                client->recvLen = 0;
            }
        }
        vm_ws_consume(s, client);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/input") == 0) {
        char *body = headerEnd + 4;
        size_t bodyLen = client->recvLen - (size_t)(body - req);
        if (s->inputHandler && bodyLen <= VM_RECV_BUFFER) {
            /* 同样补 '\0'：body 之后可能是残留字节，不能让解析读到界外。 */
            char message[VM_RECV_BUFFER + 1];
            memcpy(message, body, bodyLen);
            message[bodyLen] = '\0';
            s->inputHandler(message, bodyLen, s->inputUserData);
        }
        /* HTTP 回退路径同样立即出帧，保持与 WebSocket 路径一致的点击时序。 */
        vm_poll_frames(s);
        vm_push_frames(s);
        vm_send_response(client->fd, 200, "OK", "application/json", "{\"ok\":true}", 11);
        VM_CLOSE(client->fd);
        client->fd = VM_INVALID;
        return;
    }

    if (strcmp(method, "GET") == 0) {
        vm_serve_static(s, client->fd, path);
        VM_CLOSE(client->fd);
        client->fd = VM_INVALID;
        return;
    }

    vm_send_response(client->fd, 405, "Method Not Allowed", "text/plain", "method not allowed", 18);
    VM_CLOSE(client->fd);
    client->fd = VM_INVALID;
}

/* 客户端槽位满时仍要 accept：否则连接留在 backlog 里，对端一直空等。
   回一条专用控制事件并立刻断开，客户端按 retryMs 退避重连。 */
static void vm_reject_full(vm_socket fd) {
    static const char body[] = "{\"op\":\"serverFull\",\"retryMs\":2000}";
    char resp[512];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/event-stream\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Connection: close\r\n\r\n"
                     "event: control\ndata: %s\n\n",
                     body);
    if (n > 0) vm_send_all(fd, resp, (size_t)n);
    VM_CLOSE(fd);
}

static void vm_accept(VmServer *s) {
    int slot = -1;
    for (int i = 0; i < VM_MAX_CLIENTS; i++) {
        if (s->clients[i].fd == VM_INVALID) { slot = i; break; }
    }
    struct sockaddr_in addr;
#ifdef _WIN32
    int addrLen = sizeof(addr);
#else
    socklen_t addrLen = sizeof(addr);
#endif
    vm_socket fd = accept(s->listenFd, (struct sockaddr *)&addr, &addrLen);
    if (fd == VM_INVALID) return;
    if (slot < 0) {
        vm_reject_full(fd);
        return;
    }
    memset(&s->clients[slot], 0, sizeof(s->clients[slot]));
    s->clients[slot].fd = fd;
}

VmServer *VmServer_Create(unsigned short port, const char *webRoot) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;
#endif
    VmServer *s = (VmServer *)calloc(1, sizeof(VmServer));
    if (!s) return NULL;
    s->port = port;
    snprintf(s->webRoot, sizeof(s->webRoot), "%s", webRoot ? webRoot : ".");
    for (int i = 0; i < VM_MAX_CLIENTS; i++) s->clients[i].fd = VM_INVALID;

    s->listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listenFd == VM_INVALID) {
        free(s);
        return NULL;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s->listenFd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s->listenFd, 8) != 0) {
        VM_CLOSE(s->listenFd);
        free(s);
        return NULL;
    }
    return s;
}

void VmServer_Free(VmServer *s) {
    if (!s) return;
    for (int i = 0; i < VM_MAX_CLIENTS; i++) {
        if (s->clients[i].fd != VM_INVALID) {
            VM_CLOSE(s->clients[i].fd);
            s->clients[i].fd = VM_INVALID;
        }
    }
    if (s->listenFd != VM_INVALID) VM_CLOSE(s->listenFd);
    free(s->sseEvent);
    free(s->pendingResources);
    free(s);
#ifdef _WIN32
    WSACleanup();
#endif
}

void VmServer_SetFrameProvider(VmServer *s, VmFrameProvider provider, void *userData) {
    if (!s) return;
    s->frameProvider = provider;
    s->frameUserData = userData;
}

void VmServer_SetInputHandler(VmServer *s, VmInputHandler handler, void *userData) {
    if (!s) return;
    s->inputHandler = handler;
    s->inputUserData = userData;
}

void VmServer_SetResourceProvider(VmServer *s, VmResourceProvider provider, void *userData) {
    if (!s) return;
    s->resourceProvider = provider;
    s->resourceUserData = userData;
}

void VmServer_SetFullFrameProvider(VmServer *s, VmFullFrameProvider provider, void *userData) {
    if (!s) return;
    s->fullFrameProvider = provider;
    s->fullFrameUserData = userData;
}

void VmServer_SetTickHandler(VmServer *s, int intervalMs, VmTickHandler handler, void *userData) {
    if (!s) return;
    s->tickHandler = handler;
    s->tickUserData = userData;
    s->tickIntervalMs = intervalMs;
    s->lastTickMs = 0;
}

void VmServer_BroadcastResources(VmServer *s, const char *json) {
    if (!s || !json || !*json) return;
    free(s->pendingResources);
    s->pendingResources = (char *)malloc(strlen(json) + 1);
    if (s->pendingResources) strcpy(s->pendingResources, json);
}

void VmServer_Stop(VmServer *s) {
    if (s) s->stop = 1;
}

static void vm_poll_frames(VmServer *s) {
    if (!s->frameProvider) return;
    unsigned int id = 0;
    const char *json = s->frameProvider(s->frameUserData, &id);
    if (!json) return;
    size_t len = strlen(json);
    if (len >= sizeof(s->currentFrame)) return;
    if (s->hasFrame && id == s->currentFrameId) return;
    memcpy(s->currentFrame, json, len + 1);
    s->currentFrameId = id;
    s->hasFrame = 1;
}

static void vm_push_frames(VmServer *s) {
    if (!s->hasFrame) return;
    size_t need = strlen(s->currentFrame) + 16;
    if (need > s->sseEventCap) {
        char *grown = (char *)realloc(s->sseEvent, need);
        if (!grown) return;
        s->sseEvent = grown;
        s->sseEventCap = need;
    }
    int n = snprintf(s->sseEvent, s->sseEventCap, "data: %s\n\n", s->currentFrame);
    if (n <= 0) return;
    size_t len = (size_t)n < s->sseEventCap ? (size_t)n : s->sseEventCap - 1;
    for (int i = 0; i < VM_MAX_CLIENTS; i++) {
        VmClient *c = &s->clients[i];
        if (c->fd == VM_INVALID || !c->isSse) continue;
        if (c->lastSentFrameId == s->currentFrameId) continue;
        if (!vm_send_all(c->fd, s->sseEvent, len)) {
            /* 慢客户端：断开让它重连补 fullframe，绝不让阻塞写拖死主循环 */
            VM_CLOSE(c->fd);
            c->fd = VM_INVALID;
            c->isSse = 0;
            continue;
        }
        c->lastSentFrameId = s->currentFrameId;
    }
}

int VmServer_Run(VmServer *s) {
    if (!s) return 1;
    for (;;) {
        if (s->stop) return 0;
        vm_server_tick(s);
        vm_poll_frames(s);
        vm_push_frames(s);
        /* 资源热更新：一次性向所有已连接客户端广播，然后清空待发送缓冲。 */
        if (s->pendingResources) {
            size_t need = strlen(s->pendingResources) + 64;
            char *event = (char *)malloc(need);
            if (event) {
                int n = snprintf(event, need, "event: resources-update\ndata: %s\n\n",
                                 s->pendingResources);
                if (n > 0) {
                    for (int i = 0; i < VM_MAX_CLIENTS; i++) {
                        if (s->clients[i].fd != VM_INVALID && s->clients[i].isSse)
                            vm_send_all(s->clients[i].fd, event, (size_t)n);
                    }
                }
                free(event);
            }
            free(s->pendingResources);
            s->pendingResources = NULL;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s->listenFd, &readfds);
        vm_socket maxFd = s->listenFd;
        for (int i = 0; i < VM_MAX_CLIENTS; i++) {
            /* SSE 客户端也必须监听：它不发数据，可读只意味着对端已断开，
               不检测就会漏槽位，最终新页面连不上、curl 一直空等。 */
            if (s->clients[i].fd == VM_INVALID) continue;
            FD_SET(s->clients[i].fd, &readfds);
            if (s->clients[i].fd > maxFd) maxFd = s->clients[i].fd;
        }
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 20000;
        int ready = select((int)maxFd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) return 2;
        if (ready == 0) continue;

        if (FD_ISSET(s->listenFd, &readfds)) vm_accept(s);

        for (int i = 0; i < VM_MAX_CLIENTS; i++) {
            VmClient *c = &s->clients[i];
            if (c->fd == VM_INVALID || !FD_ISSET(c->fd, &readfds)) continue;
#ifdef _WIN32
            int n = recv(c->fd, c->recvBuf + c->recvLen, (int)(VM_RECV_BUFFER - c->recvLen), 0);
#else
            ssize_t n = recv(c->fd, c->recvBuf + c->recvLen, VM_RECV_BUFFER - c->recvLen, 0);
#endif
            if (n <= 0) {
                /* 包含 SSE 客户端：对端关闭或出错时就地回收槽位。 */
                VM_CLOSE(c->fd);
                c->fd = VM_INVALID;
                c->isSse = 0;
                c->isWs = 0;
                continue;
            }
            if (c->isSse) {
                /* SSE 只下行，读到什么都丢弃，只用于发现断开。 */
                c->recvLen = 0;
                continue;
            }
            c->recvLen += (size_t)n;
            if (c->recvLen >= VM_RECV_BUFFER) {
                if (c->isWs) {
                    vm_ws_close(c);
                } else {
                    vm_send_response(c->fd, 413, "Payload Too Large", "text/plain", "too large", 9);
                    VM_CLOSE(c->fd);
                    c->fd = VM_INVALID;
                }
                continue;
            }
            if (c->isWs) vm_ws_consume(s, c);
            else vm_handle_request(s, c);
        }
    }
}
