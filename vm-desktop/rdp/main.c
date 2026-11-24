/*
 * main.c - vm_rdp 入口（呈现端服务器）
 *
 * 职责（docs/task-vm-wms-merge.md §3.1 / M4）：
 *   浏览器 <-> 本进程：HTTP / SSE / WebSocket / 静态资源 / respack
 *   本进程 <-> vm_wms：VMWM_CLIENT_RDP，订阅 EVT_FRAME|EVT_FULLFRAME|EVT_RESOURCES
 *
 * 不持窗口表、不持内容、不做合成、不切瓦片。
 *
 * 用法：
 *   vm_rdp                                 默认管道名、端口 9000、assets 为 web root
 *   vm_rdp --port 9050 --pipe \\.\pipe\vmwm_dev --web-root ../assets --font ../assets/font.ttf
 *
 * 事件循环：HTTP 侧跑自己的 uv loop；WMS 客户端 SDK 每个实例也带一个 loop，
 * 用 Wms_PollEvents(c, 0) 在主循环里轮询（非阻塞，分发到 on_wms_event）。
 * 两个 loop 在同一线程里交替跑，互不阻塞；16 ms timer 保证没有 HTTP 事件时
 * 主循环也会醒来取 WMS 事件。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uv.h"

#include "wms_client.h"
#include "wms_ipc.h"

#include "rdp_server.h"

#define RDP_LOG(...)                                     \
    do {                                                 \
        fprintf(stderr, "[rdp] " __VA_ARGS__);           \
        fputc('\n', stderr);                             \
        fflush(stderr);                                  \
    } while (0)

static uv_loop_t   g_loop;
static RdpServer  *g_srv;
static WmsClient  *g_wms;
static const char *g_pipe = VMWM_PIPE_NAME;
static int         g_stop;
static int         g_needRespack;
static uint64_t    g_nextRetryMs;

/* ---------------- 输入体解析 ----------------
   前端 body 是一个扁平 JSON 对象（vm_renderer.js 的 postInput）。
   不做通用 JSON：只取固定几个字段，与 vm_wm.c 的 extract_* 同一口径。 */

static const char *find_key(const char *json, const char *key) {
    static char scratch[64];
    snprintf(scratch, sizeof(scratch), "\"%s\"", key);
    return strstr(json, scratch);
}

static int extract_int(const char *json, const char *key, int fallback) {
    const char *p = find_key(json, key);
    if (!p) return fallback;
    p = strchr(p, ':');
    if (!p) return fallback;
    return atoi(p + 1);
}

