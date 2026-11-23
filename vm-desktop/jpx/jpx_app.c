#include "jpx_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "vm_clipboard.h"

static JpxApp *g_jpx;

JpxApp *Jpx__App(void) { return g_jpx; }

JpxWindow *Jpx__Window(JpxApp *app, int winId) {
    if (!app) return NULL;
    for (int i = 0; i < VM_WM_MAX_WINDOWS; i++) {
        if (app->windows[i].used && app->windows[i].id == winId) return &app->windows[i];
    }
    return NULL;
}

void Jpx__Invalidate(JpxApp *app) {
    if (!app) return;
    app->dirty = 1;
    /* layout 期间知道是哪个窗口在动，只作废它；否则（主题、标题这类
       影响面不确定的操作）只能保守地全量作废。
       用 WM 记录的 layoutWinId：外壳任务栏不是 JPX 窗口，JPX 自己记不到。 */
    if (app->wm && app->wm->layoutWinId > 0)
        VmWm_InvalidateWindow(app->wm, app->wm->layoutWinId);
    else VmWm_Invalidate(app->wm);
}

/* 诊断开关：JPX_TRACE=1 时在输入处理与每帧合成后各转储一次 WM 状态。
   只读一次环境变量，关闭时几乎零开销。 */
static int trace_on(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("JPX_TRACE");
        cached = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return cached;
}

static unsigned long long jpx_now_ms(void) {
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
#endif
}

/* 从原始输入体里读一个整数字段；WM 不解析修饰键，JPX 自己取。 */
static int json_int_flag(const char *body, size_t len, const char *key) {
    size_t klen = strlen(key);
    for (size_t i = 0; i + klen < len; i++) {
        if (memcmp(body + i, key, klen) != 0) continue;
        size_t j = i + klen;
        while (j < len && (body[j] == ' ' || body[j] == ':' || body[j] == '"')) j++;
        if (j < len && body[j] >= '0' && body[j] <= '9') return body[j] - '0';
        return 1;   /* "true" 也算按下 */
    }
    return 0;
}

/* 首连接一次性事件的内容：只有一条 loadResPack，资源本体走 HTTP。 */
static void rebuild_resources_event(JpxApp *app) {
    static VmDisplayList ev;
    VmDisplay_Init(&ev);
    VmResourceCommand *c = VmDisplay_AppendResource(&ev, VM_CMD_LOAD_RES_PACK, "respack");
    if (c) {
        VmDisplay_CopyStr(c->args.loadResPack.resPackId,
                          sizeof(c->args.loadResPack.resPackId), app->resPackId);
    }
    free(app->resJson);
    app->resJson = VmSerialize_CommandArrayJson(&ev);
}

static void set_res_pack(JpxApp *app, const char *id, const char *json, int broadcast) {
    if (!app || !id || !*id || !json) return;
    char *copy = (char *)malloc(strlen(json) + 1);
    if (!copy) return;
    strcpy(copy, json);
    free(app->resPackJson);
    app->resPackJson = copy;
    snprintf(app->resPackId, sizeof(app->resPackId), "%s", id);
    rebuild_resources_event(app);
    /* 广播同样是那条 loadResPack：已连接的 client 自己重新拉取，SSE 不传资源本体。 */
    if (broadcast && app->server && app->resJson)
        VmServer_BroadcastResources(app->server, app->resJson);
}

int Jpx_ResPack_Register(JpxApp *app, const char *resPackId, const char *json) {
    if (!app || !resPackId || !*resPackId || !json) return 0;
    set_res_pack(app, resPackId, json, 1);
    return app->resPackJson && strcmp(app->resPackId, resPackId) == 0;
}

void Jpx_ResPack_SetImageProvider(JpxApp *app,
                                  int (*provider)(void *userData, const char *resPackId,
                                                  int index, const char **imgId,
                                                  const char **mime,
                                                  const char **data, size_t *size),
                                  void *userData) {
    if (!app || !app->server) return;
    /* 签名与 VmResPackImageAt 完全一致，直接转交；jpx.h 不必 include vm_server.h。 */
    VmServer_SetResPackImageProvider(app->server, (VmResPackImageAt)provider, userData);
}

