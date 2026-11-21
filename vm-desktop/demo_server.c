/* C HTTP/SSE demo: pushes Clay frames to /events, accepts raw input on POST /input.
   Build: gcc -std=c11 -I<clay-dir> -I. demo_server.c demo_layout.c clay_frame_adapter.c vm_server.c -o demo_server
   Run:   ./demo_server 9000 ../ConsoleApp2 */
#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "clay_frame_adapter.h"
#include "demo_layout.h"
#include "vm_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static ClayFrameBuffer *g_frame;
static unsigned int g_frameId;

static const char *ResolveFont(uint16_t fontId) {
    return fontId == 1 ? "Microsoft YaHei UI, sans-serif" : "sans-serif";
}

static const char *FrameProvider(void *userData, unsigned int *frameId) {
    (void)userData;
    Clay_RenderCommandArray commands = DemoLayout_Build();
    ClayFrame_Build(g_frame, g_frameId, commands.internalArray, commands.length, ResolveFont, NULL);
    *frameId = g_frameId;
    g_frameId++;
    return ClayFrameBuffer_Data(g_frame);
}

static void InputHandler(const char *body, size_t length, void *userData) {
    (void)userData;
    printf("[input] %.*s\n", (int)length, body);
    fflush(stdout);
}

int main(int argc, char **argv) {
    unsigned short port = argc > 1 ? (unsigned short)atoi(argv[1]) : 9000;
    const char *webRoot = argc > 2 ? argv[2] : "../assets";

    DemoLayout_Init();
    g_frame = ClayFrameBuffer_Create();
    if (!g_frame) return 1;

    VmServer *server = VmServer_Create(port, webRoot);
    if (!server) {
        fprintf(stderr, "failed to bind port %u\n", port);
        ClayFrameBuffer_Free(g_frame);
        return 1;
    }
    VmServer_SetFrameProvider(server, FrameProvider, NULL);
    VmServer_SetInputHandler(server, InputHandler, NULL);

    printf("VM server on http://127.0.0.1:%u/ (SSE /events, POST /input)\n", port);
    fflush(stdout);
    int result = VmServer_Run(server);

    VmServer_Free(server);
    ClayFrameBuffer_Free(g_frame);
    return result;
}
