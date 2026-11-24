/*
 * vmwm_client.c - 客户端 SDK 实现（APP / SHELL / RDP 共用）· v2 实例化
 *
 * 每个 WmsClient 持有自己的 uv_loop_t / 管道 / seq / 回调，所以同一进程里
 * 可以同时连多个服务（rdp 与 app 并存的场景）。
 *
 * 同步模型：每个 Wms_Xxx 调用 = 打包请求 -> uv_write -> 等对应 seq 的 ACK；
 * 等待期间照常跑自己的 loop，服务端推送的事件会被分发到用户回调。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#  include <time.h>
#endif

/* uv.h 必须最先包含：Windows 上它会把 winsock2.h / windows.h 按正确顺序引入 */
#include "uv.h"

#include "wms_ipc.h"
#include "wms_client.h"
#include "wms_codec.h"
#include "wms_log.h"

#define VMWM_PATH_MAX 256

typedef struct CliWrite {
    uv_write_t req;
    char*      buf;
} CliWrite;

struct WmsClient {
    uv_loop_t      loop;
    uv_pipe_t      pipe;
    uv_connect_t   connect_req;

    VmWmBuf        rx;
    VmWmBuf        ack;          /* 最近一条 ACK 的载荷 */
    uint16_t       ack_seq;
    int            ack_got;

    int            inited;
    int            connected;
    int            connect_failed;

    uint16_t       seq;
    uint32_t       id;
    VmWmClientType type;
    uint32_t       mask;

    char           path[VMWM_PATH_MAX];

    int32_t        screen_w, screen_h;
    VmWmReqWorkArea work;
    uint32_t       layout_frames;

    int            evt_count;
    WmsEventCb     cb;
    void*          cb_user;
};

/* ================= 小工具 ================= */

static uint64_t vmwm_now_ms(void)
{
    return (uint64_t)(uv_hrtime() / 1000000ull);
}

static void vmwm_sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
#endif
}

