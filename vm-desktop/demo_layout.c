#include "demo_layout.h"

#include <stdio.h>
#include <stdlib.h>

static const Clay_Color BG     = {30, 32, 38, 255};
static const Clay_Color WINDOW = {245, 247, 250, 255};
static const Clay_Color TITLE  = {38, 110, 190, 255};
static const Clay_Color TEXT   = {32, 36, 42, 255};
static const Clay_Color BUTTON = {52, 140, 220, 255};
static const Clay_Color BORDER = {180, 188, 198, 255};

static Clay_Dimensions MeasureText(
    Clay_StringSlice text, Clay_TextElementConfig *config, void *userData) {
    (void)userData;
    return (Clay_Dimensions){
        .width = (float)text.length * (float)config->fontSize * 0.6f,
        .height = (float)config->fontSize,
    };
}

static void HandleClayErrors(Clay_ErrorData data) {
    fwrite(data.errorText.chars, 1, data.errorText.length, stderr);
}

void DemoLayout_Init(void) {
    uint64_t total = Clay_MinMemorySize();
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(total, malloc(total));
    Clay_Initialize(arena, (Clay_Dimensions){800, 600},
                    (Clay_ErrorHandler){HandleClayErrors});
    Clay_SetMeasureTextFunction(MeasureText, 0);
}

Clay_RenderCommandArray DemoLayout_Build(void) {
    Clay_BeginLayout();
    CLAY(CLAY_ID("Desktop"), {
        .layout = {
            .sizing = {CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600)},
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .childGap = 16,
            .padding = CLAY_PADDING_ALL(24),
        },
        .backgroundColor = BG,
    }) {
        CLAY(CLAY_ID("Window"), {
            .layout = {
                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .childGap = 12,
                .padding = CLAY_PADDING_ALL(16),
            },
            .backgroundColor = WINDOW,
            .border = {.color = BORDER, .width = CLAY_BORDER_ALL(2)},
        }) {
            CLAY_TEXT(CLAY_STRING("VM Instruction Desktop"),
                      CLAY_TEXT_CONFIG({.fontId = 1, .fontSize = 24, .textColor = TITLE}));
            CLAY_TEXT(CLAY_STRING("Layout by Clay, rendered by cross-end primitives"),
                      CLAY_TEXT_CONFIG({.fontId = 0, .fontSize = 14, .textColor = TEXT}));
            CLAY(CLAY_ID("Button"), {
                .layout = {
                    .sizing = {CLAY_SIZING_FIXED(140), CLAY_SIZING_FIXED(40)},
                    .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER},
                },
                .backgroundColor = BUTTON,
                .cornerRadius = CLAY_CORNER_RADIUS(6),
            }) {
                CLAY_TEXT(CLAY_STRING("Open"),
                          CLAY_TEXT_CONFIG({.fontId = 0, .fontSize = 16,
                                            .textColor = (Clay_Color){255,255,255,255}}));
            }
        }
    }
    return Clay_EndLayout(0.016f);
}
