#ifndef CLAY_FRAME_ADAPTER_H
#define CLAY_FRAME_ADAPTER_H

#include "vm_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Convert a Clay render command array into protocol-neutral VmDisplayList entries.
   meterResolver maps fontId to a CSS family (unused by the model, reserved);
   imgResolver maps imageData pointer to an imgId string (may be NULL). */
void ClayFrame_AppendCommands(
    VmCmdBuffer *out,
    const void *renderCommands,
    int commandCount,
    int offsetX,
    int offsetY,
    const char *(*fontResolver)(uint16_t fontId),
    const char *(*imgResolver)(const void *imageData));

#ifdef __cplusplus
}
#endif

#endif
