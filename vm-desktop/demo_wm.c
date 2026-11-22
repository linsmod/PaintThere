/* Minimal WM demo: two Clay windows, focus + title-bar drag over SSE.
   Build: gcc -std=c11 -I<clay-dir> -I. demo_wm.c demo_layout.c clay_frame_adapter.c vm_wm.c vm_server.c -lws2_32 -o demo_wm
   Run:   ./demo_wm 9000 ../ConsoleApp2 */
#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "clay_frame_adapter.h"
#include "vm_display.h"
#include "vm_serialize_json.h"
#include "vm_wm.h"
#include "vm_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VmWm *g_wm;
static unsigned int g_frameId;
static int g_dirty = 1;   /* adaptive frame rate: only publish on change */
static int g_forceFull = 0; /* 下一次出帧必须是全量（新客户端/字体就绪） */
static char *g_resourcesJson = NULL;
static char *g_fullOpsJson = NULL;

/* Static nested program: desktop background rectangle. */
static VmDisplayList g_desktopProgramCommands;

static void BuildResources(void) {
    VmDisplayList res;
    VmDisplay_Init(&res);

    VmDrawCommand *p = VmDisplay_Append(&res, VM_CMD_DEFINE_PATTERN, "def_p_desktop");
    VmDisplay_CopyStr(p->args.definePattern.patternId, sizeof(p->args.definePattern.patternId), "p_desktop");
    p->args.definePattern.type = VM_PATTERN_GRADIENT;
    VmDisplay_CopyStr(p->args.definePattern.from, sizeof(p->args.definePattern.from), "rgba(24,26,32,1)");
    VmDisplay_CopyStr(p->args.definePattern.to, sizeof(p->args.definePattern.to), "rgba(48,54,66,1)");
    VmDisplay_CopyStr(p->args.definePattern.direction, sizeof(p->args.definePattern.direction), "diagonal");

    p = VmDisplay_Append(&res, VM_CMD_DEFINE_PATTERN, "def_p_window");
    VmDisplay_CopyStr(p->args.definePattern.patternId, sizeof(p->args.definePattern.patternId), "p_window");
    p->args.definePattern.type = VM_PATTERN_SOLID;
    VmDisplay_CopyStr(p->args.definePattern.color, sizeof(p->args.definePattern.color), "rgba(250,250,252,1)");

    p = VmDisplay_Append(&res, VM_CMD_DEFINE_STYLE, "def_s_desktop");
    VmDisplay_CopyStr(p->args.defineStyle.styleId, sizeof(p->args.defineStyle.styleId), "s_desktop");
    VmDisplay_CopyStr(p->args.defineStyle.bgPattern, sizeof(p->args.defineStyle.bgPattern), "p_desktop");

    p = VmDisplay_Append(&res, VM_CMD_DEFINE_STYLE, "def_s_window");
    VmDisplay_CopyStr(p->args.defineStyle.styleId, sizeof(p->args.defineStyle.styleId), "s_window");
    VmDisplay_CopyStr(p->args.defineStyle.bgPattern, sizeof(p->args.defineStyle.bgPattern), "p_window");

    VmDisplay_Init(&g_desktopProgramCommands);
    VmDrawCommand *bg = VmDisplay_Append(&g_desktopProgramCommands, VM_CMD_DRAW_RECT, "desktop_bg");
    VmRect r = {0, 0, 800, 600};
    bg->rect = r;
    bg->args.drawRect.rect = r;
    VmDisplay_CopyStr(bg->args.drawRect.paint, sizeof(bg->args.drawRect.paint), "s_desktop");

    p = VmDisplay_Append(&res, VM_CMD_DEFINE_DRAW_PROGRAM, "def_prog_desktop");
    VmDisplay_CopyStr(p->args.defineProgram.programId, sizeof(p->args.defineProgram.programId), "prog_desktop");
    p->args.defineProgram.commands = &g_desktopProgramCommands;
    VmDisplay_CopyStr(p->args.defineProgram.layerId, sizeof(p->args.defineProgram.layerId), "bg");

    p = VmDisplay_Append(&res, VM_CMD_START_PROGRAM, "start_prog_desktop");
    VmDisplay_CopyStr(p->args.programControl.programId, sizeof(p->args.programControl.programId), "prog_desktop");
    VmDisplay_CopyStr(p->args.programControl.mode, sizeof(p->args.programControl.mode), "once");

    g_resourcesJson = VmSerialize_CommandArrayJson(&res);
}

