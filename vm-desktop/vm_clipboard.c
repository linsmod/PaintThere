#include "vm_clipboard.h"

#include <string.h>

static char g_text[VM_CLIPBOARD_CAP];
static int g_len;

void VmClipboard_SetText(const char *utf8) {
    if (!utf8) { VmClipboard_Clear(); return; }
    size_t n = strlen(utf8);
    if (n >= (size_t)VM_CLIPBOARD_CAP) n = (size_t)VM_CLIPBOARD_CAP - 1;
    memcpy(g_text, utf8, n);
    g_text[n] = '\0';
    g_len = (int)n;
}

int VmClipboard_GetText(char *out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = g_len < cap - 1 ? g_len : cap - 1;
    if (n <= 0) { out[0] = '\0'; return 0; }
    memcpy(out, g_text, (size_t)n);
    out[n] = '\0';
    return n;
}

int VmClipboard_HasText(void) {
    return g_len > 0;
}

void VmClipboard_Clear(void) {
    g_text[0] = '\0';
    g_len = 0;
}
