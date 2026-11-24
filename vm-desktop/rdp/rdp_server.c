/*
 * rdp_server.c - 呈现端 HTTP/SSE 服务（libuv 实现）
 *
 * 从 vm_server.c 移植 HTTP/SSE/静态资源部分，改动点：
 *   1. select() 单循环 -> libuv：监听、连接、写完成都是异步回调。
 *   2. 帧不再由本进程生成：PushFrame / PushFullframe 由 main 从 EVT_FRAME /
 *      EVT_FULLFRAME 转来。
 *   3. 新连接请求一次新的 fullframe（而不是本进程重新合成）。
 *   4. /respack/<id> 用缓存的 EVT_RESOURCES 内容，返回同格式的 VMRP 容器
 *      （图片数 0：拆进程后只有主题资源，位图通道还没进来）。
 *   5. 上行两条通道都保留：GET /ws（WebSocket，前端主路径）与 POST /input
 *      （回退路径）。vm_renderer.js 先连 ws，连不上才逐事件 POST。
 *
 * 写队列：每条连接一个输出缓冲。uv_write 异步，缓冲在写完成前必须有效，
 * 所以待写字节先拷进 out.buf，flush 时整体转给 uv_write，写完成回调里释放。
 * SSE 连接待写量超过 8 MB 视为慢客户端断开（对齐旧 vm_server 策略）。
 */
#include "rdp_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uv.h"

#define RDP_LOG(...)                                     \
    do {                                                 \
        fprintf(stderr, "[rdp] " __VA_ARGS__);           \
        fputc('\n', stderr);                             \
        fflush(stderr);                                  \
    } while (0)

#define RDP_MAX_CONNS        16
#define RDP_RECV_CHUNK       65536
#define RDP_SSE_MAX_PENDING  (8u * 1024u * 1024u)
#define RDP_HTTP_MAX_PENDING (48u * 1024u * 1024u)
#define RDP_STATIC_MAX       (32u * 1024u * 1024u)
#define RDP_REQUEST_MAX      (1024u * 1024u)
#define RDP_RESPACK_MAX      (32u * 1024u * 1024u)

typedef struct RdpOut {
    char      *buf;          /* 排队等待写的字节 */
    size_t     len;
    size_t     cap;
    char      *inflight;     /* 已交给 uv_write、尚未完成的字节 */
    size_t     inflightLen;
    int        writing;
    uv_write_t wreq;
} RdpOut;

typedef struct RdpConn {
    struct RdpConn *next;
    RdpServer      *srv;
    uv_tcp_t        sock;
    RdpOut          out;
    char           *in;
    size_t          inLen;
    size_t          inCap;
    int             isSse;
    int             isWs;       /* WebSocket 上行通道 */
    int             waitFull;   /* 已连上但还没拿到基线 */
    int             closeAfterFlush;  /* 响应写完后关闭（HTTP 一次性请求） */
    int             closing;
} RdpConn;

struct RdpServer {
    uv_loop_t       *loop;
    uv_tcp_t         listener;
    uv_timer_t       timer;
    int              listening;
    RdpConn         *conns;
    int              connCount;

    unsigned short   port;
    char             webRoot[512];
    char             fontPath[640];

    char             respackId[64];
    char            *respackJson;
    size_t           respackLen;

    RdpInputHandler  input;
    void            *inputUser;

    int              fullPending;   /* 已向上游请求、尚未收到 EVT_FULLFRAME */
    int              stop;
};

/* ---------------- 输出队列 ---------------- */

static int out_reserve(RdpOut *o, size_t add, size_t limit) {
    if (add > limit) return 0;
    if (o->len + add <= o->cap) return 1;
    size_t cap = o->cap ? o->cap : 4096;
    while (cap < o->len + add) cap *= 2;
    if (cap > limit) cap = limit;
    if (o->len + add > cap) return 0;
    char *p = (char *)realloc(o->buf, cap);
    if (!p) return 0;
    o->buf = p;
    o->cap = cap;
    return 1;
}

static void out_free(RdpOut *o) {
    free(o->buf);
    free(o->inflight);
    o->buf = NULL;
    o->inflight = NULL;
    o->len = o->cap = o->inflightLen = 0;
}

