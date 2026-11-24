/*
 * main.c - wm_server 入口
 *
 * 用法：
 *   wm_server                                  默认管道名、默认 800x600、挂模拟宿主
 *   wm_server --pipe <path>                    指定管道名 / unix socket 路径
 *   wm_server --screen 800x600                 画布尺寸（宿主接入前用命令行给）
 *   wm_server --no-mock                        不挂模拟宿主（接真实合成层时用）
 *
 * 退出：Ctrl+C / SIGTERM。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "uv.h"

#include "wms_ipc.h"
#include "wms_server.h"
#include "wms_host.h"
#include "wms_log.h"

static void on_signal(uv_signal_t* handle, int signum)
{
    (void)handle;
    vmwm_log("signal %d received, shutting down", signum);
    VmWm_ServerStop();
}

static void usage(void)
{
    printf("usage: wm_server [--pipe <path>] [--screen <W>x<H>] [--no-mock]\n");
    printf("  default pipe : %s\n", VMWM_PIPE_NAME);
    printf("  protocol     : v%u, payload <= %u bytes\n",
           (unsigned)VMWM_VERSION, (unsigned)VMWM_MAX_PAYLOAD);
}

int main(int argc, char** argv)
{
    const char* path     = VMWM_PIPE_NAME;
    int32_t     screen_w = 800;
    int32_t     screen_h = 600;
    int         use_mock = 1;
    uv_signal_t sig_int;
    uv_signal_t sig_term;
    int         i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pipe") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &screen_w, &screen_h) != 2 ||
                screen_w <= 0 || screen_h <= 0) {
                printf("bad --screen value\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--no-mock") == 0) {
            use_mock = 0;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            printf("unknown argument: %s\n", argv[i]);
            usage();
            return 2;
        }
    }

    printf("wm-server starting, pipe = %s, screen = %dx%d\n",
           path, (int)screen_w, (int)screen_h);
    fflush(stdout);

    if (VmWm_ServerInit(path) != VMWM_OK) {
        printf("wm-server init failed\n");
        return 1;
    }

    VmWm_ServerSetScreenSize(screen_w, screen_h);

    if (use_mock) {
        VmWm_ServerSetHost(VmWm_MockHost());
        printf("mock host registered (占位帧，仅用于联调)\n");
        fflush(stdout);
    }

    uv_signal_init(uv_default_loop(), &sig_int);
    uv_signal_start(&sig_int, on_signal, SIGINT);

    uv_signal_init(uv_default_loop(), &sig_term);
#ifdef SIGTERM
    uv_signal_start(&sig_term, on_signal, SIGTERM);
#endif

    VmWm_ServerRun();

    vmwm_log("wm-server exited");
    return 0;
}
