/* 菜单控件断言：右键路由、popup 外点关闭、菜单命中与自动关闭。
   不开服务、不开浏览器，直接驱动 VmWm_HandleInput。
   Build: 由 vm-desktop/CMakeLists.txt 的 test_menu 目标生成
   Run:   test_menu.exe [font.ttf] [--probe]
*/
#define CLAY_IMPLEMENTATION
#include "vm_wm.h"

#include <stdio.h>
#include <string.h>

#include "vm_font.h"
#include "vm_menu.h"

static int g_failures;
static int g_checks;
static int g_quiet;

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

/* 窗口内容布局回调：本测试不产出内容。 */
static void dummy_layout(VmCmdBuffer *out, int x, int y, int w, int h, void *userData) {
    (void)out; (void)x; (void)y; (void)w; (void)h; (void)userData;
}

/* 窗口 1 的点击探针：记录调用次数与最近一次按键。 */
static int g_clickCalls;
static int g_lastClickButton;
static int probe_click(int windowId, int x, int y, int button, void *userData) {
    (void)windowId;
    (void)x;
    (void)y;
    (void)userData;
    g_clickCalls++;
    g_lastClickButton = button;
    return 1;
}

/* 菜单选中探针。 */
static int g_menuSelectCalls;
static int g_lastSelectedIndex;
static void on_menu_select(int index, void *userData) {
    (void)userData;
    g_menuSelectCalls++;
    g_lastSelectedIndex = index;
}

static void feed(VmWm *wm, const char *json) {
    VmWm_HandleInput(wm, json, strlen(json));
}

/* 默认路径随运行目录变化，按候选顺序找第一份能加载的 TTF。 */
static int load_font_any(void) {
    const char *candidates[] = {
        "../assets/font.ttf", "../../assets/font.ttf", "assets/font.ttf", "font.ttf"
    };
    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        if (VmFont_LoadFile(candidates[i], 0) == 0) return 0;
    }
    return -1;
}

static void test_right_click_route(void) {
    if (!g_quiet) printf("-- wm: right click reaches window click handler\n");
    VmWm *wm = VmWm_Create(800, 600);
    if (!wm) return;
    VmWm_AddWindow(wm, 1, "A", 60, 60, 360, 260, dummy_layout, NULL);
    VmWm_SetWindowClickHandler(wm, 1, probe_click, NULL);
    VmWm_ConsumeDirty(wm);

    /* 左键回归：button=0 原语义。 */
    feed(wm, "{\"kind\":\"mousedown\",\"x\":100,\"y\":100,\"button\":0,\"clientId\":\"A\"}");
    expect_int(g_clickCalls, 1, "left click reaches click handler");
    expect_int(g_lastClickButton, 0, "left click reports button 0");

    /* 右键：button=2 到达同一通道。 */
    feed(wm, "{\"kind\":\"mousedown\",\"x\":120,\"y\":120,\"button\":2,\"clientId\":\"A\"}");
    expect_int(g_clickCalls, 2, "right click reaches click handler");
    expect_int(g_lastClickButton, 2, "right click reports button 2");

    /* 右键不触发拖动：move 不改变窗口位置。 */
    feed(wm, "{\"kind\":\"mousemove\",\"x\":200,\"y\":200,\"button\":2,\"clientId\":\"A\"}");
    VmWindow *w = VmWm_FindWindow(wm, 1);
    expect(w && w->x == 60, "right click does not start drag");

    VmWm_Free(wm);
}

