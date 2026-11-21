let loadingSplash = {
    gradientX: 0,
    gradientY: 0,
    config: {
        speed: 0.1,
        colorStops: ['gray', '#ffffff'],
        borderColor: '#f0f0f0',
        borderWidth: 2,
        fontSize: '24px',
        fontColor: '#505050',
        hueShift: 0
    },
    frames: 0,
    lastTime: performance.now(),
    fps: 0,
    lastFrameTime: performance.now(), // 记录上一帧的时间
    targetFps: 30, // 目标FPS
    targetFrameTime: 1000 / 30, // 每帧的目标时间（毫秒）

    update: function (canvas, ctx) {
        const now = performance.now();
        const elapsed = now - this.lastFrameTime;

        // 只有当经过足够的时间（≈33.33ms）才更新
        if (elapsed >= this.targetFrameTime) {
            this.lastFrameTime = now - (elapsed % this.targetFrameTime); // 保持时间同步

            // FPS计算
            this.frames++;
            if (now - this.lastTime > 1000) {
                this.fps = Math.round((this.frames * 1000) / (now - this.lastTime));
                this.lastTime = now;
                this.frames = 0;
            }

            // 更新动画参数
            this.gradientX = (this.gradientX + this.config.speed) % canvas.width;
            this.gradientY = (canvas.height - this.gradientX) % canvas.height;

            this.config.hueShift = (this.config.hueShift + this.config.speed) % 360;
            this.config.colorStops[0] = `hsl(${(this.config.hueShift + 60) % 360}, 100%, 80%)`;
            this.config.colorStops[1] = `hsl(${this.config.hueShift % 360}, 100%, 90%)`;

            this.config.borderWidth = Math.abs(Math.sin(this.gradientX / 50) * 4) + 1;

            this.paint(canvas, ctx, this.gradientX, this.gradientY, this.config);
            this.displayFps(ctx);
        } else {
            // 如果不需要更新，直接显示当前帧（避免画面闪烁）
            this.paint(canvas, ctx, this.gradientX, this.gradientY, this.config);
            this.displayFps(ctx);
        }
    },

    paint: function (canvas, ctx, gradX, gradY, config) {
        var gradient = ctx.createLinearGradient(gradX, gradY, canvas.width - gradX, canvas.height - gradY);
        gradient.addColorStop(0, config.colorStops[0]);
        gradient.addColorStop(1, config.colorStops[1]);

        ctx.fillStyle = gradient;
        ctx.fillRect(0, 0, canvas.width, canvas.height);

        ctx.strokeStyle = config.borderColor;
        ctx.lineWidth = config.borderWidth;
        ctx.strokeRect(0, 0, canvas.width, canvas.height);

        ctx.font = config.fontSize + ' Arial';
        ctx.fillStyle = config.fontColor;
        ctx.textAlign = 'center';
        ctx.fillText('Loading...', canvas.width / 2, canvas.height / 2);
    },

    displayFps: function (ctx) {
        ctx.fillStyle = 'black';
        ctx.font = '16px Arial';
        ctx.textAlign = 'left';
        ctx.fillText(`FPS: ${this.fps}`, 10, 20);
    }
};

export { loadingSplash };