static void build_resources(JpxApp *app) {
    static VmDisplayList res;
    VmDisplay_Init(&res);
    VmTheme_AppendResources(&res, VmTheme_Default());

    VmDisplay_Init(&app->desktopProgram);
    VmDrawCommand *bg = VmDisplay_Append(&app->desktopProgram, VM_CMD_DRAW_RECT, "desktop_bg");
    if (bg) {
        VmRect r = {0, 0, (float)app->opt.width, (float)app->opt.height};
        bg->rect = r;
        bg->args.drawRect.rect = r;
        VmDisplay_CopyStr(bg->args.drawRect.paint, sizeof(bg->args.drawRect.paint), "s_desktop");
    }

    VmResourceCommand *p = VmDisplay_AppendResource(&res, VM_CMD_DEFINE_DRAW_PROGRAM, "def_prog_desktop");
    if (p) {
        VmDisplay_CopyStr(p->args.defineProgram.programId,
                          sizeof(p->args.defineProgram.programId), "prog_desktop");
        p->args.defineProgram.commands = &app->desktopProgram;
        VmDisplay_CopyStr(p->args.defineProgram.layerId,
                          sizeof(p->args.defineProgram.layerId), "bg");
    }
    p = VmDisplay_AppendResource(&res, VM_CMD_START_PROGRAM, "start_prog_desktop");
    if (p) {
        VmDisplay_CopyStr(p->args.programControl.programId,
                          sizeof(p->args.programControl.programId), "prog_desktop");
        VmDisplay_CopyStr(p->args.programControl.mode,
                          sizeof(p->args.programControl.mode), "once");
    }

    /* 光标只下发一次，每帧只引用 imgId。 */
    p = VmDisplay_AppendResource(&res, VM_CMD_LOAD_IMG, "res_cursor");
    if (p) {
        VmDisplay_CopyStr(p->args.loadImg.imgId, sizeof(p->args.loadImg.imgId), "cursor");
        VmDisplay_CopyStr(p->args.loadImg.dataUrl, sizeof(p->args.loadImg.dataUrl),
                          VmWm_CursorDataUrl());
    }

    char *json = VmSerialize_CommandArrayJson(&res);
    if (json) {
        set_res_pack(app, "default", json, 0);
        free(json);
    }
}

static const char *resource_provider(void *userData) {
    JpxApp *app = (JpxApp *)userData;
    return app->resJson ? app->resJson : "[]";
}

/* client 带 id 来取资源包本体。只提供当前这一个：旧主题包留着没有消费者。 */
static const char *respack_provider(const char *resPackId, void *userData) {
    JpxApp *app = (JpxApp *)userData;
    if (!app || !app->resPackJson || !resPackId) return NULL;
    if (strcmp(resPackId, app->resPackId) != 0) return NULL;
    return app->resPackJson;
}

static const char *resolve_font(uint16_t fontId) {
    (void)fontId;
    return "sans-serif";
}

static char *build_tiles(JpxApp *app, int full) {
    VmWm_BuildFrame(app->wm, &app->frame, app->frameId, resolve_font);
    VmTiles *tiles = VmTiles_Create();
    if (!tiles) return NULL;
    VmTiles_SetFrameId(tiles, app->frameId);
    if (!VmTileGrid_Build(&app->grid, &app->frame, full, tiles)) {
        VmTiles_Destroy(tiles);
        return NULL;
    }
    return VmTiles_Finish(tiles);
}

static const char *frame_provider(void *userData, unsigned int *frameId) {
    JpxApp *app = (JpxApp *)userData;
    int wmDirty = VmWm_ConsumeDirty(app->wm);
    if (!app->dirty && !wmDirty) return NULL;
    app->dirty = 0;
    *frameId = app->frameId++;
    free(app->tilesJson);
    app->tilesJson = build_tiles(app, 0);
    /* 合成之后的窗口状态：built=<frameId> 表示这一帧重建过，dirty=1 表示还有遗留。 */
    if (trace_on()) VmWm_Dump(app->wm, "after-build");
    /* 诊断：这一帧重跑了几个窗口的布局。正常应只 +1（被作废的那个），
       若一次 +窗口总数说明失效又被放大成全量。 */
    if (app->opt.logLevel >= 2) {
        printf("[jpx] frame=%u layoutTotal=%u\n", *frameId, app->wm->layoutCount);
        fflush(stdout);
    }
    return app->tilesJson;
}