static const char *ResourceProvider(void *userData) {
    (void)userData;
    return g_resourcesJson ? g_resourcesJson : "[]";
}

static const char *ResolveFont(uint16_t fontId) {
    (void)fontId;
    return "sans-serif";
}

static Clay_Dimensions MeasureText(
    Clay_StringSlice text, Clay_TextElementConfig *config, void *userData) {
    (void)userData;
    return (Clay_Dimensions){
        .width = (float)text.length * (float)config->fontSize * 0.6f,
        .height = (float)config->fontSize,
    };
}

static void HandleClayErrors(Clay_ErrorData data) {
    fwrite(data.errorText.chars, 1, data.errorText.length, stderr);
}

static Clay_Context *CreateWindowContext(int w, int h) {
    uint64_t total = Clay_MinMemorySize();
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(total, malloc(total));
    Clay_Context *ctx = Clay_Initialize(arena, (Clay_Dimensions){(float)w, (float)h},
                                        (Clay_ErrorHandler){HandleClayErrors});
    Clay_SetMeasureTextFunction(MeasureText, 0);
    return ctx;
}

typedef struct {
    int clicks;
    char label[64];   /* stable storage for Clay_String (must outlive layout) */
    char text[128];   /* simple line editor buffer */
    size_t textLen;
} AppStateA;
static AppStateA g_appA;

static int OnWindowKey(int windowId, const char *key, const char *code,
                       int isDown, void *userData) {
    (void)windowId;
    (void)code;
    if (!isDown) return 0;  /* only act on keydown */
    AppStateA *state = (AppStateA *)userData;
    if (strcmp(key, "Backspace") == 0) {
        if (state->textLen > 0) state->text[--state->textLen] = '\0';
        return 1;
    }
    if (strcmp(key, "Enter") == 0) return 1;
    /* Accept single UTF-8 characters (browser reports printable key as-is). */
    size_t n = strlen(key);
    if (n == 1 && (unsigned char)key[0] >= 0x20 &&
        state->textLen + n + 1 < sizeof(state->text)) {
        memcpy(state->text + state->textLen, key, n);
        state->textLen += n;
        state->text[state->textLen] = '\0';
        return 1;
    }
    return 0;
}

static void OnButtonHover(Clay_ElementId id, Clay_PointerData pointerData, void *userData) {
    (void)id;
    AppStateA *state = (AppStateA *)userData;
    if (pointerData.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) state->clicks++;
}

static Clay_RenderCommandArray LayoutA(void *userData) {
    AppStateA *state = (AppStateA *)userData;
    snprintf(state->label, sizeof(state->label), "Clicked: %d", state->clicks);
    Clay_String labelStr = {.chars = state->label, .length = (int)strlen(state->label)};
    Clay_String textStr = {.chars = state->text, .length = (int)state->textLen};
    Clay_BeginLayout();
    CLAY(CLAY_ID("A_root"), {
        .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                   .layoutDirection = CLAY_TOP_TO_BOTTOM,
                   .padding = CLAY_PADDING_ALL(12), .childGap = 8},
    }) {
        CLAY_TEXT(CLAY_STRING("Type below; focus this client with a click"),
                  CLAY_TEXT_CONFIG({.fontId = 0, .fontSize = 14,
                                    .textColor = (Clay_Color){30,30,30,255}}));
        CLAY(CLAY_ID("A_input"), {
            .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32)},
                       .padding = CLAY_PADDING_ALL(6)},
            .backgroundColor = (Clay_Color){255,255,255,255},
            .border = {.color = (Clay_Color){180,186,194,255}, .width = CLAY_BORDER_ALL(1)},
        }) {
            CLAY_TEXT(textStr,
                      CLAY_TEXT_CONFIG({.fontId = 0, .fontSize = 14,
                                        .textColor = (Clay_Color){20,24,30,255}}));
        }
        CLAY(CLAY_ID("A_btn"), {
            .layout = {.sizing = {CLAY_SIZING_FIXED(140), CLAY_SIZING_FIXED(40)},
                       .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
            .backgroundColor = Clay_Hovered() ? (Clay_Color){240,140,40,255}
                                              : (Clay_Color){52,140,220,255},
        }) {
            CLAY_TEXT(labelStr,
                      CLAY_TEXT_CONFIG({.fontId = 0, .fontSize = 14,
                                        .textColor = (Clay_Color){255,255,255,255}}));
            Clay_OnHover(OnButtonHover, state);
        }
    }
    return Clay_EndLayout(0.016f);
}

