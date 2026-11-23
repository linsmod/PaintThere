#ifndef JPX_SHELL_SETTINGS_H
#define JPX_SHELL_SETTINGS_H

/* 任务栏设置组件（对标 RetroBar 的 Properties 对话框）：
   jp_xaml 表单 + 主题预设扫描 + shell 选项应用 + 配置持久化。
   复用方式：Init 一次（启动时读配置并应用），宿主在任务栏「属性」回调里
   调 Open 弹出设置窗口。 */

#include "jpx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JpxShellSettingsCtx JpxShellSettingsCtx;

typedef struct {
    int showClock;      /* 任务栏显示时钟，默认 1 */
    int showTrayIcon;   /* 任务栏显示托盘图标，默认 1 */
    char theme[64];     /* 预编译主题目录名（<webRoot>/themes/ 下），空 = 不换主题 */
} JpxShellSettings;

/* 启动时调用一次：读配置文件（configPath 为 NULL 时用 <webRoot>/shell.ini），
   应用主题与任务栏选项。配置不存在或字段缺失时用默认值，不算失败。
   返回 1 = 配置文件存在且已读取；0 = 文件不存在/解析失败（仍应用默认值）。 */
int JpxShellSettings_Init(JpxApp *app, const char *webRoot,
                          const char *configPath, JpxShellSettings *cfg);

/* 打开设置窗口（同一时刻一个实例；已打开返回现有 winId）。
   返回 0 = 失败（窗口数已达上限）。cfg 必须是长期有效的存储（组件借用它）。 */
int JpxShellSettings_Open(JpxApp *app, const char *webRoot, JpxShellSettings *cfg);

/* 设置窗口是否开着。 */
int JpxShellSettings_IsOpen(void);

#ifdef __cplusplus
}
#endif

#endif
