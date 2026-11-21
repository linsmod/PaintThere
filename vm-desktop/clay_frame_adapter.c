#include "clay_frame_adapter.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* clay.h is provided by the external Clay checkout; pass -I<clay-dir> at build time. */
#include "clay.h"

struct ClayFrameBuffer {
    char *data;
    size_t length;
    size_t capacity;
};

static int buffer_reserve(ClayFrameBuffer *b, size_t extra) {
    size_t needed = b->length + extra + 1;
    if (needed <= b->capacity) return 1;
    size_t cap = b->capacity ? b->capacity : 1024;
    while (cap < needed) {
        if (cap > (size_t)-1 / 2) return 0;
        cap *= 2;
    }
    char *p = (char *)realloc(b->data, cap);
    if (!p) return 0;
    b->data = p;
    b->capacity = cap;
    return 1;
}

static void buffer_puts(ClayFrameBuffer *b, const char *s) {
    size_t n = strlen(s);
    if (!buffer_reserve(b, n)) return;
    memcpy(b->data + b->length, s, n);
    b->length += n;
    b->data[b->length] = '\0';
}

static void buffer_putf(ClayFrameBuffer *b, const char *fmt, ...) {
    char tmp[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n <= 0) return;
    if ((size_t)n < sizeof(tmp)) {
        buffer_puts(b, tmp);
        return;
    }
    char *big = (char *)malloc((size_t)n + 1);
    if (!big) return;
    va_start(args, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, args);
    va_end(args);
    buffer_puts(b, big);
    free(big);
}

static void append_json_string(ClayFrameBuffer *b, const char *s, size_t len) {
    buffer_puts(b, "\"");
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': buffer_puts(b, "\\\""); break;
            case '\\': buffer_puts(b, "\\\\"); break;
            case '\b': buffer_puts(b, "\\b"); break;
            case '\f': buffer_puts(b, "\\f"); break;
            case '\n': buffer_puts(b, "\\n"); break;
            case '\r': buffer_puts(b, "\\r"); break;
            case '\t': buffer_puts(b, "\\t"); break;
            default:
                if (c < 0x20) buffer_putf(b, "\\u%04x", c);
                else buffer_putf(b, "%c", c);
        }
    }
    buffer_puts(b, "\"");
}

static void append_color(ClayFrameBuffer *b, Clay_Color color) {
    buffer_putf(b, "\"rgba(%g,%g,%g,%.3f)\"",
                color.r, color.g, color.b, color.a / 255.0f);
}

static void append_bounding_box(ClayFrameBuffer *b, Clay_BoundingBox box) {
    buffer_putf(b, "[%g,%g,%g,%g]", box.x, box.y, box.width, box.height);
}

/* args expect four separate numbers, while rect uses the array form. */
static void append_box_args(ClayFrameBuffer *b, Clay_BoundingBox box) {
    buffer_putf(b, "%g,%g,%g,%g", box.x, box.y, box.width, box.height);
}

static void append_command_prefix(ClayFrameBuffer *b, uint32_t id, const char *cmd) {
    buffer_putf(b, "{\"id\":\"%u\",\"cmd\":\"%s\",\"args\":[", id, cmd);
}

static void append_command_suffix(ClayFrameBuffer *b, Clay_BoundingBox box) {
    buffer_puts(b, "],\"rect\":");
    append_bounding_box(b, box);
    buffer_puts(b, "}");
}

static void append_rect_command(
    ClayFrameBuffer *b, const Clay_RenderCommand *rc,
    const char *cmd, Clay_Color color, Clay_CornerRadius radius) {
    append_command_prefix(b, rc->id, cmd);
    append_box_args(b, rc->boundingBox);
    buffer_puts(b, ",");
    append_color(b, color);
    float r = radius.topLeft;
    if (radius.topRight > r) r = radius.topRight;
    if (radius.bottomLeft > r) r = radius.bottomLeft;
    if (radius.bottomRight > r) r = radius.bottomRight;
    buffer_putf(b, ",%g", r);
    append_command_suffix(b, rc->boundingBox);
}

static void append_border_command(ClayFrameBuffer *b, const Clay_RenderCommand *rc) {
    Clay_BorderRenderData data = rc->renderData.border;
    append_command_prefix(b, rc->id, "drawBorder");
    append_box_args(b, rc->boundingBox);
    buffer_puts(b, ",");
    append_color(b, data.color);
    buffer_putf(b, ",[%u,%u,%u,%u]",
                data.width.left, data.width.right, data.width.top, data.width.bottom);
    float r = data.cornerRadius.topLeft;
    if (data.cornerRadius.topRight > r) r = data.cornerRadius.topRight;
    if (data.cornerRadius.bottomLeft > r) r = data.cornerRadius.bottomLeft;
    if (data.cornerRadius.bottomRight > r) r = data.cornerRadius.bottomRight;
    buffer_putf(b, ",%g", r);
    append_command_suffix(b, rc->boundingBox);
}