static void copy_str(char* dst, unsigned cap, const char* src)
{
    unsigned i = 0;
    if (!dst || !cap) return;
    if (src) {
        for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

const char* Wms_StrError(VmWmResult r)
{
    switch (r) {
    case VMWM_OK:                 return "ok";
    case VMWM_ERR_INVALID_WINDOW: return "invalid window id";
    case VMWM_ERR_PERMISSION:     return "permission denied";
    case VMWM_ERR_NOMEM:          return "out of memory";
    case VMWM_ERR_FAILED:         return "failed";
    case VMWM_ERR_BAD_ARG:        return "bad argument";
    case VMWM_ERR_NOT_CONNECTED:  return "not connected";
    case VMWM_ERR_TIMEOUT:        return "timeout";
    case VMWM_ERR_LIMIT:          return "limit reached";
    case VMWM_ERR_PROTOCOL:       return "protocol error";
    case VMWM_ERR_NOT_READY:      return "service not ready (no host)";
    default:                      return "unknown";
    }
}

/* ================= 发送 ================= */

static void on_write_done(uv_write_t* req, int status)
{
    CliWrite* ctx = (CliWrite*)req;
    (void)status;
    free(ctx->buf);
    free(ctx);
}

static int send_msg(WmsClient* c, uint16_t type, uint16_t id, uint16_t seq,
                    const void* body, uint32_t blen)
{
    VmWmBuf   b;
    CliWrite* ctx;
    uv_buf_t  ub;
    int       r;

    if (!c || !c->connected) return VMWM_ERR_NOT_CONNECTED;

    vmwm_buf_init(&b);
    if (vmwm_msg_pack(&b, type, id, seq, body, blen) != VMWM_OK) {
        vmwm_buf_free(&b);
        return VMWM_ERR_LIMIT;
    }

    ctx = (CliWrite*)calloc(1, sizeof(CliWrite));
    if (!ctx) {
        vmwm_buf_free(&b);
        return VMWM_ERR_NOMEM;
    }

    ctx->buf = (char*)b.data;         /* 所有权转移 */
    b.data   = NULL;

    ub = uv_buf_init(ctx->buf, (unsigned int)b.len);
    r = uv_write(&ctx->req, (uv_stream_t*)&c->pipe, &ub, 1, on_write_done);
    vmwm_buf_free(&b);

    if (r != 0) {
        free(ctx->buf);
        free(ctx);
        return VMWM_ERR_FAILED;
    }
    return VMWM_OK;
}

/* ================= 接收 ================= */

static void dispatch_event(WmsClient* c, uint32_t evt, const uint8_t* body,
                          uint32_t blen)
{
    WmsEvent e;

    c->evt_count++;

    if (c->cb) {
        e.evt     = evt;
        e.payload = body;
        e.len     = blen;
        c->cb(c, &e, c->cb_user);
    }
}

static void on_alloc(uv_handle_t* handle, size_t suggested, uv_buf_t* buf)
{
    (void)handle;
    (void)suggested;
    buf->base = (char*)malloc(VMWM_READ_CHUNK);
    buf->len  = buf->base ? VMWM_READ_CHUNK : 0;
}

static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
    WmsClient* c = (WmsClient*)stream->data;

    if (nread < 0) {
        free(buf->base);
        c->connected = 0;
        if (!uv_is_closing((uv_handle_t*)&c->pipe))
            uv_close((uv_handle_t*)&c->pipe, NULL);
        return;
    }
    if (nread == 0) {
        free(buf->base);
        return;
    }

    if (!vmwm_buf_append(&c->rx, buf->base, (size_t)nread)) {
        free(buf->base);
        vmwm_log("client id=%u rx over limit, dropping", (unsigned)c->id);
        c->connected = 0;
        if (!uv_is_closing((uv_handle_t*)&c->pipe))
            uv_close((uv_handle_t*)&c->pipe, NULL);
        return;
    }
    free(buf->base);

    for (;;) {
        VmWmMsg m;
        VmWmParse pr = vmwm_msg_parse(&c->rx, &m);

        if (pr == VMWM_PARSE_NEED_MORE) break;
        if (pr == VMWM_PARSE_ERROR) {
            vmwm_log("protocol error on client id=%u: bad frame", (unsigned)c->id);
            c->connected = 0;
            c->rx.len = 0;
            if (!uv_is_closing((uv_handle_t*)&c->pipe))
                uv_close((uv_handle_t*)&c->pipe, NULL);
            return;
        }

        if (m.type == VMWM_CMD_TYPE_ACK) {
            if (m.hdr.seq == c->ack_seq) {
                vmwm_buf_free(&c->ack);
                vmwm_buf_init(&c->ack);
                if (m.body_len &&
                    !vmwm_buf_append(&c->ack, m.body, m.body_len)) {
                    c->ack.len = 0;
                }
                c->ack_got = 1;
            } else {
                vmwm_log("client id=%u stale ack seq=%u (expecting %u)",
                         (unsigned)c->id, (unsigned)m.hdr.seq,
                         (unsigned)c->ack_seq);
            }
        } else if (m.type == VMWM_CMD_TYPE_EVT) {
            dispatch_event(c, m.id, m.body, m.body_len);
        } else if (m.type == VMWM_CMD_TYPE_MSG) {
            /* 服务端目前不发 MSG；忽略即可 */
        } else {
            vmwm_log("client id=%u unexpected type %u", (unsigned)c->id,
                     (unsigned)m.type);
        }

        vmwm_buf_consume(&c->rx, m.total);
    }
}

static void on_connect(uv_connect_t* req, int status)
{
    WmsClient* c = (WmsClient*)req->data;

    if (status < 0) {
        c->connect_failed = 1;
        c->connected = 0;
        return;
    }
    c->connect_failed = 0;
    c->connected = 1;
}

/* ================= 同步请求 ================= */

static VmWmResult client_call(WmsClient* c, uint16_t cmd, const void* body,
                              uint32_t blen, VmWmBuf* out, uint32_t timeout_ms)
{
    uint16_t seq;
    uint64_t deadline;

    if (!c) return VMWM_ERR_BAD_ARG;
    if (!c->connected) return VMWM_ERR_NOT_CONNECTED;
    if (!out) return VMWM_ERR_BAD_ARG;

    vmwm_buf_free(&c->ack);
    vmwm_buf_init(&c->ack);
    c->ack_got = 0;

    seq = c->seq++;
    if (c->seq == 0) c->seq = 1;
    c->ack_seq = seq;

    if (send_msg(c, VMWM_CMD_TYPE_REQ, cmd, seq, body, blen) != VMWM_OK)
        return VMWM_ERR_FAILED;

    deadline = vmwm_now_ms() + (timeout_ms ? timeout_ms : VMWM_IO_TIMEOUT_MS);

    while (!c->ack_got) {
        uv_run(&c->loop, UV_RUN_NOWAIT);
        if (c->ack_got) break;
        if (!c->connected) return VMWM_ERR_NOT_CONNECTED;
        if (vmwm_now_ms() >= deadline) return VMWM_ERR_TIMEOUT;
        vmwm_sleep_ms(1);
    }

    if (c->ack.len && !vmwm_buf_append(out, c->ack.data, c->ack.len))
        return VMWM_ERR_NOMEM;

    return VMWM_OK;
}

static VmWmResult call_simple(WmsClient* c, uint16_t cmd, const void* body,
                              uint32_t blen, uint64_t* out_value)
{
    VmWmBuf    reply;
    VmWmAck    ack;
    VmWmResult r;

    vmwm_buf_init(&reply);

    r = client_call(c, cmd, body, blen, &reply, 0);
    if (r != VMWM_OK) {
        vmwm_buf_free(&reply);
        return r;
    }
    if (reply.len < sizeof(ack)) {
        vmwm_buf_free(&reply);
        return VMWM_ERR_PROTOCOL;
    }

    memcpy(&ack, reply.data, sizeof(ack));
    vmwm_buf_free(&reply);

    if (out_value) *out_value = ack.value;
    return (VmWmResult)ack.result;
}

/* ================= 生命周期 ================= */

WmsClient* Wms_ClientCreate(const char* pipe_path, VmWmClientType type)
{
    WmsClient* c = (WmsClient*)calloc(1, sizeof(WmsClient));
    if (!c) return NULL;

    copy_str(c->path, sizeof(c->path),
             pipe_path ? pipe_path : VMWM_PIPE_NAME);
    c->type  = type;
    c->seq   = 1;
    c->ack_seq = 0;
    return c;
}

VmWmResult Wms_ClientConnect(WmsClient* c)
{
    VmWmReqHello   hello;
    VmWmBuf        reply;
    VmWmAckHello   ack;
    VmWmResult     r;
    uint64_t       deadline;

    if (!c) return VMWM_ERR_BAD_ARG;
    if (c->connected) return VMWM_OK;
    if (c->inited) return VMWM_ERR_FAILED;      /* 先 Destroy 再建 */

    if (uv_loop_init(&c->loop) != 0) {
        vmwm_log("uv_loop_init failed");
        return VMWM_ERR_FAILED;
    }
    c->inited = 1;

    vmwm_buf_init(&c->rx);
    vmwm_buf_init(&c->ack);
    c->ack_got = 0;
    c->evt_count = 0;

    uv_pipe_init(&c->loop, &c->pipe, 1);
    c->pipe.data = c;

    c->connect_req.data = c;
    c->connect_failed = 0;
    uv_pipe_connect(&c->connect_req, &c->pipe, c->path, on_connect);

    deadline = vmwm_now_ms() + VMWM_IO_TIMEOUT_MS;
    while (!c->connected && !c->connect_failed) {
        uv_run(&c->loop, UV_RUN_NOWAIT);
        if (c->connected || c->connect_failed) break;
        if (vmwm_now_ms() >= deadline) break;
        vmwm_sleep_ms(1);
    }

    if (!c->connected) {
        vmwm_log("connect to '%s' failed", c->path);
        if (!uv_is_closing((uv_handle_t*)&c->pipe))
            uv_close((uv_handle_t*)&c->pipe, NULL);
        uv_run(&c->loop, UV_RUN_NOWAIT);
        uv_loop_close(&c->loop);
        vmwm_buf_free(&c->rx);
        vmwm_buf_free(&c->ack);
        c->inited = 0;
        return VMWM_ERR_FAILED;
    }

    if (uv_read_start((uv_stream_t*)&c->pipe, on_alloc, on_read) != 0) {
        vmwm_log("uv_read_start failed");
        return VMWM_ERR_FAILED;
    }

    hello.version     = VMWM_VERSION;
    hello.client_type = (uint32_t)c->type;

    vmwm_buf_init(&reply);
    r = client_call(c, CMD_HANDSHAKE, &hello, sizeof(hello), &reply, 0);
    if (r != VMWM_OK) {
        vmwm_buf_free(&reply);
        vmwm_log("handshake transport failed: %s", Wms_StrError(r));
        return r;
    }
    if (reply.len < sizeof(ack)) {
        vmwm_buf_free(&reply);
        return VMWM_ERR_PROTOCOL;
    }

    memcpy(&ack, reply.data, sizeof(ack));
    vmwm_buf_free(&reply);

    r = (VmWmResult)ack.result;
    if (r != VMWM_OK) {
        vmwm_log("handshake rejected: %s", Wms_StrError(r));
        return r;
    }

    c->id            = (uint32_t)ack.client_id;
    c->screen_w      = ack.screen_w;
    c->screen_h      = ack.screen_h;
    c->work.x        = ack.work_x;
    c->work.y        = ack.work_y;
    c->work.w        = ack.work_w;
    c->work.h        = ack.work_h;
    c->layout_frames = ack.layout_timeout_frames;

    vmwm_log("connected to '%s' as %s (client id=%u, screen=%dx%d)",
             c->path,
             c->type == VMWM_CLIENT_SHELL ? "SHELL" :
             c->type == VMWM_CLIENT_RDP ? "RDP" : "APP",
             (unsigned)c->id, (int)c->screen_w, (int)c->screen_h);
    return VMWM_OK;
}

void Wms_ClientDestroy(WmsClient* c)
{
    int i;

    if (!c) return;

    if (c->inited) {
        if (!uv_is_closing((uv_handle_t*)&c->pipe)) {
            uv_read_stop((uv_stream_t*)&c->pipe);
            uv_close((uv_handle_t*)&c->pipe, NULL);
        }
        for (i = 0; i < 50; i++) {
            uv_run(&c->loop, UV_RUN_NOWAIT);
            if (!uv_loop_alive(&c->loop)) break;
        }
        uv_loop_close(&c->loop);
        vmwm_buf_free(&c->rx);
        vmwm_buf_free(&c->ack);
    }
    free(c);
}

int Wms_ClientIsConnected(const WmsClient* c)
{
    return c ? c->connected : 0;
}

uint32_t Wms_ClientId(const WmsClient* c)
{
    return c ? c->id : 0;
}

VmWmClientType Wms_ClientType(const WmsClient* c)
{
    return c ? c->type : VMWM_CLIENT_APP;
}

int32_t Wms_ScreenWidth(const WmsClient* c)
{
    return c ? c->screen_w : 0;
}

int32_t Wms_ScreenHeight(const WmsClient* c)
{
    return c ? c->screen_h : 0;
}

void Wms_WorkArea(const WmsClient* c, int32_t* x, int32_t* y,
                  int32_t* w, int32_t* h)
{
    if (!c) return;
    if (x) *x = c->work.x;
    if (y) *y = c->work.y;
    if (w) *w = c->work.w;
    if (h) *h = c->work.h;
}

uint32_t Wms_LayoutTimeoutFrames(const WmsClient* c)
{
    return c ? c->layout_frames : 0;
}

/* ================= 事件 ================= */

void Wms_SetEventCallback(WmsClient* c, WmsEventCb cb, void* user)
{
    if (!c) return;
    c->cb      = cb;
    c->cb_user = user;
}

VmWmResult Wms_Subscribe(WmsClient* c, uint32_t mask)
{
    VmWmReqSubscribe sub;

    if (!c) return VMWM_ERR_BAD_ARG;

    sub.mask     = mask & (uint32_t)EVT_ALL;
    sub.reserved = 0;

    c->mask = sub.mask;
    return call_simple(c, CMD_SUBSCRIBE, &sub, sizeof(sub), NULL);
}

int Wms_PollEvents(WmsClient* c, int timeout_ms)
{
    uint64_t deadline;
    int      start;

    if (!c || !c->inited) return 0;

    start    = c->evt_count;
    deadline = vmwm_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);

    for (;;) {
        uv_run(&c->loop, UV_RUN_NOWAIT);
        if (c->evt_count != start) break;
        if (!c->connected) break;
        if (vmwm_now_ms() >= deadline) break;
        vmwm_sleep_ms(1);
    }

    return c->evt_count - start;
}

