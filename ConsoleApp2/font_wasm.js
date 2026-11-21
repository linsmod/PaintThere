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

async function rasterizeText(fontId, fontSize, text) {
    const m = await ensureModule();
    const key = `${fontId}|${fontSize}|${text}`;
    const cached = rasterCache.get(key);
    if (cached) return cached;

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

function drawRaster(ctx, raster, x, y, color) {
    // 先在离屏 canvas 上把白色字形染成目标色，再画到目标，避免 source-in 影响目标已有内容
    const tinted = document.createElement('canvas');
    tinted.width = raster.w;
    tinted.height = raster.h;
    const tc = tinted.getContext('2d');
    tc.drawImage(raster.canvas, 0, 0);
    if (color) {
        tc.globalCompositeOperation = 'source-in';
        tc.fillStyle = color;
        tc.fillRect(0, 0, raster.w, raster.h);
        tc.globalCompositeOperation = 'source-over';
    }
    ctx.drawImage(tinted, x, y);
}

function hasRaster(fontId, fontSize, text) {
    return rasterCache.has(`${fontId}|${fontSize}|${text}`);
}

function getRaster(fontId, fontSize, text) {
    return rasterCache.get(`${fontId}|${fontSize}|${text}`) || null;
}

export { loadFont, rasterizeText, drawRaster, hasRaster, getRaster, ensureModule };
