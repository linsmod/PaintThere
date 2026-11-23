#include "jpx_xaml_theme.h"
#include "jpx_xaml_internal.h"
#include "vm_serialize_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 一步应用主题：需要 JPX 完整依赖，单独成 TU，便于纯解析测试独立链接。 */

/* 资源包 id 只要求进程内唯一且 URL 安全；每次应用递增，client 缓存自然不会串包。 */
static int s_themeSerial;

int JpxXamlTheme_Apply(JpxApp *app, const JpxXamlResourceTable *table) {
    if (!app || !table) return 0;
    VmTheme theme;
    JpxXamlTheme_DeriveTheme(table, &theme);
    Jpx_SetVmTheme(app, &theme);

    static VmDisplayList resList;
    VmDisplay_Init(&resList);
    JpxXamlTheme_AppendPatterns(table, &resList);
    VmTheme_AppendResources(&resList, &theme);

    /* RetroBar 的 TaskbarBackground 通常是多 stop 渐变；把它覆盖到 p_panel，
       让任务栏（bodyStyleId=s_panel→p_panel）显示真实主题渐变而非单色代表色。 */
    const JpxXamlResource *panel = JpxRes_Find(table, "TaskbarBackground");
    if (panel && (panel->kind == JPX_XAML_RES_LINEAR_GRADIENT ||
                  panel->kind == JPX_XAML_RES_RADIAL_GRADIENT) && panel->stopCount > 0) {
        for (int i = 0; i < resList.resourceCount; i++) {
            VmResourceCommand *c = &resList.resources[i];
            if (c->type != VM_CMD_DEFINE_PATTERN) continue;
            if (strcmp(c->args.definePattern.patternId, "p_panel") != 0) continue;
            VmDefinePatternArgs *p = &c->args.definePattern;
            p->type = panel->kind == JPX_XAML_RES_RADIAL_GRADIENT
                          ? VM_PATTERN_RADIAL : VM_PATTERN_GRADIENT;
            p->stopCount = panel->stopCount;
            for (int s = 0; s < panel->stopCount && s < VM_PATTERN_MAX_STOPS; s++)
                p->stops[s] = panel->stops[s];
            VmDisplay_CopyStr(p->direction, sizeof(p->direction), panel->direction);
            break;
        }
    }

    char *json = VmSerialize_CommandArrayJson(&resList);
    int ok = 0;
    if (json) {
        char resPackId[32];
        snprintf(resPackId, sizeof(resPackId), "theme-%d", ++s_themeSerial);
        ok = Jpx_ResPack_Register(app, resPackId, json);
        free(json);
    }
    return ok;
}
