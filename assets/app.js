// app.js - Windows Forms 控件渲染库

import { resolveColor } from './systemColors.js';
import { loadFont as wasmLoadFont, rasterizeText, drawRaster, drawRasterScaled, getRaster } from './font_wasm.js';

let textNeedsReplay = null;
function setTextNeedsReplay(cb) { textNeedsReplay = cb; }
// 全局 Canvas 上下文
let ctx = null;
let dpiScale = 1;
/* 文字光栅化倍率：渲染层按「设备像素比 × 显示缩放」设置。
   字形按 fontSize × 倍率光栅，绘制时缩回逻辑尺寸 —— 高分屏与放大显示下才清晰。 */
let textScale = 1;
function setTextScale(scale) {
    if (scale > 0 && Math.abs(scale - textScale) > 0.001) {
        textScale = scale;
        if (textNeedsReplay) textNeedsReplay();
    }
}

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
const patternCache = new Map();  // patternId -> descriptor
const styleCache = new Map();    // styleId -> {bgPattern?, ...}
const programCache = new Map();  // programId -> {commands, layerId, mode, running}
const layerCanvases = new Map(); // layerId -> canvas
let drawImgCtx = null;

function resolvePaint(value, fallback) {
    if (typeof value === 'string' && styleCache.has(value)) {
        const style = styleCache.get(value);
        if (style.bgPattern && patternCache.has(style.bgPattern)) {
            return patternToFill(patternCache.get(style.bgPattern), fallback);
        }
        if (style.textColor) return style.textColor;
    }
    return value === undefined ? fallback : value;
}

function patternToFill(desc, fallback) {
    if (!desc || typeof desc !== 'object') return fallback;
    if (desc.type === 'solid') return desc.color || fallback;
    if (desc.type === 'gradient' || desc.type === 'radial') {
        // CanvasGradient needs ctx; handled at draw time via gradientFill.
        return desc;
    }
    if (desc.type === 'image' && imageCache.has(desc.imgId)) {
        const entry = imageCache.get(desc.imgId);
        return entry.ready ? entry.image : (desc.color || fallback);
    }
    return fallback;
}

function setLayerCanvas(layerId, canvas) {
    layerCanvases.set(layerId, canvas);
}

function definePattern(patternId, descriptor) {
    if (descriptor && typeof descriptor === 'object') patternCache.set(patternId, descriptor);
}

function defineStyle(styleId, config) {
    if (config && typeof config === 'object') styleCache.set(styleId, config);
}

/* 资源包：guest 只下发 id（loadResPack），资源本体由 client 自己 GET 拉取并按 id 缓存。
   刷新页面后重新拉同一个 id 即可恢复主题；同一 id 只解码一次。 */
const resPackCache = new Map();
let resPackApplied = null;
function setResPackApplied(cb) { resPackApplied = cb; }

function applyResPackCommands(list) {
    if (!Array.isArray(list)) return;
    for (const c of list) {
        const fn = commandHandlers[c.cmd];
        if (fn) {
            try { fn(...c.args); } catch (e) { console.error('respack cmd failed:', c.cmd, e); }
        }
    }
    replayBackgroundPrograms();
    /* 已画出的 tile 用的是旧颜色：通知渲染层按新缓存重放。 */
    if (resPackApplied) resPackApplied();
}

/* 资源包容器（与 vm_server.c 的 vm_send_respack 对应，全部小端）：
   u32 magic "VMRP" | u32 jsonLen | u32 imgCount
   imgCount × { u16 idLen, id, u8 mimeLen, mime, u32 dataLen } | json | images */
const RESPACK_MAGIC = 0x50524D56;

function registerImageBlob(imgId, blob) {
    if (imageCache.has(imgId)) return;
    const entry = { image: null, ready: false, failed: false };
    imageCache.set(imgId, entry);
    const done = (src) => {
        entry.image = src;
        entry.ready = true;
        tintedCache.clear();
        if (textNeedsReplay) textNeedsReplay();
        /* 图片是异步解码的：就绪后按新缓存重放，否则首帧会画不出位图。 */
        if (resPackApplied) resPackApplied();
    };
    if (typeof createImageBitmap === 'function') {
        createImageBitmap(blob).then(done).catch(() => { entry.failed = true; });
    } else {
        const img = new Image();
        img.onload = () => done(img);
        img.onerror = () => { entry.failed = true; };
        img.src = URL.createObjectURL(blob);
    }
}