/* ================= 窗口操作 ================= */

VmWmResult Wms_CreateWindow(WmsClient* c, int x, int y, int w, int h,
                            uint64_t* out_win_id)
{
    VmWmReqCreate req;
    uint64_t      id = 0;
    VmWmResult    r;

    if (!c) return VMWM_ERR_BAD_ARG;
    req.x = x; req.y = y; req.w = w; req.h = h;

    r = call_simple(c, CMD_CREATE_WINDOW, &req, sizeof(req), &id);
    if (r == VMWM_OK && out_win_id) *out_win_id = id;
    return r;
}

VmWmResult Wms_CloseWindow(WmsClient* c, uint64_t win_id)
{
    VmWmReqWinId q;
    q.win_id = win_id;
    return call_simple(c, CMD_CLOSE_WINDOW, &q, sizeof(q), NULL);
}

VmWmResult Wms_MoveWindow(WmsClient* c, uint64_t win_id, int x, int y)
{
    VmWmReqMove q;
    q.win_id = win_id;
    q.x = x;
    q.y = y;
    return call_simple(c, CMD_MOVE_WINDOW, &q, sizeof(q), NULL);
}

VmWmResult Wms_ResizeWindow(WmsClient* c, uint64_t win_id, int w, int h)
{
    VmWmReqResize q;
    q.win_id = win_id;
    q.w = w;
    q.h = h;
    return call_simple(c, CMD_RESIZE_WINDOW, &q, sizeof(q), NULL);
}