static void conn_close(RdpConn *c);
static void conn_flush(RdpConn *c);

static void on_write(uv_write_t *req, int status) {
    RdpConn *c = (RdpConn *)req->data;
    free(c->out.inflight);
    c->out.inflight = NULL;
    c->out.inflightLen = 0;
    c->out.writing = 0;
    if (status < 0) {
        conn_close(c);
        return;
    }
    if (c->closing) return;
    conn_flush(c);
}

static void conn_flush(RdpConn *c) {
    uv_buf_t buf;

    if (c->closing) return;
    if (c->out.writing || c->out.len == 0) {
        /* 队列空了且请求要求「写完就关」：先让 uv_write 完成，再 close。
           直接在写之前 uv_close 会把尚未进内核的数据丢掉。 */
        if (c->closeAfterFlush && !c->out.writing && c->out.len == 0)
            conn_close(c);
        return;
    }

    /* 整块转交 uv_write：同一时刻只有一次写在途，缓冲独占。 */
    c->out.inflight = c->out.buf;
    c->out.inflightLen = c->out.len;
    c->out.buf = NULL;
    c->out.len = 0;
    c->out.cap = 0;

    buf = uv_buf_init(c->out.inflight, (unsigned int)c->out.inflightLen);
    c->out.wreq.data = c;
    c->out.writing = 1;
    if (uv_write(&c->out.wreq, (uv_stream_t *)&c->sock, &buf, 1, on_write) != 0) {
        c->out.writing = 0;
        free(c->out.inflight);
        c->out.inflight = NULL;
        c->out.inflightLen = 0;
        conn_close(c);
    }
}

/* 一次 append 多段，最后只 flush 一次（避免数据 + 前缀各写一个包）。 */
static int conn_send_parts(RdpConn *c, const char *const *parts,
                           const size_t *lens, int n) {
    size_t total = 0;
    size_t limit = c->isSse ? RDP_SSE_MAX_PENDING : RDP_HTTP_MAX_PENDING;
    int i;

    if (c->closing) return 0;
    for (i = 0; i < n; i++) total += lens[i];
    if (!out_reserve(&c->out, total, limit)) {
        RDP_LOG("%s client is too slow (%zu pending), dropping",
                c->isSse ? "sse" : "http", c->out.len);
        conn_close(c);
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (!lens[i]) continue;
        memcpy(c->out.buf + c->out.len, parts[i], lens[i]);
        c->out.len += lens[i];
    }
    conn_flush(c);
    return 1;
}

static int conn_send(RdpConn *c, const char *data, size_t len) {
    const char *parts[1];
    size_t lens[1];
    parts[0] = data;
    lens[0] = len;
    return conn_send_parts(c, parts, lens, 1);
}

/* ---------------- 连接生命周期 ---------------- */

static void on_conn_closed(uv_handle_t *handle) {
    RdpConn *c = (RdpConn *)handle->data;
    RdpServer *s = c->srv;

    if (s) {
        RdpConn **pp = &s->conns;
        while (*pp) {
            if (*pp == c) { *pp = c->next; break; }
            pp = &(*pp)->next;
        }
        s->connCount--;
        if (c->isSse)
            RDP_LOG("sse client closed (sse=%d)", s->connCount);
    }
    out_free(&c->out);
    free(c->in);
    free(c);
}

static void conn_close(RdpConn *c) {
    if (c->closing) return;
    c->closing = 1;
    if (!uv_is_closing((uv_handle_t *)&c->sock)) {
        uv_read_stop((uv_stream_t *)&c->sock);
        uv_close((uv_handle_t *)&c->sock, on_conn_closed);
    }
}

/* ---------------- HTTP 小工具 ---------------- */

#ifdef _WIN32
#define rdp_strnicmp _strnicmp
#else
#define rdp_strnicmp strncasecmp
#endif

static const char *find_header(const char *req, const char *name) {
    size_t nlen = strlen(name);
    const char *p = req;
    while (*p) {
        if ((p == req || p[-1] == '\n') && rdp_strnicmp(p, name, nlen) == 0 &&
            p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ') v++;
            return v;
        }
        p++;
    }
    return NULL;
}

