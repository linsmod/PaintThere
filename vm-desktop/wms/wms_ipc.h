/*
 * vmwm_ipc.h - wmwms 进程间协议定义（服务端 / 客户端共用）· 协议 v2
 *
 * 消息 = 16 字节包头 + 变长载荷，全部小端、按 1 字节对齐。
 * 包头：magic | cmd(type:2bit + id:14bit) | seq | body_len | reserved
 *
 * v2 相对 v1 的变化（对齐上游 docs/task-vm-wms-merge.md §5）：
 *   1. 新增单向消息类型 VMWM_CMD_TYPE_MSG（不发 ACK，用于输入与帧）
 *   2. 新增 VMWM_CLIENT_RDP 角色（呈现端：订阅帧、请求 fullframe、发输入）
 *   3. 新增 5 条命令 + 1 条可选命令（RESPACK）、5 个事件
 *   4. 新增 6 个载荷结构 + VMWM_MAX_PAYLOAD 上限
 *   5. VmWmWindowInfo 补 z / created_seq / focused，供外壳稳定排序
 *   6. 不做 PROCMAN 角色（进程表按当前决策不实现）
 *
 * 坐标系约定（D5）：
 *   CMD_WINDOW_CONTENT 里的命令数组使用**body 局部坐标**（客户端画的就是 body）；
 *   CMD_INPUT 在 win_id == 0 时携带画布绝对坐标，由服务端命中并转局部；
 *   win_id != 0 时 x/y 是窗口内坐标（相对窗口左上角）。
 *   服务端发给属主的 EVT_INPUT **一律是 body 局部坐标**：装饰偏移由服务端按
 *   宿主 body_rect 减掉，客户端不需要知道自己的装饰有多厚。
 *   合成时把 body 内容叠加到窗口上所需的偏移由宿主负责。
 */
#ifndef VMWM_IPC_H
#define VMWM_IPC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* ---------------- 版本与容量 ---------------- */
#define VMWM_MAGIC          0x566D574Du   /* 'VmWM' */
#define VMWM_VERSION        2u

#define VMWM_TITLE_MAX      128
#define VMWM_CLASS_MAX      64
#define VMWM_MAX_WINDOWS    128           /* 含外壳窗口 */
#define VMWM_MAX_CLIENTS    32            /* 含外壳与 rdp */
#define VMWM_IO_TIMEOUT_MS  2000          /* 同步请求默认超时 */
#define VMWM_READ_CHUNK     4096

/* 单条消息载荷上限：挡住错版本与伪造的 body_len，防 OOM。
 * 全帧 JSON 约 48 KB、增量几 KB，1 MB 留足余量。 */
#define VMWM_MAX_PAYLOAD          (1u << 20)

/* 内容通道配额 */
#define VMWM_CONTENT_MAX_COMMANDS 1024u   /* 与主项目 VM_WM_LAYER_CAPACITY 对齐 */
#define VMWM_CONTENT_CMD_MAX_SIZE 512u    /* 单条命令字节上限（VmDrawCommand = 496） */

/* 尺寸变化后等内容的帧数上限；宿主据此标记「未响应」（本层只提供常量） */
#define VMWM_LAYOUT_TIMEOUT_FRAMES 30u

/* 管道名：Windows 命名管道 / POSIX 上的 unix socket 路径 */
#ifdef _WIN32
#define VMWM_PIPE_NAME      "\\\\.\\pipe\\vmwm_pipe"
#else
#define VMWM_PIPE_NAME      "/tmp/vmwm.sock"
#endif

/* ---------------- 结果码 ---------------- */
typedef enum {
    VMWM_OK = 0,
    VMWM_ERR_INVALID_WINDOW = 1,
    VMWM_ERR_PERMISSION     = 2,
    VMWM_ERR_NOMEM          = 3,
    VMWM_ERR_FAILED         = 4,
    VMWM_ERR_BAD_ARG        = 5,
    VMWM_ERR_NOT_CONNECTED  = 6,
    VMWM_ERR_TIMEOUT        = 7,
    VMWM_ERR_LIMIT          = 8,
    VMWM_ERR_PROTOCOL       = 9,
    VMWM_ERR_NOT_READY      = 10           /* 服务端未接入宿主，该能力暂不可用 */
} VmWmResult;

