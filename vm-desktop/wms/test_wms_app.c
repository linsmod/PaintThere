/*
 * test_app.c - 扮演一个普通应用（VMWM_CLIENT_APP）
 *
 * 做的事：建窗口 → 设标题/类名 → 映射 → 回推内容 → 处理布局/输入/主题事件。
 * 用途是 e2e：它是唯一会「产出窗口内容」的角色，能证明
 *   内容通道 → 宿主 → 帧 → RDP 这条链路真的通。
 *
 * 用法：
 *   test_app --pipe <path> [--width 320] [--height 200] [--seconds 8] [--no-content]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uv.h"
#include "wms_client.h"

#include "vm_display.h"   /* 真实 VmDrawCommand：内容要被宿主按同一结构解释 */

#define APP_CMD_MAX  16

static uint64_t g_win_id  = 0;
static uint32_t g_epoch   = 1;
static int      g_content = 1;
static int      g_anim    = 1;
static int      g_events  = 0;

/* 造 count 条真实命令（body 局部坐标）：count-1 个矩形 + 1 行文字。
   文字里带 seed，每次回推内容不同，宿主才能产出增量帧。 */
static void build_cmds(VmDrawCommand* out, uint32_t count, uint32_t seed)
{
    static const char* colors[3] = {
        "rgba(64,120,220,1)", "rgba(220,96,64,1)", "rgba(72,180,120,1)"
    };
    uint32_t i;
    for (i = 0; i < count; i++) {
        VmDrawCommand* c = &out[i];
        memset(c, 0, sizeof(*c));
        if (i + 1 == count) {
            VmRect r = {8.0f, 8.0f, 260.0f, 22.0f};
            c->type = VM_CMD_DRAW_TEXT;
            snprintf(c->id, sizeof(c->id), "app_text");
            c->rect = r;
            c->args.drawText.rect = r;
            snprintf(c->args.drawText.text, sizeof(c->args.drawText.text),
                     "app seed=%u n=%u", (unsigned)seed, (unsigned)count);
            c->args.drawText.fontId = 0;
            c->args.drawText.fontSize = 14;
            snprintf(c->args.drawText.color, sizeof(c->args.drawText.color),
                     "%s", "rgba(255,255,255,1)");
        } else {
            VmRect r = {(float)(8 + (int)i * 72), 40.0f, 62.0f, 42.0f};
            c->type = VM_CMD_DRAW_RECT;
            snprintf(c->id, sizeof(c->id), "app_rect_%u", (unsigned)i);
            c->rect = r;
            c->args.drawRect.rect = r;
            snprintf(c->args.drawRect.paint, sizeof(c->args.drawRect.paint),
                     "%s", colors[i % 3]);
            c->args.drawRect.cornerRadius = 4.0f;
        }
    }
}

static void push_content(WmsClient* c, uint32_t epoch, int w, int h)
{
    VmDrawCommand cmds[APP_CMD_MAX];
    uint32_t      n;
    int           accepted = 0, overflow = 0;
    VmWmResult    r;

    if (!g_content || !g_win_id) return;

    n = 4;
    build_cmds(cmds, n, epoch + (uint32_t)w);
    r = Wms_PushContent(c, g_win_id, epoch, cmds, n,
                        (uint32_t)sizeof(VmDrawCommand), &accepted, &overflow);
    printf("[app] push content win=%llu epoch=%u n=%u body=%dx%d -> %s accepted=%d overflow=%d\n",
           (unsigned long long)g_win_id, (unsigned)epoch, (unsigned)n,
           w, h, Wms_StrError(r), accepted, overflow);
    fflush(stdout);
}

static void on_event(WmsClient* c, const WmsEvent* e, void* user)
{
    (void)user;
    g_events++;

    switch (e->evt) {
    case EVT_LAYOUT_REQ: {
        const VmWmEvtLayoutReq* lr = Wms_AsLayoutReq(e);
        if (!lr) break;
        printf("[app] layout_req win=%llu epoch=%u body=%dx%d\n",
               (unsigned long long)lr->win_id, (unsigned)lr->epoch,
               lr->width, lr->height);
        fflush(stdout);
        g_epoch = lr->epoch;
        push_content(c, lr->epoch, lr->width, lr->height);
        break;
    }
    case EVT_INPUT: {
        const VmWmReqInput* in = Wms_AsInput(e);
        if (!in) break;
        printf("[app] input win=%llu kind=%u down=%u at (%d,%d)\n",
               (unsigned long long)in->win_id, (unsigned)in->kind,
               (unsigned)in->is_down, in->x, in->y);
        fflush(stdout);
        break;
    }
    case EVT_THEME_CHANGED: {
        const VmWmReqTheme* t = Wms_AsTheme(e);
        printf("[app] theme '%s' (各自重新解析)\n", t ? t->name : "");
        fflush(stdout);
        break;
    }
    case EVT_FOCUS_CHANGE: {
        const VmWmWindowInfo* wi = Wms_AsWindowInfo(e);
        printf("[app] focus -> %llu\n",
               wi ? (unsigned long long)wi->win_id : 0ull);
        fflush(stdout);
        break;
    }
    case EVT_WINDOW_CONFIGURE: {
        const VmWmWindowInfo* wi = Wms_AsWindowInfo(e);
        if (wi)
            printf("[app] configure win=%llu (%d,%d) %dx%d z=%u\n",
                   (unsigned long long)wi->win_id, wi->x, wi->y,
                   wi->w, wi->h, (unsigned)wi->z);
        fflush(stdout);
        break;
    }
    default:
        printf("[app] evt 0x%x len=%u\n", (unsigned)e->evt, (unsigned)e->len);
        fflush(stdout);
        break;
    }
}

