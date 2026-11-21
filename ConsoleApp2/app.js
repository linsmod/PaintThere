// app.js - Windows Forms 控件渲染库

import { resolveColor } from './systemColors.js';
// 全局 Canvas 上下文
let ctx = null;
let dpiScale = 1;

function initCanvas(canvas, color, designWidth, designHeight) {
    ctx = canvas.getContext('2d');

    //const dpr = window.devicePixelRatio || 1;
    const dpr = 1;
    // 1. 设置容器为设计尺寸
    const container = canvas.parentElement;
    container.style.width = `100%`;
    container.style.height = `100%`;

    // 2. 计算物理尺寸（不乘以DPI缩放！）

    canvas.width = Math.round(designWidth * dpr);
    canvas.height = Math.round(designHeight * dpr);

    canvas.style.width = `${Math.round(designWidth / dpr / 1.5)}px`;
    canvas.style.height = `${Math.round(designHeight / dpr / 1.5)}px`;

    //canvas.style.width = `${designWidth / dpr}px`;
    //canvas.style.height = `${designHeight / dpr}px`;
    // 4. 坐标系缩放（抵消DPI影响）
    ctx.scale(dpr, dpr);

    // 绘制背景
    ctx.fillStyle = color || '#FFFFFF';
    //ctx.fillRect(0, 0, designWidth, designHeight);
    // 初始化时关闭全局抗锯齿（匹配WinForm风格）
    ctx.imageSmoothingEnabled = false;      // 禁用图像插值
    ctx.textRendering = "optimizeSpeed";     // 文本锐利模式
    return ctx;
}

// 资源缓存：imgId -> { image, ready, failed }
const imageCache = new Map();
// 染色缓存：`imgId|tint` -> HTMLCanvasElement
const tintedCache = new Map();
let drawImgCtx = null;

function setDrawImgTarget(target) {
    drawImgCtx = target;
}

function loadImg(imgId, dataUrl) {
    if (imageCache.has(imgId)) return;
    const entry = { image: null, ready: false, failed: false };
    imageCache.set(imgId, entry);
    const img = new Image();
    img.onload = () => {
        entry.image = img;
        entry.ready = true;
        tintedCache.clear();
    };
    img.onerror = () => { entry.failed = true; };
    img.src = dataUrl;
}

function getTintedImage(imgId, tint) {
    const key = imgId + '|' + (tint || '');
    if (tintedCache.has(key)) return tintedCache.get(key);
    const entry = imageCache.get(imgId);
    if (!entry || !entry.ready || !entry.image) return null;
    const src = entry.image;
    const off = document.createElement('canvas');
    off.width = src.naturalWidth || src.width;
    off.height = src.naturalHeight || src.height;
    const c = off.getContext('2d');
    c.drawImage(src, 0, 0);
    if (tint) {
        c.globalCompositeOperation = 'source-in';
        c.fillStyle = tint;
        c.fillRect(0, 0, off.width, off.height);
        c.globalCompositeOperation = 'source-over';
    }
    tintedCache.set(key, off);
    return off;
}

function drawImg(imgId, x, y, w, h, tint, cornerRadius = 0) {
    const target = drawImgCtx || ctx;
    if (!target) return;
    const src = getTintedImage(imgId, tint);
    if (!src) return; // 资源未就绪时静默跳过，不打断既有渲染
    const dw = w || src.width, dh = h || src.height;
    if (cornerRadius > 0) {
        target.save();
        target.beginPath();
        if (target.roundRect) target.roundRect(x, y, dw, dh, cornerRadius);
        else target.rect(x, y, dw, dh);
        target.clip();
    }
    target.drawImage(src, x, y, dw, dh);
    if (cornerRadius > 0) target.restore();
}

// VM 指令化桌面绘制原语：只做绘制，不含任何窗口/桌面语义
const fontTable = new Map();
function registerFont(fontId, family) { fontTable.set(fontId, family); }
const primitiveClipStack = [];
const overlayStack = [];

function applyOverlay(x, y, w, h) {
    if (!overlayStack.length) return;
    ctx.save();
    ctx.fillStyle = resolveColor(overlayStack[overlayStack.length - 1]);
    ctx.fillRect(x, y, w, h);
    ctx.restore();
}

