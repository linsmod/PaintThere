#ifndef VM_WM_H
#define VM_WM_H

#include "clay.h"
#include "clay_frame_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VM_WM_MAX_WINDOWS 8
#define VM_WM_TITLE_HEIGHT 28

typedef Clay_RenderCommandArray (*VmWindowLayout)(void *userData);

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
    /* pointer state in window-local layout coordinates */
    int pointerX, pointerY;
    int pointerDown;
    int clicks;             /* number of PRESSED_THIS_FRAME handled by app */
} VmWindow;

typedef struct {
    int width, height;
    VmWindow windows[VM_WM_MAX_WINDOWS];
    int count;
    int nextZ;
    int focusId;
    /* drag state */
    int dragId;
    int dragOffX, dragOffY;
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

/* Handle raw input JSON body. Returns 1 if the input changed window state. */
int VmWm_HandleInput(VmWm *wm, const char *jsonBody, size_t length);

/* Build a composite frame: background + all windows in z order. */
void VmWm_BuildFrame(VmWm *wm, ClayFrameBuffer *out, uint32_t frameId,
                     const char *(*fontResolver)(uint16_t));

#ifdef __cplusplus
}
#endif

#endif