static void respond(RdpConn *c, int code, const char *reason,
                    const char *contentType, const char *body, size_t len) {
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n",
                     code, reason, contentType, len);
    if (n <= 0) { conn_close(c); return; }
    conn_send(c, header, (size_t)n);
    if (len) conn_send(c, body, len);
    /* HTTP/1.1 + Connection: close：写队列排空后再关，否则最后一段数据可能
       在 uv_close 时被丢掉，客户端表现为「连上了但 0 字节」。 */
    c->closeAfterFlush = 1;
    conn_flush(c);
}

static void respond_text(RdpConn *c, int code, const char *reason, const char *text) {
    respond(c, code, reason, "text/plain; charset=utf-8", text, strlen(text));
}

/* ---------------- WebSocket 上行 ----------------
   前端优先走 ws：每帧只是一次 send，不建新连接、不带 HTTP 头。
   这里只实现「文本帧上行 + ping/pong + close」这三件事，不做分片重组：
   输入事件都是几十字节的单帧，>125 的 126 长度形式仍然支持。 */

typedef struct {
    unsigned int h[5];
    unsigned long long total;
    unsigned char buf[64];
    size_t bufLen;
} RdpSha1;

static unsigned int rdp_rol32(unsigned int v, int bits) {
    return (v << bits) | (v >> (32 - bits));
}

static void rdp_sha1_init(RdpSha1 *c) {
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->total = 0;
    c->bufLen = 0;
}

static void rdp_sha1_block(RdpSha1 *c, const unsigned char *p) {
    unsigned int w[80];
    unsigned int a, b, d, e, f;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned int)p[i * 4] << 24) | ((unsigned int)p[i * 4 + 1] << 16) |
               ((unsigned int)p[i * 4 + 2] << 8) | (unsigned int)p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = rdp_rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = c->h[0]; b = c->h[1]; d = c->h[2]; e = c->h[3]; f = c->h[4];
    for (i = 0; i < 80; i++) {
        unsigned int t, k, tmp;
        if (i < 20) { t = (b & d) | ((~b) & e); k = 0x5A827999u; }
        else if (i < 40) { t = b ^ d ^ e; k = 0x6ED9EBA1u; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e); k = 0x8F1BBCDCu; }
        else { t = b ^ d ^ e; k = 0xCA62C1D6u; }
        tmp = rdp_rol32(a, 5) + t + f + k + w[i];
        f = e; e = d; d = rdp_rol32(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

static void rdp_sha1_update(RdpSha1 *c, const void *data, size_t len) {
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
            rdp_sha1_block(c, c->buf);
            c->bufLen = 0;
        }
    }
}

static void rdp_sha1_final(RdpSha1 *c, unsigned char out[20]) {
    unsigned long long bits = c->total * 8;
    unsigned char pad = 0x80;
    unsigned char zero = 0;
    unsigned char lenBytes[8];
    int i;
    rdp_sha1_update(c, &pad, 1);
    while (c->bufLen != 56) rdp_sha1_update(c, &zero, 1);
    for (i = 0; i < 8; i++) lenBytes[i] = (unsigned char)(bits >> (56 - i * 8));
    rdp_sha1_update(c, lenBytes, 8);
    for (i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->h[i];
    }
}

