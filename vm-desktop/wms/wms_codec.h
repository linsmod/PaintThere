/*
 * vmwm_codec.h - 帧编解码（纯函数，无 socket / 无状态）
 *
 * 单独抽出来的目的是让 test_wms_ipc 能在内存里做往返断言：协议层的 bug
 * 不应该只能靠起三个进程才发现。服务端与客户端共用同一份实现。
 */
#ifndef VMWM_CODEC_H
#define VMWM_CODEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "wms_ipc.h"
#include "wms_buf.h"

/* 解析结果：与 VmWmResult 分开，因为「半包」不是错误 */
typedef enum {
    VMWM_PARSE_ERROR     = -1,   /* magic 错 / body_len 超上限：必须断开 */
    VMWM_PARSE_OK        = 0,    /* 拿到一条完整消息 */
    VMWM_PARSE_NEED_MORE = 1     /* 半包，等更多数据 */
} VmWmParse;

/* 一条完整消息的视图；body 指向缓冲区内部，仅在本次处理内有效 */
typedef struct {
    VmWmMsgHdr     hdr;
    const uint8_t* body;
    uint32_t       body_len;
    uint32_t       total;        /* 头 + 载荷，消费用 */
    uint16_t       type;
    uint16_t       id;
} VmWmMsg;

/* 把一条消息追加到 out（含包头）。body 可为 NULL（blen 为 0 时）。 */
VmWmResult vmwm_msg_pack(VmWmBuf* out, uint16_t type, uint16_t id,
                         uint16_t seq, const void* body, uint32_t blen);

/* 从缓冲区头部解析一条消息，不消费数据。 */
VmWmParse vmwm_msg_parse(const VmWmBuf* buf, VmWmMsg* out);

/* 校验内容通道载荷：条数/单条尺寸/总长度是否自洽。
 * 允许客户端因配额截断，此时 out_count 变小、out_overflow 置 1。 */
VmWmResult vmwm_content_validate(const VmWmMsg* msg, uint32_t* out_count,
                                 uint32_t* out_cmd_size, int* out_overflow);

#ifdef __cplusplus
}
#endif
#endif /* VMWM_CODEC_H */
