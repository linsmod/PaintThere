#include "jpx_internal.h"

#include <stdio.h>
#include <string.h>

/* 颜色与度量都从 theme 推导，应用不传颜色。 */

static Clay_Color fg(JpxApp *app) {
    return Jpx__Color(app->theme.fgColor, (Clay_Color){30, 30, 30, 255});
}

static Clay_Color accent(JpxApp *app) {
    return Jpx__Color(app->theme.accentColor, (Clay_Color){52, 140, 220, 255});
}

static Clay_Color border(JpxApp *app) {
    return Jpx__Color(app->theme.borderColor, (Clay_Color){180, 186, 194, 255});
}

static Clay_Color mix(Clay_Color a, Clay_Color b, float t) {
    return (Clay_Color){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

/* Clay__OpenTextElement 按值收配置；CLAY_TEXT 宏会把参数当成初始化器，
   传 &cfg 会被塞进结构体首个字段（fontSize 变 0）。这里直接调底层 API。 */
static void jpx_text(Clay_String text, Clay_TextElementConfig config) {
    Clay__OpenTextElement(text, config);
}

static Clay_TextElementConfig text_config(JpxApp *app, Clay_Color color, int align, int wrap) {
    Clay_TextElementConfig config = {0};
    config.fontId = (uint16_t)app->theme.fontId;
    config.fontSize = (uint16_t)app->theme.fontSize;
    config.textColor = color;
    config.textAlignment = align == 1 ? CLAY_TEXT_ALIGN_CENTER
                          : align == 2 ? CLAY_TEXT_ALIGN_RIGHT
                                       : CLAY_TEXT_ALIGN_LEFT;
    config.wrapMode = wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE;
    return config;
}

void Jpx__OpenRoot(JpxApp *app, JpxWindow *win) {
    (void)win;
    Clay__OpenElement();
    Clay__ConfigureOpenElement((Clay_ElementDeclaration){
        .layout = {
            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = {(uint16_t)app->theme.padding, (uint16_t)app->theme.padding,
                        (uint16_t)app->theme.padding, (uint16_t)app->theme.padding},
            .childGap = (uint16_t)app->theme.gap,
        },
        .backgroundColor = Jpx__Color(app->theme.bgColor, (Clay_Color){250, 250, 252, 255}),
    });
}

/* ---------------- 容器 ---------------- */

static void open_flow(int win, Clay_LayoutDirection dir, int growW, int growH) {
    (void)win;
    JpxApp *app = Jpx__App();
    Clay__OpenElement();
    Clay__ConfigureOpenElement((Clay_ElementDeclaration){
        .layout = {
            .sizing = {growW ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIT(0),
                       growH ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIT(0)},
            .layoutDirection = dir,
            .childGap = app ? (uint16_t)app->theme.gap : 8,
            .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER},
        },
    });
}

void Jpx_RowBegin(int win) { open_flow(win, CLAY_LEFT_TO_RIGHT, 1, 0); }
void Jpx_RowEnd(int win) { (void)win; Clay__CloseElement(); }
void Jpx_ColumnBegin(int win) { open_flow(win, CLAY_TOP_TO_BOTTOM, 1, 0); }
void Jpx_ColumnEnd(int win) { (void)win; Clay__CloseElement(); }

void Jpx_PanelBegin(int win) {
    (void)win;
    JpxApp *app = Jpx__App();
    if (!app) return;
    Clay__OpenElement();
    Clay__ConfigureOpenElement((Clay_ElementDeclaration){
        .layout = {
            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = {(uint16_t)app->theme.padding, (uint16_t)app->theme.padding,
                        (uint16_t)app->theme.padding, (uint16_t)app->theme.padding},
            .childGap = (uint16_t)app->theme.gap,
        },
        .backgroundColor = Jpx__Color(app->theme.inputBgColor, (Clay_Color){255, 255, 255, 255}),
        .cornerRadius = CLAY_CORNER_RADIUS((float)app->theme.radius),
        .border = {.color = border(app), .width = CLAY_BORDER_OUTSIDE(1)},
    });
}
void Jpx_PanelEnd(int win) { (void)win; Clay__CloseElement(); }

void Jpx_ScrollBegin(int win, const char *id, int height) {
    JpxApp *app = Jpx__App();
    Clay_ElementId eid = Jpx__Id(win, id ? id : "scroll");
    Clay__OpenElementWithId(eid);
    Clay__ConfigureOpenElement((Clay_ElementDeclaration){
        .layout = {
            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)height)},
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = CLAY_PADDING_ALL(4),
            .childGap = 4,
        },
        /* 滚动偏移在阶段 D 接入 wheel 后填充；先做静态裁剪容器。 */
        .clip = {.vertical = true, .childOffset = {0, 0}},
        .backgroundColor = app ? Jpx__Color(app->theme.inputBgColor,
                                           (Clay_Color){255, 255, 255, 255})
                               : (Clay_Color){255, 255, 255, 255},
    });
}
void Jpx_ScrollEnd(int win) { (void)win; Clay__CloseElement(); }

