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
/* 资源包内容：按 id 返回命令数组 JSON，供 GET /respack/<id> 直接返回。
   id 不属于该 provider 时返回 NULL（404）。 */
typedef const char *(*VmResPackProvider)(const char *resPackId, void *userData);
/* Latest full frame JSON sent as an SSE 'fullframe' event on each new connection,
   so a late client can establish a baseline before incremental frames arrive. */
typedef const char *(*VmFullFrameProvider)(void *userData);
/* Periodic tick for animations, caret blinking and pointer expiry.
   Runs on the server loop thread; do not block. */
typedef void (*VmTickHandler)(void *userData);

VmServer *VmServer_Create(unsigned short port, const char *webRoot);
void VmServer_Free(VmServer *server);
void VmServer_SetFrameProvider(VmServer *server, VmFrameProvider provider, void *userData);
void VmServer_SetInputHandler(VmServer *server, VmInputHandler handler, void *userData);
void VmServer_SetResourceProvider(VmServer *server, VmResourceProvider provider, void *userData);
/* 资源包服务：client 收到 loadResPack(id) 后拉 GET /respack/<id>。
   只登记回调，不做缓存；内容由提供方持有并在切换主题时更新。 */
void VmServer_SetResPackProvider(VmServer *server, VmResPackProvider provider, void *userData);
void VmServer_SetFullFrameProvider(VmServer *server, VmFullFrameProvider provider, void *userData);
void VmServer_SetTickHandler(VmServer *server, int intervalMs, VmTickHandler handler, void *userData);
/* 向所有已连接客户端广播一次 resources-update 事件（覆盖 defineStyle/Pattern 缓存）。
   调用方持有 json 字符串；本函数会立即复制，返回后即可释放。
   可从任意线程调用，广播在 server 主循环中完成。 */
void VmServer_BroadcastResources(VmServer *server, const char *json);
/* 请求退出 VmServer_Run（在 tick 或输入回调里调用即可）。 */
void VmServer_Stop(VmServer *server);
int VmServer_Run(VmServer *server); /* blocks until error; returns non-zero on failure */

#ifdef __cplusplus
}
#endif

#endif