VmWmResult Wms_ConfigureWindow(WmsClient* c, uint64_t win_id,
                               int x, int y, int w, int h)
{
    VmWmReqConfigure q;
    q.win_id = win_id;
    q.x = x; q.y = y; q.w = w; q.h = h;
    return call_simple(c, CMD_CONFIGURE_WIN, &q, sizeof(q), NULL);
}

VmWmResult Wms_StackWindow(WmsClient* c, uint64_t win_id, VmWmStackMode mode)
{
    VmWmReqStack q;
    q.win_id = win_id;
    q.mode = (uint32_t)mode;
    q.reserved = 0;
    return call_simple(c, CMD_STACK_WINDOW, &q, sizeof(q), NULL);
}

VmWmResult Wms_SetFocus(WmsClient* c, uint64_t win_id)
{
    VmWmReqWinId q;
    q.win_id = win_id;
    return call_simple(c, CMD_SET_FOCUS, &q, sizeof(q), NULL);
}

VmWmResult Wms_MapWindow(WmsClient* c, uint64_t win_id)
{
    VmWmReqWinId q;
    q.win_id = win_id;
    return call_simple(c, CMD_MAP_WINDOW, &q, sizeof(q), NULL);
}

VmWmResult Wms_UnmapWindow(WmsClient* c, uint64_t win_id)
{
    VmWmReqWinId q;
    q.win_id = win_id;
    return call_simple(c, CMD_UNMAP_WINDOW, &q, sizeof(q), NULL);
}

