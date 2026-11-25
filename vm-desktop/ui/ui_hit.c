#include "ui_hit.h"

int VmUiHit_Test(const VmUiHitRect *rects, int count, int x, int y) {
    if (!rects) return -1;
    for (int i = count - 1; i >= 0; i--) {
        const VmUiHitRect *r = &rects[i];
        if (!r->enabled) continue;
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return r->id;
    }
    return -1;
}