static const char *full_frame_provider(void *userData) {
    JpxApp *app = (JpxApp *)userData;
    free(app->fullJson);
    app->fullJson = build_tiles(app, 1);
    return app->fullJson ? app->fullJson : "";
}

static void input_handler(const char *body, size_t length, void *userData) {
    JpxApp *app = (JpxApp *)userData;
    app->ctrl = json_int_flag(body, length, "\"ctrl\"");
    app->shift = json_int_flag(body, length, "\"shift\"");
    app->alt = json_int_flag(body, length, "\"alt\"");
    int changed = VmWm_HandleInput(app->wm, body, length);
    app->dirty = 1;
    if (app->opt.logLevel >= 2) {
        printf("[jpx] changed=%d focus=%d %.*s\n", changed, app->wm->focusId,
               (int)(length > 160 ? 160 : length), body);
        fflush(stdout);
    }
    /* 输入之后的窗口状态：谁被作废、因何作废、任务栏在不在作废名单里。 */
    if (trace_on()) VmWm_Dump(app->wm, "after-input");
}

/* caret 闪烁：仅有聚焦控件时才翻相位并出帧，否则保持空闲零帧。 */
static void caret_blink(void *userData) {
    JpxApp *app = (JpxApp *)userData;
    for (int i = 0; i < VM_WM_MAX_WINDOWS; i++) {
        JpxWindow *w = &app->windows[i];
        if (!w->used || !w->focusId[0]) continue;
        app->caretOn = !app->caretOn;
        app->dirty = 1;
        /* caret 只属于这一个窗口：其他窗口的内容原样复用，不跑 Clay。 */
        VmWm_InvalidateWindow(app->wm, w->id);
        return;
    }
}

static void tick_handler(void *userData) {
    JpxApp *app = (JpxApp *)userData;
    unsigned long long now = jpx_now_ms();
    for (int i = 0; i < JPX_MAX_TIMERS; i++) {
        JpxTimer *t = &app->timers[i];
        if (!t->used) continue;
        if (t->last != 0 && now - t->last < (unsigned long long)t->ms) continue;
        t->last = now;
        /* 出帧由回调自己决定：caret_blink / procs_tick / launcher_tick 都会
           在真正改变状态时调 Jpx_Invalidate。这里无条件标脏会让纯轮询型
           定时器（如开始菜单的 pending 检查）每 60ms 空转出一帧。 */
        if (t->fn) t->fn(t->user);
    }
}