void Jpx_Spacer(int win, int w, int h) {
    (void)win;
    JpxApp *app = Jpx__App();
    CLAY(Jpx__LocalId(app, "spacer"), {
        .layout = {.sizing = {CLAY_SIZING_FIXED((float)w), CLAY_SIZING_FIXED((float)h)}},
    }) {}
}

void Jpx_Separator(int win) {
    (void)win;
    JpxApp *app = Jpx__App();
    CLAY(Jpx__LocalId(app, "sep"), {
        .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}},
        .backgroundColor = app ? border(app) : (Clay_Color){180, 186, 194, 255},
    }) {}
}

/* ---------------- 控件 ---------------- */

void Jpx_Label(int win, const char *text) {
    JpxTextStyle style = {.wrap = 1, .align = 0};
    Jpx_LabelEx(win, text, &style);
}

void Jpx_LabelEx(int win, const char *text, const JpxTextStyle *style) {
    (void)win;
    JpxApp *app = Jpx__App();
    if (!app) return;
    int align = style ? style->align : 0;
    int wrap = style ? style->wrap : 1;
    Clay_TextElementConfig cfg = text_config(app, fg(app), align, wrap);
    CLAY(Jpx__LocalId(app, "label"), {
        .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}},
    }) {
        jpx_text(Jpx__Str(text), cfg);
    }
}

/* Clay 的 onHover 回调在窗口上下文里不可靠触发，交互统一用
   Clay_Hovered() + WM 指针的按下沿（JpxWindow::pressEdge）判定。 */
static int pressed_this_frame(JpxApp *app, int win, int hovered) {
    if (!hovered) return 0;
    JpxWindow *w = Jpx__Window(app, win);
    return w ? w->pressEdge : 0;
}

