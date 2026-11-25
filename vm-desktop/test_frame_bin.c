/* test_frame_bin - binary tile frame round-trip check.
   Build: gcc -std=c11 -I. test_frame_bin.c vm_frame_bin.c vm_serialize_json.c vm_display.c -o test_frame_bin */
#include "vm_frame_bin.h"
#include "vm_serialize_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VmDrawCommand make_rect(const char *id, int x, int y, int w, int h, const char *paint) {
    VmDrawCommand c;
    memset(&c, 0, sizeof(c));
    c.type = VM_CMD_DRAW_RECT;
    VmDisplay_CopyStr(c.id, sizeof(c.id), id);
    VmRect r = {(float)x, (float)y, (float)w, (float)h};
    c.rect = r;
    c.args.drawRect.rect = r;
    VmDisplay_CopyStr(c.args.drawRect.paint, sizeof(c.args.drawRect.paint), paint);
    c.args.drawRect.cornerRadius = 2.5f;
    return c;
}

static VmDrawCommand make_text(const char *id, const char *text) {
    VmDrawCommand c;
    memset(&c, 0, sizeof(c));
    c.type = VM_CMD_DRAW_TEXT;
    VmDisplay_CopyStr(c.id, sizeof(c.id), id);
    VmRect r = {1, 2, 300, 24};
    c.rect = r;
    c.args.drawText.rect = r;
    VmDisplay_CopyStr(c.args.drawText.text, sizeof(c.args.drawText.text), text);
    c.args.drawText.fontId = 1;
    c.args.drawText.fontSize = 14;
    VmDisplay_CopyStr(c.args.drawText.color, sizeof(c.args.drawText.color), "rgba(1,2,3,1)");
    return c;
}

int main(void) {
    VmTiles *a = VmTiles_Create();
    VmTiles_SetFrameId(a, 42);
    VmTiles_Config(a, 4, 3, 200, 200, 800, 600);
    VmTiles_ShowLoading(a);
    VmDrawCommand cmds[2];
    cmds[0] = make_rect("r1", 10, 20, 30, 40, "s_window");
    cmds[1] = make_text("t1", "hello world");
    VmTiles_SetTile(a, 5, cmds, 2);
    VmTiles_HideLoading(a);

    char *jsonA = VmTiles_Finish(a); /* destroys tiles */
    size_t binLen = 0;

    /* Rebuild the same VmTiles to produce binary form. */
    VmTiles *b = VmTiles_Create();
    VmTiles_SetFrameId(b, 42);
    VmTiles_Config(b, 4, 3, 200, 200, 800, 600);
    VmTiles_ShowLoading(b);
    VmTiles_SetTile(b, 5, cmds, 2);
    VmTiles_HideLoading(b);
    uint8_t *bin = VmTiles_FinishBin(b, &binLen);
    VmTiles_Destroy(b);

    if (!jsonA || !bin || !binLen) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }

    uint32_t decodedFrame = 0;
    VmTiles *c = VmFrameBin_ToTiles(bin, binLen, &decodedFrame);
    if (!c) { fprintf(stderr, "decode failed\n"); return 1; }
    char *jsonC = VmTiles_Finish(c);

    int ok = decodedFrame == 42 && strcmp(jsonA, jsonC) == 0;
    printf("jsonA=%zu jsonC=%zu bin=%zu ok=%d\n", strlen(jsonA), strlen(jsonC), binLen, ok);
    if (!ok) {
        fprintf(stderr, "--- A ---\n%s\n--- C ---\n%s\n", jsonA, jsonC);
    }

    free(jsonA);
    free(jsonC);
    free(bin);
    return ok ? 0 : 1;
}
