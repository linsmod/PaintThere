/* 帧断言测试：不开服务、不开浏览器，直接断言度量、装饰、裁剪、脏标记与输入路由。
   Build: 由 vm-desktop/CMakeLists.txt 的 test_frame 目标生成
   Run:   test_frame.exe <font.ttf>            (默认 ../assets/font.ttf)
*/
#define CLAY_IMPLEMENTATION
#include "vm_wm.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "vm_clipboard.h"
#include "vm_font.h"
#include "vm_ipc.h"
#include "vm_shell.h"

static int g_failures;
static int g_checks;
static int g_quiet;   /* --probe：只输出失败与汇总，便于集中运行 */

static void expect(int cond, const char *what) {
    g_checks++;
    if (cond) {
        if (!g_quiet) printf("PASS  %s\n", what);
    } else {
        g_failures++;
        printf("FAIL  %s\n", what);
    }
}

static void expect_int(int actual, int expected, const char *what) {
    g_checks++;
    if (actual == expected) {
        if (!g_quiet) printf("PASS  %s (=%d)\n", what, actual);
    } else {
        g_failures++;
        printf("FAIL  %s (expected %d, got %d)\n", what, expected, actual);
    }
}

/* 窗口内容布局回调：本测试不建 Clay 上下文，BuildFrame 只发裁剪对，不调用它。 */
static Clay_RenderCommandArray dummy_layout(void *userData) {
    (void)userData;
    Clay_RenderCommandArray empty;
    memset(&empty, 0, sizeof(empty));
    return empty;
}

typedef struct {
    int keyCalls;
    char lastKey[32];
    int lastIsDown;
    int wheelCalls;
    int lastWheelLocalX, lastWheelLocalY, lastWheelDelta;
} InputProbe;

static InputProbe g_probe;

static int on_key(int windowId, const char *key, const char *code, int isDown, void *userData) {
    (void)windowId;
    (void)code;
    InputProbe *p = (InputProbe *)userData;
    p->keyCalls++;
    p->lastIsDown = isDown;
    snprintf(p->lastKey, sizeof(p->lastKey), "%s", key ? key : "");
    return 1;
}

static int on_wheel(int windowId, int x, int y, int deltaY, void *userData) {
    (void)windowId;
    InputProbe *p = (InputProbe *)userData;
    p->wheelCalls++;
    p->lastWheelLocalX = x;
    p->lastWheelLocalY = y;
    p->lastWheelDelta = deltaY;
    return 1;
}

/* 默认路径随运行目录变化，按候选顺序找第一份能加载的 TTF。 */
static int load_font_any(const char *preferred) {
    const char *candidates[] = {
        preferred,
        "../../assets/font.ttf",
        "../assets/font.ttf",
        "assets/font.ttf",
        "font.ttf"
    };
    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        if (!candidates[i]) continue;
        if (VmFont_LoadFile(candidates[i], 0) == 0) {
            if (!g_quiet) printf("       loaded font: %s\n", candidates[i]);
            return 0;
        }
    }
    return -1;
}

static void test_font(const char *fontPath) {
    if (!g_quiet) printf("-- font: %s\n", fontPath);
    expect_int(load_font_any(fontPath), 0, "load font slot 0");

    VmFontMetrics m = VmFont_Measure(0, 14, "Window A", -1);
    expect(m.width > 0, "measure width > 0");
    expect(m.lineStep > 0, "line step > 0");
    expect(m.baseline > 0 && m.baseline < m.height + m.lineStep, "baseline inside glyph box");
    expect_int(m.lines, 1, "single line");
    if (!g_quiet) printf("       \"Window A\" @14 -> w=%d h=%d baseline=%d lineStep=%d\n",
           m.width, m.height, m.baseline, m.lineStep);

    VmFontMetrics empty = VmFont_Measure(0, 14, "", 0);
    expect_int(empty.width, 0, "empty text width = 0");
    expect_int(empty.height, empty.lineStep, "empty text height = one line");

    VmFontMetrics two = VmFont_Measure(0, 14, "A\nB", -1);
    expect_int(two.lines, 2, "two lines");
    expect(two.height > m.height, "two lines taller than one");

    const char *cn = "跨端图绘指令流";
    VmFontMetrics cm = VmFont_Measure(0, 16, cn, (int)strlen(cn));
    expect(cm.width > 0, "CJK text width > 0");
    if (!g_quiet) printf("       \"%s\" @16 -> w=%d\n", cn, cm.width);

    int starts[16];
    const char *para = "the quick brown fox jumps over the lazy dog";
    int lines = VmFont_Wrap(0, 14, para, (int)strlen(para), 80, starts, 16);
    expect(lines > 1, "wrap produces multiple lines");
    int monotonic = 1;
    for (int i = 1; i < lines && i < 16; i++) {
        if (starts[i] <= starts[i - 1]) monotonic = 0;
    }
    expect(monotonic, "wrap line starts increase");
    expect_int(starts[0], 0, "wrap first line starts at 0");
}