int Jpx_Button(int win, const char *id, const char *text) {
    JpxApp *app = Jpx__App();
    if (!app) return 0;
    /* buttonBgColor 非空 = 实体按钮皮肤（XP）：底色与文字都走实体色，
       提亮/压暗幅度也收窄，保持「平面灰块 + 深边」的观感。 */
    const int solid = app->theme.buttonBgColor != NULL;
    Clay_Color base = solid ? Jpx__Color(app->theme.buttonBgColor, accent(app)) : accent(app);
    Clay_Color hover = mix(base, (Clay_Color){255, 255, 255, 255}, solid ? 0.10f : 0.2f);
    if (app->theme.hoverColor) hover = Jpx__Color(app->theme.hoverColor, hover);
    Clay_Color pressedColor = mix(base, (Clay_Color){0, 0, 0, 255}, solid ? 0.12f : 0.28f);
    int down = app->wm && app->wm->pointerDown;
    int hovered = 0;
    Clay_Color textColor = solid
        ? Jpx__Color(app->theme.buttonTextColor, (Clay_Color){0, 0, 0, 255})
        : Jpx__Color(app->theme.accentTextColor, (Clay_Color){255, 255, 255, 255});
    Clay_TextElementConfig cfg = text_config(app, textColor, 1, 0);
    CLAY(Jpx__Id(win, id), {
        .layout = {
            .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
            .padding = solid ? (Clay_Padding){10, 10, 4, 4} : (Clay_Padding){16, 16, 7, 7},
            .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER},
        },
        /* Clay_Hovered() 必须在元素声明内部求值：在 CLAY(...) 之前调用拿到的是
           父容器的 hover，同一窗口里所有按钮会一起亮。 */
        .backgroundColor = !Clay_Hovered() ? base : (down ? pressedColor : hover),
        .cornerRadius = CLAY_CORNER_RADIUS((float)app->theme.radius),
        .border = {.color = mix(base, (Clay_Color){0, 0, 0, 255}, 0.35f),
                   .width = CLAY_BORDER_OUTSIDE(1)},
    }) {
        hovered = Clay_Hovered();
        jpx_text(Jpx__Str(text), cfg);
    }
    int clicked = pressed_this_frame(app, win, hovered);
    if (clicked) Jpx__Invalidate(app);
    return clicked;
}

int Jpx_Checkbox(int win, const char *id, const char *label, int *checked) {
    JpxApp *app = Jpx__App();
    if (!app || !checked) return 0;
    int hovered = 0;
    Clay_TextElementConfig cfg = text_config(app, fg(app), 0, 0);
    CLAY(Jpx__Id(win, id), {
        .layout = {
            .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childGap = 8,
            .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER},
        },
    }) {
        hovered = Clay_Hovered();
        /* XP 的勾选框是白底 + 深色勾；现代主题保持强调色块 + 白勾。 */
        const int solidSkin = app->theme.buttonBgColor != NULL;
        Clay_Color boxBg = Jpx__Color(app->theme.inputBgColor, (Clay_Color){18, 21, 32, 255});
        if (*checked && !solidSkin) boxBg = accent(app);
        Clay_Color markColor = solidSkin
            ? Jpx__Color(app->theme.buttonTextColor, (Clay_Color){0, 0, 0, 255})
            : (Clay_Color){255, 255, 255, 255};
        CLAY(Jpx__LocalId(app, "box"), {
            .layout = {
                .sizing = {CLAY_SIZING_FIXED(16), CLAY_SIZING_FIXED(16)},
                .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER},
            },
            .backgroundColor = boxBg,
            .border = {.color = border(app), .width = CLAY_BORDER_OUTSIDE(1)},
            .cornerRadius = CLAY_CORNER_RADIUS(solidSkin ? 2.0f : 3.0f),
        }) {
            if (*checked) {
                /* font.ttf 没有 U+2713（✓）字形，勾选会画成空白；
                   U+221A（√）字形存在且形状等价。 */
                Clay_String mark = {.length = 3, .chars = "√"};
                jpx_text(mark, text_config(app, markColor, 1, 0));
            }
        }
        jpx_text(Jpx__Str(label), cfg);
    }
    if (pressed_this_frame(app, win, hovered)) {
        *checked = *checked ? 0 : 1;
        Jpx__Invalidate(app);
        return 1;
    }
    return 0;
}

/* 单行文本输入：编辑状态按控件 id 持久化，输入路由在 jpx_window.c 的焦点链。
   选区高亮尚未绘制（语义已正确），caret 用「前段文本 + 竖条 + 后段文本」定位。 */
