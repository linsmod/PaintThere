/*
 * test_shell.c - 扮演桌面外壳（VMWM_CLIENT_SHELL）
 *
 * 外壳是唯一有权声明工作区 / 切主题 / 管理别人窗口的角色，所以 e2e 里
 * 「窗口列表 + 工作区 + 主题 + 把输入打到别人窗口上」都由它来触发。
 *
 * 用法：
 *   test_shell --pipe <path> [--seconds 5] [--theme dark] [--click 200,150] [--focus]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uv.h"
#include "wms_client.h"

static int      g_events  = 0;
static uint64_t g_taskbar = 0;

static void on_event(WmsClient* c, const WmsEvent* e, void* user)
{
    (void)c;
    (void)user;
    g_events++;

    switch (e->evt) {
    case EVT_WINDOW_NEW:
    case EVT_WINDOW_CLOSE:
    case EVT_WINDOW_CONFIGURE:
    case EVT_FOCUS_CHANGE: {
        const VmWmWindowInfo* wi = Wms_AsWindowInfo(e);
        if (!wi) break;
        printf("[shell] evt=0x%x id=%llu (%d,%d) %dx%d z=%u focused=%d '%s'\n",
               (unsigned)e->evt, (unsigned long long)wi->win_id,
               wi->x, wi->y, wi->w, wi->h, (unsigned)wi->z, wi->focused,
               wi->title);
        fflush(stdout);
        break;
    }
    case EVT_TITLE_CHANGE: {
        const VmWmWindowInfo* wi = Wms_AsWindowInfo(e);
        if (wi)
            printf("[shell] title -> %llu '%s'\n",
                   (unsigned long long)wi->win_id, wi->title);
        fflush(stdout);
        break;
    }
    case EVT_LAYOUT_REQ: {
        const VmWmEvtLayoutReq* lr = Wms_AsLayoutReq(e);
        if (lr)
            printf("[shell] layout_req id=%llu epoch=%u body=%dx%d\n",
                   (unsigned long long)lr->win_id, (unsigned)lr->epoch,
                   lr->width, lr->height);
        fflush(stdout);
        break;
    }
    case EVT_INPUT: {
        const VmWmReqInput* in = Wms_AsInput(e);
        if (in)
            printf("[shell] input id=%llu kind=%u down=%u (%d,%d)\n",
                   (unsigned long long)in->win_id, (unsigned)in->kind,
                   (unsigned)in->is_down, in->x, in->y);
        fflush(stdout);
        break;
    }
    default:
        printf("[shell] evt 0x%x len=%u\n", (unsigned)e->evt, (unsigned)e->len);
        fflush(stdout);
        break;
    }
}

int main(int argc, char** argv)
{
    const char* pipe_path = NULL;
    const char* theme     = NULL;
    int         seconds   = 5;
    int         do_focus  = 0;
    int         click_x   = -1, click_y = -1;
    WmsClient*  c;
    VmWmResult  r;
    int32_t     sw = 0, sh = 0;
    uint64_t    t0;
    int         i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pipe") == 0 && i + 1 < argc) {
            pipe_path = argv[++i];
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            theme = argv[++i];
        } else if (strcmp(argv[i], "--click") == 0 && i + 1 < argc) {
            sscanf(argv[++i], "%d,%d", &click_x, &click_y);
        } else if (strcmp(argv[i], "--focus") == 0) {
            do_focus = 1;
        }
    }

    c = Wms_ClientCreate(pipe_path, VMWM_CLIENT_SHELL);
    if (!c) { printf("[shell] create failed\n"); return 1; }

    r = Wms_ClientConnect(c);
    printf("[shell] connect -> %s client_id=%u\n", Wms_StrError(r),
           (unsigned)Wms_ClientId(c));
    if (r != VMWM_OK) { Wms_ClientDestroy(c); return 1; }

    Wms_SetEventCallback(c, on_event, NULL);
    Wms_Subscribe(c, EVT_ALL);

    sw = Wms_ScreenWidth(c);
    sh = Wms_ScreenHeight(c);

    /* 工作区 = 屏幕减去底部 32px 任务栏 */
    r = Wms_SetWorkArea(c, 0, 0, sw, sh - 32);
    printf("[shell] set_workarea 0,0 %dx%d -> %s\n", (int)sw, (int)(sh - 32),
           Wms_StrError(r));

    /* 外壳自己也有窗口：任务栏以窗口形式参与堆叠 */
    r = Wms_CreateWindow(c, 0, sh - 32, sw, 32, &g_taskbar);
    printf("[shell] create taskbar -> %s id=%llu\n", Wms_StrError(r),
           (unsigned long long)g_taskbar);
    if (r == VMWM_OK) {
        Wms_SetTitle(c, g_taskbar, "Taskbar");
        Wms_SetClassHint(c, g_taskbar, "VmShell");
        Wms_MapWindow(c, g_taskbar);
    }

    if (theme) {
        r = Wms_SetTheme(c, theme);
        printf("[shell] set_theme '%s' -> %s\n", theme, Wms_StrError(r));
    }

    if (do_focus) {
        VmWmWindowInfo list[VMWM_MAX_WINDOWS];
        int n = Wms_ListWindows(c, list, VMWM_MAX_WINDOWS);
        printf("[shell] list_windows -> %d\n", n);
        for (i = 0; i < n; i++)
            printf("[shell]   #%d id=%llu '%s' z=%u\n", i,
                   (unsigned long long)list[i].win_id, list[i].title,
                   (unsigned)list[i].z);
        if (n > 0) {
            r = Wms_SetFocus(c, list[0].win_id);
            printf("[shell] set_focus %llu -> %s\n",
                   (unsigned long long)list[0].win_id, Wms_StrError(r));
        }
    }

    if (click_x >= 0) {
        VmWmReqInput in;
        memset(&in, 0, sizeof(in));
        in.win_id  = 0;                    /* 0 = 让服务端按 z 序命中 */
        in.kind    = 1;                    /* down */
        in.is_down = 1;
        in.x       = click_x;
        in.y       = click_y;
        in.button  = 0;
        r = Wms_SendInput(c, &in);
        printf("[shell] input click (%d,%d) -> %s\n", click_x, click_y,
               Wms_StrError(r));
    }

    t0 = uv_hrtime();
    while (Wms_ClientIsConnected(c)) {
        double elapsed = (double)(uv_hrtime() - t0) / 1e9;
        if (elapsed >= (double)seconds) break;
        Wms_PollEvents(c, 50);
    }

    printf("[shell] events=%d, done\n", g_events);
    Wms_ClientDestroy(c);
    return 0;
}
