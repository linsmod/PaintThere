/*
 * test_rdp.c - 扮演呈现端（VMWM_CLIENT_RDP）
 *
 * 在 wmwms 这一层它不画像素，只验证三件事：
 *   1. 能订阅并收到 EVT_FRAME / EVT_FULLFRAME（帧字节由宿主生成）
 *   2. 能请求 fullframe / respack，且应答是异步事件而不是 ACK 载荷
 *   3. 能把画布坐标的输入打回去，命中后转成窗口局部坐标发给属主
 *
 * 用法：
 *   test_rdp --pipe <path> [--seconds 5] [--respack default] [--no-input]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uv.h"
#include "wms_client.h"

static int      g_frames     = 0;
static int      g_fullframes = 0;
static int      g_events     = 0;

static void on_event(WmsClient* c, const WmsEvent* e, void* user)
{
    (void)user;
    g_events++;

    switch (e->evt) {
    case EVT_FRAME:
    case EVT_FULLFRAME: {
        uint32_t len = 0, fid = 0;
        const char* json = Wms_AsFrame(e, &len, &fid);
        if (e->evt == EVT_FRAME) g_frames++; else g_fullframes++;
        printf("[rdp] %s frame_id=%u len=%u head=%.60s\n",
               e->evt == EVT_FRAME ? "frame" : "fullframe",
               (unsigned)fid, (unsigned)len, json ? json : "");
        fflush(stdout);
        break;
    }
    case EVT_RESOURCES: {
        char     id[64];
        uint32_t len = 0;
        const char* json = Wms_AsRespack(e, id, sizeof(id), &len);
        printf("[rdp] resources id='%s' len=%u head=%.60s\n", id,
               (unsigned)len, json ? json : "");
        fflush(stdout);
        break;
    }
    case EVT_WINDOW_NEW:
    case EVT_WINDOW_CONFIGURE:
    case EVT_WINDOW_CLOSE: {
        const VmWmWindowInfo* wi = Wms_AsWindowInfo(e);
        if (wi)
            printf("[rdp] evt=0x%x id=%llu (%d,%d) %dx%d\n",
                   (unsigned)e->evt, (unsigned long long)wi->win_id,
                   wi->x, wi->y, wi->w, wi->h);
        fflush(stdout);
        break;
    }
    case EVT_THEME_CHANGED: {
        const VmWmReqTheme* t = Wms_AsTheme(e);
        printf("[rdp] theme '%s'\n", t ? t->name : "");
        fflush(stdout);
        break;
    }
    default:
        /* EVT_INPUT 之类 RDP 收不到的事件，静默 */
        break;
    }
}

int main(int argc, char** argv)
{
    const char* pipe_path = NULL;
    const char* respack   = "default";
    int         seconds   = 5;
    int         send_input = 1;
    WmsClient*  c;
    VmWmResult  r;
    uint64_t    t0;
    int         i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pipe") == 0 && i + 1 < argc) {
            pipe_path = argv[++i];
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--respack") == 0 && i + 1 < argc) {
            respack = argv[++i];
        } else if (strcmp(argv[i], "--no-input") == 0) {
            send_input = 0;
        }
    }

    c = Wms_ClientCreate(pipe_path, VMWM_CLIENT_RDP);
    if (!c) { printf("[rdp] create failed\n"); return 1; }

    r = Wms_ClientConnect(c);
    printf("[rdp] connect -> %s client_id=%u screen=%dx%d\n", Wms_StrError(r),
           (unsigned)Wms_ClientId(c), (int)Wms_ScreenWidth(c),
           (int)Wms_ScreenHeight(c));
    if (r != VMWM_OK) { Wms_ClientDestroy(c); return 1; }

    Wms_SetEventCallback(c, on_event, NULL);
    Wms_Subscribe(c, EVT_FRAME | EVT_FULLFRAME | EVT_RESOURCES |
                     EVT_WINDOW_NEW | EVT_WINDOW_CLOSE |
                     EVT_WINDOW_CONFIGURE | EVT_THEME_CHANGED);

    r = Wms_RequestFullframe(c);
    printf("[rdp] request_fullframe -> %s (数据走 EVT_FULLFRAME)\n",
           Wms_StrError(r));

    r = Wms_RequestRespack(c, respack);
    printf("[rdp] request_respack '%s' -> %s (数据走 EVT_RESOURCES)\n",
           respack, Wms_StrError(r));

    if (send_input) {
        VmWmReqInput in;
        memset(&in, 0, sizeof(in));
        in.win_id = 0;
        in.kind   = 0;                 /* move */
        in.x      = 220;
        in.y      = 160;
        r = Wms_SendInput(c, &in);
        printf("[rdp] input move (220,160) -> %s\n", Wms_StrError(r));
    }

    t0 = uv_hrtime();
    while (Wms_ClientIsConnected(c)) {
        double elapsed = (double)(uv_hrtime() - t0) / 1e9;
        if (elapsed >= (double)seconds) break;
        Wms_PollEvents(c, 50);
    }

    printf("[rdp] frames=%d fullframes=%d events=%d, done\n",
           g_frames, g_fullframes, g_events);
    Wms_ClientDestroy(c);
    return 0;
}
