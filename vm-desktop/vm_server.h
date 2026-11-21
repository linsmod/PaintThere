#ifndef VM_SERVER_H
#define VM_SERVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VmServer VmServer;

/* Provide the latest frame JSON. Return the same pointer across calls is fine;
   the server copies it before sending. frameId change triggers a broadcast. */
typedef const char *(*VmFrameProvider)(void *userData, unsigned int *frameId);

/* Raw input event body; the server does not interpret its contents. */
typedef void (*VmInputHandler)(const char *body, size_t length, void *userData);
/* One-time resources sent as an SSE 'resources' event on each new connection. */
typedef const char *(*VmResourceProvider)(void *userData);

VmServer *VmServer_Create(unsigned short port, const char *webRoot);
void VmServer_Free(VmServer *server);
void VmServer_SetFrameProvider(VmServer *server, VmFrameProvider provider, void *userData);
void VmServer_SetInputHandler(VmServer *server, VmInputHandler handler, void *userData);
void VmServer_SetResourceProvider(VmServer *server, VmResourceProvider provider, void *userData);
int VmServer_Run(VmServer *server); /* blocks until error; returns non-zero on failure */

#ifdef __cplusplus
}
#endif

#endif
