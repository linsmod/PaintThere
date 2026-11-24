/*
 * vm_host.c - VmWmHost 的本项目实现
 *
 * 分工：
 *   wmwms 侧：窗口元数据、z、焦点、命中、连接生命周期
 *   本文件：渲染副本 + 装饰 + 命令级合成 + 瓦片切分 + 资源包
 *
 * 渲染副本（VmWm）只由 wmwms 的事件驱动，不调用 VmWm_Move / VmWm_Focus
 * 这类窗口管理 API —— 那些职责在 wmwms 侧，副本只负责画。
 */
#include "vm_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wms_ipc.h"
#include "wms_server.h"

#include "vm_display.h"
#include "vm_serialize_json.h"
#include "vm_theme.h"
#include "vm_tiles.h"
#include "vm_wm.h"

struct VmHost {
    VmWm *wm;
    VmDisplayList frame;
    VmTileGrid grid;
    uint32_t frameId;
    VmTheme theme;
    char *respackJson;
    char respackId[64];
    /* wmwms 的 info->title 只在回调期间有效，副本必须自己存 */
    char titles[VM_WM_MAX_WINDOWS][VMWM_TITLE_MAX];
    VmWmHost cb;
};

static const char *resolve_font(uint16_t fontId) {
    (void)fontId;
    return "sans-serif";
}

/* 主题资源包：definePattern / defineStyle。主题是全局的，只在这里生成一次。
   VmDisplayList 约 263 KB，不能放栈。 */
static void rebuild_resources(VmHost *h) {
    static VmDisplayList res;
    VmDisplay_Init(&res);
    VmTheme_AppendResources(&res, &h->theme);
    char *json = VmSerialize_CommandArrayJson(&res);
    free(h->respackJson);
    h->respackJson = json;
    snprintf(h->respackId, sizeof(h->respackId), "%s", "default");
}

/* full=1 下发 config + 全部块（新连接的基线）；full=0 只发变化的块。 */
static char *build_frame_json(VmHost *h, int full) {
    VmWm_BuildFrame(h->wm, &h->frame, h->frameId, resolve_font);
    VmTiles *tiles = VmTiles_Create();
    if (!tiles) return NULL;
    VmTiles_SetFrameId(tiles, h->frameId);
    if (!VmTileGrid_Build(&h->grid, &h->frame, full, tiles)) {
        VmTiles_Destroy(tiles);
        return NULL;
    }
    return VmTiles_Finish(tiles);
}

/* ---------------- VmWmHost 回调 ---------------- */

static void host_on_content(void *user, uint64_t win_id, uint32_t epoch,
                            uint32_t count, uint32_t cmd_size,
                            const void *cmds, int overflow) {
    VmHost *h = (VmHost *)user;
    (void)epoch;      /* 代次校验已在 wmwms 侧完成，能到这里就是有效的 */
    (void)overflow;
    if (!h || !cmds || cmd_size == 0) return;
    if (cmd_size != sizeof(VmDrawCommand)) {
        /* 两端的命令结构不匹配：宁可不画，也不按错误布局解释内存 */
        fprintf(stderr, "[vm_host] content cmd_size=%u != %u, dropped\n",
                (unsigned)cmd_size, (unsigned)sizeof(VmDrawCommand));
        return;
    }
    VmWindow *vw = VmWm_FindWindow(h->wm, (int)win_id);
    if (!vw) return;
    int n = (int)count;
    if (n > VM_WM_LAYER_CAPACITY) n = VM_WM_LAYER_CAPACITY;
    VmWindowLayer *L = &vw->layer;
    L->external = 1;
    memcpy(L->content, cmds, (size_t)n * sizeof(VmDrawCommand));
    L->contentCount = n;
    VmWm_InvalidateWindow(h->wm, (int)win_id);
}

static void host_on_window_event(void *user, uint32_t evt, const VmWmWindowInfo *info) {
    VmHost *h = (VmHost *)user;
    if (!h || !info) return;
    int id = (int)info->win_id;
    VmWindow *vw = VmWm_FindWindow(h->wm, id);

    if ((evt & EVT_WINDOW_NEW) && !vw) {
        vw = VmWm_AddWindow(h->wm, id, "", info->x, info->y, info->w, info->h,
                            NULL, NULL);
        if (vw) vw->layer.external = 1;
    }
    if (!vw) return;

    vw->x = info->x;
    vw->y = info->y;
    vw->w = info->w;
    vw->h = info->h;
    vw->minimized = info->mapped ? 0 : 1;
    vw->focused = info->focused ? 1 : 0;

    int slot = (int)(vw - h->wm->windows);
    if (slot >= 0 && slot < VM_WM_MAX_WINDOWS) {
        snprintf(h->titles[slot], sizeof(h->titles[slot]), "%s", info->title);
        vw->title = h->titles[slot];
    }

    VmWm_InvalidateWindow(h->wm, id);
}