static size_t rdp_base64(const unsigned char *in, size_t len, char *out, size_t cap) {
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

/* 计算 Sec-WebSocket-Accept。 */
static int ws_accept(const char *key, char *out, size_t cap) {
    static const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char src[192];
    unsigned char digest[20];
    RdpSha1 sha;
    size_t klen = 0, glen = strlen(guid);
    while (key[klen] && key[klen] != '\r' && key[klen] != '\n' &&
           klen + glen + 1 < sizeof(src)) klen++;
    memcpy(src, key, klen);
    memcpy(src + klen, guid, glen);
    rdp_sha1_init(&sha);
    rdp_sha1_update(&sha, src, klen + glen);
    rdp_sha1_final(&sha, digest);
    return rdp_base64(digest, sizeof(digest), out, cap) > 0;
}

/* 服务端发出的帧不加掩码；输入侧只有 pong，载荷 <= 125。 */
static void ws_send_frame(RdpConn *c, int opcode, const unsigned char *payload, size_t len) {
    unsigned char frame[8 + 128];
    size_t n = 0;
    if (len > 125) len = 125;
    frame[n++] = (unsigned char)(0x80 | (opcode & 0x0F));
    frame[n++] = (unsigned char)len;
    if (len) {
        memcpy(frame + n, payload, len);
        n += len;
    }
    conn_send(c, (const char *)frame, n);
}

/* 解析 c->in 里已到达的完整帧。浏览器发来的帧必须带掩码。 */
static void ws_consume(RdpConn *c) {
    size_t off = 0;

    while (!c->closing && c->inLen - off >= 2) {
        const unsigned char *b = (const unsigned char *)c->in + off;
        int fin = (b[0] & 0x80) != 0;
        int opcode = b[0] & 0x0F;
        int masked = (b[1] & 0x80) != 0;
        unsigned long long plen = (unsigned long long)(b[1] & 0x7F);
        size_t hdr = 2;
        unsigned char *payload;

        if (plen == 126) {
            if (c->inLen - off < 4) break;
            plen = ((unsigned long long)b[2] << 8) | (unsigned long long)b[3];
            hdr = 4;
        } else if (plen == 127) {
            conn_close(c);   /* 超长帧不是输入事件 */
            return;
        }
        if (masked) hdr += 4;
        if (c->inLen - off < hdr + (size_t)plen) break;

        payload = (unsigned char *)c->in + off + hdr;
        if (masked) {
            const unsigned char *m = (const unsigned char *)c->in + off + hdr - 4;
            size_t i;
            for (i = 0; i < (size_t)plen; i++)
                payload[i] = (unsigned char)(payload[i] ^ m[i & 3]);
        }

        if (opcode == 0x8) {
            conn_close(c);
            return;
        }
        if (opcode == 0x9) {
            ws_send_frame(c, 0x0A, payload, (size_t)plen);
        } else if (opcode == 0x1 && fin && plen > 0 && c->srv->input) {
            /* payload 之后可能是下一帧：不能就地当 C 字符串用，先复制补 '\0'。 */
            if (plen <= RDP_RECV_CHUNK) {
                char message[RDP_RECV_CHUNK + 1];
                memcpy(message, payload, (size_t)plen);
                message[plen] = '\0';
                c->srv->input(c->srv->inputUser, message, (size_t)plen);
            }
        }
        off += hdr + (size_t)plen;
    }

    if (off > 0) {
        memmove(c->in, c->in + off, c->inLen - off);
        c->inLen -= off;
    }
}

static void start_ws(RdpConn *c, size_t hdrLen) {
    const char *key = find_header(c->in, "Sec-WebSocket-Key");
    char accept[64];
    char resp[512];
    int n;

    if (!key || !ws_accept(key, accept, sizeof(accept))) {
        respond_text(c, 400, "Bad Request", "bad ws");
        return;
    }
    n = snprintf(resp, sizeof(resp),
                 "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (n <= 0 || !conn_send(c, resp, (size_t)n)) return;

    c->isWs = 1;
    /* 握手请求之后可能已经跟着帧数据：保留并按帧解析。 */
    if (c->inLen > hdrLen) {
        memmove(c->in, c->in + hdrLen, c->inLen - hdrLen);
        c->inLen -= hdrLen;
    } else {
        c->inLen = 0;
    }
    RDP_LOG("ws input channel opened (total=%d)", c->srv->connCount);
    ws_consume(c);
}

/* ---------------- 静态资源 ---------------- */

/* HTML 只加载 JS；所有 DOM/样式由 vm_renderer.js 统一创建，与 C 宿主可比。 */
static const char VM_INDEX_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>VM Frame</title>"
    "<script type='module' src='/vm_renderer.js'></script></head><body></body></html>";

/* 白名单只有这几个文件：本地演示服务，不做目录映射。 */
static const char *static_name(const char *path) {
    if (strcmp(path, "/vm_renderer.js") == 0) return "vm_renderer.js";
    if (strcmp(path, "/app.js") == 0) return "app.js";
    if (strcmp(path, "/systemColors.js") == 0) return "systemColors.js";
    if (strcmp(path, "/font_wasm.js") == 0) return "font_wasm.js";
    if (strcmp(path, "/stb_font_wasm.mjs") == 0) return "stb_font_wasm.mjs";
    if (strcmp(path, "/stb_font_wasm.wasm") == 0) return "stb_font_wasm.wasm";
    if (strcmp(path, "/font.ttf") == 0) return "font.ttf";
    return NULL;
}

static void serve_static(RdpConn *c, const char *path) {
    RdpServer *s = c->srv;
    const char *name;
    char full[640];
    FILE *f;
    long size;
    char *body;
    size_t readLen;
    const char *ct;

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        respond(c, 200, "OK", "text/html; charset=utf-8",
                VM_INDEX_HTML, sizeof(VM_INDEX_HTML) - 1);
        return;
    }

    name = static_name(path);
    if (!name) {
        respond_text(c, 404, "Not Found", "not found");
        return;
    }

    /* 主题字体允许指向 webRoot 之外的显式路径（例如系统 simsun.ttc），
       这样前端与 guest 用同一份字体文件，度量与字形才一致。 */
    if (strcmp(path, "/font.ttf") == 0 && s->fontPath[0])
        snprintf(full, sizeof(full), "%s", s->fontPath);
    else
        snprintf(full, sizeof(full), "%s/%s", s->webRoot, name);

    f = fopen(full, "rb");
    if (!f) {
        RDP_LOG("static miss: %s", full);
        respond_text(c, 404, "Not Found", "not found");
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || (unsigned long)size > RDP_STATIC_MAX) {
        fclose(f);
        respond_text(c, 413, "Payload Too Large", "too large");
        return;
    }
    body = (char *)malloc((size_t)size ? (size_t)size : 1);
    if (!body) {
        fclose(f);
        respond_text(c, 500, "Internal Error", "oom");
        return;
    }
    readLen = fread(body, 1, (size_t)size, f);
    fclose(f);

    ct = "text/plain; charset=utf-8";
    if (strstr(name, ".js") || strstr(name, ".mjs")) ct = "text/javascript; charset=utf-8";
    else if (strstr(name, ".wasm")) ct = "application/wasm";
    respond(c, 200, "OK", ct, body, readLen);
    free(body);
}

/* ---------------- 资源包容器 ----------------
   与 assets/app.js 的 parseResPack 对应，全部小端：
     u32 magic "VMRP" | u32 jsonLen | u32 imgCount
     imgCount × { u16 idLen, id, u8 mimeLen, mime, u32 dataLen } | json | images
   拆进程后只有主题资源（definePattern / defineStyle），图片数恒为 0。 */

#define RDP_RESPACK_MAGIC 0x50524D56u

static void put_u32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void serve_respack(RdpConn *c, const char *id) {
    RdpServer *s = c->srv;
    size_t idLen = strlen(id);
    size_t total;
    unsigned char *buf;
    int valid = idLen > 0 && idLen < 64;
    size_t i;

    for (i = 0; valid && i < idLen; i++) {
        char ch = id[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) valid = 0;
    }
    if (!valid || !s->respackJson || strcmp(id, s->respackId) != 0) {
        RDP_LOG("respack request id='%s' -> 404 (cached='%s')", id, s->respackId);
        respond_text(c, 404, "Not Found", "no respack");
        return;
    }

    total = 12 + s->respackLen;
    buf = (unsigned char *)malloc(total);
    if (!buf) {
        respond_text(c, 500, "Internal Error", "oom");
        return;
    }
    put_u32(buf, RDP_RESPACK_MAGIC);
    put_u32(buf + 4, (unsigned)s->respackLen);
    put_u32(buf + 8, 0);                  /* imgCount */
    memcpy(buf + 12, s->respackJson, s->respackLen);

    RDP_LOG("respack request id='%s' -> 200 (%zu bytes)", id, total);
    respond(c, 200, "OK", "application/octet-stream", (const char *)buf, total);
    free(buf);
}

/* ---------------- SSE ---------------- */

static void push_resources_to(RdpConn *c, const char *id) {
    char payload[160];
    int n = snprintf(payload, sizeof(payload),
                     "event: resources\n"
                     "data: [{\"cmd\":\"loadResPack\",\"args\":[\"%s\"]}]\n\n",
                     id);
    if (n > 0) conn_send(c, payload, (size_t)n);
}

static void start_sse(RdpConn *c) {
    RdpServer *s = c->srv;
    static const char header[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n\r\n";

    if (!conn_send(c, header, sizeof(header) - 1)) return;
    c->isSse = 1;
    c->inLen = 0;
    RDP_LOG("sse client opened (total=%d)", s->connCount);

    if (s->respackId[0]) push_resources_to(c, s->respackId);
    c->waitFull = 1;
}

/* ---------------- 请求分发 ---------------- */

static void handle_http(RdpConn *c) {
    RdpServer *s = c->srv;
    char *hdrEnd = strstr(c->in, "\r\n\r\n");
    size_t hdrLen;
    size_t bodyLen = 0;
    const char *cl;
    char method[16] = {0};
    char path[512] = {0};

    if (!hdrEnd) {
        if (c->inLen > 32768) {
            respond_text(c, 400, "Bad Request", "bad request");
        }
        return;
    }
    hdrLen = (size_t)(hdrEnd + 4 - c->in);

    if (sscanf(c->in, "%15s %511s", method, path) != 2) {
        respond_text(c, 400, "Bad Request", "bad request");
        return;
    }
    RDP_LOG("%s %s", method, path);

    cl = find_header(c->in, "Content-Length");
    if (cl) bodyLen = (size_t)strtoul(cl, NULL, 10);
    if (bodyLen > RDP_REQUEST_MAX) {
        respond_text(c, 413, "Payload Too Large", "too large");
        return;
    }
    if (c->inLen < hdrLen + bodyLen) return;   /* body 未到齐，等下一次读 */

    if (strcmp(method, "GET") == 0 && strcmp(path, "/events") == 0) {
        start_sse(c);
        return;
    }

    /* 上行主路径：WebSocket。握手之后这条连接只承载输入帧。 */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/ws") == 0) {
        start_ws(c, hdrLen);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/input") == 0) {
        if (s->input && bodyLen) s->input(s->inputUser, c->in + hdrLen, bodyLen);
        respond(c, 200, "OK", "application/json", "{\"ok\":true}", 11);
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/respack/", 9) == 0) {
        serve_respack(c, path + 9);
        return;
    }

    if (strcmp(method, "GET") == 0) {
        serve_static(c, path);
        return;
    }

    respond_text(c, 405, "Method Not Allowed", "method not allowed");
}

/* ---------------- TCP 读 ---------------- */

static void on_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
    (void)handle;
    (void)suggested;
    buf->base = (char *)malloc(RDP_RECV_CHUNK);
    buf->len = buf->base ? RDP_RECV_CHUNK : 0;
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    RdpConn *c = (RdpConn *)stream->data;

    /* 顺序要紧：buf->base 属于本轮读，用完才能释放（先 free 再 memcpy 会读到
       已释放内存，表现为「连上了、请求却解析不出 method/path」）。 */
    if (nread < 0) {
        free(buf->base);
        conn_close(c);
        return;
    }
    if (nread == 0 || c->closing) {
        free(buf->base);
        return;
    }

    /* SSE 只下行：读到什么都丢弃，只用于发现对端断开。 */
    if (c->isSse) {
        free(buf->base);
        return;
    }

    if (c->inLen + (size_t)nread + 1 > RDP_REQUEST_MAX) {
        free(buf->base);
        if (c->isWs) conn_close(c);
        else respond_text(c, 413, "Payload Too Large", "too large");
        return;
    }
    if (c->inLen + (size_t)nread + 1 > c->inCap) {
        size_t cap = c->inCap ? c->inCap : 4096;
        char *grown;
        while (cap < c->inLen + (size_t)nread + 1) cap *= 2;
        grown = (char *)realloc(c->in, cap);
        if (!grown) {
            free(buf->base);
            conn_close(c);
            return;
        }
        c->in = grown;
        c->inCap = cap;
    }
    /* buf->base 不带终止符：追加后统一补 '\0'，让 strstr/sscanf 安全。 */
    memcpy(c->in + c->inLen, buf->base, (size_t)nread);
    c->inLen += (size_t)nread;
    c->in[c->inLen] = '\0';
    free(buf->base);

    if (c->isWs) ws_consume(c);
    else handle_http(c);
}

