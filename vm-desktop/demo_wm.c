/* Minimal WM demo: two Clay windows, focus + title-bar drag over SSE.
   Build: gcc -std=c11 -I<clay-dir> -I. demo_wm.c demo_layout.c clay_frame_adapter.c vm_wm.c vm_server.c -lws2_32 -o demo_wm
   Run:   ./demo_wm 9000 ../ConsoleApp2 */
#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "clay_frame_adapter.h"
#include "vm_wm.h"
#include "vm_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VmWm *g_wm;
static ClayFrameBuffer *g_frame;
static unsigned int g_frameId;

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

static Clay_RenderCommandArray LayoutA(void *userData) {
    (void)userData;
    Clay_BeginLayout();
    CLAY(CLAY_ID("A_root"), {
        .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                   .layoutDirection = CLAY_TOP_TO_BOTTOM,
                   .padding = CLAY_PADDING_ALL(12), .childGap = 8},
    }) {
        CLAY_TEXT(CLAY_STRING("Window A - click title to focus/drag"),
                  CLAY_TEXT_CONFIG({.fontId = 0, .fontSize = 14,
                                    .textColor = (Clay_Color){30,30,30,255}}));
        CLAY(CLAY_ID("A_btn"), {
            .layout = {.sizing = {CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(36)}},
            .backgroundColor = (Clay_Color){52,140,220,255},
        }) {}
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

static const char *FrameProvider(void *userData, unsigned int *frameId) {
    (void)userData;
    VmWm_BuildFrame(g_wm, g_frame, g_frameId, ResolveFont);
    *frameId = g_frameId++;
    return ClayFrameBuffer_Data(g_frame);
}

static void InputHandler(const char *body, size_t length, void *userData) {
    (void)userData;
    int changed = VmWm_HandleInput(g_wm, body, length);
    printf("[input] changed=%d focus=%d %.*s\n", changed, g_wm->focusId, (int)length, body);
    fflush(stdout);
}

int main(int argc, char **argv) {
    unsigned short port = argc > 1 ? (unsigned short)atoi(argv[1]) : 9000;
    const char *webRoot = argc > 2 ? argv[2] : "../ConsoleApp2";

    g_wm = VmWm_Create(800, 600);
    g_frame = ClayFrameBuffer_Create();
    if (!g_wm || !g_frame) return 1;

    VmWindow *a = VmWm_AddWindow(g_wm, 1, "Window A", 60, 60, 360, 260, LayoutA, NULL);
    VmWindow *b = VmWm_AddWindow(g_wm, 2, "Window B", 300, 200, 360, 260, LayoutB, NULL);
    a->clayCtx = CreateWindowContext(356, 230);
    b->clayCtx = CreateWindowContext(356, 230);
    VmWm_Focus(g_wm, 1);

    VmServer *server = VmServer_Create(port, webRoot);
    if (!server) {
        fprintf(stderr, "failed to bind port %u\n", port);
        return 1;
    }
    VmServer_SetFrameProvider(server, FrameProvider, NULL);
    VmServer_SetInputHandler(server, InputHandler, NULL);

    printf("WM demo on http://127.0.0.1:%u/ (two windows, focus + drag)\n", port);
    fflush(stdout);
    int result = VmServer_Run(server);
    VmServer_Free(server);
    ClayFrameBuffer_Free(g_frame);
    VmWm_Free(g_wm);
    return result;
}
