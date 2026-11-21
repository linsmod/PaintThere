#ifndef CLAY_FRAME_ADAPTER_H
#define CLAY_FRAME_ADAPTER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque output buffer; caller frees with ClayFrameBuffer_Free. */
typedef struct ClayFrameBuffer ClayFrameBuffer;

ClayFrameBuffer *ClayFrameBuffer_Create(void);
void ClayFrameBuffer_Free(ClayFrameBuffer *buffer);
const char *ClayFrameBuffer_Data(const ClayFrameBuffer *buffer);
size_t ClayFrameBuffer_Length(const ClayFrameBuffer *buffer);
void ClayFrameBuffer_Clear(ClayFrameBuffer *buffer);

/* Build a {type:"frame",frameId,commands:[...]} JSON string from Clay render commands.
   fontResolver maps fontId to a CSS font family; may be NULL (falls back to sans-serif).
   imgResolver maps imageData pointer to an imgId string; may be NULL. */
void ClayFrame_Build(
    ClayFrameBuffer *out,
    uint32_t frameId,
    const void *renderCommands,
    int commandCount,
    const char *(*fontResolver)(uint16_t fontId),
    const char *(*imgResolver)(const void *imageData));

/* Incremental frame building (for composite/Wm use):
     Begin -> AppendCommands (per window, with offset) / AppendRaw -> End */
void ClayFrame_Begin(ClayFrameBuffer *out, uint32_t frameId);
void ClayFrame_AppendCommands(
    ClayFrameBuffer *out,
    const void *renderCommands,
    int commandCount,
    int offsetX,
    int offsetY,
    const char *(*fontResolver)(uint16_t fontId),
    const char *(*imgResolver)(const void *imageData));
/* rawCommand is a complete JSON object without surrounding frame wrapper. */
void ClayFrame_AppendRaw(ClayFrameBuffer *out, const char *rawCommand);
void ClayFrame_End(ClayFrameBuffer *out);

#ifdef __cplusplus
}
#endif

#endif
