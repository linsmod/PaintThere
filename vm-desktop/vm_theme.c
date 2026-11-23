#include "vm_theme.h"

#include <stdio.h>
#include <string.h>

/* 现代深色主题：蓝紫强调 + 冷灰层次。切换主题只需替换这一份表。 */
static const VmTheme kDefaultTheme = {
    .chrome = {
        .titleHeight = 30,
        .borderWidth = 1,
        .windowStyleId = "s_window",
        .titleFocusColor = "rgba(86,84,176,1)",
        .titleIdleColor = "rgba(34,38,54,1)",
        .borderFocusColor = "rgba(124,122,255,1)",
        .borderIdleColor = "rgba(58,64,84,1)",
        .titleFontId = 0,
        .titleFontSize = 14,
        .showShadow = 1,
    },
    .desktopFrom = "rgba(13,15,24,1)",
    .desktopTo = "rgba(26,30,46,1)",
    .panelStyleId = "s_panel",
    .panelColor = "rgba(16,18,28,1)",
    .bgColor = "rgba(26,29,42,1)",
    .fgColor = "rgba(226,231,240,1)",
    .borderColor = "rgba(72,80,104,1)",
    .accentColor = "rgba(124,122,255,1)",
    .accentTextColor = "rgba(255,255,255,1)",
    .inputBgColor = "rgba(18,21,32,1)",
    .disabledColor = "rgba(74,80,98,1)",
    .selectedColor = "rgba(124,122,255,0.30)",
    .radius = 8,
    .padding = 12,
    .gap = 10,
    .fontId = 0,
    .fontSize = 14,
    .panelHeight = 36,
};

const VmTheme *VmTheme_Default(void) {
    return &kDefaultTheme;
}

static VmResourceCommand *append_pattern(VmDisplayList *out, const char *defId, const char *patternId,
                                         VmPatternType type) {
    VmResourceCommand *c = VmDisplay_AppendResource(out, VM_CMD_DEFINE_PATTERN, defId);
    if (!c) return NULL;
    VmDisplay_CopyStr(c->args.definePattern.patternId,
                      sizeof(c->args.definePattern.patternId), patternId);
    c->args.definePattern.type = type;
    return c;
}

static VmResourceCommand *append_style(VmDisplayList *out, const char *defId, const char *styleId,
                                       const char *bgPattern) {
    VmResourceCommand *c = VmDisplay_AppendResource(out, VM_CMD_DEFINE_STYLE, defId);
    if (!c) return NULL;
    VmDisplay_CopyStr(c->args.defineStyle.styleId,
                      sizeof(c->args.defineStyle.styleId), styleId);
    if (bgPattern) {
        VmDisplay_CopyStr(c->args.defineStyle.bgPattern,
                          sizeof(c->args.defineStyle.bgPattern), bgPattern);
    }
    return c;
}

/* 一组竖直渐变：pattern p_<name> + style s_<name>（style 只做画布引用的别名）。
   前端 drawRect 的 paint 命中 styleCache 时才会走渐变，因此调用方填 style id。 */
static void append_gradient_pair(VmDisplayList *out, const char *name,
                                 const char *from, const char *to) {
    if (!from || !to) return;
    char pid[64], sid[64], did[80];
    snprintf(pid, sizeof(pid), "p_%s", name);
    snprintf(sid, sizeof(sid), "s_%s", name);
    snprintf(did, sizeof(did), "def_%s", pid);
    VmResourceCommand *p = append_pattern(out, did, pid, VM_PATTERN_GRADIENT);
    if (!p) return;
    VmDisplay_CopyStr(p->args.definePattern.from, sizeof(p->args.definePattern.from), from);
    VmDisplay_CopyStr(p->args.definePattern.to, sizeof(p->args.definePattern.to), to);
    VmDisplay_CopyStr(p->args.definePattern.direction,
                      sizeof(p->args.definePattern.direction), "0,0,0,1");
    append_style(out, sid, sid, pid);
}

void VmTheme_AppendResources(VmDisplayList *out, const VmTheme *theme) {
    if (!out) return;
    const VmTheme *t = theme ? theme : &kDefaultTheme;

    VmResourceCommand *p = append_pattern(out, "def_p_desktop", "p_desktop", VM_PATTERN_GRADIENT);
    if (p) {
        VmDisplay_CopyStr(p->args.definePattern.from, sizeof(p->args.definePattern.from),
                          t->desktopFrom);
        VmDisplay_CopyStr(p->args.definePattern.to, sizeof(p->args.definePattern.to),
                          t->desktopTo);
        VmDisplay_CopyStr(p->args.definePattern.direction,
                          sizeof(p->args.definePattern.direction), "diagonal");
    }

    p = append_pattern(out, "def_p_window", "p_window", VM_PATTERN_SOLID);
    if (p) {
        VmDisplay_CopyStr(p->args.definePattern.color, sizeof(p->args.definePattern.color),
                          t->bgColor);
    }

    p = append_pattern(out, "def_p_panel", "p_panel", VM_PATTERN_SOLID);
    if (p) {
        VmDisplay_CopyStr(p->args.definePattern.color, sizeof(p->args.definePattern.color),
                          t->panelColor);
    }

    append_style(out, "def_s_desktop", "s_desktop", "p_desktop");
    append_style(out, "def_s_window", t->chrome.windowStyleId, "p_window");
    append_style(out, "def_s_panel", t->panelStyleId, "p_panel");

    /* XP 皮肤：成对颜色非空才生成，默认深色主题不产出这些资源。 */
    append_gradient_pair(out, "title_active", t->titleActiveFrom, t->titleActiveTo);
    append_gradient_pair(out, "title_idle", t->titleIdleFrom, t->titleIdleTo);
    append_gradient_pair(out, "start", t->startFrom, t->startTo);
    append_gradient_pair(out, "task", t->taskButtonFrom, t->taskButtonTo);
    append_gradient_pair(out, "task_active", t->taskButtonActiveFrom, t->taskButtonActiveTo);
    append_gradient_pair(out, "tray", t->trayFrom, t->trayTo);
    /* 标题栏三个按钮：最小化/最大化浅蓝，关闭红。Luna 的按钮是竖直渐变。 */
    if (t->titleActiveFrom) {
        append_gradient_pair(out, "cap_btn", "rgba(102,156,240,1)", "rgba(48,107,206,1)");
        append_gradient_pair(out, "cap_close", "rgba(234,110,92,1)", "rgba(190,56,38,1)");
    }
}
