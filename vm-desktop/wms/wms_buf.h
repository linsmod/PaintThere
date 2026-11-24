/*
 * vmwm_buf.h - 极简动态字节缓冲，用于收发数据重组（处理流式粘包）
 * 仅内部使用，每个 .c 各自编译一份，函数均为 static。
 *
 * v2：所有 append/reserve 都受 VMWM_BUF_MAX 硬上限约束。上限存在的理由是
 * 对端可能报一个伪造的 body_len，无上限的话 malloc 会一路翻倍到 OOM。
 * 调用方在 append 失败时应判定「对端有问题」并断开连接。
 */
#ifndef VMWM_BUF_H
#define VMWM_BUF_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define VMWM_BUF_MAX ((size_t)(1024u * 1024u))   /* 1 MB，与 VMWM_MAX_PAYLOAD 对齐 */

typedef struct {
    uint8_t* data;
    size_t   len;
    size_t   cap;
} VmWmBuf;

static void vmwm_buf_init(VmWmBuf* b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void vmwm_buf_free(VmWmBuf* b)
{
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int vmwm_buf_reserve(VmWmBuf* b, size_t need)
{
    size_t nc;
    uint8_t* p;

    if (need > VMWM_BUF_MAX) return 0;          /* 硬上限 */
    if (b->cap >= need) return 1;

    nc = b->cap ? b->cap : 256;
    while (nc < need) {
        if (nc > VMWM_BUF_MAX / 2) { nc = VMWM_BUF_MAX; break; }
        nc *= 2;
    }

    p = (uint8_t*)realloc(b->data, nc);
    if (!p) return 0;

    b->data = p;
    b->cap = nc;
    return 1;
}

static int vmwm_buf_append(VmWmBuf* b, const void* p, size_t n)
{
    if (n > VMWM_BUF_MAX || b->len > VMWM_BUF_MAX - n) return 0;
    if (!vmwm_buf_reserve(b, b->len + n)) return 0;
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    return 1;
}

/* 消费头部 n 字节，剩余数据前移 */
static void vmwm_buf_consume(VmWmBuf* b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

#endif /* VMWM_BUF_H */