static void test_wm_chrome_and_clip(void) {
    if (!g_quiet) printf("-- wm: chrome / clip / dirty\n");
    VmWm *wm = VmWm_Create(800, 600);
    expect(wm != NULL, "create wm");
    if (!wm) return;

    expect_int(VmWm_IsDirty(wm), 1, "new wm is dirty");
    expect_int(VmWm_ConsumeDirty(wm), 1, "consume dirty");
    expect_int(VmWm_IsDirty(wm), 0, "dirty cleared");

    VmWindow *win = VmWm_AddWindow(wm, 1, "Window A", 60, 60, 360, 260,
                                   dummy_layout, NULL);
    expect(win != NULL, "add window");

    static VmDisplayList frame;
    VmDisplay_Init(&frame);
    VmWm_BuildFrame(wm, &frame, 1, NULL);

    VmDrawCommand *body = VmDisplay_Find(&frame, "w1_body");
    expect(body != NULL, "body command exists");
    if (body) {
        /* 默认主题：borderWidth=1, titleHeight=30（现代深色）。 */
        expect_int((int)body->args.drawRect.rect.x, 61, "body x = win.x + border");
        expect_int((int)body->args.drawRect.rect.y, 90, "body y = win.y + title");
        expect_int((int)body->args.drawRect.rect.w, 358, "body w = win.w - 2*border");
        expect_int((int)body->args.drawRect.rect.h, 229, "body h = win.h - title - border");
        expect(strcmp(body->args.drawRect.paint, "s_window") == 0, "body paint = theme window style");
    }

    VmDrawCommand *shadow = VmDisplay_Find(&frame, "w1_shadow");
    expect(shadow != NULL, "window shadow command exists");
    if (shadow) {
        expect(shadow->args.drawRect.cornerRadius > 0, "shadow is rounded");
    }

    VmDrawCommand *title = VmDisplay_Find(&frame, "w1_title");
    expect(title != NULL, "title command exists");
    if (title) {
        expect(strcmp(title->args.drawRect.paint, "rgba(34,38,54,1)") == 0,
               "unfocused title uses theme idle color");
        expect(title->args.drawRect.cornerRadius > 0, "title is rounded");
    }

    VmDrawCommand *ttext = VmDisplay_Find(&frame, "w1_ttext");
    expect(ttext != NULL, "title text command exists");
    if (ttext) {
        expect_int(ttext->args.drawText.fontId, 0, "title font id from theme");
        expect_int(ttext->args.drawText.fontSize, 14, "title font size from theme");
        expect(ttext->args.drawText.rect.y >= 62, "title text below border");
    }

    VmDrawCommand *clip = VmDisplay_Find(&frame, "w1_clip");
    VmDrawCommand *clipEnd = VmDisplay_Find(&frame, "w1_clip_end");
    expect(clip != NULL && clip->type == VM_CMD_CLIP_PUSH, "content clip push exists");
    expect(clipEnd != NULL && clipEnd->type == VM_CMD_CLIP_POP, "content clip pop exists");
    if (clip && clipEnd) {
        expect_int((int)clip->args.clip.x, 61, "clip x = body x");
        expect_int((int)clip->args.clip.y, 90, "clip y = body y");
        expect_int((int)clip->args.clip.w, 358, "clip w = body w");
        expect_int((int)clip->args.clip.h, 229, "clip h = body h");
        expect(clip < clipEnd, "clip push precedes clip pop");
    }

    VmWm_SetContentClip(wm, 0);
    VmWm_BuildFrame(wm, &frame, 2, NULL);
    expect(VmDisplay_Find(&frame, "w1_clip") == NULL, "clip disabled -> no clip push");

    VmWm_SetContentClip(wm, 1);
    VmChrome chrome = VmTheme_Default()->chrome;
    chrome.titleHeight = 40;
    chrome.borderWidth = 4;
    VmWm_SetChrome(wm, &chrome);
    VmWm_BuildFrame(wm, &frame, 3, NULL);
    body = VmDisplay_Find(&frame, "w1_body");
    if (body) {
        expect_int((int)body->args.drawRect.rect.x, 64, "chrome border applies to body x");
        expect_int((int)body->args.drawRect.rect.y, 100, "chrome title height applies to body y");
    }

    /* 图层复用：没有新脏标记时窗口内容不再重建，builtFrame 停在第 3 帧。 */
    VmWm_BuildFrame(wm, &frame, 5, NULL);
    expect_int((int)win->layer.builtFrame, 3, "clean layer is reused, not rebuilt");
    expect_int(wm->layerOverflow, 0, "no layer overflow at default quota");

    /* 单窗口失效：新建第二个窗口只作废它自己，第一个窗口继续复用。 */
    VmWindow *win2 = VmWm_AddWindow(wm, 2, "Window B", 460, 60, 300, 200,
                                    dummy_layout, NULL);
    expect(win2 != NULL, "add second window");
    VmWm_BuildFrame(wm, &frame, 6, NULL);
    expect_int((int)win->layer.builtFrame, 3, "adding a window does not rebuild others");
    expect_int((int)win2->layer.builtFrame, 6, "new window built once");

    VmWm_InvalidateWindow(wm, 1);
    VmWm_BuildFrame(wm, &frame, 7, NULL);
    expect_int((int)win->layer.builtFrame, 7, "invalidated window rebuilt");
    expect_int((int)win2->layer.builtFrame, 6, "untouched window still reused");

    VmWm_Free(wm);
}