function parseResPack(buf) {
    const dv = new DataView(buf);
    if (buf.byteLength < 12 || dv.getUint32(0, true) !== RESPACK_MAGIC) return null;
    const jsonLen = dv.getUint32(4, true);
    const imgCount = dv.getUint32(8, true);
    const dec = new TextDecoder();
    let off = 12;
    const entries = [];
    for (let i = 0; i < imgCount; i++) {
        if (off + 2 > buf.byteLength) return null;
        const idLen = dv.getUint16(off, true); off += 2;
        if (off + idLen + 1 > buf.byteLength) return null;
        const id = dec.decode(new Uint8Array(buf, off, idLen)); off += idLen;
        const mimeLen = dv.getUint8(off); off += 1;
        if (off + mimeLen + 4 > buf.byteLength) return null;
        const mime = dec.decode(new Uint8Array(buf, off, mimeLen)); off += mimeLen;
        const size = dv.getUint32(off, true); off += 4;
        entries.push({ id, mime, size, offset: 0, blob: null });
    }
    if (off + jsonLen > buf.byteLength) return null;
    const list = JSON.parse(dec.decode(new Uint8Array(buf, off, jsonLen)));
    off += jsonLen;
    for (const e of entries) {
        if (off + e.size > buf.byteLength) return null;
        e.offset = off;
        off += e.size;
    }
    return { list, entries };
}

function applyResPack(pack) {
    if (!pack) return;
    for (const e of pack.entries || []) if (e.blob) registerImageBlob(e.id, e.blob);
    applyResPackCommands(pack.list);
}

function loadResPack(resPackId) {
    if (!resPackId) return;
    const cached = resPackCache.get(resPackId);
    if (cached) { applyResPack(cached); return; }
    if (typeof fetch !== 'function') return;   // 无网络原语的宿主跳过
    const base = (typeof location !== 'undefined' && location.protocol !== 'file:')
        ? location.origin : '';
    fetch(base + '/respack/' + encodeURIComponent(resPackId), { cache: 'no-store' })
        .then((r) => {
            if (!r.ok) throw new Error('respack ' + r.status);
            return r.arrayBuffer();
        })
        .then((buf) => {
            const pack = parseResPack(buf);
            if (!pack) throw new Error('respack format');
            for (const e of pack.entries) {
                e.blob = new Blob([new Uint8Array(buf, e.offset, e.size)], { type: e.mime });
            }
            resPackCache.set(resPackId, pack);
            applyResPack(pack);
        })
        .catch((e) => console.error('loadResPack failed:', resPackId, e));
}

function defineDrawProgram(programId, commands, layerId) {
    programCache.set(programId, {
        commands: Array.isArray(commands) ? commands : [],
        layerId: layerId || 'windows',
        mode: 'once',
        running: false,
    });
}

function runProgram(programId, targetCtx) {
    const p = programCache.get(programId);
    if (!p || !targetCtx) return;
    const prevCtx = ctx;
    ctx = targetCtx;
    try {
        for (const c of p.commands) {
            const fn = commandHandlers[c.cmd];
            if (fn) { try { fn(...c.args); } catch (e) { console.error('program cmd failed:', c.cmd, e); } }
        }
    } finally {
        ctx = prevCtx;
    }
}

function startDrawProgram(programId, mode) {
    const p = programCache.get(programId);
    if (!p) return;
    p.mode = mode || 'once';
    p.running = true;
    const layerCanvas = layerCanvases.get(p.layerId);
    runProgram(programId, layerCanvas ? layerCanvas.getContext('2d') : ctx);
}

function pauseDrawProgram(programId) {
    const p = programCache.get(programId);
    if (p) p.running = false;
}