int main(int argc, char** argv)
{
    const char* pipe_path = NULL;
    int         width = 320, height = 200;
    int         seconds = 8;
    WmsClient*  c;
    VmWmResult  r;
    uint64_t    t0;
    int         i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pipe") == 0 && i + 1 < argc) {
            pipe_path = argv[++i];
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-content") == 0) {
            g_content = 0;
        } else if (strcmp(argv[i], "--no-anim") == 0) {
            g_anim = 0;
        }
    }

    c = Wms_ClientCreate(pipe_path, VMWM_CLIENT_APP);
    if (!c) { printf("[app] create failed\n"); return 1; }

    r = Wms_ClientConnect(c);
    printf("[app] connect -> %s client_id=%u screen=%dx%d\n",
           Wms_StrError(r), (unsigned)Wms_ClientId(c),
           (int)Wms_ScreenWidth(c), (int)Wms_ScreenHeight(c));
    if (r != VMWM_OK) { Wms_ClientDestroy(c); return 1; }

    Wms_SetEventCallback(c, on_event, NULL);

    {
        int32_t wx = 0, wy = 0, ww = 0, wh = 0;
        Wms_WorkArea(c, &wx, &wy, &ww, &wh);
        printf("[app] workarea = (%d,%d) %dx%d layout_timeout=%u frames\n",
               wx, wy, ww, wh, (unsigned)Wms_LayoutTimeoutFrames(c));
    }

    Wms_Subscribe(c, EVT_LAYOUT_REQ | EVT_INPUT | EVT_THEME_CHANGED |
                     EVT_FOCUS_CHANGE | EVT_WINDOW_CONFIGURE);

    r = Wms_CreateWindow(c, 100, 80, width, height, 0, &g_win_id);
    printf("[app] create_window -> %s id=%llu\n", Wms_StrError(r),
           (unsigned long long)g_win_id);
    if (r != VMWM_OK) { Wms_ClientDestroy(c); return 1; }

    Wms_SetTitle(c, g_win_id, "wmwm test window");
    Wms_SetClassHint(c, g_win_id, "TestApp");
    Wms_SetSizeHint(c, g_win_id, 120, 80, 0, 0);
    Wms_MapWindow(c, g_win_id);
    Wms_SetFocus(c, g_win_id);
    push_content(c, g_epoch, width, height);

    {
        /* APP 角色没有「全量枚举」权限（那是外壳/呈现端的事），
         * 它只该查自己窗口的几何——这里顺带验证 GET_GEOMETRY 路径。 */
        int gx = 0, gy = 0, gw = 0, gh = 0;
        r = Wms_GetGeometry(c, g_win_id, &gx, &gy, &gw, &gh);
        printf("[app] get_geometry -> %s (%d,%d) %dx%d\n", Wms_StrError(r),
               gx, gy, gw, gh);
        printf("[app] list_windows -> %d (APP 无 ENUM 权限，期望 -1)\n",
               Wms_ListWindows(c, NULL, 0));
        fflush(stdout);
    }

    t0 = uv_hrtime();
    {
        uint64_t last_push = t0;
        while (Wms_ClientIsConnected(c)) {
            uint64_t now     = uv_hrtime();
            double   elapsed = (double)(now - t0) / 1e9;
            if (elapsed >= (double)seconds) break;

            /* 每 ~400ms 回推一次内容：模拟动画/重绘。
             * e2e 里这是唯一能持续制造脏帧的来源，rdp 侧才能收到增量帧。 */
            if (g_anim && (double)(now - last_push) / 1e9 >= 0.4) {
                last_push = now;
                push_content(c, g_epoch, width, height);
            }
            Wms_PollEvents(c, 30);
        }
    }

    printf("[app] events=%d, closing\n", g_events);
    Wms_CloseWindow(c, g_win_id);
    Wms_PollEvents(c, 20);
    Wms_ClientDestroy(c);

    printf("[app] done\n");
    return 0;
}
