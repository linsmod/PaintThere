#include "vm_serialize_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len, cap;
} StrBuf;

static int reserve(StrBuf *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 1024;
    while (cap < need) cap *= 2;
    char *p = (char *)realloc(b->data, cap);
    if (!p) return 0;
    b->data = p;
    b->cap = cap;
    return 1;
}

static void puts_(StrBuf *b, const char *s) {
    size_t n = strlen(s);
    if (!reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void printf_(StrBuf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n < sizeof(tmp)) { puts_(b, tmp); return; }
    char *big = (char *)malloc((size_t)n + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    puts_(b, big);
    free(big);
}

static void json_float(StrBuf *b, float v) {
    if (v != v || v > 1.0e30f || v < -1.0e30f) { puts_(b, "0"); return; }
    printf_(b, "%.4f", (double)v);
}

static void json_string(StrBuf *b, const char *s) {
    if (!s) { puts_(b, "\"\""); return; }
    puts_(b, "\"");
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"': puts_(b, "\\\""); break;
            case '\\': puts_(b, "\\\\"); break;
            case '\n': puts_(b, "\\n"); break;
            case '\r': puts_(b, "\\r"); break;
            case '\t': puts_(b, "\\t"); break;
            default:
                if (c < 0x20) printf_(b, "\\u%04x", c);
                else printf_(b, "%c", c);
        }
    }
    puts_(b, "\"");
}

static void rect_json(StrBuf *b, VmRect r) {
    printf_(b, "[%g,%g,%g,%g]", r.x, r.y, r.w, r.h);
}

/* args expect four separate numbers; the 'rect' field uses the array form. */
static void rect_args(StrBuf *b, VmRect r) {
    printf_(b, "%g,%g,%g,%g", r.x, r.y, r.w, r.h);
}

static void emit_command(StrBuf *b, const VmDrawCommand *c) {
    puts_(b, "{\"id\":");
    json_string(b, c->id);
    puts_(b, ",\"cmd\":\"");
    switch (c->type) {
        case VM_CMD_CLEAR: puts_(b, "clear\",\"args\":["); break;
        case VM_CMD_DRAW_RECT: {
            const VmDrawRectArgs *a = &c->args.drawRect;
            puts_(b, "drawRect\",\"args\":[");
            rect_args(b, a->rect);
            puts_(b, ","); json_string(b, a->paint);
            printf_(b, ",%g", a->cornerRadius);
            break;
        }
        case VM_CMD_DRAW_BORDER: {
            const VmDrawBorderArgs *a = &c->args.drawBorder;
            puts_(b, "drawBorder\",\"args\":[");
            rect_args(b, a->rect);
            puts_(b, ","); json_string(b, a->paint);
            printf_(b, ",[%d,%d,%d,%d],%g", a->widths[0], a->widths[1], a->widths[2], a->widths[3],
                    a->cornerRadius);
            break;
        }
        case VM_CMD_DRAW_TEXT: {
            const VmDrawTextArgs *a = &c->args.drawText;
            puts_(b, "drawText\",\"args\":[");
            rect_args(b, a->rect);
            puts_(b, ","); json_string(b, a->text);
            printf_(b, ",%u,%u,", a->fontId, a->fontSize);
            json_string(b, a->color);
            printf_(b, ",%u,%u", a->letterSpacing, a->lineHeight);
            break;
        }
        case VM_CMD_DRAW_IMG: {
            const VmDrawImgArgs *a = &c->args.drawImg;
            puts_(b, "drawImg\",\"args\":[");
            json_string(b, a->imgId);
            puts_(b, ","); rect_args(b, a->rect);
            puts_(b, ","); json_string(b, a->tint);
            printf_(b, ",%g", a->cornerRadius);
            break;
        }
        case VM_CMD_DRAW_PATH: {
            const VmDrawPathArgs *a = &c->args.drawPath;
            puts_(b, "drawPath\",\"args\":[");
            rect_args(b, a->rect);
            puts_(b, ","); json_string(b, a->path);
            puts_(b, ","); json_string(b, a->paint);
            break;
        }
        case VM_CMD_CLIP_PUSH: {
            puts_(b, "clipPush\",\"args\":[");
            rect_args(b, c->args.clip);
            break;
        }
        case VM_CMD_CLIP_POP: puts_(b, "clipPop\",\"args\":["); break;
        case VM_CMD_OVERLAY_PUSH: {
            puts_(b, "overlayPush\",\"args\":[");
            json_string(b, c->args.paint);
            break;
        }
        case VM_CMD_OVERLAY_POP: puts_(b, "overlayPop\",\"args\":["); break;
        case VM_CMD_NONE:
        default:
            puts_(b, "none\",\"args\":[");
            break;
    }
    puts_(b, "],\"rect\":");
    rect_json(b, c->rect);
    puts_(b, "}");
}

/* 资源与程序控制命令的序列化，输出格式与它们原先在 commands 里时完全一致。
   rect 恒为零矩形：这类命令不属于任何绘制区域。 */
