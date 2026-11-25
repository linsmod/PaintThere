#include "ui_focus.h"

void VmUiFocus_Reset(VmUiFocus *f) {
    if (!f) return;
    f->count = 0;
    f->current = -1;
}

int VmUiFocus_Contains(const VmUiFocus *f, int id) {
    if (!f) return 0;
    for (int i = 0; i < f->count; i++) {
        if (f->ids[i] == id) return 1;
    }
    return 0;
}

int VmUiFocus_Add(VmUiFocus *f, int id) {
    if (!f || id < 0) return 0;
    if (VmUiFocus_Contains(f, id)) return 0;
    if (f->count >= VM_UI_MAX_FOCUS) return 0;
    f->ids[f->count++] = id;
    return 1;
}

void VmUiFocus_Remove(VmUiFocus *f, int id) {
    int w = 0;
    if (!f) return;
    for (int i = 0; i < f->count; i++) {
        if (f->ids[i] == id) continue;
        f->ids[w++] = f->ids[i];
    }
    f->count = w;
    if (f->current == id) f->current = -1;
}

void VmUiFocus_Set(VmUiFocus *f, int id) {
    if (!f) return;
    f->current = VmUiFocus_Contains(f, id) ? id : -1;
}

int VmUiFocus_Get(const VmUiFocus *f) {
    return f ? f->current : -1;
}

int VmUiFocus_Cycle(VmUiFocus *f, int dir) {
    int idx = -1;
    if (!f || f->count <= 0) return -1;
    if (dir == 0) dir = 1;

    for (int i = 0; i < f->count; i++) {
        if (f->ids[i] == f->current) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        idx = dir > 0 ? 0 : f->count - 1;
    } else {
        idx = (idx + (dir > 0 ? 1 : -1) + f->count) % f->count;
    }
    f->current = f->ids[idx];
    return f->current;
}
