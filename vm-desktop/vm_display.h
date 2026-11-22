#ifndef VM_DISPLAY_H
#define VM_DISPLAY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一帧的命令上限：单窗口配额 1024 × 最多 8 窗口，合成需要留足空间，
   否则 appending 到一半失败会把排在最后的置顶窗口（任务栏）整层丢掉。 */
#define VM_DISPLAY_MAX_COMMANDS 2048
#define VM_DISPLAY_MAX_RESOURCES 16
#define VM_DISPLAY_MAX_ID 64
#define VM_DISPLAY_MAX_TEXT 256
#define VM_DISPLAY_MAX_STR 128
#define VM_DISPLAY_MAX_DIRTY 8

typedef enum {
    VM_CMD_NONE = 0,
    VM_CMD_CLEAR,
    VM_CMD_DRAW_RECT,
    VM_CMD_DRAW_BORDER,
    VM_CMD_DRAW_TEXT,
    VM_CMD_DRAW_IMG,
    VM_CMD_CLIP_PUSH,
    VM_CMD_CLIP_POP,
    VM_CMD_OVERLAY_PUSH,
    VM_CMD_OVERLAY_POP,
    VM_CMD_LOAD_IMG,
    VM_CMD_DEFINE_PATTERN,
    VM_CMD_DEFINE_STYLE,
    VM_CMD_DEFINE_DRAW_PROGRAM,
    VM_CMD_START_PROGRAM,
    VM_CMD_PAUSE_PROGRAM,
    VM_CMD_RESUME_PROGRAM
} VmCmdType;

typedef struct {
    float x, y, w, h;
} VmRect;

typedef struct {
    VmRect rect;
    char paint[VM_DISPLAY_MAX_STR];  /* CSS color or styleId */
    float cornerRadius;
} VmDrawRectArgs;

typedef struct {
    VmRect rect;
    char paint[VM_DISPLAY_MAX_STR];
    int widths[4];          /* left, right, top, bottom */
    float cornerRadius;
} VmDrawBorderArgs;

typedef struct {
    VmRect rect;
    char text[VM_DISPLAY_MAX_TEXT];
    uint16_t fontId, fontSize;
    char color[VM_DISPLAY_MAX_STR];
    uint16_t letterSpacing, lineHeight;
} VmDrawTextArgs;

typedef struct {
    char imgId[VM_DISPLAY_MAX_STR];
    VmRect rect;
    char tint[VM_DISPLAY_MAX_STR];  /* empty string = untinted */
    float cornerRadius;
} VmDrawImgArgs;

typedef struct {
    char imgId[VM_DISPLAY_MAX_STR];
    char dataUrl[512];
} VmLoadImgArgs;

/* Declarative pattern descriptor; serialized as a small object by JSON serializer. */
typedef enum { VM_PATTERN_SOLID, VM_PATTERN_GRADIENT, VM_PATTERN_IMAGE } VmPatternType;
typedef struct {
    char patternId[VM_DISPLAY_MAX_STR];
    VmPatternType type;
    char color[VM_DISPLAY_MAX_STR];
    char from[VM_DISPLAY_MAX_STR], to[VM_DISPLAY_MAX_STR];
    char direction[VM_DISPLAY_MAX_STR];
    char imgId[VM_DISPLAY_MAX_STR];
    char tiling[VM_DISPLAY_MAX_STR];
} VmDefinePatternArgs;

/* Style fields are owned copies; empty string means unset. */
typedef struct {
    char styleId[VM_DISPLAY_MAX_STR];
    char bgPattern[VM_DISPLAY_MAX_STR];
    char border[VM_DISPLAY_MAX_STR];
    char fontId[VM_DISPLAY_MAX_STR];
    char fontSize[VM_DISPLAY_MAX_STR];
    char textColor[VM_DISPLAY_MAX_STR];
} VmDefineStyleArgs;

/* Program commands are stored as a nested VmDisplayList (pointer to guest-owned list). */
typedef struct {
    char programId[VM_DISPLAY_MAX_STR];
    struct VmDisplayList *commands;
    char layerId[VM_DISPLAY_MAX_STR];
} VmDefineProgramArgs;

typedef struct {
    char programId[VM_DISPLAY_MAX_STR];
    char mode[32];       /* "once" / "loop" */
} VmProgramControlArgs;