/* ================= 窗口属性 ================= */

VmWmResult Wms_SetTitle(WmsClient* c, uint64_t win_id, const char* title)
{
    VmWmReqTitle q;

    if (!c || !title) return VMWM_ERR_BAD_ARG;

    memset(&q, 0, sizeof(q));
    q.win_id = win_id;
    copy_str(q.title, sizeof(q.title), title);

    return call_simple(c, CMD_SET_TITLE, &q, sizeof(q), NULL);
}

VmWmResult Wms_SetSizeHint(WmsClient* c, uint64_t win_id,
                           int min_w, int min_h, int max_w, int max_h)
{
    VmWmReqSizeHint q;
    q.win_id = win_id;
    q.min_w = min_w; q.min_h = min_h;
    q.max_w = max_w; q.max_h = max_h;
    return call_simple(c, CMD_SET_SIZE_HINT, &q, sizeof(q), NULL);
}

VmWmResult Wms_SetTransientFor(WmsClient* c, uint64_t win_id,
                               uint64_t parent_win_id)
{
    VmWmReqTransient q;
    q.win_id = win_id;
    q.parent_id = parent_win_id;
    return call_simple(c, CMD_SET_TRANSIENT, &q, sizeof(q), NULL);
}

VmWmResult Wms_SetClassHint(WmsClient* c, uint64_t win_id, const char* cls)
{
    VmWmReqClass q;

    if (!c || !cls) return VMWM_ERR_BAD_ARG;

    memset(&q, 0, sizeof(q));
    q.win_id = win_id;
    copy_str(q.cls, sizeof(q.cls), cls);

    return call_simple(c, CMD_SET_CLASS, &q, sizeof(q), NULL);
}

