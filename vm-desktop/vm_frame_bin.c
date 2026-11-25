#include "vm_frame_bin.h"
#include "vm_serialize_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* little-endian byte writer */
typedef struct {
    uint8_t *data;
    size_t len, cap;
    int failed;
} Buf;

static void put_u8(Buf *b, uint8_t v) {
    if (b->failed) return;
    if (b->len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        uint8_t *p = (uint8_t *)realloc(b->data, cap);
        if (!p) { b->failed = 1; return; }
        b->data = p; b->cap = cap;
    }
    b->data[b->len++] = v;
}
static void put_u16(Buf *b, uint16_t v) { put_u8(b, (uint8_t)v); put_u8(b, (uint8_t)(v >> 8)); }
static void put_u32(Buf *b, uint32_t v) {
    put_u16(b, (uint16_t)v); put_u16(b, (uint16_t)(v >> 16));
}
static void put_i32(Buf *b, int32_t v) { put_u32(b, (uint32_t)v); }
static void put_f32(Buf *b, float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    put_u32(b, bits); /* IEEE-754 bit pattern; exact round-trip */
}
static void put_str(Buf *b, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n > 0xFFFF) n = 0xFFFF;
    put_u16(b, (uint16_t)n);
    for (size_t i = 0; i < n; i++) put_u8(b, (uint8_t)s[i]);
}
static void put_rect(Buf *b, VmRect r) {
    put_f32(b, r.x); put_f32(b, r.y); put_f32(b, r.w); put_f32(b, r.h);
}

struct VmBinWriter {
    Buf b;
    uint16_t opCount;
};

VmBinWriter *VmBinWriter_Create(void) {
    VmBinWriter *w = (VmBinWriter *)calloc(1, sizeof(*w));
    return w;
}
void VmBinWriter_Destroy(VmBinWriter *w) {
    if (!w) return;
    free(w->b.data);
    free(w);
}

void VmBinWriter_BeginFrame(VmBinWriter *w, uint32_t frameId) {
    if (!w) return;
    free(w->b.data);
    memset(&w->b, 0, sizeof(w->b));
    w->opCount = 0;
    put_u32(&w->b, VM_FRAME_BIN_MAGIC);
    put_u16(&w->b, VM_FRAME_BIN_VERSION);
    put_u16(&w->b, 0);       /* flags */
    put_u32(&w->b, frameId);
    put_u16(&w->b, 0);       /* opCount placeholder */
}

static void begin_op(VmBinWriter *w, uint8_t kind) {
    if (!w) return;
    put_u8(&w->b, kind);
    w->opCount++;
}

void VmBinWriter_OpConfig(VmBinWriter *writer, int cols, int rows, int tileW, int tileH, int viewW, int viewH) {
    if (!writer) return;
    begin_op(writer, VM_BIN_OP_CONFIG);
    put_i32(&writer->b, cols); put_i32(&writer->b, rows);
    put_i32(&writer->b, tileW); put_i32(&writer->b, tileH);
    put_i32(&writer->b, viewW); put_i32(&writer->b, viewH);
}
void VmBinWriter_OpShowLoading(VmBinWriter *w) { if (w) begin_op(w, VM_BIN_OP_SHOW_LOADING); }
void VmBinWriter_OpHideLoading(VmBinWriter *w) { if (w) begin_op(w, VM_BIN_OP_HIDE_LOADING); }

