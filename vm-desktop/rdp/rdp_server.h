/*
 * rdp_server.h - 呈现端 HTTP/SSE 服务（libuv）
 *
 * 分工（见 docs/task-vm-wms-merge.md §3.1 M4）：
 *   - 本模块只管浏览器一侧：HTTP 路由、SSE 下行、静态资源、respack 容器。
 *   - 桌面语义（窗口表、帧生成、资源内容）全部来自 vm_wms，由 main.c 通过
 *     PushFrame / PushFullframe / SetRespack 注入。
 *   - 不持窗口表、不做合成、不切瓦片。
 *
 * 数据方向：
 *   上行：POST /input 原始 JSON -> inputHandler（main 转成 VmWmReqInput）
 *   下行：WMS 事件 -> PushFrame / PushFullframe / SetRespack -> SSE / HTTP
 *
 * 新 SSE 客户端一律等一次新的 fullframe：缓存的旧 fullframe 与「连接建立前
 * 已广播的增量帧」之间有空洞，直接复用会缺块。BeginFullframe 告诉 main
 * 何时该向上游请求。
 */
#ifndef RDP_SERVER_H
#define RDP_SERVER_H

#include <stddef.h>

struct uv_loop_s;

typedef struct RdpServer RdpServer;

/* 前端输入体（HTTP POST /input 的原始 JSON，未解析）。仅在回调期间有效。 */
typedef void (*RdpInputHandler)(void *user, const char *body, size_t len);

RdpServer *RdpServer_Create(struct uv_loop_s *loop, unsigned short port,
                            const char *webRoot, const char *fontPath);
void RdpServer_Free(RdpServer *s);

void RdpServer_Stop(RdpServer *s);
int  RdpServer_Stopped(const RdpServer *s);

void RdpServer_SetInputHandler(RdpServer *s, RdpInputHandler handler, void *user);

/* 下行：JSON 只在本调用内有效，模块内部按 len 拷贝（帧 JSON 不以 '\0' 结尾）。 */
void RdpServer_PushFrame(RdpServer *s, const char *json, size_t len);
void RdpServer_PushFullframe(RdpServer *s, const char *json, size_t len);

/* 资源包：缓存供 GET /respack/<id>，并向已连接前端广播 loadResPack。
   主题热更新时用同一 id 覆盖。 */
void RdpServer_SetRespack(RdpServer *s, const char *id, const char *json, size_t len);

/* 有 SSE 客户端在等基线、且当前没有未完成的请求时返回 1（main 据此请求 fullframe）。 */
int  RdpServer_BeginFullframe(RdpServer *s);
/* 请求失败（wms 未就绪等）：清掉进行中标记，下一轮重试。 */
void RdpServer_CancelFullframe(RdpServer *s);

#endif /* RDP_SERVER_H */