/* ================= 查询 ================= */

VmWmResult Wms_GetGeometry(WmsClient* c, uint64_t win_id,
                           int* out_x, int* out_y, int* out_w, int* out_h)
{
    VmWmReqWinId q;
    VmWmBuf      reply;
    VmWmAckInfo  ai;
    VmWmResult   r;

    if (!c) return VMWM_ERR_BAD_ARG;

    q.win_id = win_id;
    vmwm_buf_init(&reply);

    r = client_call(c, CMD_GET_GEOMETRY, &q, sizeof(q), &reply, 0);
    if (r != VMWM_OK) {
        vmwm_buf_free(&reply);
        return r;
    }
    if (reply.len < sizeof(ai)) {
        vmwm_buf_free(&reply);
        return VMWM_ERR_PROTOCOL;
    }

    memcpy(&ai, reply.data, sizeof(ai));
    vmwm_buf_free(&reply);

    r = (VmWmResult)ai.result;
    if (r != VMWM_OK) return r;

    if (out_x) *out_x = ai.info.x;
    if (out_y) *out_y = ai.info.y;
    if (out_w) *out_w = ai.info.w;
    if (out_h) *out_h = ai.info.h;
    return VMWM_OK;
}

int Wms_ListWindows(WmsClient* c, VmWmWindowInfo* out, uint32_t max)
{
    VmWmBuf    reply;
    VmWmAckList lh;
    uint32_t   n;
    uint32_t   avail;
    int        r;

    if (!c || !out) return -1;

    vmwm_buf_init(&reply);
    r = client_call(c, CMD_LIST_WINDOWS, NULL, 0, &reply, 0);
    if (r != VMWM_OK) {
        vmwm_buf_free(&reply);
        return -1;
    }
    if (reply.len < sizeof(lh)) {
        vmwm_buf_free(&reply);
        return -1;
    }

    memcpy(&lh, reply.data, sizeof(lh));
    if ((VmWmResult)lh.result != VMWM_OK) {
        vmwm_buf_free(&reply);
        return -1;
    }

    n     = lh.count;
    avail = (uint32_t)((reply.len - sizeof(lh)) / sizeof(VmWmWindowInfo));
    if (n > avail) n = avail;
    if (n > max)   n = max;

    if (n) memcpy(out, (uint8_t*)reply.data + sizeof(lh),
                  n * sizeof(VmWmWindowInfo));

    vmwm_buf_free(&reply);
    return (int)n;
}

