/* test_shm - shared memory ring sanity check.
   Build: gcc -std=c11 -I. wms/test_shm.c wms/vm_shm.c -o test_shm.exe (add -lrt on POSIX) */
#include "vm_shm.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    const char *pipe = "\\\\.\\pipe\\vmwm_test_shm";
    VmShm *w = VmShm_Create(pipe);
    VmShm *r = VmShm_Open(pipe);
    if (!w || !r) { fprintf(stderr, "create/open failed\n"); return 1; }

    const char *payload = "HELLO SHARED FRAME";
    uint64_t seq = 0;
    int slot = VmShm_WriteFrame(w, 123, payload, strlen(payload) + 1, &seq);
    if (slot < 0) { fprintf(stderr, "write failed\n"); return 1; }

    char buf[128];
    uint32_t fid = 0;
    int n = VmShm_ReadFrame(r, slot, seq, buf, sizeof(buf), &fid);
    int ok = n == (int)strlen(payload) + 1 &&
             fid == 123 && strcmp(buf, payload) == 0;

    size_t len = 0;
    const uint8_t *p = VmShm_MapPayload(r, slot, seq, &len, &fid);
    ok = ok && p && len == strlen(payload) + 1 && memcmp(p, payload, len) == 0;

    printf("shm slot=%d seq=%llu n=%d len=%zu ok=%d\n",
           slot, (unsigned long long)seq, n, len, ok);
    VmShm_Destroy(r);
    VmShm_Destroy(w);
    return ok ? 0 : 1;
}