static Clay_RenderCommandArray LayoutB(void *userData) {
    (void)userData;
    Clay_BeginLayout();
    CLAY(CLAY_ID("B_root"), {
        .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                   .layoutDirection = CLAY_TOP_TO_BOTTOM,
                   .padding = CLAY_PADDING_ALL(12), .childGap = 8},
    }) {
        CLAY_TEXT(CLAY_STRING("Window B"),
                  CLAY_TEXT_CONFIG({.fontId = 0, .fontSize = 16,
                                    .textColor = (Clay_Color){30,30,30,255}}));
        CLAY(CLAY_ID("B_badge"), {
            .layout = {.sizing = {CLAY_SIZING_FIXED(90), CLAY_SIZING_FIXED(30)}},
            .backgroundColor = (Clay_Color){240,140,40,255},
        }) {}
    }
    return Clay_EndLayout(0.016f);
}

/* ---------------- 分层合成 ---------------- */

#define DEMO_MAX_WINDOW_LAYERS VM_WM_MAX_WINDOWS

typedef struct {
    int winId;
    int x, y, w, h;
    int z;
    int created;
    VmDisplayList cur;
    VmDisplayList prev;
} DemoWindowLayer;

static DemoWindowLayer g_layers[DEMO_MAX_WINDOW_LAYERS];
static int g_layerCount;
static VmDisplayList g_cursorCur;
static VmDisplayList g_cursorPrev;
static int g_cursorCreated;
static char *g_opsJson = NULL;

static int rect_ok(VmRect r) { return r.w > 0 && r.h > 0; }

static void rect_union(VmRect *acc, int *has, VmRect r) {
    if (!rect_ok(r)) return;
    if (!*has) { *acc = r; *has = 1; return; }
    float x1 = acc->x < r.x ? acc->x : r.x;
    float y1 = acc->y < r.y ? acc->y : r.y;
    float ax2 = acc->x + acc->w, rx2 = r.x + r.w;
    float ay2 = acc->y + acc->h, ry2 = r.y + r.h;
    float x2 = ax2 > rx2 ? ax2 : rx2;
    float y2 = ay2 > ry2 ? ay2 : ry2;
    acc->x = x1; acc->y = y1; acc->w = x2 - x1; acc->h = y2 - y1;
}

static int list_changed(const VmDisplayList *a, const VmDisplayList *b) {
    if (a->count != b->count) return 1;
    for (int i = 0; i < a->count; i++)
        if (!VmDisplay_CommandEqual(&a->commands[i], &b->commands[i])) return 1;
    return 0;
}

static void list_copy(VmDisplayList *dst, const VmDisplayList *src) {
    dst->count = src->count;
    dst->frameId = src->frameId;
    if (src->count)
        memcpy(dst->commands, src->commands, sizeof(VmDrawCommand) * (size_t)src->count);
}

static DemoWindowLayer *layer_for(int winId) {
    for (int i = 0; i < g_layerCount; i++)
        if (g_layers[i].winId == winId) return &g_layers[i];
    if (g_layerCount >= DEMO_MAX_WINDOW_LAYERS) return NULL;
    DemoWindowLayer *l = &g_layers[g_layerCount++];
    memset(l, 0, sizeof(*l));
    l->winId = winId;
    return l;
}

static void layer_id_of(int winId, char *out, size_t cap) {
    snprintf(out, cap, "w%d", winId);
}