static void put_command(Buf *b, const VmDrawCommand *c) {
    put_u8(b, (uint8_t)c->type);
    put_str(b, c->id);
    put_rect(b, c->rect);
    switch (c->type) {
        case VM_CMD_DRAW_RECT:
            put_rect(b, c->args.drawRect.rect);
            put_str(b, c->args.drawRect.paint);
            put_f32(b, c->args.drawRect.cornerRadius);
            break;
        case VM_CMD_DRAW_BORDER: {
            const VmDrawBorderArgs *a = &c->args.drawBorder;
            put_rect(b, a->rect); put_str(b, a->paint);
            for (int i = 0; i < 4; i++) put_i32(b, a->widths[i]);
            put_f32(b, a->cornerRadius);
            break;
        }
        case VM_CMD_DRAW_TEXT: {
            const VmDrawTextArgs *a = &c->args.drawText;
            put_rect(b, a->rect); put_str(b, a->text);
            put_u16(b, a->fontId); put_u16(b, a->fontSize);
            put_str(b, a->color);
            put_u16(b, a->letterSpacing); put_u16(b, a->lineHeight);
            break;
        }
        case VM_CMD_DRAW_IMG: {
            const VmDrawImgArgs *a = &c->args.drawImg;
            put_str(b, a->imgId); put_rect(b, a->rect);
            put_str(b, a->tint); put_f32(b, a->cornerRadius);
            break;
        }
        case VM_CMD_DRAW_PATH: {
            const VmDrawPathArgs *a = &c->args.drawPath;
            put_rect(b, a->rect); put_str(b, a->path); put_str(b, a->paint);
            break;
        }
        case VM_CMD_CLIP_PUSH:
        case VM_CMD_CLIP_POP:
            put_rect(b, c->args.clip);
            break;
        case VM_CMD_OVERLAY_PUSH:
            put_str(b, c->args.paint);
            break;
        case VM_CMD_OVERLAY_POP:
        case VM_CMD_CLEAR:
        case VM_CMD_NONE:
        default:
            break;
    }
}

void VmBinWriter_OpSetTile(VmBinWriter *w, int index, const VmDrawCommand *cmds, int count) {
    if (!w) return;
    begin_op(w, VM_BIN_OP_SET_TILE);
    put_i32(&w->b, index);
    put_u16(&w->b, (uint16_t)(count > 0xFFFF ? 0xFFFF : count));
    for (int i = 0; i < count; i++) put_command(&w->b, &cmds[i]);
}

uint8_t *VmBinWriter_Finish(VmBinWriter *w, size_t *outLen) {
    if (!w || w->b.failed) { if (outLen) *outLen = 0; return NULL; }
    /* header: magic(4) + version(2) + flags(2) + frameId(4) + opCount(2) = 14 */
    if (w->b.len >= 14) {
        w->b.data[12] = (uint8_t)w->opCount;
        w->b.data[13] = (uint8_t)(w->opCount >> 8);
    }
    if (outLen) *outLen = w->b.len;
    uint8_t *data = w->b.data;
    memset(&w->b, 0, sizeof(w->b));
    return data;
}

/* ---------------- decoder (binary -> VmTiles) ---------------- */
typedef struct {
    const uint8_t *p;
    size_t len, pos;
    int failed;
} Reader;

static uint8_t get_u8(Reader *r) {
    if (r->failed || r->pos >= r->len) { r->failed = 1; return 0; }
    return r->p[r->pos++];
}
static uint16_t get_u16(Reader *r) {
    uint16_t v = get_u8(r); return (uint16_t)(v | ((uint16_t)get_u8(r) << 8));
}
static uint32_t get_u32(Reader *r) {
    uint32_t v = get_u16(r); return v | ((uint32_t)get_u16(r) << 16);
}
static int32_t get_i32(Reader *r) { return (int32_t)get_u32(r); }
static float get_f32(Reader *r) { uint32_t v = get_u32(r); float f; memcpy(&f, &v, 4); return f; }
static void get_str(Reader *r, char *dst, size_t cap) {
    uint16_t n = get_u16(r);
    if (n + 1 > cap) n = (uint16_t)(cap - 1);
    for (uint16_t i = 0; i < n; i++) dst[i] = (char)get_u8(r);
    dst[n] = '\0';
}
static VmRect get_rect(Reader *r) {
    VmRect v = {get_f32(r), get_f32(r), get_f32(r), get_f32(r)};
    return v;
}

