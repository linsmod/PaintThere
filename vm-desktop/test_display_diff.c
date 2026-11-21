/* Unit check for protocol-neutral VmDisplayList diff.
   Build: gcc -std=c11 -I. test_display_diff.c vm_display.c -o test_display_diff */
#include "vm_display.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    VmDisplayList a, b;
    VmDisplay_Init(&a);
    VmDisplay_Init(&b);

    VmDrawCommand *x = VmDisplay_Append(&a, VM_CMD_DRAW_RECT, "r1");
    VmRect r = {10, 20, 30, 40};
    x->rect = r;
    x->args.drawRect.rect = r;
    VmDisplay_CopyStr(x->args.drawRect.paint, sizeof(x->args.drawRect.paint), "s_window");
    x->args.drawRect.cornerRadius = 2;

    VmDrawCommand *y = VmDisplay_Append(&b, VM_CMD_DRAW_RECT, "r1");
    y->rect = r;
    y->args.drawRect.rect = r;
    VmDisplay_CopyStr(y->args.drawRect.paint, sizeof(y->args.drawRect.paint), "s_window");
    y->args.drawRect.cornerRadius = 2;

    int same = VmDisplay_CommandEqual(x, y);
    VmDisplay_CopyStr(y->args.drawRect.paint, sizeof(y->args.drawRect.paint), "other");
    int changed = VmDisplay_CommandEqual(x, y);

    printf("same=%d changed=%d\n", same, changed);
    return (same == 1 && changed == 0) ? 0 : 1;
}