JpxApp *Jpx_Create(const JpxOptions *opt) {
    if (g_jpx) return NULL;   /* 单进程单实例：Clay 的度量函数是进程级状态 */
    JpxApp *app = (JpxApp *)calloc(1, sizeof(JpxApp));
    if (!app) return NULL;
    if (opt) app->opt = *opt;
    if (app->opt.width <= 0) app->opt.width = 800;
    if (app->opt.height <= 0) app->opt.height = 600;
    if (app->opt.tileW <= 0) app->opt.tileW = 100;
    if (app->opt.tileH <= 0) app->opt.tileH = 100;
    if (app->opt.port == 0) app->opt.port = 9000;
    app->theme = *Jpx__DefaultTheme();
    app->nextWinId = 1;
    app->nextTimerId = 1;

    if (app->opt.fontPath) {
        if (VmFont_LoadFile(app->opt.fontPath, app->theme.fontId) != 0 && app->opt.logLevel >= 1) {
            fprintf(stderr, "[jpx] 字体加载失败: %s（度量退回估算值）\n", app->opt.fontPath);
        }
    }

    app->wm = VmWm_Create(app->opt.width, app->opt.height);
    if (!app->wm) { free(app); return NULL; }
    Jpx__ApplyChrome(app);

    int cols = (app->opt.width + app->opt.tileW - 1) / app->opt.tileW;
    int rows = (app->opt.height + app->opt.tileH - 1) / app->opt.tileH;
    if (cols * rows > VM_TILE_MAX) {
        if (app->opt.logLevel >= 1) fprintf(stderr, "[jpx] 瓦片数超过上限: %d\n", cols * rows);
        VmWm_Free(app->wm);
        free(app);
        return NULL;
    }
    VmTileGrid_Init(&app->grid, cols, rows, app->opt.tileW, app->opt.tileH,
                    app->opt.width, app->opt.height);
    VmDisplay_Init(&app->frame);

    build_resources(app);

    app->shell = VmShell_Create(app->wm, VmTheme_Default(),
                                VmTheme_Default()->panelHeight);
    if (!app->shell && app->opt.logLevel >= 1) {
        fprintf(stderr, "[jpx] 任务栏创建失败（窗口数已达上限？）\n");
    }

    app->server = VmServer_Create(app->opt.port, app->opt.webRoot);
    if (!app->server) {
        VmWm_Free(app->wm);
        free(app->resJson);
        free(app);
        return NULL;
    }
    VmServer_SetResourceProvider(app->server, resource_provider, app);
    VmServer_SetResPackProvider(app->server, respack_provider, app);
    /* 前端 fetch('./font.ttf') 拿到的是同一份字体：度量与字形才不会两边不一致。 */
    if (app->opt.fontPath) VmServer_SetFontPath(app->server, app->opt.fontPath);
    VmServer_SetFrameProvider(app->server, frame_provider, app);
    VmServer_SetFullFrameProvider(app->server, full_frame_provider, app);
    VmServer_SetInputHandler(app->server, input_handler, app);
    VmServer_SetTickHandler(app->server, 16, tick_handler, app);

    g_jpx = app;
    app->caretOn = 1;
    app->dirty = 1;
    Jpx_TimerEvery(app, 500, caret_blink, app);
    return app;
}

void Jpx_Destroy(JpxApp *app) {
    if (!app) return;
    if (g_jpx == app) g_jpx = NULL;
    if (app->shell) VmShell_Destroy(app->shell);
    if (app->server) VmServer_Free(app->server);
    if (app->wm) VmWm_Free(app->wm);
    for (int i = 0; i < VM_WM_MAX_WINDOWS; i++) free(app->windows[i].arenaMem);
    free(app->resJson);
    free(app->resPackJson);
    free(app->tilesJson);
    free(app->fullJson);
    free(app);
}

int Jpx_Run(JpxApp *app) {
    if (!app || !app->server) return 1;
    app->running = 1;
    int rc = VmServer_Run(app->server);
    app->running = 0;
    return rc;
}

void Jpx_Stop(JpxApp *app) {
    if (!app || !app->server) return;
    app->running = 0;
    VmServer_Stop(app->server);
}

void Jpx_SetTheme(JpxApp *app, const JpxTheme *theme) {
    if (!app) return;
    app->theme = theme ? *theme : *Jpx__DefaultTheme();
    Jpx__ApplyChrome(app);
    Jpx__Invalidate(app);
}

void Jpx_SetVmTheme(JpxApp *app, const VmTheme *theme) {
    if (!app) return;
    if (!theme) {
        app->hasVmTheme = 0;
        Jpx_SetTheme(app, NULL);
        if (app->shell) {
            VmShell_SetTheme(app->shell, VmTheme_Default());
            VmShell_SetHeight(app->shell, VmTheme_Default()->panelHeight);
        }
        return;
    }
    app->vmTheme = *theme;
    app->hasVmTheme = 1;
    /* 控件主题从 VmTheme 派生；颜色指针借自 theme，生命周期由调用方保证。 */
    JpxTheme t = *Jpx__DefaultTheme();
    t.bgColor = theme->bgColor;
    t.fgColor = theme->fgColor;
    t.borderColor = theme->borderColor;
    t.accentColor = theme->accentColor;
    t.accentTextColor = theme->accentTextColor;
    t.inputBgColor = theme->inputBgColor;
    t.disabledColor = theme->disabledColor;
    t.selectedColor = theme->selectedColor;
    /* startFrom 是 XP 皮肤的标记：按钮换成 Luna 米灰实体块，文字转黑；
       控件边框用灰蓝 #7f9db9（窗口边框是深蓝，由 VmChrome 单独决定）。 */
    if (theme->startFrom) {
        t.buttonBgColor = "rgba(236,233,216,1)";
        t.buttonTextColor = "rgba(0,0,0,1)";
        t.borderColor = "rgba(127,157,185,1)";
    }
    t.fontId = theme->fontId;
    t.fontSize = theme->fontSize;
    t.radius = theme->radius;
    t.padding = theme->padding;
    t.gap = theme->gap;
    app->theme = t;
    if (app->wm) VmWm_SetChrome(app->wm, &theme->chrome);
    if (app->shell) {
        VmShell_SetTheme(app->shell, &app->vmTheme);
        if (theme->panelHeight > 0) VmShell_SetHeight(app->shell, theme->panelHeight);
    }
    Jpx__Invalidate(app);
}

