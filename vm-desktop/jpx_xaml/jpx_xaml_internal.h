#ifndef JPX_XAML_INTERNAL_H
#define JPX_XAML_INTERNAL_H

/* jp_xaml 内部共享声明：应用代码不得 include。 */

#include <stddef.h>

#include "jpx.h"
#include "jpx_xaml.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RetroBar 真实主题（Windows Vista Aero 905 节点、XP Blue 898）远超 128。
   元素树只在解析期存在，1024 节点约 600KB，调用方必须 static/malloc，
   不要放到普通栈上。 */
#define JPX_XAML_MAX_NODES 1024
#define JPX_XAML_MAX_ATTRS 12
/* ResourceDictionary.MergedDictionaries 是 38 字符，32 会截断标签名。 */
#define JPX_XAML_TAG_CAP 48
#define JPX_XAML_ATTR_NAME_CAP 32
#define JPX_XAML_ATTR_VALUE_CAP 512
#define JPX_XAML_TEXT_CAP 8192
#define JPX_XAML_INPUT_CAP 256
#define JPX_XAML_MAX_ITEMS 48
#define JPX_XAML_MAX_BINDINGS 64
#define JPX_XAML_MAX_HANDLERS 64

typedef struct {
    char name[JPX_XAML_ATTR_NAME_CAP];
    char value[JPX_XAML_ATTR_VALUE_CAP];
} JpxXamlAttr;

struct JpxXamlNode {
    char tag[JPX_XAML_TAG_CAP];
    JpxXamlAttr attrs[JPX_XAML_MAX_ATTRS];
    int attrCount;
    char text[JPX_XAML_TEXT_CAP];
    int parent;
    int firstChild;
    int lastChild;
    int nextSibling;
    int line;
};

typedef struct {
    JpxXamlNode nodes[JPX_XAML_MAX_NODES];
    int count;
} JpxXamlDoc;

typedef struct {
    int used;
    char name[JPX_XAML_ATTR_NAME_CAP];
    JpxXamlBindKind kind;
    size_t offset;
    size_t cap;
} JpxXamlBinding;

typedef struct {
    int used;
    char name[JPX_XAML_ATTR_NAME_CAP];
    JpxXamlHandler fn;
    void *userData;
} JpxXamlHandlerSlot;

/* 每个 TextBox 节点的持久缓冲：Jpx_TextInput 用 buf 地址判断编辑状态是否重建，
   因此 buf 必须跨帧稳定，且长度变化要当作外部改写同步。 */
typedef struct {
    int used;
    char buf[JPX_XAML_INPUT_CAP];   /* 传给 Jpx_TextInput 的稳定地址 */
    char synced[JPX_XAML_INPUT_CAP];/* 上次与 model 同步的值，用于双向差异检测 */
    size_t len;
} JpxXamlInputSlot;

struct JpxXamlView {
    JpxApp *app;
    void *model;
    JpxXamlDoc doc;
    JpxXamlBinding bindings[JPX_XAML_MAX_BINDINGS];
    JpxXamlHandlerSlot handlers[JPX_XAML_MAX_HANDLERS];
    JpxXamlInputSlot inputs[JPX_XAML_MAX_NODES];
};

/* 解析 XAML 子集到 doc。失败返回 0 并写 err（含行号）。 */
int  JpxXamlDoc_Parse(JpxXamlDoc *doc, const char *src, char *err, size_t errCap);
void JpxXamlDoc_Init(JpxXamlDoc *doc);
const JpxXamlNode *JpxXamlDoc_Node(const JpxXamlDoc *doc, int index);
const char *JpxXamlNode_Attr(const JpxXamlNode *node, const char *name);
const char *JpxXamlNode_Text(const JpxXamlNode *node);

/* jpx_xaml_state.c 的内部接口：回放器与测试共用，不依赖 JPX 实现。 */
JpxXamlBinding *JpxXaml__FindBinding(JpxXamlView *view, const char *name);
JpxXamlHandlerSlot *JpxXaml__FindHandler(JpxXamlView *view, const char *name);
const char *JpxXaml__BindingField(const char *value);
void JpxXaml__Fire(JpxXamlView *view, int nodeIndex, const char *eventAttr);
const char *JpxXaml__NodeId(const JpxXamlNode *node, int index,
                            char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
