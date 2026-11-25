#include "ui_xaml.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpx_xaml.h"
#include "jpx_xaml_backend_cmdbuf.h"
#include "jpx_xaml_theme.h"

typedef struct {
    char name[64];
    char email[128];
    unsigned char notify;
    unsigned char remember;
    int sel;
} XamlFormModel;

static JpxXamlView *g_view;
static XamlFormModel g_model;
static char *g_ownedXaml;
static JpxXamlResourceTable g_themeTable;   /* 768 项，只放 BSS，按需加载 */

static const char kFormXaml[] =
    "<Window>"
    "  <StackPanel Orientation='Vertical' Gap='6'>"
    "    <TextBlock Text='jp_xaml form (Direct backend)' Height='20' />"
    "    <TextBox Name='name' Text='{Binding name}' Height='28' />"
    "    <TextBox Name='email' Text='{Binding email}' Height='28' />"
    "    <CheckBox Content='notify' Height='24' IsChecked='{Binding notify}' />"
    "    <CheckBox Content='remember' Height='24' IsChecked='{Binding remember}' />"
    "    <ListBox Items='Alpha;Beta;Gamma' Height='58' SelectedIndex='{Binding sel}' />"
    "    <Button Content='OK' Height='28' Width='90' Click='OnOk' />"
    "  </StackPanel>"
    "</Window>";

static void on_ok(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)view;
    (void)nodeIndex;
    (void)userData;
    fprintf(stderr,
            "[app] form ok name='%s' email='%s' notify=%d remember=%d sel=%d\n",
            g_model.name, g_model.email, g_model.notify, g_model.remember, g_model.sel);
    fflush(stderr);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

int XamlUi_Init(const char *xamlPath, char *err, size_t errCap) {
    const char *text = kFormXaml;
    if (xamlPath) {
        g_ownedXaml = read_file(xamlPath);
        if (!g_ownedXaml) {
            snprintf(err, errCap, "cannot read %s", xamlPath);
            return 0;
        }
        text = g_ownedXaml;
    }

    g_view = JpxXamlView_Create(NULL, text, &g_model, err, errCap);
    if (!g_view) return 0;

    JpxXamlView_Bind(g_view, "name", JPX_XAML_BIND_TEXT,
                     offsetof(XamlFormModel, name), sizeof(g_model.name));
    JpxXamlView_Bind(g_view, "email", JPX_XAML_BIND_TEXT,
                     offsetof(XamlFormModel, email), sizeof(g_model.email));
    JpxXamlView_Bind(g_view, "notify", JPX_XAML_BIND_BOOL,
                     offsetof(XamlFormModel, notify), 1);
    JpxXamlView_Bind(g_view, "remember", JPX_XAML_BIND_BOOL,
                     offsetof(XamlFormModel, remember), 1);
    JpxXamlView_Bind(g_view, "sel", JPX_XAML_BIND_INT,
                     offsetof(XamlFormModel, sel), sizeof(int));
    JpxXamlView_On(g_view, "OnOk", on_ok, NULL);
    return 1;
}

int XamlUi_LoadTheme(const char *themeDir, char *err, size_t errCap) {
    if (!themeDir || !*themeDir) return 0;
    JpxXamlTheme_Init(&g_themeTable);
    if (!JpxXamlTheme_LoadPrecompiled(&g_themeTable, themeDir, err, errCap)) return 0;
    JpxXamlTheme_ResolveImages(&g_themeTable, themeDir);
    if (g_view) JpxXamlView_SetResources(g_view, &g_themeTable);
    fprintf(stderr, "[app] theme loaded '%s' resources=%d\n", themeDir, g_themeTable.count);
    fflush(stderr);
    return 1;
}

void XamlUi_Paint(VmCmdBuffer *out, int w, int h) {
    if (!g_view || !out) return;
    JpxXaml_RenderDirect(g_view, JpxXamlBackend_CmdBuf(), out, 0, 0, w, h);
}

int XamlUi_Pointer(int x, int y, int down) {
    return g_view ? JpxXaml_DirectPointer(g_view, x, y, down) : 0;
}

int XamlUi_Key(const char *key, int ctrl, int shift) {
    return g_view ? JpxXaml_DirectKey(g_view, key, ctrl, shift) : 0;
}

void XamlUi_SetCaret(int on) {
    if (g_view) JpxXaml_DirectSetCaret(g_view, on);
}
