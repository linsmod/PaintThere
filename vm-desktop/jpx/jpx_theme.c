#include "jpx_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const JpxTheme kDefaultTheme = {
    .bgColor = "rgba(26,29,42,1)",
    .fgColor = "rgba(226,231,240,1)",
    .borderColor = "rgba(72,80,104,1)",
    .accentColor = "rgba(124,122,255,1)",
    .accentTextColor = "rgba(255,255,255,1)",
    .inputBgColor = "rgba(18,21,32,1)",
    .disabledColor = "rgba(74,80,98,1)",
    .selectedColor = "rgba(124,122,255,0.30)",
    .hoverColor = NULL,
    .fontId = 0,
    .fontSize = 14,
    .radius = 8,
    .padding = 12,
    .gap = 10,
};

const JpxTheme *Jpx__DefaultTheme(void) {
    return &kDefaultTheme;
}

static float parse_float(const char **s) {
    char *end = NULL;
    float v = strtof(*s, &end);
    if (end == *s) return 0.0f;
    *s = end;
    return v;
}

Clay_Color Jpx__Color(const char *text, Clay_Color fallback) {
    if (!text || !*text) return fallback;

    /* rgb(...) / rgba(...)：先定位左括号，别被函数名里的字母带偏。 */
    const char *paren = strchr(text, '(');
    if (paren) {
        const char *p = paren + 1;
        float r = parse_float(&p);
        while (*p && (*p == ',' || *p == ' ')) p++;
        float g = parse_float(&p);
        while (*p && (*p == ',' || *p == ' ')) p++;
        float b = parse_float(&p);
        float a = 1.0f;
        while (*p && (*p == ',' || *p == ' ')) p++;
        if (*p && (isdigit((unsigned char)*p) || *p == '.' || *p == '-')) a = parse_float(&p);
        if (a <= 1.0f) a *= 255.0f;   /* CSS 的 0..1 alpha -> Clay 的 0..255 */
        return (Clay_Color){r, g, b, a};
    }

    const char *s = text;
    while (*s && *s != '#') s++;
    if (*s == '#') {
        unsigned long v = strtoul(s + 1, NULL, 16);
        size_t n = strlen(s + 1);
        if (n >= 8) {
            return (Clay_Color){(float)((v >> 24) & 0xFF), (float)((v >> 16) & 0xFF),
                                (float)((v >> 8) & 0xFF), (float)(v & 0xFF)};
        }
        if (n >= 6) {
            return (Clay_Color){(float)((v >> 16) & 0xFF), (float)((v >> 8) & 0xFF),
                                (float)(v & 0xFF), 255.0f};
        }
    }
    return fallback;
}

void Jpx__ApplyChrome(JpxApp *app) {
    if (!app || !app->wm) return;
    /* VmChrome 的颜色是 const char*，只搬指针：theme 的字符串由调用方持有。 */
    VmChrome chrome = VmTheme_Default()->chrome;
    chrome.titleFocusColor = app->theme.accentColor;
    chrome.titleIdleColor = app->theme.borderColor;
    chrome.borderFocusColor = app->theme.accentColor;
    chrome.borderIdleColor = app->theme.borderColor;
    VmWm_SetChrome(app->wm, &chrome);
}

Clay_String Jpx__Str(const char *text) {
    Clay_String s = {0};
    s.chars = text ? text : "";
    s.length = (int32_t)strlen(s.chars);
    return s;
}

Clay_ElementId Jpx__Id(int win, const char *id) {
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "w%d/%s", win, id ? id : "");
    if (n < 0) n = 0;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    Clay_String s = {.chars = buf, .length = (int32_t)n};
    return Clay__HashString(s, 0);
}

Clay_ElementId Jpx__LocalId(JpxApp *app, const char *name) {
    uint32_t serial = app ? app->localSerial++ : 0;
    Clay_String s = Jpx__Str(name);
    return Clay__HashStringWithOffset(s, serial, Clay_GetOpenElementId());
}
