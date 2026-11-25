#include "ui_clay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clay.h"
#include "clay_frame_adapter.h"

#include "ui_focus.h"
#include "ui_text_edit.h"
#include "vm_font.h"

#define UI_MAX_EDITS 8
#define UI_FONT_ID 0
#define UI_FONT_SIZE 14

static const Clay_Color UI_BG = {43, 45, 49, 255};
static const Clay_Color UI_TEXT = {232, 236, 242, 255};
static const Clay_Color UI_BTN = {48, 96, 180, 255};
static const Clay_Color UI_BTN_HOVER = {62, 116, 206, 255};
static const Clay_Color UI_BTN_DOWN = {36, 76, 150, 255};
static const Clay_Color UI_INPUT_BG = {24, 26, 30, 255};
static const Clay_Color UI_BORDER = {90, 96, 104, 255};
static const Clay_Color UI_FOCUS = {90, 150, 240, 255};

typedef struct {
    int id;
    char *buf;
    size_t cap;
    VmUiTextEdit ed;
    int used;
} UiEditSlot;

struct VmUiClay {
    Clay_Context *ctx;
    VmUiFocus focus;
    UiEditSlot edits[UI_MAX_EDITS];

    int pointerX, pointerY;
    int pointerDown;
    int pressEdge, releaseEdge;
    int hotId, pressId, clickId;
    int caretOn;
};

/* 与前端 stb_truetype 同源；字体未就绪时退回估算，保证首帧不崩。 */
static Clay_Dimensions measure_text_cb(Clay_StringSlice text,
                                       Clay_TextElementConfig *config, void *userData) {
    (void)userData;
    if (VmFont_IsLoaded(config->fontId)) {
        VmFontMetrics m = VmFont_Measure(config->fontId, config->fontSize,
                                         text.chars, text.length);
        return (Clay_Dimensions){(float)m.width, (float)m.lineStep};
    }
    return (Clay_Dimensions){(float)text.length * (float)config->fontSize * 0.6f,
                             (float)config->fontSize};
}

static void handle_errors(Clay_ErrorData data) {
    fwrite(data.errorText.chars, 1, data.errorText.length, stderr);
}

static Clay_ElementId hash_id(const char *prefix, int id) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%s/%d", prefix, id);
    Clay_String s = {.chars = buf, .length = n > 0 ? (int32_t)n : 0};
    return Clay__HashString(s, 0);
}

static Clay_TextElementConfig text_cfg(Clay_Color color) {
    Clay_TextElementConfig c = {0};
    c.textColor = color;
    c.fontId = UI_FONT_ID;
    c.fontSize = UI_FONT_SIZE;
    return c;
}

static void clay_text(const char *s, Clay_Color color) {
    Clay_String str = {.chars = s ? s : "", .length = s ? (int32_t)strlen(s) : 0};
    CLAY_TEXT(str, text_cfg(color));
}

static UiEditSlot *find_edit(VmUiClay *ui, int id) {
    for (int i = 0; i < UI_MAX_EDITS; i++) {
        if (ui->edits[i].used && ui->edits[i].id == id) return &ui->edits[i];
    }
    return NULL;
}

static UiEditSlot *ensure_edit(VmUiClay *ui, int id, char *buf, size_t cap) {
    UiEditSlot *slot = find_edit(ui, id);
    if (slot) {
        if (slot->buf != buf || slot->cap != cap) {
            slot->buf = buf;
            slot->cap = cap;
            VmUiTextEdit_Init(&slot->ed, buf, cap, buf ? strlen(buf) : 0);
        }
        return slot;
    }
    for (int i = 0; i < UI_MAX_EDITS; i++) {
        if (ui->edits[i].used) continue;
        slot = &ui->edits[i];
        slot->used = 1;
        slot->id = id;
        slot->buf = buf;
        slot->cap = cap;
        VmUiTextEdit_Init(&slot->ed, buf, cap, buf ? strlen(buf) : 0);
        return slot;
    }
    return NULL;
}

VmUiClay *VmUiClay_Create(void) {
    VmUiClay *ui;
    uint64_t total;
    Clay_Arena arena;
    Clay_Context *ctx;

    ui = (VmUiClay *)calloc(1, sizeof(*ui));
    if (!ui) return NULL;
    total = Clay_MinMemorySize();
    arena = Clay_CreateArenaWithCapacityAndMemory(total, malloc(total));
    ctx = Clay_Initialize(arena, (Clay_Dimensions){400.0f, 300.0f},
                          (Clay_ErrorHandler){handle_errors});
    if (!ctx) {
        free(ui);
        return NULL;
    }
    ui->ctx = ctx;
    ui->caretOn = 1;
    ui->pressId = -1;
    ui->clickId = -1;
    VmUiFocus_Reset(&ui->focus);
    Clay_SetMeasureTextFunction(measure_text_cb, NULL);
    return ui;
}

void VmUiClay_Destroy(VmUiClay *ui) {
    free(ui);
}

void VmUiClay_Begin(VmUiClay *ui, int w, int h) {
    if (!ui) return;
    Clay_SetCurrentContext(ui->ctx);
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)w, (float)h});
    Clay_SetPointerState((Clay_Vector2){(float)ui->pointerX, (float)ui->pointerY},
                         ui->pointerDown);
    Clay_BeginLayout();
    /* 根容器：纵向排列 + 背景。Clay 的默认根是横向，不配置会把控件排到屏幕外。 */
    Clay__OpenElement();
    Clay__ConfigureOpenElement((Clay_ElementDeclaration){
        .layout = {
            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = {12, 12, 12, 12},
            .childGap = 8,
        },
        .backgroundColor = UI_BG,
    });
    ui->hotId = -1;
}