static void test_wm_input(void) {
    if (!g_quiet) printf("-- wm: input routing\n");
    VmWm *wm = VmWm_Create(800, 600);
    if (!wm) return;
    VmWm_AddWindow(wm, 1, "A", 60, 60, 360, 260, dummy_layout, NULL);
    VmWm_SetKeyHandler(wm, 1, on_key, &g_probe);
    VmWm_SetWheelHandler(wm, 1, on_wheel, &g_probe);
    VmWm_ConsumeDirty(wm);

    const char *down = "{\"kind\":\"mousedown\",\"x\":100,\"y\":70,"
                       "\"button\":0,\"clientId\":\"A\",\"color\":\"#F44336\"}";
    int changed = VmWm_HandleInput(wm, down, strlen(down));
    expect_int(changed, 1, "mousedown on title focuses window");
    expect_int(wm->focusId, 1, "focus id = 1");
    expect_int(VmWm_IsDirty(wm), 1, "input marks wm dirty");
    VmWm_ConsumeDirty(wm);

    const char *tab = "{\"kind\":\"keydown\",\"key\":\"Tab\",\"code\":\"Tab\","
                      "\"clientId\":\"A\"}";
    VmWm_HandleInput(wm, tab, strlen(tab));
    expect_int(g_probe.keyCalls, 1, "named key reaches window handler");
    expect(strcmp(g_probe.lastKey, "Tab") == 0, "key text is Tab");
    expect_int(g_probe.lastIsDown, 1, "keydown reported as isDown");

    /* 修饰键不在 keyHandler 签名里，经 VmWm_KeyModifiers 查询。 */
    int ctrl = 0, shift = 0, alt = 0;
    VmWm_KeyModifiers(wm, &ctrl, &shift, &alt);
    expect_int(shift, 0, "no shift for plain Tab");

    const char *ctrlA = "{\"kind\":\"keydown\",\"key\":\"a\",\"code\":\"KeyA\","
                        "\"ctrl\":1,\"clientId\":\"A\"}";
    VmWm_HandleInput(wm, ctrlA, strlen(ctrlA));
    VmWm_KeyModifiers(wm, &ctrl, &shift, &alt);
    expect_int(ctrl, 1, "ctrl modifier reaches VmWm_KeyModifiers");
    expect_int(shift, 0, "shift stays 0 for ctrl-only key");

    const char *shiftTab = "{\"kind\":\"keydown\",\"key\":\"Tab\",\"code\":\"Tab\","
                           "\"shift\":1,\"clientId\":\"A\"}";
    VmWm_HandleInput(wm, shiftTab, strlen(shiftTab));
    VmWm_KeyModifiers(wm, &ctrl, &shift, &alt);
    expect_int(shift, 1, "shift modifier reaches VmWm_KeyModifiers");
    expect_int(ctrl, 0, "ctrl cleared when not pressed");

    const char *wheel = "{\"kind\":\"wheel\",\"x\":100,\"y\":100,\"deltaY\":120,"
                        "\"clientId\":\"A\"}";
    VmWm_HandleInput(wm, wheel, strlen(wheel));
    expect_int(g_probe.wheelCalls, 1, "wheel reaches window handler");
    expect_int(g_probe.lastWheelLocalX, 39, "wheel local x = canvas - body offset");
    /* body 原点 = (x+border, y+title)，与 BuildFrame 追加 Clay 内容的偏移一致。 */
    expect_int(g_probe.lastWheelLocalY, 10, "wheel local y = canvas - body offset");
    expect_int(g_probe.lastWheelDelta, 120, "wheel deltaY passed through");

    int x0 = 0;
    VmWindow *w = VmWm_FindWindow(wm, 1);
    if (w) x0 = w->x;
    const char *move = "{\"kind\":\"mousemove\",\"x\":300,\"y\":250,\"clientId\":\"A\"}";
    VmWm_HandleInput(wm, move, strlen(move));
    w = VmWm_FindWindow(wm, 1);
    expect(w && w->x == x0 + 200, "drag moves window with pointer delta");

    VmWm_Free(wm);
}