function clear(x, y, w, h) {
    if (x === undefined) ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height);
    else ctx.clearRect(x, y, w, h);
}

function drawRect(x, y, w, h, color, cornerRadius = 0) {
    ctx.save();
    ctx.fillStyle = resolveColor(color);
    if (cornerRadius > 0) {
        ctx.beginPath();
        if (ctx.roundRect) ctx.roundRect(x, y, w, h, cornerRadius);
        else ctx.rect(x, y, w, h);
        ctx.fill();
    } else {
        ctx.fillRect(x, y, w, h);
    }
    ctx.restore();
    applyOverlay(x, y, w, h);
}

function drawBorder(x, y, w, h, color, widths, cornerRadius = 0) {
    const [l, r, t, b] = widths || [0, 0, 0, 0];
    ctx.save();
    ctx.fillStyle = resolveColor(color);
    if (l > 0) ctx.fillRect(x, y, l, h);
    if (r > 0) ctx.fillRect(x + w - r, y, r, h);
    if (t > 0) ctx.fillRect(x, y, w, t);
    if (b > 0) ctx.fillRect(x, y + h - b, w, b);
    ctx.restore();
    applyOverlay(x, y, w, h);
}

function drawText(x, y, w, h, text, fontId, fontSize, color, letterSpacing = 0, lineHeight = 0) {
    const family = fontTable.get(fontId) || 'sans-serif';
    ctx.save();
    ctx.font = `${fontSize}px ${family}`;
    ctx.fillStyle = resolveColor(color);
    ctx.textBaseline = 'top';
    if (letterSpacing && 'letterSpacing' in ctx) ctx.letterSpacing = `${letterSpacing}px`;
    const lh = lineHeight || fontSize;
    String(text).split('\n').forEach((line, i) => ctx.fillText(line, x, y + i * lh));
    ctx.restore();
    applyOverlay(x, y, w, h);
}

function clipPush(x, y, w, h) {
    ctx.save();
    ctx.beginPath();
    ctx.rect(x, y, w, h);
    ctx.clip();
    primitiveClipStack.push(true);
}

function clipPop() {
    if (!primitiveClipStack.length) return;
    primitiveClipStack.pop();
    ctx.restore();
}

function overlayPush(color) { overlayStack.push(color); }
function overlayPop() { if (overlayStack.length) overlayStack.pop(); }

function drainPrimitiveState() {
    while (primitiveClipStack.length) { primitiveClipStack.pop(); ctx.restore(); }
    overlayStack.length = 0;
}

let bounds = {
    x: 0,
    y: 0,
    w: 1024,
    h: 768
}
function setBounds(width, height, x, y) {
    bounds.w = width;
    bounds.h = height;
    bounds.x = x;
    bounds.y = y;
}
function drawBackground(color) {
    ctx.fillStyle = resolveColor(color);
    ctx.fillRect(0, 0, ctx.canvas.width, ctx.canvas.height);
}