static void emit_resource(StrBuf *b, const VmResourceCommand *r) {
    puts_(b, "{\"id\":");
    json_string(b, r->id);
    puts_(b, ",\"cmd\":\"");
    switch (r->type) {
        case VM_CMD_LOAD_IMG: {
            puts_(b, "loadImg\",\"args\":[");
            json_string(b, r->args.loadImg.imgId);
            puts_(b, ","); json_string(b, r->args.loadImg.dataUrl);
            break;
        }
        case VM_CMD_DEFINE_PATTERN: {
            const VmDefinePatternArgs *a = &r->args.definePattern;
            puts_(b, "definePattern\",\"args\":[");
            json_string(b, a->patternId);
            puts_(b, ",{\"type\":\"");
            puts_(b, a->type == VM_PATTERN_SOLID ? "solid" :
                      a->type == VM_PATTERN_GRADIENT ? "gradient" :
                      a->type == VM_PATTERN_RADIAL ? "radial" : "image");
            puts_(b, "\"");
            if (a->color[0]) { puts_(b, ",\"color\":"); json_string(b, a->color); }
            if (a->from[0]) { puts_(b, ",\"from\":"); json_string(b, a->from); }
            if (a->to[0]) { puts_(b, ",\"to\":"); json_string(b, a->to); }
            if (a->stopCount >= 2) {
                puts_(b, ",\"stops\":[");
                for (int i = 0; i < a->stopCount && i < VM_PATTERN_MAX_STOPS; i++) {
                    if (i) puts_(b, ",");
                    puts_(b, "{\"color\":");
                    json_string(b, a->stops[i].color);
                    puts_(b, ",\"offset\":");
                    json_float(b, a->stops[i].offset);
                    puts_(b, "}");
                }
                puts_(b, "]");
            }
            if (a->direction[0]) { puts_(b, ",\"direction\":"); json_string(b, a->direction); }
            if (a->imgId[0]) { puts_(b, ",\"imgId\":"); json_string(b, a->imgId); }
            if (a->tiling[0]) { puts_(b, ",\"tiling\":"); json_string(b, a->tiling); }
            puts_(b, "}");
            break;
        }
        case VM_CMD_DEFINE_STYLE: {
            const VmDefineStyleArgs *a = &r->args.defineStyle;
            puts_(b, "defineStyle\",\"args\":[");
            json_string(b, a->styleId);
            puts_(b, ",{");
            int first = 1;
            #define STYLE_FIELD(name, field) \
                if (a->field[0]) { if (!first) puts_(b, ","); first = 0; puts_(b, "\"" name "\":"); json_string(b, a->field); }
            STYLE_FIELD("bgPattern", bgPattern)
            STYLE_FIELD("border", border)
            STYLE_FIELD("fontId", fontId)
            STYLE_FIELD("fontSize", fontSize)
            STYLE_FIELD("textColor", textColor)
            #undef STYLE_FIELD
            puts_(b, "}");
            break;
        }
        case VM_CMD_DEFINE_DRAW_PROGRAM: {
            const VmDefineProgramArgs *a = &r->args.defineProgram;
            puts_(b, "defineDrawProgram\",\"args\":[");
            json_string(b, a->programId);
            puts_(b, ",");
            char *inner = VmSerialize_CommandArrayJson(a->commands);
            puts_(b, inner ? inner : "[]");
            free(inner);
            puts_(b, ",");
            json_string(b, a->layerId);
            break;
        }
        case VM_CMD_START_PROGRAM:
        case VM_CMD_PAUSE_PROGRAM:
        case VM_CMD_RESUME_PROGRAM: {
            const char *name = r->type == VM_CMD_START_PROGRAM ? "startDrawProgram" :
                               r->type == VM_CMD_PAUSE_PROGRAM ? "pauseDrawProgram" : "resumeDrawProgram";
            printf_(b, "%s\",\"args\":[", name);
            json_string(b, r->args.programControl.programId);
            if (r->type == VM_CMD_START_PROGRAM) {
                puts_(b, ",");
                json_string(b, r->args.programControl.mode ? r->args.programControl.mode : "once");
            }
            break;
        }
        case VM_CMD_LOAD_RES_PACK: {
            puts_(b, "loadResPack\",\"args\":[");
            json_string(b, r->args.loadResPack.resPackId);
            break;
        }
        case VM_CMD_NONE:
        default:
            puts_(b, "none\",\"args\":[");
            break;
    }
    puts_(b, "],\"rect\":[0,0,0,0]}");
}

static char *serialize(const VmDisplayList *list, int withEnvelope) {
    if (!list) return NULL;
    StrBuf b = {0};
    if (withEnvelope)
        printf_(&b, "{\"type\":\"frame\",\"frameId\":%u,\"commands\":[", list->frameId);
    else
        puts_(&b, "[");
    /* 资源命令先输出：它们本来就是先注册、后引用，顺序与改造前一致。 */
    int first = 1;
    for (int i = 0; i < list->resourceCount; i++) {
        if (!first) puts_(&b, ",");
        first = 0;
        emit_resource(&b, &list->resources[i]);
    }
    for (int i = 0; i < list->count; i++) {
        if (!first) puts_(&b, ",");
        first = 0;
        emit_command(&b, &list->commands[i]);
    }
    puts_(&b, withEnvelope ? "]}" : "]");
    return b.data;
}

