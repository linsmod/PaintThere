const systemColors = {
    "ActiveBorder": "rgba(240, 240, 240, 1.00)",
    "ActiveCaption": "rgba(192, 192, 192, 1.00)",
    "ActiveCaptionText": "rgba(255, 255, 255, 1.00)",
    "AppWorkspace": "rgba(182, 182, 182, 1.00)",
    "ButtonFace": "rgba(240, 240, 240, 1.00)",
    "ButtonHighlight": "rgba(255, 255, 255, 1.00)",
    "ButtonShadow": "rgba(160, 160, 160, 1.00)",
    "Control": "rgba(240, 240, 240, 1.00)",
    "ControlDark": "rgba(212, 208, 200, 1.00)",
    "ControlDarkDark": "rgba(152, 152, 148, 1.00)",
    "ControlLight": "rgba(255, 255, 255, 1.00)",
    "ControlLightLight": "rgba(255, 255, 255, 1.00)",
    "ControlText": "rgba(0, 0, 0, 1.00)",
    "Desktop": "rgba(0, 128, 192, 1.00)",
    "GradientActiveCaption": "rgba(226, 227, 231, 1.00)",
    "GradientInactiveCaption": "rgba(239, 239, 243, 1.00)",
    "GrayText": "rgba(109, 109, 109, 1.00)",
    "Highlight": "rgba(51, 153, 255, 1.00)",
    "HighlightText": "rgba(255, 255, 255, 1.00)",
    "HotTrack": "rgba(255, 0, 0, 1.00)",
    "InactiveBorder": "rgba(240, 240, 240, 1.00)",
    "InactiveCaption": "rgba(212, 208, 200, 1.00)",
    "InactiveCaptionText": "rgba(0, 0, 0, 1.00)",
    "Info": "rgba(255, 255, 225, 1.00)",
    "InfoText": "rgba(0, 0, 0, 1.00)",
    "Menu": "rgba(240, 240, 240, 1.00)",
    "MenuBar": "rgba(240, 240, 240, 1.00)",
    "MenuHighlight": "rgba(51, 153, 255, 1.00)",
    "MenuText": "rgba(0, 0, 0, 1.00)",
    "ScrollBar": "rgba(228, 228, 228, 1.00)",
    "Window": "rgba(255, 255, 255, 1.00)",
    "WindowFrame": "rgba(107, 107, 107, 1.00)",
    "WindowText": "rgba(0, 0, 0, 1.00)"
};
function injectSystemColorVariables() {
    const style = document.createElement('style');
    let cssText = ':root {\n';

    for (const [name, value] of Object.entries(systemColors)) {
        cssText += `  --color-${name}: ${value};\n`;
    }

    cssText += '}\n';

    style.textContent = cssText;
    document.head.appendChild(style);
}
// 新增：解析颜色值（支持CSS变量和常规颜色）
export function resolveColor(colorValue, fallbackColor) {
    if (colorValue.startsWith('--color-')) {
        // 从系统颜色中查找
        const colorName = colorValue.substring(8); // 去掉'--color-'前缀
        return systemColors[colorName] || fallbackColor || '#FFFFFF'; // 默认白色
    }
    else if (systemColors[colorValue]) {
        return systemColors[colorValue];
    }
    else {
        return (colorValue || fallbackColor || '#FFFFFF').toLowerCase()
    }
}
injectSystemColorVariables();