/* ---------------- 客户端类型 ---------------- */
typedef enum {
    VMWM_CLIENT_APP   = 1,   /* 普通应用：只能管理自己创建的窗口 */
    VMWM_CLIENT_SHELL = 2,   /* 桌面外壳：可管理全部窗口、声明工作区与主题 */
    VMWM_CLIENT_RDP   = 3    /* 呈现端：订阅帧、请求 fullframe、转发输入 */
} VmWmClientType;

/* ---------------- 能力位（权限表按角色授予） ---------------- */
typedef enum {
    VMWM_CAP_OWN_WINDOWS    = 1u << 0,   /* 管理自己的窗口 */
    VMWM_CAP_ALL_WINDOWS    = 1u << 1,   /* 管理全部窗口 */
    VMWM_CAP_ENUM_WINDOWS   = 1u << 2,   /* 全量窗口枚举 */
    VMWM_CAP_CONTENT        = 1u << 3,   /* 回推窗口内容 */
    VMWM_CAP_INPUT          = 1u << 4,   /* 送原始输入 */
    VMWM_CAP_WORKAREA       = 1u << 5,   /* 声明工作区 */
    VMWM_CAP_THEME          = 1u << 6,   /* 主题切换 */
    VMWM_CAP_FRAME          = 1u << 7,   /* 订阅帧 / 请求 fullframe */
    VMWM_CAP_RESPACK        = 1u << 8    /* 请求资源包 */
} VmWmCap;

/* ---------------- 堆叠模式 ---------------- */
typedef enum {
    VMWM_STACK_ABOVE  = 0,
    VMWM_STACK_BELOW  = 1,
    VMWM_STACK_TOP    = 2,
    VMWM_STACK_BOTTOM = 3
} VmWmStackMode;

/* ---------------- 消息类型（cmd 字段高 2 位） ---------------- */
#define VMWM_CMD_TYPE_REQ   0u   /* 请求：服务端回 ACK */
#define VMWM_CMD_TYPE_ACK   1u
#define VMWM_CMD_TYPE_EVT   2u   /* 事件：服务端推送 */
#define VMWM_CMD_TYPE_MSG   3u   /* 单向消息：不期待 ACK（输入 / 帧） */
#define VMWM_CMD_MASK       0x3FFFu

/* ---------------- 请求命令 ID ---------------- */
typedef enum {
    /* v1（元数据） */
    CMD_HANDSHAKE      = 0,
    CMD_SUBSCRIBE      = 1,
    CMD_CREATE_WINDOW  = 2,
    CMD_CLOSE_WINDOW   = 3,
    CMD_MOVE_WINDOW    = 4,
    CMD_RESIZE_WINDOW  = 5,
    CMD_CONFIGURE_WIN  = 6,
    CMD_STACK_WINDOW   = 7,
    CMD_SET_FOCUS      = 8,
    CMD_MAP_WINDOW     = 9,
    CMD_UNMAP_WINDOW   = 10,
    CMD_SET_TITLE      = 11,
    CMD_SET_SIZE_HINT  = 12,
    CMD_SET_TRANSIENT  = 13,
    CMD_SET_CLASS      = 14,
    CMD_GET_GEOMETRY   = 15,
    CMD_LIST_WINDOWS   = 16,
    /* v2（内容 / 输入 / 全局状态 / 帧） */
    CMD_INPUT              = 17,   /* MSG，SHELL / RDP */
    CMD_WINDOW_CONTENT     = 18,   /* REQ，APP / SHELL */
    CMD_SET_WORKAREA       = 19,   /* REQ，SHELL */
    CMD_THEME_SET          = 20,   /* REQ，SHELL */
    CMD_REQUEST_FULLFRAME  = 21,   /* REQ，RDP：ACK 体为 fullframe JSON */
    CMD_REQUEST_RESPACK    = 22,   /* REQ，RDP：ACK 体为资源包 JSON（可选扩展） */
    CMD_MAX_ID             = 22
} VmWmCmdId;

