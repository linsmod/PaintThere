#ifndef JPX_XAML_H
#define JPX_XAML_H

/* jp_xaml：声明式 UI 层。应用只 include jpx.h 与 jpx_xaml.h，
   不接触 Clay / VmDisplay / tiles / JSON / frameId。 */

#include <stddef.h>

#include "jpx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JpxXamlView JpxXamlView;
typedef struct JpxXamlNode JpxXamlNode;

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

#ifdef __cplusplus
}
#endif

#endif
