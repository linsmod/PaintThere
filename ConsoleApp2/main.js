import { initCanvas, commandHandlers, loadImg, drawImg, setDrawImgTarget } from './app.js';

// eval 兼容路径需要这些函数在当前作用域可见（旧协议字符串指令用）
const { setBounds, drawBackground, drawLabel, drawButton, drawCheckBox,
    drawLinkLabel, drawEnhancedTextBox, drawControl } = commandHandlers;

// 全局变量
let canvas = null;
let ctx = null;
let pendingFrame = null;     // 只保留最新帧，旧帧直接丢弃
let lastCommands = [];       // 上一帧指令，用于增量 diff
let lastFrameSignature = '';
let cursorCanvas = null;
let cursorCtx = null;
let myClientId = null;
let myColor = null;
let cursorStates = [];
let lastCursorSend = 0;
const DEBOUNCE_INTERVAL = 1000 / 30; // 事件回传防抖间隔（毫秒）
let ws = null; // WebSocket 连接
let animationFrameId = 0
let lastDisconnectTime = 0;
let isDisconnected = true
let winFormId = null;
let _lastUpdateTime = performance.now();
// 初始化 Canvas
let isUpdating = false;

function beginUpdate(id) {
    winFormId = id;
    isUpdating = true;
    console.log('begin update.')
}
function endUpdate(id) {
    winFormId = id;
    isUpdating = false;
    console.log('end update.')
}

// 结构化指令直接调用函数；字符串指令保留 eval 以兼容旧服务端
const handlers = { ...commandHandlers };
handlers['#beginUpdate'] = beginUpdate;
handlers['#endUpdate'] = endUpdate;

function executeCommand(command, structured) {
    if (structured) {
        const fn = handlers[command.cmd];
        if (fn) {
            try { fn(...command.args); }
            catch (e) { console.error('command failed:', command.cmd, e); }
        }
    } else {
        const code = command.startsWith('#') ? command.substring(1) : command;
        try { eval(code); }
        catch (e) { console.error('eval failed:', code, e); }
    }
}

function initApp() {
    canvas = document.getElementById('formCanvas');
    ctx = initCanvas(canvas, '#FFFFFF', 1024, 768);
    cursorCanvas = document.getElementById('cursorCanvas');
    cursorCanvas.width = canvas.width;
    cursorCanvas.height = canvas.height;
    cursorCanvas.style.width = canvas.style.width;
    cursorCanvas.style.height = canvas.style.height;
    cursorCtx = cursorCanvas.getContext('2d');
    setDrawImgTarget(cursorCtx);
    startWebSocket();
    startRendering();
    setupEventListeners();
    setupCursorReporting();
}
import { loadingSplash } from "./loadingSplash.js"


// 设置事件监听器
function setupEventListeners() {
    // 鼠标事件
    canvas.addEventListener('click', handleEvent);
    canvas.addEventListener('dblclick', handleEvent);
    canvas.addEventListener('mousedown', handleEvent);
    canvas.addEventListener('mouseup', handleEvent);
    canvas.addEventListener('mousemove', handleEvent);
    canvas.addEventListener('mouseenter', handleEvent);
    canvas.addEventListener('mouseleave', handleEvent);
    canvas.addEventListener('mouseover', handleEvent);
    canvas.addEventListener('mouseout', handleEvent);
    canvas.addEventListener('contextmenu', handleEvent);
    canvas.addEventListener('wheel', handleEvent);

    // 键盘事件
    document.addEventListener('keydown', handleEvent, false);
    document.addEventListener('keyup', handleEvent, false);
    document.addEventListener('keypress', handleEvent, false);

    // 触摸事件
    canvas.addEventListener('touchstart', handleEvent);
    canvas.addEventListener('touchmove', handleEvent);
    canvas.addEventListener('touchend', handleEvent);
    canvas.addEventListener('touchcancel', handleEvent);

    // 表单事件
    canvas.addEventListener('change', handleEvent);
    canvas.addEventListener('input', handleEvent);
    canvas.addEventListener('submit', handleEvent);
    canvas.addEventListener('reset', handleEvent);
    canvas.addEventListener('focus', handleEvent);
    canvas.addEventListener('blur', handleEvent);
    canvas.addEventListener('select', handleEvent);
}
let lastInteractionTimes = {}; // 按事件类型分别记录

function handleEvent(e) {
    const now = Date.now();
    const lastTime = lastInteractionTimes[e.type] || 0;

    // 按事件类型分别防抖
    if (now - lastTime < DEBOUNCE_INTERVAL) {
        //console.log(`跳过 ${e.type} 事件，防抖中`);
        return;
    }

    lastInteractionTimes[e.type] = now;
    //console.log(`处理 ${e.type} 事件`);
    if (isUpdating)
        return;
    const eventData = createDomEvent(e);
    eventData.winFormId = winFormId;
    sendEventToServer(eventData);
}

