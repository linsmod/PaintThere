#ifndef JPX_XAML_H
#define JPX_XAML_H

/* DEPRECATED（2026-11-25）：old infra。parser/state 将被新 app 复用，
   执行层（Jpx_* 回放）随旧 app 在 M7 删除；见 docs/task-vm-app.md A6。

   jp_xaml：声明式 UI 层。应用只 include jpx.h 与 jpx_xaml.h，
   不接触 Clay / VmDisplay / tiles / JSON / frameId。 */

#include <stddef.h>

#include "jpx.h"
#include "vm_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JpxXamlView JpxXamlView;
typedef struct JpxXamlNode JpxXamlNode;
struct JpxXamlResourceTable;

typedef enum {
    JPX_XAML_BIND_BOOL = 0,   /* 1 字节；CheckBox.IsChecked */
    JPX_XAML_BIND_INT,        /* 4 字节；ListBox.SelectedIndex */
    JPX_XAML_BIND_TEXT        /* 定长 char 数组；TextBox.Text，需 cap */
} JpxXamlBindKind;

typedef void (*JpxXamlHandler)(JpxXamlView *view, int nodeIndex, void *userData);

/* 解析 XAML 并建视图。model 为应用状态结构体基址；err 可 NULL。
   失败返回 NULL，err 收到含行号的错误。 */
JpxXamlView *JpxXamlView_Create(JpxApp *app, const char *xaml, void *model,
                                char *err, size_t errCap);
void JpxXamlView_Destroy(JpxXamlView *view);

void *JpxXamlView_Model(JpxXamlView *view);
JpxApp *JpxXamlView_App(JpxXamlView *view);

/* 把 state 结构体里的字段登记为绑定目标。重复名字覆盖。
   offset 相对 model；cap 只在 JPX_XAML_BIND_TEXT 有意义。返回 0 表示槽满。 */
int JpxXamlView_Bind(JpxXamlView *view, const char *name, JpxXamlBindKind kind,
                     size_t offset, size_t cap);

/* 登记事件 handler。name 与 XAML 里 Click/TextChanged/SelectionChanged 的值对应。 */
int JpxXamlView_On(JpxXamlView *view, const char *name,
                   JpxXamlHandler fn, void *userData);

/* 节点查询：handler 通过 nodeIndex 读 XAML 属性，不接触内部节点结构。 */
int  JpxXamlView_NodeCount(JpxXamlView *view);
const JpxXamlNode *JpxXamlView_Node(JpxXamlView *view, int index);
const char *JpxXamlNode_Tag(const JpxXamlNode *node);
const char *JpxXamlNode_Attr(const JpxXamlNode *node, const char *name);
const char *JpxXamlNode_Text(const JpxXamlNode *node);

/* 每帧回放。应用把 JpxXaml_Render 包成 JpxWindowDesc.layout 回调：
   static void layout(int win, void *user) { JpxXaml_Render((JpxXamlView *)user, win); } */
void JpxXaml_Render(JpxXamlView *view, int winId);

/* ---- 绘制后端（M7.1） ----
   同一份 XAML 既能回放为 Jpx_*（Clay），也能直接产出绘制命令。
   后端由调用方提供生命周期，ctx 原样回传；能力缺失的回调可以为 NULL。 */
typedef struct {
    void (*rect)(void *ctx, const char *id, VmRect r, const char *paint, float radius);
    void (*border)(void *ctx, const char *id, VmRect r, const char *paint, const int w[4]);
    void (*text)(void *ctx, const char *id, VmRect r, const char *s,
                 int fontId, int size, const char *color);
    void (*image)(void *ctx, const char *id, VmRect r, const char *imgRef);
    void (*path)(void *ctx, const char *id, VmRect r, const char *svgPath, const char *paint);
    int  (*text_width)(void *ctx, int fontId, int size, const char *s);
    int  (*line_height)(void *ctx, int fontId, int size);
    void (*frame_begin)(void *ctx, const char *tag, const JpxXamlNode *node, VmRect r);
    void (*frame_end)(void *ctx, const char *tag);
} JpxXamlBackend;

/* Direct 后端回放：v1 只有绝对定位（X/Y/Width/Height）与
   StackPanel 的纵向/横向堆叠。Repeat / Layers / States 在 P2 接入。
   只遍历元素树并调用 backend，不依赖 JPX / Clay。 */
void JpxXaml_RenderDirect(JpxXamlView *view, const JpxXamlBackend *backend,
                          void *ctx, int x, int y, int w, int h);

/* ---- Direct 后端的输入（M7.1 交互层） ----
   坐标是渲染坐标系（与 JpxXaml_RenderDirect 的 x/y 同源）。
   down=1 按下、0 抬起；返回 1 表示状态变化（宿主应重渲染并重推内容）。
   只处理 Button / CheckBox / TextBox / ListBox；其余节点不响应。 */
int JpxXaml_DirectPointer(JpxXamlView *view, int x, int y, int down);

/* 键盘输入：只作用于当前聚焦的 TextBox。返回 1 表示状态变化。 */
int JpxXaml_DirectKey(JpxXamlView *view, const char *key, int ctrl, int shift);

/* caret 相位（宿主每 500ms 翻转一次）。 */
void JpxXaml_DirectSetCaret(JpxXamlView *view, int on);

/* Direct 渲染的资源表：`{StaticResource X}` 里的颜色从这里查。
   表由调用方长期持有；传 NULL 表示不解析资源（字面色值仍可用）。 */
void JpxXamlView_SetResources(JpxXamlView *view, const struct JpxXamlResourceTable *table);

#ifdef __cplusplus
}
#endif

#endif
