/*
 * main.c - vm_app：新架构应用（M6 / A1）
 *
 * 只做三件事：向 vm_wms 申请一个窗口、按 EVT_LAYOUT_REQ 回 body 局部命令、
 * 把 EVT_INPUT 分发给本地控件状态。不建 VmWm、不起 HTTP、不合成、不切瓦片。
 *
 * 用法：
 *   vm_app --pipe \\.\pipe\vmwm_dev --title A --font assets/font.ttf
 *          [--slow-layout 300]   故意延迟回内容（A4 用）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uv.h"

#include "wms_client.h"
#include "wms_ipc.h"

#include "ui_hello.h"
#include "ui_xaml.h"
#include "vm_display.h"
#include "vm_font.h"

#define APP_LOG(...)                                     \
    do {                                                 \
        fprintf(stderr, "[app] " __VA_ARGS__);           \
        fputc('\n', stderr);                             \
        fflush(stderr);                                  \
    } while (0)

static uv_loop_t   g_loop;
static uv_timer_t  g_timer;
static uv_timer_t  g_caretTimer;
static WmsClient  *g_wms;
static const char *g_pipe = VMWM_PIPE_NAME;
static const char *g_title = "vm_app";
static int         g_stop;

static uint64_t     g_win;
static uint32_t     g_epoch;
static int          g_dirty;
static int          g_slowLayoutMs;
static int          g_x = 60, g_y = 60;
static int          g_uiXaml;
static const char  *g_xamlPath;
static const char  *g_themeName;
static const char  *g_checkTheme;
static const char  *g_themesRoot = "assets/themes";
static int          g_xamlCaretOn = 1;
static int          g_resizeAfterMs, g_resizeW, g_resizeH;
static int          g_resizeDone;
static uint64_t     g_t0Ms;
static HelloState   g_state;

/* 内容缓冲：与 vm_wms 的 cmd_size 校验一致，不能放栈。 */
static VmDrawCommand g_cmds[VM_DISPLAY_MAX_COMMANDS];

/* ---------------- 内容 ---------------- */

static void push_content(void) {
    VmCmdBuffer buf;
    int accepted = 0, overflow = 0;
    VmWmResult r;

    if (!g_wms || !g_win || g_epoch == 0) return;   /* 等首个 layout_req */
    if (g_slowLayoutMs > 0) uv_sleep((unsigned)g_slowLayoutMs);

    buf.commands = g_cmds;
    buf.cap = (int)(sizeof(g_cmds) / sizeof(g_cmds[0]));
    buf.count = 0;
    buf.rejected = 0;
    if (g_uiXaml)
        XamlUi_Paint(&buf, g_state.bodyW, g_state.bodyH);
    else
        Hello_Paint(&g_state, &buf);

    r = Wms_PushContent(g_wms, g_win, g_epoch, g_cmds, (uint32_t)buf.count,
                        (uint32_t)sizeof(VmDrawCommand), &accepted, &overflow);
    APP_LOG("push epoch=%u count=%d accepted=%d overflow=%d -> %s",
            (unsigned)g_epoch, buf.count, accepted, overflow, Wms_StrError(r));
    if (r == VMWM_OK && accepted) g_dirty = 0;
}

/* ---------------- 输入 ---------------- */

static void append_text(const char *k) {
    size_t n = k ? strlen(k) : 0;
    if (!n) return;
    if (g_state.textLen + n + 1 >= sizeof(g_state.text)) return;
    memcpy(g_state.text + g_state.textLen, k, n);
    g_state.textLen += n;
    g_state.text[g_state.textLen] = '\0';
}

static void backspace_text(void) {
    if (g_state.textLen == 0) return;
    size_t n = g_state.textLen - 1;
    while (n > 0 && ((unsigned char)g_state.text[n] & 0xC0) == 0x80) n--;
    g_state.textLen = n;
    g_state.text[n] = '\0';
}

static int in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void on_input(const VmWmReqInput *in) {
    if (!in) return;
    APP_LOG("input kind=%u x=%d y=%d down=%d key='%s'",
            (unsigned)in->kind, in->x, in->y, in->is_down, in->key);

    if (g_uiXaml) {
        int dirty = 0;
        if (in->kind == 1) dirty = XamlUi_Pointer(in->x, in->y, 1);
        else if (in->kind == 2) dirty = XamlUi_Pointer(in->x, in->y, 0);
        else if (in->kind == 4 && in->is_down) dirty = XamlUi_Key(in->key, 0, 0);
        if (dirty) g_dirty = 1;
        return;
    }

    if (in->kind == 1 && in->is_down) {           /* mousedown */
        int x, y, w, h;
        Hello_ButtonRect(&g_state, &x, &y, &w, &h);
        if (in_rect(in->x, in->y, x, y, w, h)) {
            g_state.pressed = !g_state.pressed;
            g_state.clicks++;
            g_dirty = 1;
            APP_LOG("button -> pressed=%d clicks=%d", g_state.pressed, g_state.clicks);
            return;
        }
        Hello_InputRect(&g_state, &x, &y, &w, &h);
        g_state.focused = in_rect(in->x, in->y, x, y, w, h) ? 1 : 0;
        if (g_state.focused) g_state.caretOn = 1;
        g_dirty = 1;
        return;
    }

    if (in->kind == 4 && in->is_down && g_state.focused) {   /* keydown */
        if (strcmp(in->key, "Backspace") == 0) {
            backspace_text();
            g_state.caretOn = 1;
            g_dirty = 1;
        } else if (strcmp(in->key, "Enter") == 0 || strcmp(in->key, "Tab") == 0 ||
                   in->key[0] == '\0') {
            /* A1 单行输入：Enter/Tab 不处理 */
        } else {
            append_text(in->key);
            g_state.caretOn = 1;
            g_dirty = 1;
        }
    }
}

