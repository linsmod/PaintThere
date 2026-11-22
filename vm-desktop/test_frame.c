/* 帧断言测试：不开服务、不开浏览器，直接断言度量、装饰、裁剪、脏标记与输入路由。
   Build: 由 vm-desktop/CMakeLists.txt 的 test_frame 目标生成
   Run:   test_frame.exe <font.ttf>            (默认 ../assets/font.ttf)
*/
#define CLAY_IMPLEMENTATION
#include "vm_wm.h"

#include <stdio.h>
#include <string.h>

#include "vm_font.h"

static int g_failures;
static int g_checks;

static void expect(int cond, const char *what) {
    g_checks++;
    if (cond) {
        printf("PASS  %s\n", what);
    } else {
        g_failures++;
        printf("FAIL  %s\n", what);
    }
}

static void expect_int(int actual, int expected, const char *what) {
    g_checks++;
    if (actual == expected) {
        printf("PASS  %s (=%d)\n", what, actual);
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
            printf("       loaded font: %s\n", candidates[i]);
            return 0;
        }
    }
    return -1;
}

static void test_font(const char *fontPath) {
    printf("-- font: %s\n", fontPath);
    expect_int(load_font_any(fontPath), 0, "load font slot 0");

    VmFontMetrics m = VmFont_Measure(0, 14, "Window A", -1);
    expect(m.width > 0, "measure width > 0");
    expect(m.lineStep > 0, "line step > 0");
    expect(m.baseline > 0 && m.baseline < m.height + m.lineStep, "baseline inside glyph box");
    expect_int(m.lines, 1, "single line");
    printf("       \"Window A\" @14 -> w=%d h=%d baseline=%d lineStep=%d\n",
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
    printf("       \"%s\" @16 -> w=%d\n", cn, cm.width);

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
    printf("-- wm: chrome / clip / dirty\n");
    VmWm *wm = VmWm_Create(800, 600);
    expect(wm != NULL, "create wm");
    if (!wm) return;

    expect_int(VmWm_IsDirty(wm), 1, "new wm is dirty");
    expect_int(VmWm_ConsumeDirty(wm), 1, "consume dirty");
    expect_int(VmWm_IsDirty(wm), 0, "dirty cleared");

    VmWindow *win = VmWm_AddWindow(wm, 1, "Window A", 60, 60, 360, 260,
                                   dummy_layout, NULL);
    expect(win != NULL, "add window");

    VmDisplayList frame;
    VmDisplay_Init(&frame);
    VmWm_BuildFrame(wm, &frame, 1, NULL);

    VmDrawCommand *body = VmDisplay_Find(&frame, "w1_body");
    expect(body != NULL, "body command exists");
    if (body) {
        expect_int((int)body->args.drawRect.rect.x, 62, "body x = win.x + border");
        expect_int((int)body->args.drawRect.rect.y, 88, "body y = win.y + title");
        expect_int((int)body->args.drawRect.rect.w, 356, "body w = win.w - 2*border");
        expect_int((int)body->args.drawRect.rect.h, 230, "body h = win.h - title - border");
        expect(strcmp(body->args.drawRect.paint, "s_window") == 0, "body paint = theme window style");
    }

    VmDrawCommand *title = VmDisplay_Find(&frame, "w1_title");
    expect(title != NULL, "title command exists");
    if (title) {
        expect(strcmp(title->args.drawRect.paint, "rgba(120,126,134,1)") == 0,
               "unfocused title uses theme idle color");
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
        expect_int((int)clip->args.clip.x, 62, "clip x = body x");
        expect_int((int)clip->args.clip.y, 88, "clip y = body y");
        expect_int((int)clip->args.clip.w, 356, "clip w = body w");
        expect_int((int)clip->args.clip.h, 230, "clip h = body h");
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

    VmWm_Free(wm);
}

static void test_wm_input(void) {
    printf("-- wm: input routing\n");
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

    const char *wheel = "{\"kind\":\"wheel\",\"x\":100,\"y\":100,\"deltaY\":120,"
                        "\"clientId\":\"A\"}";
    VmWm_HandleInput(wm, wheel, strlen(wheel));
    expect_int(g_probe.wheelCalls, 1, "wheel reaches window handler");
    expect_int(g_probe.lastWheelLocalX, 38, "wheel local x = canvas - body offset");
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

int main(int argc, char **argv) {
    const char *fontPath = argc > 1 ? argv[1] : "../assets/font.ttf";
    printf("test_frame: font=%s\n", fontPath);
    test_font(fontPath);
    test_wm_chrome_and_clip();
    test_wm_input();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
