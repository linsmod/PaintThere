#include "ui_clay_demo.h"

#include <stdio.h>

#include "ui_clay.h"

static VmUiClay *g_ui;
static char g_name[64];
static int g_notify;
static int g_remember;

int ClayUi_Init(void) {
    g_ui = VmUiClay_Create();
    return g_ui != NULL;
}

void ClayUi_Paint(VmCmdBuffer *out, int w, int h) {
    if (!g_ui) return;
    VmUiClay_Begin(g_ui, w, h);
    VmUiClay_Label(g_ui, "vm_ui_clay form (Clay backend)");
    VmUiClay_TextInput(g_ui, 10, g_name, sizeof(g_name), w - 32);
    VmUiClay_CheckBox(g_ui, 11, "notify", &g_notify);
    VmUiClay_CheckBox(g_ui, 12, "remember", &g_remember);
    if (VmUiClay_Button(g_ui, 13, "OK")) {
        fprintf(stderr, "[app] clay ok name='%s' notify=%d remember=%d\n",
                g_name, g_notify, g_remember);
        fflush(stderr);
    }
    VmUiClay_End(g_ui, out);
}

void ClayUi_Pointer(int x, int y, int down) {
    VmUiClay_Pointer(g_ui, x, y, down);
}

int ClayUi_Key(const char *key, int ctrl, int shift) {
    return VmUiClay_Key(g_ui, key, ctrl, shift);
}

void ClayUi_SetCaret(int on) {
    VmUiClay_SetCaret(g_ui, on);
}
