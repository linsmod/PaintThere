#include <canvas/canvas.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* C 宿主：canvas-cpp(ant) 运行 VM 渲染页面。
 * 用法：vm_renderer <page.js> <assets_dir> [width] [height] [max_frames]
 * 路径由启动脚本以绝对路径传入，避免工作目录差异。 */

static void on_frame(canvas_instance *app, int frame_index, void *user) {
    (void)user;
    if (frame_index % 60 != 0) return;
    char state[1024];
    if (canvas_eval(app,
                    "(function(){var r=globalThis.__vmRenderer||{pending:true};"
                    "return JSON.stringify({ready:r.ready,frames:r.frames,resources:r.resources,"
                    "error:r.error,pixels:(r.pixels?r.pixels():null)})})()",
                    state, sizeof(state))) {
        printf("[STATE] frame=%d %s\n", frame_index, state);
        fflush(stdout);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: vm_renderer <page.js> <assets_dir> [width] [height] [max_frames]\n");
        return 2;
    }
    const char *page = argv[1];
    const char *assets_dir = argv[2];
    int width = argc > 3 ? atoi(argv[3]) : 800;
    int height = argc > 4 ? atoi(argv[4]) : 600;
    int max_frames = argc > 5 ? atoi(argv[5]) : 0;
    if (width <= 0) width = 800;
    if (height <= 0) height = 600;

    canvas_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.script_path = page;
    opts.base_dir = assets_dir;
    opts.width = width;
    opts.height = height;
    opts.flags = CANVAS_FLAG_MODULE;
    opts.max_frames = max_frames;

    canvas_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_frame = on_frame;

    printf("vm_renderer: page=%s assets=%s size=%dx%d frames=%d\n",
           page, assets_dir, width, height, max_frames);
    fflush(stdout);
    return canvas_run(&opts, &cb, NULL);
}