typedef struct {
    VmCmdType type;
    char id[VM_DISPLAY_MAX_ID];
    VmRect rect;            /* dirty rect; all-zero means unspecified */
    union {
        VmDrawRectArgs drawRect;
        VmDrawBorderArgs drawBorder;
        VmDrawTextArgs drawText;
        VmDrawImgArgs drawImg;
        VmRect clip;
        /* overlay 颜色。必须是内联数组而不是指针：命令要跨帧复用、并且要
           跨进程传给 WMS，指针会指向调用方的临时缓冲。 */
        char paint[VM_DISPLAY_MAX_STR];
    } args;
} VmDrawCommand;

/* 资源与程序生命周期命令：注册一次、长期有效，数量少但参数大。
   独立于绘制命令存储，避免每条绘制命令都为它们的 union 成员付出尺寸。 */
typedef struct {
    VmCmdType type;
    char id[VM_DISPLAY_MAX_ID];
    union {
        VmLoadImgArgs loadImg;
        VmDefinePatternArgs definePattern;
        VmDefineStyleArgs defineStyle;
        VmDefineProgramArgs defineProgram;
        VmProgramControlArgs programControl;
    } args;
} VmResourceCommand;

typedef struct VmDisplayList {
    VmDrawCommand commands[VM_DISPLAY_MAX_COMMANDS];
    int count;
    VmResourceCommand resources[VM_DISPLAY_MAX_RESOURCES];
    int resourceCount;
    uint32_t frameId;
} VmDisplayList;

/* 命令写入目标：窗口图层写自己的小缓冲，合成时再并入输出 list。
   容量由调用方给，写满即失败（由上层记录溢出），不做静默扩容。 */
typedef struct {
    VmDrawCommand *commands;
    int cap;
    int count;
    /* 因容量不足被丢弃的命令数：真实需求 = count + rejected，
       用于在画面上报告「某个窗口超了配额多少条」。 */
    int rejected;
} VmCmdBuffer;

/* Structural diff: changed/added commands, removed ids, and full current order. */
typedef struct {
    VmDrawCommand changed[VM_DISPLAY_MAX_COMMANDS];
    int changedCount;
    char removed[VM_DISPLAY_MAX_COMMANDS][VM_DISPLAY_MAX_ID];
    int removedCount;
    char order[VM_DISPLAY_MAX_COMMANDS][VM_DISPLAY_MAX_ID];
    int orderCount;
    /* 绘制顺序（z 序）是否变化。变化时遮挡关系变了，必须重绘重叠区域。 */
    int orderChanged;
    /* 这一帧需要重绘的区域（changed/removed 命令的 rect 与顺序变化重叠区的并集）。
       是否需要重绘、重绘哪里，全部由服务端决定，前端只按这个矩形执行。 */
    VmRect dirty;
    int hasDirty;
} VmDisplayDelta;

void VmDisplay_Init(VmDisplayList *list);
void VmDisplay_CopyStr(char *dst, size_t cap, const char *src);
void VmDisplay_Clear(VmDisplayList *list);
VmDrawCommand *VmDisplay_Append(VmDisplayList *list, VmCmdType type, const char *id);
/* 向定长缓冲追加一条命令；写满返回 NULL。 */
VmDrawCommand *VmCmdBuffer_Append(VmCmdBuffer *buf, VmCmdType type, const char *id);
/* 资源/程序控制命令走独立数组：不进 commands，不参与逐帧 diff 与 tile 切分。 */
VmResourceCommand *VmDisplay_AppendResource(VmDisplayList *list, VmCmdType type, const char *id);
VmDrawCommand *VmDisplay_Find(VmDisplayList *list, const char *id);
int VmDisplay_CommandEqual(const VmDrawCommand *a, const VmDrawCommand *b);
/* Compute a structural delta from prev -> current. Delta references current's storage. */
void VmDisplay_Diff(const VmDisplayList *prev, const VmDisplayList *current, VmDisplayDelta *delta);

/* 把一块区域并入 delta 的重绘区域（WM 层用它补充 z 序变化的重叠区）。 */
void VmDisplay_AddDirty(VmDisplayDelta *delta, VmRect r);

#ifdef __cplusplus
}
#endif

#endif
