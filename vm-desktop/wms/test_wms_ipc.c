/*
 * test_wms_ipc.c - 协议层往返与边界单测（纯内存，不开任何 socket）
 *
 * 断言覆盖：
 *   - 包头编解码往返、cmd 位域打包
 *   - 0 载荷、变长尾随、一个缓冲里两条消息
 *   - magic 错、body_len 超上限 → PARSE_ERROR
 *   - 半包 → NEED_MORE
 *   - 内容通道的条数/单条尺寸/总长度校验与截断语义
 *
 * 用法：test_wms_ipc [--probe]
 *   --probe  只跑断言，打印一行汇总（供脚本调用）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wms_ipc.h"
#include "wms_buf.h"
#include "wms_codec.h"

static int g_checks = 0;
static int g_fail   = 0;

static void check(int cond, const char* what)
{
    g_checks++;
    if (!cond) {
        g_fail++;
        printf("[FAIL] %s\n", what);
    }
}

/* 手工塞一个原始包（用来构造非法帧） */
static void raw_push(VmWmBuf* b, uint32_t magic, uint16_t cmd, uint16_t seq,
                     uint32_t body_len, const void* body, uint32_t real_len)
{
    VmWmMsgHdr h;
    h.magic    = magic;
    h.cmd      = cmd;
    h.seq      = seq;
    h.body_len = body_len;
    h.reserved = 0;
    vmwm_buf_append(b, &h, sizeof(h));
    if (real_len && body) vmwm_buf_append(b, body, real_len);
}

/* 造一条 CMD_WINDOW_CONTENT 帧：包头 body_len 必须覆盖「定长头 + 命令数组」，
 * 否则追加进去的命令字节在解析视角下不属于这条消息。 */
static void push_content_frame(VmWmBuf* b, const VmWmReqContent* hdr,
                               const void* cmds, uint32_t cmds_len)
{
    uint32_t body_len = (uint32_t)sizeof(*hdr) + cmds_len;
    raw_push(b, VMWM_MAGIC, VmWm_MakeCmd(VMWM_CMD_TYPE_REQ, CMD_WINDOW_CONTENT),
             9, body_len, NULL, 0);
    vmwm_buf_append(b, hdr, sizeof(*hdr));
    if (cmds_len) vmwm_buf_append(b, cmds, cmds_len);
}