int Jpx_TextInput(int win, const char *id, char *buf, size_t cap, size_t *len) {
    JpxApp *app = Jpx__App();
    if (!app || !buf || cap == 0 || !id) return 0;
    JpxWindow *w = Jpx__Window(app, win);
    if (!w) return 0;

    size_t n = len ? *len : strlen(buf);
    if (n >= cap) n = cap - 1;

    JpxEditSlot *slot = Jpx__EditSlot(w, id);
    if (!slot) return 0;
    /* 首次挂载或应用改写了缓冲区：重建编辑状态，caret 落到末尾。 */
    if (slot->ed.buf != buf || slot->ed.cap != cap || slot->ed.len != n) {
        Jpx__TextEditInit(&slot->ed, buf, cap, n);
    }
    if (len) *len = slot->ed.len;
    Jpx__FocusRegister(w, id, len, &slot->ed);

    int focused = strcmp(w->focusId, id) == 0;
    int hovered = 0;
    Clay_Color fgColor = fg(app);
    Clay_TextElementConfig cfg = text_config(app, fgColor, 0, 0);

    CLAY(Jpx__Id(win, id), {
        .layout = {
            /* 单行输入框高度固定：空文本时没有子元素，FIT 会算出 0 高、整块被 tile 丢弃。 */
            .sizing = {CLAY_SIZING_GROW(0),
                       CLAY_SIZING_FIXED((float)(app->theme.fontSize + 14))},
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childGap = 0,
            .padding = {8, 8, 6, 6},
            .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER},
        },
        .backgroundColor = Jpx__Color(app->theme.inputBgColor, (Clay_Color){18, 21, 32, 255}),
        /* hover 在元素声明内判定：在 CLAY 之前取会拿到父容器的 hover。 */
        .border = {.color = focused ? accent(app)
                          : (Clay_Hovered() ? mix(border(app), accent(app), 0.35f)
                                            : border(app)),
                   .width = CLAY_BORDER_OUTSIDE(focused ? 2 : 1)},
        .cornerRadius = CLAY_CORNER_RADIUS((float)app->theme.radius),
    }) {
        hovered = Clay_Hovered();
        size_t caret = slot->ed.caret;
        if (caret > slot->ed.len) caret = slot->ed.len;

        size_t selStart = 0, selEnd = 0;
        int hasSel = focused && Jpx__TextEditHasSelection(&slot->ed);
        if (hasSel) Jpx__TextEditSelection(&slot->ed, &selStart, &selEnd);

        if (hasSel) {
            if (selStart > 0) {
                Clay_String before = {.chars = buf, .length = (int32_t)selStart};
                jpx_text(before, cfg);
            }
            Clay_String sel = {.chars = buf + selStart,
                               .length = (int32_t)(selEnd - selStart)};
            CLAY(Jpx__LocalId(app, "selection"), {
                .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}},
                .backgroundColor = Jpx__Color(app->theme.selectedColor,
                                              (Clay_Color){124, 122, 255, 80}),
                .cornerRadius = CLAY_CORNER_RADIUS(2),
            }) {
                jpx_text(sel, cfg);
            }
            if (slot->ed.len > selEnd) {
                Clay_String after = {.chars = buf + selEnd,
                                     .length = (int32_t)(slot->ed.len - selEnd)};
                jpx_text(after, cfg);
            }
        } else if (focused && app->caretOn) {
            if (caret > 0) {
                Clay_String before = {.chars = buf, .length = (int32_t)caret};
                jpx_text(before, cfg);
            }
            CLAY(Jpx__LocalId(app, "caret"), {
                .layout = {.sizing = {CLAY_SIZING_FIXED(1),
                                      CLAY_SIZING_FIXED((float)app->theme.fontSize)}},
                .backgroundColor = fgColor,
            }) {}
            if (slot->ed.len > caret) {
                Clay_String after = {.chars = buf + caret,
                                     .length = (int32_t)(slot->ed.len - caret)};
                jpx_text(after, cfg);
            }
        } else if (slot->ed.len > 0) {
            Clay_String s = {.chars = buf, .length = (int32_t)slot->ed.len};
            jpx_text(s, cfg);
        }
    }

    if (pressed_this_frame(app, win, hovered)) {
        if (!focused) {
            snprintf(w->focusId, sizeof(w->focusId), "%s", id);
            Jpx__TextEditSetCaret(&slot->ed, slot->ed.len);
            app->caretOn = 1;
        }
        Jpx__Invalidate(app);
    }
    return 0;
}