static int g_closeCalls;

static void on_close(int windowId, void *userData) {
    (void)windowId;
    (void)userData;
    g_closeCalls++;
}

static void feed(VmWm *wm, const char *json) {
    VmWm_HandleInput(wm, json, strlen(json));
}

static void test_wm_maximize(void) {
    if (!g_quiet) printf("-- wm: maximize / work area / double click\n");
    VmWm *wm = VmWm_Create(800, 600);
    if (!wm) return;
    VmWm_SetWorkArea(wm, 0, 0, 800, 564);
    VmWm_AddWindow(wm, 1, "A", 60, 60, 360, 260, dummy_layout, NULL);
    VmWm_Focus(wm, 1);
    VmWindow *win = VmWm_FindWindow(wm, 1);

    /* □ 按钮在 × 左侧 20px 处 */
    feed(wm, "{\"kind\":\"mousedown\",\"x\":387,\"y\":72,\"button\":0,\"clientId\":\"A\"}");
    expect_int(VmWm_IsMaximized(wm, 1), 1, "maximize button maximizes");
    win = VmWm_FindWindow(wm, 1);
    expect_int(win->x, 0, "maximized takes work area x");
    expect_int(win->w, 800, "maximized takes work area w");
    expect_int(win->h, 564, "maximized takes work area h");

    /* 最大化后不可拖动：移动请求被忽略 */
    VmWm_Move(wm, 1, 200, 200);
    expect_int(win->x, 0, "maximized window ignores move");

    /* 双击标题栏：最大化后标题栏在 y=0..30 之间 */
    feed(wm, "{\"kind\":\"mousedown\",\"x\":200,\"y\":15,\"button\":0,\"clientId\":\"A\"}");
    feed(wm, "{\"kind\":\"mouseup\",\"x\":200,\"y\":15,\"button\":0,\"clientId\":\"A\"}");
    feed(wm, "{\"kind\":\"mousedown\",\"x\":200,\"y\":15,\"button\":0,\"clientId\":\"A\"}");
    expect_int(VmWm_IsMaximized(wm, 1), 0, "double click on title restores size");
    win = VmWm_FindWindow(wm, 1);
    expect_int(win->x, 60, "restored x");
    expect_int(win->h, 260, "restored h");

    /* 拖动钳制到工作区：向上越界被拉回 */
    VmWm_Move(wm, 1, 60, -40);
    expect_int(win->y, 0, "move clamps to work area top");

    VmWm_Free(wm);
}

