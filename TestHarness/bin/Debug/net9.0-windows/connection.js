let reconnectAttempts = 0;
const MAX_RECONNECT_ATTEMPTS = 5;

export function drawDisconnectOverlay(ctx, canvas) {
    ctx.save();
    try {
        // 绘制半透明黑色背景（覆盖整个Canvas）
        ctx.fillStyle = 'rgba(0, 0, 0, 0.7)';
        ctx.fillRect(0, 0, canvas.width, canvas.height);

        // 计算中心位置
        const centerX = canvas.width / 2 / 1.5;
        const centerY = canvas.height / 2 / 1.5;

        // 绘制提示文字
        ctx.fillStyle = 'white';
        ctx.font = '24px Arial';
        ctx.textAlign = 'center';
        ctx.fillText('连接已断开，正在尝试重新连接...', centerX, centerY);

        // 重试次数显示
        if (reconnectAttempts > 0) {
            ctx.font = '16px Arial';
            ctx.fillText(
                `重试次数: ${reconnectAttempts}/${MAX_RECONNECT_ATTEMPTS}`,
                centerX,
                centerY + 40
            );
        }

        // 绘制重连按钮
        const buttonWidth = 200;
        const buttonHeight = 50;
        const buttonX = centerX - buttonWidth / 2;
        const buttonY = centerY + 80;

        ctx.fillStyle = '#4CAF50';
        ctx.fillRect(buttonX, buttonY, buttonWidth, buttonHeight);

        ctx.fillStyle = 'white';
        ctx.font = '18px Arial';
        ctx.fillText('立即重连', centerX, buttonY + buttonHeight / 2 + 5);
    } finally {
        ctx.restore();
    }
}