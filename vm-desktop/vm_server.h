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
/* 资源包内容：按 id 返回命令数组 JSON，供 GET /respack/<id> 打包返回。
   id 不属于该 provider 时返回 NULL（404）。 */
typedef const char *(*VmResPackProvider)(const char *resPackId, void *userData);
/* 资源包里的图片：按 index 从 0 递增枚举，返回 1 表示有效。
   返回的 id/mime/data 只需在本次调用内有效（实现方可用静态缓冲）。
   GET /respack/<id> 一次返回「JSON + 全部图片」的打包容器，见 vm_server.c 顶部格式说明。 */
typedef int (*VmResPackImageAt)(void *userData, const char *resPackId, int index,
                                const char **imgId, const char **mime,
                                const char **data, size_t *size);
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
/* 资源包内的图片：设置后 /respack/<id> 返回打包容器（一次 HTTP 拿全），不设则只返回 JSON。 */
void VmServer_SetResPackImageProvider(VmServer *server, VmResPackImageAt provider,
                                      void *userData);
/* 主题字体：非空时 GET /font.ttf 直接读该路径（可指向系统字体），
   前端与 guest 因此用同一份字体文件，度量与字形一致。 */
void VmServer_SetFontPath(VmServer *server, const char *path);
void VmServer_SetFullFrameProvider(VmServer *server, VmFullFrameProvider provider, void *userData);
void VmServer_SetTickHandler(VmServer *server, int intervalMs, VmTickHandler handler, void *userData);
/* 调试自动化：GET /auto/<op>?query → handler 把 JSON 应答写进 out（超 cap 截断）。
   极简约定：无鉴权、无 JSON 解析，op 与 query 是原始字符串，由 handler 自己分发。
   op 只收 [A-Za-z0-9_-]，query 上限 127 字符。handler 未设置时 /auto/* 返回 404。 */
typedef void (*VmAutoHandler)(void *userData, const char *op, const char *query,
                              char *out, size_t cap);
void VmServer_SetAutoHandler(VmServer *server, VmAutoHandler handler, void *userData);
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
