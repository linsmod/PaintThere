#ifndef VM_FRAME_BIN_H
#define VM_FRAME_BIN_H

#include <stdint.h>
#include <stddef.h>

#include "vm_display.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VM_FRAME_BIN_MAGIC   0x4D524642u  /* 'BFRM' little-endian */
#define VM_FRAME_BIN_VERSION 1u

typedef struct VmTiles VmTiles; /* forward decl, defined in vm_serialize_json.h */
typedef struct VmBinWriter VmBinWriter;

typedef enum {
    VM_BIN_OP_CONFIG = 1,
    VM_BIN_OP_SET_TILE,
    VM_BIN_OP_SHOW_LOADING,
    VM_BIN_OP_HIDE_LOADING
} VmBinOp;

/* Incremental binary writer used by VmTiles alongside the JSON buffer. */
VmBinWriter *VmBinWriter_Create(void);
void VmBinWriter_Destroy(VmBinWriter *w);
void VmBinWriter_BeginFrame(VmBinWriter *w, uint32_t frameId);
void VmBinWriter_OpConfig(VmBinWriter *writer, int cols, int rows, int tileW, int tileH, int viewW, int viewH);
void VmBinWriter_OpSetTile(VmBinWriter *w, int index, const VmDrawCommand *cmds, int count);
void VmBinWriter_OpShowLoading(VmBinWriter *w);
void VmBinWriter_OpHideLoading(VmBinWriter *w);
/* Finalize and detach the buffer. Caller owns the returned bytes. */
uint8_t *VmBinWriter_Finish(VmBinWriter *w, size_t *outLen);

/* Decode a binary frame into VmTiles (used by rdp). Returns NULL on error. */
VmTiles *VmFrameBin_ToTiles(const uint8_t *data, size_t len, uint32_t *outFrameId);

#ifdef __cplusplus
}
#endif

#endif
