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

    /* 桌面背景程序并入每个主题包：client 只拉取最近一次注册的包，新连接
       不经过 default 包；不随包下发的话桌面底色/编译时间水印永远到不了
       新 client。s_desktop 样式由上面的 VmTheme_AppendResources 按新主题
       覆盖，程序引用不变。 */
    const VmDisplayList *desktop = Jpx_DesktopProgram(app);
    if (desktop && desktop->count > 0) {
        VmResourceCommand *p =
            VmDisplay_AppendResource(&resList, VM_CMD_DEFINE_DRAW_PROGRAM, "def_prog_desktop");
        if (p) {
            VmDisplay_CopyStr(p->args.defineProgram.programId,
                              sizeof(p->args.defineProgram.programId), "prog_desktop");
            p->args.defineProgram.commands =
                (struct VmDisplayList *)desktop;   /* 只读借用，序列化后即弃 */
            VmDisplay_CopyStr(p->args.defineProgram.layerId,
                              sizeof(p->args.defineProgram.layerId), "bg");
        }
        p = VmDisplay_AppendResource(&resList, VM_CMD_START_PROGRAM, "start_prog_desktop");
        if (p) {
            VmDisplay_CopyStr(p->args.programControl.programId,
                              sizeof(p->args.programControl.programId), "prog_desktop");
            VmDisplay_CopyStr(p->args.programControl.mode,
                              sizeof(p->args.programControl.mode), "once");
        }
    }

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