// 标签绘制函数
function drawLabel(text, x, y, width, height, foreColor, backColor, font, align = 'left') {
    // 保存当前状态
    ctx.save();

    // 透明背景（Label通常没有背景）
    ctx.fillStyle = 'transparent';
    ctx.fillRect(x, y, width, height);

    // 设置文本样式
    ctx.fillStyle = resolveColor(foreColor);
    ctx.font = font;
    //console.log(font);
    //ctx.textBaseline = 'middle';
    ctx.textAlign = align;

    // 计算文本位置（垂直居中）
    const textX = x + 3;
    const textY = y + height / 2;

    // 文本溢出处理
    const metrics = ctx.measureText(text);
    if (metrics.width > width - 6) {
        // 如果文本太长，添加省略号
        let truncatedText = text;
        while (ctx.measureText(truncatedText + '...').width > width - 6 && truncatedText.length > 0) {
            truncatedText = truncatedText.substring(0, truncatedText.length - 1);
        }
        ctx.fillText(truncatedText + '...', textX, textY);
    } else {
        ctx.fillText(text, textX, textY);
    }

    // 恢复状态
    ctx.restore();
}
function drawButton(text, x, y, width, height, foreColor, backColor, font) {
    // 保存当前状态
    ctx.save();

    // 解析颜色（支持CSS变量）
    const resolvedBackColor = resolveColor(backColor);
    const resolvedForeColor = resolveColor(foreColor);

    // 创建按钮渐变背景
    const gradient = ctx.createLinearGradient(x, y, x, y + height);
    gradient.addColorStop(0, lightenColor(resolvedBackColor, 15));  // 顶部稍亮
    gradient.addColorStop(1, resolvedBackColor);                    // 底部原始颜色

    // 绘制圆角矩形背景
    const borderRadius = 4;
    ctx.beginPath();
    ctx.moveTo(x + borderRadius, y);
    ctx.lineTo(x + width - borderRadius, y);
    ctx.quadraticCurveTo(x + width, y, x + width, y + borderRadius);
    ctx.lineTo(x + width, y + height - borderRadius);
    ctx.quadraticCurveTo(x + width, y + height, x + width - borderRadius, y + height);
    ctx.lineTo(x + borderRadius, y + height);
    ctx.quadraticCurveTo(x, y + height, x, y + height - borderRadius);
    ctx.lineTo(x, y + borderRadius);
    ctx.quadraticCurveTo(x, y, x + borderRadius, y);
    ctx.closePath();

    // 填充和描边
    ctx.fillStyle = gradient;
    ctx.fill();
    ctx.strokeStyle = '#707070';
    ctx.lineWidth = 1 / dpiScale;
    ctx.stroke();

    // 添加内阴影效果
    ctx.strokeStyle = 'rgba(255,255,255,0.3)';
    ctx.lineWidth = 1 / dpiScale;
    ctx.beginPath();
    ctx.moveTo(x + 1, y + 1);
    ctx.lineTo(x + width - 1, y + 1);
    ctx.stroke();

    // 设置文本样式
    ctx.fillStyle = resolvedForeColor;
    ctx.font = font;
    ctx.textBaseline = 'middle';
    ctx.textAlign = 'center';

    // 绘制文本（带1px垂直偏移更符合Windows风格）
    const textX = x + width / 2;
    const textY = y + height / 2 + 1;
    ctx.fillText(text, textX, textY);

    // 恢复状态
    ctx.restore();
}

// 链接标签绘制函数
function drawLinkLabel(text, x, y, width, height, linkStart, linkLength, linkColor, visitedColor, font) {
    // 保存当前状态
    ctx.save();

    // 透明背景
    ctx.fillStyle = 'transparent';
    ctx.fillRect(x, y, width, height);

    // 设置字体
    ctx.font = font;
    ctx.textBaseline = 'middle';

    // 计算文本位置
    const textX = x + 5;
    const textY = y + height / 2;

    // 绘制非链接部分
    if (linkStart > 0) {
        const beforeText = text.substring(0, linkStart);
        ctx.fillStyle = foreColor || '#000000';
        ctx.fillText(beforeText, textX, textY);
    }

    // 绘制链接部分
    const linkText = text.substring(linkStart, linkStart + linkLength);
    const metrics = ctx.measureText(linkText);
    const linkWidth = metrics.width;
    ctx.fillStyle = linkColor;
    ctx.fillText(linkText, textX + ctx.measureText(text.substring(0, linkStart)).width, textY);

    // 绘制下划线
    ctx.strokeStyle = linkColor;
    ctx.lineWidth = 1 / dpiScale;
    ctx.beginPath();
    const underlineY = textY + 8;
    ctx.moveTo(textX + ctx.measureText(text.substring(0, linkStart)).width, underlineY);
    ctx.lineTo(textX + ctx.measureText(text.substring(0, linkStart)).width + linkWidth, underlineY);
    ctx.stroke();

    // 绘制剩余文本
    if (linkStart + linkLength < text.length) {
        const afterText = text.substring(linkStart + linkLength);
        ctx.fillStyle = foreColor || '#000000';
        ctx.fillText(afterText, textX + ctx.measureText(text.substring(0, linkStart + linkLength)).width, textY);
    }

    // 恢复状态
    ctx.restore();
}
function drawControl(type, name, x, y, width, height, foreColor, backColor, font) {
    // 保存当前状态
    ctx.save();

    // 设置样式
    ctx.fillStyle = resolveColor(backColor);
    ctx.strokeStyle = '#cccccc';
    ctx.lineWidth = 1 / dpiScale;

    // 特殊处理 StatusStrip
    if (type.toLowerCase() === 'statusstrip') {
        // 绘制背景（带顶部边框线）
        ctx.fillRect(x, y, width, height);
        ctx.beginPath();
        ctx.moveTo(x, y);
        ctx.lineTo(x + width, y);
        ctx.strokeStyle = '#a0a0a0';
        ctx.stroke();

        // 设置文本样式
        ctx.fillStyle = resolveColor(foreColor);
        ctx.font = font;
        ctx.textBaseline = 'middle';
        ctx.textAlign = 'left';

        // 绘制状态条文本（居中显示控件名称）
        const textX = x + 10;
        const textY = y + height / 2;
        ctx.fillText(name, textX, textY);
    }
    else {
        // 默认控件绘制逻辑
        ctx.fillRect(x, y, width, height);
        ctx.strokeRect(x, y, width, height);

        // 绘制控件类型文本
        ctx.fillStyle = resolveColor(foreColor);
        ctx.font = font;
        ctx.textBaseline = 'middle';
        ctx.textAlign = 'center';
        ctx.fillText(type, x + width / 2, y + height / 2);
    }

    // 恢复状态
    ctx.restore();
}