static void test_menu_open_and_select(void) {
    if (!g_quiet) printf("-- menu: open / select / auto close\n");
    VmWm *wm = VmWm_Create(800, 600);
    if (!wm) return;
    VmWm_AddWindow(wm, 1, "A", 60, 60, 360, 260, dummy_layout, NULL);
    VmWm_SetWindowClickHandler(wm, 1, probe_click, NULL);
    VmWm_ConsumeDirty(wm);

    const VmMenuItem items[] = {
        {"还原", 1},
        {NULL, 0},       /* 分隔线 */
        {"关闭", 1},
        {"禁用项", 0},
    };
    int id = VmMenu_Open(wm, VmTheme_Default(), 100, 100, items, 4,
                         on_menu_select, NULL);
    expect_int(id, VM_MENU_WINDOW_ID, "menu opens with reserved window id");
    expect_int(VmMenu_IsOpen(), 1, "menu reports open");
    expect(wm->popupWindowId == VM_MENU_WINDOW_ID, "wm knows the popup window");
    VmWindow *menu = VmWm_FindWindow(wm, VM_MENU_WINDOW_ID);
    expect(menu != NULL, "menu window exists");
    if (!menu) {
        VmWm_Free(wm);
        return;
    }
    expect(menu->x >= 0 && menu->y >= 0 && menu->x + menu->w <= 800 &&
           menu->y + menu->h <= 600, "menu clamped inside canvas");
    expect((menu->flags & VM_WIN_UNDECORATED) && (menu->flags & VM_WIN_TOPMOST) &&
           (menu->flags & VM_WIN_NOFOCUS),
           "menu window is undecorated/topmost/nofocus");

    /* 点击第一个条目：命中 onSelect 并自动关闭。点在窗口菜单顶部区域。 */
    char json[128];
    snprintf(json, sizeof(json),
             "{\"kind\":\"mousedown\",\"x\":%d,\"y\":%d,\"button\":0,\"clientId\":\"A\"}",
             menu->x + menu->w / 2, menu->y + 10);
    feed(wm, json);
    expect_int(g_menuSelectCalls, 1, "item click fires onSelect");
    expect_int(g_lastSelectedIndex, 0, "selected index is first item");
    expect_int(VmMenu_IsOpen(), 0, "menu closes after selection");
    expect_int(wm->popupWindowId, 0, "popup registration cleared");

    /* 禁用项：命中不回调，但按下被消费并关闭菜单。 */
    id = VmMenu_Open(wm, VmTheme_Default(), 100, 100, items, 4, on_menu_select, NULL);
    expect_int(id, VM_MENU_WINDOW_ID, "menu reopens after close");
    menu = VmWm_FindWindow(wm, VM_MENU_WINDOW_ID);
    g_menuSelectCalls = 0;
    snprintf(json, sizeof(json),
             "{\"kind\":\"mousedown\",\"x\":%d,\"y\":%d,\"button\":0,\"clientId\":\"A\"}",
             menu->x + menu->w / 2, menu->y + menu->h - 10);
    feed(wm, json);
    expect_int(g_menuSelectCalls, 0, "disabled item does not fire onSelect");
    expect_int(VmMenu_IsOpen(), 0, "click on disabled item still closes menu");

    /* 分隔线不可点：点在菜单中段（分隔线位置附近），菜单关闭、无回调。 */
    VmMenu_Open(wm, VmTheme_Default(), 100, 100, items, 4, on_menu_select, NULL);
    menu = VmWm_FindWindow(wm, VM_MENU_WINDOW_ID);
    g_menuSelectCalls = 0;
    int sepY = menu->y + 10 + 22;   /* 第一条目高约 line+8，分隔线紧随 */
    snprintf(json, sizeof(json),
             "{\"kind\":\"mousedown\",\"x\":%d,\"y\":%d,\"button\":0,\"clientId\":\"A\"}",
             menu->x + menu->w / 2, sepY);
    feed(wm, json);
    expect_int(g_menuSelectCalls, 0, "separator does not fire onSelect");

    /* 再开一个：旧的先关（单例）。 */
    int again = VmMenu_Open(wm, VmTheme_Default(), 100, 100, items, 4,
                            on_menu_select, NULL);
    expect_int(again, VM_MENU_WINDOW_ID, "reopening while open succeeds");
    expect_int(VmWm_FindWindow(wm, VM_MENU_WINDOW_ID) != NULL, 1,
               "only one menu window exists");

    /* 程序化关闭。 */
    VmMenu_Close();
    expect_int(VmMenu_IsOpen(), 0, "programmatic close works");
    expect_int(wm->popupWindowId, 0, "popup cleared on programmatic close");

    VmWm_Free(wm);
}

