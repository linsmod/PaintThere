/*
 * main.c - vm_wms 入口（窗口管理服务）
 *
 * 用法：
 *   vm_wms                                     默认管道名、800x600、挂真实合成宿主
 *   vm_wms --pipe <path>                       指定管道名 / unix socket 路径
 *   vm_wms --screen 800x600                    画布尺寸
 *   vm_wms --mock                              挂模拟宿主（占位帧，仅联调用）
 *
 * 真实宿主 = vm_host.c：渲染副本 + 装饰 + 合成 + 瓦片切分 + 资源包。
 * 未接宿主时内容/帧类请求返回 VMWM_ERR_NOT_READY，元数据能力不受影响。
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

#include "vm_host.h"

static void on_signal(uv_signal_t* handle, int signum)
{
    (void)handle;
    vmwm_log("signal %d received, shutting down", signum);
    VmWm_ServerStop();
}

static void usage(void)
{
    printf("usage: vm_wms [--pipe <path>] [--screen <W>x<H>] [--mock]\n");
    printf("  default pipe : %s\n", VMWM_PIPE_NAME);
    printf("  protocol     : v%u, payload <= %u bytes\n",
           (unsigned)VMWM_VERSION, (unsigned)VMWM_MAX_PAYLOAD);
    printf("  host         : default = real compositing; --mock = placeholder frames\n");
}

int main(int argc, char** argv)
{
    const char* path     = VMWM_PIPE_NAME;
    int32_t     screen_w = 800;
    int32_t     screen_h = 600;
    int         use_mock = 0;
    VmHost*     host     = NULL;
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
        } else if (strcmp(argv[i], "--mock") == 0) {
            use_mock = 1;
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

    printf("vm-wms starting, pipe = %s, screen = %dx%d\n",
           path, (int)screen_w, (int)screen_h);
    fflush(stdout);

    if (VmWm_ServerInit(path) != VMWM_OK) {
        printf("vm-wms init failed\n");
        return 1;
    }

    VmWm_ServerSetScreenSize(screen_w, screen_h);

    if (use_mock) {
        VmWm_ServerSetHost(VmWm_MockHost());
        printf("mock host registered (placeholder frames, integration only)\n");
    } else {
        host = VmHost_Create(screen_w, screen_h, 100, 100);
        if (!host) {
            printf("vm host create failed\n");
            VmWm_ServerStop();
            return 1;
        }
        VmWm_ServerSetHost(VmHost_Callbacks(host));
        printf("vm host registered (real compositing)\n");
    }
    fflush(stdout);

    uv_signal_init(uv_default_loop(), &sig_int);
    uv_signal_start(&sig_int, on_signal, SIGINT);

    uv_signal_init(uv_default_loop(), &sig_term);
#ifdef SIGTERM
    uv_signal_start(&sig_term, on_signal, SIGTERM);
#endif

    VmWm_ServerRun();

    VmHost_Destroy(host);
    vmwm_log("vm-wms exited");
    return 0;
}
