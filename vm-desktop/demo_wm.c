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
static VmDisplayList g_frame;
static unsigned int g_frameId;
static int g_dirty = 1;   /* adaptive frame rate: only publish on change */
static char *g_resourcesJson = NULL;

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

static char *g_frameJson = NULL;

static const char *FrameProvider(void *userData, unsigned int *frameId) {
    (void)userData;
    if (!g_dirty) return NULL;  /* adaptive: no change, no frame */
    g_dirty = 0;
    VmWm_BuildFrame(g_wm, &g_frame, g_frameId, ResolveFont);
    *frameId = g_frameId++;
    free(g_frameJson);
    g_frameJson = VmSerialize_FrameJson(&g_frame);
    return g_frameJson;
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
    const char *webRoot = argc > 2 ? argv[2] : "../ConsoleApp2";

    g_wm = VmWm_Create(800, 600);
    VmDisplay_Init(&g_frame);
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

    printf("WM demo on http://127.0.0.1:%u/ (two windows, focus + drag)\n", port);
    fflush(stdout);
    int result = VmServer_Run(server);
    VmServer_Free(server);
    free(g_frameJson);
    free(g_resourcesJson);
    VmWm_Free(g_wm);
    return result;
}