// 增强版文本框绘制函数
function drawEnhancedTextBox(text, x, y, width, height, foreColor, backColor, font,
    multiline, readOnly, borderStyle,
    useSystemPasswordChar, textAlign, passwordChar,
    characterCasing, acceptsReturn, placeholderText, enabled) {
    // 保存当前绘图状态
    ctx.save();

    // 设置背景颜色，根据 enabled 状态决定颜色
    let bgColor = (enabled && !readOnly) ? resolveColor(backColor) : '#f0f0f0';
    ctx.fillStyle = bgColor;
    ctx.fillRect(x, y, width, height);

    // 绘制边框
    if (borderStyle === 'Fixed3D') {
        // 3D 边框效果
        ctx.strokeStyle = '#808080';
        ctx.lineWidth = 1 / dpiScale;
        ctx.strokeRect(x + 0.5, y + 0.5, width - 1, height - 1);
        ctx.strokeStyle = '#FFFFFF';
        ctx.strokeRect(x + 1.5, y + 1.5, width - 3, height - 3);
    } else {
        // 默认单线边框
        ctx.strokeStyle = '#888';
        ctx.strokeRect(x, y, width, height);
    }

    // 处理密码字符显示
    let displayText = text;
    if (useSystemPasswordChar || passwordChar) {
        displayText = passwordChar ?
            new Array(text.length + 1).join(passwordChar) :
            new Array(text.length + 1).join('•');
    }

    // 处理字符大小写转换
    switch (characterCasing.toLowerCase()) {
        case 'upper': displayText = displayText.toUpperCase(); break;
        case 'lower': displayText = displayText.toLowerCase(); break;
    }

    // 处理占位文本
    if (!displayText && placeholderText) {
        displayText = placeholderText;
        foreColor = '#808080'; // 占位文本颜色为灰色
    }

    // 设置文本样式，根据 enabled 状态决定文字颜色
    ctx.fillStyle = enabled ? resolveColor(foreColor) : '#808080';
    ctx.font = font;
    ctx.textBaseline = 'top';

    // 设置文本对齐方式
    let textX;
    switch (textAlign.toLowerCase()) {
        case 'right': textX = x + width - 5; break;
        case 'center': textX = x + width / 2; break;
        default: textX = x + 5;
    }

    const textY = y + 5;
    //console.log('drawEnhancedTextBox',text,textX,textY)
    // 绘制文本，根据 multiline 决定单行或多行绘制
    if (multiline) {
        // 多行文本处理
        const lines = wrapText(displayText, width - 10, font);
        const lineHeight = 20;
        const maxLines = Math.floor(height / lineHeight);
        for (let i = 0; i < Math.min(lines.length, maxLines); i++) {
            ctx.fillText(lines[i], textX, textY + i * lineHeight);
        }
    } else {
        // 单行文本绘制
        ctx.fillText(displayText, textX, textY);
    }

    // 恢复绘图状态
    ctx.restore();
}
function drawCheckBox(text, x, y, width, height, foreColor, backColor, font, isChecked) {
    // 保存当前状态
    ctx.save();

    // 绘制背景
    ctx.fillStyle = resolveColor(backColor);
    ctx.fillRect(x, y, width, height);

    // 计算复选框位置和大小
    const boxSize = Math.min(16, height - 4);
    const boxX = x + 2;
    const boxY = y + (height - boxSize) / 2;

    // 绘制复选框边框
    ctx.strokeStyle = '#7A7A7A';
    ctx.lineWidth = 1 / dpiScale;
    ctx.strokeRect(boxX, boxY, boxSize, boxSize);

    // 绘制复选框背景
    ctx.fillStyle = '#FFFFFF';
    ctx.fillRect(boxX + 1, boxY + 1, boxSize - 2, boxSize - 2);

    // 绘制勾选状态
    if (isChecked) {
        ctx.strokeStyle = foreColor;
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(boxX + 3, boxY + boxSize / 2);
        ctx.lineTo(boxX + boxSize / 2, boxY + boxSize - 3);
        ctx.lineTo(boxX + boxSize - 3, boxY + 3);
        ctx.stroke();
    }

    // 绘制文本
    ctx.fillStyle = resolveColor(foreColor);
    ctx.font = font;
    ctx.textBaseline = 'middle';
    ctx.textAlign = 'left';

    const textX = boxX + boxSize + 5;
    const textY = y + height / 2;
    ctx.fillText(text, textX, textY);

    // 恢复状态
    ctx.restore();
}