static void on_new_connection(uv_stream_t *server, int status) {
    RdpServer *s = (RdpServer *)server->data;
    RdpConn *c;
    int r;

    if (status < 0) {
        RDP_LOG("accept failed: %s", uv_strerror(status));
        return;
    }
    if (s->connCount >= RDP_MAX_CONNS) {
        /* 槽位满：接了就立刻关，别留在 backlog 里让对端空等。 */
        uv_tcp_t tmp;
        if (uv_tcp_init(s->loop, &tmp) == 0) {
            if (uv_accept(server, (uv_stream_t *)&tmp) == 0)
                uv_close((uv_handle_t *)&tmp, NULL);
            else
                uv_close((uv_handle_t *)&tmp, NULL);
        }
        RDP_LOG("connection rejected: %d clients already", s->connCount);
        return;
    }

    c = (RdpConn *)calloc(1, sizeof(RdpConn));
    if (!c) return;
    c->srv = s;

    r = uv_tcp_init(s->loop, &c->sock);
    if (r != 0) {
        free(c);
        return;
    }
    c->sock.data = c;

    r = uv_accept(server, (uv_stream_t *)&c->sock);
    if (r != 0) {
        uv_close((uv_handle_t *)&c->sock, NULL);
        free(c);
        return;
    }
    uv_tcp_nodelay(&c->sock, 1);

    c->next = s->conns;
    s->conns = c;
    s->connCount++;

    if (uv_read_start((uv_stream_t *)&c->sock, on_alloc, on_read) != 0) {
        conn_close(c);
        return;
    }
    RDP_LOG("conn open (total=%d)", s->connCount);
}