static void extract_str(const char *json, const char *key, char *out, size_t cap) {
    const char *p;
    size_t n = 0;
    out[0] = '\0';
    p = find_key(json, key);
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    p = strchr(p, '"');
    if (!p) return;
    p++;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = '\0';
}

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || !cap) return;
    if (src) {
        for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* 前端原始输入 -> 线路协议 VmWmReqInput。
 * kind: 0 move / 1 down / 2 up / 3 wheel / 4 key（见 wms_ipc.h）。
 * win_id = 0：交给 vm_wms 按画布坐标命中窗口，再转成 body 局部坐标发给 APP。 */
static void on_front_input(void *user, const char *body, size_t len) {
    VmWmReqInput in;
    char kind[24] = {0};
    char key[32] = {0};
    char code[32] = {0};
    int ctrl, shift, alt;

    (void)user;
    (void)len;

    memset(&in, 0, sizeof(in));
    extract_str(body, "kind", kind, sizeof(kind));
    extract_str(body, "key", key, sizeof(key));
    extract_str(body, "code", code, sizeof(code));

    in.win_id = 0;
    in.x = extract_int(body, "x", -1);
    in.y = extract_int(body, "y", -1);
    in.button = extract_int(body, "button", 0);
    in.delta_y = extract_int(body, "deltaY", 0);

    if (strcmp(kind, "mousemove") == 0) {
        in.kind = 0;
    } else if (strcmp(kind, "mousedown") == 0) {
        in.kind = 1;
        in.is_down = 1;
    } else if (strcmp(kind, "mouseup") == 0) {
        in.kind = 2;
    } else if (strcmp(kind, "wheel") == 0) {
        in.kind = 3;
    } else if (strcmp(kind, "keydown") == 0) {
        in.kind = 4;
        in.is_down = 1;
    } else if (strcmp(kind, "keyup") == 0) {
        in.kind = 4;
    } else {
        return;   /* click / 未知类型：前端不发，忽略 */
    }

    ctrl = extract_int(body, "ctrl", 0);
    shift = extract_int(body, "shift", 0);
    alt = extract_int(body, "alt", 0);
    in.mods = (uint32_t)((ctrl ? 1u : 0u) | (shift ? 2u : 0u) | (alt ? 4u : 0u));

    copy_str(in.key, sizeof(in.key), key);
    copy_str(in.code, sizeof(in.code), code);

    if (!g_wms || !Wms_ClientIsConnected(g_wms)) return;
    Wms_SendInput(g_wms, &in);

    /* 每个输入一行：e2e 用它断言「WS/HTTP -> wms」这条链路真的通。 */
    RDP_LOG("input kind=%s(%u) x=%d y=%d down=%d key='%s'",
            kind, (unsigned)in.kind, in.x, in.y, in.is_down, in.key);
}

/* ---------------- WMS 事件 ---------------- */

static void on_wms_event(WmsClient *c, const WmsEvent *e, void *user) {
    RdpServer *srv = (RdpServer *)user;
    (void)c;

    switch (e->evt) {
    case EVT_FRAME: {
        uint32_t len = 0, fid = 0;
        const char *json = Wms_AsFrame(e, &len, &fid);
        if (json && len) RdpServer_PushFrame(srv, json, len);
        break;
    }
    case EVT_FULLFRAME: {
        uint32_t len = 0, fid = 0;
        const char *json = Wms_AsFrame(e, &len, &fid);
        if (json && len) {
            RDP_LOG("fullframe frame_id=%u len=%u", (unsigned)fid, (unsigned)len);
            RdpServer_PushFullframe(srv, json, len);
        }
        break;
    }
    case EVT_RESOURCES: {
        char id[64];
        uint32_t len = 0;
        const char *json = Wms_AsRespack(e, id, sizeof(id), &len);
        if (json && len) RdpServer_SetRespack(srv, id, json, len);
        break;
    }
    case EVT_THEME_CHANGED: {
        const VmWmReqTheme *t = Wms_AsTheme(e);
        RDP_LOG("theme changed -> '%s', re-request respack", t ? t->name : "");
        g_needRespack = 1;
        break;
    }
    default:
        /* RDP 角色收不到输入类事件，静默 */
        break;
    }
}

/* ---------------- 连接管理 ---------------- */

static void wms_disconnect(void) {
    if (g_wms) {
        Wms_ClientDestroy(g_wms);
        g_wms = NULL;
    }
}

static void wms_try_connect(uint64_t now) {
    VmWmResult r;

    if (!g_wms && now < g_nextRetryMs) return;

    g_wms = Wms_ClientCreate(g_pipe, VMWM_CLIENT_RDP);
    if (!g_wms) return;

    Wms_SetEventCallback(g_wms, on_wms_event, g_srv);

    r = Wms_ClientConnect(g_wms);
    if (r != VMWM_OK) {
        RDP_LOG("wms connect failed (%s), retry in 1s", Wms_StrError(r));
        Wms_ClientDestroy(g_wms);
        g_wms = NULL;
        g_nextRetryMs = now + 1000;
        return;
    }

    r = Wms_Subscribe(g_wms, EVT_FRAME | EVT_FULLFRAME | EVT_RESOURCES |
                                 EVT_THEME_CHANGED);
    if (r != VMWM_OK) {
        RDP_LOG("wms subscribe failed (%s)", Wms_StrError(r));
        wms_disconnect();
        g_nextRetryMs = now + 1000;
        return;
    }

    RDP_LOG("wms connected pipe='%s' client_id=%u screen=%dx%d",
            g_pipe, (unsigned)Wms_ClientId(g_wms),
            (int)Wms_ScreenWidth(g_wms), (int)Wms_ScreenHeight(g_wms));
    g_needRespack = 1;
}

/* ---------------- 主循环 ---------------- */

static void on_signal(uv_signal_t *handle, int signum) {
    (void)handle;
    RDP_LOG("signal %d, shutting down", signum);
    g_stop = 1;
}

static void rdp_pump(void) {
    uint64_t now = (uint64_t)(uv_hrtime() / 1000000ull);

    if (g_wms && !Wms_ClientIsConnected(g_wms)) {
        RDP_LOG("wms disconnected, retry in 1s");
        wms_disconnect();
        g_nextRetryMs = now + 1000;
    }
    if (!g_wms) {
        wms_try_connect(now);
        return;
    }

    if (g_needRespack) {
        g_needRespack = 0;
        if (Wms_RequestRespack(g_wms, "default") != VMWM_OK)
            g_needRespack = 1;   /* 下一轮再试 */
    }

    /* 有 SSE 客户端在等基线：向上游要一次完整帧。
       阻塞点（等 ACK）在主循环里，不在 HTTP 回调里，不会打断正在处理的请求。 */
    if (RdpServer_BeginFullframe(g_srv)) {
        VmWmResult r = Wms_RequestFullframe(g_wms);
        if (r != VMWM_OK) {
            RDP_LOG("request fullframe failed (%s)", Wms_StrError(r));
            RdpServer_CancelFullframe(g_srv);
        }
    }
}

int main(int argc, char **argv) {
    unsigned short port = 9000;
    const char *webRoot = "assets";
    const char *fontPath = NULL;
    uv_signal_t sigInt, sigTerm;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (unsigned short)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pipe") == 0 && i + 1 < argc) {
            g_pipe = argv[++i];
        } else if (strcmp(argv[i], "--web-root") == 0 && i + 1 < argc) {
            webRoot = argv[++i];
        } else if (strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
            fontPath = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: vm_rdp [--port N] [--pipe <path>] [--web-root <dir>] [--font <ttf>]\n");
            printf("  default pipe : %s\n", VMWM_PIPE_NAME);
            printf("  protocol     : wms v%u (role RDP)\n", (unsigned)VMWM_VERSION);
            return 0;
        } else {
            printf("unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    printf("vm-rdp starting, port = %u, web root = %s\n", port, webRoot);
    fflush(stdout);

    if (uv_loop_init(&g_loop) != 0) {
        printf("uv_loop_init failed\n");
        return 1;
    }

    g_srv = RdpServer_Create(&g_loop, port, webRoot, fontPath);
    if (!g_srv) {
        printf("rdp server create failed\n");
        uv_loop_close(&g_loop);
        return 1;
    }
    RdpServer_SetInputHandler(g_srv, on_front_input, NULL);

    uv_signal_init(&g_loop, &sigInt);
    uv_signal_start(&sigInt, on_signal, SIGINT);
    uv_signal_init(&g_loop, &sigTerm);
#ifdef SIGTERM
    uv_signal_start(&sigTerm, on_signal, SIGTERM);
#endif

    while (!g_stop) {
        /* 没有 IO 时由 rdp_server 的 16 ms timer 唤醒。 */
        uv_run(&g_loop, UV_RUN_ONCE);
        if (g_wms) Wms_PollEvents(g_wms, 0);
        rdp_pump();
        if (RdpServer_Stopped(g_srv)) break;
    }

    wms_disconnect();
    RdpServer_Free(g_srv);
    g_srv = NULL;
    uv_run(&g_loop, UV_RUN_DEFAULT);
    uv_loop_close(&g_loop);

    RDP_LOG("exited");
    return 0;
}