// 创建符合Web标准的DOM事件对象
function createDomEvent(e) {
    const event = {
        type: e.type,
        bubbles: e.bubbles,
        cancelable: e.cancelable,
        composed: e.composed,
        timeStamp: new Date().toISOString(),
        target: extractEventTarget(e.target),
        currentTarget: extractEventTarget(e.currentTarget),
        defaultPrevented: e.defaultPrevented
    };

    // 添加特定事件类型的属性
    if (e instanceof UIEvent) {
        event.view = {
            innerWidth: window.innerWidth,
            innerHeight: window.innerHeight,
            scrollX: window.scrollX,
            scrollY: window.scrollY
        };
        event.detail = e.detail;
    }

    if (e instanceof MouseEvent) {
        event.screenX = e.screenX;
        event.screenY = e.screenY;
        event.clientX = e.clientX;
        event.clientY = e.clientY;
        event.pageX = e.pageX;
        event.pageY = e.pageY;
        event.button = e.button;
        event.buttons = e.buttons;
        event.ctrlKey = e.ctrlKey;
        event.shiftKey = e.shiftKey;
        event.altKey = e.altKey;
        event.metaKey = e.metaKey;
        event.relatedTarget = extractEventTarget(e.relatedTarget);
    }

    if (e instanceof KeyboardEvent) {
        event.key = e.key;
        event.code = e.code;
        event.keyCode = e.keyCode;
        event.charCode = e.charCode;
        event.location = e.location;
        event.repeat = e.repeat;
        event.isComposing = e.isComposing;
    }

    if (e instanceof WheelEvent) {
        event.deltaX = e.deltaX;
        event.deltaY = e.deltaY;
        event.deltaZ = e.deltaZ;
        event.deltaMode = e.deltaMode;
    }

    if (e instanceof TouchEvent) {
        event.touches = Array.from(e.touches).map(t => createTouchPoint(t));
        event.changedTouches = Array.from(e.changedTouches).map(t => createTouchPoint(t));
        event.targetTouches = Array.from(e.targetTouches).map(t => createTouchPoint(t));
    }

    // 添加画布相关信息
    event.canvasInfo = {
        width: canvas.width,
        height: canvas.height,
        position: getCanvasPosition()
    };

    return event;
}

// 创建触摸点对象
function createTouchPoint(touch) {
    return {
        identifier: touch.identifier,
        target: extractEventTarget(touch.target),
        screenX: touch.screenX,
        screenY: touch.screenY,
        clientX: touch.clientX,
        clientY: touch.clientY,
        pageX: touch.pageX,
        pageY: touch.pageY
    };
}

// 提取事件目标对象
function extractEventTarget(target) {
    if (!target) return null;

    return {
        tagName: target.tagName,
        id: target.id,
        className: target.className,
        value: target.value,
        checked: target.checked,
        disabled: target.disabled,
        type: target.type,
        name: target.name,
        innerText: target.innerText,
        attributes: target.attributes ? Array.from(target.attributes).reduce((acc, attr) => {
            acc[attr.name] = attr.value;
            return acc;
        }, {}) : {}
    };
}

// 获取画布位置信息
function getCanvasPosition() {
    const rect = canvas.getBoundingClientRect();
    return {
        left: rect.left,
        top: rect.top,
        right: rect.right,
        bottom: rect.bottom,
        width: rect.width,
        height: rect.height
    };
}

// 发送事件到服务端
function sendEventToServer(eventData) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        console.warn('WebSocket not connected, cannot send event');
        return;
    }

    try {
        ws.send(JSON.stringify(eventData));
    } catch (error) {
        console.error('Failed to send event:', error);
    }
}

// 启动 WebSocket 连接
function startWebSocket() {
    ws = new WebSocket('ws://localhost:8081/');

    ws.onopen = () => {
        isDisconnected = false
        console.log('Connected to WebSocket server');
    };

    ws.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            if (Array.isArray(data)) {
                // 只保存最新帧，渲染交给 requestAnimationFrame 统一处理
                pendingFrame = data;
            } else if (data && data.type === 'welcome') {
                myClientId = data.clientId;
                myColor = data.color;
                loadImg('cursor', data.cursorImage);
                console.log('welcome client', myClientId, myColor);
            } else if (data && data.type === 'cursors') {
                cursorStates = Array.isArray(data.list) ? data.list : [];
            }
        } catch (error) {
            console.error('Failed to parse message:', error);
        }
    };

    ws.onclose = () => {
        cursorStates = [];
        myClientId = null;
        if (!isDisconnected) {
            lastDisconnectTime = Date.now();
            isDisconnected = true;
        }
        console.log('Disconnected from WebSocket server');
        setTimeout(startWebSocket, 5000); // 5秒后重连
    };

    ws.onerror = (error) => {
        console.error('WebSocket error:', error);
    };
}

// 增量渲染：对比上一帧，只重画变化控件的脏矩形区域
function argsEqual(a, b) {
    if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
        if (a[i] !== b[i]) return false;
    }
    return true;
}