int Jpx_ListBox(int win, const char *id, const char *const *items, int count,
                int *selected, int visibleRows) {
    JpxApp *app = Jpx__App();
    if (!app || !items || count <= 0) return 0;
    if (visibleRows <= 0) visibleRows = count;
    int changed = 0;
    char rowId[32];

    CLAY(Jpx__Id(win, id), {
        .layout = {
            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = CLAY_PADDING_ALL(2),
            .childGap = 2,
        },
        .backgroundColor = Jpx__Color(app->theme.inputBgColor, (Clay_Color){255, 255, 255, 255}),
        .border = {.color = border(app), .width = CLAY_BORDER_OUTSIDE(1)},
    }) {
        for (int i = 0; i < count && i < visibleRows; i++) {
            int isSel = selected && *selected == i;
            int rowHovered = 0;
            snprintf(rowId, sizeof(rowId), "row_%d", i);
            CLAY(Jpx__Id(win, rowId), {
                .layout = {
                    .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                    .padding = {6, 6, 4, 4},
                },
                /* 选中项用 selectedColor（主题的选中蓝），不用 accentColor：
                   accent 是任务按钮渐变的代表色，玻璃主题（longhorn-aero）
                   是半透明白，白底列表上选中项会整行白掉、文字也看不清。 */
                .backgroundColor = isSel ? Jpx__Color(app->theme.selectedColor,
                                                      (Clay_Color){49, 106, 197, 255})
                                         : (Clay_Hovered()
                                                ? mix(border(app), (Clay_Color){255, 255, 255, 255}, 0.5f)
                                                : (Clay_Color){0, 0, 0, 0}),
                .cornerRadius = CLAY_CORNER_RADIUS(2),
            }) {
                rowHovered = Clay_Hovered();
                Clay_Color rowColor = isSel ? (Clay_Color){255, 255, 255, 255}
                                            : fg(app);
                Clay_TextElementConfig rowCfg = text_config(app, rowColor, 0, 0);
                jpx_text(Jpx__Str(items[i]), rowCfg);
            }
            if (pressed_this_frame(app, win, rowHovered)) {
                if (selected && *selected != i) {
                    *selected = i;
                    changed = 1;
                }
            }
        }
    }
    if (changed) Jpx__Invalidate(app);
    return changed;
}

/* ---------------- 可滚动列表 ---------------- */

/* 按 (窗口, id) 找滚动状态。ListBox 是「一屏显示前 N 条」，这里才是真滚动：
   内容用 clip.childOffset 整体平移，滚动上限由 totalRows × 固定行高算出——
   clip 容器的 boundingBox 是裁剪后的高度，反查内容高度是拿不到的。 */
static JpxList *find_list(JpxApp *app, int win, const char *id, int create) {
    for (int i = 0; i < JPX_MAX_LISTS; i++) {
        JpxList *l = &app->lists[i];
        if (l->used && l->winId == win && strcmp(l->id, id) == 0) return l;
    }
    if (!create) return NULL;
    for (int i = 0; i < JPX_MAX_LISTS; i++) {
        JpxList *l = &app->lists[i];
        if (l->used) continue;
        memset(l, 0, sizeof(*l));
        l->used = 1;
        l->winId = win;
        l->rowH = JPX_LIST_ROW_H;
        snprintf(l->id, sizeof(l->id), "%s", id);
        return l;
    }
    return NULL;
}

