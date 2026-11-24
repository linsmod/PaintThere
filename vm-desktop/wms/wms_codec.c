/*
 * vmwm_codec.c - 帧编解码实现（纯函数）
 */
#include <string.h>

#include "wms_codec.h"

VmWmResult vmwm_msg_pack(VmWmBuf* out, uint16_t type, uint16_t id,
                         uint16_t seq, const void* body, uint32_t blen)
{
    VmWmMsgHdr hdr;

    if (!out) return VMWM_ERR_BAD_ARG;
    if (blen > VMWM_MAX_PAYLOAD) return VMWM_ERR_LIMIT;
    if (blen && !body) return VMWM_ERR_BAD_ARG;

    hdr.magic    = VMWM_MAGIC;
    hdr.cmd      = VmWm_MakeCmd(type, id);
    hdr.seq      = seq;
    hdr.body_len = blen;
    hdr.reserved = 0;

    if (!vmwm_buf_append(out, &hdr, sizeof(hdr))) return VMWM_ERR_NOMEM;
    if (blen && !vmwm_buf_append(out, body, blen)) return VMWM_ERR_NOMEM;
    return VMWM_OK;
}

VmWmParse vmwm_msg_parse(const VmWmBuf* buf, VmWmMsg* out)
{
    VmWmMsgHdr hdr;
    size_t     total;

    if (!buf || !out || !buf->data) return VMWM_PARSE_NEED_MORE;
    if (buf->len < sizeof(VmWmMsgHdr)) return VMWM_PARSE_NEED_MORE;

    memcpy(&hdr, buf->data, sizeof(hdr));

    if (hdr.magic != VMWM_MAGIC) return VMWM_PARSE_ERROR;
    if (hdr.body_len > VMWM_MAX_PAYLOAD) return VMWM_PARSE_ERROR;

    total = sizeof(VmWmMsgHdr) + (size_t)hdr.body_len;
    if (buf->len < total) return VMWM_PARSE_NEED_MORE;

    out->hdr      = hdr;
    out->body     = buf->data + sizeof(VmWmMsgHdr);
    out->body_len = hdr.body_len;
    out->total    = (uint32_t)total;
    out->type     = VmWm_CmdType(hdr.cmd);
    out->id       = VmWm_CmdId(hdr.cmd);
    return VMWM_PARSE_OK;
}

VmWmResult vmwm_content_validate(const VmWmMsg* msg, uint32_t* out_count,
                                 uint32_t* out_cmd_size, int* out_overflow)
{
    VmWmReqContent c;
    uint32_t       expected;
    uint32_t       actually_fits;

    if (!msg || !msg->body) return VMWM_ERR_BAD_ARG;
    if (msg->body_len < sizeof(c)) return VMWM_ERR_BAD_ARG;

    memcpy(&c, msg->body, sizeof(c));

    if (c.cmd_size == 0 || c.cmd_size > VMWM_CONTENT_CMD_MAX_SIZE)
        return VMWM_ERR_PROTOCOL;
    if (c.count > VMWM_CONTENT_MAX_COMMANDS)
        return VMWM_ERR_PROTOCOL;

    expected = (uint32_t)(sizeof(c) + (size_t)c.count * c.cmd_size);
    if (msg->body_len > expected)
        return VMWM_ERR_PROTOCOL;               /* 尾部有协议不认识的字节 */

    /* 允许截断：客户端配额满了会少发几条，并把 overflow 置 1 */
    actually_fits = (msg->body_len - (uint32_t)sizeof(c)) / c.cmd_size;
    if (actually_fits < c.count) {
        c.count    = actually_fits;
        c.overflow = 1;
    }

    if (out_count)    *out_count    = c.count;
    if (out_cmd_size) *out_cmd_size = c.cmd_size;
    if (out_overflow) *out_overflow = c.overflow ? 1 : 0;
    return VMWM_OK;
}