/* ---------------- WMS 事件 ---------------- */

static void on_wms_event(WmsClient *c, const WmsEvent *e, void *user) {
    (void)c;
    (void)user;

    switch (e->evt) {
    case EVT_LAYOUT_REQ: {
        const VmWmEvtLayoutReq *lr = Wms_AsLayoutReq(e);
        if (!lr) break;
        /* 服务端在建窗处理里就发了 layout_req，那时 create 还没返回（g_win=0）。
           本 app 只建一个窗口，第一条 layout_req 必然属于它。 */
        if (g_win == 0) g_win = lr->win_id;
        if (lr->win_id == g_win) {
            g_epoch = lr->epoch;
            g_state.bodyW = lr->width;
            g_state.bodyH = lr->height;
            g_dirty = 1;
            if (g_t0Ms == 0) g_t0Ms = (uint64_t)(uv_hrtime() / 1000000ull);
            APP_LOG("layout epoch=%u body=%dx%d",
                    (unsigned)lr->epoch, lr->width, lr->height);
        }
        break;
    }
    case EVT_INPUT:
        on_input(Wms_AsInput(e));
        break;
    case EVT_WINDOW_CLOSE: {
        const VmWmWindowInfo *info = Wms_AsWindowInfo(e);
        if (info && info->win_id == g_win) {
            APP_LOG("close -> exit");
            g_stop = 1;
        }
        break;
    }
    case EVT_THEME_CHANGED: {
        const VmWmReqTheme *t = Wms_AsTheme(e);
        APP_LOG("theme changed -> '%s'", t ? t->name : "");
        if (g_uiXaml && t && t->name[0]) {
            char dir[512], err[256];
            snprintf(dir, sizeof(dir), "%s/%s", g_themesRoot, t->name);
            if (!XamlUi_LoadTheme(dir, err, sizeof(err)))
                APP_LOG("theme load failed: %s", err);
            g_dirty = 1;
        }
        break;
    }
    default:
        break;
    }
}

/* ---------------- 连接与主循环 ---------------- */

static int connect_wms(void) {
    VmWmResult r;

    g_wms = Wms_ClientCreate(g_pipe, VMWM_CLIENT_APP);
    if (!g_wms) return 0;
    Wms_SetEventCallback(g_wms, on_wms_event, NULL);

    r = Wms_ClientConnect(g_wms);
    if (r != VMWM_OK) {
        APP_LOG("wms connect failed (%s)", Wms_StrError(r));
        Wms_ClientDestroy(g_wms);
        g_wms = NULL;
        return 0;
    }

    r = Wms_Subscribe(g_wms, EVT_LAYOUT_REQ | EVT_INPUT |
                             EVT_WINDOW_CLOSE | EVT_THEME_CHANGED);
    if (r != VMWM_OK) {
        APP_LOG("wms subscribe failed (%s)", Wms_StrError(r));
        Wms_ClientDestroy(g_wms);
        g_wms = NULL;
        return 0;
    }

    r = Wms_CreateWindow(g_wms, g_x, g_y, 420, 280, 0, &g_win);
    if (r != VMWM_OK) {
        APP_LOG("create window failed (%s)", Wms_StrError(r));
        return 0;
    }
    Wms_SetTitle(g_wms, g_win, g_title);
    Wms_MapWindow(g_wms, g_win);

    APP_LOG("connected pipe='%s' client_id=%u win=%llu",
            g_pipe, (unsigned)Wms_ClientId(g_wms), (unsigned long long)g_win);
    return 1;
}

static void on_timer(uv_timer_t *t) {
    (void)t;
    if (!g_wms) return;
    if (!Wms_ClientIsConnected(g_wms)) {
        APP_LOG("wms disconnected, exiting");
        g_stop = 1;
        return;
    }
    /* A4：到点后由 app 自己改一次窗口尺寸，触发 epoch+1 的布局请求。 */
    if (g_resizeAfterMs > 0 && !g_resizeDone && g_t0Ms > 0) {
        uint64_t now = (uint64_t)(uv_hrtime() / 1000000ull);
        if (now - g_t0Ms >= (uint64_t)g_resizeAfterMs) {
            g_resizeDone = 1;
            APP_LOG("resize -> %dx%d", g_resizeW, g_resizeH);
            Wms_ResizeWindow(g_wms, g_win, g_resizeW, g_resizeH);
        }
    }
    if (g_dirty) push_content();
}

