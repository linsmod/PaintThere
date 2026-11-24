#ifndef VM_TEMPLATE_H
#define VM_TEMPLATE_H

/* 模板层绘制：把「主层 + 周围若干层」这种按钮皮肤画成命令序列。
   数据来自主题的 VmTaskButtonLayer（RetroBar 的 TaskButton 模板：
   外 4 边 + 内 3 边 + 对角高光），这里只负责按层序产出命令。

   任务栏是第一个使用者；将来的 XAML Direct 后端（docs/task-shell-xaml-framework.md）
   应复用同一函数，避免把层序再写一遍。 */

#include "vm_display.h"
#include "vm_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 通用矩形命令：id 由调用方给（要跨帧稳定，差分依赖它）。 */
void VmCmd_AppendRect(VmCmdBuffer *out, const char *id, int x, int y, int w, int h,
                      const char *paint, float radius);

/* 按层集画外 4 边、内 3 边与高光层。主层与文字由调用方先画。
   idPrefix + id 组成每条命令的 id（如 "tb_item" + 3 → "tb_item_top_3"），
   与拆分前的命名保持一致。hlPattern 为 NULL 或空时不画高光层。 */
void VmTemplate_PaintLayers(VmCmdBuffer *out, const VmTaskButtonLayer *st,
                            const char *idPrefix, int id,
                            int x, int y, int w, int h, float radius,
                            const char *hlPattern);

#ifdef __cplusplus
}
#endif

#endif
