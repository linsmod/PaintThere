// VM 渲染页面唯一入口：浏览器（HTML + module）与 canvas-cpp C 宿主共用此文件。
// 逻辑只有一份；两端差异仅通过标准 location / globalThis 配置吸收。
import {
  initCanvas, commandHandlers, setLayerCanvas, replayBackgroundPrograms,
  applyFrameDiff, applyIncrementalFrame, setTextNeedsReplay
} from './app.js';
import { loadFont } from './font_wasm.js';

const PORT = globalThis.VM_RENDERER_PORT || 9000;
const WIDTH = 800;
const HEIGHT = 600;
// 浏览器：location.origin = http://host:port；canvas 宿主：file:/// → 用本机端口。
const BASE = (typeof location !== 'undefined' && location.protocol !== 'file:')
  ? location.origin
  : `http://127.0.0.1:${PORT}`;
const EVENTS_URL = `${BASE}/events`;
const INPUT_URL = `${BASE}/input`;

window.__vmRenderer = { ready: false, frames: 0, resources: 0, error: '' };

// drawText 异步光栅化：首帧文本未就绪，需在字体就绪后重放最近一帧。
let lastFrame = null;
let lastCanvas = null;
function replayLastFrame() {
  if (!lastFrame || !lastCanvas) return;
  const ctx = lastCanvas.getContext('2d');
  ctx.clearRect(0, 0, lastCanvas.width, lastCanvas.height);
  for (const c of lastFrame) {
    const fn = commandHandlers[c.cmd];
    if (fn) {
      try { fn(...c.args); }
      catch (e) { window.__vmRenderer.error = 'replay ' + c.cmd + ': ' + e; }
    }
  }
  replayBackgroundPrograms();
}
setTextNeedsReplay(replayLastFrame);

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

  const es = new EventSource(EVENTS_URL);
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
      lastFrame = f.commands; lastCanvas = cv;
      window.__vmRenderer.frames++;
    } catch (err) { window.__vmRenderer.error = 'fullframe: ' + err; }
  });
  es.onmessage = (e) => {
    try {
      const f = JSON.parse(e.data);
      if (f.incremental) {
        applyIncrementalFrame(f, cv);
      } else {
        applyFrameDiff(f.commands, cv);
        lastFrame = f.commands;
      }
      lastCanvas = cv;
      window.__vmRenderer.frames++;
    } catch (err) { window.__vmRenderer.error = 'frame: ' + err; }
  };
  es.onerror = () => { window.__vmRenderer.error = 'SSE disconnected'; };

  let cid = null;
  try { cid = sessionStorage.getItem('vmCid'); } catch (e) { /* 无存储环境 */ }
  if (!cid) {
    cid = Math.random().toString(36).slice(2);
    try { sessionStorage.setItem('vmCid', cid); } catch (e) { /* 忽略 */ }
  }
  const palette = ['#F44336', '#2196F3', '#4CAF50', '#FF9800', '#9C27B0', '#00BCD4'];
  let h = 0;
  for (let i = 0; i < cid.length; i++) h = (h * 31 + cid.charCodeAt(i)) | 0;
  const color = palette[Math.abs(h) % palette.length];

  const postInput = (payload) => {
    fetch(INPUT_URL, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    }).catch((err) => { window.__vmRenderer.error = 'input: ' + err; });
  };
  const sendPointer = (kind, e) => {
    const r = cv.getBoundingClientRect();
    const x = Math.round((e.clientX - r.left) / r.width * cv.width);
    const y = Math.round((e.clientY - r.top) / r.height * cv.height);
    postInput({ type: 'input', kind, x, y, button: e.button, clientId: cid, color });
  };
  cv.addEventListener('mousedown', (e) => sendPointer('mousedown', e));
  document.addEventListener('mousemove', (e) => sendPointer('mousemove', e));
  document.addEventListener('mouseup', (e) => sendPointer('mouseup', e));
  const sendKey = (kind, e) => {
    postInput({ type: 'input', kind, key: e.key, code: e.code, clientId: cid, color });
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
  console.log('[VM_RENDER] ready base=' + BASE);
}

document.addEventListener('DOMContentLoaded', function () {
  boot().catch((err) => {
    window.__vmRenderer.error = String(err && err.stack ? err.stack : err);
    console.error('[VM_RENDER] boot failed: ' + window.__vmRenderer.error);
  });
  function draw() { requestAnimationFrame(draw); }
  draw();
});