int Jpx_ListBegin(int win, const char *id, int height, int totalRows) {
    JpxApp *app = Jpx__App();
    if (!app || !id || height <= 0) return 0;
    JpxList *l = find_list(app, win, id, 1);
    if (!l) return 0;

    l->viewH = height;
    l->totalRows = totalRows > 0 ? totalRows : 1;
    int content = l->totalRows * l->rowH;
    l->maxOffset = content > height ? content - height : 0;
    if (l->offset < 0) l->offset = 0;
    if (l->offset > l->maxOffset) l->offset = l->maxOffset;
    l->rowIndex = 0;

    Clay_ElementId eid = Jpx__Id(win, id);
    Clay__OpenElementWithId(eid);
    Clay__ConfigureOpenElement((Clay_ElementDeclaration){
        .layout = {
            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)height)},
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
        },
        .clip = {.vertical = true, .childOffset = {0, -(float)l->offset}},
        .backgroundColor = Jpx__Color(app->theme.inputBgColor, (Clay_Color){255, 255, 255, 255}),
        .border = {.color = border(app), .width = CLAY_BORDER_OUTSIDE(1)},
    });
    app->activeList = (int)(l - app->lists) + 1;
    return 1;
}

int Jpx_ListRowEx(int win, const char *id, const char *const *cols, const int *widths,
                  int ncols, int selected, const char *btnText) {
    JpxApp *app = Jpx__App();
    if (!app || !id || !cols || ncols <= 0) return 0;
    int li = app->activeList;
    if (li <= 0) return 0;
    JpxList *l = &app->lists[li - 1];
    int rowIdx = l->rowIndex++;

    char rowId[128];
    snprintf(rowId, sizeof(rowId), "%s#%dr", id, rowIdx);

    /* 视口外的行只占位、不生成任何绘制命令：20 行全画会直接撞上
       单窗口 64 条配额，表现为帧里内容被静默截断。 */
    int firstVisible = l->offset / l->rowH;
    int lastVisible = (l->offset + l->viewH - 1) / l->rowH;
    if (rowIdx < firstVisible || rowIdx > lastVisible) {
        CLAY(Jpx__Id(win, rowId), {
            .layout = {
                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)l->rowH)},
            },
        }) {}
        return 0;
    }

    int hasBtn = btnText != NULL && btnText[0] != '\0';

    int hovered = 0;
    Clay_ElementId btnEid = {0};
    Clay_Color zebra = (rowIdx & 1) ? (Clay_Color){255, 255, 255, 10}
                                    : (Clay_Color){0, 0, 0, 0};
    CLAY(Jpx__Id(win, rowId), {
        .layout = {
            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)l->rowH)},
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .padding = {8, 8, 0, 0},
            .childGap = 8,
            .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER},
        },
        /* 选中项用 selectedColor（主题的选中蓝），不用 accentColor：
           accent 是任务按钮渐变的代表色，玻璃主题（longhorn-aero）
           是半透明白，白底列表上选中项会整行白掉、文字也看不清。 */
        .backgroundColor = selected ? Jpx__Color(app->theme.selectedColor,
                                                 (Clay_Color){49, 106, 197, 255})
                                   : (Clay_Hovered()
                                          ? mix(border(app), (Clay_Color){255, 255, 255, 255}, 0.28f)
                                          : zebra),
    }) {
        hovered = Clay_Hovered();
        Clay_Color rowColor = selected
                                  ? (Clay_Color){255, 255, 255, 255}
                                  : fg(app);
        Clay_TextElementConfig cfg = text_config(app, rowColor, 0, 0);

        for (int i = 0; i < ncols; i++) {
            char colId[160];
            snprintf(colId, sizeof(colId), "%s#%dr#c%d", id, rowIdx, i);
            int fixedW = widths ? widths[i] : 0;
            CLAY(Jpx__Id(win, colId), {
                .layout = {
                    .sizing = {(i == 0 || fixedW <= 0) ? CLAY_SIZING_GROW(0)
                                                       : CLAY_SIZING_FIXED((float)fixedW),
                               CLAY_SIZING_GROW(0)},
                    .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER},
                },
            }) {
                jpx_text(Jpx__Str(cols[i] ? cols[i] : ""), cfg);
            }
        }

        if (hasBtn) {
            char btnId[160];
            snprintf(btnId, sizeof(btnId), "%s#%dr#btn", id, rowIdx);
            btnEid = Jpx__Id(win, btnId);
            CLAY(btnEid, {
                .layout = {
                    .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                    .padding = {10, 10, 3, 3},
                    .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER},
                },
                .backgroundColor = Clay_Hovered()
                                       ? mix(border(app), accent(app), 0.4f)
                                       : (Clay_Color){40, 45, 62, 255},
                .border = {.color = border(app), .width = CLAY_BORDER_OUTSIDE(1)},
                .cornerRadius = CLAY_CORNER_RADIUS(2),
            }) {
                jpx_text(Jpx__Str(btnText), text_config(app, fg(app), 0, 0));
            }
        }
    }

    int result = 0;
    /* 行与行尾按钮互斥：Clay_Hovered 是父子都真，只靠它就分不开。
       Clay_PointerOver 查的是上一帧元素，第一帧可能漏判，之后正常。 */
    if (hasBtn && Clay_PointerOver(btnEid)) {
        if (pressed_this_frame(app, win, 1)) result |= 2;
    } else if (pressed_this_frame(app, win, hovered)) {
        result |= 1;
    }
    return result;
}