function unionRects(rects) {
    if (!rects.length) return null;
    let x1 = Infinity, y1 = Infinity, x2 = -Infinity, y2 = -Infinity;
    for (const r of rects) {
        if (!r || r.length < 4) continue;
        x1 = Math.min(x1, r[0]);
        y1 = Math.min(y1, r[1]);
        x2 = Math.max(x2, r[0] + r[2]);
        y2 = Math.max(y2, r[1] + r[3]);
    }
    if (x1 === Infinity) return null;
    x1 = Math.max(0, Math.floor(x1));
    y1 = Math.max(0, Math.floor(y1));
    x2 = Math.min(canvas.width, Math.ceil(x2));
    y2 = Math.min(canvas.height, Math.ceil(y2));
    return [x1, y1, x2 - x1, y2 - y1];
}

function applyFrame() {
    const frame = pendingFrame;
    if (!frame) return;
    pendingFrame = null;

    const structured = frame.length > 0 && typeof frame[0] === 'object' && frame[0] !== null;
    const signature = structured
        ? frame.map(c => (c.id || '') + ':' + c.cmd + ':' + JSON.stringify(c.args)).join('|')
        : frame.join('\n');
    if (signature === lastFrameSignature) return; // 双保险：服务端已做值去重
    lastFrameSignature = signature;

    if (!structured) {
        // 旧协议：全量清空 + eval
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        for (const command of frame) executeCommand(command, false);
        return;
    }

    // 结构化协议：收集变化的脏矩形
    const dirty = [];
    const oldById = new Map();
    for (const c of lastCommands) if (c.id) oldById.set(c.id, c);
    const newIds = new Set();

    for (const c of frame) {
        if (c.id) {
            newIds.add(c.id);
            const old = oldById.get(c.id);
            if (!old || old.cmd !== c.cmd || !argsEqual(old.args, c.args)) {
                if (c.rect) dirty.push(c.rect);
                if (old && old.rect) dirty.push(old.rect); // 文本变短时旧区域也要擦除
            }
        } else if (c.cmd === 'setBounds' || c.cmd === 'drawBackground') {
            dirty.push([0, 0, canvas.width, canvas.height]);
        }
        // #beginUpdate / #endUpdate 不影响画面
    }
    for (const [id, old] of oldById) {
        if (!newIds.has(id) && old.rect) dirty.push(old.rect); // 控件消失
    }

    const region = unionRects(dirty);
    if (region) {
        ctx.save();
        ctx.beginPath();
        ctx.rect(region[0], region[1], region[2], region[3]);
        ctx.clip();
        ctx.clearRect(region[0], region[1], region[2], region[3]);
        for (const c of frame) executeCommand(c, true);
        ctx.restore();
    }
    lastCommands = frame;
}

// 光标上报独立于 DOM 事件回传，不受 isUpdating 影响
function setupCursorReporting() {
    document.addEventListener('mousemove', (e) => {
        if (!ws || ws.readyState !== WebSocket.OPEN || !myClientId) return;
        const rect = canvas.getBoundingClientRect();
        if (e.clientX < rect.left || e.clientX > rect.right ||
            e.clientY < rect.top || e.clientY > rect.bottom) return;
        const now = performance.now();
        if (now - lastCursorSend < 33) return;
        lastCursorSend = now;
        const x = (e.clientX - rect.left) / rect.width * canvas.width;
        const y = (e.clientY - rect.top) / rect.height * canvas.height;
        ws.send(JSON.stringify({ type: 'cursor', clientId: myClientId, x, y, visible: true }));
    });
    canvas.addEventListener('mouseleave', () => {
        if (!ws || ws.readyState !== WebSocket.OPEN || !myClientId) return;
        ws.send(JSON.stringify({ type: 'cursor', clientId: myClientId, visible: false }));
    });
}

// 指针层独立绘制：不进入 applyFrame 的签名与脏矩形流程
function renderCursors() {
    if (!cursorCtx) return;
    cursorCtx.clearRect(0, 0, cursorCanvas.width, cursorCanvas.height);
    for (const c of cursorStates) {
        if (!c.visible || c.clientId === myClientId) continue;
        drawImg('cursor', c.x, c.y, 24, 24, c.color);
    }
}

import { drawDisconnectOverlay } from "./connection.js"
// 启动渲染循环：渲染完全跟随 requestAnimationFrame，不再做二次 30fps 节流
function startRendering() {
    function renderLoop() {
        if (isDisconnected) {
            pendingFrame = null;
            loadingSplash.update(canvas, ctx);
            //drawDisconnectOverlay(ctx, canvas);

            //// 显示断开时长
            //const disconnectDuration = Math.floor((Date.now() - lastDisconnectTime) / 1000);
            //ctx.fillStyle = 'rgba(255, 255, 255, 0.7)';
            //ctx.font = '16px Arial';
            //ctx.textAlign = 'center';
            //ctx.fillText(`已断开 ${disconnectDuration}秒`, canvas.width / 2, canvas.height / 2 + 70);
        }
        else {
            applyFrame();
        }
        renderCursors();
        animationFrameId = requestAnimationFrame(renderLoop);
    }
    renderLoop();
}

// 初始化应用
window.onload = initApp;