static void on_caret(uv_timer_t *t) {
    (void)t;
    if (g_uiXaml) {
        g_xamlCaretOn = !g_xamlCaretOn;
        XamlUi_SetCaret(g_xamlCaretOn);
        g_dirty = 1;
        return;
    }
    if (!g_state.focused) return;
    g_state.caretOn = !g_state.caretOn;
    g_dirty = 1;
}

int main(int argc, char **argv) {
    const char *fontPath = "assets/font.ttf";
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pipe") == 0 && i + 1 < argc) {
            g_pipe = argv[++i];
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            g_title = argv[++i];
        } else if (strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
            fontPath = argv[++i];
        } else if (strcmp(argv[i], "--slow-layout") == 0 && i + 1 < argc) {
            g_slowLayoutMs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--x") == 0 && i + 1 < argc) {
            g_x = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--y") == 0 && i + 1 < argc) {
            g_y = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--resize-after") == 0 && i + 3 < argc) {
            g_resizeAfterMs = atoi(argv[++i]);
            g_resizeW = atoi(argv[++i]);
            g_resizeH = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ui") == 0 && i + 1 < argc) {
            g_uiXaml = strcmp(argv[++i], "xaml") == 0;
        } else if (strcmp(argv[i], "--xaml") == 0 && i + 1 < argc) {
            g_xamlPath = argv[++i];
            g_uiXaml = 1;
        } else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            g_themeName = argv[++i];
        } else if (strcmp(argv[i], "--themes-root") == 0 && i + 1 < argc) {
            g_themesRoot = argv[++i];
        } else if (strcmp(argv[i], "--check-theme") == 0 && i + 1 < argc) {
            g_checkTheme = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: vm_app [--pipe <path>] [--title <name>] [--font <ttf>]"
                   " [--x <n>] [--y <n>] [--slow-layout <ms>]"
                   " [--ui hello|xaml] [--xaml <file>]"
                   " [--theme <slug>] [--themes-root <dir>]\n");
            printf("  default pipe : %s\n", VMWM_PIPE_NAME);
            printf("  protocol     : wms v%u (role APP)\n", (unsigned)VMWM_VERSION);
            return 0;
        } else {
            printf("unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (VmFont_LoadFile(fontPath, 0) < 0)
        APP_LOG("font load failed: %s", fontPath);

    /* 主题自检：只加载表并退出，用于逐个验证 19 个预编译主题。 */
    if (g_checkTheme) {
        char err[256], dir[512];
        if (!XamlUi_Init(NULL, err, sizeof(err))) {
            APP_LOG("xaml init failed: %s", err);
            return 1;
        }
        snprintf(dir, sizeof(dir), "%s/%s", g_themesRoot, g_checkTheme);
        if (!XamlUi_LoadTheme(dir, err, sizeof(err))) {
            APP_LOG("theme check failed: %s", err);
            return 1;
        }
        return 0;
    }

    if (g_uiXaml) {
        char err[256];
        if (!XamlUi_Init(g_xamlPath, err, sizeof(err))) {
            APP_LOG("xaml init failed: %s", err);
            return 1;
        }
        if (g_themeName) {
            char dir[512];
            snprintf(dir, sizeof(dir), "%s/%s", g_themesRoot, g_themeName);
            if (!XamlUi_LoadTheme(dir, err, sizeof(err))) {
                APP_LOG("theme load failed: %s", err);
                return 1;
            }
        }
    }
    g_state.caretOn = 1;
    snprintf(g_state.text, sizeof(g_state.text), "%s", "");

    if (uv_loop_init(&g_loop) != 0) {
        printf("uv_loop_init failed\n");
        return 1;
    }
    if (!connect_wms()) {
        APP_LOG("startup failed");
        uv_loop_close(&g_loop);
        return 1;
    }

    uv_timer_init(&g_loop, &g_timer);
    uv_timer_start(&g_timer, on_timer, 16, 16);
    uv_timer_init(&g_loop, &g_caretTimer);
    uv_timer_start(&g_caretTimer, on_caret, 500, 500);

    while (!g_stop) {
        uv_run(&g_loop, UV_RUN_ONCE);
        if (g_wms) Wms_PollEvents(g_wms, 0);
    }

    uv_timer_stop(&g_timer);
    uv_timer_stop(&g_caretTimer);
    if (g_wms) Wms_ClientDestroy(g_wms);
    uv_close((uv_handle_t *)&g_timer, NULL);
    uv_close((uv_handle_t *)&g_caretTimer, NULL);
    uv_run(&g_loop, UV_RUN_DEFAULT);
    uv_loop_close(&g_loop);

    APP_LOG("exited");
    return 0;
}
