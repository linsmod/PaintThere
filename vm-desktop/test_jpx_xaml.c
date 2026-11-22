/* jp_xaml 断言：解析、元素树、实体、错误定位、绑定表、handler 表。
   不链接 Clay / WM / 网络，毫秒级。
   Build: vm-desktop/CMakeLists.txt 的 test_jpx_xaml 目标
   Run:   test_jpx_xaml.exe [--probe]
*/

#include <stdio.h>
#include <string.h>

#include "jpx_xaml.h"
#include "jpx_xaml_internal.h"

static int g_failures;
static int g_checks;
static int g_probe;

static void section(const char *name) {
    if (!g_probe) printf("-- %s\n", name);
}

static void expect(int cond, const char *what) {
    g_checks++;
    if (cond) {
        if (!g_probe) printf("PASS  %s\n", what);
    } else {
        g_failures++;
        printf("FAIL  %s\n", what);
    }
}

static void expect_int(int actual, int expected, const char *what) {
    g_checks++;
    if (actual == expected) {
        if (!g_probe) printf("PASS  %s (=%d)\n", what, actual);
    } else {
        g_failures++;
        printf("FAIL  %s (expected %d, got %d)\n", what, expected, actual);
    }
}

static void expect_str(const char *actual, const char *expected, const char *what) {
    g_checks++;
    if (actual && expected && strcmp(actual, expected) == 0) {
        if (!g_probe) printf("PASS  %s (=\"%s\")\n", what, actual);
    } else {
        g_failures++;
        printf("FAIL  %s (expected \"%s\", got \"%s\")\n", what,
               expected ? expected : "(null)", actual ? actual : "(null)");
    }
}

static int parse_ok(const char *xaml, JpxXamlDoc *doc) {
    char err[256];
    return JpxXamlDoc_Parse(doc, xaml, err, sizeof(err));
}

static void test_parse_tree(void) {
    section("parse / tree");
    JpxXamlDoc doc;
    int ok = parse_ok(
        "<!-- head -->\n"
        "<Window Title='Demo'>\n"
        "  <StackPanel Orientation='Horizontal'>\n"
        "    <Button Name='ok' Content='OK' Click='OnOk' />\n"
        "    <Button Content='Cancel' />\n"
        "  </StackPanel>\n"
        "  <TextBlock Text='tail' />\n"
        "</Window>", &doc);
    expect(ok, "document parses");
    if (!ok) return;

    expect_int(doc.count, 5, "five nodes parsed (Window, StackPanel, 2 Button, TextBlock)");
    expect_str(JpxXamlDoc_Node(&doc, 0)->tag, "Window", "root tag is Window");
    expect_str(JpxXamlNode_Attr(JpxXamlDoc_Node(&doc, 0), "Title"), "Demo",
               "single-quoted attribute value");
    expect_int(JpxXamlDoc_Node(&doc, 0)->firstChild, 1, "root's first child is StackPanel");
    expect_int(JpxXamlDoc_Node(&doc, 1)->parent, 0, "StackPanel's parent is root");
    expect_int(JpxXamlDoc_Node(&doc, 1)->firstChild, 2, "StackPanel's first child is Button");
    expect_int(JpxXamlDoc_Node(&doc, 2)->nextSibling, 3, "first Button's sibling is second Button");
    expect_int(JpxXamlDoc_Node(&doc, 3)->nextSibling, -1, "second Button has no sibling");
    expect_int(JpxXamlDoc_Node(&doc, 4)->parent, 0, "TextBlock is child of root");
    expect_str(JpxXamlNode_Attr(JpxXamlDoc_Node(&doc, 2), "Click"), "OnOk",
               "Click event name read back");
    expect(JpxXamlDoc_Node(&doc, 3)->attrCount >= 1, "self-closing Button parsed");
}

static void test_entities_and_text(void) {
    section("entities / text content");
    JpxXamlDoc doc;
    int ok = parse_ok(
        "<Window Title=\"A &amp; B &lt;C&gt; &quot;D&quot; &apos;E&apos;\">"
        "<TextBlock>hello &amp; bye</TextBlock></Window>", &doc);
    expect(ok, "entity document parses");
    if (!ok) return;
    expect_str(JpxXamlNode_Attr(JpxXamlDoc_Node(&doc, 0), "Title"), "A & B <C> \"D\" 'E'",
               "all five entities decoded in attribute");
    expect_str(JpxXamlNode_Text(JpxXamlDoc_Node(&doc, 1)), "hello & bye",
               "entity decoded in text content");
}

static void test_errors(void) {
    section("error reporting");
    JpxXamlDoc doc;
    char err[256];

    expect(!JpxXamlDoc_Parse(&doc, "<Window><Button></Window>", err, sizeof(err)),
           "mismatched closing tag fails");
    expect(strstr(err, "line 1") != NULL, "error carries line number");

    expect(!JpxXamlDoc_Parse(&doc, "<Window Title=oops />", err, sizeof(err)),
           "unquoted attribute fails");
    expect(strstr(err, "quote") != NULL, "error explains quoting requirement");

    expect(!JpxXamlDoc_Parse(&doc, "<Window Title='a &bogus;' />", err, sizeof(err)),
           "unknown entity fails");
    expect(strstr(err, "entity") != NULL, "error names invalid entity");

    expect(!JpxXamlDoc_Parse(&doc, "<Window Title='x'>", err, sizeof(err)),
           "unclosed tag at end of input fails");
    expect(strstr(err, "unclosed") != NULL, "error names unclosed tag");
}