/* ---------------- 定时器：只为唤醒主循环 ----------------
   主循环用 uv_run(UV_RUN_ONCE)，没有 IO 时会阻塞到下一个 timer；
   16 ms 与 vm_wms 的 tick 同频，WMS 事件最多延迟一个 tick 被取走。 */

static void on_timer(uv_timer_t *timer) {
    (void)timer;
}

/* ---------------- 对外接口 ---------------- */

RdpServer *RdpServer_Create(struct uv_loop_s *loop, unsigned short port,
                            const char *webRoot, const char *fontPath) {
    RdpServer *s;
    struct sockaddr_in addr;
    int r;

    if (!loop) return NULL;

    s = (RdpServer *)calloc(1, sizeof(RdpServer));
    if (!s) return NULL;
    s->loop = loop;
    s->port = port;
    snprintf(s->webRoot, sizeof(s->webRoot), "%s", webRoot ? webRoot : ".");
    snprintf(s->fontPath, sizeof(s->fontPath), "%s", fontPath ? fontPath : "");

    r = uv_tcp_init(loop, &s->listener);
    if (r != 0) {
        RDP_LOG("tcp init failed: %s", uv_strerror(r));
        free(s);
        return NULL;
    }
    s->listener.data = s;

    uv_ip4_addr("127.0.0.1", port, &addr);
    r = uv_tcp_bind(&s->listener, (const struct sockaddr *)&addr, 0);
    if (r != 0) {
        RDP_LOG("bind 127.0.0.1:%u failed: %s", port, uv_strerror(r));
        uv_close((uv_handle_t *)&s->listener, NULL);
        free(s);
        return NULL;
    }
    r = uv_listen((uv_stream_t *)&s->listener, 16, on_new_connection);
    if (r != 0) {
        RDP_LOG("listen failed: %s", uv_strerror(r));
        uv_close((uv_handle_t *)&s->listener, NULL);
        free(s);
        return NULL;
    }
    s->listening = 1;

    uv_timer_init(loop, &s->timer);
    uv_timer_start(&s->timer, on_timer, 16, 16);

    RDP_LOG("listening on 127.0.0.1:%u (web root: %s)", port, s->webRoot);
    return s;
}

