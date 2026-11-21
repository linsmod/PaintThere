#ifndef VM_WM_H
#define VM_WM_H

#include <time.h>

#include "clay.h"
#include "clay_frame_adapter.h"
#include "vm_display.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VM_WM_MAX_WINDOWS 8
#define VM_WM_TITLE_HEIGHT 28
#define VM_WM_MAX_CURSORS 16

typedef Clay_RenderCommandArray (*VmWindowLayout)(void *userData);
/* Guest-internal key handler: return 1 if the key was consumed. */
typedef int (*VmWindowKeyHandler)(int windowId, const char *key, const char *code,
                                  int isDown, void *userData);

typedef struct {
    int id;
    const char *title;
    int x, y, w, h;
    int z;            /* higher is on top */
    int focused;
    int minimized;
    Clay_Context *clayCtx;
    VmWindowLayout layout;
    void *userData;
    VmWindowKeyHandler keyHandler;
    void *keyUserData;
    /* pointer state in window-local layout coordinates */
    int pointerX, pointerY;
    int pointerDown;
    int clicks;             /* number of PRESSED_THIS_FRAME handled by app */
} VmWindow;

typedef struct {
    char id[64];
    char color[32];
    int x, y;
    int visible;
    time_t lastSeen;   /* updated on every input from this client */
} VmCursor;

typedef struct {
    int width, height;
    VmWindow windows[VM_WM_MAX_WINDOWS];
    int count;
    int nextZ;
    int focusId;
    /* drag state */
    int dragId;
    int dragOffX, dragOffY;
    /* multi-client cursors (guest-side only, drawn as primitives) */
    VmCursor cursors[VM_WM_MAX_CURSORS];
    int cursorCount;
    char focusClientId[64];
} VmWm;

VmWm *VmWm_Create(int width, int height);
void VmWm_Free(VmWm *wm);

VmWindow *VmWm_AddWindow(VmWm *wm, int id, const char *title,
                         int x, int y, int w, int h,
                         VmWindowLayout layout, void *userData);
void VmWm_RemoveWindow(VmWm *wm, int id);
VmWindow *VmWm_FindWindow(VmWm *wm, int id);
VmWindow *VmWm_HitTest(VmWm *wm, int x, int y);
void VmWm_Focus(VmWm *wm, int id);
void VmWm_Raise(VmWm *wm, int id);
void VmWm_Move(VmWm *wm, int id, int x, int y);
void VmWm_SetKeyHandler(VmWm *wm, int id, VmWindowKeyHandler handler, void *userData);

/* Handle raw input JSON body. Returns 1 if the input changed window state. */
int VmWm_HandleInput(VmWm *wm, const char *jsonBody, size_t length);

/* Build a composite frame: background + all windows in z order. */
void VmWm_BuildFrame(VmWm *wm, VmDisplayList *out, uint32_t frameId,
                     const char *(*fontResolver)(uint16_t));

#ifdef __cplusplus
}
#endif

#endif