static void test_wm_flags(void) {
    if (!g_quiet) printf("-- wm: flags / close / minimize\n");
    VmWm *wm = VmWm_Create(800, 600);
    if (!wm) return;
    VmWm_AddWindow(wm, 1, "A", 60, 60, 360, 260, dummy_layout, NULL);
    VmWm_AddWindow(wm, 2, "B", 300, 200, 360, 260, dummy_layout, NULL);
    VmWm_SetCloseHandler(wm, 1, on_close, NULL);
    VmWm_Focus(wm, 1);

    /* 三个按钮从右往左：× / □ / —，默认 chrome 下各 16px、间隔 4px。 */
    feed(wm, "{\"kind\":\"mousedown\",\"x\":405,\"y\":72,\"button\":0,\"clientId\":\"A\"}");
    expect_int(g_closeCalls, 1, "close button invokes close handler");
    expect_int(wm->count, 2, "close handler does not remove the window");

    feed(wm, "{\"kind\":\"mousedown\",\"x\":367,\"y\":72,\"button\":0,\"clientId\":\"A\"}");
    expect_int(VmWm_IsMinimized(wm, 1), 1, "minimize button minimizes");
    expect_int(VmWm_FocusId(wm), -1, "minimized window loses focus");

    VmWm_Restore(wm, 1);
    expect_int(VmWm_IsMinimized(wm, 1), 0, "restore clears minimized");
    expect_int(VmWm_FocusId(wm), 1, "restore refocuses");

    /* NOFOCUS：不允许被聚焦，也不夺走焦点。 */
    VmWm_SetWindowFlags(wm, 2, VM_WIN_NOFOCUS);
    VmWm_Focus(wm, 2);
    expect_int(VmWm_FocusId(wm), 1, "NOFOCUS window cannot take focus");

    /* TOPMOST：整层最后合成。 */
    VmWm_SetWindowFlags(wm, 2, VM_WIN_TOPMOST);
    static VmDisplayList frame;
    VmDisplay_Init(&frame);
    VmWm_BuildFrame(wm, &frame, 1, NULL);
    VmDrawCommand *b1 = VmDisplay_Find(&frame, "w1_body");
    VmDrawCommand *b2 = VmDisplay_Find(&frame, "w2_body");
    expect(b1 && b2 && b2 > b1, "topmost window composites after normal window");

    /* UNDECORATED：没有标题与边框，body 即整个窗口。 */
    VmWm_SetWindowFlags(wm, 2, VM_WIN_UNDECORATED | VM_WIN_TOPMOST);
    VmWm_BuildFrame(wm, &frame, 2, NULL);
    expect(VmDisplay_Find(&frame, "w2_title") == NULL, "undecorated window has no title");
    expect(VmDisplay_Find(&frame, "w2_border") == NULL, "undecorated window has no border");
    b2 = VmDisplay_Find(&frame, "w2_body");
    if (b2) {
        expect_int((int)b2->args.drawRect.rect.x, 300, "undecorated body x = window x");
        expect_int((int)b2->args.drawRect.rect.y, 200, "undecorated body y = window y");
        expect_int((int)b2->args.drawRect.rect.h, 260, "undecorated body h = window h");
    }

    int ids[VM_WM_MAX_WINDOWS];
    expect_int(VmWm_Enumerate(wm, ids, VM_WM_MAX_WINDOWS), 2, "enumerate returns window count");
    /* 聚焦即提升：win1 被 Focus 过，z 高于 win2，故升序为 [2,1]。 */
    expect(ids[0] == 2 && ids[1] == 1, "enumerate ordered by z ascending");
    expect_int(VmWm_EnumerateCreated(wm, ids, VM_WM_MAX_WINDOWS), 2,
               "created enumerate returns window count");
    expect(ids[0] == 1 && ids[1] == 2, "created enumerate keeps creation order after raise");

    VmWm_Free(wm);
}