int Jpx_ListRow(int win, const char *id, const char *const *cols, const int *widths,
                int ncols, int selected) {
    return Jpx_ListRowEx(win, id, cols, widths, ncols, selected, NULL);
}

void Jpx_ListVisibleRange(int win, const char *id, int *first, int *last) {
    JpxApp *app = Jpx__App();
    int f = 0, l = -1;
    if (app && id) {
        JpxList *l2 = find_list(app, win, id, 0);
        if (l2) {
            f = l2->offset / l2->rowH;
            l = (l2->offset + l2->viewH - 1) / l2->rowH;
        }
    }
    if (first) *first = f;
    if (last) *last = l;
}

void Jpx_ListEnd(int win) {
    (void)win;
    Clay__CloseElement();
    JpxApp *app = Jpx__App();
    if (app) app->activeList = 0;
}

void Jpx__ListRefreshRects(JpxApp *app, int winId) {
    if (!app) return;
    for (int i = 0; i < JPX_MAX_LISTS; i++) {
        JpxList *l = &app->lists[i];
        if (!l->used || l->winId != winId) continue;
        Clay_ElementData d = Clay_GetElementData(Jpx__Id(winId, l->id));
        if (!d.found) continue;
        l->rx = (int)d.boundingBox.x;
        l->ry = (int)d.boundingBox.y;
        l->rw = (int)d.boundingBox.width;
        l->rh = (int)d.boundingBox.height;
        l->hasRect = 1;
    }
}

int Jpx__ListWheel(JpxApp *app, int winId, int x, int y, int deltaY) {
    if (!app || deltaY == 0) return 0;
    for (int i = 0; i < JPX_MAX_LISTS; i++) {
        JpxList *l = &app->lists[i];
        if (!l->used || l->winId != winId || !l->hasRect) continue;
        if (l->maxOffset <= 0) continue;
        if (x < l->rx || x >= l->rx + l->rw || y < l->ry || y >= l->ry + l->rh) continue;
        /* 标准滚轮一格 deltaY = 120（像素），折算 3 行；正数表示向下滚。
           按比例折算让「大 deltaY」也能一次跳很远，触控板的细碎增量也自然。 */
        int step = deltaY * 3 * l->rowH / 120;
        int next = l->offset + step;
        if (next < 0) next = 0;
        if (next > l->maxOffset) next = l->maxOffset;
        if (next == l->offset) continue;
        l->offset = next;
        Jpx__Invalidate(app);
        return 1;
    }
    return 0;
}