/* ---------------- 事件类型（EVT 包的 cmd id 为位值） ---------------- */
typedef enum {
    EVT_WINDOW_NEW       = 1 << 0,
    EVT_WINDOW_CLOSE     = 1 << 1,
    EVT_WINDOW_CONFIGURE = 1 << 2,
    EVT_TITLE_CHANGE     = 1 << 3,
    EVT_FOCUS_CHANGE     = 1 << 4,
    EVT_LAYOUT_REQ       = 1 << 5,   /* APP / SHELL：按新 body 尺寸重跑布局 */
    EVT_INPUT            = 1 << 6,   /* APP / SHELL：窗口局部坐标的输入 */
    EVT_FRAME            = 1 << 7,   /* RDP：tile JSON 增量帧 */
    EVT_FULLFRAME        = 1 << 8,   /* RDP：fullframe JSON 基线 */
    EVT_THEME_CHANGED    = 1 << 9,   /* 全部：主题名，各自重新解析 */
    EVT_RESOURCES        = 1 << 10,  /* RDP：资源包 JSON（CMD_REQUEST_RESPACK 的应答） */
    /* SHELL：全局指针，画布坐标。外壳的窗口不抢焦点，鼠标移出后收不到
       EVT_INPUT，hover 无法清除；这条事件只给 SHELL，与输入路由无关。 */
    EVT_POINTER          = 1 << 11,

    EVT_ALL              = 0xFFF
} VmWmEvent;

/* 窗口行为标志：数值与本项目 VM_WIN_* 对齐，宿主直接赋值。
   由 create 请求携带，服务端据此决定焦点仲裁（NOFOCUS 不抢焦点），
   宿主据此决定装饰与合成层（TOPMOST 分两遍绘制）。 */
typedef enum {
    VMWM_WINFLAG_UNDECORATED = 1u << 0,
    VMWM_WINFLAG_TOPMOST     = 1u << 1,
    VMWM_WINFLAG_PINNED      = 1u << 2,
    VMWM_WINFLAG_NOFOCUS     = 1u << 3
} VmWmWinFlag;

#pragma pack(push, 1)

/* ---------------- 包头（16 字节） ---------------- */
typedef struct {
    uint32_t magic;
    uint16_t cmd;
    uint16_t seq;
    uint32_t body_len;
    uint32_t reserved;
} VmWmMsgHdr;

/* ---------------- 窗口信息（事件载荷 / 列表项） ---------------- */
typedef struct {
    uint64_t win_id;
    int32_t  x, y, w, h;
    uint32_t mapped;       /* 1 = 已映射显示 */
    uint32_t stack;        /* VmWmStackMode */
    uint32_t z;            /* 全局堆叠序：越大越靠前（宿主实现 z 序后可忽略 stack） */
    uint32_t created_seq;  /* 创建序：不随 raise 变化，外壳用它做稳定排序 */
    int32_t  focused;      /* 1 = 当前焦点窗口 */
    uint32_t flags;        /* VmWmWinFlag 位或 */
    char     title[VMWM_TITLE_MAX];
} VmWmWindowInfo;

/* ---------------- 通用应答载荷 ---------------- */
typedef struct {
    int32_t  result;       /* VmWmResult */
    int32_t  reserved;
    uint64_t value;        /* 命令相关返回值，如新建窗口的 win_id */
} VmWmAck;

/* ---------------- 握手应答载荷 ----------------
 * 比通用 ACK 多给三项客户端必需的事实：画布尺寸、工作区、布局超时帧数。 */
typedef struct {
    int32_t  result;
    uint64_t client_id;
    int32_t  screen_w, screen_h;
    int32_t  work_x, work_y, work_w, work_h;
    uint32_t layout_timeout_frames;
    uint32_t reserved;
} VmWmAckHello;

/* ---------------- 各命令请求载荷 ---------------- */
typedef struct {
    uint32_t version;
    uint32_t client_type;  /* VmWmClientType */
} VmWmReqHello;

typedef struct {
    uint32_t mask;         /* VmWmEvent 位或 */
    uint32_t reserved;
} VmWmReqSubscribe;

typedef struct {
    int32_t  x, y, w, h;
    uint32_t flags;        /* VmWmWinFlag 位或；0 = 普通装饰窗口 */
} VmWmReqCreate;