function resumeDrawProgram(programId) {
    const p = programCache.get(programId);
    if (!p) return;
    p.running = true;
    const layerCanvas = layerCanvases.get(p.layerId);
    runProgram(programId, layerCanvas ? layerCanvas.getContext('2d') : ctx);
}

/* Called by the frame loop to replay 'once' programs bound to background layer. */
function replayBackgroundPrograms() {
    const bgCanvas = layerCanvases.get('bg');
    if (!bgCanvas) return;
    const bgCtx = bgCanvas.getContext('2d');
    bgCtx.clearRect(0, 0, bgCanvas.width, bgCanvas.height);
    for (const [id, p] of programCache) {
        if (p.running && p.layerId === 'bg') runProgram(id, bgCtx);
    }
}

function setDrawImgTarget(target) {
    drawImgCtx = target;
}

/* 切换后续绘制原语的绘制目标：前端内部渲染机制（例如 tile 小画布），不是跨端协议概念。 */
function setDrawTarget(target) {
    ctx = target;
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
        if (textNeedsReplay) textNeedsReplay();
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

/* imgId 可带 "#u0,v0,u1,v1" 指定源图的归一化子区域：
   XP 的三态图是竖排雪碧（普通/悬停/按下各占 1/3），靠这个取其中一段，
   不需要在 guest 侧解码重编码 PNG。 */
function parseImgId(imgId) {
    if (typeof imgId !== 'string') return { id: imgId, src: null };
    const hash = imgId.indexOf('#');
    if (hash <= 0) return { id: imgId, src: null };
    const p = imgId.slice(hash + 1).split(',').map(Number);
    if (p.length !== 4 || !p.every((v) => Number.isFinite(v))) return { id: imgId, src: null };
    return { id: imgId.slice(0, hash), src: p };
}

function drawImg(imgId, x, y, w, h, tint, cornerRadius = 0) {
    const target = drawImgCtx || ctx;
    if (!target) return;
    const parsed = parseImgId(imgId);
    const src = getTintedImage(parsed.id, tint);
    if (!src) return; // 资源未就绪时静默跳过，不打断既有渲染
    const dw = w || src.width, dh = h || src.height;
    if (cornerRadius > 0) {
        target.save();
        target.beginPath();
        if (target.roundRect) target.roundRect(x, y, dw, dh, cornerRadius);
        else target.rect(x, y, dw, dh);
        target.clip();
    }
    if (parsed.src) {
        const sw = src.width, sh = src.height;
        target.drawImage(src,
                         parsed.src[0] * sw, parsed.src[1] * sh,
                         parsed.src[2] * sw, parsed.src[3] * sh,
                         x, y, dw, dh);
    } else {
        target.drawImage(src, x, y, dw, dh);
    }
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

/* 把 pattern descriptor 构造为 CanvasGradient。多 stop 优先；
   direction 是 "x0,y0,x1,y1"（线性）或 "cx,cy,r"（径向）的比例，
   解析失败退回对角线/中心径向。 */
function gradientFill(paint, x, y, w, h, fallback) {
    let g = null;
    if (paint.type === 'radial') {
        let cx = 0.5, cy = 0.5, r = 0.5;
        if (paint.direction) {
            const p = paint.direction.split(',').map(Number);
            if (p.length >= 3 && p.every(Number.isFinite)) { cx = p[0]; cy = p[1]; r = p[2]; }
        }
        const maxR = Math.max(w, h) || 1;
        g = ctx.createRadialGradient(x + w * cx, y + h * cy, 0,
                                     x + w * cx, y + h * cy, maxR * r);
    } else {
        let x0 = x, y0 = y, x1 = x + w, y1 = y + h;
        if (paint.direction) {
            const p = paint.direction.split(',').map(Number);
            if (p.length >= 4 && p.every(Number.isFinite)) {
                x0 = x + w * p[0]; y0 = y + h * p[1];
                x1 = x + w * p[2]; y1 = y + h * p[3];
            }
        }
        g = ctx.createLinearGradient(x0, y0, x1, y1);
    }
    const stops = (paint.stops && paint.stops.length >= 2)
        ? paint.stops
        : [{ color: paint.from || '#FFFFFF', offset: 0 },
           { color: paint.to || '#000000', offset: 1 }];
    for (const s of stops) {
        const off = Math.max(0, Math.min(1, Number(s.offset) || 0));
        g.addColorStop(off, s.color || '#FFFFFF');
    }
    return g;
}

function drawRect(x, y, w, h, color, cornerRadius = 0) {
    ctx.save();
    const paint = resolvePaint(color, '#FFFFFF');
    if (paint && typeof paint === 'object' &&
        (paint.type === 'gradient' || paint.type === 'radial')) {
        ctx.fillStyle = gradientFill(paint, x, y, w, h, '#FFFFFF');
    } else {
        ctx.fillStyle = resolveColor(paint);
    }
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

/* 矢量填充：path 是 SVG path data，坐标取 0..1，按 rect 缩放后填充。
   DrawingBrush 这类只有几何、没有像素的皮肤走这条路（不需要位图）。 */
function drawPath(x, y, w, h, d, paint) {
    if (!ctx || !d || w <= 0 || h <= 0) return;
    if (typeof Path2D !== 'function') return;   // 无 Path2D 的宿主跳过
    ctx.save();
    ctx.translate(x, y);
    ctx.scale(w, h);
    const p = resolvePaint(paint, '#FFFFFF');
    if (p && typeof p === 'object') {
        ctx.fillStyle = gradientFill(p, 0, 0, 1, 1, '#FFFFFF');
    } else {
        ctx.fillStyle = resolveColor(p);
    }
    ctx.fill(new Path2D(d));
    ctx.restore();
    applyOverlay(x, y, w, h);
}

// 栅格化失败的 key：避免逐帧重试并逐帧刷错误日志，字体重新加载后放开
const rasterFailed = new Set();

function loadFont(fontId, ttfBase64) {
    wasmLoadFont(fontId, ttfBase64).then(() => {
        rasterFailed.clear();
        if (textNeedsReplay) textNeedsReplay();
    }).catch((e) => console.error('loadFont failed:', e));
}

function drawText(x, y, w, h, text, fontId, fontSize, color, letterSpacing = 0, lineHeight = 0) {
    const s = String(text);
    /* 按物理像素光栅：放大显示/高分屏下，用逻辑字号的位图会被插值放大而发虚。 */
    const px = Math.max(1, Math.round(fontSize * textScale));
    const raster = getRaster(fontId, px, s);
    if (!raster) {
        const key = `${fontId}|${px}|${s}`;
        if (rasterFailed.has(key)) return;
        rasterizeText(fontId, px, s).then(() => {
            if (textNeedsReplay) textNeedsReplay();
        }).catch((e) => {
            rasterFailed.add(key);
            console.error('rasterizeText failed:', e);
        });
        return;
    }
    const ink = resolveColor(color);
    if (textScale === 1) {
        drawRaster(ctx, raster, x, y, ink);
    } else {
        drawRasterScaled(ctx, raster, x, y, raster.w / textScale, raster.h / textScale, ink);
    }
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
    loadFont,
    setTextNeedsReplay,
    setLayerCanvas,
    definePattern,
    defineStyle,
    loadResPack,
    defineDrawProgram,
    startDrawProgram,
    pauseDrawProgram,
    resumeDrawProgram,
    replayBackgroundPrograms,
    clear,
    drawRect,
    drawBorder,
    drawPath,
    drawText,
    clipPush,
    clipPop,
    overlayPush,
    overlayPop
};

// ES模块导出
export {
    initCanvas,
    setTextScale,
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
    loadFont,
    setDrawImgTarget,
    setDrawTarget,
    setTextNeedsReplay,
    setLayerCanvas,
    definePattern,
    defineStyle,
    loadResPack,
    setResPackApplied,
    defineDrawProgram,
    startDrawProgram,
    pauseDrawProgram,
    resumeDrawProgram,
    replayBackgroundPrograms,
    registerFont,
    drainPrimitiveState,
    clear,
    drawRect,
    drawBorder,
    drawPath,
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