#include "vm_tiles.h"

#include <stdlib.h>
#include <string.h>

/* 逐块复用的暂存列表：VmDisplayList 较大，不能放在栈上，也不按块各存一份。 */
static VmDisplayList s_tileScratch;

static unsigned long long hash_json(const char *s) {
    unsigned long long h = 1469598103934665603ULL;
    if (!s) return h;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        h ^= (unsigned long long)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

static int rect_intersects(VmRect a, VmRect b) {
    if (a.w <= 0 || a.h <= 0) return 0;
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x ||
             a.y + a.h <= b.y || b.y + b.h <= a.y);
}

/* tile canvas 自身负责裁剪，因此只做坐标平移，不做几何裁剪。 */
static void translate_command(VmDrawCommand *c, float dx, float dy) {
    c->rect.x += dx;
    c->rect.y += dy;
    switch (c->type) {
        case VM_CMD_DRAW_RECT:
            c->args.drawRect.rect.x += dx;
            c->args.drawRect.rect.y += dy;
            break;
        case VM_CMD_DRAW_BORDER:
            c->args.drawBorder.rect.x += dx;
            c->args.drawBorder.rect.y += dy;
            break;
        case VM_CMD_DRAW_TEXT:
            c->args.drawText.rect.x += dx;
            c->args.drawText.rect.y += dy;
            break;
        case VM_CMD_DRAW_IMG:
            c->args.drawImg.rect.x += dx;
            c->args.drawImg.rect.y += dy;
            break;
        case VM_CMD_CLIP_PUSH:
        case VM_CMD_CLIP_POP:
            c->args.clip.x += dx;
            c->args.clip.y += dy;
            break;
        default:
            break;
    }
}

void VmTileGrid_Init(VmTileGrid *grid, int cols, int rows, int tileW, int tileH, int w, int h) {
    if (!grid) return;
    memset(grid, 0, sizeof(*grid));
    grid->cols = cols;
    grid->rows = rows;
    grid->tileW = tileW;
    grid->tileH = tileH;
    grid->w = w;
    grid->h = h;
}

int VmTileGrid_Build(VmTileGrid *grid, const VmDisplayList *frame, int full, VmTiles *out) {
    if (!grid || !frame || !out) return 0;
    int total = grid->cols * grid->rows;
    if (total <= 0 || total > VM_TILE_MAX) return 0;

    int any = 0;
    for (int idx = 0; idx < total; idx++) {
        float x0 = (float)((idx % grid->cols) * grid->tileW);
        float y0 = (float)((idx / grid->cols) * grid->tileH);
        VmRect tile = {x0, y0, (float)grid->tileW, (float)grid->tileH};

        VmDisplay_Clear(&s_tileScratch);
        for (int i = 0; i < frame->count; i++) {
            const VmDrawCommand *src = &frame->commands[i];
            /* 防御：资源命令已独立存储、不进 commands，若误混入则跳过。 */
            if (src->type == VM_CMD_LOAD_IMG) continue;
            /* clip / overlay 必须成对保留，否则 tile 内绘制状态会失衡。 */
            int keepPair = (src->type == VM_CMD_CLIP_PUSH || src->type == VM_CMD_CLIP_POP ||
                            src->type == VM_CMD_OVERLAY_PUSH || src->type == VM_CMD_OVERLAY_POP);
            if (!keepPair && !rect_intersects(src->rect, tile)) continue;
            VmDrawCommand *dst = VmDisplay_Append(&s_tileScratch, src->type, src->id);
            if (!dst) break;
            *dst = *src;
            translate_command(dst, -x0, -y0);
        }

        char *json = VmSerialize_CommandArrayJson(&s_tileScratch);
        unsigned long long h = hash_json(json);
        int changed = full || !grid->hasPrev || h != grid->hash[idx];
        if (changed) {
            if (!any) {
                VmTiles_ShowLoading(out);
                if (full)
                    VmTiles_Config(out, grid->cols, grid->rows, grid->tileW, grid->tileH,
                                   grid->w, grid->h);
            }
            VmTiles_SetTileJson(out, idx, json ? json : "[]",
                                s_tileScratch.commands, s_tileScratch.count);
            any = 1;
            if (!full) grid->hash[idx] = h;
        }
        free(json);
    }

    if (!any) return 0;
    VmTiles_HideLoading(out);
    if (!full) grid->hasPrev = 1;
    return 1;
}
