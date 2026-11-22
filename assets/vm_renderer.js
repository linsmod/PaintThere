// VM 渲染页面唯一入口：浏览器（HTML + module）与 canvas-cpp C 宿主共用此文件。
// 逻辑只有一份；两端差异仅通过标准 location / globalThis 配置吸收。
import {
  initCanvas, commandHandlers, setLayerCanvas, replayBackgroundPrograms,
  setDrawTarget, setTextNeedsReplay
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

window.__vmRenderer = { ready: false, frames: 0, resources: 0, tiles: 0, loading: false, error: '' };

// 文本光栅化是异步的：首帧可能还没位图，就绪后请服务端重发一次全量合成。
function requestRedraw() {
  fetch('/redraw', { method: 'POST' }).catch(() => {});
}
setTextNeedsReplay(requestRedraw);

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

  // 样式全部由 JS 设置，HTML 只负责加载本模块。
  // 覆盖 app.js 写入的 1/1.5 CSS 缩放：两端都按 1:1 物理像素显示，便于对比。
  document.body.style.margin = '0px';
  stage.style.width = WIDTH + 'px';
  stage.style.height = HEIGHT + 'px';
  stage.style.position = 'relative';
  stage.style.margin = '0px';
  const setupLayer = (canvas) => {
    canvas.style.width = WIDTH + 'px';
    canvas.style.height = HEIGHT + 'px';
    canvas.style.position = 'absolute';
    canvas.style.left = '0px';
    canvas.style.top = '0px';
  };
  setupLayer(bg);
  setupLayer(cv);

  initCanvas(cv, '#FFFFFF', WIDTH, HEIGHT);
  setupLayer(bg);
  setupLayer(cv);
  cv.width = WIDTH; cv.height = HEIGHT;
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

  const cvCtx = cv.getContext('2d');

  /* Remote tiles：网格由 WM 指令下发，前端只管理远端小画布并按 index 贴齐。
     窗口、图层、z 序与脏区都不在协议里。 */
  const tileCanvases = [];
  let grid = null;

  const configTiles = (op) => {
    const total = op.cols * op.rows;
    if (grid && grid.cols === op.cols && grid.rows === op.rows &&
        grid.tileW === op.tileW && grid.tileH === op.tileH && tileCanvases.length === total) return;
    grid = { cols: op.cols, rows: op.rows, tileW: op.tileW, tileH: op.tileH, w: op.w, h: op.h };
    tileCanvases.length = 0;
    for (let i = 0; i < total; i++) {
      const tile = document.createElement('canvas');
      tile.width = op.tileW;
      tile.height = op.tileH;
      tileCanvases.push(tile);
    }
  };

  const present = () => {
    cvCtx.clearRect(0, 0, cv.width, cv.height);
    if (!grid) return;
    for (let i = 0; i < tileCanvases.length; i++) {
      const col = i % grid.cols;
      const row = Math.floor(i / grid.cols);
      cvCtx.drawImage(tileCanvases[i], col * grid.tileW, row * grid.tileH);
    }
    window.__vmRenderer.tiles = tileCanvases.length;
  };

  const setTile = (op) => {
    const tile = tileCanvases[op.index];
    if (!tile) return;
    const tctx = tile.getContext('2d');
    tctx.clearRect(0, 0, tile.width, tile.height);
    setDrawTarget(tctx);
    for (const c of (op.cmds || [])) {
      const fn = commandHandlers[c.cmd];
      if (fn) { try { fn(...c.args); } catch (e) { console.error('cmd failed:', c.cmd, e); } }
    }
  };

  const showLoading = () => {
    cvCtx.save();
    cvCtx.fillStyle = 'rgba(24,26,32,0.72)';
    cvCtx.fillRect(0, 0, cv.width, cv.height);
    cvCtx.restore();
    window.__vmRenderer.loading = true;
  };

  const hideLoading = () => {
    window.__vmRenderer.loading = false;
    present();
  };

  const execOps = (ops) => {
    let batched = false;
    for (const op of ops) {
      if (!op) continue;
      switch (op.op) {
        case 'config': configTiles(op); break;
        case 'setTile': setTile(op); break;
        case 'showLoading': showLoading(); batched = true; break;
        case 'hideLoading': hideLoading(); batched = true; break;
        default: {
          const fn = commandHandlers[op.cmd];
          if (fn) { try { fn(...op.args); } catch (e) { console.error('op failed:', op.op, e); } }
        }
      }
    }
    if (!batched) present();
  };

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
      if (Array.isArray(f.ops)) execOps(f.ops);
      window.__vmRenderer.frames++;
    } catch (err) { window.__vmRenderer.error = 'fullframe: ' + err; }
  });
  es.onmessage = (e) => {
    try {
      const f = JSON.parse(e.data);
      if (Array.isArray(f.ops)) {
        execOps(f.ops);
        window.__vmRenderer.frames++;
      }
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

  // 上行输入：优先 WebSocket（无 HTTP 头、无新连接、无请求排队），失败回退 HTTP POST。
  const WS_URL = `${BASE.replace(/^http/, 'ws')}/ws`;
  let inputWs = null;
  const connectInput = () => {
    let ws;
    try { ws = new WebSocket(WS_URL); } catch (e) { inputWs = null; return; }
    inputWs = ws;
    ws.onopen = () => { window.__vmRenderer.input = 'ws'; };
    ws.onclose = () => {
      if (inputWs === ws) inputWs = null;
      window.__vmRenderer.input = 'http';
      setTimeout(connectInput, 1000);
    };
  };
  const postInput = (payload) => {
    if (inputWs && inputWs.readyState === WebSocket.OPEN) {
      inputWs.send(JSON.stringify(payload));
      return;
    }
    fetch(INPUT_URL, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    }).catch((err) => { window.__vmRenderer.error = 'input: ' + err; });
  };
  connectInput();
  const toCanvasPoint = (e) => {
    const r = cv.getBoundingClientRect();
    const x = Math.round((e.clientX - r.left) / r.width * cv.width);
    const y = Math.round((e.clientY - r.top) / r.height * cv.height);
    return {
      x: Math.min(cv.width - 1, Math.max(0, x)),
      y: Math.min(cv.height - 1, Math.max(0, y))
    };
  };
  const insideCanvas = (e) => {
    const r = cv.getBoundingClientRect();
    return e.clientX >= r.left && e.clientX < r.right &&
           e.clientY >= r.top && e.clientY < r.bottom;
  };
  const sendPointer = (kind, e) => {
    const p = toCanvasPoint(e);
    postInput({ type: 'input', kind, x: p.x, y: p.y, button: e.button, clientId: cid, color });
  };
  // mousemove 节流：鼠标事件密度远高于服务端出帧节奏（20ms 一轮）。
  // 走 WS 后每帧只是一次 send，但仍要合并，避免把服务端打成每事件一帧。
  const MOVE_INTERVAL_MS = 16;
  let moveTimer = 0;
  let pendingMove = null;
  const flushMove = () => {
    moveTimer = 0;
    const p = pendingMove;
    pendingMove = null;
    if (!p) return;
    postInput({ type: 'input', kind: 'mousemove', x: p.x, y: p.y, button: p.button, clientId: cid, color });
  };
  document.addEventListener('mousemove', (e) => {
    // 指针移出画布就不再上报：越界坐标会把服务端的 hover 状态清掉，
    // 远端指针停在最后位置即可。
    if (!insideCanvas(e)) {
      pendingMove = null;
      if (moveTimer) {
        clearTimeout(moveTimer);
        moveTimer = 0;
      }
      return;
    }
    const p = toCanvasPoint(e);
    pendingMove = { x: p.x, y: p.y, button: e.button };
    if (moveTimer) return;
    moveTimer = setTimeout(flushMove, MOVE_INTERVAL_MS);
  });

  // 按下/抬起立即发送，并丢弃尚未发出的移动：否则节流定时器会在松手之后
  // 把指针位置写回旧坐标。抬起必须发出（坐标钳在画布内），否则拖动状态会卡住。
  const sendImmediate = (kind, e) => {
    pendingMove = null;
    if (moveTimer) {
      clearTimeout(moveTimer);
      moveTimer = 0;
    }
    sendPointer(kind, e);
  };
  cv.addEventListener('mousedown', (e) => sendImmediate('mousedown', e));
  document.addEventListener('mouseup', (e) => sendImmediate('mouseup', e));
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