const JpxTheme *Jpx_Theme(JpxApp *app) {
    return app ? &app->theme : Jpx__DefaultTheme();
}

int Jpx_WindowOpen(JpxApp *app, const JpxWindowDesc *desc) {
    JpxWindow *w = Jpx__WindowCreate(app, desc);
    return w ? w->id : 0;
}

void Jpx_WindowClose(JpxApp *app, int winId) {
    Jpx__WindowDestroy(app, winId);
}

void Jpx_WindowMaximize(JpxApp *app, int winId) {
    if (!app || !app->wm) return;
    if (VmWm_Maximize(app->wm, winId)) app->dirty = 1;
}

void Jpx_WindowUnmaximize(JpxApp *app, int winId) {
    if (!app || !app->wm) return;
    if (VmWm_Unmaximize(app->wm, winId)) app->dirty = 1;
}

void Jpx_WindowToggleMaximize(JpxApp *app, int winId) {
    if (!app || !app->wm) return;
    if (VmWm_ToggleMaximize(app->wm, winId)) app->dirty = 1;
}

int Jpx_WindowIsMaximized(JpxApp *app, int winId) {
    return (app && app->wm) ? VmWm_IsMaximized(app->wm, winId) : 0;
}

void Jpx_WindowSetTitle(JpxApp *app, int winId, const char *title) {
    JpxWindow *w = Jpx__Window(app, winId);
    if (!w) return;
    snprintf(w->title, sizeof(w->title), "%s", title ? title : "");
    VmWindow *vw = VmWm_FindWindow(app->wm, winId);
    if (vw) vw->title = w->title;
    /* 只有这个窗口的标题变了，其他窗口不必重跑布局。 */
    app->dirty = 1;
    VmWm_InvalidateWindow(app->wm, winId);
}

void Jpx_Invalidate(JpxApp *app) {
    Jpx__Invalidate(app);
}

int Jpx_TimerEvery(JpxApp *app, int ms, void (*fn)(void *), void *user) {
    if (!app || !fn || ms <= 0) return 0;
    for (int i = 0; i < JPX_MAX_TIMERS; i++) {
        JpxTimer *t = &app->timers[i];
        if (t->used) continue;
        t->used = 1;
        t->id = app->nextTimerId++;
        t->ms = ms;
        t->last = 0;
        t->fn = fn;
        t->user = user;
        return t->id;
    }
    return 0;
}

void Jpx_TimerCancel(JpxApp *app, int timerId) {
    if (!app) return;
    for (int i = 0; i < JPX_MAX_TIMERS; i++) {
        if (app->timers[i].used && app->timers[i].id == timerId) {
            memset(&app->timers[i], 0, sizeof(JpxTimer));
            return;
        }
    }
}

void Jpx_ShellRefresh(JpxApp *app) {
    if (!app) return;
    VmShell_Refresh(app->shell);
    app->dirty = 1;
}

void Jpx_SetStartHandler(JpxApp *app, JpxStartHandler handler, void *userData) {
    if (!app) return;
    VmShell_SetStartHandler(app->shell, (VmShellStartHandler)handler, userData);
}

void Jpx_ClipboardSet(const char *utf8) {
    VmClipboard_SetText(utf8);
}

int Jpx_ClipboardGet(char *out, int cap) {
    return VmClipboard_GetText(out, cap);
}

void Jpx_Log(JpxApp *app, int level, const char *fmt, ...) {
    int minLevel = app ? app->opt.logLevel : 1;
    if (level > minLevel) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}