static VmCmdType get_command(Reader *r, VmDrawCommand *c) {
    memset(c, 0, sizeof(*c));
    c->type = (VmCmdType)get_u8(r);
    char id[VM_DISPLAY_MAX_ID];
    get_str(r, id, sizeof(id));
    VmDisplay_CopyStr(c->id, sizeof(c->id), id);
    c->rect = get_rect(r);
    switch (c->type) {
        case VM_CMD_DRAW_RECT:
            c->args.drawRect.rect = get_rect(r);
            get_str(r, c->args.drawRect.paint, sizeof(c->args.drawRect.paint));
            c->args.drawRect.cornerRadius = get_f32(r);
            break;
        case VM_CMD_DRAW_BORDER: {
            VmDrawBorderArgs *a = &c->args.drawBorder;
            a->rect = get_rect(r);
            get_str(r, a->paint, sizeof(a->paint));
            for (int i = 0; i < 4; i++) a->widths[i] = get_i32(r);
            a->cornerRadius = get_f32(r);
            break;
        }
        case VM_CMD_DRAW_TEXT: {
            VmDrawTextArgs *a = &c->args.drawText;
            a->rect = get_rect(r);
            get_str(r, a->text, sizeof(a->text));
            a->fontId = get_u16(r); a->fontSize = get_u16(r);
            get_str(r, a->color, sizeof(a->color));
            a->letterSpacing = get_u16(r); a->lineHeight = get_u16(r);
            break;
        }
        case VM_CMD_DRAW_IMG: {
            VmDrawImgArgs *a = &c->args.drawImg;
            get_str(r, a->imgId, sizeof(a->imgId));
            a->rect = get_rect(r);
            get_str(r, a->tint, sizeof(a->tint));
            a->cornerRadius = get_f32(r);
            break;
        }
        case VM_CMD_DRAW_PATH: {
            VmDrawPathArgs *a = &c->args.drawPath;
            a->rect = get_rect(r);
            get_str(r, a->path, sizeof(a->path));
            get_str(r, a->paint, sizeof(a->paint));
            break;
        }
        case VM_CMD_CLIP_PUSH:
        case VM_CMD_CLIP_POP:
            c->args.clip = get_rect(r);
            break;
        case VM_CMD_OVERLAY_PUSH:
            get_str(r, (char *)c->args.paint, VM_DISPLAY_MAX_STR);
            break;
        default:
            break;
    }
    return c->type;
}

VmTiles *VmFrameBin_ToTiles(const uint8_t *data, size_t len, uint32_t *outFrameId) {
    if (!data || len < 16) return NULL;
    Reader r = {data, len, 0, 0};
    if (get_u32(&r) != VM_FRAME_BIN_MAGIC) return NULL;
    if (get_u16(&r) != VM_FRAME_BIN_VERSION) return NULL;
    get_u16(&r); /* flags */
    uint32_t frameId = get_u32(&r);
    uint16_t opCount = get_u16(&r);
    if (r.failed) return NULL;

    VmTiles *tiles = VmTiles_Create();
    if (!tiles) return NULL;
    VmTiles_SetFrameId(tiles, frameId);

    for (uint16_t i = 0; i < opCount && !r.failed; i++) {
        uint8_t op = get_u8(&r);
        switch (op) {
            case VM_BIN_OP_CONFIG: {
                int cols = get_i32(&r), rows = get_i32(&r);
                int tw = get_i32(&r), th = get_i32(&r);
                int w = get_i32(&r), h = get_i32(&r);
                VmTiles_Config(tiles, cols, rows, tw, th, w, h);
                break;
            }
            case VM_BIN_OP_SHOW_LOADING: VmTiles_ShowLoading(tiles); break;
            case VM_BIN_OP_HIDE_LOADING: VmTiles_HideLoading(tiles); break;
            case VM_BIN_OP_SET_TILE: {
                int index = get_i32(&r);
                uint16_t count = get_u16(&r);
                if (count > VM_DISPLAY_MAX_COMMANDS) { r.failed = 1; break; }
                VmDrawCommand cmds[VM_DISPLAY_MAX_COMMANDS];
                for (uint16_t k = 0; k < count && !r.failed; k++) get_command(&r, &cmds[k]);
                if (!r.failed) VmTiles_SetTile(tiles, index, cmds, count);
                break;
            }
            default:
                r.failed = 1;
                break;
        }
    }

    if (r.failed) { VmTiles_Destroy(tiles); return NULL; }
    if (outFrameId) *outFrameId = frameId;
    return tiles;
}
