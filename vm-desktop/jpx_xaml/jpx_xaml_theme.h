#ifndef JPX_XAML_THEME_H
#define JPX_XAML_THEME_H

/* jp_xaml ResourceDictionary 主题：解析资源字典、按 key 查询、派生主题。 */

#include <stddef.h>

#include "jpx.h"
#include "vm_display.h"
#include "vm_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPX_XAML_MAX_RESOURCES 256

typedef enum {
    JPX_XAML_RES_NONE = 0,
    JPX_XAML_RES_COLOR,
    JPX_XAML_RES_LINEAR_GRADIENT,
    JPX_XAML_RES_RADIAL_GRADIENT,
    JPX_XAML_RES_NUMBER,
    JPX_XAML_RES_IMAGE      /* BitmapImage / ImageBrush：uri 是图片路径 */
} JpxXamlResKind;

typedef struct {
    int used;
    char key[VM_DISPLAY_MAX_ID];
    JpxXamlResKind kind;
    /* color / gradient stop 的颜色 */
    char color[VM_DISPLAY_MAX_COLOR];
    VmGradientStop stops[VM_PATTERN_MAX_STOPS];
    int stopCount;
    char direction[VM_DISPLAY_MAX_STR];  /* 线性端点或径向 cx,cy,r */
    double number;
    /* 图片资源：xaml 里写的相对路径，解析可执行目录后为绝对路径。 */
    char uri[256];
} JpxXamlResource;

typedef struct {
    JpxXamlResource items[JPX_XAML_MAX_RESOURCES];
    int count;
} JpxXamlResourceTable;

/* 从 XAML 文本解析资源字典。失败返回 0 并写 err（含行号）。 */
int  JpxXamlTheme_Parse(JpxXamlResourceTable *table, const char *xaml,
                        char *err, size_t errCap);
void JpxXamlTheme_Init(JpxXamlResourceTable *table);

/* 按 key 查询；返回 NULL 表示不存在。 */
const JpxXamlResource *JpxRes_Find(const JpxXamlResourceTable *table, const char *key);
const char *JpxRes_Color(const JpxXamlResourceTable *table, const char *key,
                         const char *fallback);
double JpxRes_Number(const JpxXamlResourceTable *table, const char *key,
                     double fallback);

/* 把资源表转换成 definePattern 资源命令，追加到 list->resources。
   返回追加数量。 */
int JpxXamlTheme_AppendPatterns(const JpxXamlResourceTable *table, VmDisplayList *list);

/* 图片资源的相对路径以主题文件目录为基准：解析后写回 uri（绝对路径）。 */
void JpxXamlTheme_ResolveImages(JpxXamlResourceTable *table, const char *themeFilePath);

/* VmResPackImageAt 的实现：userData 是资源表，按 index 枚举图片资源并读文件。
   mime 按扩展名给（png/jpg/bmp/gif），未知一律 image/png。
   图片保持原始字节，打包容器直接把 PNG/JPEG 拼进去，不做解码重编码。 */
int JpxXamlTheme_ImageAt(void *userData, const char *resPackId, int index,
                         const char **imgId, const char **mime,
                         const char **data, size_t *size);

/* 从资源表派生 VmTheme（颜色 token）。缺失的 key 用 base 的值。 */
void JpxXamlTheme_DeriveTheme(const JpxXamlResourceTable *table, VmTheme *out);

/* 解析 {StaticResource key} / {DynamicResource key}；命中返回颜色字符串，
   否则返回 fallback。v1 两者等价。 */
const char *JpxXamlTheme_Resolve(const JpxXamlResourceTable *table,
                                 const char *expr, const char *fallback);

/* 一步应用主题：派生 VmTheme → Jpx_SetVmTheme（WM/shell/控件）→
   序列化资源包 → Jpx_ResPack_Register（下发 loadResPack(id)，client 自取）。
   资源表必须由调用方长期持有（颜色指针借给 VmTheme/JpxTheme）。 */
int JpxXamlTheme_Apply(JpxApp *app, const JpxXamlResourceTable *table);

#ifdef __cplusplus
}
#endif

#endif
