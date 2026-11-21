#ifndef VM_SERIALIZE_JSON_H
#define VM_SERIALIZE_JSON_H

#include "vm_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Serialize a display list into a {type:"frame",frameId,commands:[...]} JSON string.
   Returns a heap-allocated string the caller must free. */
char *VmSerialize_FrameJson(const VmDisplayList *list);

/* Serialize a command list (without frame envelope) for the 'resources' SSE event. */
char *VmSerialize_CommandArrayJson(const VmDisplayList *list);

#ifdef __cplusplus
}
#endif

#endif
