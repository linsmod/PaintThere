import { initCanvas, setBounds, drawBackground, drawLabel, drawButton, drawCheckBox, drawLinkLabel, drawEnhancedTextBox, drawControl } from './app.js';

// 全局变量
let canvas = null;
let ctx = null;
let paint_commands = null;
const DEBOUNCE_INTERVAL = 1000 / 30; // 防抖间隔（毫秒）
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

function initApp() {
    canvas = document.getElementById('formCanvas');
    ctx = initCanvas(canvas, '#FFFFFF', 1024, 768);
    startWebSocket();
    startRendering();
    setupEventListeners();
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
            paint_commands = JSON.parse(event.data);
            update();
        } catch (error) {
            console.error('Failed to parse message:', error);
        }
    };

    ws.onclose = () => {
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

// 解析 XML 并渲染表单
let fps = 30;
function update() {
    const delta = performance.now() - _lastUpdateTime;
    if (delta > 1000 / fps) {
        let copied = paint_commands;
        if (!copied) return;

        // 清空 Canvas
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        copied.forEach(command => eval(command.startsWith('#') ? command.substring(1) : command));

        // skip commands which should only applied once
        copied = copied.filter(x => !x.startsWith('#'));
        paint_commands = copied;
    }
}

import { drawDisconnectOverlay } from "./connection.js"
// 启动渲染循环
function startRendering() {
    function renderLoop() {
        if (isDisconnected) {
            paint_commands = null;
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
            update();
        }
        animationFrameId = requestAnimationFrame(renderLoop);
    }
    renderLoop();
}

// 初始化应用
window.onload = initApp;