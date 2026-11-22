#include "jpx_xaml_theme.h"
#include "vm_serialize_json.h"

#include <stdlib.h>

/* 一步应用主题：需要 JPX 完整依赖，单独成 TU，便于纯解析测试独立链接。 */

int JpxXamlTheme_Apply(JpxApp *app, const JpxXamlResourceTable *table) {
    if (!app || !table) return 0;
    VmTheme theme;
    JpxXamlTheme_DeriveTheme(table, &theme);
    Jpx_SetVmTheme(app, &theme);

    static VmDisplayList resList;
    VmDisplay_Init(&resList);
    JpxXamlTheme_AppendPatterns(table, &resList);
    VmTheme_AppendResources(&resList, &theme);
    char *json = VmSerialize_CommandArrayJson(&resList);
    if (json) {
        Jpx_BroadcastResources(app, json);
        free(json);
    }
    return 1;
}