typedef struct {
    uint64_t win_id;
} VmWmReqWinId;

typedef struct {
    uint64_t win_id;
    int32_t  x, y;
} VmWmReqMove;

typedef struct {
    uint64_t win_id;
    int32_t  w, h;
} VmWmReqResize;

typedef struct {
    uint64_t win_id;
    int32_t  x, y, w, h;
} VmWmReqConfigure;

typedef struct {
    uint64_t win_id;
    uint32_t mode;         /* VmWmStackMode */
    uint32_t reserved;
} VmWmReqStack;

typedef struct {
    uint64_t win_id;
    char     title[VMWM_TITLE_MAX];
} VmWmReqTitle;

typedef struct {
    uint64_t win_id;
    int32_t  min_w, min_h;
    int32_t  max_w, max_h;
} VmWmReqSizeHint;

typedef struct {
    uint64_t win_id;
    uint64_t parent_id;
} VmWmReqTransient;

typedef struct {
    uint64_t win_id;
    char     cls[VMWM_CLASS_MAX];
} VmWmReqClass;

/* ---------------- v2 载荷 ---------------- */

/* CMD_INPUT（MSG）与 EVT_INPUT 共用同一布局。
 * kind: 0 move / 1 down / 2 up / 3 wheel / 4 key
 * mods: bit0 ctrl / bit1 shift / bit2 alt
 * 坐标规则见文件头「坐标系约定」。 */
typedef struct {
    uint64_t win_id;       /* 0 = 让服务端命中；非 0 = x/y 已是窗口局部坐标 */
    uint16_t kind;
    uint16_t is_down;
    int32_t  x, y;
    int32_t  button;       /* 0 左 / 1 中 / 2 右 */
    int32_t  delta_y;      /* wheel */
    uint32_t mods;
    char     key[32];
    char     code[32];
} VmWmReqInput;

/* CMD_WINDOW_CONTENT：定长头 + 紧跟 count 条命令，每条 cmd_size 字节。
 * cmd_size 由客户端上报，服务端校验一致性：不把 VmDrawCommand 的形状写死在
 * 协议里，wmwms 才能独立于主项目的结构体定义发布。
 * epoch 与服务端窗口当前代次不符则丢弃（value=0）。 */
typedef struct {
    uint64_t win_id;
    uint32_t epoch;
    uint32_t count;
    int32_t  overflow;     /* 1 = 客户端侧命令被截断 */
    uint32_t cmd_size;     /* 单条命令字节数，<= VMWM_CONTENT_CMD_MAX_SIZE */
} VmWmReqContent;

/* CMD_SET_WORKAREA */
typedef struct {
    int32_t x, y, w, h;
} VmWmReqWorkArea;

/* CMD_THEME_SET / EVT_THEME_CHANGED */
typedef struct {
    char name[32];         /* 主题目录名，空 = 默认主题 */
} VmWmReqTheme;

/* CMD_REQUEST_RESPACK 的请求体 */
typedef struct {
    char id[32];           /* 资源包 id */
} VmWmReqRespack;

/* EVT_LAYOUT_REQ：width/height 是 body 尺寸（已去掉装饰） */
typedef struct {
    uint64_t win_id;
    uint32_t epoch;
    int32_t  width, height;
} VmWmEvtLayoutReq;

/* EVT_POINTER：全局指针（画布坐标）。kind 与 VmWmReqInput.kind 同值。 */
typedef struct {
    int32_t  x, y;
    uint32_t down;         /* 1 = 左键按住 */
    uint32_t kind;         /* 0 move / 1 down / 2 up（与 VmWmReqInput.kind 同值） */
} VmWmEvtPointer;

/* EVT_FRAME / EVT_FULLFRAME：定长头 + 紧跟 len 字节 JSON */
typedef struct {
    uint32_t frame_id;
    uint32_t len;
} VmWmEvtFrame;

/* EVT_RESOURCES：定长头 + 紧跟 len 字节 JSON */
typedef struct {
    char     id[32];
    uint32_t len;
} VmWmEvtRespack;

/* ---------------- 带窗口信息的应答载荷 ---------------- */
typedef struct {
    int32_t        result;
    int32_t        reserved;
    VmWmWindowInfo info;
} VmWmAckInfo;

