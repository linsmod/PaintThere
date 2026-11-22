#include "vm_theme.h"

#include <string.h>

/* 默认主题保持与 demo_wm 既有视觉一致，切换主题只需替换这一份表。 */
static const VmTheme kDefaultTheme = {
    .chrome = {
        .titleHeight = 28,
        .borderWidth = 2,
        .windowStyleId = "s_window",
        .titleFocusColor = "rgba(38,110,190,1)",
        .titleIdleColor = "rgba(120,126,134,1)",
        .borderFocusColor = "rgba(38,110,190,1)",
        .borderIdleColor = "rgba(90,96,104,1)",
        .titleFontId = 0,
        .titleFontSize = 14,
    },
    .desktopFrom = "rgba(24,26,32,1)",
    .desktopTo = "rgba(48,54,66,1)",
    .panelStyleId = "s_panel",
    .panelColor = "rgba(32,36,44,1)",
    .bgColor = "rgba(250,250,252,1)",
    .fgColor = "rgba(24,26,32,1)",
    .borderColor = "rgba(180,186,194,1)",
    .accentColor = "rgba(52,140,220,1)",
    .accentTextColor = "rgba(255,255,255,1)",
    .inputBgColor = "rgba(255,255,255,1)",
    .disabledColor = "rgba(170,176,184,1)",
    .selectedColor = "rgba(52,140,220,0.25)",
    .radius = 4,
    .padding = 8,
    .gap = 8,
    .fontId = 0,
    .fontSize = 14,
};

const VmTheme *VmTheme_Default(void) {
    return &kDefaultTheme;
}

static VmDrawCommand *append_pattern(VmDisplayList *out, const char *defId, const char *patternId,
                                     VmPatternType type) {
    VmDrawCommand *c = VmDisplay_Append(out, VM_CMD_DEFINE_PATTERN, defId);
    if (!c) return NULL;
    VmDisplay_CopyStr(c->args.definePattern.patternId,
                      sizeof(c->args.definePattern.patternId), patternId);
    c->args.definePattern.type = type;
    return c;
}

static VmDrawCommand *append_style(VmDisplayList *out, const char *defId, const char *styleId,
                                   const char *bgPattern) {
    VmDrawCommand *c = VmDisplay_Append(out, VM_CMD_DEFINE_STYLE, defId);
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

    VmDrawCommand *p = append_pattern(out, "def_p_desktop", "p_desktop", VM_PATTERN_GRADIENT);
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
