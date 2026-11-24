/*
 * vmwm_mock_host.c - 内置模拟宿主（联调 / e2e 用）
 *
 * 它不合成真实画面，只证明「内容 → 宿主 → 帧 → RDP → 浏览器」这条管道是通的：
 *   - 收到内容就标脏
 *   - 每个脏 tick 广播一条占位 tiles JSON
 *   - fullframe / respack 请求回一条占位 JSON
 *
 * 接入真实合成层（主项目的 VmWm + VmDisplay + VmTileGrid）后不要再注册它。
 */
#include <stdio.h>
#include <string.h>

#include "wms_host.h"
#include "wms_server.h"
#include "wms_log.h"

static int      g_dirty    = 1;      /* 首帧必发 */
static uint32_t g_last_frame = 0;
static int      g_win_count  = 0;

static void refresh_count(void)
{
    VmWmWindowInfo list[VMWM_MAX_WINDOWS];
    g_win_count = (int)VmWm_ListWindows(list, VMWM_MAX_WINDOWS);
}

static void mock_on_content(void* user, uint64_t win_id, uint32_t epoch,
                            uint32_t count, uint32_t cmd_size,
                            const void* cmds, int overflow)
{
    (void)user; (void)cmds;
    g_dirty = 1;
    vmwm_log("[mock] content win=%llu epoch=%u count=%u cmd_size=%u%s",
             (unsigned long long)win_id, (unsigned)epoch, (unsigned)count,
             (unsigned)cmd_size, overflow ? " (truncated)" : "");
}

static void mock_on_window_event(void* user, uint32_t evt,
                                 const VmWmWindowInfo* info)
{
    (void)user;
    g_dirty = 1;
    if (info) {
        vmwm_log("[mock] window evt=0x%x id=%llu (%d,%d) %dx%d mapped=%u",
                 (unsigned)evt, (unsigned long long)info->win_id,
                 info->x, info->y, info->w, info->h, (unsigned)info->mapped);
    }
}

static void mock_on_window_removed(void* user, uint64_t win_id)
{
    (void)user;
    g_dirty = 1;
    vmwm_log("[mock] window removed id=%llu", (unsigned long long)win_id);
}

static int mock_body_rect(void* user, uint64_t win_id, int32_t w, int32_t h,
                          VmWmRect* out)
{
    (void)user; (void)win_id; (void)w; (void)h; (void)out;
    return 0;        /* 0 = 整窗就是 body，没有装饰偏移 */
}

static uint64_t mock_hit_test(void* user, int32_t x, int32_t y)
{
    (void)user; (void)x; (void)y;
    return 0;        /* 0 = 交给服务端自带的 z 序命中 */
}

static void mock_on_tick(void* user, uint32_t frame_id)
{
    char json[256];
    (void)user;

    if (!g_dirty) return;

    g_dirty      = 0;
    g_last_frame = frame_id;
    refresh_count();

    snprintf(json, sizeof(json),
             "{\"type\":\"tiles\",\"frameId\":%u,\"windows\":%d,\"mock\":true}",
             (unsigned)frame_id, g_win_count);

    VmWm_ServerBroadcastFrame(json, (uint32_t)strlen(json), 0);
}

static void mock_on_frame_request(void* user, uint32_t client_id)
{
    char json[256];
    (void)user;

    refresh_count();
    snprintf(json, sizeof(json),
             "{\"type\":\"fullframe\",\"frameId\":%u,\"windows\":%d,\"mock\":true}",
             (unsigned)g_last_frame, g_win_count);

    if (VmWm_ServerSendFrame(client_id, json, (uint32_t)strlen(json), 1) != VMWM_OK)
        vmwm_log("[mock] fullframe reply failed (client=%u)", (unsigned)client_id);
}

static void mock_on_respack_request(void* user, uint32_t client_id,
                                    const char* id)
{
    char json[256];
    (void)user;

    snprintf(json, sizeof(json),
             "{\"id\":\"%s\",\"styles\":[],\"patterns\":[],\"images\":[],\"mock\":true}",
             id && id[0] ? id : "default");

    if (VmWm_ServerSendRespack(client_id, id, json,
                              (uint32_t)strlen(json)) != VMWM_OK)
        vmwm_log("[mock] respack reply failed (client=%u)", (unsigned)client_id);
}

static void mock_on_workarea(void* user, const VmWmReqWorkArea* area)
{
    (void)user;
    vmwm_log("[mock] workarea (%d,%d) %dx%d", area->x, area->y,
             area->w, area->h);
    g_dirty = 1;
}

static void mock_on_theme(void* user, const char* name)
{
    (void)user;
    vmwm_log("[mock] theme '%s'", name);
    g_dirty = 1;
}

const VmWmHost* VmWm_MockHost(void)
{
    static VmWmHost host;
    static int init = 0;

    if (!init) {
        memset(&host, 0, sizeof(host));
        host.on_content          = mock_on_content;
        host.on_window_event     = mock_on_window_event;
        host.on_window_removed   = mock_on_window_removed;
        host.body_rect           = mock_body_rect;
        host.hit_test            = mock_hit_test;
        host.on_tick             = mock_on_tick;
        host.on_frame_request    = mock_on_frame_request;
        host.on_respack_request  = mock_on_respack_request;
        host.on_workarea         = mock_on_workarea;
        host.on_theme            = mock_on_theme;
        init = 1;
    }
    return &host;
}
