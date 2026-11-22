/* Minimal single-window demo: declare a Clay layout, convert it to a VM frame.
   Build: gcc -std=c11 -I<clay-dir> -I. demo_frame.c demo_layout.c clay_frame_adapter.c vm_display.c vm_serialize_json.c -o demo_frame */
#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "clay_frame_adapter.h"
#include "vm_display.h"
#include "vm_serialize_json.h"
#include "demo_layout.h"

#include <stdio.h>
#include <stdlib.h>

static const char *ResolveFont(uint16_t fontId) {
    return fontId == 1 ? "Microsoft YaHei UI, sans-serif" : "sans-serif";
}

int main(void) {
    DemoLayout_Init();
    Clay_RenderCommandArray commands = DemoLayout_Build();

    static VmDisplayList frame;
    VmDisplay_Init(&frame);
    frame.frameId = 1;
    ClayFrame_AppendCommands(&frame, commands.internalArray, commands.length, 0, 0,
                             ResolveFont, NULL);
    char *json = VmSerialize_FrameJson(&frame);
    printf("%s\n", json ? json : "{}");
    free(json);
    return 0;
}