void RdpServer_Free(RdpServer *s) {
    RdpConn *c;

    if (!s) return;

    for (c = s->conns; c; c = c->next) {
        if (!c->closing) {
            c->closing = 1;
            if (!uv_is_closing((uv_handle_t *)&c->sock)) {
                uv_read_stop((uv_stream_t *)&c->sock);
                uv_close((uv_handle_t *)&c->sock, on_conn_closed);
            }
        }
    }
    if (s->listening) {
        s->listening = 0;
        uv_close((uv_handle_t *)&s->listener, NULL);
    }
    uv_close((uv_handle_t *)&s->timer, NULL);
    uv_run(s->loop, UV_RUN_DEFAULT);   /* 让 close 回调把连接结构释放掉 */

    free(s->respackJson);
    free(s);
}

void RdpServer_Stop(RdpServer *s) {
    if (s) s->stop = 1;
}

int RdpServer_Stopped(const RdpServer *s) {
    return s ? s->stop : 1;
}

void RdpServer_SetInputHandler(RdpServer *s, RdpInputHandler handler, void *user) {
    if (!s) return;
    s->input = handler;
    s->inputUser = user;
}

void RdpServer_PushFrame(RdpServer *s, const char *json, size_t len) {
    RdpConn *c;

    if (!s || !json || !len) return;
    for (c = s->conns; c; c = c->next) {
        const char *parts[3];
        size_t lens[3];
        if (!c->isSse || c->closing || c->waitFull) continue;  /* 等基线的连接不收增量 */
        parts[0] = "data: ";
        lens[0] = 6;
        parts[1] = json;
        lens[1] = len;
        parts[2] = "\n\n";
        lens[2] = 2;
        conn_send_parts(c, parts, lens, 3);
    }
}

