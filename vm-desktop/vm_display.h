#ifndef VM_DISPLAY_H
#define VM_DISPLAY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VM_DISPLAY_MAX_COMMANDS 512
#define VM_DISPLAY_MAX_ID 64
#define VM_DISPLAY_MAX_TEXT 256
#define VM_DISPLAY_MAX_STR 128

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
        const char *paint;
        VmLoadImgArgs loadImg;
        VmDefinePatternArgs definePattern;
        VmDefineStyleArgs defineStyle;
        VmDefineProgramArgs defineProgram;
        VmProgramControlArgs programControl;
    } args;
} VmDrawCommand;

typedef struct VmDisplayList {
    VmDrawCommand commands[VM_DISPLAY_MAX_COMMANDS];
    int count;
    uint32_t frameId;
} VmDisplayList;

void VmDisplay_Init(VmDisplayList *list);
void VmDisplay_CopyStr(char *dst, size_t cap, const char *src);
void VmDisplay_Clear(VmDisplayList *list);
VmDrawCommand *VmDisplay_Append(VmDisplayList *list, VmCmdType type, const char *id);
VmDrawCommand *VmDisplay_Find(VmDisplayList *list, const char *id);
int VmDisplay_CommandEqual(const VmDrawCommand *a, const VmDrawCommand *b);

#ifdef __cplusplus
}
#endif

#endif