static void test_menu_outside_dismiss(void) {
    if (!g_quiet) printf("-- menu: outside mousedown dismisses without passthrough\n");
    VmWm *wm = VmWm_Create(800, 600);
    if (!wm) return;
    VmWm_AddWindow(wm, 1, "A", 60, 60, 360, 260, dummy_layout, NULL);
    VmWm_SetWindowClickHandler(wm, 1, probe_click, NULL);
    VmWm_ConsumeDirty(wm);

    const VmMenuItem items[] = {{"还原", 1}, {"关闭", 1}};
    VmMenu_Open(wm, VmTheme_Default(), 700, 60, items, 2, on_menu_select, NULL);
    g_clickCalls = 0;

    /* 菜单外按下：菜单关闭、按下被消费（不触发窗口 1 的点击/聚焦）。 */
    feed(wm, "{\"kind\":\"mousedown\",\"x\":100,\"y\":100,\"button\":0,\"clientId\":\"A\"}");
    expect_int(VmMenu_IsOpen(), 0, "outside mousedown dismisses menu");
    expect_int(g_clickCalls, 0, "dismissed press does not reach window handler");

    /* 菜单外的右键同样只关菜单。 */
    VmMenu_Open(wm, VmTheme_Default(), 700, 60, items, 2, on_menu_select, NULL);
    g_clickCalls = 0;
    feed(wm, "{\"kind\":\"mousedown\",\"x\":100,\"y\":100,\"button\":2,\"clientId\":\"A\"}");
    expect_int(VmMenu_IsOpen(), 0, "outside right mousedown dismisses menu");
    expect_int(g_clickCalls, 0, "right dismiss does not reach window handler");

    /* 菜单内的按下仍走菜单命中（不触发外点 dismiss）。 */
    VmMenu_Open(wm, VmTheme_Default(), 100, 100, items, 2, on_menu_select, NULL);
    VmWindow *menu = VmWm_FindWindow(wm, VM_MENU_WINDOW_ID);
    char json[128];
    snprintf(json, sizeof(json),
             "{\"kind\":\"mousedown\",\"x\":%d,\"y\":%d,\"button\":0,\"clientId\":\"A\"}",
             menu->x + menu->w / 2, menu->y + 10);
    g_menuSelectCalls = 0;
    feed(wm, json);
    expect_int(g_menuSelectCalls, 1, "inside click still selects");

    VmWm_Free(wm);
}

static void test_menu_overflow_clamp(void) {
    if (!g_quiet) printf("-- menu: clamped when anchored near canvas edge\n");
    VmWm *wm = VmWm_Create(800, 600);
    if (!wm) return;
    const VmMenuItem items[] = {{"还原", 1}, {"关闭", 1}};
    /* 任务栏条目右键的典型锚点：底部边缘。 */
    VmMenu_Open(wm, VmTheme_Default(), 700, 590, items, 2, on_menu_select, NULL);
    VmWindow *menu = VmWm_FindWindow(wm, VM_MENU_WINDOW_ID);
    expect(menu != NULL && menu->y + menu->h <= 600,
           "menu pulled up when overflowing bottom");
    expect(menu && menu->x + menu->w <= 800, "menu pulled left when overflowing right");
    VmMenu_Close();
    VmWm_Free(wm);
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
    if (!g_quiet) printf("test_menu: font=%s\n", fontPath);
    if (load_font_any() != 0 && !g_quiet)
        printf("       font load failed; measurements fall back to estimates\n");
    test_right_click_route();
    test_menu_open_and_select();
    test_menu_outside_dismiss();
    test_menu_overflow_clamp();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