char *VmSerialize_FrameJson(const VmDisplayList *list) {
    return serialize(list, 1);
}

char *VmSerialize_CommandArrayJson(const VmDisplayList *list) {
    return serialize(list, 0);
}

/* 增量帧带完整命令列表和唯一的重绘矩形：前端只做 clip/clear/重放，
   是否需要重绘、重绘哪里全部由服务端决定。dirty 为空表示这一帧不用画。 */
char *VmSerialize_DeltaFrameJson(const VmDisplayList *list, const VmDisplayDelta *delta) {
    if (!list || !delta) return NULL;
    StrBuf b = {0};
    printf_(&b, "{\"type\":\"frame\",\"frameId\":%u,\"incremental\":true,\"commands\":[",
            list->frameId);
    int first = 1;
    for (int i = 0; i < list->resourceCount; i++) {
        if (!first) puts_(&b, ",");
        first = 0;
        emit_resource(&b, &list->resources[i]);
    }
    for (int i = 0; i < list->count; i++) {
        if (!first) puts_(&b, ",");
        first = 0;
        emit_command(&b, &list->commands[i]);
    }
    /* dirty 是单个矩形 [x,y,w,h]；没有脏区时是空数组 []。
       注意不要再套一层 []，否则前端拿到的是 [[x,y,w,h]]，长度 1 会被当成无效。 */
    puts_(&b, "],\"dirty\":");
    if (delta->hasDirty) rect_json(&b, delta->dirty);
    else puts_(&b, "[]");
    puts_(&b, "}");
    return b.data;
}

/* ---------------- Remote tiles 指令流 ---------------- */

struct VmTiles {
    StrBuf b;
    int count;
    uint32_t frameId;
};

VmTiles *VmTiles_Create(void) {
    VmTiles *t = (VmTiles *)calloc(1, sizeof(VmTiles));
    return t;
}

void VmTiles_Destroy(VmTiles *tiles) {
    if (!tiles) return;
    free(tiles->b.data);
    free(tiles);
}

void VmTiles_SetFrameId(VmTiles *tiles, uint32_t frameId) {
    if (tiles) tiles->frameId = frameId;
}

static void op_begin(VmTiles *t) {
    if (!t) return;
    if (t->count == 0)
        printf_(&t->b, "{\"type\":\"tiles\",\"frameId\":%u,\"ops\":[", t->frameId);
    else
        puts_(&t->b, ",");
    t->count++;
}

void VmTiles_ShowLoading(VmTiles *tiles) {
    if (!tiles) return;
    op_begin(tiles);
    puts_(&tiles->b, "{\"op\":\"showLoading\"}");
}

void VmTiles_HideLoading(VmTiles *tiles) {
    if (!tiles) return;
    op_begin(tiles);
    puts_(&tiles->b, "{\"op\":\"hideLoading\"}");
}

void VmTiles_Config(VmTiles *tiles, int cols, int rows, int tileW, int tileH, int w, int h) {
    if (!tiles) return;
    op_begin(tiles);
    printf_(&tiles->b,
           "{\"op\":\"config\",\"cols\":%d,\"rows\":%d,\"tileW\":%d,\"tileH\":%d,\"w\":%d,\"h\":%d}",
           cols, rows, tileW, tileH, w, h);
}

void VmTiles_SetTile(VmTiles *tiles, int index, const VmDrawCommand *cmds, int count) {
    if (!tiles) return;
    op_begin(tiles);
    printf_(&tiles->b, "{\"op\":\"setTile\",\"index\":%d,\"cmds\":[", index);
    for (int i = 0; i < count; i++) {
        if (i) puts_(&tiles->b, ",");
        emit_command(&tiles->b, &cmds[i]);
    }
    puts_(&tiles->b, "]}");
}

void VmTiles_SetTileJson(VmTiles *tiles, int index, const char *cmdsJson) {
    if (!tiles) return;
    op_begin(tiles);
    printf_(&tiles->b, "{\"op\":\"setTile\",\"index\":%d,\"cmds\":", index);
    puts_(&tiles->b, cmdsJson ? cmdsJson : "[]");
    puts_(&tiles->b, "}");
}

char *VmTiles_Finish(VmTiles *tiles) {
    if (!tiles) return NULL;
    if (tiles->count == 0)
        printf_(&tiles->b, "{\"type\":\"tiles\",\"frameId\":%u,\"ops\":[", tiles->frameId);
    puts_(&tiles->b, "]}");
    char *data = tiles->b.data;
    tiles->b.data = NULL;
    VmTiles_Destroy(tiles);
    return data;
}