/* 生成一帧的合成指令。full=1 时不依赖也不修改历史状态，用于新客户端。 */
static char *BuildOps(int full) {
    if (!g_wm) return NULL;
    VmOps *ops = VmOps_Create();
    if (!ops) return NULL;

    VmRect dirty = {0, 0, 0, 0};
    int hasDirty = 0;
    char id[32];

    if (full) rect_union(&dirty, &hasDirty,
                         (VmRect){0, 0, (float)g_wm->width, (float)g_wm->height});

    for (int wi = 0; wi < g_wm->count; wi++) {
        VmWindow *win = &g_wm->windows[wi];
        if (win->minimized) continue;
        DemoWindowLayer *l = layer_for(win->id);
        if (!l) continue;

        VmWm_BuildWindowLayer(g_wm, win, &l->cur, ResolveFont);
        int sizeChanged = (l->w != win->w || l->h != win->h);
        int contentChanged = full || !l->created || sizeChanged || list_changed(&l->cur, &l->prev);
        int zChanged = (l->created && l->z != win->z);

        layer_id_of(win->id, id, sizeof(id));
        if (contentChanged) {
            VmOps_CreateLayer(ops, id, win->w, win->h);
            VmOps_Bind(ops, id);
            VmOps_ClearLayer(ops, (VmRect){0, 0, (float)win->w, (float)win->h});
            VmOps_Draw(ops, l->cur.commands, l->cur.count);
            rect_union(&dirty, &hasDirty,
                       (VmRect){(float)win->x, (float)win->y, (float)win->w, (float)win->h});
        }
        if (l->created && (l->x != win->x || l->y != win->y)) {
            /* 移动：旧位置和新位置都要重合成 */
            rect_union(&dirty, &hasDirty,
                       (VmRect){(float)l->x, (float)l->y, (float)l->w, (float)l->h});
            rect_union(&dirty, &hasDirty,
                       (VmRect){(float)win->x, (float)win->y, (float)win->w, (float)win->h});
        }
        if (zChanged) {
            /* 层叠次序变了：所有窗口区域都可能换显示内容 */
            for (int k = 0; k < g_wm->count; k++) {
                VmWindow *w2 = &g_wm->windows[k];
                rect_union(&dirty, &hasDirty,
                           (VmRect){(float)w2->x, (float)w2->y, (float)w2->w, (float)w2->h});
            }
        }

        if (!full) {
            l->created = 1;
            l->x = win->x; l->y = win->y;
            l->w = win->w; l->h = win->h;
            l->z = win->z;
            list_copy(&l->prev, &l->cur);
        }
    }

    /* 指针层：全屏坐标，只清新旧指针覆盖的区域 */
    VmWm_BuildCursorLayer(g_wm, &g_cursorCur);
    if (full || !g_cursorCreated || list_changed(&g_cursorCur, &g_cursorPrev)) {
        VmRect c = {0, 0, 0, 0};
        int hc = 0;
        for (int i = 0; i < g_cursorPrev.count; i++)
            rect_union(&c, &hc, g_cursorPrev.commands[i].rect);
        for (int i = 0; i < g_cursorCur.count; i++)
            rect_union(&c, &hc, g_cursorCur.commands[i].rect);
        if (!hc) c = (VmRect){0, 0, (float)g_wm->width, (float)g_wm->height};

        VmOps_CreateLayer(ops, "cursor", g_wm->width, g_wm->height);
        VmOps_Bind(ops, "cursor");
        VmOps_ClearLayer(ops, c);
        VmOps_Draw(ops, g_cursorCur.commands, g_cursorCur.count);
        rect_union(&dirty, &hasDirty, c);
        if (!full) {
            g_cursorCreated = 1;
            list_copy(&g_cursorPrev, &g_cursorCur);
        }
    }

    if (!hasDirty) {
        VmOps_Destroy(ops);
        return NULL;
    }

    /* 合成到主画布：清脏区 -> bg -> 窗口（按 z）-> cursor */
    VmOps_ClearScreen(ops, dirty);
    VmOps_Composite(ops, "bg", dirty, (int)dirty.x, (int)dirty.y);

    for (int z = 1; z < g_wm->nextZ; z++) {
        for (int wi = 0; wi < g_wm->count; wi++) {
            VmWindow *win = &g_wm->windows[wi];
            if (win->z != z || win->minimized) continue;
            DemoWindowLayer *l = layer_for(win->id);
            if (!l || (!l->created && !full)) continue;
            float x1 = dirty.x > (float)win->x ? dirty.x : (float)win->x;
            float y1 = dirty.y > (float)win->y ? dirty.y : (float)win->y;
            float dx2 = dirty.x + dirty.w, wx2 = (float)(win->x + win->w);
            float dy2 = dirty.y + dirty.h, wy2 = (float)(win->y + win->h);
            float x2 = dx2 < wx2 ? dx2 : wx2;
            float y2 = dy2 < wy2 ? dy2 : wy2;
            if (x2 <= x1 || y2 <= y1) continue;
            layer_id_of(win->id, id, sizeof(id));
            VmRect src = {x1 - (float)win->x, y1 - (float)win->y, x2 - x1, y2 - y1};
            VmOps_Composite(ops, id, src, (int)x1, (int)y1);
        }
    }
    if (full || g_cursorCreated)
        VmOps_Composite(ops, "cursor", dirty, (int)dirty.x, (int)dirty.y);

    return VmOps_Finish(ops);
}

