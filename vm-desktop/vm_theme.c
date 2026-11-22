#include "vm_theme.h"

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
}