/* ---------------- 窗口列表应答载荷（变长） ---------------- */
typedef struct {
    int32_t  result;
    uint32_t count;
    /* 紧跟 count 个 VmWmWindowInfo */
} VmWmAckList;

#pragma pack(pop)

/* ---------------- 编译期尺寸守护 ----------------
 * 指针被打包成垃圾发到对端，症状是对方崩在随机地址；尺寸变了忘了同步两端
 * 症状是 len 与实际解释不一致。两种都在编译期挡住。 */
#if defined(__cplusplus)
#define VMWM_STATIC_ASSERT(c, m) static_assert((c), m)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define VMWM_STATIC_ASSERT(c, m) _Static_assert((c), m)
#else
#define VMWM_STATIC_ASSERT(c, m) typedef char vmwm_assert_##__LINE__[(c) ? 1 : -1]
#endif

VMWM_STATIC_ASSERT(sizeof(VmWmMsgHdr) == 16, "VmWmMsgHdr must be 16 bytes");
VMWM_STATIC_ASSERT(sizeof(VmWmAck) == 16, "VmWmAck must be 16 bytes");
VMWM_STATIC_ASSERT(sizeof(VmWmAckHello) == 44, "VmWmAckHello size changed");
VMWM_STATIC_ASSERT(sizeof(VmWmWindowInfo) == 176, "VmWmWindowInfo size changed");
VMWM_STATIC_ASSERT(sizeof(VmWmReqCreate) == 20, "VmWmReqCreate size changed");
VMWM_STATIC_ASSERT(sizeof(VmWmEvtPointer) == 16, "VmWmEvtPointer size changed");
VMWM_STATIC_ASSERT(sizeof(VmWmReqInput) == 96, "VmWmReqInput size changed");
VMWM_STATIC_ASSERT(sizeof(VmWmReqContent) == 24, "VmWmReqContent size changed");
VMWM_STATIC_ASSERT(sizeof(VmWmEvtLayoutReq) == 20, "VmWmEvtLayoutReq size changed");
VMWM_STATIC_ASSERT(sizeof(VmWmEvtFrame) == 8, "VmWmEvtFrame size changed");
VMWM_STATIC_ASSERT(sizeof(VmWmEvtRespack) == 36, "VmWmEvtRespack size changed");
VMWM_STATIC_ASSERT(sizeof(VmWmReqWorkArea) == 16, "VmWmReqWorkArea size changed");
VMWM_STATIC_ASSERT(sizeof(VmWmReqTheme) == 32, "VmWmReqTheme size changed");

/* ---------------- cmd 字段打包/解包 ---------------- */
static inline uint16_t VmWm_MakeCmd(uint16_t type, uint16_t id)
{
    return (uint16_t)(((type & 0x3u) << 14) | (id & VMWM_CMD_MASK));
}
static inline uint16_t VmWm_CmdType(uint16_t cmd)
{
    return (uint16_t)(cmd >> 14);
}
static inline uint16_t VmWm_CmdId(uint16_t cmd)
{
    return (uint16_t)(cmd & VMWM_CMD_MASK);
}

/* ---------------- 角色 → 能力 ---------------- */
/* 单一真源：服务端鉴权与 SEAM.md 里的权限表都由它派生。 */
static inline uint32_t VmWm_RoleCaps(uint32_t client_type)
{
    switch (client_type) {
    case VMWM_CLIENT_APP:
        return VMWM_CAP_OWN_WINDOWS | VMWM_CAP_CONTENT;
    case VMWM_CLIENT_SHELL:
        return VMWM_CAP_OWN_WINDOWS | VMWM_CAP_ALL_WINDOWS | VMWM_CAP_ENUM_WINDOWS |
               VMWM_CAP_CONTENT | VMWM_CAP_INPUT | VMWM_CAP_WORKAREA | VMWM_CAP_THEME;
    case VMWM_CLIENT_RDP:
        return VMWM_CAP_FRAME | VMWM_CAP_RESPACK | VMWM_CAP_ENUM_WINDOWS | VMWM_CAP_INPUT;
    default:
        return 0;
    }
}

#ifdef __cplusplus
}
#endif
#endif /* VMWM_IPC_H */
