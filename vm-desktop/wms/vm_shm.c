#include "vm_shm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct VmShm {
    unsigned char *base;
    size_t total;
    int isWriter;
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
};

void VmShm_NameFromPipe(const char *pipePath, char *out, size_t cap) {
    uint32_t h = 2166136261u;
    if (pipePath) {
        for (const unsigned char *p = (const unsigned char *)pipePath; *p; ++p) {
            h ^= *p;
            h *= 16777619u;
        }
    }
    snprintf(out, cap, "vmwm_shm_%08x", h);
}

static size_t slot_stride(void) { return sizeof(VmShmSlotHeader) + VM_SHM_SLOT_SIZE; }
static size_t total_size(void) { return sizeof(VmShmHeader) + VM_SHM_SLOTS * slot_stride(); }

static unsigned char *slot_ptr(VmShm *shm, int slot) {
    if (!shm || !shm->base || slot < 0 || slot >= VM_SHM_SLOTS) return NULL;
    return shm->base + sizeof(VmShmHeader) + (size_t)slot * slot_stride();
}

VmShm *VmShm_Create(const char *pipePath) {
    char name[128];
    VmShm_NameFromPipe(pipePath, name, sizeof(name));
    size_t total = total_size();
    VmShm *shm = (VmShm *)calloc(1, sizeof(*shm));
    if (!shm) return NULL;
    shm->total = total;
    shm->isWriter = 1;

#ifdef _WIN32
    shm->handle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    0, (DWORD)total, name);
    if (!shm->handle) { free(shm); return NULL; }
    shm->base = (unsigned char *)MapViewOfFile(shm->handle, FILE_MAP_ALL_ACCESS, 0, 0, total);
    if (!shm->base) { CloseHandle(shm->handle); free(shm); return NULL; }
#else
    shm->fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (shm->fd < 0) { free(shm); return NULL; }
    if (ftruncate(shm->fd, (off_t)total) != 0) { close(shm->fd); free(shm); return NULL; }
    shm->base = (unsigned char *)mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED,
                                      shm->fd, 0);
    if (shm->base == MAP_FAILED) { close(shm->fd); free(shm); return NULL; }
#endif

    VmShmHeader *h = (VmShmHeader *)shm->base;
    memset(h, 0, total);
    h->magic = VM_SHM_MAGIC;
    h->version = VM_SHM_VERSION;
    h->slotCount = VM_SHM_SLOTS;
    h->slotSize = VM_SHM_SLOT_SIZE;
    return shm;
}

VmShm *VmShm_Open(const char *pipePath) {
    char name[128];
    VmShm_NameFromPipe(pipePath, name, sizeof(name));
    size_t total = total_size();
    VmShm *shm = (VmShm *)calloc(1, sizeof(*shm));
    if (!shm) return NULL;
    shm->total = total;

#ifdef _WIN32
    shm->handle = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    if (!shm->handle) { free(shm); return NULL; }
    shm->base = (unsigned char *)MapViewOfFile(shm->handle, FILE_MAP_READ, 0, 0, total);
    if (!shm->base) { CloseHandle(shm->handle); free(shm); return NULL; }
#else
    shm->fd = shm_open(name, O_RDONLY, 0);
    if (shm->fd < 0) { free(shm); return NULL; }
    shm->base = (unsigned char *)mmap(NULL, total, PROT_READ, MAP_SHARED, shm->fd, 0);
    if (shm->base == MAP_FAILED) { close(shm->fd); free(shm); return NULL; }
#endif

    VmShmHeader *h = (VmShmHeader *)shm->base;
    if (h->magic != VM_SHM_MAGIC || h->version != VM_SHM_VERSION ||
        h->slotCount != VM_SHM_SLOTS || h->slotSize != VM_SHM_SLOT_SIZE) {
        VmShm_Destroy(shm);
        return NULL;
    }
    return shm;
}

void VmShm_Destroy(VmShm *shm) {
    if (!shm) return;
#ifdef _WIN32
    if (shm->base) UnmapViewOfFile(shm->base);
    if (shm->handle) CloseHandle(shm->handle);
#else
    if (shm->base && shm->base != MAP_FAILED) munmap(shm->base, shm->total);
    if (shm->fd >= 0) close(shm->fd);
#endif
    free(shm);
}

int VmShm_WriteFrame(VmShm *shm, uint32_t frameId, const void *data, size_t len,
                     uint64_t *outSeq) {
    if (!shm || !shm->isWriter || len > VM_SHM_SLOT_SIZE) return -1;
    static int nextSlot;
    int slot = nextSlot++ % VM_SHM_SLOTS;
    unsigned char *sp = slot_ptr(shm, slot);
    if (!sp) return -1;
    VmShmSlotHeader *hdr = (VmShmSlotHeader *)sp;
    uint64_t seq = __atomic_load_n(&hdr->seq, __ATOMIC_ACQUIRE);
    if (seq & 1) seq++; /* skip an in-progress generation */
    __atomic_store_n(&hdr->seq, seq + 1, __ATOMIC_RELEASE); /* odd = writing */
    hdr->frame_id = frameId;
    hdr->len = (uint32_t)len;
    memcpy(sp + sizeof(VmShmSlotHeader), data, len);
    __atomic_store_n(&hdr->seq, seq + 2, __ATOMIC_RELEASE); /* even = ready */
    if (outSeq) *outSeq = seq + 2;
    return slot;
}

static const uint8_t *read_payload(VmShm *shm, int slot, uint64_t expectSeq,
                                   size_t *outLen, uint32_t *outFrameId) {
    unsigned char *sp = slot_ptr(shm, slot);
    if (!sp) return NULL;
    VmShmSlotHeader *hdr = (VmShmSlotHeader *)sp;
    uint64_t s1 = __atomic_load_n(&hdr->seq, __ATOMIC_ACQUIRE);
    if (s1 & 1) return NULL;
    if (expectSeq && s1 != expectSeq) return NULL;
    uint32_t len = hdr->len;
    uint32_t fid = hdr->frame_id;
    if (len == 0 || len > VM_SHM_SLOT_SIZE) return NULL;
    uint64_t s2 = __atomic_load_n(&hdr->seq, __ATOMIC_ACQUIRE);
    if (s1 != s2) return NULL;
    if (outLen) *outLen = len;
    if (outFrameId) *outFrameId = fid;
    return sp + sizeof(VmShmSlotHeader);
}

int VmShm_ReadFrame(VmShm *shm, int slot, uint64_t expectSeq,
                    void *outBuf, size_t outCap, uint32_t *outFrameId) {
    size_t len = 0;
    const uint8_t *p = read_payload(shm, slot, expectSeq, &len, outFrameId);
    if (!p || len > outCap) return -1;
    memcpy(outBuf, p, len);
    return (int)len;
}

const uint8_t *VmShm_MapPayload(VmShm *shm, int slot, uint64_t expectSeq,
                                size_t *outLen, uint32_t *outFrameId) {
    return read_payload(shm, slot, expectSeq, outLen, outFrameId);
}