static void test_capacity(void) {
    section("capacity guards");
    JpxXamlDoc doc;
    char src[16384];
    src[0] = '\0';
    for (int i = 0; i < JPX_XAML_MAX_NODES + 5; i++) {
        strcat(src, "<Button />");
    }
    char wrapped[17000];
    snprintf(wrapped, sizeof(wrapped), "<Window>%s</Window>", src);
    char err[256];
    expect(!JpxXamlDoc_Parse(&doc, wrapped, err, sizeof(err)), "node overflow is rejected");
    expect(strstr(err, "too many") != NULL, "node overflow reports capacity error");
}

static void test_binding_table(void) {
    section("binding table");
    JpxXamlView *view = JpxXamlView_Create(NULL, "<Window><TextBox Text='{Binding name}' /></Window>",
                                           NULL, NULL, 0);
    expect(view != NULL, "view created without app");
    if (!view) return;

    char state[64] = {0};
    expect(JpxXamlView_Bind(view, "name", JPX_XAML_BIND_TEXT, 8, 32), "text binding registered");
    expect(!JpxXamlView_Bind(view, "  ", JPX_XAML_BIND_TEXT, 0, 16), "blank binding name rejected");
    expect(JpxXamlView_Bind(view, "name", JPX_XAML_BIND_TEXT, 12, 16), "duplicate binding overwrites");
    expect(JpxXamlView_Bind(view, "ok", JPX_XAML_BIND_BOOL, 4, 1), "bool binding registered");
    expect(JpxXamlView_Bind(view, "sel", JPX_XAML_BIND_INT, 2, 4), "int binding registered");
    JpxXamlBinding *b = NULL;
    for (int i = 0; i < JPX_XAML_MAX_BINDINGS; i++) {
        if (view->bindings[i].used && strcmp(view->bindings[i].name, "name") == 0) {
            b = &view->bindings[i];
            break;
        }
    }
    expect(b != NULL, "binding lookup by name finds entry");
    if (b) {
        expect_int((int)b->offset, 12, "overwritten offset takes effect");
        expect_int((int)b->cap, 16, "overwritten cap takes effect");
        expect_int((int)b->kind, (int)JPX_XAML_BIND_TEXT, "binding kind preserved");
    }
    (void)state;
    JpxXamlView_Destroy(view);
}

static int g_handlerCalls;
static int g_handlerNode;

static void on_click(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)view;
    g_handlerCalls++;
    g_handlerNode = nodeIndex;
    (*(int *)userData)++;
}

static void test_handler_table(void) {
    section("handler table");
    g_handlerCalls = 0;
    g_handlerNode = -1;
    int users = 0;
    JpxXamlView *view = JpxXamlView_Create(NULL,
        "<Window><Button Name='go' Content='Go' Click='OnGo' /></Window>", NULL, NULL, 0);
    expect(view != NULL, "view with handler attribute created");
    if (!view) return;

    expect(JpxXamlView_On(view, "OnGo", on_click, &users), "handler registered by name");
    expect(!JpxXamlView_On(view, "Empty", NULL, NULL), "null handler rejected");

    JpxXamlNode *node = (JpxXamlNode *)JpxXamlView_Node(view, 1);
    expect_str(JpxXamlNode_Attr(node, "Click"), "OnGo", "Click attribute read via public API");
    expect_str(JpxXamlNode_Tag(node), "Button", "node tag read via public API");

    JpxXamlHandlerSlot *h = NULL;
    for (int i = 0; i < JPX_XAML_MAX_HANDLERS; i++) {
        if (view->handlers[i].used && strcmp(view->handlers[i].name, "OnGo") == 0) {
            h = &view->handlers[i];
            break;
        }
    }
    expect(h != NULL, "handler lookup by name finds entry");
    if (h) {
        h->fn(view, 1, h->userData);
        expect_int(g_handlerCalls, 1, "handler invoked exactly once");
        expect_int(g_handlerNode, 1, "handler receives correct node index");
        expect_int(users, 1, "handler userData updated");
    }
    JpxXamlView_Destroy(view);
}

static void test_binding_syntax(void) {
    section("binding expression parsing");
    JpxXamlView *view = JpxXamlView_Create(NULL,
        "<Window><TextBox Text='{Binding a.b}' /><TextBox Text='{Binding plain}' /></Window>",
        NULL, NULL, 0);
    expect(view != NULL, "binding-syntax document parses");
    if (!view) return;
    const JpxXamlNode *a = JpxXamlView_Node(view, 1);
    const JpxXamlNode *b = JpxXamlView_Node(view, 2);
    expect_str(JpxXamlNode_Attr(a, "Text"), "{Binding a.b}", "dotted binding preserved as text");
    expect_str(JpxXamlNode_Attr(b, "Text"), "{Binding plain}", "single binding preserved as text");
    JpxXamlView_Destroy(view);
}

int main(int argc, char **argv) {
    g_probe = argc > 1 && strcmp(argv[1], "--probe") == 0;

    test_parse_tree();
    test_entities_and_text();
    test_errors();
    test_capacity();
    test_binding_table();
    test_handler_table();
    test_binding_syntax();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
