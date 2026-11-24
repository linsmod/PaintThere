/*
 * wm_server.c - 窗口管理服务
 *
 * 职责（这就是 wmwms 这一层的全部边界）：
 *   1. 监听命名管道，接受任意多个客户端（APP / SHELL / RDP）
 *   2. 维护窗口表（位置、尺寸、标题、映射、堆叠、焦点、owner、内容代次）
 *   3. 处理请求，回 ACK；状态变化向订阅者广播事件
 *   4. 输入命中与焦点仲裁：RDP 送来的画布坐标转成窗口局部坐标，转给属主
 *   5. 连接生命周期：APP 断开即回收其窗口，SHELL 断开窗口保留（owner 置 0）
 *   6. 帧下发管道：宿主合成完把 JSON 交给本层，按订阅广播给所有 RDP
 *
 * 不做：合成、瓦片、JSON 生成、装饰绘制、资源包、HTTP —— 全在宿主里。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uv.h"

#include "wms_ipc.h"
#include "wms_codec.h"
#include "wms_server.h"
#include "wms_host.h"
#include "wms_peer.h"
#include "wms_log.h"

/* ================= 类型 ================= */

typedef struct WinSlot {
    int              used;
    uint64_t         win_id;
    VmWmWindowInfo   info;                /* 协议快照（权威） */
    uint32_t         owner;               /* 客户端 id，0 = 无主 */
    uint64_t         parent;              /* transient parent，0 = 无 */
    int32_t          min_w, min_h;        /* 0 = 不限 */
    int32_t          max_w, max_h;
    char             cls[VMWM_CLASS_MAX];
    uint32_t         epoch;               /* 内容代次：几何每变一次 +1 */
} WinSlot;

typedef struct Conn {
    uv_pipe_t      pipe;
    VmWmBuf        rx;
    uint32_t       id;
    uint32_t       type;                  /* VmWmClientType，0 = 未握手 */
    uint32_t       mask;                  /* 事件订阅掩码 */
    int            handshaked;
    int            closing;
    int            peer_dead;
    VmWmPeer       peer;                  /* 内核提供的进程身份（可能未验证） */
    struct Conn*   next;
} Conn;

typedef struct {
    uv_write_t req;
    char*      buf;
} WriteCtx;

static WinSlot        g_wins[VMWM_MAX_WINDOWS];
static uint64_t       g_next_win_id  = 1;
static uint64_t       g_created_seq  = 0;
static uint32_t       g_z_top        = 0;
static uint64_t       g_focus        = 0;

static VmWmReqWorkArea g_work;
static int32_t          g_screen_w = 0;
static int32_t          g_screen_h = 0;
static char             g_theme[32];

static VmWmHost        g_host;
static int             g_host_set = 0;

static uint32_t        g_frame_id  = 0;

static uv_loop_t*      g_loop = NULL;
static uv_pipe_t       g_server_pipe;
static uv_timer_t      g_tick_timer;
static uv_timer_t      g_peer_timer;
static int             g_inited = 0;
static Conn*           g_conns = NULL;
static uint32_t        g_next_conn_id = 1;
static uint32_t        g_conn_count = 0;

/* ================= 基础工具 ================= */