static void append_text_command(
    ClayFrameBuffer *b, const Clay_RenderCommand *rc,
    const char *(*fontResolver)(uint16_t)) {
    Clay_TextRenderData data = rc->renderData.text;
    append_command_prefix(b, rc->id, "drawText");
    append_box_args(b, rc->boundingBox);
    buffer_puts(b, ",");
    append_json_string(b, data.stringContents.chars, data.stringContents.length);
    buffer_putf(b, ",%u,%u,", data.fontId, data.fontSize);
    append_color(b, data.textColor);
    buffer_putf(b, ",%u,%u", data.letterSpacing, data.lineHeight);
    append_command_suffix(b, rc->boundingBox);
    if (fontResolver) {
        /* fontResolver is consulted by the host when building its font table. */
        (void)fontResolver(data.fontId);
    }
}

static void append_image_command(
    ClayFrameBuffer *b, const Clay_RenderCommand *rc,
    const char *(*imgResolver)(const void *)) {
    Clay_ImageRenderData data = rc->renderData.image;
    append_command_prefix(b, rc->id, "drawImg");
    const char *imgId = imgResolver ? imgResolver(data.imageData) : NULL;
    append_json_string(b, imgId ? imgId : "", imgId ? strlen(imgId) : 0);
    buffer_puts(b, ",");
    append_box_args(b, rc->boundingBox);
    buffer_puts(b, ",");
    append_color(b, data.backgroundColor);
    append_command_suffix(b, rc->boundingBox);
}

ClayFrameBuffer *ClayFrameBuffer_Create(void) {
    ClayFrameBuffer *b = (ClayFrameBuffer *)calloc(1, sizeof(ClayFrameBuffer));
    if (b && !buffer_reserve(b, 0)) {
        free(b);
        return NULL;
    }
    return b;
}

void ClayFrameBuffer_Free(ClayFrameBuffer *b) {
    if (!b) return;
    free(b->data);
    free(b);
}

const char *ClayFrameBuffer_Data(const ClayFrameBuffer *b) {
    return b ? b->data : "";
}

size_t ClayFrameBuffer_Length(const ClayFrameBuffer *b) {
    return b ? b->length : 0;
}

void ClayFrameBuffer_Clear(ClayFrameBuffer *b) {
    if (b) {
        b->length = 0;
        if (b->data) b->data[0] = '\0';
    }
}

void ClayFrame_Build(
    ClayFrameBuffer *out,
    uint32_t frameId,
    const void *renderCommands,
    int commandCount,
    const char *(*fontResolver)(uint16_t),
    const char *(*imgResolver)(const void *)) {
    if (!out || !renderCommands) return;
    const Clay_RenderCommand *commands = (const Clay_RenderCommand *)renderCommands;

    ClayFrameBuffer_Clear(out);
    buffer_putf(out, "{\"type\":\"frame\",\"frameId\":%u,\"commands\":[", frameId);

    int appended = 0;
    for (int i = 0; i < commandCount; i++) {
        const Clay_RenderCommand *rc = &commands[i];
        if (appended++) buffer_puts(out, ",");
        switch (rc->commandType) {
            case CLAY_RENDER_COMMAND_TYPE_NONE:
                appended--;
                break;
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
                append_rect_command(out, rc, "drawRect",
                                    rc->renderData.rectangle.backgroundColor,
                                    rc->renderData.rectangle.cornerRadius);
                break;
            case CLAY_RENDER_COMMAND_TYPE_BORDER:
                append_border_command(out, rc);
                break;
            case CLAY_RENDER_COMMAND_TYPE_TEXT:
                append_text_command(out, rc, fontResolver);
                break;
            case CLAY_RENDER_COMMAND_TYPE_IMAGE:
                append_image_command(out, rc, imgResolver);
                break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
                append_command_prefix(out, rc->id, "clipPush");
                append_box_args(out, rc->boundingBox);
                append_command_suffix(out, rc->boundingBox);
                break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
                append_command_prefix(out, rc->id, "clipPop");
                append_command_suffix(out, rc->boundingBox);
                break;
            case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_START:
                append_command_prefix(out, rc->id, "overlayPush");
                append_color(out, rc->renderData.overlayColor.color);
                append_command_suffix(out, rc->boundingBox);
                break;
            case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_END:
                append_command_prefix(out, rc->id, "overlayPop");
                append_command_suffix(out, rc->boundingBox);
                break;
            case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
                append_command_prefix(out, rc->id, "drawRect");
                append_bounding_box(out, rc->boundingBox);
                buffer_puts(out, ",\"rgba(255,0,255,1)\",0");
                append_command_suffix(out, rc->boundingBox);
                break;
        }
    }
    buffer_puts(out, "]}");
}