/* ================= 内容通道 ================= */

VmWmResult Wms_PushContent(WmsClient* c, uint64_t win_id, uint32_t epoch,
                           const void* cmds, uint32_t count, uint32_t cmd_size,
                           int* out_accepted, int* out_overflow)
{
    VmWmBuf        body;
    VmWmReqContent hdr;
    VmWmResult     r = VMWM_OK;
    uint64_t       value = 0;

    if (!c) return VMWM_ERR_BAD_ARG;
    if (count && (!cmds || cmd_size == 0)) return VMWM_ERR_BAD_ARG;
    if (cmd_size > VMWM_CONTENT_CMD_MAX_SIZE) return VMWM_ERR_LIMIT;
    if (count > VMWM_CONTENT_MAX_COMMANDS) return VMWM_ERR_LIMIT;
    if ((uint64_t)count * cmd_size + (uint64_t)sizeof(hdr) > VMWM_MAX_PAYLOAD)
        return VMWM_ERR_LIMIT;

    memset(&hdr, 0, sizeof(hdr));
    hdr.win_id   = win_id;
    hdr.epoch    = epoch;
    hdr.count    = count;
    hdr.cmd_size = cmd_size;
    hdr.overflow = 0;

    vmwm_buf_init(&body);
    if (!vmwm_buf_append(&body, &hdr, sizeof(hdr))) {
        vmwm_buf_free(&body);
        return VMWM_ERR_NOMEM;
    }
    if (count && !vmwm_buf_append(&body, cmds, (size_t)count * cmd_size)) {
        vmwm_buf_free(&body);
        return VMWM_ERR_NOMEM;
    }

    {
        VmWmBuf reply;
        VmWmAck ack;

        vmwm_buf_init(&reply);
        r = client_call(c, CMD_WINDOW_CONTENT, body.data, (uint32_t)body.len,
                        &reply, 0);
        vmwm_buf_free(&body);

        if (r != VMWM_OK) {
            vmwm_buf_free(&reply);
            return r;
        }
        if (reply.len < sizeof(ack)) {
            vmwm_buf_free(&reply);
            return VMWM_ERR_PROTOCOL;
        }
        memcpy(&ack, reply.data, sizeof(ack));
        value = ack.value;
        vmwm_buf_free(&reply);
        r = (VmWmResult)ack.result;
    }

    if (r != VMWM_OK) return r;

    if (out_accepted) *out_accepted = (value != 0) ? 1 : 0;
    if (out_overflow) *out_overflow = 0;      /* 服务端只在日志里告警 */
    return VMWM_OK;
}

/* ================= 输入 ================= */

VmWmResult Wms_SendInput(WmsClient* c, const VmWmReqInput* in)
{
    VmWmResult r;

    if (!c || !in) return VMWM_ERR_BAD_ARG;

    r = (VmWmResult)(send_msg(c, VMWM_CMD_TYPE_MSG, CMD_INPUT, 0,
                              in, sizeof(*in)) == VMWM_OK
                     ? VMWM_OK : VMWM_ERR_FAILED);

    /* 立刻推一次 loop，让这次写尽快落到管道上（不等 ACK，但不留着） */
    uv_run(&c->loop, UV_RUN_NOWAIT);
    return r;
}

