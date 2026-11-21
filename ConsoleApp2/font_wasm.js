// font_wasm.js - 用 stb_truetype wasm 本端光栅化文本，保证与 guest 度量一致
import StbFontWasmFactory from './stb_font_wasm.mjs';

let mod = null;
let initPromise = null;
const loadedFonts = new Map(); // fontId -> Uint8Array (for re-load after growth)
const rasterCache = new Map(); // key -> { w, h, canvas }

async function ensureModule() {
    if (mod) return mod;
    if (!initPromise) initPromise = StbFontWasmFactory();
    mod = await initPromise;
    return mod;
}

function base64ToBytes(b64) {
    const bin = atob(b64);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
}

async function loadFont(fontId, ttfBase64) {
    const m = await ensureModule();
    const bytes = typeof ttfBase64 === 'string' ? base64ToBytes(ttfBase64) : ttfBase64;
    const ptr = m._alloc(bytes.length);
    m.HEAPU8.set(bytes, ptr);
    const rc = m._loadFont(fontId, ptr, bytes.length);
    m._freeMem(ptr);
    if (rc !== 0) throw new Error('loadFont failed: ' + rc);
    loadedFonts.set(fontId, bytes);
    rasterCache.clear();
    return true;
}

// key -> Promise：同一 key 的并发栅格化只做一次，避免逐帧重复发起
const inflightRaster = new Map();

async function rasterizeText(fontId, fontSize, text) {
    const key = `${fontId}|${fontSize}|${text}`;
    const cached = rasterCache.get(key);
    if (cached) return cached;
    const pending = inflightRaster.get(key);
    if (pending) return pending;
    const task = doRasterize(key, fontId, fontSize, text)
        .finally(() => { inflightRaster.delete(key); });
    inflightRaster.set(key, task);
    return task;
}

async function doRasterize(key, fontId, fontSize, text) {
    const m = await ensureModule();
    const bytes = new TextEncoder().encode(String(text));
    const textPtr = m._alloc(bytes.length + 1);
    m.HEAPU8.set(bytes, textPtr);
    m.HEAPU8[textPtr + bytes.length] = 0;

    const outW = m._alloc(4);
    const outH = m._alloc(4);
    const cap = 8192 * 2048 * 4;
    const outPtr = m._alloc(cap);
    const rc = m._rasterizeText(fontId, fontSize, textPtr, bytes.length, outPtr, cap, outW, outH);
    if (rc !== 0) {
        m._freeMem(textPtr); m._freeMem(outW); m._freeMem(outH); m._freeMem(outPtr);
        throw new Error('rasterizeText failed: ' + rc);
    }
    const w = m.HEAPU32[outW >> 2];
    const h = m.HEAPU32[outH >> 2];
    const rgba = new Uint8ClampedArray(w * h * 4);
    rgba.set(m.HEAPU8.subarray(outPtr, outPtr + w * h * 4));
    m._freeMem(textPtr); m._freeMem(outW); m._freeMem(outH); m._freeMem(outPtr);

    const off = document.createElement('canvas');
    off.width = w; off.height = h;
    off.getContext('2d').putImageData(new ImageData(rgba, w, h), 0, 0);
    const result = { w, h, canvas: off };
    rasterCache.set(key, result);
    return result;
}

// raster -> Map<color, canvas>：染色结果按颜色缓存，同一字形不再每次绘制都重建离屏 canvas
const tintCache = new WeakMap();

function tintedRaster(raster, color) {
    if (!color) return raster.canvas;
    let byColor = tintCache.get(raster);
    if (!byColor) {
        byColor = new Map();
        tintCache.set(raster, byColor);
    }
    const cached = byColor.get(color);
    if (cached) return cached;

    // 先在离屏 canvas 上把白色字形染成目标色，再画到目标，避免 source-in 影响目标已有内容
    const canvas = document.createElement('canvas');
    canvas.width = raster.w;
    canvas.height = raster.h;
    const tc = canvas.getContext('2d');
    tc.drawImage(raster.canvas, 0, 0);
    tc.globalCompositeOperation = 'source-in';
    tc.fillStyle = color;
    tc.fillRect(0, 0, raster.w, raster.h);
    tc.globalCompositeOperation = 'source-over';
    byColor.set(color, canvas);
    return canvas;
}

function drawRaster(ctx, raster, x, y, color) {
    ctx.drawImage(tintedRaster(raster, color), x, y);
}

function hasRaster(fontId, fontSize, text) {
    return rasterCache.has(`${fontId}|${fontSize}|${text}`);
}

function getRaster(fontId, fontSize, text) {
    return rasterCache.get(`${fontId}|${fontSize}|${text}`) || null;
}

export { loadFont, rasterizeText, drawRaster, hasRaster, getRaster, ensureModule };
