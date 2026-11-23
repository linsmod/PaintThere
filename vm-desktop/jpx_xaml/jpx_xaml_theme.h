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

/* 厚度/模板几何采集后 XP Silver 实测 510 条，512 余量不足；
   NUMBER 只驻 guest 内存（不进下发通道），代价是表容量本身。 */
#define JPX_XAML_MAX_RESOURCES 768

/* 资源种类即预编译二进制的元素标记 id（xalm.bin 的 tagId），数值冻结：
   1=SOLID_COLOR 2=LINEAR_GRADIENT 3=RADIAL_GRADIENT 4=NUMBER
   5=STRING 6=PATH；IMAGE 只存在于 resid2path.bin（7 仅运行时使用）。 */
typedef enum {
    JPX_XAML_RES_NONE = 0,
    JPX_XAML_RES_COLOR = 1,           /* SolidColorBrush（含 <Key>.Brush 派生色） */
    JPX_XAML_RES_LINEAR_GRADIENT = 2, /* LinearGradientBrush */
    JPX_XAML_RES_RADIAL_GRADIENT = 3, /* RadialGradientBrush */
    JPX_XAML_RES_NUMBER = 4,          /* system:Double + Style 尺寸 Setter */
    JPX_XAML_RES_STRING = 5,          /* FontFamily / system:String */
    JPX_XAML_RES_PATH = 6,            /* DrawingBrush 的几何（归一化 0..1） */
    JPX_XAML_RES_IMAGE = 7            /* BitmapImage / ImageBrush：resid2path */
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
    /* STRING / PATH 的值：字符串值或归一化 path data（前端 drawPath 上限 192）。 */
    char value[192];
    /* PATH：bbox 是原始坐标包围盒（宽高比用）。 */
    float bbox[4];  /* x0,y0,x1,y1；非 PATH 全 0 */
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

/* 加载真实主题文件，并递归合并 ResourceDictionary.MergedDictionaries。
   WPF 优先级语义：本文件条目 > 后合并的字典 > 先合并的字典（逆序合并实现）。
   Source 相对当前文件目录解析；深度上限 8（防环）。失败返回 0 并写 err。
   返回 1 时 table 持有合并后的全部资源，调用方长期持有。 */
int  JpxXamlTheme_LoadFile(JpxXamlResourceTable *table, const char *path,
                           char *err, size_t errCap);

/* 预编译写出：把（已合并、图片已 ResolveImages 绝对化的）资源表写入 dir：
   xalm.bin + strings.bin + resid2path.bin，并把图片按 <资源id>.<扩展名> 复制进 dir。
   dir 不存在时创建。失败返回 0 并写 err。 */
int  JpxXamlTheme_WritePrecompiled(const JpxXamlResourceTable *table, const char *dir,
                                   char *err, size_t errCap);

/* 预编译读入：dir 里的三件套 → 资源表。图片资源 uri 是 dir 内相对路径，
   调用方随后用 JpxXamlTheme_ResolveImages(table, dir) 绝对化。
   与 LoadFile 等价（同一 DeriveTheme/Apply 链路），但不再解析 XAML 文本。 */
int  JpxXamlTheme_LoadPrecompiled(JpxXamlResourceTable *table, const char *dir,
                                  char *err, size_t errCap);

/* 按 key 查询；返回 NULL 表示不存在。 */
const JpxXamlResource *JpxRes_Find(const JpxXamlResourceTable *table, const char *key);
const char *JpxRes_Color(const JpxXamlResourceTable *table, const char *key,
                         const char *fallback);
double JpxRes_Number(const JpxXamlResourceTable *table, const char *key,
                     double fallback);

/* 把资源表转换成 definePattern 资源命令，追加到 list->resources。
   返回追加数量。 */
int JpxXamlTheme_AppendPatterns(const JpxXamlResourceTable *table, VmDisplayList *list);

/* 图片资源的相对路径解析基准：baseDir 是**目录**（不是文件）。
   外置主题传主题文件所在目录；内置主题传资源根目录（如 assets）。 */
void JpxXamlTheme_ResolveImages(JpxXamlResourceTable *table, const char *baseDir);

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
