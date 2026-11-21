// canvas-cpp(ant) 下的 VM 渲染页面：与 vm_server.c 内置页面等价。
// 直接复用共享的 app.js 原语 + wasm 字体 + EventSource，无兼容 shim。
import {
  initCanvas, commandHandlers, setLayerCanvas, replayBackgroundPrograms,
  applyFrameDiff, applyIncrementalFrame
} from './app.js';
import { loadFont } from './font_wasm.js';

const PORT = globalThis.VM_RENDERER_PORT || 9000;
const WIDTH = 800;
const HEIGHT = 600;

window.__vmRenderer = { ready: false, frames: 0, resources: 0, error: '' };

async function boot() {
  const stage = document.createElement('div');
  stage.id = 'stage';
  document.body.appendChild(stage);
  const bg = document.createElement('canvas');
  bg.id = 'bg'; bg.width = WIDTH; bg.height = HEIGHT;
  stage.appendChild(bg);
  const cv = document.createElement('canvas');
  cv.id = 'c';
  stage.appendChild(cv);

  // canvas-cpp 的 getContext 每次返回新对象且没有 ctx.canvas；app.js 依赖标准行为。
  // 在 initCanvas 前 patch：缓存同一 context 并补 canvas 反向引用。
  function patchCanvasContext(canvas) {
    const original = canvas.getContext.bind(canvas);
    let cached = null;
    canvas.getContext = function (type) {
      if (type !== '2d') return original(type);
      if (!cached) {
        cached = original(type);
        if (cached && !cached.canvas) cached.canvas = canvas;
      }
      return cached;
    };
  }
  patchCanvasContext(cv);
  patchCanvasContext(bg);

  initCanvas(cv, '#FFFFFF', WIDTH, HEIGHT);
  setLayerCanvas('bg', bg);
  replayBackgroundPrograms();

  const fr = await fetch('./font.ttf');
  if (!fr.ok) throw new Error('font.ttf fetch failed: ' + fr.status);
  const fb = new Uint8Array(await fr.arrayBuffer());
  let bs = '';
  for (let i = 0; i < fb.length; i += 8192) {
    bs += String.fromCharCode.apply(null, fb.subarray(i, i + 8192));
  }
  await loadFont(0, btoa(bs));
  await loadFont(1, btoa(bs));

  const es = new EventSource(`http://127.0.0.1:${PORT}/events`);
  es.addEventListener('resources', (e) => {
    try {
      const list = JSON.parse(e.data);
      for (const c of list) {
        const fn = commandHandlers[c.cmd];
        if (fn) fn(...c.args);
      }
      replayBackgroundPrograms();
      window.__vmRenderer.resources += list.length;
    } catch (err) { window.__vmRenderer.error = 'resource: ' + err; }
  });
  es.addEventListener('fullframe', (e) => {
    try {
      const f = JSON.parse(e.data);
      applyFrameDiff(f.commands, cv);
      window.__vmRenderer.frames++;
    } catch (err) { window.__vmRenderer.error = 'fullframe: ' + (err && err.stack ? err.stack : err); }
  });
  es.onmessage = (e) => {
    try {
      const f = JSON.parse(e.data);
      if (f.incremental) applyIncrementalFrame(f, cv);
      else applyFrameDiff(f.commands, cv);
      window.__vmRenderer.frames++;
    } catch (err) { window.__vmRenderer.error = 'frame: ' + err; }
  };
  es.onerror = () => { window.__vmRenderer.error = 'SSE disconnected'; };

  const cid = 'vm-c-' + Math.random().toString(36).slice(2);
  const palette = ['#F44336', '#2196F3', '#4CAF50', '#FF9800', '#9C27B0', '#00BCD4'];
  let h = 0;
  for (let i = 0; i < cid.length; i++) h = (h * 31 + cid.charCodeAt(i)) | 0;
  const color = palette[Math.abs(h) % palette.length];

  const send = (kind, e) => {
    const r = cv.getBoundingClientRect();
    const w = r.width || WIDTH, h = r.height || HEIGHT;
    const x = Math.round(((e.clientX || 0) - (r.left || 0)) / w * cv.width);
    const y = Math.round(((e.clientY || 0) - (r.top || 0)) / h * cv.height);
    fetch(`http://127.0.0.1:${PORT}/input`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ type: 'input', kind, x, y, button: e.button || 0,
                             clientId: cid, color })
    }).catch((err) => { window.__vmRenderer.error = 'input: ' + err; });
  };
  cv.addEventListener('mousedown', (e) => send('mousedown', e));
  document.addEventListener('mousemove', (e) => send('mousemove', e));
  document.addEventListener('mouseup', (e) => send('mouseup', e));
  const sendKey = (kind, e) => {
    fetch(`http://127.0.0.1:${PORT}/input`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ type: 'input', kind, key: e.key, code: e.code,
                             clientId: cid, color })
    }).catch((err) => { window.__vmRenderer.error = 'key: ' + err; });
  };
  document.addEventListener('keydown', (e) => {
    if (e.key.length === 1 || e.key === 'Backspace' || e.key === 'Enter') sendKey('keydown', e);
  });
  document.addEventListener('keyup', (e) => {
    if (e.key.length === 1 || e.key === 'Backspace' || e.key === 'Enter') sendKey('keyup', e);
  });

  window.__vmRenderer.ready = true;
  window.__vmRenderer.pixels = function () {
    const c = cv.getContext('2d');
    const at = (x, y) => Array.from(c.getImageData(x, y, 1, 1).data).join(',');
    return { titleA: at(100, 70), buttonA: at(100, 195), bg: at(5, 5) };
  };
  console.log('[VM_RENDER] ready port=' + PORT);
}

document.addEventListener('DOMContentLoaded', function () {
  boot().catch((err) => {
    window.__vmRenderer.error = String(err && err.stack ? err.stack : err);
    console.error('[VM_RENDER] boot failed: ' + window.__vmRenderer.error);
  });
  function draw() { requestAnimationFrame(draw); }
  draw();
});
