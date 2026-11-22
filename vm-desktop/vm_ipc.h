#ifndef VM_IPC_H
#define VM_IPC_H

/* WMS ↔ 应用进程 的线路协议。
 *
 * 传输：TCP 127.0.0.1，WMS 监听（默认主端口 + 1）。app 启动后主动连回来，
 *       因此 WMS 不需要知道怎么启动程序，启动器只负责 spawn + 传端口。
 * 帧：VmIpcHeader + payload。payload 全部是定长 POD（内联数组，无指针），
 *     可以直接 memcpy；变长部分（命令数组、进程表）紧跟定长头之后。
 * 所有权：连接断开即该 app 的全部窗口失效，WMS 负责清理。
 */

#include <stdint.h>

#include "vm_display.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VM_IPC_MAGIC 0x4B534D56u        /* "VMSK" */
#define VM_IPC_MAX_APPS 16
#define VM_IPC_MAX_WINDOWS_PER_APP 8
#define VM_IPC_MAX_TITLE 64
#define VM_IPC_MAX_APPID 32
#define VM_IPC_MAX_NAME 64
#define VM_IPC_MAX_KEY 32
/* 单次内容回包的命令上限，与窗口层缓冲容量对齐。 */
#define VM_IPC_MAX_CONTENT_COMMANDS VM_WM_LAYER_CAPACITY
#define VM_IPC_MAX_PROC_ROWS (VM_IPC_MAX_APPS + 1)

/* 布局请求发出后等待回包的帧数；超时标记「无响应」，继续用旧内容合成。 */
#define VM_IPC_LAYOUT_TIMEOUT_FRAMES 30

typedef struct {
    uint32_t magic;   /* VM_IPC_MAGIC */
    uint16_t type;    /* VmIpcMsgType */
    uint16_t flags;   /* 保留，必须为 0 */
    uint32_t len;     /* payload 字节数（不含本头） */
} VmIpcHeader;        /* 12 B */

typedef enum {
    /* ---- app → wms ---- */
    VM_IPC_HELLO = 1,          /* VmIpcHello */
    VM_IPC_WINDOW_CREATE,      /* VmIpcWindowCreate */
    VM_IPC_WINDOW_DESTROY,     /* VmIpcWindowId */
    VM_IPC_WINDOW_RETITLE,     /* VmIpcWindowRetitle */
    VM_IPC_WINDOW_CONTENT,     /* VmIpcWindowContent + VmDrawCommand[count] */
    VM_IPC_INVALIDATE,         /* VmIpcWindowId：请求重跑该窗口的 layout */
    VM_IPC_PROC_LIST_REQ,      /* 空 payload：要进程表（ProcMan） */
    VM_IPC_PROC_KILL_REQ,      /* VmIpcWindowId.appHandle 复用为 appHandle */
    VM_IPC_BYE,                /* 空 payload：正常退出 */

    /* ---- wms → app ---- */
    VM_IPC_HELLO_ACK = 64,     /* VmIpcHelloAck */
    VM_IPC_LAYOUT_REQ,         /* VmIpcLayoutReq */
    VM_IPC_INPUT,              /* VmIpcInput */
    VM_IPC_WINDOW_CLOSED,      /* VmIpcWindowId：WMS 已移除，app 不要再用该 id */
    VM_IPC_PROC_LIST_REPLY,    /* VmIpcProcListReply + VmIpcProcRow[count] */
    VM_IPC_SHUTDOWN            /* 空 payload：WMS 正在退出 */
} VmIpcMsgType;

/* 窗口 id 复用：只带一个 id 的消息用它。 */
typedef struct {
    int32_t winId;
    int32_t appHandle;   /* PROC_KILL_REQ 时作为目标 appHandle，其余为 0 */
} VmIpcWindowId;

typedef struct {
    char appId[VM_IPC_MAX_APPID];
    char name[VM_IPC_MAX_NAME];
    int32_t pid;
    int32_t reserved;
} VmIpcHello;

typedef struct {
    int32_t appHandle;   /* 由 WMS 分配，后续消息里可省略 */
    int32_t screenW, screenH;
    int32_t layoutTimeoutFrames;
} VmIpcHelloAck;

typedef struct {
    int32_t winId;
    char title[VM_IPC_MAX_TITLE];
    int32_t x, y, w, h;
    uint32_t flags;      /* VM_WIN_* 同一套位；WMS 侧仍可强制加 OWNED 限定位 */
} VmIpcWindowCreate;

typedef struct {
    int32_t winId;
    char title[VM_IPC_MAX_TITLE];
} VmIpcWindowRetitle;

/* 布局请求：app 用这组尺寸跑 Clay；epoch 单调递增，回包必须原样带回，
   WMS 用它丢弃过期内容（快速 resize 时旧尺寸的回包会晚到）。 */
typedef struct {
    int32_t winId;
    int32_t width, height;    /* body 尺寸，已去掉装饰 */
    uint32_t epoch;
} VmIpcLayoutReq;

/* 内容回包：紧跟 VmDrawCommand[count]。坐标必须是画布绝对坐标，
   与 WMS 的 layer.commands 语义一致（装饰由 WMS 自己生成）。 */
typedef struct {
    int32_t winId;
    uint32_t epoch;
    int32_t count;
    int32_t overflow;    /* 1 = app 侧内容被截断，WMS 记日志 */
} VmIpcWindowContent;

typedef enum {
    VM_IPC_INPUT_MOVE = 0,
    VM_IPC_INPUT_DOWN,
    VM_IPC_INPUT_UP,
    VM_IPC_INPUT_WHEEL,
    VM_IPC_INPUT_KEY
} VmIpcInputKind;

typedef struct {
    int32_t winId;
    uint16_t kind;       /* VmIpcInputKind */
    uint16_t isDown;
    int32_t x, y;        /* 窗口局部坐标；MOVE/DOWN/UP/WHEEL 有效 */
    int32_t button;      /* 0 左 / 1 中 / 2 右 */
    int32_t deltaY;      /* WHEEL */
    uint32_t mods;       /* bit0 ctrl, bit1 shift, bit2 alt */
    char key[VM_IPC_MAX_KEY];
    char code[VM_IPC_MAX_KEY];
} VmIpcInput;

typedef enum {
    VM_IPC_PROC_RUNNING = 0,
    VM_IPC_PROC_UNRESPONSIVE = 1,   /* 布局超时 */
    VM_IPC_PROC_EXITING = 2
} VmIpcProcState;

typedef struct {
    int32_t appHandle;
    int32_t pid;
    int32_t winCount;
    int32_t memKB;
    uint32_t cpuPermille;           /* 千分比；拿不到时填 0 */
    uint32_t state;                 /* VmIpcProcState */
    char appId[VM_IPC_MAX_APPID];
    char name[VM_IPC_MAX_NAME];
} VmIpcProcRow;

typedef struct {
    int32_t count;
    int32_t wmsUptimeMs;
} VmIpcProcListReply;

/* 编译期守护：协议结构必须是定长 POD，否则 memcpy 与跨进程都不成立。
   指针会被打包成 4/8 字节的垃圾发到对端，症状是对方崩在随机地址。 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(VmIpcHeader) == 12, "VmIpcHeader must stay 12 bytes");
_Static_assert(sizeof(VmIpcHello) == 104, "VmIpcHello layout changed");
#endif

#ifdef __cplusplus
}
#endif

#endif