static void copy_str(char* dst, unsigned cap, const char* src)
{
    unsigned i = 0;
    if (!dst || !cap) return;
    if (src) {
        for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

static WinSlot* slot_find(uint64_t win_id)
{
    int i;
    if (win_id == 0) return NULL;
    for (i = 0; i < VMWM_MAX_WINDOWS; i++) {
        if (g_wins[i].used && g_wins[i].info.win_id == win_id)
            return &g_wins[i];
    }
    return NULL;
}

static WinSlot* slot_alloc(void)
{
    int i;
    for (i = 0; i < VMWM_MAX_WINDOWS; i++) {
        if (!g_wins[i].used) {
            memset(&g_wins[i], 0, sizeof(g_wins[i]));
            g_wins[i].used = 1;
            return &g_wins[i];
        }
    }
    return NULL;
}

static Conn* conn_by_id(uint32_t id)
{
    Conn* c;
    for (c = g_conns; c; c = c->next) {
        if (c->id == id && !c->closing) return c;
    }
    return NULL;
}

static uint32_t conn_caps(const Conn* c)
{
    if (!c) return 0;
    return VmWm_RoleCaps(c->type);
}

static const char* conn_type_name(uint32_t type)
{
    switch (type) {
    case VMWM_CLIENT_APP:   return "APP";
    case VMWM_CLIENT_SHELL: return "SHELL";
    case VMWM_CLIENT_RDP:   return "RDP";
    default:                return "?";
    }
}

/* ================= 发送 ================= */

static void on_write_done(uv_write_t* req, int status)
{
    WriteCtx* ctx = (WriteCtx*)req;
    (void)status;
    free(ctx->buf);
    free(ctx);
}

static int conn_send(Conn* c, uint16_t type, uint16_t id, uint16_t seq,
                     const void* body, uint32_t blen)
{
    VmWmBuf   b;
    WriteCtx* ctx;
    uv_buf_t  ub;
    int       r;

    if (!c || c->closing) return VMWM_ERR_FAILED;

    vmwm_buf_init(&b);
    if (vmwm_msg_pack(&b, type, id, seq, body, blen) != VMWM_OK) {
        vmwm_buf_free(&b);
        vmwm_log("pack failed: type=%u id=%u len=%u", (unsigned)type,
                 (unsigned)id, (unsigned)blen);
        return VMWM_ERR_FAILED;
    }

    ctx = (WriteCtx*)calloc(1, sizeof(WriteCtx));
    if (!ctx) {
        vmwm_buf_free(&b);
        return VMWM_ERR_NOMEM;
    }

    ctx->buf = (char*)b.data;      /* 所有权转移，省一次拷贝 */
    b.data = NULL;

    ub = uv_buf_init(ctx->buf, (unsigned int)b.len);
    r = uv_write(&ctx->req, (uv_stream_t*)&c->pipe, &ub, 1, on_write_done);
    vmwm_buf_free(&b);

    if (r != 0) {
        vmwm_log("uv_write failed: %s", uv_strerror(r));
        /* ctx->buf 由 on_write_done 释放的路径不存在，这里自己放 */
        free(ctx->buf);
        free(ctx);
        return VMWM_ERR_FAILED;
    }
    return VMWM_OK;
}

static int send_ack(Conn* c, uint16_t seq, VmWmResult result, uint64_t value)
{
    VmWmAck ack;
    ack.result   = (int32_t)result;
    ack.reserved = 0;
    ack.value    = value;
    return conn_send(c, VMWM_CMD_TYPE_ACK, 0, seq, &ack, sizeof(ack));
}

/* ================= 事件广播 ================= */

/* 窗口事件只发 APP / SHELL：RDP 从帧里拿结果，不需要元数据事件。 */
static void broadcast_window_event(uint32_t evt, const VmWmWindowInfo* info)
{
    Conn* c;
    for (c = g_conns; c; c = c->next) {
        if (c->closing) continue;
        if (c->type != VMWM_CLIENT_APP && c->type != VMWM_CLIENT_SHELL) continue;
        if ((c->mask & evt) == 0) continue;
        conn_send(c, VMWM_CMD_TYPE_EVT, (uint16_t)evt, 0, info, sizeof(*info));
    }
}

static void broadcast_theme(const char* name)
{
    VmWmReqTheme th;
    Conn* c;

    memset(&th, 0, sizeof(th));
    copy_str(th.name, sizeof(th.name), name);

    for (c = g_conns; c; c = c->next) {
        if (c->closing || !c->handshaked) continue;
        if ((c->mask & EVT_THEME_CHANGED) == 0) continue;
        conn_send(c, VMWM_CMD_TYPE_EVT, (uint16_t)EVT_THEME_CHANGED, 0,
                  &th, sizeof(th));
    }
}

static void notify_host(uint32_t evt, const VmWmWindowInfo* info)
{
    if (g_host_set && g_host.on_window_event)
        g_host.on_window_event(g_host.user, evt, info);
}

/* ================= 布局请求 ================= */

static VmWmRect body_of(const WinSlot* s)
{
    VmWmRect r;
    r.x = 0; r.y = 0; r.w = s->info.w; r.h = s->info.h;

    if (g_host_set && g_host.body_rect) {
        VmWmRect t;
        t.x = 0; t.y = 0; t.w = 0; t.h = 0;
        if (g_host.body_rect(g_host.user, s->win_id, s->info.w, s->info.h, &t) &&
            t.w > 0 && t.h > 0) {
            r = t;
        }
    }
    return r;
}

/* 给窗口属主发 EVT_LAYOUT_REQ。几何变化时由服务端自己发；宿主也可主动调。 */
VmWmResult VmWm_ServerRequestLayout(uint64_t win_id)
{
    WinSlot* s = slot_find(win_id);
    Conn*    owner;
    VmWmRect body;
    VmWmEvtLayoutReq lr;

    if (!s) return VMWM_ERR_INVALID_WINDOW;
    owner = conn_by_id(s->owner);
    if (!owner) return VMWM_ERR_NOT_CONNECTED;      /* 无主窗口没有布局对端 */

    body = body_of(s);

    lr.win_id  = s->win_id;
    lr.epoch   = s->epoch;
    lr.width   = body.w;
    lr.height  = body.h;

    return (VmWmResult)(conn_send(owner, VMWM_CMD_TYPE_EVT,
                                  (uint16_t)EVT_LAYOUT_REQ, 0,
                                  &lr, sizeof(lr)) == VMWM_OK
                        ? VMWM_OK : VMWM_ERR_FAILED);
}

/* 几何/尺寸真的变了：代次 +1、广播 CONFIGURE、通知宿主、请属主重跑布局 */
static void on_geometry_changed(WinSlot* s)
{
    s->epoch++;
    s->info.z = ++g_z_top;
    broadcast_window_event(EVT_WINDOW_CONFIGURE, &s->info);
    notify_host(EVT_WINDOW_CONFIGURE, &s->info);
    VmWm_ServerRequestLayout(s->win_id);
}

/* ================= 焦点 ================= */

static void apply_focus(uint64_t win_id)
{
    int i;
    WinSlot* target;

    if (g_focus == win_id) return;
    g_focus = win_id;

    for (i = 0; i < VMWM_MAX_WINDOWS; i++) {
        if (!g_wins[i].used) continue;
        g_wins[i].info.focused =
            (g_wins[i].info.win_id == win_id) ? 1 : 0;
    }

    target = slot_find(win_id);
    if (target) {
        broadcast_window_event(EVT_FOCUS_CHANGE, &target->info);
        notify_host(EVT_FOCUS_CHANGE, &target->info);
    }
}

/* ================= 窗口内核操作 ================= */

VmWmResult VmWm_CreateWindow(const VmWmReqCreate* req, uint32_t owner,
                             uint64_t* out_id)
{
    WinSlot* s;

    if (!req) return VMWM_ERR_BAD_ARG;
    if (req->w <= 0 || req->h <= 0) return VMWM_ERR_BAD_ARG;

    s = slot_alloc();
    if (!s) return VMWM_ERR_LIMIT;

    s->win_id    = g_next_win_id++;
    s->owner     = owner;
    s->epoch     = 1;

    memset(&s->info, 0, sizeof(s->info));
    s->info.win_id      = s->win_id;
    s->info.x           = req->x;
    s->info.y           = req->y;
    s->info.w           = req->w;
    s->info.h           = req->h;
    s->info.mapped      = 0;
    s->info.stack       = (uint32_t)VMWM_STACK_TOP;
    s->info.z           = ++g_z_top;
    s->info.created_seq = (uint32_t)++g_created_seq;
    s->info.focused     = 0;

    if (out_id) *out_id = s->win_id;

    broadcast_window_event(EVT_WINDOW_NEW, &s->info);
    notify_host(EVT_WINDOW_NEW, &s->info);
    VmWm_ServerRequestLayout(s->win_id);

    vmwm_log("create window id=%llu pos=(%d,%d) size=%dx%d owner=%u",
             (unsigned long long)s->win_id, s->info.x, s->info.y,
             s->info.w, s->info.h, (unsigned)owner);
    return VMWM_OK;
}

VmWmResult VmWm_ServerRemoveWindow(uint64_t win_id, int32_t* out_prev_focus)
{
    WinSlot* s = slot_find(win_id);
    VmWmWindowInfo info;

    if (!s) return VMWM_ERR_INVALID_WINDOW;

    info = s->info;
    info.mapped  = 0;
    info.focused = 0;

    if (out_prev_focus) *out_prev_focus = (int32_t)g_focus;

    s->used = 0;

    if (g_focus == win_id) {
        g_focus = 0;
        {
            int i;
            for (i = 0; i < VMWM_MAX_WINDOWS; i++)
                if (g_wins[i].used) g_wins[i].info.focused = 0;
        }
    }

    if (g_host_set && g_host.on_window_removed)
        g_host.on_window_removed(g_host.user, win_id);

    broadcast_window_event(EVT_WINDOW_CLOSE, &info);
    notify_host(EVT_WINDOW_CLOSE, &info);

    vmwm_log("remove window id=%llu", (unsigned long long)win_id);
    return VMWM_OK;
}

static void clamp_size(WinSlot* s, int32_t* w, int32_t* h)
{
    if (s->min_w > 0 && *w < s->min_w) *w = s->min_w;
    if (s->min_h > 0 && *h < s->min_h) *h = s->min_h;
    if (s->max_w > 0 && *w > s->max_w) *w = s->max_w;
    if (s->max_h > 0 && *h > s->max_h) *h = s->max_h;
}

VmWmResult VmWm_MoveWindow(uint64_t win_id, int32_t x, int32_t y)
{
    WinSlot* s = slot_find(win_id);
    if (!s) return VMWM_ERR_INVALID_WINDOW;
    if (s->info.x == x && s->info.y == y) return VMWM_OK;
    s->info.x = x;
    s->info.y = y;
    on_geometry_changed(s);
    return VMWM_OK;
}

VmWmResult VmWm_ResizeWindow(uint64_t win_id, int32_t w, int32_t h)
{
    WinSlot* s = slot_find(win_id);
    if (!s) return VMWM_ERR_INVALID_WINDOW;
    if (w <= 0 || h <= 0) return VMWM_ERR_BAD_ARG;
    clamp_size(s, &w, &h);
    if (s->info.w == w && s->info.h == h) return VMWM_OK;
    s->info.w = w;
    s->info.h = h;
    on_geometry_changed(s);
    return VMWM_OK;
}

VmWmResult VmWm_ConfigureWindow(uint64_t win_id, int32_t x, int32_t y,
                                int32_t w, int32_t h)
{
    WinSlot* s = slot_find(win_id);
    if (!s) return VMWM_ERR_INVALID_WINDOW;
    if (w <= 0 || h <= 0) return VMWM_ERR_BAD_ARG;
    clamp_size(s, &w, &h);
    if (s->info.x == x && s->info.y == y && s->info.w == w && s->info.h == h)
        return VMWM_OK;
    s->info.x = x;
    s->info.y = y;
    s->info.w = w;
    s->info.h = h;
    on_geometry_changed(s);
    return VMWM_OK;
}

VmWmResult VmWm_StackWindow(uint64_t win_id, uint32_t mode)
{
    WinSlot* s = slot_find(win_id);
    if (!s) return VMWM_ERR_INVALID_WINDOW;
    if (mode > (uint32_t)VMWM_STACK_BOTTOM) return VMWM_ERR_BAD_ARG;
    s->info.stack = mode;
    if (mode != (uint32_t)VMWM_STACK_BOTTOM) s->info.z = ++g_z_top;
    broadcast_window_event(EVT_WINDOW_CONFIGURE, &s->info);
    notify_host(EVT_WINDOW_CONFIGURE, &s->info);
    return VMWM_OK;
}

VmWmResult VmWm_SetFocus(uint64_t win_id)
{
    WinSlot* s = slot_find(win_id);
    if (!s) return VMWM_ERR_INVALID_WINDOW;
    if (g_focus == win_id) return VMWM_OK;
    if (s->info.stack != (uint32_t)VMWM_STACK_BOTTOM) s->info.z = ++g_z_top;
    apply_focus(win_id);
    return VMWM_OK;
}

VmWmResult VmWm_MapWindow(uint64_t win_id, int mapped)
{
    WinSlot* s = slot_find(win_id);
    if (!s) return VMWM_ERR_INVALID_WINDOW;
    if (s->info.mapped == (uint32_t)(mapped ? 1 : 0)) return VMWM_OK;
    s->info.mapped = mapped ? 1u : 0u;
    broadcast_window_event(EVT_WINDOW_CONFIGURE, &s->info);
    notify_host(EVT_WINDOW_CONFIGURE, &s->info);
    return VMWM_OK;
}

VmWmResult VmWm_SetWindowTitle(uint64_t win_id, const char* title)
{
    WinSlot* s = slot_find(win_id);
    if (!s) return VMWM_ERR_INVALID_WINDOW;
    if (!title) return VMWM_ERR_BAD_ARG;
    copy_str(s->info.title, sizeof(s->info.title), title);
    broadcast_window_event(EVT_TITLE_CHANGE, &s->info);
    notify_host(EVT_TITLE_CHANGE, &s->info);
    return VMWM_OK;
}

VmWmResult VmWm_GetWindowInfo(uint64_t win_id, VmWmWindowInfo* out)
{
    WinSlot* s = slot_find(win_id);
    if (!s || !out) return VMWM_ERR_INVALID_WINDOW;
    out->win_id      = s->info.win_id;
    out->x           = s->info.x;
    out->y           = s->info.y;
    out->w           = s->info.w;
    out->h           = s->info.h;
    out->mapped      = s->info.mapped;
    out->stack       = s->info.stack;
    out->z           = s->info.z;
    out->created_seq = s->info.created_seq;
    out->focused     = s->info.focused;
    memcpy(out->title, s->info.title, sizeof(out->title));
    return VMWM_OK;
}

/* 按 z 从高到低返回，外壳可以直接按顺序渲染条目 */
uint32_t VmWm_ListWindows(VmWmWindowInfo* out, uint32_t max)
{
    uint32_t n = 0;
    uint32_t i, j;
    int      k;

    if (!out) return 0;

    for (k = 0; k < VMWM_MAX_WINDOWS; k++) {
        if (g_wins[k].used) out[n++] = g_wins[k].info;
    }

    /* 选择排序：n <= 128，且只在快照时发生 */
    for (i = 0; i + 1 < n; i++) {
        uint32_t top = i;
        for (j = i + 1; j < n; j++) {
            if (out[j].z > out[top].z) top = j;
        }
        if (top != i) {
            VmWmWindowInfo t = out[i];
            out[i]   = out[top];
            out[top] = t;
        }
    }

    if (n > max) n = max;
    return n;
}

uint64_t VmWm_GetFocus(void)
{
    return g_focus;
}

const VmWmReqWorkArea* VmWm_GetWorkArea(void)
{
    return &g_work;
}

void VmWm_ServerSetScreenSize(int32_t w, int32_t h)
{
    g_screen_w = w;
    g_screen_h = h;
    if (g_work.w == 0 || g_work.h == 0) {
        g_work.x = 0; g_work.y = 0; g_work.w = w; g_work.h = h;
    }
}

/* ================= 输入 ================= */

static uint64_t default_hit_test(int32_t x, int32_t y)
{
    WinSlot* best = NULL;
    int      i;

    for (i = 0; i < VMWM_MAX_WINDOWS; i++) {
        WinSlot* s = &g_wins[i];
        if (!s->used || !s->info.mapped) continue;
        if (x < s->info.x || y < s->info.y) continue;
        if (x >= s->info.x + s->info.w || y >= s->info.y + s->info.h) continue;
        if (!best || s->info.z > best->info.z) best = s;
    }
    return best ? best->win_id : 0;
}

static void raise_window(WinSlot* s)
{
    if (s->info.stack == (uint32_t)VMWM_STACK_BOTTOM) return;
    s->info.z = ++g_z_top;
    broadcast_window_event(EVT_WINDOW_CONFIGURE, &s->info);
    notify_host(EVT_WINDOW_CONFIGURE, &s->info);
}

/* 输入统一走这里：CMD_INPUT（MSG）与后续可能的其它来源都复用
 *
 * 发出的 EVT_INPUT 一律是**body 局部坐标**：客户端回推的内容画的是 body，
 * 所以输入也必须在同一坐标系里，否则应用自己还得知道装饰偏移。多余的位移
 * 由服务端按宿主给的 body_rect 减掉。 */
static void route_input(uint32_t from_client, const VmWmReqInput* in)
{
    WinSlot* s = NULL;
    int32_t  lx = in->x;
    int32_t  ly = in->y;
    VmWmRect body;
    Conn*    owner;
    VmWmReqInput out;

    if (in->win_id) {
        s = slot_find(in->win_id);          /* 坐标是窗口内坐标（相对窗口左上角） */
    } else {
        uint64_t hit = 0;
        if (g_host_set && g_host.hit_test)
            hit = g_host.hit_test(g_host.user, in->x, in->y);
        if (!hit) hit = default_hit_test(in->x, in->y);
        if (!hit && in->kind == 4) hit = g_focus;   /* 键盘无命中 → 焦点窗口 */
        s = slot_find(hit);
        if (s) {
            lx = in->x - s->info.x;
            ly = in->y - s->info.y;
        }
    }

    if (!s) return;                          /* 没有窗口接这条输入，丢弃 */

    body = body_of(s);                       /* 只剩 body 偏移要减 */
    lx  -= body.x;
    ly  -= body.y;

    if (in->kind == 1 && in->is_down) {      /* 按下即抬升 + 焦点仲裁 */
        if (g_focus != s->win_id) {
            raise_window(s);
            apply_focus(s->win_id);
        }
    }

    owner = conn_by_id(s->owner);
    if (!owner) return;                      /* 无主窗口没人处理输入 */

    out      = *in;                          /* 拷贝后改写 win_id 与坐标 */
    out.win_id = s->win_id;
    out.x      = lx;
    out.y      = ly;

    conn_send(owner, VMWM_CMD_TYPE_EVT, (uint16_t)EVT_INPUT, 0,
              &out, sizeof(out));

    (void)from_client;
}

/* ================= 请求分发 ================= */

#define CAP_FAIL(c, cap, seq, what)                                  \
    do {                                                             \
        if ((conn_caps(c) & (cap)) == 0) {                            \
            vmwm_log("client id=%u (%s) denied %s (cap 0x%x)",       \
                     (unsigned)(c)->id, conn_type_name((c)->type),    \
                     (what), (unsigned)(cap));                        \
            send_ack(c, seq, VMWM_ERR_PERMISSION, 0);                 \
            return;                                                  \
        }                                                            \
    } while (0)

#define NEED(sz, seq)                                                \
    do {                                                             \
        if (blen < (uint32_t)(sz)) {                                 \
            send_ack(c, seq, VMWM_ERR_BAD_ARG, 0);                    \
            return;                                                  \
        }                                                            \
    } while (0)

static void handle_request(Conn* c, uint16_t cmdid, uint16_t seq,
                           const uint8_t* body, uint32_t blen)
{
    VmWmResult r = VMWM_OK;

    switch (cmdid) {
    case CMD_HANDSHAKE: {
        VmWmReqHello   hello;
        VmWmAckHello   ack;
        char           peer_desc[128];

        NEED(sizeof(hello), seq);
        memcpy(&hello, body, sizeof(hello));

        if (hello.version != VMWM_VERSION) {
            vmwm_log("client id=%u protocol version %u != %u",
                     (unsigned)c->id, (unsigned)hello.version,
                     (unsigned)VMWM_VERSION);
            send_ack(c, seq, VMWM_ERR_PROTOCOL, 0);
            return;
        }
        if (hello.client_type != VMWM_CLIENT_APP &&
            hello.client_type != VMWM_CLIENT_SHELL &&
            hello.client_type != VMWM_CLIENT_RDP) {
            send_ack(c, seq, VMWM_ERR_BAD_ARG, 0);
            return;
        }

        c->type       = hello.client_type;
        c->handshaked = 1;

        vmwm_log("handshake client id=%u type=%s %s", (unsigned)c->id,
                 conn_type_name(c->type),
                 VmWm_PeerDescribe(&c->peer, peer_desc, sizeof(peer_desc)));

        memset(&ack, 0, sizeof(ack));
        ack.result               = (int32_t)VMWM_OK;
        ack.client_id            = c->id;
        ack.screen_w             = g_screen_w;
        ack.screen_h             = g_screen_h;
        ack.work_x               = g_work.x;
        ack.work_y               = g_work.y;
        ack.work_w               = g_work.w;
        ack.work_h               = g_work.h;
        ack.layout_timeout_frames = VMWM_LAYOUT_TIMEOUT_FRAMES;

        conn_send(c, VMWM_CMD_TYPE_ACK, 0, seq, &ack, sizeof(ack));
        return;
    }
    case CMD_SUBSCRIBE: {
        VmWmReqSubscribe sub;
        NEED(sizeof(sub), seq);
        memcpy(&sub, body, sizeof(sub));
        c->mask = sub.mask & (uint32_t)EVT_ALL;
        send_ack(c, seq, VMWM_OK, c->mask);
        return;
    }
    case CMD_CREATE_WINDOW: {
        VmWmReqCreate req;
        uint64_t      new_id = 0;
        CAP_FAIL(c, VMWM_CAP_OWN_WINDOWS, seq, "create");
        NEED(sizeof(req), seq);
        memcpy(&req, body, sizeof(req));
        r = VmWm_CreateWindow(&req, c->id, &new_id);
        send_ack(c, seq, r, new_id);
        return;
    }
    case CMD_CLOSE_WINDOW: {
        VmWmReqWinId q;
        WinSlot*     s;
        NEED(sizeof(q), seq);
        memcpy(&q, body, sizeof(q));
        s = slot_find(q.win_id);
        if (!s)                                      r = VMWM_ERR_INVALID_WINDOW;
        else if (!(conn_caps(c) & (VMWM_CAP_OWN_WINDOWS | VMWM_CAP_ALL_WINDOWS)))
                                                     r = VMWM_ERR_PERMISSION;
        else if (!(conn_caps(c) & VMWM_CAP_ALL_WINDOWS) && s->owner != c->id)
                                                     r = VMWM_ERR_PERMISSION;
        else {
            int32_t dummy = 0;
            r = VmWm_ServerRemoveWindow(q.win_id, &dummy);
        }
        send_ack(c, seq, r, 0);
        return;
    }
    case CMD_MOVE_WINDOW:
    case CMD_RESIZE_WINDOW:
    case CMD_CONFIGURE_WIN:
    case CMD_STACK_WINDOW:
    case CMD_MAP_WINDOW:
    case CMD_UNMAP_WINDOW:
    case CMD_SET_TITLE:
    case CMD_SET_SIZE_HINT:
    case CMD_SET_TRANSIENT:
    case CMD_SET_CLASS:
    case CMD_GET_GEOMETRY: {
        /* 统一先做「归属 + 能力」判断，再按命令取值 */
        uint64_t win_id = 0;
        WinSlot* s;
        int      allowed;

        CAP_FAIL(c, VMWM_CAP_OWN_WINDOWS | VMWM_CAP_ALL_WINDOWS, seq, "window op");

        switch (cmdid) {
        case CMD_MOVE_WINDOW: {
            VmWmReqMove q; NEED(sizeof(q), seq); memcpy(&q, body, sizeof(q));
            win_id = q.win_id; break;
        }
        case CMD_RESIZE_WINDOW: {
            VmWmReqResize q; NEED(sizeof(q), seq); memcpy(&q, body, sizeof(q));
            win_id = q.win_id; break;
        }
        case CMD_CONFIGURE_WIN: {
            VmWmReqConfigure q; NEED(sizeof(q), seq); memcpy(&q, body, sizeof(q));
            win_id = q.win_id; break;
        }
        case CMD_STACK_WINDOW: {
            VmWmReqStack q; NEED(sizeof(q), seq); memcpy(&q, body, sizeof(q));
            win_id = q.win_id; break;
        }
        case CMD_MAP_WINDOW:
        case CMD_UNMAP_WINDOW:
        case CMD_GET_GEOMETRY: {
            VmWmReqWinId q; NEED(sizeof(q), seq); memcpy(&q, body, sizeof(q));
            win_id = q.win_id; break;
        }
        case CMD_SET_TITLE: {
            VmWmReqTitle q; NEED(sizeof(q), seq); memcpy(&q, body, sizeof(q));
            q.title[sizeof(q.title) - 1] = '\0';
            win_id = q.win_id; break;
        }
        case CMD_SET_SIZE_HINT: {
            VmWmReqSizeHint q; NEED(sizeof(q), seq); memcpy(&q, body, sizeof(q));
            win_id = q.win_id; break;
        }
        case CMD_SET_TRANSIENT: {
            VmWmReqTransient q; NEED(sizeof(q), seq); memcpy(&q, body, sizeof(q));
            win_id = q.win_id; break;
        }
        default: { /* CMD_SET_CLASS */
            VmWmReqClass q; NEED(sizeof(q), seq); memcpy(&q, body, sizeof(q));
            q.cls[sizeof(q.cls) - 1] = '\0';
            win_id = q.win_id; break;
        }
        }

        s = slot_find(win_id);
        if (!s) { send_ack(c, seq, VMWM_ERR_INVALID_WINDOW, 0); return; }

        allowed = (conn_caps(c) & VMWM_CAP_ALL_WINDOWS) ? 1 : (s->owner == c->id);
        if (!allowed) {
            vmwm_log("client id=%u denied window op %u on win=%llu",
                     (unsigned)c->id, (unsigned)cmdid,
                     (unsigned long long)win_id);
            send_ack(c, seq, VMWM_ERR_PERMISSION, 0);
            return;
        }

        switch (cmdid) {
        case CMD_MOVE_WINDOW: {
            VmWmReqMove q;
            memcpy(&q, body, sizeof(q));
            r = VmWm_MoveWindow(q.win_id, q.x, q.y);
            break;
        }
        case CMD_RESIZE_WINDOW: {
            VmWmReqResize q;
            memcpy(&q, body, sizeof(q));
            r = VmWm_ResizeWindow(q.win_id, q.w, q.h);
            break;
        }
        case CMD_CONFIGURE_WIN: {
            VmWmReqConfigure q;
            memcpy(&q, body, sizeof(q));
            r = VmWm_ConfigureWindow(q.win_id, q.x, q.y, q.w, q.h);
            break;
        }
        case CMD_STACK_WINDOW: {
            VmWmReqStack q;
            memcpy(&q, body, sizeof(q));
            r = VmWm_StackWindow(q.win_id, q.mode);
            break;
        }
        case CMD_MAP_WINDOW:
        case CMD_UNMAP_WINDOW:
            r = VmWm_MapWindow(win_id, cmdid == CMD_MAP_WINDOW ? 1 : 0);
            break;
        case CMD_SET_TITLE: {
            VmWmReqTitle q;
            memcpy(&q, body, sizeof(q));
            r = VmWm_SetWindowTitle(q.win_id, q.title);
            break;
        }
        case CMD_SET_SIZE_HINT: {
            VmWmReqSizeHint q;
            memcpy(&q, body, sizeof(q));
            s->min_w = q.min_w; s->min_h = q.min_h;
            s->max_w = q.max_w; s->max_h = q.max_h;
            r = VMWM_OK;
            break;
        }
        case CMD_SET_TRANSIENT: {
            VmWmReqTransient q;
            memcpy(&q, body, sizeof(q));
            s->parent = q.parent_id;
            r = VMWM_OK;
            break;
        }
        case CMD_SET_CLASS: {
            VmWmReqClass q;
            memcpy(&q, body, sizeof(q));
            copy_str(s->cls, sizeof(s->cls), q.cls);
            r = VMWM_OK;
            break;
        }
        case CMD_GET_GEOMETRY: {
            VmWmAckInfo ai;
            memset(&ai, 0, sizeof(ai));
            ai.result = (int32_t)VMWM_OK;
            ai.info   = s->info;
            conn_send(c, VMWM_CMD_TYPE_ACK, 0, seq, &ai, sizeof(ai));
            return;
        }
        default:
            r = VMWM_ERR_BAD_ARG;
            break;
        }

        send_ack(c, seq, r, 0);
        return;
    }
    case CMD_SET_FOCUS: {
        VmWmReqWinId q;
        WinSlot*     s;
        CAP_FAIL(c, VMWM_CAP_OWN_WINDOWS | VMWM_CAP_ALL_WINDOWS, seq, "focus");
        NEED(sizeof(q), seq);
        memcpy(&q, body, sizeof(q));
        s = slot_find(q.win_id);
        if (!s) r = VMWM_ERR_INVALID_WINDOW;
        else if (!(conn_caps(c) & VMWM_CAP_ALL_WINDOWS) && s->owner != c->id)
            r = VMWM_ERR_PERMISSION;
        else r = VmWm_SetFocus(q.win_id);
        send_ack(c, seq, r, 0);
        return;
    }
    case CMD_LIST_WINDOWS: {
        VmWmWindowInfo list[VMWM_MAX_WINDOWS];
        VmWmBuf        out;
        VmWmAckList    lh;
        uint32_t       n;
        CAP_FAIL(c, VMWM_CAP_ENUM_WINDOWS, seq, "list");
        (void)body; (void)blen;

        n = VmWm_ListWindows(list, VMWM_MAX_WINDOWS);

        lh.result = (int32_t)VMWM_OK;
        lh.count  = n;

        vmwm_buf_init(&out);
        if (!vmwm_buf_append(&out, &lh, sizeof(lh)) ||
            (n && !vmwm_buf_append(&out, list, n * sizeof(VmWmWindowInfo)))) {
            vmwm_buf_free(&out);
            send_ack(c, seq, VMWM_ERR_NOMEM, 0);
            return;
        }
        conn_send(c, VMWM_CMD_TYPE_ACK, 0, seq, out.data, (uint32_t)out.len);
        vmwm_buf_free(&out);
        return;
    }
    case CMD_WINDOW_CONTENT: {
        VmWmMsg        m;
        uint32_t       count = 0, cmd_size = 0;
        int            overflow = 0;
        VmWmReqContent hdr;
        WinSlot*       s;

        CAP_FAIL(c, VMWM_CAP_CONTENT, seq, "content");
        NEED(sizeof(hdr), seq);
        memcpy(&hdr, body, sizeof(hdr));

        s = slot_find(hdr.win_id);
        if (!s) { send_ack(c, seq, VMWM_ERR_INVALID_WINDOW, 0); return; }
        if (!(conn_caps(c) & VMWM_CAP_ALL_WINDOWS) && s->owner != c->id) {
            send_ack(c, seq, VMWM_ERR_PERMISSION, 0);
            return;
        }

        m.body     = body;
        m.body_len = blen;
        r = vmwm_content_validate(&m, &count, &cmd_size, &overflow);
        if (r != VMWM_OK) {
            vmwm_log("client id=%u bad content payload (win=%llu len=%u)",
                     (unsigned)c->id, (unsigned long long)hdr.win_id,
                     (unsigned)blen);
            send_ack(c, seq, r, 0);
            return;
        }

        if (hdr.epoch != s->epoch) {          /* 旧代次：丢弃，value=0 */
            send_ack(c, seq, VMWM_OK, 0);
            return;
        }
        if (!g_host_set || !g_host.on_content) {
            send_ack(c, seq, VMWM_ERR_NOT_READY, 0);
            return;
        }

        g_host.on_content(g_host.user, hdr.win_id, hdr.epoch, count, cmd_size,
                          body + sizeof(VmWmReqContent), overflow);
        send_ack(c, seq, VMWM_OK, 1);
        return;
    }
    case CMD_SET_WORKAREA: {
        VmWmReqWorkArea area;
        CAP_FAIL(c, VMWM_CAP_WORKAREA, seq, "workarea");
        NEED(sizeof(area), seq);
        memcpy(&area, body, sizeof(area));
        if (area.w <= 0 || area.h <= 0) {
            send_ack(c, seq, VMWM_ERR_BAD_ARG, 0);
            return;
        }
        g_work = area;
        vmwm_log("workarea = (%d,%d) %dx%d", area.x, area.y, area.w, area.h);
        if (g_host_set && g_host.on_workarea)
            g_host.on_workarea(g_host.user, &g_work);
        send_ack(c, seq, VMWM_OK, 0);
        return;
    }
    case CMD_THEME_SET: {
        VmWmReqTheme th;
        CAP_FAIL(c, VMWM_CAP_THEME, seq, "theme");
        NEED(sizeof(th), seq);
        memcpy(&th, body, sizeof(th));
        th.name[sizeof(th.name) - 1] = '\0';
        copy_str(g_theme, sizeof(g_theme), th.name);
        vmwm_log("theme -> '%s'", g_theme);
        if (g_host_set && g_host.on_theme)
            g_host.on_theme(g_host.user, g_theme);
        broadcast_theme(g_theme);
        send_ack(c, seq, VMWM_OK, 0);
        return;
    }
    case CMD_REQUEST_FULLFRAME: {
        CAP_FAIL(c, VMWM_CAP_FRAME, seq, "fullframe");
        if (!g_host_set || !g_host.on_frame_request) {
            send_ack(c, seq, VMWM_ERR_NOT_READY, 0);
            return;
        }
        /* ACK 只表示「已受理」，数据走 EVT_FULLFRAME，避免 1 MB 塞进 ACK */
        send_ack(c, seq, VMWM_OK, g_frame_id);
        g_host.on_frame_request(g_host.user, c->id);
        return;
    }
    case CMD_REQUEST_RESPACK: {
        VmWmReqRespack q;
        CAP_FAIL(c, VMWM_CAP_RESPACK, seq, "respack");
        NEED(sizeof(q), seq);
        memcpy(&q, body, sizeof(q));
        q.id[sizeof(q.id) - 1] = '\0';
        if (!g_host_set || !g_host.on_respack_request) {
            send_ack(c, seq, VMWM_ERR_NOT_READY, 0);
            return;
        }
        send_ack(c, seq, VMWM_OK, 0);
        g_host.on_respack_request(g_host.user, c->id, q.id);
        return;
    }
    default:
        vmwm_log("unknown cmd %u from client id=%u (ignored, not fatal)",
                 (unsigned)cmdid, (unsigned)c->id);
        send_ack(c, seq, VMWM_ERR_PROTOCOL, 0);
        return;
    }
}

/* 单向消息：不产生 ACK，出错只能记日志 */
static void handle_message(Conn* c, uint16_t cmdid, const uint8_t* body,
                           uint32_t blen)
{
    switch (cmdid) {
    case CMD_INPUT: {
        VmWmReqInput in;
        if ((conn_caps(c) & VMWM_CAP_INPUT) == 0) {
            vmwm_log("client id=%u (%s) denied input", (unsigned)c->id,
                     conn_type_name(c->type));
            return;
        }
        if (blen < sizeof(in)) {
            vmwm_log("client id=%u truncated input (%u)", (unsigned)c->id,
                     (unsigned)blen);
            return;
        }
        memcpy(&in, body, sizeof(in));
        in.key[sizeof(in.key) - 1]   = '\0';
        in.code[sizeof(in.code) - 1] = '\0';
        route_input(c->id, &in);
        return;
    }
    default:
        vmwm_log("unknown MSG %u from client id=%u (ignored)", (unsigned)cmdid,
                 (unsigned)c->id);
        return;
    }
}

/* ================= 连接管理 ================= */

static void conn_unlink(Conn* c)
{
    Conn** pp = &g_conns;
    while (*pp) {
        if (*pp == c) {
            *pp = c->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void on_conn_closed(uv_handle_t* handle)
{
    Conn* c = (Conn*)handle->data;
    int   i;

    if (!c) return;

    conn_unlink(c);
    if (g_conn_count) g_conn_count--;

    if (c->type == VMWM_CLIENT_SHELL) {
        /* 外壳掉线：窗口保留，owner 置 0，重连后可继续管理 */
        for (i = 0; i < VMWM_MAX_WINDOWS; i++) {
            if (g_wins[i].used && g_wins[i].owner == c->id)
                g_wins[i].owner = 0;
        }
    } else {
        /* APP / RDP：名下窗口全部回收 */
        for (i = 0; i < VMWM_MAX_WINDOWS; i++) {
            if (g_wins[i].used && g_wins[i].owner == c->id) {
                int32_t dummy = 0;
                VmWm_ServerRemoveWindow(g_wins[i].info.win_id, &dummy);
            }
        }
    }

    vmwm_log("client id=%u (%s) disconnected%s, %u bytes buffered dropped",
             (unsigned)c->id, conn_type_name(c->type),
             c->peer_dead ? " (peer process exited)" : "",
             (unsigned)c->rx.len);

    VmWm_PeerRelease(&c->peer);
    vmwm_buf_free(&c->rx);
    free(c);
}

static void close_conn(Conn* c, const char* why)
{
    if (!c || c->closing) return;
    c->closing = 1;
    if (why) vmwm_log("closing client id=%u: %s", (unsigned)c->id, why);
    vmwm_buf_free(&c->rx);
    if (!uv_is_closing((uv_handle_t*)&c->pipe))
        uv_close((uv_handle_t*)&c->pipe, on_conn_closed);
}

static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
    Conn* c = (Conn*)stream->data;

    if (nread < 0) {
        free(buf->base);
        close_conn(c, uv_strerror((int)nread));
        return;
    }
    if (nread == 0) {
        free(buf->base);
        return;
    }

    if (!vmwm_buf_append(&c->rx, buf->base, (size_t)nread)) {
        free(buf->base);
        close_conn(c, "rx buffer over limit");
        return;
    }
    free(buf->base);

    for (;;) {
        VmWmMsg m;
        VmWmParse pr = vmwm_msg_parse(&c->rx, &m);

        if (pr == VMWM_PARSE_NEED_MORE) break;
        if (pr == VMWM_PARSE_ERROR) {
            close_conn(c, "bad frame (magic or body_len)");
            return;
        }

        if (m.type == VMWM_CMD_TYPE_REQ) {
            handle_request(c, m.id, m.hdr.seq, m.body, m.body_len);
        } else if (m.type == VMWM_CMD_TYPE_MSG) {
            handle_message(c, m.id, m.body, m.body_len);
        } else {
            /* ACK / EVT 不应该由客户端发来：忽略，不断连 */
            vmwm_log("client id=%u sent type %u (ignored)", (unsigned)c->id,
                     (unsigned)m.type);
        }

        vmwm_buf_consume(&c->rx, m.total);
        if (c->closing) return;
    }
}

static void on_alloc(uv_handle_t* handle, size_t suggested, uv_buf_t* buf)
{
    (void)handle;
    (void)suggested;
    buf->base = (char*)malloc(VMWM_READ_CHUNK);
    buf->len  = buf->base ? VMWM_READ_CHUNK : 0;
}

static void on_new_connection(uv_stream_t* server, int status)
{
    Conn* c;
    uv_os_fd_t fd;

    if (status < 0) {
        vmwm_log("accept failed: %s", uv_strerror(status));
        return;
    }

    if (g_conn_count >= VMWM_MAX_CLIENTS) {
        vmwm_log("connection refused: client limit %u reached",
                 (unsigned)VMWM_MAX_CLIENTS);
        /* 不 accept 会让 libuv 反复回调；接受后立即关掉更干净 */
    }

    c = (Conn*)calloc(1, sizeof(Conn));
    if (!c) {
        vmwm_log("out of memory: cannot accept client");
        return;
    }

    uv_pipe_init(server->loop, &c->pipe, 1);
    c->pipe.data = c;
    c->id        = g_next_conn_id++;
    c->type      = 0;
    c->mask      = 0;
    vmwm_buf_init(&c->rx);

    if (uv_accept(server, (uv_stream_t*)&c->pipe) != 0) {
        uv_close((uv_handle_t*)&c->pipe, NULL);
        free(c);
        vmwm_log("uv_accept failed");
        return;
    }

    /* 进程身份：内核给，不用客户端自报 */
    fd = 0;
    if (uv_fileno((uv_handle_t*)&c->pipe, &fd) == 0 && fd) {
        c->peer = VmWm_PeerFromFd((intptr_t)fd);
    }

    c->next    = g_conns;
    g_conns    = c;
    g_conn_count++;

    if (g_conn_count > VMWM_MAX_CLIENTS) {
        close_conn(c, "client limit reached");
        return;
    }

    if (uv_read_start((uv_stream_t*)&c->pipe, on_alloc, on_read) != 0) {
        close_conn(c, "uv_read_start failed");
        return;
    }

    vmwm_log("client id=%u connected%s", (unsigned)c->id,
             c->peer.verified ? " (peer verified)" : "");
}

/* ================= 定时器 ================= */

static void on_tick(uv_timer_t* timer)
{
    (void)timer;
    g_frame_id++;
    if (g_host_set && g_host.on_tick)
        g_host.on_tick(g_host.user, g_frame_id);
}

/* 进程级探活：管道断开会正常触发 on_read<0，这一路兜住「连接被继承」
 * 或「管道没断但进程已死」的情况。判定只用句柄/pidfd，不看 pid 数值。 */
static void on_peer_timer(uv_timer_t* timer)
{
    Conn* c;
    (void)timer;

    for (c = g_conns; c; c = c->next) {
        if (c->closing || !c->peer.verified) continue;
        if (VmWm_PeerAlive(&c->peer) == 0) {
            c->peer_dead = 1;
            close_conn(c, "peer process exited");
        }
    }
}

/* ================= 帧下发 ================= */

static VmWmResult send_frame_to(Conn* c, const char* json, uint32_t len, int full)
{
    VmWmBuf        body;
    VmWmEvtFrame   eh;
    VmWmResult     r;

    if (!c || c->closing) return VMWM_ERR_NOT_CONNECTED;
    if (!(conn_caps(c) & VMWM_CAP_FRAME)) return VMWM_ERR_PERMISSION;
    if (len && !json) return VMWM_ERR_BAD_ARG;
    if (len > VMWM_MAX_PAYLOAD - sizeof(eh)) return VMWM_ERR_LIMIT;

    eh.frame_id = g_frame_id;
    eh.len      = len;

    vmwm_buf_init(&body);
    if (!vmwm_buf_append(&body, &eh, sizeof(eh)) ||
        (len && !vmwm_buf_append(&body, json, len))) {
        vmwm_buf_free(&body);
        return VMWM_ERR_NOMEM;
    }

    r = (VmWmResult)(conn_send(c, VMWM_CMD_TYPE_EVT,
                               (uint16_t)(full ? EVT_FULLFRAME : EVT_FRAME),
                               0, body.data, (uint32_t)body.len) == VMWM_OK
                      ? VMWM_OK : VMWM_ERR_FAILED);
    vmwm_buf_free(&body);
    return r;
}

VmWmResult VmWm_ServerBroadcastFrame(const char* json, uint32_t len, int full)
{
    Conn*      c;
    VmWmResult last = VMWM_ERR_NOT_CONNECTED;
    uint32_t   bit  = (uint32_t)(full ? EVT_FULLFRAME : EVT_FRAME);

    for (c = g_conns; c; c = c->next) {
        if (c->closing) continue;
        if (!(conn_caps(c) & VMWM_CAP_FRAME)) continue;
        if ((c->mask & bit) == 0) continue;
        last = send_frame_to(c, json, len, full);
    }
    return last;
}

VmWmResult VmWm_ServerSendFrame(uint32_t client_id, const char* json,
                                uint32_t len, int full)
{
    Conn* c = conn_by_id(client_id);
    if (!c) return VMWM_ERR_NOT_CONNECTED;
    return send_frame_to(c, json, len, full);
}

VmWmResult VmWm_ServerSendRespack(uint32_t client_id, const char* id,
                                  const char* json, uint32_t len)
{
    VmWmBuf        body;
    VmWmEvtRespack eh;
    Conn*          c = conn_by_id(client_id);
    VmWmResult     r;

    if (!c || c->closing) return VMWM_ERR_NOT_CONNECTED;
    if (len && !json) return VMWM_ERR_BAD_ARG;
    if (len > VMWM_MAX_PAYLOAD - sizeof(eh)) return VMWM_ERR_LIMIT;

    memset(&eh, 0, sizeof(eh));
    copy_str(eh.id, sizeof(eh.id), id);
    eh.len = len;

    vmwm_buf_init(&body);
    if (!vmwm_buf_append(&body, &eh, sizeof(eh)) ||
        (len && !vmwm_buf_append(&body, json, len))) {
        vmwm_buf_free(&body);
        return VMWM_ERR_NOMEM;
    }

    r = (VmWmResult)(conn_send(c, VMWM_CMD_TYPE_EVT, (uint16_t)EVT_RESOURCES, 0,
                               body.data, (uint32_t)body.len) == VMWM_OK
                      ? VMWM_OK : VMWM_ERR_FAILED);
    vmwm_buf_free(&body);
    return r;
}

/* ================= 宿主 ================= */

void VmWm_ServerSetHost(const VmWmHost* host)
{
    if (host) {
        g_host     = *host;
        g_host_set = 1;
        vmwm_log("host registered (on_tick=%s on_content=%s)",
                 host->on_tick ? "yes" : "no",
                 host->on_content ? "yes" : "no");
    } else {
        memset(&g_host, 0, sizeof(g_host));
        g_host_set = 0;
        vmwm_log("host removed");
    }
}

const VmWmHost* VmWm_ServerHost(void)
{
    return g_host_set ? &g_host : NULL;
}

uint32_t VmWm_ServerClientCount(void)
{
    return g_conn_count;
}

VmWmResult VmWm_ServerGetPeer(uint32_t client_id, VmWmPeer* out)
{
    Conn* c = conn_by_id(client_id);
    if (!c || !out) return VMWM_ERR_NOT_CONNECTED;
    *out = c->peer;
    return VMWM_OK;
}

/* ================= 服务生命周期 ================= */

VmWmResult VmWm_ServerInit(const char* pipe_path)
{
    int r;

    if (g_inited) return VMWM_OK;

    g_loop = uv_default_loop();

    /* 服务端管道必须 ipc=0：libuv 对 ipc 管道执行 listen 会返回 EINVAL */
    uv_pipe_init(g_loop, &g_server_pipe, 0);

    r = uv_pipe_bind(&g_server_pipe, pipe_path ? pipe_path : VMWM_PIPE_NAME);
    if (r != 0) {
        vmwm_log("bind '%s' failed: %s", pipe_path ? pipe_path : VMWM_PIPE_NAME,
                 uv_strerror(r));
        uv_close((uv_handle_t*)&g_server_pipe, NULL);
        uv_run(g_loop, UV_RUN_NOWAIT);
        return VMWM_ERR_FAILED;
    }

    r = uv_listen((uv_stream_t*)&g_server_pipe, VMWM_MAX_CLIENTS,
                  on_new_connection);
    if (r != 0) {
        vmwm_log("listen failed: %s", uv_strerror(r));
        uv_close((uv_handle_t*)&g_server_pipe, NULL);
        uv_run(g_loop, UV_RUN_NOWAIT);
        return VMWM_ERR_FAILED;
    }

    uv_timer_init(g_loop, &g_tick_timer);
    uv_timer_start(&g_tick_timer, on_tick, 16, 16);

    uv_timer_init(g_loop, &g_peer_timer);
    uv_timer_start(&g_peer_timer, on_peer_timer, 1000, 1000);

    g_inited = 1;
    vmwm_log("wm-server listening on %s (protocol v%u, payload<=%u)",
             pipe_path ? pipe_path : VMWM_PIPE_NAME, (unsigned)VMWM_VERSION,
             (unsigned)VMWM_MAX_PAYLOAD);
    return VMWM_OK;
}

int VmWm_ServerRun(void)
{
    if (!g_inited || !g_loop) return -1;
    uv_run(g_loop, UV_RUN_DEFAULT);
    return 0;
}

void VmWm_ServerStop(void)
{
    if (!g_inited || !g_loop) return;
    uv_stop(g_loop);
}