void VmUiClay_Label(VmUiClay *ui, const char *text) {
    if (!ui) return;
    CLAY(hash_id("ui/label", (int)strlen(text ? text : "")), {
        .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}},
    }) {
        clay_text(text, UI_TEXT);
    }
}

int VmUiClay_Button(VmUiClay *ui, int id, const char *text) {
    Clay_ElementId eid;
    int hovered = 0;
    if (!ui) return 0;
    eid = hash_id("ui/btn", id);
    CLAY(eid, {
        .layout = {
            .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
            .padding = {14, 14, 6, 6},
            .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER},
        },
        .backgroundColor = ui->pointerDown ? UI_BTN_DOWN : UI_BTN,
        .cornerRadius = CLAY_CORNER_RADIUS(4.0f),
        .border = {.color = UI_BORDER, .width = CLAY_BORDER_OUTSIDE(1)},
    }) {
        hovered = Clay_Hovered();
        if (hovered && !ui->pointerDown) {
            /* 无法在宏内改 backgroundColor，hover 用边框提示。 */
        }
        clay_text(text, UI_TEXT);
    }
    if (hovered) {
        ui->hotId = id;
        if (ui->pressEdge) ui->pressId = id;
    }
    return ui->clickId == id;
}

int VmUiClay_CheckBox(VmUiClay *ui, int id, const char *label, int *checked) {
    Clay_ElementId eid;
    int hovered = 0;
    if (!ui || !checked) return 0;
    eid = hash_id("ui/chk", id);
    CLAY(eid, {
        .layout = {
            .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
            .padding = {4, 4, 4, 4},
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childGap = 8,
            .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER},
        },
    }) {
        hovered = Clay_Hovered();
        CLAY(hash_id("ui/chk/box", id), {
            .layout = {.sizing = {CLAY_SIZING_FIXED(14), CLAY_SIZING_FIXED(14)}},
            .backgroundColor = *checked ? UI_BTN : UI_INPUT_BG,
            .cornerRadius = CLAY_CORNER_RADIUS(2.0f),
            .border = {.color = UI_BORDER, .width = CLAY_BORDER_ALL(1)},
        }) {}
        clay_text(label, UI_TEXT);
    }
    if (hovered) {
        ui->hotId = id;
        if (ui->pressEdge) ui->pressId = id;
    }
    if (ui->clickId == id) {
        *checked = !*checked;
        return 1;
    }
    return 0;
}

int VmUiClay_TextInput(VmUiClay *ui, int id, char *buf, size_t cap, int width) {
    Clay_ElementId eid;
    UiEditSlot *slot;
    int hovered = 0;
    int focused;
    if (!ui || !buf || cap == 0) return 0;
    slot = ensure_edit(ui, id, buf, cap);
    if (!slot) return 0;
    VmUiFocus_Add(&ui->focus, id);   /* 登记到 Tab 链 */
    focused = VmUiFocus_Get(&ui->focus) == id;
    eid = hash_id("ui/in", id);
    CLAY(eid, {
        .layout = {
            .sizing = {CLAY_SIZING_FIXED((float)width), CLAY_SIZING_FIT(0)},
            .padding = {8, 8, 5, 5},
        },
        .backgroundColor = UI_INPUT_BG,
        .cornerRadius = CLAY_CORNER_RADIUS(3.0f),
        .border = {.color = focused ? UI_FOCUS : UI_BORDER,
                   .width = CLAY_BORDER_ALL(1)},
    }) {
        hovered = Clay_Hovered();
        clay_text(buf, UI_TEXT);
    }
    if (hovered) {
        ui->hotId = id;
        if (ui->pressEdge) {
            ui->pressId = id;
            VmUiFocus_Set(&ui->focus, id);
        }
    }
    return focused && ui->pressEdge && hovered;
}

void VmUiClay_End(VmUiClay *ui, VmCmdBuffer *out) {
    Clay_RenderCommandArray cmds;
    if (!ui) return;
    Clay__CloseElement();   /* 关闭 Begin 里打开的根容器 */
    cmds = Clay_EndLayout(0.016f);
    if (out)
        ClayFrame_AppendCommands(out, cmds.internalArray, cmds.length, 0, 0, NULL, NULL);
    ui->pressEdge = 0;
    ui->releaseEdge = 0;
    ui->clickId = -1;
}

void VmUiClay_Pointer(VmUiClay *ui, int x, int y, int down) {
    if (!ui) return;
    ui->pointerX = x;
    ui->pointerY = y;
    if (down) {
        ui->pointerDown = 1;
        ui->pressEdge = 1;
        ui->clickId = -1;
    } else if (ui->pointerDown) {
        ui->pointerDown = 0;
        ui->releaseEdge = 1;
        if (ui->hotId == ui->pressId && ui->pressId >= 0) ui->clickId = ui->pressId;
        ui->pressId = -1;
    }
}

int VmUiClay_Key(VmUiClay *ui, const char *key, int ctrl, int shift) {
    int focused;
    UiEditSlot *slot;
    if (!ui || !key) return 0;
    if (strcmp(key, "Tab") == 0) {
        VmUiFocus_Cycle(&ui->focus, shift ? -1 : 1);
        return 1;
    }
    focused = VmUiFocus_Get(&ui->focus);
    slot = focused >= 0 ? find_edit(ui, focused) : NULL;
    if (!slot) return 0;
    return VmUiTextEdit_Key(&slot->ed, key, ctrl, shift);
}

void VmUiClay_SetCaret(VmUiClay *ui, int on) {
    if (ui) ui->caretOn = on ? 1 : 0;
}