void RdpServer_PushFullframe(RdpServer *s, const char *json, size_t len) {
    RdpConn *c;

    if (!s || !json || !len) return;
    s->fullPending = 0;
    for (c = s->conns; c; c = c->next) {
        const char *parts[3];
        size_t lens[3];
        if (!c->isSse || c->closing || !c->waitFull) continue;
        parts[0] = "event: fullframe\ndata: ";
        lens[0] = 23;
        parts[1] = json;
        lens[1] = len;
        parts[2] = "\n\n";
        lens[2] = 2;
        if (conn_send_parts(c, parts, lens, 3)) c->waitFull = 0;
    }
    RDP_LOG("fullframe delivered (%zu bytes)", len);
}

void RdpServer_SetRespack(RdpServer *s, const char *id, const char *json, size_t len) {
    char *copy;
    RdpConn *c;

    if (!s || !id || !*id || !json || !len) return;
    if (len > RDP_RESPACK_MAX) {
        RDP_LOG("respack '%s' too large (%zu), ignored", id, len);
        return;
    }

    copy = (char *)malloc(len);
    if (!copy) return;
    memcpy(copy, json, len);

    free(s->respackJson);
    s->respackJson = copy;
    s->respackLen = len;
    snprintf(s->respackId, sizeof(s->respackId), "%s", id);

    RDP_LOG("respack '%s' ready (%zu bytes)", id, len);
    /* 已连接的前端重新拉取：SSE 不传资源本体，只传 loadResPack。 */
    for (c = s->conns; c; c = c->next) {
        if (c->isSse && !c->closing) push_resources_to(c, s->respackId);
    }
}

int RdpServer_BeginFullframe(RdpServer *s) {
    RdpConn *c;

    if (!s || s->fullPending) return 0;
    for (c = s->conns; c; c = c->next) {
        if (c->isSse && !c->closing && c->waitFull) {
            s->fullPending = 1;
            return 1;
        }
    }
    return 0;
}

void RdpServer_CancelFullframe(RdpServer *s) {
    if (s) s->fullPending = 0;
}