static void test_shell(void) {
    if (!g_quiet) printf("-- shell: taskbar\n");
    VmWm *wm = VmWm_Create(800, 600);
    if (!wm) return;
    VmWm_AddWindow(wm, 1, "A", 60, 60, 360, 260, dummy_layout, NULL);
    VmWm_AddWindow(wm, 2, "B", 300, 200, 360, 260, dummy_layout, NULL);
    VmWm_Focus(wm, 1);

    VmShell *shell = VmShell_Create(wm, VmTheme_Default(), VM_SHELL_DEFAULT_HEIGHT);
    expect(shell != NULL, "taskbar created");
    if (!shell) { VmWm_Free(wm); return; }

    expect_int(wm->count, 3, "taskbar is an extra window");
    VmWindow *tb = VmWm_FindWindow(wm, VM_SHELL_WINDOW_ID);
    expect(tb != NULL, "taskbar window exists");
    if (tb) {
        expect_int((int)(tb->flags & VM_WIN_TOPMOST) != 0, 1, "taskbar is topmost");
        expect_int((int)(tb->flags & VM_WIN_NOFOCUS) != 0, 1, "taskbar does not take focus");
        expect_int((int)(tb->flags & VM_WIN_PINNED) != 0, 1, "taskbar is pinned");
        expect_int((int)(tb->flags & VM_WIN_UNDECORATED) != 0, 1, "taskbar is undecorated");
        expect_int(tb->y, 600 - VM_SHELL_DEFAULT_HEIGHT, "taskbar sits at bottom");
        VmRect body;
        VmWm_WindowBody(wm, tb, &body);
        expect_int((int)body.w, 800, "taskbar body spans full width");
    }

    static VmDisplayList frame;
    VmDisplay_Init(&frame);
    VmWm_BuildFrame(wm, &frame, 1, NULL);
    char tbBody[32];
    snprintf(tbBody, sizeof(tbBody), "w%d_body", VM_SHELL_WINDOW_ID);
    VmDrawCommand *taskBody = VmDisplay_Find(&frame, tbBody);
    VmDrawCommand *aBody = VmDisplay_Find(&frame, "w1_body");
    expect(taskBody != NULL, "taskbar body is composited");
    expect(taskBody && aBody && taskBody > aBody, "taskbar composites above normal windows");
    if (taskBody) {
        expect(strcmp(taskBody->args.drawRect.paint, "s_panel") == 0, "taskbar uses panel style");
    }

    /* 点任务栏条目：最小化的窗口被恢复并获得焦点。
       条目顺序 = 创建序；几何全部来自主题 token（pad/start/item/gap），
       中心 = pad + startWidth + gap + i*(itemWidth+gap) + itemWidth/2。 */
    VmWm_Minimize(wm, 1);
    expect_int(VmWm_FocusId(wm), -1, "no focus after minimize");
    int ord[VM_WM_MAX_WINDOWS];
    int cnt = VmWm_EnumerateCreated(wm, ord, VM_WM_MAX_WINDOWS);
    int idx = -1;
    for (int i = 0; i < cnt; i++) if (ord[i] == 1) idx = i;
    expect(idx >= 0, "minimized window still listed in taskbar");
    if (idx >= 0) {
        char payload[160];
        const VmTheme *dt = VmTheme_Default();
        int cx = dt->panelPad + dt->panelStartWidth + dt->panelItemGap +
                 idx * (dt->panelItemWidth + dt->panelItemGap) + dt->panelItemWidth / 2;
        snprintf(payload, sizeof(payload),
                 "{\"kind\":\"mousedown\",\"x\":%d,\"y\":580,\"button\":0,\"clientId\":\"A\"}",
                 cx);
        feed(wm, payload);
        VmWm_BuildFrame(wm, &frame, 2, NULL);
        expect_int(VmWm_IsMinimized(wm, 1), 0, "clicking taskbar item restores minimized window");
        expect_int(VmWm_FocusId(wm), 1, "clicking taskbar item focuses that window");
    }

    /* 任务栏文字命令按内容识别（Clay 元素 id 是数字哈希，不便于按字符串找）。 */
    int hasStart = 0, hasClock = 0;
    for (int i = 0; i < frame.count; i++) {
        VmDrawCommand *c = &frame.commands[i];
        if (c->type != VM_CMD_DRAW_TEXT) continue;
        if (strstr(c->args.drawText.text, "开始") != NULL) hasStart = 1;
        const char *txt = c->args.drawText.text;
        if (strlen(txt) == 5 && txt[2] == ':' && isdigit((unsigned char)txt[0]) &&
            isdigit((unsigned char)txt[1]) && isdigit((unsigned char)txt[3]) &&
            isdigit((unsigned char)txt[4])) hasClock = 1;
    }
    expect(hasStart, "taskbar renders start button label");
    expect(hasClock, "taskbar renders HH:MM clock");

    /* 无装饰窗口不生成任务栏条目：对比加入前后的任务栏文字命令数。 */
    int textBefore = 0;
    for (int i = 0; i < frame.count; i++)
        if (frame.commands[i].type == VM_CMD_DRAW_TEXT) textBefore++;
    VmWm_AddWindow(wm, 3, "desktop", 0, 0, 100, 100, dummy_layout, NULL);
    VmWm_SetWindowFlags(wm, 3, VM_WIN_UNDECORATED | VM_WIN_NOFOCUS);
    VmWm_BuildFrame(wm, &frame, 3, NULL);
    int textAfter = 0;
    for (int i = 0; i < frame.count; i++)
        if (frame.commands[i].type == VM_CMD_DRAW_TEXT) textAfter++;
    expect_int(textAfter, textBefore, "undecorated window adds no taskbar item");

    VmWm_RemoveWindow(wm, 3);
    VmShell_Destroy(shell);
    expect_int(wm->count, 2, "destroying shell removes taskbar window");
    VmWm_Free(wm);
}