// 辅助函数：颜色亮度调整
function lightenColor(color, percent) {
    const num = parseInt(color.replace('#', ''), 16);
    const amt = Math.round(2.55 * percent);
    const R = Math.min(255, (num >> 16) + amt);
    const G = Math.min(255, (num >> 8 & 0x00FF) + amt);
    const B = Math.min(255, (num & 0x0000FF) + amt);
    return `#${(1 << 24 | R << 16 | G << 8 | B).toString(16).slice(1)}`;
}

// 文本换行辅助函数
function wrapText(text, maxWidth, font) {
    ctx.font = font;
    const words = text.split(' ');
    const lines = [];
    let currentLine = words[0] || '';

    for (let i = 1; i < words.length; i++) {
        const word = words[i];
        const width = ctx.measureText(currentLine + ' ' + word).width;
        if (width < maxWidth) {
            currentLine += ' ' + word;
        } else {
            lines.push(currentLine);
            currentLine = word;
        }
    }
    lines.push(currentLine);
    return lines;
}

// 结构化指令分发表：避免 eval 解析字符串
const commandHandlers = {
    setBounds,
    drawBackground,
    drawLabel,
    drawButton,
    drawCheckBox,
    drawLinkLabel,
    drawEnhancedTextBox,
    drawControl,
    loadImg,
    drawImg,
    clear,
    drawRect,
    drawBorder,
    drawText,
    clipPush,
    clipPop,
    overlayPush,
    overlayPop
};

// ES模块导出
export {
    initCanvas,
    setBounds,
    drawBackground,
    drawLabel,
    drawButton,
    drawCheckBox,
    drawLinkLabel,
    drawEnhancedTextBox,
    drawControl,
    loadImg,
    drawImg,
    setDrawImgTarget,
    registerFont,
    drainPrimitiveState,
    clear,
    drawRect,
    drawBorder,
    drawText,
    clipPush,
    clipPop,
    overlayPush,
    overlayPop,
    commandHandlers
};

// CommonJS兼容
//if (typeof module !== 'undefined' && module.exports) {
//    module.exports = {
//        initCanvas,
//        setBounds,
//        drawBackground,
//        drawLabel,
//        drawButton,
//        drawCheckBox,
//        drawLinkLabel,
//        drawEnhancedTextBox,
//        drawControl
//    };
//}