int main(int argc, char** argv)
{
    int probe = 0;
    int i;

    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--probe") == 0) probe = 1;

    /* 1. cmd 位域 */
    {
        check(VmWm_MakeCmd(VMWM_CMD_TYPE_MSG, CMD_INPUT) ==
              (uint16_t)((3u << 14) | 17u), "MakeCmd(MSG, INPUT)");
        check(VmWm_CmdType(VmWm_MakeCmd(VMWM_CMD_TYPE_EVT, EVT_FRAME)) ==
              VMWM_CMD_TYPE_EVT, "CmdType(EVT)");
        check(VmWm_CmdId(VmWm_MakeCmd(VMWM_CMD_TYPE_ACK, 0)) == 0,
              "CmdId(ACK)=0");
        {
            uint16_t types[] = { VMWM_CMD_TYPE_REQ, VMWM_CMD_TYPE_ACK,
                                 VMWM_CMD_TYPE_EVT, VMWM_CMD_TYPE_MSG };
            uint16_t ids[]   = { 0, 1, 16, 22, 500 };
            unsigned a, b;
            int ok = 1;
            for (a = 0; a < 4; a++)
                for (b = 0; b < 5; b++) {
                    uint16_t c = VmWm_MakeCmd(types[a], ids[b]);
                    if (VmWm_CmdType(c) != types[a] || VmWm_CmdId(c) != ids[b])
                        ok = 0;
                }
            check(ok, "cmd roundtrip over type/id matrix");
        }
    }

    /* 2. 普通往返 */
    {
        VmWmBuf  buf;
        VmWmMsg  m;
        VmWmReqHello hello;

        vmwm_buf_init(&buf);
        memset(&hello, 0, sizeof(hello));
        hello.version     = VMWM_VERSION;
        hello.client_type = VMWM_CLIENT_RDP;

        check(vmwm_msg_pack(&buf, VMWM_CMD_TYPE_REQ, CMD_HANDSHAKE, 7,
                            &hello, sizeof(hello)) == VMWM_OK, "pack handshake");
        check(vmwm_msg_parse(&buf, &m) == VMWM_PARSE_OK, "parse handshake");
        check(m.type == VMWM_CMD_TYPE_REQ, "parsed type");
        check(m.id == CMD_HANDSHAKE, "parsed id");
        check(m.hdr.seq == 7, "parsed seq");
        check(m.body_len == sizeof(hello), "parsed body_len");
        check(m.total == sizeof(VmWmMsgHdr) + sizeof(hello), "parsed total");
        check(memcmp(m.body, &hello, sizeof(hello)) == 0, "parsed body bytes");
        vmwm_buf_free(&buf);
    }

    /* 3. 零载荷 */
    {
        VmWmBuf buf;
        VmWmMsg m;
        vmwm_buf_init(&buf);
        check(vmwm_msg_pack(&buf, VMWM_CMD_TYPE_REQ, CMD_LIST_WINDOWS, 1,
                            NULL, 0) == VMWM_OK, "pack empty body");
        check(vmwm_msg_parse(&buf, &m) == VMWM_PARSE_OK, "parse empty body");
        check(m.body_len == 0, "empty body_len");
        vmwm_buf_free(&buf);
    }

    /* 4. 半包 */
    {
        VmWmBuf buf;
        VmWmMsg m;
        VmWmReqWinId q;
        vmwm_buf_init(&buf);
        q.win_id = 42;
        vmwm_msg_pack(&buf, VMWM_CMD_TYPE_REQ, CMD_CLOSE_WINDOW, 2, &q, sizeof(q));
        buf.len -= 3;                        /* 人为砍掉尾巴 */
        check(vmwm_msg_parse(&buf, &m) == VMWM_PARSE_NEED_MORE, "half packet");
        buf.len += 3;
        check(vmwm_msg_parse(&buf, &m) == VMWM_PARSE_OK, "half packet completes");
        vmwm_buf_free(&buf);
    }

    /* 5. 非法 magic / 超上限 body_len */
    {
        VmWmBuf buf;
        VmWmMsg m;
        vmwm_buf_init(&buf);
        raw_push(&buf, 0xDEADBEEFu, VmWm_MakeCmd(VMWM_CMD_TYPE_REQ, 1), 0,
                 0, NULL, 0);
        check(vmwm_msg_parse(&buf, &m) == VMWM_PARSE_ERROR, "bad magic rejected");
        vmwm_buf_free(&buf);

        vmwm_buf_init(&buf);
        raw_push(&buf, VMWM_MAGIC, VmWm_MakeCmd(VMWM_CMD_TYPE_REQ, 1), 0,
                 VMWM_MAX_PAYLOAD + 1, NULL, 0);
        check(vmwm_msg_parse(&buf, &m) == VMWM_PARSE_ERROR, "oversize body_len rejected");
        vmwm_buf_free(&buf);
    }

    /* 6. 一个缓冲里两条消息，按序消费 */
    {
        VmWmBuf buf;
        VmWmMsg a, b;
        VmWmReqWinId q;
        vmwm_buf_init(&buf);
        q.win_id = 100;
        vmwm_msg_pack(&buf, VMWM_CMD_TYPE_REQ, CMD_MAP_WINDOW, 3, &q, sizeof(q));
        vmwm_msg_pack(&buf, VMWM_CMD_TYPE_REQ, CMD_UNMAP_WINDOW, 4, &q, sizeof(q));

        check(vmwm_msg_parse(&buf, &a) == VMWM_PARSE_OK, "stream msg #1");
        check(a.hdr.seq == 3, "stream msg #1 seq");
        vmwm_buf_consume(&buf, a.total);
        check(vmwm_msg_parse(&buf, &b) == VMWM_PARSE_OK, "stream msg #2");
        check(b.hdr.seq == 4, "stream msg #2 seq");
        vmwm_buf_consume(&buf, b.total);
        check(buf.len == 0, "stream fully consumed");
        vmwm_buf_free(&buf);
    }

    /* 7. 内容通道：正常 */
    {
        VmWmBuf        buf;
        VmWmMsg        m;
        VmWmReqContent hdr;
        uint8_t        cmds[3 * 496];
        uint32_t       count = 0, size = 0;
        int            ovf = 0;

        memset(cmds, 0x5A, sizeof(cmds));
        vmwm_buf_init(&buf);

        memset(&hdr, 0, sizeof(hdr));
        hdr.win_id   = 7;
        hdr.epoch    = 3;
        hdr.count    = 3;
        hdr.cmd_size = 496;

        push_content_frame(&buf, &hdr, cmds, (uint32_t)sizeof(cmds));

        check(vmwm_msg_parse(&buf, &m) == VMWM_PARSE_OK, "content pack/parse");
        check(vmwm_content_validate(&m, &count, &size, &ovf) == VMWM_OK,
              "content validate ok");
        check(count == 3 && size == 496 && ovf == 0, "content count/size/overflow");
        vmwm_buf_free(&buf);
    }

    /* 8. 内容通道：截断（客户端少发了 2 条） */
    {
        VmWmBuf        buf;
        VmWmMsg        m;
        VmWmReqContent hdr;
        uint8_t        cmds[1 * 128];
        uint32_t       count = 0, size = 0;
        int            ovf = 0;

        memset(cmds, 0x11, sizeof(cmds));
        vmwm_buf_init(&buf);

        memset(&hdr, 0, sizeof(hdr));
        hdr.win_id   = 7;
        hdr.epoch    = 3;
        hdr.count    = 3;          /* 声称 3 条 */
        hdr.cmd_size = 128;

        push_content_frame(&buf, &hdr, cmds, (uint32_t)sizeof(cmds));  /* 只带了 1 条 */

        check(vmwm_msg_parse(&buf, &m) == VMWM_PARSE_OK, "truncated content parse");
        check(vmwm_content_validate(&m, &count, &size, &ovf) == VMWM_OK,
              "truncated content validate");
        check(count == 1, "truncated content count clamped");
        check(ovf == 1, "truncated content overflow flag");
        vmwm_buf_free(&buf);
    }

    /* 9. 内容通道：非法形状 */
    {
        VmWmBuf        buf;
        VmWmMsg        m;
        VmWmReqContent hdr;
        uint32_t       count = 0, size = 0;
        int            ovf = 0;
        uint8_t        pad[16];

        memset(pad, 0, sizeof(pad));

        /* cmd_size = 0 */
        vmwm_buf_init(&buf);
        memset(&hdr, 0, sizeof(hdr));
        hdr.count = 1; hdr.cmd_size = 0;
        vmwm_msg_pack(&buf, VMWM_CMD_TYPE_REQ, CMD_WINDOW_CONTENT, 1,
                      &hdr, sizeof(hdr));
        vmwm_msg_parse(&buf, &m);
        check(vmwm_content_validate(&m, &count, &size, &ovf) == VMWM_ERR_PROTOCOL,
              "cmd_size=0 rejected");
        vmwm_buf_free(&buf);

        /* count 超配额 */
        vmwm_buf_init(&buf);
        memset(&hdr, 0, sizeof(hdr));
        hdr.count = VMWM_CONTENT_MAX_COMMANDS + 1;
        hdr.cmd_size = 8;
        vmwm_msg_pack(&buf, VMWM_CMD_TYPE_REQ, CMD_WINDOW_CONTENT, 1,
                      &hdr, sizeof(hdr));
        vmwm_msg_parse(&buf, &m);
        check(vmwm_content_validate(&m, &count, &size, &ovf) == VMWM_ERR_PROTOCOL,
              "count over quota rejected");
        vmwm_buf_free(&buf);

        /* 尾部多余字节 */
        vmwm_buf_init(&buf);
        memset(&hdr, 0, sizeof(hdr));
        hdr.count = 1;
        hdr.cmd_size = 8;
        push_content_frame(&buf, &hdr, pad, (uint32_t)sizeof(pad));
        vmwm_msg_parse(&buf, &m);
        check(vmwm_content_validate(&m, &count, &size, &ovf) == VMWM_ERR_PROTOCOL,
              "trailing bytes rejected");
        vmwm_buf_free(&buf);
    }

    /* 10. 缓冲上限：append 超过 1 MB 必须失败，而不是继续翻倍到 OOM */
    {
        VmWmBuf  b;
        uint8_t* big = (uint8_t*)malloc(VMWM_BUF_MAX + 1);
        if (!big) {
            check(0, "malloc for buffer-limit test");
        } else {
            memset(big, 0, VMWM_BUF_MAX + 1);
            vmwm_buf_init(&b);
            check(vmwm_buf_append(&b, big, VMWM_BUF_MAX) == 1,
                  "buf accepts exactly max");
            check(vmwm_buf_append(&b, big, 1) == 0, "buf rejects max+1");
            vmwm_buf_free(&b);
            free(big);
        }
    }

    if (probe) {
        printf("test_wms_ipc: %d checks, %d failed\n", g_checks, g_fail);
    } else {
        printf("== test_wms_ipc ==\n");
        printf("checks=%d failed=%d\n", g_checks, g_fail);
        if (!g_fail) printf("ALL PASS\n");
    }
    return g_fail ? 1 : 0;
}
