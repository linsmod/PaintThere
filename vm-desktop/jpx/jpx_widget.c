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
        .border = {.color = border(app), .width = CLAY_BORDER_ALL(1)},
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
    Clay_Color base = accent(app);
    Clay_Color hover = mix(base, (Clay_Color){255, 255, 255, 255}, 0.2f);
    if (app->theme.hoverColor) hover = Jpx__Color(app->theme.hoverColor, hover);
    int hovered = 0;
    Clay_TextElementConfig cfg = text_config(
        app, Jpx__Color(app->theme.accentTextColor, (Clay_Color){255, 255, 255, 255}), 1, 0);
    CLAY(Jpx__Id(win, id), {
        .layout = {
            .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
            .padding = {14, 14, 6, 6},
            .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER},
        },
        .backgroundColor = Clay_Hovered() ? hover : base,
        .cornerRadius = CLAY_CORNER_RADIUS((float)app->theme.radius),
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
        CLAY(Jpx__LocalId(app, "box"), {
            .layout = {.sizing = {CLAY_SIZING_FIXED(16), CLAY_SIZING_FIXED(16)}},
            .backgroundColor = *checked ? accent(app)
                                        : Jpx__Color(app->theme.inputBgColor,
                                                     (Clay_Color){255, 255, 255, 255}),
            .border = {.color = border(app), .width = CLAY_BORDER_ALL(1)},
            .cornerRadius = CLAY_CORNER_RADIUS(2),
        }) {}
        jpx_text(Jpx__Str(label), cfg);
    }
    if (pressed_this_frame(app, win, hovered)) {
        *checked = *checked ? 0 : 1;
        Jpx__Invalidate(app);
        return 1;
    }
    return 0;
}

/* 阶段 C：只做展示与点击聚焦。编辑、caret、选区、剪贴板在阶段 D 补。 */
int Jpx_TextInput(int win, const char *id, char *buf, size_t cap, size_t *len) {
    JpxApp *app = Jpx__App();
    if (!app || !buf) return 0;
    size_t n = len ? *len : strlen(buf);
    if (n >= cap) n = cap ? cap - 1 : 0;
    int hovered = 0;
    Clay_TextElementConfig cfg = text_config(app, fg(app), 0, 0);
    CLAY(Jpx__Id(win, id), {
        .layout = {
            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
            .padding = {8, 8, 6, 6},
        },
        .backgroundColor = Jpx__Color(app->theme.inputBgColor, (Clay_Color){255, 255, 255, 255}),
        .border = {.color = Clay_Hovered() ? accent(app) : border(app), .width = CLAY_BORDER_ALL(1)},
        .cornerRadius = CLAY_CORNER_RADIUS((float)app->theme.radius),
    }) {
        hovered = Clay_Hovered();
        Clay_String s = {.chars = buf, .length = (int32_t)n};
        jpx_text(s, cfg);
    }
    if (pressed_this_frame(app, win, hovered)) Jpx__Invalidate(app);
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
        .border = {.color = border(app), .width = CLAY_BORDER_ALL(1)},
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
                .backgroundColor = isSel ? accent(app)
                                         : (Clay_Hovered()
                                                ? mix(border(app), (Clay_Color){255, 255, 255, 255}, 0.5f)
                                                : (Clay_Color){0, 0, 0, 0}),
                .cornerRadius = CLAY_CORNER_RADIUS(2),
            }) {
                rowHovered = Clay_Hovered();
                Clay_Color rowColor = isSel ? Jpx__Color(app->theme.accentTextColor,
                                                         (Clay_Color){255, 255, 255, 255})
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