/* ================= 全局状态 ================= */

VmWmResult Wms_SetWorkArea(WmsClient* c, int x, int y, int w, int h)
{
    VmWmReqWorkArea area;
    area.x = x; area.y = y; area.w = w; area.h = h;
    return call_simple(c, CMD_SET_WORKAREA, &area, sizeof(area), NULL);
}

VmWmResult Wms_SetTheme(WmsClient* c, const char* name)
{
    VmWmReqTheme th;

    if (!c) return VMWM_ERR_BAD_ARG;

    memset(&th, 0, sizeof(th));
    copy_str(th.name, sizeof(th.name), name ? name : "");
    return call_simple(c, CMD_THEME_SET, &th, sizeof(th), NULL);
}

/* ================= 呈现端 ================= */

VmWmResult Wms_RequestFullframe(WmsClient* c)
{
    return call_simple(c, CMD_REQUEST_FULLFRAME, NULL, 0, NULL);
}

VmWmResult Wms_RequestRespack(WmsClient* c, const char* id)
{
    VmWmReqRespack q;

    if (!c) return VMWM_ERR_BAD_ARG;

    memset(&q, 0, sizeof(q));
    copy_str(q.id, sizeof(q.id), id ? id : "");
    return call_simple(c, CMD_REQUEST_RESPACK, &q, sizeof(q), NULL);
}

/* ================= 事件载荷解码 ================= */

const VmWmWindowInfo* Wms_AsWindowInfo(const WmsEvent* e)
{
    if (!e || !e->payload || e->len < sizeof(VmWmWindowInfo)) return NULL;
    return (const VmWmWindowInfo*)e->payload;
}

const VmWmEvtLayoutReq* Wms_AsLayoutReq(const WmsEvent* e)
{
    if (!e || !e->payload || e->len < sizeof(VmWmEvtLayoutReq)) return NULL;
    return (const VmWmEvtLayoutReq*)e->payload;
}

const VmWmReqInput* Wms_AsInput(const WmsEvent* e)
{
    if (!e || !e->payload || e->len < sizeof(VmWmReqInput)) return NULL;
    return (const VmWmReqInput*)e->payload;
}

const VmWmReqTheme* Wms_AsTheme(const WmsEvent* e)
{
    if (!e || !e->payload || e->len < sizeof(VmWmReqTheme)) return NULL;
    return (const VmWmReqTheme*)e->payload;
}

const char* Wms_AsFrame(const WmsEvent* e, uint32_t* out_len,
                        uint32_t* out_frame_id)
{
    const VmWmEvtFrame* eh;

    if (!e || !e->payload || e->len < sizeof(VmWmEvtFrame)) return NULL;

    eh = (const VmWmEvtFrame*)e->payload;
    if (eh->len > e->len - (uint32_t)sizeof(VmWmEvtFrame)) return NULL;

    if (out_len)      *out_len      = eh->len;
    if (out_frame_id) *out_frame_id = eh->frame_id;

    return (const char*)((const uint8_t*)e->payload + sizeof(VmWmEvtFrame));
}

const char* Wms_AsRespack(const WmsEvent* e, char* out_id, uint32_t idcap,
                          uint32_t* out_len)
{
    const VmWmEvtRespack* rh;

    if (!e || !e->payload || e->len < sizeof(VmWmEvtRespack)) return NULL;

    rh = (const VmWmEvtRespack*)e->payload;
    if (rh->len > e->len - (uint32_t)sizeof(VmWmEvtRespack)) return NULL;

    if (out_id && idcap) copy_str(out_id, idcap, rh->id);
    if (out_len) *out_len = rh->len;

    return (const char*)((const uint8_t*)e->payload + sizeof(VmWmEvtRespack));
}
