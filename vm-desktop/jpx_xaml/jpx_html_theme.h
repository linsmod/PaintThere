#ifndef JPX_HTML_THEME_H
#define JPX_HTML_THEME_H

/* CSS 子集主题 → 资源表。与 jp_xaml 工具链只共享协议（JpxXamlResourceTable
   结构 + 预编译三件套格式），解析实现完全独立，不引用 jp_xaml 解析器。 */

#include <stddef.h>

#include "jpx_xaml_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 解析 CSS 子集（canonical，由 tools/export_theme_css.py 导出）：
   只认 :root 块内的自定义属性声明，其余内容（选择器/注释/var()）报错。
   值语法按前缀分派：linear-gradient(dir(...),...) / radial-gradient /
   path(bbox..., 'data') / url(file) / 'string' / rgba(...)|#hex / 数字。
   资源按声明顺序入表（与预编译 xalm.bin record 顺序一致）。
   失败返回 0 并写 err。 */
int JpxHtmlTheme_LoadCss(JpxXamlResourceTable *table, const char *cssPath,
                         char *err, size_t errCap);

#ifdef __cplusplus
}
#endif

#endif