static const char *FrameProvider(void *userData, unsigned int *frameId) {
    (void)userData;
    if (!g_dirty) return NULL;  /* adaptive: no change, no frame */
    g_dirty = 0;
    *frameId = g_frameId++;
    free(g_opsJson);
    g_opsJson = BuildOps(g_forceFull);
    g_forceFull = 0;
    return g_opsJson;
}

/* 客户端请求整屏重绘（字体就绪、层丢内容等情况）。 */
static void RedrawHandler(void *userData) {
    (void)userData;
    g_forceFull = 1;
    g_dirty = 1;
}

static const char *FullFrameProvider(void *userData) {
    (void)userData;
    free(g_fullOpsJson);
    g_fullOpsJson = BuildOps(1);
    return g_fullOpsJson ? g_fullOpsJson : "";
}

static void InputHandler(const char *body, size_t length, void *userData) {
    (void)userData;
    int changed = VmWm_HandleInput(g_wm, body, length);
    g_dirty = 1;  /* pointer moved or window state changed -> publish a new frame */
    printf("[input] changed=%d focus=%d %.*s\n", changed, g_wm->focusId, (int)length, body);
    fflush(stdout);
}

int main(int argc, char **argv) {
    unsigned short port = argc > 1 ? (unsigned short)atoi(argv[1]) : 9000;
    const char *webRoot = argc > 2 ? argv[2] : "../assets";

    g_wm = VmWm_Create(800, 600);
    VmDisplay_Init(&g_cursorCur);
    VmDisplay_Init(&g_cursorPrev);
    if (!g_wm) return 1;
    BuildResources();

    VmWindow *a = VmWm_AddWindow(g_wm, 1, "Window A", 60, 60, 360, 260, LayoutA, &g_appA);
    VmWindow *b = VmWm_AddWindow(g_wm, 2, "Window B", 300, 200, 360, 260, LayoutB, NULL);
    a->clayCtx = CreateWindowContext(356, 230);
    b->clayCtx = CreateWindowContext(356, 230);
    VmWm_SetKeyHandler(g_wm, 1, OnWindowKey, &g_appA);
    VmWm_Focus(g_wm, 1);

    VmServer *server = VmServer_Create(port, webRoot);
    if (!server) {
        fprintf(stderr, "failed to bind port %u\n", port);
        return 1;
    }
    VmServer_SetFrameProvider(server, FrameProvider, NULL);
    VmServer_SetInputHandler(server, InputHandler, NULL);
    VmServer_SetResourceProvider(server, ResourceProvider, NULL);
    VmServer_SetFullFrameProvider(server, FullFrameProvider, NULL);
    VmServer_SetRedrawHandler(server, RedrawHandler, NULL);

    printf("WM demo on http://127.0.0.1:%u/ (two windows, focus + drag)\n", port);
    fflush(stdout);
    int result = VmServer_Run(server);
    VmServer_Free(server);
    free(g_opsJson);
    free(g_fullOpsJson);
    free(g_resourcesJson);
    VmWm_Free(g_wm);
    return result;
}