static void test_clipboard(void) {
    if (!g_quiet) printf("-- clipboard\n");
    VmClipboard_Clear();
    expect_int(VmClipboard_HasText(), 0, "empty at start");
    VmClipboard_SetText("hello 世界");
    expect_int(VmClipboard_HasText(), 1, "has text after set");
    char buf[64];
    int n = VmClipboard_GetText(buf, (int)sizeof(buf));
    expect(strcmp(buf, "hello 世界") == 0, "round-trips utf8");
    expect_int(n, (int)strlen("hello 世界"), "returns byte length");
    char small[4];
    expect_int(VmClipboard_GetText(small, (int)sizeof(small)), 3, "truncates to cap-1");
    VmClipboard_Clear();
    expect_int(VmClipboard_GetText(buf, (int)sizeof(buf)), 0, "cleared returns 0");
}

int main(int argc, char **argv) {
    const char *fontPath = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--probe") == 0) {
            g_quiet = 1;
            continue;
        }
        if (!fontPath) fontPath = argv[i];
    }
    if (!fontPath) fontPath = "../assets/font.ttf";
    if (!g_quiet) printf("test_frame: font=%s\n", fontPath);
    /* 架构预算：资源类 args 已外置，命令结构再被撑大就会在这里失败。 */
    expect(sizeof(VmDrawCommand) <= 512, "draw command within size budget");
    /* 命令与 IPC 头都必须是定长 POD：跨进程直接 memcpy，指针会被打成垃圾。
       vm_ipc.h 里有 _Static_assert 兜底，这里再钉住帧头尺寸。 */
    expect(sizeof(VmIpcHeader) == 12, "ipc header stays 12 bytes");
    if (!g_quiet) printf("test_frame: sizeof(cmd)=%zu sizeof(list)=%zu\n",
                         sizeof(VmDrawCommand), sizeof(VmDisplayList));
    test_font(fontPath);
    test_wm_chrome_and_clip();
    test_wm_input();
    test_wm_maximize();
    test_wm_flags();
    test_shell();
    test_clipboard();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
