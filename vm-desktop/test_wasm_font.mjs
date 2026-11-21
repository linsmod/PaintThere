import fs from 'fs';

const fontPath = process.argv[2] || 'H:/github_repos/mystbfont_c/fonts/NotoSans-Regular.ttf';
const fontSize = Number(process.argv[3] || 24);
const text = process.argv[4] || 'Hello World';

const StbFontWasm = (await import('./stb_font_wasm.mjs')).default;
const mod = await StbFontWasm();

const ttf = fs.readFileSync(fontPath);
const ttfPtr = mod._alloc(ttf.length);
mod.HEAPU8.set(ttf, ttfPtr);
const rc = mod._loadFont(0, ttfPtr, ttf.length);
if (rc !== 0) throw new Error('loadFont failed: ' + rc);

const textBytes = Buffer.from(text, 'utf8');
const textPtr = mod._alloc(textBytes.length + 1);
mod.HEAPU8.set(textBytes, textPtr);
mod.HEAPU8[textPtr + textBytes.length] = 0;

const width = mod._measureText(0, fontSize, textPtr, textBytes.length);
const wPtr = mod._alloc(4);
const hPtr = mod._alloc(4);
const cap = 4096 * 4096 * 4;
const outPtr = mod._alloc(cap);
const rrc = mod._rasterizeText(0, fontSize, textPtr, textBytes.length, outPtr, cap, wPtr, hPtr);
if (rrc !== 0) throw new Error('rasterizeText failed: ' + rrc);

const outW = mod.HEAPU32[wPtr >> 2];
const outH = mod.HEAPU32[hPtr >> 2];
const rgba = Buffer.from(mod.HEAPU8.subarray(outPtr, outPtr + outW * outH * 4));
let nonZero = 0;
for (let i = 3; i < rgba.length; i += 4) if (rgba[i] > 0) nonZero++;

console.log(JSON.stringify({ fontId: 0, fontSize, text, width, outW, outH, nonZeroAlpha: nonZero }));