static void host_on_window_removed(void *user, uint64_t win_id) {
    VmHost *h = (VmHost *)user;
    if (!h) return;
    VmWm_RemoveWindow(h->wm, (int)win_id);
}

static int host_body_rect(void *user, uint64_t win_id, int32_t w, int32_t h,
                          VmWmRect *out) {
    VmHost *host = (VmHost *)user;
    (void)w;
    (void)h;   /* 入参是 wmwms 侧给的窗口尺寸，这里以渲染副本为准 */
    if (!host || !out) return 0;
    VmWindow *vw = VmWm_FindWindow(host->wm, (int)win_id);
    if (!vw) return 0;
    VmRect body;
    VmWm_WindowBody(host->wm, vw, &body);
    /* 返回「窗口局部坐标下的 body 矩形」：wmwms 用它把画布坐标换成 body 局部 */
    out->x = (int32_t)body.x - vw->x;
    out->y = (int32_t)body.y - vw->y;
    out->w = (int32_t)body.w;
    out->h = (int32_t)body.h;
    return 1;
}

static uint64_t host_hit_test(void *user, int32_t x, int32_t y) {
    VmHost *h = (VmHost *)user;
    if (!h) return 0;
    VmWindow *vw = VmWm_HitTest(h->wm, x, y);
    return vw ? (uint64_t)vw->id : 0;
}

static void host_on_tick(void *user, uint32_t frame_id) {
    VmHost *h = (VmHost *)user;
    if (!h) return;
    if (!VmWm_ConsumeDirty(h->wm)) return;   /* 没变化就不出帧 */
    h->frameId = frame_id;
    char *json = build_frame_json(h, 0);
    if (!json) return;
    VmWm_ServerBroadcastFrame(json, (uint32_t)strlen(json), 0);
    free(json);
}

static void host_on_frame_request(void *user, uint32_t client_id) {
    VmHost *h = (VmHost *)user;
    if (!h) return;
    h->frameId++;
    char *json = build_frame_json(h, 1);
    if (!json) return;
    VmWm_ServerSendFrame(client_id, json, (uint32_t)strlen(json), 1);
    free(json);
}

static void host_on_respack_request(void *user, uint32_t client_id, const char *id) {
    VmHost *h = (VmHost *)user;
    if (!h || !h->respackJson) return;
    if (id && *id && strcmp(id, h->respackId) != 0) return;
    VmWm_ServerSendRespack(client_id, h->respackId, h->respackJson,
                           (uint32_t)strlen(h->respackJson));
}

static void host_on_workarea(void *user, const VmWmReqWorkArea *area) {
    VmHost *h = (VmHost *)user;
    if (!h || !area) return;
    VmWm_SetWorkArea(h->wm, area->x, area->y, area->w, area->h);
}

static void host_on_theme(void *user, const char *name) {
    VmHost *h = (VmHost *)user;
    (void)name;   /* M3 只处理默认主题；带名字的主题解析在 M5/M6 接 */
    if (!h) return;
    h->theme = *VmTheme_Default();
    VmWm_SetChrome(h->wm, &h->theme.chrome);
    rebuild_resources(h);
    VmWm_Invalidate(h->wm);
}

/* ---------------- 生命周期 ---------------- */

VmHost *VmHost_Create(int width, int height, int tileW, int tileH) {
    if (width <= 0) width = 800;
    if (height <= 0) height = 600;
    if (tileW <= 0) tileW = 100;
    if (tileH <= 0) tileH = 100;

    VmHost *h = (VmHost *)calloc(1, sizeof(VmHost));
    if (!h) return NULL;
    h->wm = VmWm_Create(width, height);
    if (!h->wm) { free(h); return NULL; }

    h->theme = *VmTheme_Default();
    VmWm_SetChrome(h->wm, &h->theme.chrome);
    VmDisplay_Init(&h->frame);

    int cols = (width + tileW - 1) / tileW;
    int rows = (height + tileH - 1) / tileH;
    VmTileGrid_Init(&h->grid, cols, rows, tileW, tileH, width, height);

    rebuild_resources(h);

    h->cb.user = h;
    h->cb.on_content = host_on_content;
    h->cb.on_window_event = host_on_window_event;
    h->cb.on_window_removed = host_on_window_removed;
    h->cb.body_rect = host_body_rect;
    h->cb.hit_test = host_hit_test;
    h->cb.on_tick = host_on_tick;
    h->cb.on_frame_request = host_on_frame_request;
    h->cb.on_respack_request = host_on_respack_request;
    h->cb.on_workarea = host_on_workarea;
    h->cb.on_theme = host_on_theme;
    return h;
}

void VmHost_Destroy(VmHost *host) {
    if (!host) return;
    if (host->wm) VmWm_Free(host->wm);
    free(host->respackJson);
    free(host);
}

const VmWmHost *VmHost_Callbacks(VmHost *host) {
    return host ? &host->cb : NULL;
}

struct VmWm *VmHost_Wm(VmHost *host) {
    return host ? host->wm : NULL;
}
