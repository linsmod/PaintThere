// VM 渲染页面唯一入口：浏览器（HTML + module）与 canvas-cpp C 宿主共用此文件。
// 逻辑只有一份；两端差异仅通过标准 location / globalThis 配置吸收。
import {
  initCanvas, commandHandlers, setLayerCanvas, replayBackgroundPrograms,
  setDrawTarget, setTextNeedsReplay, setTextScale, setResPackApplied
} from './app.js';
import { loadFont, rasterizeText, drawRasterScaled } from './font_wasm.js';

const PORT = globalThis.VM_RENDERER_PORT || 9000;
const WIDTH = 800;
const HEIGHT = 600;
// 浏览器：location.origin = http://host:port；canvas 宿主：file:/// → 用本机端口。
const BASE = (typeof location !== 'undefined' && location.protocol !== 'file:')
  ? location.origin
  : `http://127.0.0.1:${PORT}`;
const EVENTS_URL = `${BASE}/events`;
const INPUT_URL = `${BASE}/input`;

window.__vmRenderer = { ready: false, frames: 0, resources: 0, tiles: 0, loading: false, disconnected: false, error: '' };

// 字体位图与图片解码是异步的：就绪后由本地重放受影响的 tile，不惊动服务端。
// boot() 建立 tile 状态后接入真正的重放函数；此前的就绪通知由首次重放兜底。
let replaySink = null;
setTextNeedsReplay(() => { if (replaySink) replaySink(); });

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
  // 物理画布保持 800×600（tile 网格与协议坐标不变）；CSS 等比缩放填满视口，
  // 页面背景用桌面同色渐变，避免窗口变大后露出白边。
  const applyPageStyle = () => {
    document.documentElement.style.height = '100%';
    document.body.style.height = '100%';
    document.body.style.margin = '0px';
    document.body.style.overflow = 'hidden';
    /* 画布外的留边跟随桌面配色：深色主题下不像白边，XP 主题下像桌面延伸。 */
    document.body.style.background =
      'linear-gradient(180deg, #1e6fd9 0%, #6fa8e8 100%)';
    document.body.style.display = 'flex';
    document.body.style.alignItems = 'center';
    document.body.style.justifyContent = 'center';
  };
  applyPageStyle();

  const setupLayer = (canvas) => {
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
     窗口、图层、z 序与脏区都不在协议里。

     分辨率：tile 的物理尺寸 = 逻辑尺寸 × renderScale（DPR × 显示缩放）。
     绘制时用 transform 把逻辑坐标映射到物理像素，贴屏时 1:1 拷贝、不重采样。 */
  const tileCanvases = [];
  const tileCmds = [];
  let grid = null;
  let renderScale = 1;
  let cssZoom = 1;
  let lastFrameId = 0;   // 上行事件带上「客户端所见的最新帧号」
  const MAX_RENDER_SCALE = 2.5;

  const makeTileCanvases = () => {
    tileCanvases.length = 0;
    if (!grid) return;
    const total = grid.cols * grid.rows;
    for (let i = 0; i < total; i++) {
      const tile = document.createElement('canvas');
      tile.width = Math.max(1, Math.round(grid.tileW * renderScale));
      tile.height = Math.max(1, Math.round(grid.tileH * renderScale));
      tileCanvases.push(tile);
    }
  };

  const configTiles = (op) => {
    const same = grid && grid.cols === op.cols && grid.rows === op.rows &&
                 grid.tileW === op.tileW && grid.tileH === op.tileH;
    if (same) return;
    grid = { cols: op.cols, rows: op.rows, tileW: op.tileW, tileH: op.tileH, w: op.w, h: op.h };
    tileCmds.length = 0;
    makeTileCanvases();
  };

  const present = () => {
    cvCtx.setTransform(1, 0, 0, 1, 0, 0);
    cvCtx.clearRect(0, 0, cv.width, cv.height);
    if (!grid) {
      if (showDisconnectOverlay) drawDisconnectOverlay();
      return;
    }
    /* 瓦片定位必须用「前一块的实际物理宽度累加」：renderScale 带小数时
       （窗口尺寸不是 800 的整数倍 → cssZoom 小数），round(k * tileW * scale)
       与逐块 round 出来的瓦片宽度会差出 1px，瓦片之间留下未覆盖的透明缝，
       透出页面底色，表现为任务栏/桌面上随窗口尺寸移动的竖线。
       累加定位保证相邻瓦片像素级贴合：无缝隙、无重叠。 */
    let px = 0;
    let py = 0;
    for (let i = 0; i < tileCanvases.length; i++) {
      const tile = tileCanvases[i];
      if (i > 0) {
        if (i % grid.cols === 0) {
          px = 0;
          py += tileCanvases[i - grid.cols].height;
        } else {
          px += tileCanvases[i - 1].width;
        }
      }
      cvCtx.drawImage(tile, px, py);
    }
    window.__vmRenderer.tiles = tileCanvases.length;
    if (showDisconnectOverlay) drawDisconnectOverlay();
  };

  const drawTile = (index, cmds) => {
    const tile = tileCanvases[index];
    if (!tile) return;
    const tctx = tile.getContext('2d');
    /* 清空必须在单位变换下做，否则只清到「逻辑尺寸」那么大。 */
    tctx.setTransform(1, 0, 0, 1, 0, 0);
    tctx.clearRect(0, 0, tile.width, tile.height);
    const s = tile.width / grid.tileW;   // 取整后的实际比例
    tctx.setTransform(s, 0, 0, s, 0, 0);
    setDrawTarget(tctx);
    for (const c of cmds) {
      const fn = commandHandlers[c.cmd];
      if (fn) { try { fn(...c.args); } catch (e) { console.error('cmd failed:', c.cmd, e); } }
    }
  };

  const setTile = (op) => {
    tileCmds[op.index] = op.cmds || [];
    drawTile(op.index, tileCmds[op.index]);
  };

  // 多个资源同时就绪时合并为一次重放；批次未结束时不提前贴屏。
  let replayQueued = false;
  const replayTiles = () => {
    if (!grid) return;
    for (let i = 0; i < tileCanvases.length; i++) {
      if (tileCmds[i]) drawTile(i, tileCmds[i]);
    }
    if (!window.__vmRenderer.loading) present();
  };
  const scheduleReplay = () => {
    if (replayQueued) return;
    replayQueued = true;
    requestAnimationFrame(() => {
      replayQueued = false;
      /* 文字位图/图片是异步就绪的：bg 层绘制程序里的 drawText（如编译时间
         水印）同样依赖重放才能补画，只重放 tile 会漏掉它们。 */
      replayBackgroundPrograms();
      replayTiles();
    });
  };
  replaySink = scheduleReplay;
  /* 资源包拉取完成（异步）后按新 pattern/style 缓存重放所有 tile。 */
  setResPackApplied(scheduleReplay);

  /* 显示缩放（CSS）与物理分辨率（DPR）分开算：
     CSS 尺寸决定占多大地方，renderScale 决定画多少像素。
     两者脱钩后，放大显示或高分屏都不会再拿 800×600 的位图去插值。 */
  const resizeRenderTargets = () => {
    cv.width = Math.max(1, Math.round(WIDTH * renderScale));
    cv.height = Math.max(1, Math.round(HEIGHT * renderScale));
    bg.width = cv.width;
    bg.height = cv.height;
    const bctx = bg.getContext('2d');
    bctx.setTransform(renderScale, 0, 0, renderScale, 0, 0);
    replayBackgroundPrograms();
    setTextScale(renderScale);
    if (grid) {
      makeTileCanvases();
      replayTiles();
    }
    present();
  };

  const layoutStage = () => {
    /* 原生 canvas 宿主没有 innerWidth/devicePixelRatio，兜底成 1 倍。 */
    const vw = Number(window.innerWidth) || WIDTH;
    const vh = Number(window.innerHeight) || HEIGHT;
    /* 逻辑画布最大按 1:1 显示：再大就只是把同样的内容抻开，不如提高物理分辨率。 */
    cssZoom = Math.min(vw / WIDTH, vh / HEIGHT, 1) || 1;
    const cssW = Math.round(WIDTH * cssZoom);
    const cssH = Math.round(HEIGHT * cssZoom);
    stage.style.width = cssW + 'px';
    stage.style.height = cssH + 'px';
    stage.style.position = 'relative';
    stage.style.flex = '0 0 auto';
    stage.style.overflow = 'hidden';
    /* 桌面是屏幕本身：不做卡片圆角与投影，避免画布像浮在页面上的演示框。 */
    stage.style.borderRadius = '0px';
    stage.style.boxShadow = 'none';
    for (const layer of [bg, cv]) {
      layer.style.width = cssW + 'px';
      layer.style.height = cssH + 'px';
    }
    const dpr = Number(window.devicePixelRatio) || 1;
    const nextScale = Math.min(Math.max(cssZoom * dpr, 1), MAX_RENDER_SCALE);
    if (Math.abs(nextScale - renderScale) > 0.001) {
      renderScale = nextScale;
      resizeRenderTargets();
    } else {
      present();
    }
  };

  /* 断线覆盖层：SSE 短暂抖动不闪屏，断开持续超过阈值才显示。
     覆盖层画在 tile 合成层之上，bg/tiles 保持最后一帧冻结；
     恢复后服务端先发 fullframe，覆盖层随下一次 present() 自动消失。 */
  const DISCONNECT_DELAY_MS = 1500;
  let disconnectTimer = 0;
  let showDisconnectOverlay = false;
  let disconnectTextPending = false;

  const drawDisconnectOverlay = () => {
    cvCtx.setTransform(1, 0, 0, 1, 0, 0);
    cvCtx.fillStyle = 'rgba(24,26,32,0.78)';
    cvCtx.fillRect(0, 0, cv.width, cv.height);
    /* 文字异步光栅化：就绪后若覆盖层仍在，按物理像素 1:1 画在正中。 */
    if (disconnectTextPending) return;
    disconnectTextPending = true;
    const px = Math.max(1, Math.round(15 * renderScale));
    rasterizeText(0, px, '连接已断开').then((r) => {
      disconnectTextPending = false;
      if (!showDisconnectOverlay) return;
      drawRasterScaled(cvCtx, r, Math.round((cv.width - r.w) / 2),
                       Math.round((cv.height - r.h) / 2), r.w, r.h, '#E8E8E8');
    }).catch((err) => {
      disconnectTextPending = false;
      console.error('disconnect text rasterize failed:', err);
    });
  };

  const markConnected = () => {
    if (disconnectTimer) {
      clearTimeout(disconnectTimer);
      disconnectTimer = 0;
    }
    if (showDisconnectOverlay) {
      showDisconnectOverlay = false;
      window.__vmRenderer.disconnected = false;
      present();
    }
  };

  layoutStage();
  window.addEventListener('resize', layoutStage);
  scheduleReplay();

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

  let reconnectTimer = 0;
  let resourcesReady = Promise.resolve();
  let frameChain = Promise.resolve();
  const connectEvents = () => {
    clearTimeout(reconnectTimer);
    window.__vmRenderer.error = '';
    resourcesReady = Promise.resolve();
    frameChain = Promise.resolve();
    const source = new EventSource(EVENTS_URL);
    let rejected = false;

    function applyResources(e) {
      const list = JSON.parse(e.data);
      let pending = Promise.resolve();
      for (const c of list) {
        const fn = commandHandlers[c.cmd];
        if (fn) {
          const r = fn(...c.args);
          // loadResPack 是异步的：帧必须等 defineStyle 进表后才能应用。
          if (r && typeof r.then === 'function') pending = pending.then(() => r);
        }
      }
      replayBackgroundPrograms();
      window.__vmRenderer.resources += list.length;
      return pending;
    }
    // 帧按到达顺序排队，且每一帧都等当前资源（含 respack）就绪。
    const queueFrame = (data, kind) => {
      const ready = resourcesReady;
      frameChain = frameChain.then(() => ready).then(() => {
        const f = JSON.parse(data);
        if (typeof f.frameId === 'number') lastFrameId = f.frameId;
        if (Array.isArray(f.ops)) {
          execOps(f.ops);
          window.__vmRenderer.frames++;
        }
      }).catch((err) => { window.__vmRenderer.error = kind + ': ' + err; });
    };
    source.addEventListener('resources', (e) => {
      markConnected();
      resourcesReady = Promise.resolve().then(() => applyResources(e))
        .catch((err) => { window.__vmRenderer.error = 'resource: ' + err; });
    });
    /* 主题热切换：覆盖 pattern/style 缓存；随后服务端会因窗口置脏推新帧，
       新帧按新颜色绘制。definePattern 等命令按 id 覆盖，无需重连。 */
    source.addEventListener('resources-update', (e) => {
      resourcesReady = resourcesReady.then(() => applyResources(e))
        .catch((err) => { window.__vmRenderer.error = 'resources-update: ' + err; });
    });
    source.addEventListener('fullframe', (e) => {
      markConnected();
      queueFrame(e.data, 'fullframe');
    });
    source.addEventListener('control', (e) => {
      try {
        const c = JSON.parse(e.data);
        if (c.op !== 'serverFull') return;
        rejected = true;
        source.close();
        window.__vmRenderer.error = 'serverFull';
        reconnectTimer = setTimeout(connectEvents, Math.max(500, c.retryMs || 2000));
      } catch (err) { /* 控制事件解析失败时交给 EventSource 自身重连 */ }
    });
    source.onmessage = (e) => {
      markConnected();
      queueFrame(e.data, 'frame');
    };
    source.onerror = () => {
      if (rejected) return;
      window.__vmRenderer.error = 'SSE disconnected';
      if (disconnectTimer || showDisconnectOverlay) return;
      disconnectTimer = setTimeout(() => {
        disconnectTimer = 0;
        showDisconnectOverlay = true;
        window.__vmRenderer.disconnected = true;
        present();
      }, DISCONNECT_DELAY_MS);
    };
  };
  connectEvents();

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
    /* 画布物理尺寸随 DPR 变化，但协议坐标永远是 800×600 逻辑坐标：
       这里必须按 WIDTH/HEIGHT 换算，用 cv.width 会把坐标放大 renderScale 倍。 */
    const x = Math.round((e.clientX - r.left) / r.width * WIDTH);
    const y = Math.round((e.clientY - r.top) / r.height * HEIGHT);
    return {
      x: Math.min(WIDTH - 1, Math.max(0, x)),
      y: Math.min(HEIGHT - 1, Math.max(0, y))
    };
  };
  const insideCanvas = (e) => {
    const r = cv.getBoundingClientRect();
    return e.clientX >= r.left && e.clientX < r.right &&
           e.clientY >= r.top && e.clientY < r.bottom;
  };
  // 客户端所见的最新帧号：上行事件带上它，guest 可判断「所见即所操作」。
  const sendPointer = (kind, e) => {
    const p = toCanvasPoint(e);
    postInput({ type: 'input', kind, x: p.x, y: p.y, button: e.button,
                clientId: cid, color, frameId: lastFrameId });
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
    postInput({ type: 'input', kind: 'mousemove', x: p.x, y: p.y, button: p.button,
                clientId: cid, color, frameId: lastFrameId });
  };
  document.addEventListener('mousemove', (e) => {
    // 未按下时移出画布就不再上报：越界坐标会把服务端的 hover 状态清掉，
    // 远端指针停在最后位置即可。
    // 按下（拖动）时移出画布继续上报：toCanvasPoint 已钳位到边界，
    // guest 侧对应轴停在 0 / WIDTH-1，拖动不会中途断掉。
    if (!insideCanvas(e) && e.buttons === 0) {
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
  // 右键是 guest 的上下文菜单输入：压掉浏览器原生菜单，让 mousedown(button=2) 落到画布。
  cv.addEventListener('contextmenu', (e) => e.preventDefault());

  // 滚轮：只上报原始增量，滚动语义全在 guest。
  cv.addEventListener('wheel', (e) => {
    e.preventDefault();
    const p = toCanvasPoint(e);
    postInput({ type: 'input', kind: 'wheel', x: p.x, y: p.y, deltaY: e.deltaY,
                clientId: cid, color, frameId: lastFrameId });
  }, { passive: false });

  // 可上报的命名键：控件焦点链与文本编辑依赖这些键。
  const NAMED_KEYS = new Set([
    'Backspace', 'Enter', 'Tab', 'Escape', 'Delete',
    'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight',
    'Home', 'End', 'PageUp', 'PageDown'
  ]);
  const sendKey = (kind, e) => {
    postInput({ type: 'input', kind, key: e.key, code: e.code,
                ctrl: e.ctrlKey ? 1 : 0, shift: e.shiftKey ? 1 : 0,
                alt: e.altKey ? 1 : 0, meta: e.metaKey ? 1 : 0,
                clientId: cid, color, frameId: lastFrameId });
  };
  const isReportableKey = (e) => e.key.length === 1 || NAMED_KEYS.has(e.key);
  document.addEventListener('keydown', (e) => {
    if (!isReportableKey(e)) return;
    e.preventDefault();   // Tab/方向键不能把焦点移出画布
    sendKey('keydown', e);
  });
  document.addEventListener('keyup', (e) => {
    if (!isReportableKey(e)) return;
    sendKey('keyup', e);
  });

  window.__vmRenderer.ready = true;
  window.__vmRenderer.pixels = function () {
    const overlay = cv.getContext('2d');
    const bgCtx = bg.getContext('2d');
    const r = cv.width / WIDTH;   // 逻辑坐标 → 物理像素
    const at = (ctx, x, y) => Array.from(
      ctx.getImageData(Math.round(x * r), Math.round(y * r), 1, 1).data).join(',');
    // bg 保留旧含义：叠加层在 (5,5) 的像素；desktop 才是背景画布。
    return {
      titleA: at(overlay, 100, 70),
      buttonA: at(overlay, 100, 195),
      bg: at(overlay, 5, 5),
      desktop: at(bgCtx, 5, 5)
    };
  };
  /* 诊断：当前物理像素比，便于确认清晰度改动是否生效。 */
  window.__vmRenderer.scale = () => ({ cssZoom, renderScale, dpr: Number(window.devicePixelRatio) || 1 });
  console.log('[VM_RENDER] ready base=' + BASE);
}

document.addEventListener('DOMContentLoaded', function () {
  boot().catch((err) => {
    window.__vmRenderer.error = String(err && err.stack ? err.stack : err);
    console.error('[VM_RENDER] boot failed: ' + window.__vmRenderer.error);
  });
});
