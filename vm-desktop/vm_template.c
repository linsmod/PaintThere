#include "vm_template.h"

#include <stdio.h>

void VmCmd_AppendRect(VmCmdBuffer *out, const char *id, int x, int y, int w, int h,
                      const char *paint, float radius) {
    VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_RECT, id);
    if (!c) return;
    VmRect r = {(float)x, (float)y, (float)w, (float)h};
    c->rect = r;
    c->args.drawRect.rect = r;
    VmDisplay_CopyStr(c->args.drawRect.paint, sizeof(c->args.drawRect.paint), paint);
    c->args.drawRect.cornerRadius = radius;
}

void VmTemplate_PaintLayers(VmCmdBuffer *out, const VmTaskButtonLayer *st,
                            const char *idPrefix, int id,
                            int x, int y, int w, int h, float radius,
                            const char *hlPattern) {
    char idbuf[64];
    if (!out || !st || !idPrefix) return;

    /* 外 4 边：各状态有自己的边色，不做 bevel 反转。 */
    if (st->borderTop) {
        snprintf(idbuf, sizeof(idbuf), "%s_top_%d", idPrefix, id);
        VmCmd_AppendRect(out, idbuf, x, y, w, 1, st->borderTop, 0.0f);
    }
    if (st->borderLeft) {
        snprintf(idbuf, sizeof(idbuf), "%s_left_%d", idPrefix, id);
        VmCmd_AppendRect(out, idbuf, x, y + 1, 1, h - 2, st->borderLeft, 0.0f);
    }
    if (st->borderBottom) {
        snprintf(idbuf, sizeof(idbuf), "%s_bottom_%d", idPrefix, id);
        VmCmd_AppendRect(out, idbuf, x, y + h - 1, w, 1, st->borderBottom, 0.0f);
    }
    if (st->borderRight) {
        snprintf(idbuf, sizeof(idbuf), "%s_right_%d", idPrefix, id);
        VmCmd_AppendRect(out, idbuf, x + w - 1, y + 1, 1, h - 2, st->borderRight, 0.0f);
    }

    /* 内 3 边。 */
    if (st->innerLeft) {
        snprintf(idbuf, sizeof(idbuf), "%s_il_%d", idPrefix, id);
        VmCmd_AppendRect(out, idbuf, x + 1, y + 1, 1, h - 3, st->innerLeft, 0.0f);
    }
    if (st->innerRight) {
        snprintf(idbuf, sizeof(idbuf), "%s_ir_%d", idPrefix, id);
        VmCmd_AppendRect(out, idbuf, x + w - 2, y + 1, 1, h - 3, st->innerRight, 0.0f);
    }
    if (st->innerBottom) {
        snprintf(idbuf, sizeof(idbuf), "%s_ib_%d", idPrefix, id);
        VmCmd_AppendRect(out, idbuf, x + 1, y + h - 2, w - 2, 1, st->innerBottom, 0.0f);
    }

    /* 对角高光层：只有层集给了至少两个 stop 才画。 */
    if (st->hlStopCount >= 2 && hlPattern && *hlPattern) {
        snprintf(idbuf, sizeof(idbuf), "%s_hl_%d", idPrefix, id);
        VmCmd_AppendRect(out, idbuf, x + 2, y + 1, w - 4, h - 3, hlPattern, radius);
    }
}
