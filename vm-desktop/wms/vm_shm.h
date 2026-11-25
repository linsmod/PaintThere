#ifndef VM_SHM_H
#define VM_SHM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VM_SHM_MAGIC      0x4D485356u  /* 'VSHM' */
#define VM_SHM_VERSION    1u
#define VM_SHM_SLOTS      8
#define VM_SHM_SLOT_SIZE  (256u * 1024u)

typedef struct {
    uint64_t seq;       /* even = ready, odd = writing */
    uint32_t frame_id;
    uint32_t len;
} VmShmSlotHeader;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t slotCount;
    uint32_t slotSize;
    uint32_t reserved[4];
} VmShmHeader;

typedef struct VmShm VmShm;

/* Derive the shared-memory object name from the pipe path. Both ends call this. */
void VmShm_NameFromPipe(const char *pipePath, char *out, size_t cap);

/* Create (writer) / open (reader) the shared memory region. */
VmShm *VmShm_Create(const char *pipePath);   /* returns NULL on error */
VmShm *VmShm_Open(const char *pipePath);     /* returns NULL if writer not ready */
void VmShm_Destroy(VmShm *shm);

/* Writer: round-robin slots, seqlock protected. Returns slot index or -1. */
int VmShm_WriteFrame(VmShm *shm, uint32_t frameId, const void *data, size_t len,
                     uint64_t *outSeq);
/* Reader: stable-read a slot into caller buffer. Returns bytes copied, or -1. */
int VmShm_ReadFrame(VmShm *shm, int slot, uint64_t expectSeq,
                    void *outBuf, size_t outCap, uint32_t *outFrameId);
/* Direct read-only payload pointer for zero-copy consumption; valid until next write.
   Returns NULL on error. */
const uint8_t *VmShm_MapPayload(VmShm *shm, int slot, uint64_t expectSeq,
                                size_t *outLen, uint32_t *outFrameId);

#ifdef __cplusplus
}
#endif

#endif
